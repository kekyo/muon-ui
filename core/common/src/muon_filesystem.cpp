/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "muon_filesystem.h"

#if defined(_WIN32)
#include "muon_windows_path.h"
#include <memory>
#include <vector>
#endif

namespace muon_internal {

std::filesystem::path FilesystemPathFromUtf8(std::string_view value) {
  return std::filesystem::path(std::u8string_view(
      reinterpret_cast<const char8_t*>(value.data()), value.size()));
}

std::string FilesystemPathToUtf8(const std::filesystem::path& path) {
  const auto value = path.u8string();
  return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

std::filesystem::path ResolveFilesystemPath(const std::filesystem::path& base,
                                            const std::filesystem::path& path) {
#if defined(_WIN32)
  // MinGW does not recognize the extended namespace as absolute and removes a
  // leading separator in lexically_normal(). Keep namespace parsing in the
  // shared Windows converter, including ordinary UNC roots.
  const auto absolute = path.is_absolute() || path.native().starts_with(L"\\\\") ||
                        path.native().starts_with(L"//");
  const auto relative_to_exact_base = !absolute && !path.has_root_path() &&
                                    base.native().starts_with(L"\\\\?\\");
  const auto input = relative_to_exact_base ? base : absolute ? path : base / path;
  auto normalized = muon_windows_path{};
  const auto owner = std::unique_ptr<muon_windows_path,
      decltype(&muon_windows_path_destroy)>(&normalized, &muon_windows_path_destroy);
  const auto error = muon_windows_path_normalize_wide(
      input.c_str(), input.native().size(), &normalized);
  if (error != ERROR_SUCCESS) {
    throw std::system_error(static_cast<int>(error), std::system_category(),
                            "Filesystem path resolution failed");
  }
  auto result = std::filesystem::path(normalized.value);
  if (relative_to_exact_base) {
    for (const auto& component : path.lexically_normal()) {
      if (component.empty() || component == L".") continue;
      if (component == L"..") {
        if (result.native().size() > normalized.root_length) result = result.parent_path();
      } else {
        result /= component;
      }
    }
  }
  return result;
#else
  return (path.is_absolute() ? path : base / path).lexically_normal();
#endif
}

std::filesystem::path FilesystemPathForIo(const std::filesystem::path& path,
                                         FilesystemIoUsage usage) {
#if defined(_WIN32)
  auto native = muon_windows_path{};
  const auto owner = std::unique_ptr<muon_windows_path,
      decltype(&muon_windows_path_destroy)>(&native, &muon_windows_path_destroy);
  auto error = muon_windows_path_normalize_wide(
      path.c_str(), path.native().size(), &native);
  if (error == ERROR_SUCCESS) {
    error = muon_windows_path_for_io(&native,
        usage == FilesystemIoUsage::Tree ? MUON_WINDOWS_PATH_TREE
                                        : MUON_WINDOWS_PATH_FILE);
  }
  if (error != ERROR_SUCCESS) {
    throw std::system_error(static_cast<int>(error), std::system_category(),
                            "Filesystem path conversion failed");
  }
  return std::filesystem::path(native.value);
#else
  (void)usage;
  return path;
#endif
}

#if defined(_WIN32)
static std::filesystem::path TryFilesystemPathForIo(
    const std::filesystem::path& path, FilesystemIoUsage usage,
    std::error_code& error) {
  error.clear();
  try {
    return FilesystemPathForIo(path, usage);
  } catch (const std::system_error& failure) {
    error = failure.code();
    return {};
  }
}
#endif

std::filesystem::file_type GetFilesystemType(const std::filesystem::path& path,
                                            std::error_code& error) {
#if defined(_WIN32)
  const auto native = TryFilesystemPathForIo(path, FilesystemIoUsage::File, error);
  if (error) return std::filesystem::file_type::none;
  // MinGW's _wstat64 rejects extended paths. Query the opened target so a
  // dangling link is missing, and a link to a directory has directory type.
  const auto raw = CreateFileW(native.c_str(), 0,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (raw == INVALID_HANDLE_VALUE) {
    const auto code = GetLastError();
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
      return std::filesystem::file_type::not_found;
    }
    error.assign(static_cast<int>(code), std::system_category());
    return std::filesystem::file_type::none;
  }
  const auto handle = std::unique_ptr<void, decltype(&CloseHandle)>(raw, &CloseHandle);
  auto information = BY_HANDLE_FILE_INFORMATION{};
  if (!GetFileInformationByHandle(handle.get(), &information)) {
    error.assign(static_cast<int>(GetLastError()), std::system_category());
    return std::filesystem::file_type::none;
  }
  return (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0
      ? std::filesystem::file_type::directory : std::filesystem::file_type::regular;
#else
  const auto type = std::filesystem::status(path, error).type();
  if (type == std::filesystem::file_type::not_found) error.clear();
  return type;
#endif
}

std::filesystem::path CanonicalFilesystemPath(const std::filesystem::path& path,
                                              std::error_code& error) {
#if defined(_WIN32)
  const auto native = TryFilesystemPathForIo(path, FilesystemIoUsage::File, error);
  if (error) return {};
  const auto raw = CreateFileW(native.c_str(), 0,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (raw == INVALID_HANDLE_VALUE) {
    error.assign(static_cast<int>(GetLastError()), std::system_category());
    return {};
  }
  const auto handle = std::unique_ptr<void, decltype(&CloseHandle)>(raw, &CloseHandle);
  auto result = std::wstring(260, L'\0');
  for (;;) {
    const auto length = GetFinalPathNameByHandleW(handle.get(), result.data(),
        static_cast<DWORD>(result.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (length == 0) {
      error.assign(static_cast<int>(GetLastError()), std::system_category());
      return {};
    }
    if (length < result.size()) {
      result.resize(length);
      return std::filesystem::path(result);
    }
    // Insufficient-buffer results include the terminating NUL.
    result.resize(length);
  }
#else
  return std::filesystem::canonical(path, error);
#endif
}

bool CreateFilesystemDirectories(const std::filesystem::path& path,
                                 std::error_code& error) {
#if defined(_WIN32)
  const auto native = TryFilesystemPathForIo(path, FilesystemIoUsage::Tree, error);
  if (error) return false;
  auto missing = std::vector<std::filesystem::path>{};
  auto parent = native;
  for (;;) {
    const auto attributes = GetFileAttributesW(parent.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
      if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        error.assign(ERROR_DIRECTORY, std::system_category());
        return false;
      }
      break;
    }
    const auto code = GetLastError();
    const auto next = parent.parent_path();
    if ((code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND) ||
        next.empty() || next == parent) {
      error.assign(static_cast<int>(code), std::system_category());
      return false;
    }
    missing.push_back(parent);
    parent = next;
  }
  for (auto entry = missing.rbegin(); entry != missing.rend(); ++entry) {
    if (CreateDirectoryW(entry->c_str(), nullptr)) continue;
    const auto code = GetLastError();
    if (code == ERROR_ALREADY_EXISTS) {
      const auto attributes = GetFileAttributesW(entry->c_str());
      if (attributes != INVALID_FILE_ATTRIBUTES &&
          (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
    }
    error.assign(static_cast<int>(code), std::system_category());
    return false;
  }
  return true;
#else
  std::filesystem::create_directories(path, error);
  return !error;
#endif
}

}  // namespace muon_internal
