// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

#include <windows.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "common.h"
#include "prepare_cef.h"
#include "prepare_node.h"
#include "muon_prepare_resource.h"

static int fail_resource_write;
size_t __real_fwrite(const void *buffer, size_t size, size_t count, FILE *file);

// Inject a disk-write failure after real bytes reach the output. The observable
// contract is a failed resource update with no partial PE left on disk.
size_t __wrap_fwrite(const void *buffer, size_t size, size_t count, FILE *file) {
  if (!fail_resource_write || size != 1 || count <= 16) {
    return __real_fwrite(buffer, size, count, file);
  }
  size_t written = __real_fwrite(buffer, 1, 16, file);
  fflush(file);
  fputs("Injected partial resource write failure\n", stderr);
  errno = ENOSPC;
  return written;
}

static int resource_failure(int argc, char **argv) {
  fail_resource_write = 1;
  return muon_prepare_resource_main(argc, argv, 2);
}

static char *utf8(const wchar_t *value) {
  int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1,
                                  NULL, 0, NULL, NULL);
  if (length == 0) return NULL;
  char *result = malloc((size_t)length);
  if (result == NULL) return NULL;
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1,
                         result, length, NULL, NULL) == 0) {
    free(result);
    return NULL;
  }
  return result;
}

static int fail(const char *action) {
  fprintf(stderr, "%s failed (errno=%d, Windows error=%lu)\n", action, errno, GetLastError());
  return 1;
}

static int expect_contents(const char *path, const char *expected) {
  char *actual = muon_read_text_file(path);
  int result = actual != NULL && strcmp(actual, expected) == 0 ? 0 : fail("file contents");
  free(actual);
  return result;
}

static int files(const char *root, const wchar_t *reference_root) {
  char *source = muon_path_join(root, "source.txt");
  char *destination = muon_path_join(root, "copy.txt");
  char *replacement = muon_path_join(root, "replacement.txt");
  int result = 1;
  if (source == NULL || destination == NULL || replacement == NULL) goto done;
  if (muon_write_text_file(source, "first") != 0 ||
      muon_append_text_file(source, " second") != 0) {
    fail("write/append");
    goto done;
  }
  if (expect_contents(source, "first second") != 0 || !muon_path_exists(source)) goto done;
  size_t reference_length = wcslen(reference_root) + wcslen(L"\\source.txt") + 1;
  wchar_t *reference = malloc(reference_length * sizeof(wchar_t));
  if (reference == NULL) goto done;
  swprintf(reference, reference_length, L"%ls\\source.txt", reference_root);
  HANDLE handle = CreateFileW(reference, GENERIC_READ, FILE_SHARE_READ, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  free(reference);
  if (handle == INVALID_HANDLE_VALUE) {
    fail("independent file location");
    goto done;
  }
  char actual[32] = {0};
  DWORD read = 0;
  BOOL success = ReadFile(handle, actual, sizeof(actual), &read, NULL);
  CloseHandle(handle);
  if (!success || read != strlen("first second") || memcmp(actual, "first second", read)) {
    fail("independent file contents");
    goto done;
  }
  unsigned long long size = 0;
  if (muon_get_file_size(source, &size) != 0 || size != strlen("first second")) {
    fail("file size");
    goto done;
  }
  if (muon_copy_file_with_source_mode(source, destination) != 0 ||
      expect_contents(destination, "first second") != 0) {
    fail("copy");
    goto done;
  }
  char source_hash[SHA256_DIGEST_STRING_LENGTH];
  char destination_hash[SHA256_DIGEST_STRING_LENGTH];
  if (muon_sha256_file_hex(source, source_hash) != 0 ||
      muon_sha256_file_hex(destination, destination_hash) != 0 ||
      strcmp(source_hash, destination_hash) != 0) {
    fail("SHA-256");
    goto done;
  }
  if (muon_write_text_file(replacement, "new version") != 0 ||
      muon_atomic_replace(replacement, destination) != 0 ||
      muon_path_exists(replacement) || expect_contents(destination, "new version") != 0) {
    fail("atomic replace");
    goto done;
  }
  // A failed replacement must preserve the file already at the destination.
  if (muon_atomic_replace(replacement, destination) == 0 ||
      expect_contents(destination, "new version") != 0) goto done;
  if (muon_remove_recursive(source) != 0 || muon_remove_recursive(destination) != 0 ||
      muon_path_exists(source) || muon_path_exists(destination)) {
    fail("file removal");
    goto done;
  }
  result = 0;
done:
  free(source);
  free(destination);
  free(replacement);
  return result;
}

static int tree(const char *source, const char *destination) {
  size_t copied = 0;
  size_t counted = 0;
  if (muon_copy_directory_contents(source, destination, &copied) != 0 || copied != 2 ||
      muon_count_files_recursive(destination, &counted) != 0 || counted != 2) {
    return fail("tree copy/count");
  }
  char *first = muon_path_join(destination, "日本語.txt");
  char *second = muon_path_join3(destination, "子階層", "emoji-😀.txt");
  int result = first != NULL && second != NULL &&
      expect_contents(first, "first") == 0 && expect_contents(second, "second") == 0 ? 0 : 1;
  free(first);
  free(second);
  if (result == 0 && (muon_remove_recursive(destination) != 0 || muon_path_exists(destination))) {
    result = fail("tree removal");
  }
  return result;
}

// Behavioral counterparts of .NET Directory.GetFiles' two legacy-boundary
// tests and Directory.EnumerableTests.EnumerateDirectories_TrailingDot.
// https://github.com/dotnet/runtime/tree/v10.0.0/src/libraries/System.Runtime/tests/System.IO.FileSystem.Tests/Directory
static int named_tree(const char *source, const char *destination, int exact) {
  const char *names[] = {"0000000", "11111111", "222222222", "3333333333",
                         "44444444444", "555555555555"};
  const char *exact_names[] = {"item", "item.", "item "};
  size_t count = exact ? 3 : 6;
  size_t copied = 0;
  size_t counted = 0;
  if (muon_copy_directory_contents(source, destination, &copied) != 0 || copied != count ||
      muon_count_files_recursive(destination, &counted) != 0 || counted != count) {
    return fail("named tree copy/count");
  }
  for (size_t index = 0; index < count; ++index) {
    const char *name = exact ? exact_names[index] : names[index];
    size_t length = strlen(destination) + strlen(name) + 6;
    char *path = malloc(length);
    if (path == NULL) return 1;
    // Construct the reference independently to distinguish exact sibling names.
    snprintf(path, length, "\\\\?\\%s\\%s", destination, name);
    char expected[16];
    snprintf(expected, sizeof(expected), "%zu", index);
    int result = expect_contents(path, expected);
    free(path);
    if (result != 0) return result;
  }
  char source_hash[SHA256_DIGEST_STRING_LENGTH];
  char destination_hash[SHA256_DIGEST_STRING_LENGTH];
  if (muon_fingerprint_directory_contents(source, source_hash) != 0 ||
      muon_fingerprint_directory_contents(destination, destination_hash) != 0 ||
      strcmp(source_hash, destination_hash) != 0) return fail("tree fingerprint");
  if (muon_remove_recursive(destination) != 0 || muon_path_exists(destination)) {
    return fail("named tree deletion");
  }
  return 0;
}

// File.Copy.ShortenLongPath: aliases of the same file must not destroy data.
static int copy_to_self(const char *root) {
  char *source = muon_path_join(root, "same.txt");
  size_t length = strlen(root) + 320;
  char *alias = malloc(length);
  if (source == NULL || alias == NULL) { free(source); free(alias); return 1; }
  strcpy(alias, root);
  for (int index = 0; index < 150; ++index) strcat(alias, "\\.");
  strcat(alias, "\\same.txt");
  int result = 1;
  if (muon_write_text_file(source, "preserved") == 0 &&
      muon_copy_file_with_source_mode(source, alias) != 0 &&
      expect_contents(source, "preserved") == 0) result = 0;
  free(source);
  free(alias);
  return result == 0 ? 0 : fail("copying a normalized alias must preserve the source");
}

// IOInputs uses 32767/32768 for rejected paths and 256 for rejected components.
// Generate these in-process to avoid testing CreateProcess's command-line limit.
static int limits(const char *root) {
  char *source = muon_path_join(root, "preserve.txt");
  char *path = malloc(32770);
  if (source == NULL || path == NULL) { free(source); free(path); return 1; }
  int result = 1;
  if (muon_write_text_file(source, "preserved") != 0) goto done;
  for (int extended = 0; extended <= 1; ++extended) {
    for (size_t length = 32767; length <= 32768; ++length) {
      snprintf(path, 32770, "%s%s", extended ? "\\\\?\\" : "", root);
      size_t start = strlen(path);
      for (size_t i = start; i < length; ++i) path[i] = (i - start) % 71 == 0 ? '\\' : 'a';
      path[length] = '\0';
      if (muon_ensure_directory(path) == 0 || muon_path_exists(path) ||
          muon_copy_file_with_source_mode(source, path) == 0 ||
          muon_atomic_replace(source, path) == 0 || expect_contents(source, "preserved") != 0) goto done;
    }
  }
  size_t start = strlen(root) + 1;
  snprintf(path, 32770, "%s\\", root);
  memset(path + start, 'c', 255);
  path[start + 255] = '\0';
  if (muon_write_text_file(path, "255") != 0 || expect_contents(path, "255") != 0) goto done;
  path[start + 255] = 'c';
  path[start + 256] = '\0';
  if (muon_write_text_file(path, "256") == 0 || muon_ensure_directory(path) == 0 ||
      muon_path_exists(path)) goto done;
  path[start + 255] = '\0';
  if (expect_contents(path, "255") != 0) goto done;
  result = 0;
done:
  free(source);
  free(path);
  return result == 0 ? 0 : fail("path/component limits");
}

static int cef_archive(const char *path, const char *output, int force) {
  size_t count = 0;
  if (muon_prepare_extract_cef_archive_full(path, output, force, &count) != 0 || count != 4) {
    return fail("CEF archive extraction");
  }
  char *license = muon_read_tar_bz2_text_file(path, "LICENSE", 1);
  int valid = license != NULL && strcmp(license, "CEF license") == 0;
  free(license);
  if (!valid) return fail("CEF archive text");
  return 0;
}

static int node_archive(const char *path, const char *output) {
  MuonNodeArtifact artifact = {
    .version = "v24.4.0",
#ifdef _WIN64
    .catalog_file = "win-x64-zip",
    .archive_target = "win-x64",
    .file_name = "node-v24.4.0-win-x64.zip",
#else
    .catalog_file = "win-x86-zip",
    .archive_target = "win-x86",
    .file_name = "node-v24.4.0-win-x86.zip",
#endif
    .url = "", .sha256 = "", .lts = "Krypton"
  };
  size_t count = 0;
  return muon_prepare_install_node_runtime_progress(path, MUON_PREPARE_TARGET_NAME,
      &artifact, output, &count, NULL, NULL) == 0 && count == 2
      ? 0 : fail("Node archive installation");
}

int wmain(int argc, wchar_t **argv) {
  if (argc == 5 && !wcscmp(argv[1], L"resource-failure")) {
    wchar_t *arguments[] = {argv[0], L"resource", L"--input", argv[2],
        L"--updates-json", argv[3], L"--output", argv[4], NULL};
    return muon_windows_main(8, arguments, resource_failure);
  }
  if (argc < 3 || argc > 4) return 2;
  char *path = utf8(argv[2]);
  char *other = argc == 4 ? utf8(argv[3]) : NULL;
  if (path == NULL || (argc == 4 && other == NULL)) {
    free(path);
    free(other);
    return 2;
  }
  int result = 2;
  if (!wcscmp(argv[1], L"mkdir")) {
    result = muon_ensure_directory(path) == 0 ? 0 : fail("recursive mkdir");
  } else if (!wcscmp(argv[1], L"exists-directory")) {
    struct stat entry;
    result = muon_stat(path, &entry) == 0 && S_ISDIR(entry.st_mode)
        ? 0 : fail("expected directory");
  } else if (!wcscmp(argv[1], L"files") && argc == 4) {
    result = files(path, argv[3]);
  } else if (!wcscmp(argv[1], L"tree") && other != NULL) {
    result = tree(path, other);
  } else if (!wcscmp(argv[1], L"enumerate") && other != NULL) {
    result = named_tree(path, other, 0);
  } else if (!wcscmp(argv[1], L"exact") && other != NULL) {
    result = named_tree(path, other, 1);
  } else if (!wcscmp(argv[1], L"copy-self")) {
    result = copy_to_self(path);
  } else if (!wcscmp(argv[1], L"limits")) {
    result = limits(path);
  } else if ((!wcscmp(argv[1], L"cef") || !wcscmp(argv[1], L"cef-force")) && other != NULL) {
    result = cef_archive(path, other, !wcscmp(argv[1], L"cef-force"));
  } else if (!wcscmp(argv[1], L"node") && other != NULL) {
    result = node_archive(path, other);
  } else if (!wcscmp(argv[1], L"remove")) {
    result = muon_remove_recursive(path) == 0 && !muon_path_exists(path) ? 0 : fail("recursive remove");
  } else if (!wcscmp(argv[1], L"contents") && other != NULL) {
    result = expect_contents(path, other);
  } else if (!wcscmp(argv[1], L"move") && other != NULL) {
    result = muon_rename(path, other) == 0 && !muon_path_exists(path) &&
        expect_contents(other, "preserved") == 0 && muon_rename(other, path) == 0 &&
        !muon_path_exists(other) && expect_contents(path, "preserved") == 0 ? 0 : fail("move/restore");
  }
  free(path);
  free(other);
  return result;
}
