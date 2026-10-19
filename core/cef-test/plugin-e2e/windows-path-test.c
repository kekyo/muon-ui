// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "muon_windows_path.h"

static int fail(const char *operation, DWORD error) {
  fprintf(stderr, "%s failed: Windows error %lu\n", operation, error);
  return 1;
}

static wchar_t *prepare_path(const wchar_t *input, int directory, int utf8) {
  muon_windows_path path = {0};
  DWORD error;
  if (utf8) {
    int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input, -1, NULL, 0, NULL, NULL);
    if (count == 0) return NULL;
    char *bytes = malloc((size_t)count);
    if (bytes == NULL) {
      SetLastError(ERROR_NOT_ENOUGH_MEMORY);
      return NULL;
    }
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input, -1, bytes, count, NULL, NULL) == 0) {
      error = GetLastError();
    } else {
      error = muon_windows_path_normalize_utf8(bytes, (size_t)count - 1, &path);
    }
    free(bytes);
  } else {
    error = muon_windows_path_normalize_wide(input, wcslen(input), &path);
  }
  if (error == ERROR_SUCCESS) error = muon_windows_path_for_io(&path,
      directory ? MUON_WINDOWS_PATH_TREE : MUON_WINDOWS_PATH_FILE);
  if (error != ERROR_SUCCESS) {
    muon_windows_path_destroy(&path);
    SetLastError(error);
    return NULL;
  }
  return path.value;
}

static int contracts(void) {
  const struct {
    const char *input;
    size_t length;
    DWORD expected;
  } invalid[] = {
    {"", 0, ERROR_INVALID_NAME},
    {"   ", 3, ERROR_INVALID_NAME},
    {"a\0b", 3, ERROR_INVALID_NAME},
    {"\xc0\xaf", 2, ERROR_NO_UNICODE_TRANSLATION},
    {"\xed\xa0\x80", 3, ERROR_NO_UNICODE_TRANSLATION},
    {"NUL.txt", 7, ERROR_INVALID_NAME},
    {"CON", 3, ERROR_INVALID_NAME},
    {"C:\\CONIN$", 9, ERROR_INVALID_NAME},
    {"C:\\COM1.txt", 11, ERROR_INVALID_NAME},
    {"\\\\.\\pipe\\test", 13, ERROR_INVALID_NAME},
    {"\\??\\C:\\test", 11, ERROR_INVALID_NAME},
    {"//?/C:/test", 11, ERROR_INVALID_NAME},
    {"\\\\server", 8, ERROR_INVALID_NAME},
  };
  for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
    muon_windows_path path = {0};
    DWORD error = muon_windows_path_normalize_utf8(invalid[i].input, invalid[i].length, &path);
    if (error != invalid[i].expected || path.value != NULL || path.length != 0 ||
        path.root_length != 0 || path.explicit_extended != 0) {
      fprintf(stderr, "invalid case %zu returned %lu\n", i, error);
      muon_windows_path_destroy(&path);
      return 1;
    }
  }
  const struct {
    const wchar_t *input;
    const wchar_t *expected;
    size_t root;
    int explicit_extended;
  } normalized[] = {
    {L"C:/missing/../new-file. ", L"\\\\?\\C:\\new-file", 7, 0},
    {L"\\\\server\\share\\folder\\..\\new", L"\\\\?\\UNC\\server\\share\\new", 21, 0},
    {L"\\\\server\\share", L"\\\\?\\UNC\\server\\share", 20, 0},
    {L"\\\\?\\C:\\name. ", L"\\\\?\\C:\\name. ", 7, 1},
    {L"\\\\?\\C:\\dir/../name", L"\\\\?\\C:\\dir/../name", 7, 1},
    {L"\\\\?\\UNC\\server\\share\\name", L"\\\\?\\UNC\\server\\share\\name", 21, 1},
    {L"\\\\?\\C:\\NUL.txt", L"\\\\?\\C:\\NUL.txt", 7, 1},
  };
  for (size_t i = 0; i < sizeof(normalized) / sizeof(normalized[0]); ++i) {
    muon_windows_path path = {0};
    DWORD error = muon_windows_path_normalize_wide(normalized[i].input,
        wcslen(normalized[i].input), &path);
    if (error == ERROR_SUCCESS) error = muon_windows_path_for_io(&path, MUON_WINDOWS_PATH_TREE);
    if (error == ERROR_SUCCESS) error = muon_windows_path_for_io(&path, MUON_WINDOWS_PATH_TREE);
    if (error != ERROR_SUCCESS || wcscmp(path.value, normalized[i].expected) ||
        path.length != wcslen(normalized[i].expected) || path.root_length != normalized[i].root ||
        path.explicit_extended != normalized[i].explicit_extended) {
      fprintf(stderr, "normalization case %zu returned %lu\n", i, error);
      muon_windows_path_destroy(&path);
      return 1;
    }
    SetLastError(ERROR_SHARING_VIOLATION);
    muon_windows_path_destroy(&path);
    if (GetLastError() != ERROR_SHARING_VIOLATION || path.value != NULL || path.length != 0) {
      return fail("destroy", ERROR_INVALID_DATA);
    }
  }
  const wchar_t embedded[] = {L'C', L':', L'\\', L'a', L'\0', L'b'};
  muon_windows_path path = {0};
  if (muon_windows_path_normalize_wide(embedded, 6, &path) != ERROR_INVALID_NAME) {
    muon_windows_path_destroy(&path);
    return fail("wide embedded NUL", ERROR_INVALID_DATA);
  }
  wchar_t *too_long = malloc(32768 * sizeof(wchar_t));
  if (too_long == NULL) return 1;
  wmemset(too_long, L'a', 32767);
  too_long[32767] = L'\0';
  DWORD error = muon_windows_path_normalize_wide(too_long, 32767, &path);
  free(too_long);
  muon_windows_path_destroy(&path);
  return error == ERROR_FILENAME_EXCED_RANGE ? 0 : fail("native length limit", error);
}

static int roundtrip(const wchar_t *path, const wchar_t *reference) {
  const char contents[] = "Muon Windows path roundtrip";
  HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, NULL);
  if (file == INVALID_HANDLE_VALUE) return fail("CreateFileW(write)", GetLastError());
  DWORD count;
  BOOL written = WriteFile(file, contents, sizeof(contents), &count, NULL);
  DWORD error = GetLastError();
  CloseHandle(file);
  if (!written) return fail("WriteFile", error);
  if (count != sizeof(contents)) return fail("WriteFile(count)", ERROR_WRITE_FAULT);

  // Verify the actual target through an independently supplied extended path,
  // so normalizing to the wrong filename cannot pass a self-consistent test.
  file = CreateFileW(reference, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                     FILE_ATTRIBUTE_NORMAL, NULL);
  if (file == INVALID_HANDLE_VALUE) return fail("CreateFileW(reference)", GetLastError());
  char actual[sizeof(contents) + 1] = {0};
  BOOL read = ReadFile(file, actual, sizeof(actual), &count, NULL);
  error = GetLastError();
  CloseHandle(file);
  if (!read) return fail("ReadFile(reference)", error);
  if (count != sizeof(contents) || memcmp(contents, actual, sizeof(contents))) {
    return fail("reference contents", ERROR_INVALID_DATA);
  }
  file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                     FILE_ATTRIBUTE_NORMAL, NULL);
  if (file == INVALID_HANDLE_VALUE) return fail("CreateFileW(read)", GetLastError());
  read = ReadFile(file, actual, sizeof(actual), &count, NULL);
  error = GetLastError();
  CloseHandle(file);
  if (!read) return fail("ReadFile", error);
  return count == sizeof(contents) && !memcmp(contents, actual, sizeof(contents))
      ? 0 : fail("roundtrip contents", ERROR_INVALID_DATA);
}

static int check_manifest(const wchar_t *executable) {
  ACTCTXW description = {0};
  description.cbSize = sizeof(description);
  description.dwFlags = ACTCTX_FLAG_RESOURCE_NAME_VALID;
  description.lpSource = executable;
  description.lpResourceName = MAKEINTRESOURCEW(1);
  HANDLE context = CreateActCtxW(&description);
  if (context == INVALID_HANDLE_VALUE) return fail("CreateActCtxW", GetLastError());
  wchar_t value[16] = {0};
  BOOL queried = QueryActCtxSettingsW(0, context,
      L"http://schemas.microsoft.com/SMI/2016/WindowsSettings", L"longPathAware",
      value, sizeof(value) / sizeof(value[0]), NULL);
  DWORD error = GetLastError();
  ReleaseActCtx(context);
  if (!queried) return fail("QueryActCtxSettingsW(longPathAware)", error);
  return !wcscmp(value, L"true") ? 0 : fail("longPathAware value", ERROR_INVALID_DATA);
}

int wmain(int argc, wchar_t **argv) {
  if (argc == 2 && !wcscmp(argv[1], L"contracts")) return contracts();
  if (argc == 3 && !wcscmp(argv[1], L"manifest")) return check_manifest(argv[2]);
  if (argc != 4) return 2;
  int directory = !wcscmp(argv[1], L"directory");
  wchar_t *path = prepare_path(argv[2], directory, !wcscmp(argv[1], L"utf8"));
  if (path == NULL) return fail("prepare_path", GetLastError());
  int result;
  if (directory) {
    if (!CreateDirectoryW(path, NULL)) {
      result = fail("CreateDirectoryW", GetLastError());
    } else {
      DWORD attributes = GetFileAttributesW(argv[3]);
      result = attributes != INVALID_FILE_ATTRIBUTES &&
          (attributes & FILE_ATTRIBUTE_DIRECTORY) ? 0 : fail("directory target", GetLastError());
    }
  } else {
    result = roundtrip(path, argv[3]);
  }
  free(path);
  return result;
}
