// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui
#include "muon_notification_windows.h"
#include <windows.h>
#include <stdio.h>
#include <wchar.h>

#if defined(_WIN64)
static const wchar_t *test_app = L"dev.muon.NotificationProbe.Maintenance.amd64";
static const char *test_app_utf8 = "dev.muon.NotificationProbe.Maintenance.amd64";
static const wchar_t *test_clsid = L"{A6408C97-B622-4428-AE4F-510174097864}";
static const char *test_clsid_utf8 = "{A6408C97-B622-4428-AE4F-510174097864}";
#else
static const wchar_t *test_app = L"dev.muon.NotificationProbe.Maintenance.i686";
static const char *test_app_utf8 = "dev.muon.NotificationProbe.Maintenance.i686";
static const wchar_t *test_clsid = L"{A6408C97-B622-4428-AE4F-510174097832}";
static const char *test_clsid_utf8 = "{A6408C97-B622-4428-AE4F-510174097832}";
#endif

static int write_value(const wchar_t *path, const wchar_t *name, const wchar_t *value) {
    HKEY key = NULL;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, NULL, 0,
                       KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &key, NULL) != ERROR_SUCCESS) return 0;
    LONG result = RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *)value,
                                (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

int main(void) {
    wchar_t executable[32768];
    char executable_utf8[131072];
    wchar_t app_path[256], clsid_path[256], server_path[280], command[32768];
    if (!GetModuleFileNameW(NULL, executable, 32768) ||
        !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, executable, -1,
                            executable_utf8, sizeof(executable_utf8), NULL, NULL)) return 2;
    _snwprintf(app_path, 256, L"Software\\Classes\\AppUserModelId\\%ls", test_app);
    _snwprintf(clsid_path, 256, L"Software\\Classes\\CLSID\\%ls", test_clsid);
    _snwprintf(server_path, 280, L"%ls\\LocalServer32", clsid_path);
    _snwprintf(command, 32768, L"\"%ls\" --muon-notification-activation", executable);
    int passed = 0;
    RegDeleteKeyExW(HKEY_CURRENT_USER, server_path, KEY_WOW64_64KEY, 0);
    RegDeleteKeyExW(HKEY_CURRENT_USER, app_path, KEY_WOW64_64KEY, 0);
    if (muon_notification_clear_history(test_app_utf8, executable_utf8, test_clsid_utf8) == 0) {
        fputs("Missing registration was accepted\n", stderr);
        goto done;
    }
    if (!write_value(app_path, L"CustomActivator", test_clsid) ||
        !write_value(server_path, NULL, command) ||
        !write_value(server_path, L"ServerExecutable", executable) ||
        !write_value(server_path, L"MuonAppId", L"dev.muon.NotificationProbe.Other")) goto done;
    if (muon_notification_clear_history(test_app_utf8, executable_utf8, test_clsid_utf8) == 0) {
        fputs("Foreign application ownership was accepted\n", stderr);
        goto done;
    }
    if (!write_value(server_path, L"MuonAppId", test_app) ||
        !write_value(server_path, L"ServerExecutable", L"C:\\other.exe")) goto done;
    if (muon_notification_clear_history(test_app_utf8, executable_utf8, test_clsid_utf8) == 0) {
        fputs("Foreign launcher ownership was accepted\n", stderr);
        goto done;
    }
    if (!write_value(server_path, L"ServerExecutable", executable) ||
        !write_value(server_path, NULL, L"\"C:\\other.exe\" --muon-notification-activation")) goto done;
    if (muon_notification_clear_history(test_app_utf8, executable_utf8, test_clsid_utf8) == 0) {
        fputs("Foreign command ownership was accepted\n", stderr);
        goto done;
    }
    if (!write_value(server_path, NULL, command) ||
        !write_value(app_path, L"CustomActivator", L"{00000000-0000-0000-0000-000000000000}")) goto done;
    if (muon_notification_clear_history(test_app_utf8, executable_utf8, test_clsid_utf8) == 0) {
        fputs("Foreign activation association was accepted\n", stderr);
        goto done;
    }
    if (!write_value(app_path, L"CustomActivator", test_clsid)) goto done;
    if (muon_notification_clear_history(test_app_utf8, executable_utf8, test_clsid_utf8) != 0) {
        fputs("Owned history could not be cleared\n", stderr);
        goto done;
    }
    HKEY key = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, server_path, 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
        fputs("Clearing history removed its registration\n", stderr);
        goto done;
    }
    RegCloseKey(key);
    passed = 1;
    puts("Owned history cleared; absent and mismatched ownership rejected; registration preserved");
done:
    RegDeleteKeyExW(HKEY_CURRENT_USER, server_path, KEY_WOW64_64KEY, 0);
    RegDeleteKeyExW(HKEY_CURRENT_USER, clsid_path, KEY_WOW64_64KEY, 0);
    RegDeleteKeyExW(HKEY_CURRENT_USER, app_path, KEY_WOW64_64KEY, 0);
    return passed ? 0 : 1;
}
