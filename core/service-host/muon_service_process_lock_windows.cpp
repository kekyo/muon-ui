/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "muon_service_process_lock.h"
#include <windows.h>
#include <sddl.h>
#include <shlobj.h>
#include <vector>

static void Check(bool success, const char* operation) {
  if (!success) throw std::system_error(static_cast<int>(GetLastError()),
                                      std::system_category(), operation);
}

struct Handle {
  HANDLE value = INVALID_HANDLE_VALUE;
  ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

static std::wstring LogonSid() {
  Handle token;
  Check(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value) != 0, "open lock token");
  DWORD size = 0;
  GetTokenInformation(token.value, TokenLogonSid, nullptr, 0, &size);
  Check(GetLastError() == ERROR_INSUFFICIENT_BUFFER, "size lock logon SID");
  std::vector<unsigned char> bytes(size);
  Check(GetTokenInformation(token.value, TokenLogonSid, bytes.data(), size, &size) != 0,
        "read lock logon SID");
  const auto* groups = reinterpret_cast<TOKEN_GROUPS*>(bytes.data());
  if (groups->GroupCount != 1) throw std::runtime_error("Missing lock logon SID");
  LPWSTR sid = nullptr;
  Check(ConvertSidToStringSidW(groups->Groups[0].Sid, &sid) != 0, "format lock logon SID");
  auto owner = std::shared_ptr<void>(sid, [](void* value) { LocalFree(value); });
  return sid;
}

cardio::promise<std::shared_ptr<void>> AcquireMuonServiceProcessLock(
    std::string key, cardio::cancellation cancellation) {
  if (cancellation.is_cancellation_requested()) throw cardio::canceled_exception();
  const auto sid = LogonSid();
  const auto sddl = L"D:P(A;;GA;;;" + sid + L")";
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  Check(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
      &descriptor, nullptr) != 0, "create process lock security");
  auto security = std::shared_ptr<void>(descriptor, [](void* value) { LocalFree(value); });
  SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
  // TMP/TEMP may differ between login and UI launches. A known OS user folder
  // gives both processes the same lock identity regardless of inherited env.
  PWSTR folder = nullptr;
  const auto result = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &folder);
  auto folder_owner = std::shared_ptr<void>(folder, CoTaskMemFree);
  if (FAILED(result)) throw std::system_error(static_cast<int>(result),
      std::system_category(), "locate service process lock directory");
  const auto directory = std::wstring(folder) + L"\\muon-service-locks";
  if (!CreateDirectoryW(directory.c_str(), nullptr))
    Check(GetLastError() == ERROR_ALREADY_EXISTS, "create service process lock directory");
  const auto hash = MuonServiceProcessLockHash(key);
  const auto path = directory + L"\\" + sid + L"-" +
      std::wstring(hash.begin(), hash.end());
  auto lock = std::make_shared<Handle>();
  lock->value = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, OPEN_ALWAYS,
      FILE_FLAG_OVERLAPPED | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  Check(lock->value != INVALID_HANDLE_VALUE, "open service process lock");
  BY_HANDLE_FILE_INFORMATION information{};
  Check(GetFileInformationByHandle(lock->value, &information) != 0, "inspect service process lock");
  if (GetFileType(lock->value) != FILE_TYPE_DISK ||
      (information.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
    throw std::runtime_error("Service process lock must be a regular file");
  // LockFileEx has no acquiring-thread affinity. A named mutex would instead be
  // owned by cardio's helper wait thread, preventing release on this dispatcher.
  // submit retains OVERLAPPED until native completion, including cancellation.
  co_await cardio::win32::submit(lock->value, [](HANDLE file, OVERLAPPED& overlapped) {
    return LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &overlapped)
        ? static_cast<DWORD>(ERROR_SUCCESS) : GetLastError();
  }, cancellation);
  co_return lock;
}
