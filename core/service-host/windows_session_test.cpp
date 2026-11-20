/* Test-only driver for a synthetic session end; never logs off the machine. */
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <cwchar>
#include <iostream>
#include <string>

int wmain(int argc, wchar_t** argv) {
  if (argc != 3) return 2;
  if (std::wstring(argv[1]) == L"end") {
    const auto window = FindWindowW(L"MuonServiceSessionHost", argv[2]);
    if (!window || IsWindowVisible(window)) return 3;
    if (!SendMessageW(window, WM_QUERYENDSESSION, 0, ENDSESSION_LOGOFF)) return 4;
    SendMessageW(window, WM_ENDSESSION, FALSE, ENDSESSION_LOGOFF);
    // Cancelled logout must leave the same hidden top-level window alive.
    if (!IsWindow(window)) return 5;
    SendMessageW(window, WM_ENDSESSION, TRUE, ENDSESSION_LOGOFF);
    return 0;
  }
  if (std::wstring(argv[1]) != L"security") return 2;
  const auto pipe = CreateFileW(argv[2], READ_CONTROL, 0, nullptr, OPEN_EXISTING,
                                SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
  if (pipe == INVALID_HANDLE_VALUE) return 6;
  auto descriptor = PSECURITY_DESCRIPTOR{};
  PACL acl = nullptr;
  const auto result = GetSecurityInfo(pipe, SE_KERNEL_OBJECT,
      DACL_SECURITY_INFORMATION, nullptr, nullptr, &acl, nullptr, &descriptor);
  CloseHandle(pipe);
  if (result != ERROR_SUCCESS || !acl || acl->AceCount != 1) return 6;
  void* raw = nullptr;
  if (!GetAce(acl, 0, &raw)) return 7;
  auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw);
  LPWSTR sid = nullptr;
  if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE ||
      !ConvertSidToStringSidW(&ace->SidStart, &sid)) return 8;
  const auto is_logon = std::wstring(sid).starts_with(L"S-1-5-5-");
  LocalFree(sid);
  LocalFree(descriptor);
  if (!is_logon) return 9;
  std::cout << "logon-only" << std::endl;
  return 0;
}
