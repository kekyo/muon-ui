// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

#include <windows.h>
#include <winioctl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

// Keep fixture preparation independent of Muon and the machine's long-path
// policy. Only the driver uses extended paths; API tests use ordinary paths.
static int report_error(const char *operation, DWORD error) {
  fprintf(stderr, "%s failed: Windows error %lu\n", operation, error);
  return 1;
}

static wchar_t *join_path(const wchar_t *parent, const wchar_t *name) {
  size_t length = wcslen(parent) + wcslen(name) + 2;
  wchar_t *path = malloc(length * sizeof(wchar_t));
  if (path != NULL) swprintf(path, length, L"%ls\\%ls", parent, name);
  return path;
}

static int create_directory(wchar_t *path) {
  // The caller supplies a local extended absolute path, including its drive.
  for (size_t index = 7; ; ++index) {
    wchar_t separator = path[index];
    if (separator != L'\\' && separator != L'\0') continue;
    path[index] = L'\0';
    BOOL created = CreateDirectoryW(path, NULL);
    DWORD error = created ? ERROR_SUCCESS : GetLastError();
    path[index] = separator;
    if (!created && error != ERROR_ALREADY_EXISTS) {
      return report_error("CreateDirectoryW", error);
    }
    if (separator == L'\0') return 0;
  }
}

static int remove_tree(const wchar_t *path) {
  DWORD attributes = GetFileAttributesW(path);
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
        ? 0 : report_error("GetFileAttributesW", error);
  }
  if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
    return DeleteFileW(path) ? 0 : report_error("DeleteFileW", GetLastError());
  }
  if (!(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
    wchar_t *pattern = join_path(path, L"*");
    if (pattern == NULL) return 1;
    WIN32_FIND_DATAW entry;
    HANDLE search = FindFirstFileW(pattern, &entry);
    DWORD error = search == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    free(pattern);
    if (search == INVALID_HANDLE_VALUE) return report_error("FindFirstFileW", error);
    int result = 0;
    do {
      if (!wcscmp(entry.cFileName, L".") || !wcscmp(entry.cFileName, L"..")) continue;
      wchar_t *child = join_path(path, entry.cFileName);
      result = child == NULL ? 1 : remove_tree(child);
      free(child);
      if (result != 0) break;
    } while (FindNextFileW(search, &entry));
    error = GetLastError();
    FindClose(search);
    if (result != 0) return result;
    if (error != ERROR_NO_MORE_FILES) return report_error("FindNextFileW", error);
  }
  return RemoveDirectoryW(path) ? 0 : report_error("RemoveDirectoryW", GetLastError());
}

static int write_file(const wchar_t *path, const wchar_t *content) {
  int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, content, -1,
                                  NULL, 0, NULL, NULL);
  if (length == 0) return report_error("encode content", GetLastError());
  char *bytes = malloc((size_t)length);
  if (bytes == NULL) return 1;
  if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, content, -1,
                          bytes, length, NULL, NULL)) {
    free(bytes);
    return report_error("encode content", GetLastError());
  }
  HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL, NULL);
  DWORD written = 0;
  BOOL success = file != INVALID_HANDLE_VALUE &&
      WriteFile(file, bytes, (DWORD)length - 1, &written, NULL);
  DWORD error = success ? ERROR_SUCCESS : GetLastError();
  if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
  free(bytes);
  return success && written == (DWORD)length - 1 ? 0 : report_error("write fixture", error);
}

typedef struct fixture_mount_point {
  ULONG tag;
  USHORT data_length;
  USHORT reserved;
  USHORT substitute_offset;
  USHORT substitute_length;
  USHORT print_offset;
  USHORT print_length;
  WCHAR names[1];
} fixture_mount_point;

static int create_junction(const wchar_t *path, const wchar_t *target) {
  if (wcsncmp(target, L"\\\\?\\", 4) || wcslen(target) < 8) return 2;
  size_t substitute_units = wcslen(target);
  size_t print_units = substitute_units - 4;
  size_t size = offsetof(fixture_mount_point, names) +
      (substitute_units + print_units + 2) * sizeof(wchar_t);
  if (size > MAXIMUM_REPARSE_DATA_BUFFER_SIZE) return 2;
  fixture_mount_point *data = calloc(1, size);
  if (data == NULL) return 1;
  data->tag = IO_REPARSE_TAG_MOUNT_POINT;
  data->data_length = (USHORT)(size - 8);
  data->substitute_length = (USHORT)(substitute_units * sizeof(wchar_t));
  data->print_offset = (USHORT)((substitute_units + 1) * sizeof(wchar_t));
  data->print_length = (USHORT)(print_units * sizeof(wchar_t));
  swprintf(data->names, substitute_units + 1, L"\\??\\%ls", target + 4);
  wcscpy(data->names + substitute_units + 1, target + 4);
  if (!CreateDirectoryW(path, NULL)) {
    DWORD error = GetLastError();
    free(data);
    return report_error("create junction directory", error);
  }
  HANDLE link = CreateFileW(path, GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
  DWORD returned = 0;
  BOOL created = link != INVALID_HANDLE_VALUE && DeviceIoControl(link,
      FSCTL_SET_REPARSE_POINT, data, (DWORD)size, NULL, 0, &returned, NULL);
  DWORD error = created ? ERROR_SUCCESS : GetLastError();
  if (link != INVALID_HANDLE_VALUE) CloseHandle(link);
  free(data);
  if (!created) return report_error("create junction", error);

  // Verify the actual object independently. cmd.exe's mklink can normalize a
  // trailing dot even when its target argument uses extended syntax.
  link = CreateFileW(path, 0,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS, NULL);
  HANDLE destination = CreateFileW(target, 0,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS, NULL);
  BY_HANDLE_FILE_INFORMATION actual, expected;
  BOOL inspected = link != INVALID_HANDLE_VALUE && destination != INVALID_HANDLE_VALUE &&
      GetFileInformationByHandle(link, &actual) &&
      GetFileInformationByHandle(destination, &expected);
  error = inspected ? ERROR_SUCCESS : GetLastError();
  if (link != INVALID_HANDLE_VALUE) CloseHandle(link);
  if (destination != INVALID_HANDLE_VALUE) CloseHandle(destination);
  if (!inspected) return report_error("inspect junction target", error);
  return actual.dwVolumeSerialNumber == expected.dwVolumeSerialNumber &&
      actual.nFileIndexHigh == expected.nFileIndexHigh &&
      actual.nFileIndexLow == expected.nFileIndexLow
      ? 0 : report_error("junction target identity", ERROR_INVALID_DATA);
}

static int launch(const wchar_t *path, const wchar_t *arguments) {
  size_t length = wcslen(path) + wcslen(arguments) + 3;
  wchar_t *command = malloc(length * sizeof(wchar_t));
  if (command == NULL) return 1;
  swprintf(command, length, L"\"%ls\"%ls", path, arguments);
  SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
  HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &security, OPEN_EXISTING, 0, NULL);
  STARTUPINFOW startup = {0};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = input;
  startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
  startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  if (input == INVALID_HANDLE_VALUE ||
      !SetHandleInformation(startup.hStdOutput, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT) ||
      !SetHandleInformation(startup.hStdError, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
    DWORD error = GetLastError();
    if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
    free(command);
    return report_error("fixture standard handles", error);
  }
  PROCESS_INFORMATION process = {0};
  BOOL started = CreateProcessW(path, command, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                                NULL, NULL, &startup, &process);
  DWORD error = started ? ERROR_SUCCESS : GetLastError();
  CloseHandle(input);
  free(command);
  if (!started) return report_error("CreateProcessW", error);
  DWORD wait = WaitForSingleObject(process.hProcess, 20000);
  DWORD code = 1;
  if (wait == WAIT_OBJECT_0) {
    if (!GetExitCodeProcess(process.hProcess, &code)) error = GetLastError();
  } else {
    error = wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
    TerminateProcess(process.hProcess, 1);
    WaitForSingleObject(process.hProcess, 5000);
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return error == ERROR_SUCCESS ? (int)code : report_error("fixture child", error);
}

int wmain(int argc, wchar_t **argv) {
  if ((argc != 3 && argc != 4) || wcslen(argv[2]) < 8 ||
      wcsncmp(argv[2], L"\\\\?\\", 4) || argv[2][5] != L':' || argv[2][6] != L'\\') {
    fprintf(stderr, "Usage: fixture.exe mkdir|remove \\\\?\\C:\\test-path\n");
    return 2;
  }
  if (!wcscmp(argv[1], L"mkdir")) return create_directory(argv[2]);
  if (!wcscmp(argv[1], L"remove")) return remove_tree(argv[2]);
  if (!wcscmp(argv[1], L"absent")) {
    DWORD attributes = GetFileAttributesW(argv[2]);
    DWORD error = GetLastError();
    return attributes == INVALID_FILE_ATTRIBUTES &&
        (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
        ? 0 : report_error("expected absent fixture path", error);
  }
  if (!wcscmp(argv[1], L"write") && argc == 4) return write_file(argv[2], argv[3]);
  if (!wcscmp(argv[1], L"junction") && argc == 4) return create_junction(argv[2], argv[3]);
  if (!wcscmp(argv[1], L"copy") && argc == 4) {
    return CopyFileExW(argv[3], argv[2], NULL, NULL, NULL, COPY_FILE_FAIL_IF_EXISTS)
        ? 0 : report_error("CopyFileExW", GetLastError());
  }
  if (!wcscmp(argv[1], L"launch")) return launch(argv[2], L"");
  if (!wcscmp(argv[1], L"launch-help")) return launch(argv[2], L" --help");
  if (!wcscmp(argv[1], L"launch-command") && argc == 4) return launch(argv[2], argv[3]);
  return 2;
}
