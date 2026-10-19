// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "config/muon_autostart.h"

static constexpr wchar_t run_key[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

int wmain(int argc, wchar_t** argv) {
  if (argc != 2) return 2;
  const auto length = static_cast<size_t>(_wtoi(argv[1]));
  if (length < 140 || length > 4200) return 2;
  const auto name = L"muon-path-" + std::to_wstring(GetCurrentProcessId()) +
      L"-" + std::to_wstring(GetTickCount64()) + L"-日本語😀";
  const auto filename = name + L".exe";
  auto path = std::wstring(L"C:\\");
  while (path.size() + filename.size() + 73 < length - 2) {
    path += std::wstring(70, L'p') + L"\\";
  }
  path += std::wstring(length - path.size() - filename.size() - 3, L'q') +
      L"\\" + filename;
  const auto expected = L"\"" + path + L"\"";
  if (expected.size() != length) return 3;

  // Use a unique temporary value and inspect the actual registry independently
  // of the product's getter. Never replace another application's registration.
  if (RegGetValueW(HKEY_CURRENT_USER, run_key, name.c_str(), RRF_RT_REG_SZ,
                  nullptr, nullptr, nullptr) != ERROR_FILE_NOT_FOUND) return 4;
  MuonAutostartOptions options{path, "normal", ""};
  MuonAutostartStatus status = kMuonAutostartStatusUnknown;
  std::string error;
  if (!GetMuonAutostartStatus(options, &status, &error) ||
      status != kMuonAutostartStatusDisabled) return 5;
  const auto enabled = SetMuonAutostart(options, true, &error);
  auto passed = true;
  if (length <= 260) {
    std::vector<wchar_t> actual(length + 1);
    DWORD bytes = static_cast<DWORD>(actual.size() * sizeof(wchar_t));
    passed = enabled &&
        RegGetValueW(HKEY_CURRENT_USER, run_key, name.c_str(), RRF_RT_REG_SZ,
                     nullptr, actual.data(), &bytes) == ERROR_SUCCESS &&
        expected == actual.data() &&
        GetMuonAutostartStatus(options, &status, &error) &&
        status == kMuonAutostartStatusEnabled;
  } else {
    passed = !enabled && error == "Windows Run command is too long" &&
        RegGetValueW(HKEY_CURRENT_USER, run_key, name.c_str(), RRF_RT_REG_SZ,
                     nullptr, nullptr, nullptr) == ERROR_FILE_NOT_FOUND;
  }
  std::string cleanup_error;
  passed = SetMuonAutostart(options, false, &cleanup_error) && passed;
  passed = RegGetValueW(HKEY_CURRENT_USER, run_key, name.c_str(), RRF_RT_REG_SZ,
                        nullptr, nullptr, nullptr) == ERROR_FILE_NOT_FOUND && passed;
  if (!passed) std::fprintf(stderr, "Run command boundary failed: %s %s\n",
                           error.c_str(), cleanup_error.c_str());
  return passed ? 0 : 1;
}
