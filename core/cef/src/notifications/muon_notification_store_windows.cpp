/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "notifications/muon_notification_store.h"
#include "muon_windows_path.h"
#include "notifications/muon_notification_windows_async.h"

#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace muon_internal {

static void ThrowWindowsStoreError(const char* operation, DWORD error) {
  throw std::runtime_error(std::string(operation) + ": Windows error " + std::to_string(error));
}

static std::wstring NativePath(const std::string& path) {
  auto native = muon_windows_path{};
  auto error = muon_windows_path_normalize_utf8(path.data(), path.size(), &native);
  if (error == ERROR_SUCCESS)
    error = muon_windows_path_for_io(&native, MUON_WINDOWS_PATH_TREE);
  if (error != ERROR_SUCCESS) {
    muon_windows_path_destroy(&native);
    ThrowWindowsStoreError("Notification state path", error);
  }
  auto result = std::wstring(native.value, native.length);
  muon_windows_path_destroy(&native);
  return result;
}

static void EnsureParentDirectory(const std::wstring& path) {
  auto native = muon_windows_path{};
  const auto error = muon_windows_path_normalize_wide(path.data(), path.size(), &native);
  if (error != ERROR_SUCCESS) ThrowWindowsStoreError("Notification parent path", error);
  const auto root_length = native.root_length;
  muon_windows_path_destroy(&native);
  // MinGW filesystem directory creation does not consistently accept the
  // extended Win32 namespace on Windows. Keep the validated native spelling
  // and create only components below its drive/share root with the W APIs.
  auto separator = path.find(L'\\', root_length);
  while (separator != std::wstring::npos) {
    const auto directory = path.substr(0, separator);
    if (!CreateDirectoryW(directory.c_str(), nullptr)) {
      const auto create_error = GetLastError();
      if (create_error != ERROR_ALREADY_EXISTS)
        ThrowWindowsStoreError("Create notification directory", create_error);
      const auto attributes = GetFileAttributesW(directory.c_str());
      if (attributes == INVALID_FILE_ATTRIBUTES)
        ThrowWindowsStoreError("Inspect notification directory", GetLastError());
      if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        ThrowWindowsStoreError("Notification parent is not a directory", ERROR_DIRECTORY);
    }
    separator = path.find(L'\\', separator + 1);
  }
}

struct WindowsNotificationFile {
  HANDLE handle = INVALID_HANDLE_VALUE;
  ~WindowsNotificationFile() {
    // Normal and error paths close asynchronously. This fallback only applies
    // when scheduling cleanup itself failed.
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
  }
};

static cardio::promise<void> CloseFile(std::shared_ptr<WindowsNotificationFile> file) {
  if (file->handle == INVALID_HANDLE_VALUE) co_return;
  co_await RunMuonWindowsNotificationTask([file] {
    const auto handle = std::exchange(file->handle, INVALID_HANDLE_VALUE);
    if (!CloseHandle(handle)) ThrowWindowsStoreError("Close notification state", GetLastError());
  });
}

cardio::promise<MuonNotificationSnapshot> LoadMuonNotificationSnapshot(std::string path) {
  const auto native = NativePath(path);
  auto file = std::make_shared<WindowsNotificationFile>();
  co_await RunMuonWindowsNotificationTask([file, native] {
    file->handle = CreateFileW(native.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (file->handle == INVALID_HANDLE_VALUE) {
      const auto error = GetLastError();
      if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
        ThrowWindowsStoreError("Open notification state", error);
    }
  });
  if (file->handle == INVALID_HANDLE_VALUE) co_return MuonNotificationSnapshot{};
  auto json = std::string{};
  auto buffer = std::array<std::byte, 65536>{};
  auto failure = std::exception_ptr{};
  try {
    while (true) {
      auto count = size_t{0};
      try { count = co_await cardio::win32::read(file->handle, buffer, json.size()); }
      catch (const std::system_error& error) {
        if (error.code().value() != ERROR_HANDLE_EOF) throw;
      }
      if (count == 0) break;
      if (json.size() + count > 32 * 1024 * 1024)
        throw std::runtime_error("Notification snapshot is too large");
      json.append(reinterpret_cast<const char*>(buffer.data()), count);
    }
  } catch (...) { failure = std::current_exception(); }
  co_await CloseFile(file);
  if (failure) std::rethrow_exception(failure);
  co_return ParseMuonNotificationSnapshot(json);
}

cardio::promise<void> SaveMuonNotificationSnapshot(std::string path,
                                                   MuonNotificationSnapshot state) {
  const auto native = NativePath(path);
  const auto temporary = NativePath(path + ".next");
  const auto json = SerializeMuonNotificationSnapshot(state);
  auto file = std::make_shared<WindowsNotificationFile>();
  co_await RunMuonWindowsNotificationTask([file, temporary] {
    EnsureParentDirectory(temporary);
    // Recreate the staging entry, so a stale hard link cannot truncate the
    // durable file. A directory at this path is an error, not removable state.
    if (!DeleteFileW(temporary.c_str())) {
      const auto error = GetLastError();
      if (error != ERROR_FILE_NOT_FOUND)
        ThrowWindowsStoreError("Remove notification staging file", error);
    }
    file->handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (file->handle == INVALID_HANDLE_VALUE)
      ThrowWindowsStoreError("Create notification staging file", GetLastError());
  });
  auto failure = std::exception_ptr{};
  try {
    const auto bytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(json.data()), json.size());
    auto offset = size_t{0};
    while (offset < bytes.size()) {
      const auto count = co_await cardio::win32::write(file->handle, bytes.subspan(offset), offset);
      if (count == 0) throw std::runtime_error("Notification snapshot write made no progress");
      offset += count;
    }
    co_await RunMuonWindowsNotificationTask([file] {
      if (!FlushFileBuffers(file->handle))
        ThrowWindowsStoreError("Flush notification state", GetLastError());
    });
  } catch (...) { failure = std::current_exception(); }
  co_await CloseFile(file);
  if (failure) std::rethrow_exception(failure);
  co_await RunMuonWindowsNotificationTask([temporary, native] {
    // The sibling rename stays on one volume. Never enable a copy-and-delete
    // fallback, which could expose a partially written destination.
    if (!MoveFileExW(temporary.c_str(), native.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
      ThrowWindowsStoreError("Commit notification state", GetLastError());
  });
}

cardio::promise<std::string> CreateMuonNotificationToken() {
  auto bytes = std::array<unsigned char, 16>{};
  const auto status = BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                                     BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  if (status < 0) throw std::runtime_error("Notification random source failed: " + std::to_string(status));
  // A UUID v4 fits the two 16-character Windows toast identity fields without
  // truncating a revision identifier or mapping different revisions to one ID.
  bytes[6] = static_cast<unsigned char>((bytes[6] & 15) | 64);
  bytes[8] = static_cast<unsigned char>((bytes[8] & 63) | 128);
  auto token = std::string{};
  token.reserve(36);
  static constexpr char hexadecimal[] = "0123456789abcdef";
  for (size_t index = 0; index < bytes.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) token += '-';
    const auto byte = bytes[index];
    token += hexadecimal[byte >> 4];
    token += hexadecimal[byte & 15];
  }
  co_return token;
}

}  // namespace muon_internal
