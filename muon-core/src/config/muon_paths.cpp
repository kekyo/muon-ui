/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "config/muon_paths.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

#include <string>

bool GetMuonExecutablePath(std::filesystem::path* path) {
  if (path == nullptr) {
    return false;
  }
  path->clear();
#if defined(_WIN32)
  std::wstring executable_path(32768, L'\0');
  const auto length = GetModuleFileNameW(
      nullptr, executable_path.data(),
      static_cast<DWORD>(executable_path.size()));
  if (length > 0 &&
      static_cast<size_t>(length) < executable_path.size()) {
    executable_path.resize(static_cast<size_t>(length));
    *path = std::filesystem::path(executable_path).lexically_normal();
    return true;
  }
#else
  char executable_path[PATH_MAX];
  const auto length =
      readlink("/proc/self/exe", executable_path, sizeof(executable_path) - 1);
  if (length > 0) {
    executable_path[length] = '\0';
    *path = std::filesystem::path(executable_path).lexically_normal();
    return true;
  }
#endif
  return false;
}

std::filesystem::path GetMuonExecutableDirectory() {
  std::filesystem::path executable_path;
  if (GetMuonExecutablePath(&executable_path)) {
    return executable_path.parent_path();
  }
  return std::filesystem::current_path();
}
