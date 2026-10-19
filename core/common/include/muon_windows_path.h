/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#ifndef MUON_WINDOWS_PATH_H
#define MUON_WINDOWS_PATH_H

#include <stddef.h>
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Owned, absolute Windows filesystem path. Initialize all members to zero. */
typedef struct muon_windows_path {
  /** NUL-terminated UTF-16 buffer, owned until muon_windows_path_destroy. */
  wchar_t *value;
  /** Number of UTF-16 code units, excluding the terminator. */
  size_t length;
  /** Length of the drive or UNC share root, including its separator if present. */
  size_t root_length;
  /** Nonzero only when the caller supplied the strict extended prefix. */
  int explicit_extended;
} muon_windows_path;

/** Selects the native representation after ordinary path normalization. */
typedef enum muon_windows_path_usage {
  /** Prefix when required by length or an exact trailing period or space. */
  MUON_WINDOWS_PATH_FILE,
  /** Always prefix: directory creation, enumeration and recursive operations. */
  MUON_WINDOWS_PATH_TREE
} muon_windows_path_usage;

/**
 * Normalizes an ordinary UTF-8 filesystem path without requiring its existence.
 * @param input Input bytes. File URI decoding is the caller's responsibility.
 * @param length Byte count excluding the terminator; embedded NUL is rejected.
 * @param result Empty output, owned by the caller on success; empty on failure.
 * @return ERROR_SUCCESS or a Windows error code, captured before cleanup.
 * @remarks Invalid UTF-8, empty input and unsupported device namespaces fail.
 * Relative input uses a captured drive/current-directory base. Explicit extended
 * drive/UNC input is preserved. This is not a symbolic-link target converter.
 */
DWORD muon_windows_path_normalize_utf8(const char *input, size_t length,
                                     muon_windows_path *result);

/**
 * Normalizes a native UTF-16 path using the same rules as the UTF-8 entry point.
 * @param input Input UTF-16 code units.
 * @param length Code-unit count excluding NUL; embedded NUL is rejected.
 * @param result Empty output, owned on success; empty on failure.
 * @return ERROR_SUCCESS or a Windows error code.
 * @remarks No separator or dot-segment normalization is performed on an explicit
 * extended path. Other device namespaces are outside this filesystem contract.
 */
DWORD muon_windows_path_normalize_wide(const wchar_t *input, size_t length,
                                     muon_windows_path *result);

/**
 * Converts a normalized path to the representation used for native file I/O.
 * @param path Owned normalized path; unchanged on failure.
 * @param usage Operation's prefix requirements.
 * @return ERROR_SUCCESS or a Windows error code.
 * @remarks Does not normalize again, consult machine policy, or double-prefix.
 * An internally added prefix does not change explicit_extended.
 */
DWORD muon_windows_path_for_io(muon_windows_path *path,
                              muon_windows_path_usage usage);

/**
 * Releases an owned path and resets every member to zero.
 * @param path Owned path, including an empty result. NULL is allowed.
 * @remarks Preserves the calling thread's last Windows error.
 */
void muon_windows_path_destroy(muon_windows_path *path);

#ifdef __cplusplus
}
#endif
#endif
