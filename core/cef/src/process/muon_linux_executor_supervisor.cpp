/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "process/muon_linux_executor_supervisor.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cstring>
#include <set>
#include <string>
#include <vector>

extern char** environ;

namespace muon_internal {

static constexpr int kFirstTemporarySpawnFd = 64;

static void CloseFileDescriptor(int* fd) {
  if (*fd < 0) {
    return;
  }
  while (close(*fd) < 0 && errno == EINTR) {
  }
  *fd = -1;
}

static int DuplicateForSpawn(int fd, std::string* error_message) {
  const auto duplicate = fcntl(fd, F_DUPFD_CLOEXEC, kFirstTemporarySpawnFd);
  if (duplicate < 0) {
    *error_message =
        "Failed to duplicate executor supervisor descriptor: " +
        std::string(std::strerror(errno));
  }
  return duplicate;
}

static void ReapProcess(pid_t process_id) {
  if (process_id <= 0) {
    return;
  }
  int status = 0;
  while (waitpid(process_id, &status, 0) < 0 && errno == EINTR) {
  }
}

static void AbortSupervisorLaunch(pid_t process_id, int* control_fd) {
  if (*control_fd >= 0) {
    std::string ignored_error;
    SendMuonExecutorSupervisorKill(*control_fd, &ignored_error);
    CloseFileDescriptor(control_fd);
  }
  if (process_id > 0) {
    kill(process_id, SIGKILL);
    ReapProcess(process_id);
  }
}

static bool AddSpawnFileActions(
    posix_spawn_file_actions_t* actions,
    const std::array<int, 4>& temporary_fds,
    const std::array<int, 5>& inherited_fds,
    std::string* error_message) {
  std::set<int> descriptors_to_close;
  for (const auto fd : inherited_fds) {
    if (fd >= 0) {
      descriptors_to_close.insert(fd);
    }
  }
  for (const auto fd : descriptors_to_close) {
    const auto result = posix_spawn_file_actions_addclose(actions, fd);
    if (result != 0) {
      *error_message =
          "Failed to configure executor supervisor descriptor closing: " +
          std::string(std::strerror(result));
      return false;
    }
  }

  constexpr std::array<int, 4> target_fds = {
      kMuonExecutorSupervisorControlFd,
      kMuonExecutorSupervisorTargetStdinFd,
      kMuonExecutorSupervisorTargetStdoutFd,
      kMuonExecutorSupervisorTargetStderrFd,
  };
  for (auto index = size_t{0}; index < temporary_fds.size(); ++index) {
    auto result = posix_spawn_file_actions_adddup2(
        actions, temporary_fds[index], target_fds[index]);
    if (result == 0) {
      result =
          posix_spawn_file_actions_addclose(actions, temporary_fds[index]);
    }
    if (result != 0) {
      *error_message =
          "Failed to configure executor supervisor descriptor mapping: " +
          std::string(std::strerror(result));
      return false;
    }
  }
  return true;
}

bool LaunchMuonLinuxExecutorSupervisor(
    const MuonLinuxExecutorSupervisorLaunchOptions& options,
    MuonLinuxExecutorSupervisorConnection* connection,
    std::string* error_message) {
  *connection = {};
  if (options.supervisor_path.empty() || options.config.command.empty() ||
      options.target_stdin_fd < 0 || options.target_stdout_fd < 0 ||
      options.target_stderr_fd < 0) {
    *error_message = "Invalid executor supervisor launch options";
    return false;
  }

  std::array<int, 2> control_fds = {-1, -1};
  if (socketpair(
          AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, control_fds.data()) != 0) {
    *error_message =
        "Failed to create executor supervisor control channel: " +
        std::string(std::strerror(errno));
    return false;
  }

  std::array<int, 4> temporary_fds = {
      DuplicateForSpawn(control_fds[1], error_message),
      -1,
      -1,
      -1,
  };
  if (temporary_fds[0] >= 0) {
    temporary_fds[1] =
        DuplicateForSpawn(options.target_stdin_fd, error_message);
  }
  if (temporary_fds[1] >= 0) {
    temporary_fds[2] =
        DuplicateForSpawn(options.target_stdout_fd, error_message);
  }
  if (temporary_fds[2] >= 0) {
    temporary_fds[3] =
        DuplicateForSpawn(options.target_stderr_fd, error_message);
  }
  auto descriptors_ready = true;
  for (const auto fd : temporary_fds) {
    if (fd < 0) {
      descriptors_ready = false;
      break;
    }
  }
  if (!descriptors_ready) {
    for (auto& fd : temporary_fds) {
      CloseFileDescriptor(&fd);
    }
    CloseFileDescriptor(&control_fds[0]);
    CloseFileDescriptor(&control_fds[1]);
    return false;
  }

  posix_spawn_file_actions_t actions;
  auto actions_initialized = false;
  auto result = posix_spawn_file_actions_init(&actions);
  if (result == 0) {
    actions_initialized = true;
    const std::array<int, 5> inherited_fds = {
        control_fds[0],
        control_fds[1],
        options.target_stdin_fd,
        options.target_stdout_fd,
        options.target_stderr_fd,
    };
    if (!AddSpawnFileActions(
            &actions, temporary_fds, inherited_fds, error_message)) {
      result = EINVAL;
    }
  } else {
    *error_message =
        "Failed to initialize executor supervisor launch: " +
        std::string(std::strerror(result));
  }

  pid_t supervisor_process_id = -1;
  if (result == 0) {
    std::array<char*, 2> arguments = {
        const_cast<char*>(options.supervisor_path.c_str()),
        nullptr,
    };
    result = posix_spawn(
        &supervisor_process_id,
        options.supervisor_path.c_str(),
        &actions,
        nullptr,
        arguments.data(),
        environ);
    if (result != 0) {
      *error_message =
          "Failed to start executor supervisor: " +
          std::string(std::strerror(result));
    }
  }

  if (actions_initialized) {
    posix_spawn_file_actions_destroy(&actions);
  }
  for (auto& fd : temporary_fds) {
    CloseFileDescriptor(&fd);
  }
  CloseFileDescriptor(&control_fds[1]);
  if (result != 0) {
    CloseFileDescriptor(&control_fds[0]);
    return false;
  }

  if (!SendMuonExecutorSupervisorConfig(
          control_fds[0], options.config, error_message)) {
    AbortSupervisorLaunch(supervisor_process_id, &control_fds[0]);
    return false;
  }

  MuonExecutorSupervisorMessage message;
  const auto receive_result = ReceiveMuonExecutorSupervisorMessage(
      control_fds[0], &message, error_message);
  if (receive_result != MuonExecutorSupervisorReceiveResult::kMessage) {
    if (receive_result == MuonExecutorSupervisorReceiveResult::kClosed) {
      *error_message =
          "Executor supervisor closed before reporting READY";
    }
    AbortSupervisorLaunch(supervisor_process_id, &control_fds[0]);
    return false;
  }
  if (message.type == MuonExecutorSupervisorMessageType::kError) {
    *error_message = message.text.empty()
                         ? "Executor supervisor failed to start"
                         : message.text;
    AbortSupervisorLaunch(supervisor_process_id, &control_fds[0]);
    return false;
  }
  if (message.type != MuonExecutorSupervisorMessageType::kReady ||
      message.value <= 0 || message.secondary_value <= 0) {
    *error_message = "Executor supervisor returned an invalid READY frame";
    AbortSupervisorLaunch(supervisor_process_id, &control_fds[0]);
    return false;
  }

  if (options.config.daemon) {
    ReapProcess(supervisor_process_id);
    supervisor_process_id = -1;
  }
  connection->supervisor_process_id = supervisor_process_id;
  connection->target_process_id = static_cast<pid_t>(message.value);
  connection->target_process_group_id =
      static_cast<pid_t>(message.secondary_value);
  connection->control_fd = control_fds[0];
  return true;
}

}  // namespace muon_internal
