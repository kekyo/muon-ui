// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui
#include "muon_notification_windows.h"
#include <windows.h>
#include <inspectable.h>
#include <roapi.h>
#include <shellapi.h>
#include <winstring.h>
#include <stdlib.h>
#include <wchar.h>

// Public UniversalApiContract v1 ABI prefixes, checked against Microsoft's SDK
// interface comments and declaration order. MinGW does not provide these types.
// https://github.com/microsoft/win32metadata/blob/main/generation/WinSDK/RecompiledIdlHeaders/winrt/windows.ui.notifications.h
typedef struct MuonToastHistory MuonToastHistory;
typedef struct MuonToastHistoryFactory MuonToastHistoryFactory;
typedef struct {
    IInspectableVtbl inspectable;
    HRESULT (STDMETHODCALLTYPE *RemoveGroup)(MuonToastHistory *, HSTRING);
    HRESULT (STDMETHODCALLTYPE *RemoveGroupWithId)(MuonToastHistory *, HSTRING, HSTRING);
    HRESULT (STDMETHODCALLTYPE *RemoveGroupedTagWithId)(MuonToastHistory *, HSTRING, HSTRING, HSTRING);
    HRESULT (STDMETHODCALLTYPE *RemoveGroupedTag)(MuonToastHistory *, HSTRING, HSTRING);
    HRESULT (STDMETHODCALLTYPE *Remove)(MuonToastHistory *, HSTRING);
    HRESULT (STDMETHODCALLTYPE *Clear)(MuonToastHistory *);
    HRESULT (STDMETHODCALLTYPE *ClearWithId)(MuonToastHistory *, HSTRING);
} MuonToastHistoryVtable;
struct MuonToastHistory { const MuonToastHistoryVtable *lpVtbl; };
typedef struct {
    IInspectableVtbl inspectable;
    HRESULT (STDMETHODCALLTYPE *get_History)(MuonToastHistoryFactory *, MuonToastHistory **);
} MuonToastHistoryFactoryVtable;
struct MuonToastHistoryFactory { const MuonToastHistoryFactoryVtable *lpVtbl; };

static wchar_t *notification_wide(const char *value) {
    if (!value || !*value) return NULL;
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, NULL, 0);
    if (count <= 0 || count > 32768) return NULL;
    wchar_t *result = malloc((size_t)count * sizeof(wchar_t));
    if (!result) return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, result, count) != count) {
        free(result);
        return NULL;
    }
    return result;
}

static wchar_t *notification_key_path(const wchar_t *prefix, const wchar_t *id,
                                      const wchar_t *suffix) {
    size_t size = wcslen(prefix) + wcslen(id) + wcslen(suffix) + 1;
    wchar_t *result = malloc(size * sizeof(wchar_t));
    if (result) _snwprintf(result, size, L"%ls%ls%ls", prefix, id, suffix);
    return result;
}

static wchar_t *notification_registry_string(HKEY key, const wchar_t *name) {
    DWORD bytes = 0;
    if (RegGetValueW(key, NULL, name, RRF_RT_REG_SZ, NULL, NULL, &bytes) != ERROR_SUCCESS ||
        bytes < sizeof(wchar_t) || bytes > 65536 || bytes % sizeof(wchar_t)) return NULL;
    wchar_t *value = calloc(bytes / sizeof(wchar_t) + 1, sizeof(wchar_t));
    if (!value) return NULL;
    if (RegGetValueW(key, NULL, name, RRF_RT_REG_SZ, NULL, value, &bytes) != ERROR_SUCCESS) {
        free(value);
        return NULL;
    }
    return value;
}

static wchar_t *notification_normalize_path(const wchar_t *value) {
    if (!value) return NULL;
    size_t length = wcslen(value);
    wchar_t *slashes = malloc((length + 1) * sizeof(wchar_t));
    if (!slashes) return NULL;
    for (size_t i = 0; i <= length; i++) slashes[i] = value[i] == L'/' ? L'\\' : value[i];
    if (length < 3 || !((slashes[1] == L':' && slashes[2] == L'\\') ||
                       (slashes[0] == L'\\' && slashes[1] == L'\\'))) {
        free(slashes);
        return NULL;
    }
    DWORD size = GetFullPathNameW(slashes, 0, NULL, NULL);
    if (!size || size > 32768) {
        free(slashes);
        return NULL;
    }
    wchar_t *result = malloc((size_t)size * sizeof(wchar_t));
    DWORD written = result ? GetFullPathNameW(slashes, size, result, NULL) : 0;
    free(slashes);
    if (!written || written >= size) {
        free(result);
        return NULL;
    }
    return result;
}

static int notification_same_path(const wchar_t *expected, const wchar_t *actual) {
    wchar_t *normalized = notification_normalize_path(actual);
    int matches = normalized && CompareStringOrdinal(expected, -1, normalized, -1, TRUE) == CSTR_EQUAL;
    free(normalized);
    return matches;
}

static int notification_owns_registration(const wchar_t *app, const wchar_t *launcher,
                                           const wchar_t *clsid) {
    HKEY key = NULL;
    wchar_t *path = NULL, *owner = NULL, *server = NULL, *command = NULL, *activator = NULL;
    wchar_t **arguments = NULL;
    int count = 0, matches = 0;
    GUID expected_clsid, actual_clsid;
    if (wcschr(app, L'\\') || FAILED(CLSIDFromString(clsid, &expected_clsid))) return 0;
    path = notification_key_path(L"Software\\Classes\\CLSID\\", clsid, L"\\LocalServer32");
    if (!path || RegOpenKeyExW(HKEY_CURRENT_USER, path, 0,
                              KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) goto done;
    owner = notification_registry_string(key, L"MuonAppId");
    server = notification_registry_string(key, L"ServerExecutable");
    command = notification_registry_string(key, NULL);
    RegCloseKey(key);
    key = NULL;
    if (!owner || wcscmp(owner, app) || !notification_same_path(launcher, server) || !command) goto done;
    arguments = CommandLineToArgvW(command, &count);
    if (!arguments || count != 2 || !notification_same_path(launcher, arguments[0]) ||
        wcscmp(arguments[1], L"--muon-notification-activation")) goto done;
    free(path);
    path = notification_key_path(L"Software\\Classes\\AppUserModelId\\", app, L"");
    if (!path || RegOpenKeyExW(HKEY_CURRENT_USER, path, 0,
                              KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) goto done;
    activator = notification_registry_string(key, L"CustomActivator");
    matches = activator && SUCCEEDED(CLSIDFromString(activator, &actual_clsid)) &&
              IsEqualGUID(&expected_clsid, &actual_clsid);
done:
    if (key) RegCloseKey(key);
    if (arguments) LocalFree(arguments);
    free(path);
    free(owner);
    free(server);
    free(command);
    free(activator);
    return matches;
}

int muon_notification_clear_history(const char *app_id,
                                    const char *launcher_path,
                                    const char *activator_clsid) {
    wchar_t *app = notification_wide(app_id);
    wchar_t *launcher = notification_wide(launcher_path);
    wchar_t *clsid = notification_wide(activator_clsid);
    wchar_t *normalized = notification_normalize_path(launcher);
    int result = 1, initialized = 0;
    HSTRING class_name = NULL, app_name = NULL;
    MuonToastHistoryFactory *factory = NULL;
    MuonToastHistory *history = NULL;
    const wchar_t *manager_class = L"Windows.UI.Notifications.ToastNotificationManager";
    const IID factory_iid = {0x7ab93c52, 0x0e48, 0x4750, {0xba, 0x9d, 0x1a, 0x41, 0x13, 0x98, 0x18, 0x47}};
    // Ownership must be established before any OS notification mutation.
    if (!app || !clsid || !normalized || !notification_owns_registration(app, normalized, clsid)) goto done;
    if (FAILED(RoInitialize(RO_INIT_MULTITHREADED))) goto done;
    initialized = 1;
    if (FAILED(WindowsCreateString(manager_class, (UINT32)wcslen(manager_class), &class_name)) ||
        FAILED(WindowsCreateString(app, (UINT32)wcslen(app), &app_name)) ||
        FAILED(RoGetActivationFactory(class_name, &factory_iid, (void **)&factory)) ||
        FAILED(factory->lpVtbl->get_History(factory, &history)) ||
        FAILED(history->lpVtbl->ClearWithId(history, app_name))) goto done;
    result = 0;
done:
    if (history) history->lpVtbl->inspectable.Release((IInspectable *)history);
    if (factory) factory->lpVtbl->inspectable.Release((IInspectable *)factory);
    if (app_name) WindowsDeleteString(app_name);
    if (class_name) WindowsDeleteString(class_name);
    if (initialized) RoUninitialize();
    free(app);
    free(launcher);
    free(clsid);
    free(normalized);
    return result;
}
