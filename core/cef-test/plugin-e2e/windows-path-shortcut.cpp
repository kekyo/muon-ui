// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

#include <windows.h>
#include <shobjidl.h>
#include <cstdio>
#include <cwchar>

int wmain(int argc, wchar_t** argv) {
  if (argc != 3 || std::wcslen(argv[2]) >= MAX_PATH) return 2;
  auto result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(result)) return 3;
  IShellLinkW* link = nullptr;
  IPersistFile* file = nullptr;
  result = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                            IID_IShellLinkW, reinterpret_cast<void**>(&link));
  if (SUCCEEDED(result)) result = link->QueryInterface(
      IID_IPersistFile, reinterpret_cast<void**>(&file));
  if (SUCCEEDED(result)) result = file->Load(argv[1], STGM_READ);
  // IShellLinkW documents MAX_PATH as its output limit. This test checks the
  // supported short installation independently of WScript's ANSI TargetPath.
  wchar_t actual[MAX_PATH] = {};
  if (SUCCEEDED(result)) result = link->GetPath(actual, MAX_PATH, nullptr, 0);
  const auto passed = result == S_OK && std::wcscmp(actual, argv[2]) == 0;
  if (file != nullptr) file->Release();
  if (link != nullptr) link->Release();
  CoUninitialize();
  if (!passed) std::fprintf(stderr, "Unexpected shortcut target (HRESULT %lx)\n",
                            static_cast<unsigned long>(result));
  return passed ? 0 : 1;
}
