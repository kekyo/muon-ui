/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once

#include "process/muon_executor_supervisor_protocol.h"

#include <sys/types.h>

#include <string>

namespace muon_internal {

/**
 * Describes one Linux executor supervisor launch.
 */
struct MuonLinuxExecutorSupervisorLaunchOptions {
  /** Absolute path to the bundled muon-executor-supervisor executable. */
  std::string supervisor_path;
  /** Target process configuration sent over the control channel. */
  MuonExecutorSupervisorConfig config;
  /** Child end of the target stdin pipe. Ownership remains with the caller. */
  int target_stdin_fd = -1;
  /** Child end of the target stdout pipe. Ownership remains with the caller. */
  int target_stdout_fd = -1;
  /** Child end of the target stderr pipe. Ownership remains with the caller. */
  int target_stderr_fd = -1;
};

/**
 * Holds the connected Linux executor supervisor endpoints.
 */
struct MuonLinuxExecutorSupervisorConnection {
  /**
   * Supervisor PID to reap for non-daemons.
   *
   * @remarks Daemon launches reap their bootstrap process before returning and
   * set this field to -1.
   */
  pid_t supervisor_process_id = -1;
  /** Root target process ID reported by READY. */
  pid_t target_process_id = -1;
  /** Target process group ID reported by READY. */
  pid_t target_process_group_id = -1;
  /** Connected control socket owned by the caller after a successful launch. */
  int control_fd = -1;
};

/**
 * Starts and completes the READY handshake with a Linux executor supervisor.
 *
 * @remarks The three target stdio descriptors are copied into the supervisor;
 * the caller must close its child-side descriptors after this function
 * returns. Closing control_fd detaches a daemon supervisor and terminates a
 * non-daemon supervisor. Explicit termination is requested by sending KILL.
 *
 * @param options Supervisor executable, target configuration, and stdio.
 * @param connection Receives the connected control endpoint and process IDs.
 * @param error_message Receives a diagnostic when false is returned.
 * @return true after the supervisor reports READY.
 */
bool LaunchMuonLinuxExecutorSupervisor(
    const MuonLinuxExecutorSupervisorLaunchOptions& options,
    MuonLinuxExecutorSupervisorConnection* connection,
    std::string* error_message);

}  // namespace muon_internal
