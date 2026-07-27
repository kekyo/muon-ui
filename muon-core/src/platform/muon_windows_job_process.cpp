/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "muon_windows_job_process.h"

#include <cstddef>
#include <stdexcept>
#include <system_error>
#include <vector>

static void CloseMuonWindowsJobHandle(HANDLE* handle) noexcept {
  if (handle == nullptr || *handle == nullptr ||
      *handle == INVALID_HANDLE_VALUE) {
    return;
  }
  (void)CloseHandle(*handle);
  *handle = nullptr;
}

static void ValidateMuonWindowsInheritedHandles(
    const MuonWindowsJobProcessLaunchOptions& options) {
  if (options.inherited_handle_count == 0) {
    return;
  }
  if (options.inherited_handles == nullptr) {
    throw std::invalid_argument(
        "inherited_handles is required when inherited_handle_count is set");
  }
  for (size_t index = 0; index < options.inherited_handle_count; ++index) {
    const auto handle = options.inherited_handles[index];
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
      throw std::invalid_argument(
          "inherited_handles contains an invalid handle");
    }
    DWORD flags = 0;
    if (GetHandleInformation(handle, &flags) == FALSE) {
      throw std::system_error(
          static_cast<int>(GetLastError()), std::system_category(),
          "GetHandleInformation failed for an inherited process handle");
    }
    if ((flags & HANDLE_FLAG_INHERIT) == 0) {
      throw std::invalid_argument(
          "inherited_handles contains a non-inheritable handle");
    }
  }
}

MuonWindowsJobProcess LaunchMuonWindowsJobProcess(
    const MuonWindowsJobProcessLaunchOptions& options) {
  if (options.command_line == nullptr ||
      options.command_line[0] == L'\0') {
    throw std::invalid_argument("command_line is required");
  }
  ValidateMuonWindowsInheritedHandles(options);

  auto job = CreateJobObjectW(nullptr, nullptr);
  if (job == nullptr) {
    throw std::system_error(
        static_cast<int>(GetLastError()), std::system_category(),
        "CreateJobObjectW failed");
  }

  LPPROC_THREAD_ATTRIBUTE_LIST attribute_list = nullptr;
  auto attribute_list_initialized = false;
  PROCESS_INFORMATION process_info{};
  try {
    if (options.lifetime ==
        MuonWindowsJobProcessLifetime::KillOnOwnerClose) {
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
      limits.BasicLimitInformation.LimitFlags =
          JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
      if (SetInformationJobObject(
              job, JobObjectExtendedLimitInformation, &limits,
              sizeof(limits)) == FALSE) {
        throw std::system_error(
            static_cast<int>(GetLastError()), std::system_category(),
            "SetInformationJobObject failed");
      }
    }

    const auto attribute_count =
        options.inherited_handle_count == 0 ? DWORD{1} : DWORD{2};
    SIZE_T attribute_size = 0;
    // The sizing call fails by design and reports the required byte count.
    (void)InitializeProcThreadAttributeList(
        nullptr, attribute_count, 0, &attribute_size);
    if (attribute_size == 0) {
      throw std::system_error(
          static_cast<int>(GetLastError()), std::system_category(),
          "Failed to size the process attribute list");
    }
    std::vector<std::byte> attribute_storage(attribute_size);
    attribute_list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
        attribute_storage.data());
    if (InitializeProcThreadAttributeList(
            attribute_list, attribute_count, 0,
            &attribute_size) == FALSE) {
      throw std::system_error(
          static_cast<int>(GetLastError()), std::system_category(),
          "Failed to initialize the process attribute list");
    }
    attribute_list_initialized = true;

    if (UpdateProcThreadAttribute(
            attribute_list, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
            &job, sizeof(job), nullptr, nullptr) == FALSE) {
      throw std::system_error(
          static_cast<int>(GetLastError()), std::system_category(),
          "Failed to assign the process job attribute");
    }
    if (options.inherited_handle_count != 0 &&
        UpdateProcThreadAttribute(
            attribute_list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            const_cast<HANDLE*>(options.inherited_handles),
            sizeof(HANDLE) * options.inherited_handle_count,
            nullptr, nullptr) == FALSE) {
      throw std::system_error(
          static_cast<int>(GetLastError()), std::system_category(),
          "Failed to restrict inherited process handles");
    }

    BOOL current_process_in_job = FALSE;
    if (IsProcessInJob(
            GetCurrentProcess(), nullptr,
            &current_process_in_job) == FALSE) {
      throw std::system_error(
          static_cast<int>(GetLastError()), std::system_category(),
          "IsProcessInJob failed");
    }

    auto creation_flags =
        options.creation_flags | EXTENDED_STARTUPINFO_PRESENT;
    if (current_process_in_job != FALSE) {
      // A nested host job could otherwise keep detached descendants alive or
      // terminate them independently of Muon's selected lifetime policy.
      creation_flags |= CREATE_BREAKAWAY_FROM_JOB;
    }
    if (options.lifetime ==
        MuonWindowsJobProcessLifetime::Detached) {
      creation_flags |= DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP;
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
      const auto error = GetLastError();
      throw std::system_error(
          static_cast<int>(error), std::system_category(),
          current_process_in_job != FALSE
              ? "CreateProcessW failed while breaking away from the parent job"
              : "CreateProcessW failed");
    }

    DeleteProcThreadAttributeList(attribute_list);
    attribute_list_initialized = false;
    attribute_list = nullptr;
    CloseMuonWindowsJobHandle(&process_info.hThread);

    MuonWindowsJobProcess process{};
    process.process = process_info.hProcess;
    process.job = job;
    process.process_id = process_info.dwProcessId;
    process_info.hProcess = nullptr;
    job = nullptr;
    return process;
  } catch (...) {
    if (attribute_list_initialized) {
      DeleteProcThreadAttributeList(attribute_list);
    }
    if (process_info.hProcess != nullptr) {
      (void)TerminateJobObject(job, 137u);
    }
    CloseMuonWindowsJobHandle(&process_info.hThread);
    CloseMuonWindowsJobHandle(&process_info.hProcess);
    CloseMuonWindowsJobHandle(&job);
    throw;
  }
}

bool TerminateMuonWindowsJobProcess(
    const MuonWindowsJobProcess* process,
    unsigned int exit_code) noexcept {
  return process != nullptr && process->job != nullptr &&
         TerminateJobObject(process->job, exit_code) != FALSE;
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
