/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "process/muon_executor_supervisor_protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

extern char** environ;

namespace muon_internal {

static constexpr int kTerminationGracePeriodSeconds = 2;

struct SupervisorState {
  int control_fd = kMuonExecutorSupervisorControlFd;
  int signal_fd = -1;
  int timer_fd = -1;
  pid_t target_process_id = -1;
  pid_t target_process_group_id = -1;
  bool daemon = false;
  bool control_open = true;
  bool root_exited = false;
  bool exit_sent = false;
  bool termination_requested = false;
  bool force_kill_sent = false;
  bool terminal_failure = false;
  int32_t root_exit_code = -1;
};

static void CloseFileDescriptor(int* fd) {
  if (*fd < 0) {
    return;
  }
  while (close(*fd) < 0 && errno == EINTR) {
  }
  *fd = -1;
}

static void CloseInheritedTargetDescriptors() {
  auto stdin_fd = kMuonExecutorSupervisorTargetStdinFd;
  auto stdout_fd = kMuonExecutorSupervisorTargetStdoutFd;
  auto stderr_fd = kMuonExecutorSupervisorTargetStderrFd;
  CloseFileDescriptor(&stdin_fd);
  CloseFileDescriptor(&stdout_fd);
  CloseFileDescriptor(&stderr_fd);
}

static bool SetCloseOnExec(int fd, std::string* error_message) {
  const auto flags = fcntl(fd, F_GETFD);
  if (flags >= 0 && fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0) {
    return true;
  }
  *error_message =
      "Failed to protect executor supervisor descriptor: " +
      std::string(std::strerror(errno));
  return false;
}

static void ReportSupervisorError(
    int control_fd,
    int error_number,
    const std::string& message) {
  std::string ignored_error;
  SendMuonExecutorSupervisorError(
      control_fd, error_number, message, &ignored_error);
}

static bool DetachDaemonSupervisor(
    int control_fd,
    std::string* error_message) {
  const auto session_child = fork();
  if (session_child < 0) {
    *error_message =
        "Failed to fork executor daemon supervisor: " +
        std::string(std::strerror(errno));
    return false;
  }
  if (session_child > 0) {
    _exit(0);
  }
  if (setsid() < 0) {
    *error_message =
        "Failed to create executor daemon session: " +
        std::string(std::strerror(errno));
    ReportSupervisorError(control_fd, errno, *error_message);
    _exit(1);
  }

  const auto orphan_supervisor = fork();
  if (orphan_supervisor < 0) {
    *error_message =
        "Failed to orphan executor daemon supervisor: " +
        std::string(std::strerror(errno));
    ReportSupervisorError(control_fd, errno, *error_message);
    _exit(1);
  }
  if (orphan_supervisor > 0) {
    _exit(0);
  }
  return true;
}

static bool PrepareSupervisorProcess(
    bool daemon,
    int control_fd,
    std::string* error_message) {
  if (daemon) {
    if (!DetachDaemonSupervisor(control_fd, error_message)) {
      return false;
    }
  } else if (setsid() < 0) {
    *error_message =
        "Failed to create executor supervisor session: " +
        std::string(std::strerror(errno));
    return false;
  }
  if (prctl(PR_SET_CHILD_SUBREAPER, 1) != 0) {
    *error_message =
        "Failed to make executor supervisor a child subreaper: " +
        std::string(std::strerror(errno));
    return false;
  }
  return SetCloseOnExec(control_fd, error_message);
}

static bool ResetSupervisorSignalDispositions(
    std::string* error_message) {
  struct sigaction default_action = {};
  default_action.sa_handler = SIG_DFL;
  sigemptyset(&default_action.sa_mask);
  constexpr std::array<int, 4> monitored_signals = {
      SIGCHLD,
      SIGTERM,
      SIGINT,
      SIGHUP,
  };
  for (const auto signal_number : monitored_signals) {
    if (sigaction(signal_number, &default_action, nullptr) != 0) {
      *error_message =
          "Failed to reset executor supervisor signal disposition: " +
          std::string(std::strerror(errno));
      return false;
    }
  }
  return true;
}

static bool ConfigureSignalMonitoring(
    sigset_t* target_signal_mask,
    int* signal_fd,
    std::string* error_message) {
  sigset_t supervisor_signals;
  sigemptyset(&supervisor_signals);
  sigaddset(&supervisor_signals, SIGCHLD);
  sigaddset(&supervisor_signals, SIGTERM);
  sigaddset(&supervisor_signals, SIGINT);
  sigaddset(&supervisor_signals, SIGHUP);
  if (sigprocmask(SIG_BLOCK, &supervisor_signals, target_signal_mask) != 0) {
    *error_message =
        "Failed to block executor supervisor signals: " +
        std::string(std::strerror(errno));
    return false;
  }
  *signal_fd =
      signalfd(-1, &supervisor_signals, SFD_CLOEXEC | SFD_NONBLOCK);
  if (*signal_fd < 0) {
    *error_message =
        "Failed to create executor supervisor signal source: " +
        std::string(std::strerror(errno));
    return false;
  }
  return true;
}

static void RestoreTargetSignals(const sigset_t& target_signal_mask) {
  sigprocmask(SIG_SETMASK, &target_signal_mask, nullptr);
  signal(SIGCHLD, SIG_DFL);
  signal(SIGTERM, SIG_DFL);
  signal(SIGINT, SIG_DFL);
  signal(SIGHUP, SIG_DFL);
  signal(SIGPIPE, SIG_DFL);
}

static void CloseDescriptorsFrom(int first_fd, long maximum_fd) {
#if defined(SYS_close_range)
  if (syscall(
          SYS_close_range,
          static_cast<unsigned int>(first_fd),
          std::numeric_limits<unsigned int>::max(),
          0) == 0) {
    return;
  }
#endif
  for (auto fd = static_cast<long>(first_fd); fd < maximum_fd; ++fd) {
    close(fd);
  }
}

static long GetMaximumFileDescriptor() {
  const auto maximum_fd = sysconf(_SC_OPEN_MAX);
  return maximum_fd < 0 ? 1024 : maximum_fd;
}

static bool NormalizeSupervisorStandardDescriptors(
    std::string* error_message) {
  auto null_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
  if (null_fd < 0) {
    const auto error_number = errno;
    *error_message =
        "Failed to open executor supervisor null device: " +
        std::string(std::strerror(error_number));
    errno = error_number;
    return false;
  }
  for (auto fd = STDIN_FILENO; fd <= STDERR_FILENO; ++fd) {
    if (dup2(null_fd, fd) >= 0) {
      continue;
    }
    const auto error_number = errno;
    CloseFileDescriptor(&null_fd);
    *error_message =
        "Failed to redirect executor supervisor standard descriptor: " +
        std::string(std::strerror(error_number));
    errno = error_number;
    return false;
  }
  if (null_fd > STDERR_FILENO) {
    CloseFileDescriptor(&null_fd);
  }
  return true;
}

static bool ValidateTargetEnvironment(
    const MuonExecutorSupervisorConfig& config,
    std::string* error_message) {
  if (!config.has_environment) {
    return true;
  }
  for (const auto& entry : config.environment) {
    const auto separator = entry.find('=');
    if (separator == std::string::npos || separator == 0) {
      *error_message =
          "Executor target environment contains an invalid entry";
      return false;
    }
  }
  return true;
}

static pid_t SpawnTargetProcess(
    const MuonExecutorSupervisorConfig& config,
    const sigset_t& target_signal_mask,
    std::string* error_message) {
  std::vector<std::string> argument_storage;
  argument_storage.reserve(config.arguments.size() + 1);
  argument_storage.push_back(config.command);
  argument_storage.insert(
      argument_storage.end(),
      config.arguments.begin(),
      config.arguments.end());
  std::vector<char*> argument_pointers;
  argument_pointers.reserve(argument_storage.size() + 1);
  for (auto& argument : argument_storage) {
    argument_pointers.push_back(argument.data());
  }
  argument_pointers.push_back(nullptr);

  std::vector<std::string> environment_storage = config.environment;
  std::vector<char*> environment_pointers;
  if (config.has_environment) {
    environment_pointers.reserve(environment_storage.size() + 1);
    for (auto& entry : environment_storage) {
      environment_pointers.push_back(entry.data());
    }
    environment_pointers.push_back(nullptr);
  }

  const auto maximum_fd = GetMaximumFileDescriptor();
  const auto target_process_id = fork();
  if (target_process_id < 0) {
    *error_message =
        "Failed to fork executor target: " +
        std::string(std::strerror(errno));
    return -1;
  }
  if (target_process_id == 0) {
    if (setpgid(0, 0) != 0 ||
        dup2(kMuonExecutorSupervisorTargetStdinFd, STDIN_FILENO) < 0 ||
        dup2(kMuonExecutorSupervisorTargetStdoutFd, STDOUT_FILENO) < 0 ||
        dup2(kMuonExecutorSupervisorTargetStderrFd, STDERR_FILENO) < 0) {
      _exit(126);
    }
    CloseDescriptorsFrom(STDERR_FILENO + 1, maximum_fd);
    RestoreTargetSignals(target_signal_mask);
    if (config.has_cwd && chdir(config.cwd.c_str()) != 0) {
      _exit(126);
    }
    if (config.has_environment) {
      environ = environment_pointers.data();
    }
    execvp(config.command.c_str(), argument_pointers.data());
    _exit(errno == ENOENT ? 127 : 126);
  }

  if (setpgid(target_process_id, target_process_id) != 0 &&
      errno != EACCES && errno != ESRCH) {
    const auto error_number = errno;
    kill(target_process_id, SIGKILL);
    int status = 0;
    while (waitpid(target_process_id, &status, 0) < 0 && errno == EINTR) {
    }
    *error_message =
        "Failed to create executor target process group: " +
        std::string(std::strerror(error_number));
    return -1;
  }
  return target_process_id;
}

static bool ProcessGroupExists(pid_t process_group_id) {
  if (process_group_id <= 0) {
    return false;
  }
  if (kill(-process_group_id, 0) == 0) {
    return true;
  }
  return errno == EPERM;
}

static bool ArmTerminationTimer(
    int timer_fd,
    std::string* error_message) {
  itimerspec timer = {};
  timer.it_value.tv_sec = kTerminationGracePeriodSeconds;
  if (timerfd_settime(timer_fd, 0, &timer, nullptr) == 0) {
    return true;
  }
  const auto error_number = errno;
  *error_message =
      "Failed to arm executor termination timer: " +
      std::string(std::strerror(error_number));
  errno = error_number;
  return false;
}

static void DisarmTerminationTimer(int timer_fd) {
  itimerspec timer = {};
  timerfd_settime(timer_fd, 0, &timer, nullptr);
}

static void ReportTerminalFailure(
    SupervisorState* state,
    int error_number,
    const std::string& error_message) {
  state->terminal_failure = true;
  ReportSupervisorError(
      state->control_fd, error_number, error_message);
}

static void ForceKillProcessGroupBestEffort(SupervisorState* state) {
  state->force_kill_sent = true;
  if (state->target_process_group_id > 0) {
    (void)kill(-state->target_process_group_id, SIGKILL);
  }
}

static void BeginTreeTermination(
    SupervisorState* state,
    std::string* error_message) {
  if (state->termination_requested) {
    return;
  }
  state->termination_requested = true;
  if (ProcessGroupExists(state->target_process_group_id)) {
    if (kill(-state->target_process_group_id, SIGTERM) != 0) {
      const auto error_number = errno;
      if (error_number == ESRCH) {
        return;
      }
      *error_message =
          "Failed to terminate executor process group: " +
          std::string(std::strerror(error_number));
      ReportTerminalFailure(
          state, error_number, *error_message);
      ForceKillProcessGroupBestEffort(state);
      return;
    }
    if (!ArmTerminationTimer(state->timer_fd, error_message)) {
      const auto error_number = errno;
      ReportTerminalFailure(
          state, error_number, *error_message);
      ForceKillProcessGroupBestEffort(state);
    }
  }
}

static int32_t DecodeExitCode(int status) {
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  if (WIFSIGNALED(status)) {
    return 128 + WTERMSIG(status);
  }
  return -1;
}

static void SendRootExit(SupervisorState* state) {
  if (!state->root_exited || state->exit_sent || !state->control_open) {
    return;
  }
  std::string error_message;
  if (!SendMuonExecutorSupervisorExit(
          state->control_fd, state->root_exit_code, &error_message)) {
    state->control_open = false;
    CloseFileDescriptor(&state->control_fd);
    return;
  }
  state->exit_sent = true;
}

static void ReapExitedChildren(
    SupervisorState* state,
    std::string* error_message) {
  while (true) {
    int status = 0;
    const auto process_id = waitpid(-1, &status, WNOHANG);
    if (process_id > 0) {
      if (process_id == state->target_process_id && !state->root_exited) {
        state->root_exited = true;
        state->root_exit_code = DecodeExitCode(status);
        SendRootExit(state);
        if (!state->daemon) {
          BeginTreeTermination(state, error_message);
        }
      }
      continue;
    }
    if (process_id == 0 || errno == ECHILD) {
      return;
    }
    if (errno == EINTR) {
      continue;
    }
    *error_message =
        "Failed to reap executor target process: " +
        std::string(std::strerror(errno));
    ReportSupervisorError(state->control_fd, errno, *error_message);
    return;
  }
}

static bool ProcessControlMessage(
    SupervisorState* state,
    std::string* error_message) {
  MuonExecutorSupervisorMessage message;
  const auto result = ReceiveMuonExecutorSupervisorMessage(
      state->control_fd, &message, error_message);
  if (result == MuonExecutorSupervisorReceiveResult::kClosed) {
    state->control_open = false;
    CloseFileDescriptor(&state->control_fd);
    if (!state->daemon || state->termination_requested) {
      BeginTreeTermination(state, error_message);
    }
    return true;
  }
  if (result == MuonExecutorSupervisorReceiveResult::kError) {
    ReportSupervisorError(state->control_fd, EPROTO, *error_message);
    state->control_open = false;
    CloseFileDescriptor(&state->control_fd);
    BeginTreeTermination(state, error_message);
    return false;
  }
  if (message.type != MuonExecutorSupervisorMessageType::kKill) {
    *error_message = "Executor supervisor received an unexpected frame";
    ReportSupervisorError(state->control_fd, EPROTO, *error_message);
    BeginTreeTermination(state, error_message);
    return false;
  }

  std::string send_error;
  if (!SendMuonExecutorSupervisorAck(
          state->control_fd,
          MuonExecutorSupervisorMessageType::kKill,
          &send_error)) {
    state->control_open = false;
    CloseFileDescriptor(&state->control_fd);
  }
  BeginTreeTermination(state, error_message);
  return true;
}

static void ProcessSupervisorSignals(
    SupervisorState* state,
    std::string* error_message) {
  while (true) {
    signalfd_siginfo signal_info = {};
    const auto size =
        read(state->signal_fd, &signal_info, sizeof(signal_info));
    if (size == static_cast<ssize_t>(sizeof(signal_info))) {
      if (signal_info.ssi_signo == SIGCHLD) {
        ReapExitedChildren(state, error_message);
      } else {
        BeginTreeTermination(state, error_message);
      }
      if (state->terminal_failure) {
        return;
      }
      continue;
    }
    if (size < 0 && errno == EINTR) {
      continue;
    }
    if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return;
    }
    if (size == 0) {
      return;
    }
    *error_message =
        "Failed to read executor supervisor signal source: " +
        std::string(std::strerror(errno));
    ReportSupervisorError(state->control_fd, errno, *error_message);
    BeginTreeTermination(state, error_message);
    return;
  }
}

static void ProcessTerminationTimer(
    SupervisorState* state,
    std::string* error_message) {
  uint64_t expiration_count = 0;
  while (read(
             state->timer_fd,
             &expiration_count,
             sizeof(expiration_count)) < 0) {
    if (errno == EINTR) {
      continue;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      const auto error_number = errno;
      *error_message =
          "Failed to read executor termination timer: " +
          std::string(std::strerror(error_number));
      ReportTerminalFailure(
          state, error_number, *error_message);
    }
    return;
  }
  if (!ProcessGroupExists(state->target_process_group_id)) {
    return;
  }
  if (state->force_kill_sent) {
    *error_message =
        "Executor process group remained alive after forced termination";
    ReportTerminalFailure(
        state, ETIMEDOUT, *error_message);
    ForceKillProcessGroupBestEffort(state);
    return;
  }

  state->force_kill_sent = true;
  if (kill(-state->target_process_group_id, SIGKILL) != 0) {
    const auto error_number = errno;
    if (error_number == ESRCH) {
      return;
    }
    *error_message =
        "Failed to kill executor process group: " +
        std::string(std::strerror(error_number));
    ReportTerminalFailure(
        state, error_number, *error_message);
    return;
  }
  // A process-group signal succeeds when it reaches at least one member.
  // Confirm that no inaccessible or slow-to-exit members remain.
  if (!ArmTerminationTimer(state->timer_fd, error_message)) {
    const auto error_number = errno;
    ReportTerminalFailure(
        state, error_number, *error_message);
    ForceKillProcessGroupBestEffort(state);
  }
}

static bool SupervisorCanExit(const SupervisorState& state) {
  if (!state.control_open && state.daemon &&
      !state.termination_requested) {
    return true;
  }
  if (!state.termination_requested) {
    return false;
  }
  return state.root_exited &&
         !ProcessGroupExists(state.target_process_group_id);
}

static void EmergencyKillTargetTree(SupervisorState* state) {
  if (state->target_process_group_id > 0) {
    kill(-state->target_process_group_id, SIGKILL);
  } else if (state->target_process_id > 0) {
    kill(state->target_process_id, SIGKILL);
  }
}

static int MonitorTargetProcess(SupervisorState* state) {
  std::string error_message;
  while (!state->terminal_failure && !SupervisorCanExit(*state)) {
    std::array<pollfd, 3> poll_descriptors = {
        pollfd{
            state->control_open ? state->control_fd : -1,
            POLLIN,
            0,
        },
        pollfd{state->signal_fd, POLLIN, 0},
        pollfd{state->timer_fd, POLLIN, 0},
    };
    const auto poll_result = poll(
        poll_descriptors.data(), poll_descriptors.size(), -1);
    if (poll_result < 0) {
      if (errno == EINTR) {
        continue;
      }
      error_message =
          "Failed to poll executor supervisor sources: " +
          std::string(std::strerror(errno));
      ReportSupervisorError(state->control_fd, errno, error_message);
      EmergencyKillTargetTree(state);
      return 1;
    }
    if (poll_descriptors[0].revents != 0) {
      ProcessControlMessage(state, &error_message);
    }
    if (!state->terminal_failure &&
        (poll_descriptors[1].revents & POLLIN) != 0) {
      ProcessSupervisorSignals(state, &error_message);
    }
    if (!state->terminal_failure &&
        (poll_descriptors[2].revents & POLLIN) != 0) {
      ProcessTerminationTimer(state, &error_message);
    }
    if ((poll_descriptors[1].revents &
         (POLLERR | POLLHUP | POLLNVAL)) != 0) {
      EmergencyKillTargetTree(state);
      return 1;
    }
  }
  DisarmTerminationTimer(state->timer_fd);
  ReapExitedChildren(state, &error_message);
  if (state->terminal_failure) {
    EmergencyKillTargetTree(state);
    return 1;
  }
  return 0;
}

static int RunExecutorSupervisor() {
  std::string error_message;
  CloseDescriptorsFrom(
      kMuonExecutorSupervisorTargetStderrFd + 1,
      GetMaximumFileDescriptor());
  if (!NormalizeSupervisorStandardDescriptors(&error_message)) {
    ReportSupervisorError(
        kMuonExecutorSupervisorControlFd, errno, error_message);
    CloseInheritedTargetDescriptors();
    return 1;
  }

  MuonExecutorSupervisorMessage config_message;
  const auto receive_result = ReceiveMuonExecutorSupervisorMessage(
      kMuonExecutorSupervisorControlFd, &config_message, &error_message);
  if (receive_result != MuonExecutorSupervisorReceiveResult::kMessage ||
      config_message.type !=
          MuonExecutorSupervisorMessageType::kConfig) {
    if (receive_result == MuonExecutorSupervisorReceiveResult::kMessage) {
      error_message = "Executor supervisor expected a CONFIG frame";
    } else if (receive_result ==
               MuonExecutorSupervisorReceiveResult::kClosed) {
      error_message =
          "Executor supervisor control channel closed before CONFIG";
    }
    ReportSupervisorError(
        kMuonExecutorSupervisorControlFd, EPROTO, error_message);
    CloseInheritedTargetDescriptors();
    return 1;
  }
  const auto& config = config_message.config;
  if (!ValidateTargetEnvironment(config, &error_message)) {
    ReportSupervisorError(
        kMuonExecutorSupervisorControlFd, EINVAL, error_message);
    CloseInheritedTargetDescriptors();
    return 1;
  }
  if (!ResetSupervisorSignalDispositions(&error_message)) {
    ReportSupervisorError(
        kMuonExecutorSupervisorControlFd, errno, error_message);
    CloseInheritedTargetDescriptors();
    return 1;
  }
  if (!PrepareSupervisorProcess(
          config.daemon,
          kMuonExecutorSupervisorControlFd,
          &error_message)) {
    ReportSupervisorError(
        kMuonExecutorSupervisorControlFd, errno, error_message);
    CloseInheritedTargetDescriptors();
    return 1;
  }

  SupervisorState state;
  state.daemon = config.daemon;
  sigset_t target_signal_mask;
  if (!ConfigureSignalMonitoring(
          &target_signal_mask, &state.signal_fd, &error_message)) {
    ReportSupervisorError(state.control_fd, errno, error_message);
    CloseInheritedTargetDescriptors();
    return 1;
  }
  state.timer_fd =
      timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
  if (state.timer_fd < 0) {
    error_message =
        "Failed to create executor termination timer: " +
        std::string(std::strerror(errno));
    ReportSupervisorError(state.control_fd, errno, error_message);
    CloseInheritedTargetDescriptors();
    CloseFileDescriptor(&state.signal_fd);
    return 1;
  }

  state.target_process_id =
      SpawnTargetProcess(config, target_signal_mask, &error_message);
  CloseInheritedTargetDescriptors();
  if (state.target_process_id <= 0) {
    ReportSupervisorError(state.control_fd, errno, error_message);
    CloseFileDescriptor(&state.signal_fd);
    CloseFileDescriptor(&state.timer_fd);
    return 1;
  }
  state.target_process_group_id = state.target_process_id;

  if (!SendMuonExecutorSupervisorReady(
          state.control_fd,
          static_cast<int32_t>(state.target_process_id),
          static_cast<int32_t>(state.target_process_group_id),
          &error_message)) {
    EmergencyKillTargetTree(&state);
    CloseFileDescriptor(&state.control_fd);
    CloseFileDescriptor(&state.signal_fd);
    CloseFileDescriptor(&state.timer_fd);
    return 1;
  }

  const auto result = MonitorTargetProcess(&state);
  CloseFileDescriptor(&state.control_fd);
  CloseFileDescriptor(&state.signal_fd);
  CloseFileDescriptor(&state.timer_fd);
  return result;
}

}  // namespace muon_internal

int main() {
  return muon_internal::RunExecutorSupervisor();
}
