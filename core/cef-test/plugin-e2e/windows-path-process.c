// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

// This executable observes the actual child arguments, environment, cwd and
// streams. It does not link any Muon path or process implementation.
static void print_json_string(const wchar_t *value) {
  int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1,
                                NULL, 0, NULL, NULL);
  char *text = size == 0 ? NULL : malloc((size_t)size);
  if (text == NULL || !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
      value, -1, text, size, NULL, NULL)) exit(2);
  putchar('"');
  for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
    if (*p < 32) printf("\\u%04x", *p);
    else {
      if (*p == '\\' || *p == '"') putchar('\\');
      putchar(*p);
    }
  }
  putchar('"');
  free(text);
}

static wchar_t *environment(const wchar_t *name) {
  DWORD size = GetEnvironmentVariableW(name, NULL, 0);
  wchar_t *value = calloc(size == 0 ? 1 : size, sizeof(wchar_t));
  if (value == NULL) exit(2);
  if (size != 0 && GetEnvironmentVariableW(name, value, size) >= size) exit(2);
  return value;
}

static int wait_forever(void) {
  HANDLE ready = CreateEventW(NULL, TRUE, FALSE, NULL);
  if (ready == NULL) return 2;
  // The product's process-tree ownership must terminate this wait. No elapsed
  // delay decides whether a descendant is alive or ready for termination.
  WaitForSingleObject(ready, INFINITE);
  CloseHandle(ready);
  return 2;
}

static int start_tree(void) {
  DWORD capacity = 256;
  wchar_t *module = NULL;
  for (;;) {
    free(module);
    module = calloc(capacity, sizeof(wchar_t));
    if (module == NULL) return 2;
    DWORD length = GetModuleFileNameW(NULL, module, capacity);
    if (length == 0) { free(module); return 2; }
    if (length < capacity) break;
    capacity *= 2;
  }
  size_t size = wcslen(module) + 10;
  wchar_t *command = calloc(size, sizeof(wchar_t));
  if (command == NULL) { free(module); return 2; }
  swprintf(command, size, L"\"%ls\" wait", module);
  STARTUPINFOW startup = {0};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION child = {0};
  BOOL started = CreateProcessW(module, command, NULL, NULL, FALSE,
      CREATE_NO_WINDOW, NULL, NULL, &startup, &child);
  free(module);
  free(command);
  if (!started) return 2;
  printf("%lu\n", child.dwProcessId);
  fflush(stdout);
  CloseHandle(child.hThread);
  CloseHandle(child.hProcess);
  return wait_forever();
}

int wmain(int argc, wchar_t **argv) {
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
  _setmode(_fileno(stderr), _O_BINARY);
  if (argc > 1 && !wcscmp(argv[1], L"wait")) return wait_forever();
  if (argc > 1 && !wcscmp(argv[1], L"tree")) return start_tree();
  DWORD size = GetCurrentDirectoryW(0, NULL);
  wchar_t *cwd = calloc(size, sizeof(wchar_t));
  if (cwd == NULL || GetCurrentDirectoryW(size, cwd) >= size) return 2;
  printf("{\"args\":[");
  for (int index = 1; index < argc; ++index) {
    if (index > 1) putchar(',');
    print_json_string(argv[index]);
  }
  printf("],\"cwd\":");
  print_json_string(cwd);
  free(cwd);
  wchar_t *value = environment(L"MUON_PATH_VALUE");
  printf(",\"value\":");
  print_json_string(value);
  free(value);
  value = environment(L"MUON_LAUNCHER_APP_ID");
  printf(",\"appId\":");
  print_json_string(value);
  free(value);
  printf("}\n");
  fprintf(stderr, "process-stderr\n");
  if (argc > 1 && !wcscmp(argv[1], L"io")) {
    char buffer[256];
    size_t count;
    while ((count = fread(buffer, 1, sizeof(buffer), stdin)) != 0) {
      if (fwrite(buffer, 1, count, stdout) != count) return 2;
    }
  }
  value = environment(L"MUON_PATH_RESTART");
  int result = 37;
  if (*value) {
    HANDLE file = CreateFileW(value, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) { CloseHandle(file); result = 88; }
    else if (GetLastError() != ERROR_FILE_EXISTS) result = 2;
  }
  free(value);
  return result;
}
