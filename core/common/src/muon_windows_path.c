/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "muon_windows_path.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

// The native limit includes the NUL and namespace prefix. Individual components
// and filesystem-specific limits remain the responsibility of the I/O API.
#define MUON_WINDOWS_PATH_MAX_UNITS 32766u

static DWORD allocate_path(size_t length, wchar_t **result) {
  if (length > MUON_WINDOWS_PATH_MAX_UNITS ||
      length > SIZE_MAX / sizeof(wchar_t) - 1) {
    return ERROR_FILENAME_EXCED_RANGE;
  }
  *result = malloc((length + 1) * sizeof(wchar_t));
  return *result == NULL ? ERROR_NOT_ENOUGH_MEMORY : ERROR_SUCCESS;
}

static int is_drive_letter(wchar_t value) {
  return (value >= L'A' && value <= L'Z') || (value >= L'a' && value <= L'z');
}

static int is_extended(const wchar_t *path, size_t length) {
  return length >= 4 && !wmemcmp(path, L"\\\\?\\", 4);
}

static size_t root_length(const wchar_t *path, size_t length) {
  size_t offset = is_extended(path, length) ? 4 : 0;
  if (length >= offset + 3 && is_drive_letter(path[offset]) &&
      path[offset + 1] == L':' && path[offset + 2] == L'\\') return offset + 3;
  if (offset == 4) {
    if (length < 8 || _wcsnicmp(path + 4, L"UNC\\", 4)) return 0;
    offset = 8;
  } else {
    if (length < 2 || path[0] != L'\\' || path[1] != L'\\') return 0;
    offset = 2;
  }
  size_t server = offset;
  while (offset < length && path[offset] != L'\\') ++offset;
  if (offset == server || offset == length) return 0;
  size_t share = ++offset;
  while (offset < length && path[offset] != L'\\') ++offset;
  if (offset == share) return 0;
  return offset < length ? offset + 1 : offset;
}

static int reserved_component(const wchar_t *name, size_t length) {
  size_t end = 0;
  while (end < length && name[end] != L'.' && name[end] != L':') ++end;
  while (end > 0 && name[end - 1] == L' ') --end;
  if (end == 3 && (!_wcsnicmp(name, L"CON", 3) ||
      !_wcsnicmp(name, L"PRN", 3) || !_wcsnicmp(name, L"AUX", 3) ||
      !_wcsnicmp(name, L"NUL", 3))) return 1;
  if (end == 4 && (!_wcsnicmp(name, L"COM", 3) || !_wcsnicmp(name, L"LPT", 3))) {
    wchar_t digit = name[3];
    return (digit >= L'1' && digit <= L'9') || digit == 0xb9 ||
        digit == 0xb2 || digit == 0xb3;
  }
  return (end == 6 && !_wcsnicmp(name, L"CONIN$", 6)) ||
      (end == 7 && !_wcsnicmp(name, L"CONOUT$", 7));
}

static int has_reserved_component(const wchar_t *path, size_t length, size_t root) {
  for (size_t start = root; start < length;) {
    size_t end = start;
    while (end < length && path[end] != L'\\') ++end;
    if (reserved_component(path + start, end - start)) return 1;
    start = end + 1;
  }
  return 0;
}

static DWORD full_path(const wchar_t *input, wchar_t **result, size_t *length) {
  DWORD capacity = MAX_PATH;
  for (;;) {
    wchar_t *buffer = NULL;
    DWORD error = allocate_path((size_t)capacity - 1, &buffer);
    if (error != ERROR_SUCCESS) return error;
    DWORD count = GetFullPathNameW(input, capacity, buffer, NULL);
    if (count == 0) {
      error = GetLastError();
      free(buffer);
      return error;
    }
    if (count < capacity) {
      *result = buffer;
      *length = count;
      return ERROR_SUCCESS;
    }
    free(buffer);
    // A small-buffer result includes NUL, unlike a successful result.
    if (count > MUON_WINDOWS_PATH_MAX_UNITS + 1) return ERROR_FILENAME_EXCED_RANGE;
    capacity = count;
  }
}

static DWORD resolve_base(wchar_t **input, size_t *length) {
  if (root_length(*input, *length) != 0) return ERROR_SUCCESS;
  wchar_t base_input[] = L".";
  wchar_t drive_input[] = L"C:.";
  const wchar_t *base = base_input;
  size_t offset = 0;
  if (*length >= 2 && is_drive_letter((*input)[0]) && (*input)[1] == L':') {
    drive_input[0] = (*input)[0];
    base = drive_input;
    offset = 2;
  } else if ((*input)[0] == L'\\') {
    if (*length >= 2 && (*input)[1] == L'\\') return ERROR_INVALID_NAME;
    base = L"\\";
    offset = 1;
  }
  wchar_t *captured = NULL;
  size_t captured_length = 0;
  DWORD error = full_path(base, &captured, &captured_length);
  if (error != ERROR_SUCCESS) return error;
  size_t combined_length = captured_length + 1 + *length - offset;
  wchar_t *combined = NULL;
  error = allocate_path(combined_length, &combined);
  if (error == ERROR_SUCCESS) {
    wmemcpy(combined, captured, captured_length);
    combined[captured_length] = L'\\';
    wmemcpy(combined + captured_length + 1, *input + offset, *length - offset + 1);
    free(*input);
    *input = combined;
    *length = combined_length;
  }
  free(captured);
  return error;
}

DWORD muon_windows_path_normalize_wide(const wchar_t *input, size_t length,
                                     muon_windows_path *result) {
  if (result == NULL) return ERROR_INVALID_PARAMETER;
  memset(result, 0, sizeof(*result));
  if (input == NULL || length == 0) return ERROR_INVALID_NAME;
  if (length > MUON_WINDOWS_PATH_MAX_UNITS) return ERROR_FILENAME_EXCED_RANGE;
  if (wmemchr(input, L'\0', length) != NULL) return ERROR_INVALID_NAME;
  size_t non_space = 0;
  while (non_space < length && input[non_space] == L' ') ++non_space;
  if (non_space == length) return ERROR_INVALID_NAME;
  wchar_t *copy = NULL;
  DWORD error = allocate_path(length, &copy);
  if (error != ERROR_SUCCESS) return error;
  wmemcpy(copy, input, length);
  copy[length] = L'\0';
  int extended = is_extended(input, length);
  if (!extended) {
    for (size_t i = 0; i < length; ++i) if (copy[i] == L'/') copy[i] = L'\\';
    // Reject device syntax rather than accidentally turning it into an ordinary
    // literal file. Only the strict extended drive/UNC syntax bypasses parsing.
    if ((length >= 4 && (!wmemcmp(copy, L"\\\\.\\", 4) ||
        !wmemcmp(copy, L"\\\\?\\", 4) || !wmemcmp(copy, L"\\??\\", 4)))) {
      free(copy);
      return ERROR_INVALID_NAME;
    }
    error = resolve_base(&copy, &length);
    if (error == ERROR_SUCCESS) {
      wchar_t *normalized = NULL;
      error = full_path(copy, &normalized, &length);
      free(copy);
      copy = normalized;
    }
    if (error != ERROR_SUCCESS) {
      free(copy);
      return error;
    }
  }
  size_t root = root_length(copy, length);
  if (root == 0 || (!extended && has_reserved_component(copy, length, root))) {
    free(copy);
    return ERROR_INVALID_NAME;
  }
  result->value = copy;
  result->length = length;
  result->root_length = root;
  result->explicit_extended = extended;
  return ERROR_SUCCESS;
}

DWORD muon_windows_path_normalize_utf8(const char *input, size_t length,
                                     muon_windows_path *result) {
  if (result == NULL) return ERROR_INVALID_PARAMETER;
  memset(result, 0, sizeof(*result));
  if (input == NULL || length == 0) return ERROR_INVALID_NAME;
  if (length > INT_MAX) return ERROR_FILENAME_EXCED_RANGE;
  if (memchr(input, '\0', length) != NULL) return ERROR_INVALID_NAME;
  int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input, (int)length, NULL, 0);
  if (count == 0) return GetLastError();
  wchar_t *wide = NULL;
  DWORD error = allocate_path((size_t)count, &wide);
  if (error != ERROR_SUCCESS) return error;
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input, (int)length, wide, count) != count) {
    error = GetLastError();
  } else {
    error = muon_windows_path_normalize_wide(wide, (size_t)count, result);
  }
  free(wide);
  return error;
}

DWORD muon_windows_path_for_io(muon_windows_path *path, muon_windows_path_usage usage) {
  if (path == NULL || path->value == NULL || path->length == 0 ||
      (usage != MUON_WINDOWS_PATH_FILE && usage != MUON_WINDOWS_PATH_TREE)) {
    return ERROR_INVALID_PARAMETER;
  }
  if (is_extended(path->value, path->length)) return ERROR_SUCCESS;
  wchar_t last = path->value[path->length - 1];
  if (usage == MUON_WINDOWS_PATH_FILE && path->length < MAX_PATH &&
      last != L' ' && last != L'.') return ERROR_SUCCESS;
  int unc = path->value[0] == L'\\';
  size_t extra = unc ? 6 : 4;
  wchar_t *extended = NULL;
  DWORD error = allocate_path(path->length + extra, &extended);
  if (error != ERROR_SUCCESS) return error;
  if (unc) {
    wmemcpy(extended, L"\\\\?\\UNC\\", 8);
    wmemcpy(extended + 8, path->value + 2, path->length - 1);
  } else {
    wmemcpy(extended, L"\\\\?\\", 4);
    wmemcpy(extended + 4, path->value, path->length + 1);
  }
  free(path->value);
  path->value = extended;
  path->length += extra;
  path->root_length += extra;
  return ERROR_SUCCESS;
}

void muon_windows_path_destroy(muon_windows_path *path) {
  if (path != NULL) {
    DWORD error = GetLastError();
    free(path->value);
    memset(path, 0, sizeof(*path));
    SetLastError(error);
  }
}
