/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "process/muon_executor_supervisor_protocol.h"
#include "process/muon_linux_executor_supervisor.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstring>
#include <iostream>
#include <string>

static constexpr int kExitNotificationTimeoutMilliseconds = 5000;

static void CloseFileDescriptor(int* fd) {
  if (*fd < 0) {
    return;
  }
  while (close(*fd) < 0 && errno == EINTR) {
  }
  *fd = -1;
}

static void TerminateAndReapSupervisor(pid_t process_id) {
  if (process_id <= 0) {
    return;
  }
  kill(process_id, SIGKILL);
  int status = 0;
  while (waitpid(process_id, &status, 0) < 0 && errno == EINTR) {
  }
}

static bool ReapSuccessfulSupervisor(pid_t process_id) {
  int status = 0;
  pid_t result = -1;
  do {
    result = waitpid(process_id, &status, 0);
  } while (result < 0 && errno == EINTR);
  return result == process_id && WIFEXITED(status) &&
         WEXITSTATUS(status) == 0;
}

static int RunTargetProcess() {
  char buffer = '\0';
  ssize_t read_result = -1;
  do {
    read_result = read(STDIN_FILENO, &buffer, sizeof(buffer));
  } while (read_result < 0 && errno == EINTR);
  return read_result == 0 ? 0 : 1;
}

static int RunSupervisorTest(
    const std::string& supervisor_path,
    const std::string& target_path) {
  int target_input_fds[2] = {-1, -1};
  if (pipe2(target_input_fds, O_CLOEXEC) != 0) {
    std::cerr << "Failed to create target input pipe: "
              << std::strerror(errno) << "\n";
    return 1;
  }
  auto null_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
  if (null_fd < 0) {
    std::cerr << "Failed to open null device: " << std::strerror(errno)
              << "\n";
    CloseFileDescriptor(&target_input_fds[0]);
    CloseFileDescriptor(&target_input_fds[1]);
    return 2;
  }

  struct sigaction ignored_action = {};
  struct sigaction previous_action = {};
  ignored_action.sa_handler = SIG_IGN;
  sigemptyset(&ignored_action.sa_mask);
  if (sigaction(SIGCHLD, &ignored_action, &previous_action) != 0) {
    std::cerr << "Failed to ignore SIGCHLD: " << std::strerror(errno)
              << "\n";
    CloseFileDescriptor(&null_fd);
    CloseFileDescriptor(&target_input_fds[0]);
    CloseFileDescriptor(&target_input_fds[1]);
    return 3;
  }

  muon_internal::MuonLinuxExecutorSupervisorLaunchOptions options;
  options.supervisor_path = supervisor_path;
  options.config.command = target_path;
  options.config.arguments = {"--target"};
  options.config.daemon = false;
  // Keep the target alive until READY has been observed by the test process.
  options.target_stdin_fd = target_input_fds[0];
  options.target_stdout_fd = null_fd;
  options.target_stderr_fd = null_fd;

  muon_internal::MuonLinuxExecutorSupervisorConnection connection;
  std::string error_message;
  const auto launched = muon_internal::LaunchMuonLinuxExecutorSupervisor(
      options, &connection, &error_message);
  const auto restore_result =
      sigaction(SIGCHLD, &previous_action, nullptr);
  const auto restore_error = errno;
  CloseFileDescriptor(&target_input_fds[0]);
  CloseFileDescriptor(&null_fd);

  if (!launched) {
    std::cerr << "Supervisor did not report READY: " << error_message
              << "\n";
    CloseFileDescriptor(&target_input_fds[1]);
    return 4;
  }
  if (restore_result != 0) {
    std::cerr << "Failed to restore SIGCHLD disposition: "
              << std::strerror(restore_error) << "\n";
    CloseFileDescriptor(&target_input_fds[1]);
    CloseFileDescriptor(&connection.control_fd);
    TerminateAndReapSupervisor(connection.supervisor_process_id);
    return 5;
  }

  CloseFileDescriptor(&target_input_fds[1]);

  pollfd control_poll = {
      connection.control_fd,
      POLLIN,
      0,
  };
  int poll_result = -1;
  do {
    poll_result = poll(
        &control_poll, 1, kExitNotificationTimeoutMilliseconds);
  } while (poll_result < 0 && errno == EINTR);
  if (poll_result <= 0) {
    if (poll_result == 0) {
      std::cerr << "Supervisor did not report EXIT after target exit\n";
    } else {
      std::cerr << "Failed to wait for supervisor EXIT: "
                << std::strerror(errno) << "\n";
    }
    CloseFileDescriptor(&connection.control_fd);
    TerminateAndReapSupervisor(connection.supervisor_process_id);
    return 6;
  }

  muon_internal::MuonExecutorSupervisorMessage message;
  const auto receive_result =
      muon_internal::ReceiveMuonExecutorSupervisorMessage(
          connection.control_fd, &message, &error_message);
  CloseFileDescriptor(&connection.control_fd);
  if (receive_result !=
          muon_internal::MuonExecutorSupervisorReceiveResult::kMessage ||
      message.type !=
          muon_internal::MuonExecutorSupervisorMessageType::kExit ||
      message.value != 0) {
    std::cerr << "Supervisor returned an invalid EXIT notification";
    if (!error_message.empty()) {
      std::cerr << ": " << error_message;
    }
    std::cerr << "\n";
    TerminateAndReapSupervisor(connection.supervisor_process_id);
    return 7;
  }
  if (!ReapSuccessfulSupervisor(connection.supervisor_process_id)) {
    std::cerr << "Supervisor did not exit successfully\n";
    return 8;
  }
  return 0;
}

int main(int argc, char** argv) {
  if (argc == 2 && std::strcmp(argv[1], "--target") == 0) {
    return RunTargetProcess();
  }
  if (argc != 3) {
    std::cerr << "Expected supervisor and target executable paths\n";
    return 9;
  }
  return RunSupervisorTest(argv[1], argv[2]);
}
