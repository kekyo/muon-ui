/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#ifndef MUON_WINDOWS_JOB_PROCESS_H
#define MUON_WINDOWS_JOB_PROCESS_H

#if !defined(_WIN32)
#error "muon_windows_job_process is available only on Windows"
#endif

#include <cstddef>

#include <windows.h>

/**
 * Defines how the job object behaves when its owner releases the last handle.
 */
enum class MuonWindowsJobProcessLifetime {
  /** Terminates every process in the job when its last handle is closed. */
  KillOnOwnerClose,
  /** Leaves every process in the job running when its last handle is closed. */
  Detached,
};

/**
 * Describes a process that must be created atomically inside a new job object.
 */
struct MuonWindowsJobProcessLaunchOptions {
  /** Optional executable path passed to CreateProcessW. */
  const wchar_t* application_name = nullptr;
  /** Writable command-line buffer passed to CreateProcessW. */
  wchar_t* command_line = nullptr;
  /** Optional current directory passed to CreateProcessW. */
  const wchar_t* current_directory = nullptr;
  /** Optional environment block passed to CreateProcessW. */
  void* environment = nullptr;
  /** Caller-defined CreateProcessW creation flags. */
  DWORD creation_flags = 0;
  /** Startup information copied into the extended startup structure. */
  STARTUPINFOW startup_info{};
  /** Explicit list of inheritable handles available to the child. */
  const HANDLE* inherited_handles = nullptr;
  /** Number of entries in inherited_handles. */
  size_t inherited_handle_count = 0;
  /** Lifetime policy applied to the new job object. */
  MuonWindowsJobProcessLifetime lifetime =
      MuonWindowsJobProcessLifetime::KillOnOwnerClose;
};

/**
 * Owns the root process handle and the job containing its process tree.
 */
struct MuonWindowsJobProcess {
  /** Root process handle returned by CreateProcessW. */
  HANDLE process = nullptr;
  /** Job object containing the root process and its descendants. */
  HANDLE job = nullptr;
  /** Identifier of the root process. */
  DWORD process_id = 0;
};

/**
 * Describes why a process could not be launched inside its job object.
 */
struct MuonWindowsJobProcessLaunchError {
  /** Win32 error code associated with the failed operation. */
  DWORD windows_error = ERROR_SUCCESS;
  /** Static description of the operation or validation that failed. */
  const char* message = nullptr;
};

/**
 * Creates a process atomically inside a dedicated job object.
 *
 * @param options Process creation and job lifetime options.
 * @param process Receives handles that own the process and its job. Must point
 *     to an empty value.
 * @param error Receives a Win32 error code and static failure description.
 * @return true when the process was launched; otherwise false. A detached
 *     launch from a calling process that belongs to a job requests breakaway
 *     and fails if the parent job does not permit it.
 */
[[nodiscard]] bool LaunchMuonWindowsJobProcess(
    const MuonWindowsJobProcessLaunchOptions& options,
    MuonWindowsJobProcess* process,
    MuonWindowsJobProcessLaunchError* error) noexcept;

/**
 * Terminates every process currently associated with a job.
 *
 * @param process Process and job handles returned by
 *     LaunchMuonWindowsJobProcess.
 * @param exit_code Exit code assigned to terminated processes.
 * @return true when TerminateJobObject succeeds; otherwise false.
 */
[[nodiscard]] bool TerminateMuonWindowsJobProcess(
    const MuonWindowsJobProcess* process,
    unsigned int exit_code) noexcept;

/**
 * Adds kill-on-close behavior to an existing process job.
 *
 * @param process Process and job handles returned by
 *     LaunchMuonWindowsJobProcess.
 * @return true when the existing limits were preserved and
 *     JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE is active; otherwise false.
 */
[[nodiscard]] bool EnableMuonWindowsJobKillOnClose(
    const MuonWindowsJobProcess* process) noexcept;

/**
 * Closes the process and job handles.
 *
 * @param process Process and job handles to close. Closing a
 *     KillOnOwnerClose job terminates its remaining processes; closing a
 *     Detached job only releases ownership.
 */
void CloseMuonWindowsJobProcess(
    MuonWindowsJobProcess* process) noexcept;

#endif
