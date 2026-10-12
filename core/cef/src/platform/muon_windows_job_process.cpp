/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "muon_windows_job_process.h"

#include <cstddef>
#include <memory>
#include <new>

static void CloseMuonWindowsJobHandle(HANDLE* handle) noexcept {
  if (handle == nullptr || *handle == nullptr ||
      *handle == INVALID_HANDLE_VALUE) {
    return;
  }
  (void)CloseHandle(*handle);
  *handle = nullptr;
}

static bool SetMuonWindowsJobProcessLaunchError(
    MuonWindowsJobProcessLaunchError* error,
    DWORD windows_error,
    const char* message) noexcept {
  if (error != nullptr) {
    error->windows_error = windows_error;
    error->message = message;
  }
  return false;
}

static bool ValidateMuonWindowsInheritedHandles(
    const MuonWindowsJobProcessLaunchOptions& options,
    MuonWindowsJobProcessLaunchError* error) noexcept {
  if (options.inherited_handle_count == 0) {
    return true;
  }
  if (options.inherited_handles == nullptr) {
    return SetMuonWindowsJobProcessLaunchError(
        error, ERROR_INVALID_PARAMETER,
        "inherited_handles is required when inherited_handle_count is set");
  }
  for (size_t index = 0; index < options.inherited_handle_count; ++index) {
    const auto handle = options.inherited_handles[index];
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
      return SetMuonWindowsJobProcessLaunchError(
          error, ERROR_INVALID_HANDLE,
          "inherited_handles contains an invalid handle");
    }
    DWORD flags = 0;
    if (GetHandleInformation(handle, &flags) == FALSE) {
      return SetMuonWindowsJobProcessLaunchError(
          error, GetLastError(),
          "GetHandleInformation failed for an inherited process handle");
    }
    if ((flags & HANDLE_FLAG_INHERIT) == 0) {
      return SetMuonWindowsJobProcessLaunchError(
          error, ERROR_INVALID_HANDLE,
          "inherited_handles contains a non-inheritable handle");
    }
  }
  return true;
}

bool LaunchMuonWindowsJobProcess(
    const MuonWindowsJobProcessLaunchOptions& options,
    MuonWindowsJobProcess* process,
    MuonWindowsJobProcessLaunchError* error) noexcept {
  if (error != nullptr) {
    *error = MuonWindowsJobProcessLaunchError{};
  }
  if (process == nullptr) {
    return SetMuonWindowsJobProcessLaunchError(
        error, ERROR_INVALID_PARAMETER, "process output is required");
  }
  if (process->process != nullptr || process->job != nullptr ||
      process->process_id != 0) {
    return SetMuonWindowsJobProcessLaunchError(
        error, ERROR_INVALID_PARAMETER, "process output must be empty");
  }
  if (options.command_line == nullptr ||
      options.command_line[0] == L'\0') {
    return SetMuonWindowsJobProcessLaunchError(
        error, ERROR_INVALID_PARAMETER, "command_line is required");
  }
  if (!ValidateMuonWindowsInheritedHandles(options, error)) {
    return false;
  }

  auto job = CreateJobObjectW(nullptr, nullptr);
  if (job == nullptr) {
    return SetMuonWindowsJobProcessLaunchError(
        error, GetLastError(),
        "CreateJobObjectW failed");
  }

  LPPROC_THREAD_ATTRIBUTE_LIST attribute_list = nullptr;
  auto attribute_list_initialized = false;
  auto attribute_storage = std::unique_ptr<std::byte[]>{};
  PROCESS_INFORMATION process_info{};
  const auto fail = [&](DWORD windows_error, const char* message) {
    if (attribute_list_initialized) {
      DeleteProcThreadAttributeList(attribute_list);
    }
    if (process_info.hProcess != nullptr) {
      (void)TerminateJobObject(job, 137u);
    }
    CloseMuonWindowsJobHandle(&process_info.hThread);
    CloseMuonWindowsJobHandle(&process_info.hProcess);
    CloseMuonWindowsJobHandle(&job);
    return SetMuonWindowsJobProcessLaunchError(
        error, windows_error, message);
  };

  if (options.lifetime ==
      MuonWindowsJobProcessLifetime::KillOnOwnerClose) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (SetInformationJobObject(
            job, JobObjectExtendedLimitInformation, &limits,
            sizeof(limits)) == FALSE) {
      return fail(GetLastError(), "SetInformationJobObject failed");
    }
  }

  const auto attribute_count =
      options.inherited_handle_count == 0 ? DWORD{1} : DWORD{2};
  SIZE_T attribute_size = 0;
  // The sizing call fails by design and reports the required byte count.
  (void)InitializeProcThreadAttributeList(
      nullptr, attribute_count, 0, &attribute_size);
  if (attribute_size == 0) {
    return fail(
        GetLastError(), "Failed to size the process attribute list");
  }
  attribute_storage.reset(
      new (std::nothrow) std::byte[attribute_size]);
  if (!attribute_storage) {
    return fail(
        ERROR_NOT_ENOUGH_MEMORY,
        "Failed to allocate the process attribute list");
  }
  attribute_list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
      attribute_storage.get());
  if (InitializeProcThreadAttributeList(
          attribute_list, attribute_count, 0,
          &attribute_size) == FALSE) {
    return fail(
        GetLastError(), "Failed to initialize the process attribute list");
  }
  attribute_list_initialized = true;

  if (UpdateProcThreadAttribute(
          attribute_list, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
          &job, sizeof(job), nullptr, nullptr) == FALSE) {
    return fail(
        GetLastError(), "Failed to assign the process job attribute");
  }
  if (options.inherited_handle_count != 0 &&
      UpdateProcThreadAttribute(
          attribute_list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
          const_cast<HANDLE*>(options.inherited_handles),
          sizeof(HANDLE) * options.inherited_handle_count,
          nullptr, nullptr) == FALSE) {
    return fail(
        GetLastError(), "Failed to restrict inherited process handles");
  }

  BOOL current_process_in_job = FALSE;
  if (IsProcessInJob(
          GetCurrentProcess(), nullptr,
          &current_process_in_job) == FALSE) {
    return fail(GetLastError(), "IsProcessInJob failed");
  }

  auto creation_flags =
      options.creation_flags | EXTENDED_STARTUPINFO_PRESENT;
  const auto breakaway_requested =
      current_process_in_job != FALSE &&
      options.lifetime == MuonWindowsJobProcessLifetime::Detached;
  if (breakaway_requested) {
    // A nested host job could otherwise keep detached descendants alive or
    // terminate them independently of Muon's selected lifetime policy.
    creation_flags |= CREATE_BREAKAWAY_FROM_JOB;
  }
  STARTUPINFOEXW startup{};
  startup.StartupInfo = options.startup_info;
  startup.StartupInfo.cb = sizeof(startup);
  startup.lpAttributeList = attribute_list;
  const auto inherit_handles =
      options.inherited_handle_count == 0 ? FALSE : TRUE;
  if (CreateProcessW(
          options.application_name, options.command_line,
          nullptr, nullptr, inherit_handles, creation_flags,
          options.environment, options.current_directory,
          &startup.StartupInfo, &process_info) == FALSE) {
    const auto windows_error = GetLastError();
    return fail(
        windows_error,
        breakaway_requested
            ? "CreateProcessW failed while breaking away from the parent job"
            : "CreateProcessW failed");
  }

  DeleteProcThreadAttributeList(attribute_list);
  attribute_list_initialized = false;
  attribute_list = nullptr;
  CloseMuonWindowsJobHandle(&process_info.hThread);

  process->process = process_info.hProcess;
  process->job = job;
  process->process_id = process_info.dwProcessId;
  return true;
}

bool TerminateMuonWindowsJobProcess(
    const MuonWindowsJobProcess* process,
    unsigned int exit_code) noexcept {
  return process != nullptr && process->job != nullptr &&
         TerminateJobObject(process->job, exit_code) != FALSE;
}

bool EnableMuonWindowsJobKillOnClose(
    const MuonWindowsJobProcess* process) noexcept {
  if (process == nullptr || process->job == nullptr) {
    return false;
  }
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  if (QueryInformationJobObject(
          process->job, JobObjectExtendedLimitInformation, &limits,
          sizeof(limits), nullptr) == FALSE) {
    return false;
  }
  if ((limits.BasicLimitInformation.LimitFlags &
       JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE) != 0) {
    return true;
  }
  limits.BasicLimitInformation.LimitFlags |=
      JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  return SetInformationJobObject(
             process->job, JobObjectExtendedLimitInformation, &limits,
             sizeof(limits)) != FALSE;
}

void CloseMuonWindowsJobProcess(
    MuonWindowsJobProcess* process) noexcept {
  if (process == nullptr) {
    return;
  }
  CloseMuonWindowsJobHandle(&process->process);
  CloseMuonWindowsJobHandle(&process->job);
  process->process_id = 0;
}
