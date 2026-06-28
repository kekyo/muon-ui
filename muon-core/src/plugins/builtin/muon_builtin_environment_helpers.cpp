/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "plugins/builtin/muon_builtin_environment_helpers.h"

#if defined(_WIN32)
#include <windows.h>

#include <cwchar>
#else
#include <cstring>
#endif

#if !defined(_WIN32)
extern char** environ;
#endif

namespace muon_internal {

#if defined(_WIN32)

bool MuonWideToUtf8(const wchar_t* source, std::string* target) {
  if (source == nullptr || target == nullptr) {
    return false;
  }
  const auto required = WideCharToMultiByte(
      CP_UTF8, 0, source, -1, nullptr, 0, nullptr, nullptr);
  if (required <= 0) {
    return false;
  }
  std::string converted;
  converted.resize(static_cast<size_t>(required));
  if (required > 1 &&
      WideCharToMultiByte(CP_UTF8, 0, source, -1, converted.data(), required,
                          nullptr, nullptr) <= 0) {
    return false;
  }
  converted.resize(static_cast<size_t>(required - 1));
  *target = std::move(converted);
  return true;
}

bool MuonUtf8ToWide(const std::string& source, std::wstring* target) {
  if (target == nullptr) {
    return false;
  }
  const auto required = MultiByteToWideChar(
      CP_UTF8, 0, source.c_str(), -1, nullptr, 0);
  if (required <= 0) {
    return false;
  }
  std::wstring converted;
  converted.resize(static_cast<size_t>(required));
  if (required > 1 &&
      MultiByteToWideChar(CP_UTF8, 0, source.c_str(), -1, converted.data(),
                          required) <= 0) {
    return false;
  }
  converted.resize(static_cast<size_t>(required - 1));
  *target = std::move(converted);
  return true;
}

std::vector<std::pair<std::string, std::string>> GetMuonEnvironmentEntries() {
  std::vector<std::pair<std::string, std::string>> entries;
  auto* block = GetEnvironmentStringsW();
  if (block == nullptr) {
    return entries;
  }
  for (auto* entry = block; *entry != L'\0'; entry += std::wcslen(entry) + 1) {
    const auto* separator = std::wcschr(entry, L'=');
    if (separator == nullptr || separator == entry) {
      continue;
    }
    std::wstring key_wide(entry, separator - entry);
    std::wstring value_wide(separator + 1);
    std::string key;
    std::string value;
    if (MuonWideToUtf8(key_wide.c_str(), &key) &&
        MuonWideToUtf8(value_wide.c_str(), &value)) {
      entries.emplace_back(std::move(key), std::move(value));
    }
  }
  FreeEnvironmentStringsW(block);
  return entries;
}

#else

std::vector<std::pair<std::string, std::string>> GetMuonEnvironmentEntries() {
  std::vector<std::pair<std::string, std::string>> entries;
  if (environ == nullptr) {
    return entries;
  }
  for (auto index = size_t{0}; environ[index] != nullptr; ++index) {
    const auto* entry = environ[index];
    const auto* separator = std::strchr(entry, '=');
    if (separator == nullptr) {
      entries.emplace_back(entry, "");
      continue;
    }
    entries.emplace_back(std::string(entry, separator - entry), separator + 1);
  }
  return entries;
}

#endif

}  // namespace muon_internal
