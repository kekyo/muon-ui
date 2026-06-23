// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#define _POSIX_C_SOURCE 200809L
#endif

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#else
#include <direct.h>
#include <io.h>
#include <process.h>
#endif

#include "archive.h"
#include "archive_entry.h"
#include "common.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifdef _WIN32
#define MUON_OPEN_READ_FLAGS (_O_RDONLY | _O_BINARY)
#define MUON_OPEN_WRITE_FLAGS (_O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY)
#define MUON_PROGRESS_POLL_MILLISECONDS 100
typedef int MuonSSize;
#else
#define MUON_OPEN_READ_FLAGS O_RDONLY
#define MUON_OPEN_WRITE_FLAGS (O_WRONLY | O_CREAT | O_TRUNC)
typedef ssize_t MuonSSize;
#endif

static int g_quiet = 0;

void muon_set_quiet(int quiet) { g_quiet = quiet; }

void muon_print_error(const char *format, ...) {
  if (g_quiet) {
    return;
  }
  va_list arguments;
  va_start(arguments, format);
  vfprintf(stderr, format, arguments);
  va_end(arguments);
  fflush(stderr);
}

void muon_print_errno(const char *message) {
  if (g_quiet) {
    return;
  }
  perror(message);
  fflush(stderr);
}

void muon_log_message(const char *format, ...) {
  if (g_quiet) {
    return;
  }
  va_list arguments;
  va_start(arguments, format);
  vfprintf(stderr, format, arguments);
  va_end(arguments);
  fputc('\n', stderr);
  fflush(stderr);
}

void muon_report_progress(MuonPrepareProgressCallback callback,
                          void *user_data,
                          MuonPrepareProgressPhase phase,
                          const char *status,
                          unsigned long long current,
                          unsigned long long total,
                          int determinate) {
  if (callback == NULL) {
    return;
  }
  MuonPrepareProgress progress;
  progress.phase = phase;
  progress.status = status;
  progress.current = current;
  progress.total = total;
  progress.determinate = determinate;
  callback(&progress, user_data);
}

void muon_free_string_array(char **values, size_t count) {
  if (values == NULL) {
    return;
  }
  for (size_t index = 0; index < count; index += 1) {
    free(values[index]);
  }
  free(values);
}

char *muon_duplicate_string(const char *value) {
  const size_t size = strlen(value) + 1;
  char *result = (char *)malloc(size);
  if (result == NULL) {
    return NULL;
  }
  memcpy(result, value, size);
  return result;
}

char *muon_substring(const char *start, size_t length) {
  char *result = (char *)malloc(length + 1);
  if (result == NULL) {
    return NULL;
  }
  memcpy(result, start, length);
  result[length] = '\0';
  return result;
}

static int is_path_separator(char value) {
  return value == '/' || value == '\\';
}

void muon_normalize_path_separators(char *path) {
  if (path == NULL) {
    return;
  }
  for (char *cursor = path; *cursor != '\0'; cursor += 1) {
    if (*cursor == '\\') {
      *cursor = '/';
    }
  }
}

char *muon_duplicate_path_string(const char *value) {
  char *result = muon_duplicate_string(value);
  muon_normalize_path_separators(result);
  return result;
}

char *muon_path_join(const char *left, const char *right) {
  if (left == NULL || left[0] == '\0') {
    return muon_duplicate_string(right);
  }
  if (right == NULL || right[0] == '\0') {
    return muon_duplicate_string(left);
  }
  const int needs_separator = !is_path_separator(left[strlen(left) - 1]);
  const size_t size = strlen(left) + strlen(right) + (needs_separator ? 2 : 1);
  char *result = (char *)malloc(size);
  if (result == NULL) {
    return NULL;
  }
  snprintf(result, size, needs_separator ? "%s/%s" : "%s%s", left, right);
  return result;
}

char *muon_path_join3(const char *first, const char *second,
                      const char *third) {
  char *left = muon_path_join(first, second);
  if (left == NULL) {
    return NULL;
  }
  char *result = muon_path_join(left, third);
  free(left);
  return result;
}

char *muon_parent_directory(const char *path) {
  const char *slash = strrchr(path, '/');
#ifdef _WIN32
  const char *backslash = strrchr(path, '\\');
  if (backslash != NULL && (slash == NULL || backslash > slash)) {
    slash = backslash;
  }
#endif
  if (slash == NULL) {
    return muon_duplicate_string(".");
  }
  if (slash == path) {
    return muon_duplicate_string("/");
  }
#ifdef _WIN32
  if (slash == path + 2 && isalpha((unsigned char)path[0]) &&
      path[1] == ':') {
    return muon_substring(path, 3);
  }
#endif
  return muon_substring(path, (size_t)(slash - path));
}

#ifdef _WIN32
static int muon_access(const char *path, int mode) { return _access(path, mode); }

static int muon_chmod(const char *path, int mode) { return _chmod(path, mode); }

static int muon_close(int descriptor) { return _close(descriptor); }

static int muon_getpid(void) { return _getpid(); }

static int muon_lstat(const char *path, struct stat *entry) {
  return stat(path, entry);
}

static int muon_mkdir(const char *path, int mode) {
  (void)mode;
  return _mkdir(path);
}

static int muon_open(const char *path, int flags, int mode) {
  return _open(path, flags, mode);
}

static MuonSSize muon_read(int descriptor, void *buffer, size_t size) {
  return _read(descriptor, buffer, (unsigned int)size);
}

static int muon_rmdir(const char *path) { return _rmdir(path); }

static int muon_stat(const char *path, struct stat *entry) {
  return stat(path, entry);
}

static int muon_unlink(const char *path) { return _unlink(path); }

static MuonSSize muon_write(int descriptor, const void *buffer, size_t size) {
  return _write(descriptor, buffer, (unsigned int)size);
}

static int is_windows_drive_root_path(const char *path) {
  return isalpha((unsigned char)path[0]) && path[1] == ':' &&
         (path[2] == '\0' || ((path[2] == '/' || path[2] == '\\') &&
                              path[3] == '\0'));
}
#else
static int muon_access(const char *path, int mode) { return access(path, mode); }

static int muon_chmod(const char *path, int mode) { return chmod(path, mode); }

static int muon_close(int descriptor) { return close(descriptor); }

static int muon_getpid(void) { return (int)getpid(); }

static int muon_lstat(const char *path, struct stat *entry) {
  return lstat(path, entry);
}

static int muon_mkdir(const char *path, int mode) {
  return mkdir(path, (mode_t)mode);
}

static int muon_open(const char *path, int flags, int mode) {
  return open(path, flags, (mode_t)mode);
}

static MuonSSize muon_read(int descriptor, void *buffer, size_t size) {
  return read(descriptor, buffer, size);
}

static int muon_rmdir(const char *path) { return rmdir(path); }

static int muon_stat(const char *path, struct stat *entry) {
  return stat(path, entry);
}

static int muon_unlink(const char *path) { return unlink(path); }

static MuonSSize muon_write(int descriptor, const void *buffer, size_t size) {
  return write(descriptor, buffer, size);
}
#endif

int muon_path_exists(const char *path) {
  return muon_access(path, F_OK) == 0;
}

int muon_ensure_directory(const char *path) {
  char buffer[PATH_MAX];
  const size_t length = strlen(path);
  if (length == 0 || strcmp(path, ".") == 0) {
    return 0;
  }
  if (length >= sizeof(buffer)) {
    muon_print_error("Path is too long: %s\n", path);
    return -1;
  }
  memcpy(buffer, path, length + 1);
#ifdef _WIN32
  if (is_windows_drive_root_path(buffer)) {
    return 0;
  }
#endif
  char *cursor = buffer + 1;
#ifdef _WIN32
  if (length >= 3 && isalpha((unsigned char)buffer[0]) && buffer[1] == ':' &&
      (buffer[2] == '/' || buffer[2] == '\\')) {
    cursor = buffer + 3;
  }
#endif
  for (; *cursor != '\0'; cursor += 1) {
    if (is_path_separator(*cursor)) {
      const char separator = *cursor;
      *cursor = '\0';
      if (muon_mkdir(buffer, 0777) != 0 && errno != EEXIST) {
        muon_print_errno(buffer);
        return -1;
      }
      *cursor = separator;
    }
  }
  if (muon_mkdir(buffer, 0777) != 0 && errno != EEXIST) {
    muon_print_errno(buffer);
    return -1;
  }
  return 0;
}

static int muon_copy_file_internal(
    const char *source, const char *destination, int mode,
    unsigned long long total_size,
    MuonPrepareProgressCallback progress_callback, void *progress_user_data,
    MuonPrepareProgressPhase phase, const char *status) {
  char buffer[64 * 1024];
  char *parent = muon_parent_directory(destination);
  if (parent == NULL) {
    return -1;
  }
  if (muon_ensure_directory(parent) != 0) {
    free(parent);
    return -1;
  }
  free(parent);
  const int input = muon_open(source, MUON_OPEN_READ_FLAGS, 0);
  if (input < 0) {
    muon_print_errno(source);
    return -1;
  }
  const int output =
      muon_open(destination, MUON_OPEN_WRITE_FLAGS, mode & 0777);
  if (output < 0) {
    muon_print_errno(destination);
    muon_close(input);
    return -1;
  }
  unsigned long long copied_size = 0;
  for (;;) {
    const MuonSSize read_size = muon_read(input, buffer, sizeof(buffer));
    if (read_size < 0) {
      muon_print_errno(source);
      muon_close(input);
      muon_close(output);
      return -1;
    }
    if (read_size == 0) {
      break;
    }
    MuonSSize written = 0;
    while (written < read_size) {
      const MuonSSize write_size =
          muon_write(output, buffer + written, (size_t)(read_size - written));
      if (write_size < 0) {
        muon_print_errno(destination);
        muon_close(input);
        muon_close(output);
        return -1;
      }
      written += write_size;
      copied_size += (unsigned long long)write_size;
      if (total_size != 0) {
        muon_report_progress(progress_callback, progress_user_data, phase,
                             status, copied_size, total_size, 1);
      }
    }
  }
  muon_close(input);
  if (muon_close(output) != 0) {
    muon_print_errno(destination);
    return -1;
  }
  if (muon_chmod(destination, mode & 0777) != 0) {
    muon_print_errno(destination);
    return -1;
  }
  return 0;
}

int muon_copy_file(const char *source, const char *destination, int mode) {
  return muon_copy_file_internal(source, destination, mode, 0, NULL, NULL,
                                 MUON_PREPARE_PROGRESS_PHASE_INSTALLING, "");
}

int muon_copy_file_with_source_mode(const char *source,
                                    const char *destination) {
  struct stat entry;
  if (muon_stat(source, &entry) != 0) {
    muon_print_errno(source);
    return -1;
  }
  return muon_copy_file_internal(source, destination, (int)entry.st_mode, 0,
                                 NULL, NULL,
                                 MUON_PREPARE_PROGRESS_PHASE_INSTALLING, "");
}

int muon_copy_file_with_source_mode_progress(
    const char *source, const char *destination,
    MuonPrepareProgressCallback progress_callback, void *progress_user_data,
    MuonPrepareProgressPhase phase, const char *status) {
  struct stat entry;
  if (muon_stat(source, &entry) != 0) {
    muon_print_errno(source);
    return -1;
  }
  return muon_copy_file_internal(
      source, destination, (int)entry.st_mode, (unsigned long long)entry.st_size,
      progress_callback, progress_user_data, phase, status);
}

static int muon_copy_path(const char *source, const char *destination,
                          size_t *file_count,
                          MuonPrepareProgressCallback progress_callback,
                          void *progress_user_data,
                          MuonPrepareProgressPhase phase,
                          const char *status) {
  struct stat entry;
  if (muon_lstat(source, &entry) != 0) {
    muon_print_errno(source);
    return -1;
  }
  if (S_ISDIR(entry.st_mode)) {
    if (muon_ensure_directory(destination) != 0) {
      return -1;
    }
    DIR *directory = opendir(source);
    if (directory == NULL) {
      muon_print_errno(source);
      return -1;
    }
    struct dirent *child;
    while ((child = readdir(directory)) != NULL) {
      if (strcmp(child->d_name, ".") == 0 || strcmp(child->d_name, "..") == 0) {
        continue;
      }
      char *child_source = muon_path_join(source, child->d_name);
      char *child_destination = muon_path_join(destination, child->d_name);
      if (child_source == NULL || child_destination == NULL ||
          muon_copy_path(child_source, child_destination, file_count,
                         progress_callback, progress_user_data, phase,
                         status) != 0) {
        free(child_source);
        free(child_destination);
        closedir(directory);
        return -1;
      }
      free(child_source);
      free(child_destination);
    }
    closedir(directory);
    muon_chmod(destination, entry.st_mode & 0777);
    return 0;
  }
  if (S_ISREG(entry.st_mode)) {
    if (muon_copy_file(source, destination, (int)entry.st_mode) != 0) {
      return -1;
    }
    if (file_count != NULL) {
      *file_count += 1;
    }
    muon_report_progress(progress_callback, progress_user_data, phase, status,
                         file_count == NULL ? 0 : (unsigned long long)*file_count,
                         0, 0);
    return 0;
  }
  return 0;
}

int muon_copy_directory_contents_progress(
    const char *source, const char *destination, size_t *file_count,
    MuonPrepareProgressCallback progress_callback, void *progress_user_data,
    MuonPrepareProgressPhase phase, const char *status) {
  DIR *directory = opendir(source);
  if (directory == NULL) {
    if (errno == ENOENT) {
      return 0;
    }
    muon_print_errno(source);
    return -1;
  }
  if (muon_ensure_directory(destination) != 0) {
    closedir(directory);
    return -1;
  }
  struct dirent *child;
  while ((child = readdir(directory)) != NULL) {
    if (strcmp(child->d_name, ".") == 0 || strcmp(child->d_name, "..") == 0) {
      continue;
    }
    char *child_source = muon_path_join(source, child->d_name);
    char *child_destination = muon_path_join(destination, child->d_name);
    if (child_source == NULL || child_destination == NULL ||
        muon_copy_path(child_source, child_destination, file_count,
                       progress_callback, progress_user_data, phase,
                       status) != 0) {
      free(child_source);
      free(child_destination);
      closedir(directory);
      return -1;
    }
    free(child_source);
    free(child_destination);
  }
  closedir(directory);
  return 0;
}

int muon_copy_directory_contents(const char *source, const char *destination,
                                 size_t *file_count) {
  return muon_copy_directory_contents_progress(
      source, destination, file_count, NULL, NULL,
      MUON_PREPARE_PROGRESS_PHASE_INSTALLING, "");
}

int muon_remove_recursive(const char *path) {
  struct stat entry;
  if (muon_lstat(path, &entry) != 0) {
    return errno == ENOENT ? 0 : -1;
  }
  if (S_ISDIR(entry.st_mode)) {
    DIR *directory = opendir(path);
    if (directory == NULL) {
      muon_print_errno(path);
      return -1;
    }
    struct dirent *child;
    while ((child = readdir(directory)) != NULL) {
      if (strcmp(child->d_name, ".") == 0 || strcmp(child->d_name, "..") == 0) {
        continue;
      }
      char *child_path = muon_path_join(path, child->d_name);
      if (child_path == NULL || muon_remove_recursive(child_path) != 0) {
        free(child_path);
        closedir(directory);
        return -1;
      }
      free(child_path);
    }
    closedir(directory);
    if (muon_rmdir(path) != 0) {
      muon_print_errno(path);
      return -1;
    }
    return 0;
  }
  if (muon_unlink(path) != 0) {
    muon_print_errno(path);
    return -1;
  }
  return 0;
}

char *muon_read_text_file(const char *path) {
  FILE *file = fopen(path, "rb");
  if (file == NULL) {
    muon_print_errno(path);
    return NULL;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return NULL;
  }
  const long size = ftell(file);
  if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return NULL;
  }
  char *buffer = (char *)malloc((size_t)size + 1);
  if (buffer == NULL) {
    fclose(file);
    return NULL;
  }
  if (fread(buffer, 1, (size_t)size, file) != (size_t)size) {
    free(buffer);
    fclose(file);
    return NULL;
  }
  buffer[size] = '\0';
  fclose(file);
  return buffer;
}

int muon_write_text_file(const char *path, const char *content) {
  char *parent = muon_parent_directory(path);
  if (parent == NULL) {
    return -1;
  }
  if (muon_ensure_directory(parent) != 0) {
    free(parent);
    return -1;
  }
  free(parent);
  FILE *file = fopen(path, "wb");
  if (file == NULL) {
    muon_print_errno(path);
    return -1;
  }
  if (fwrite(content, 1, strlen(content), file) != strlen(content)) {
    fclose(file);
    return -1;
  }
  fclose(file);
  return 0;
}

int muon_append_text_file(const char *path, const char *content) {
  char *parent = muon_parent_directory(path);
  if (parent == NULL) {
    return -1;
  }
  if (muon_ensure_directory(parent) != 0) {
    free(parent);
    return -1;
  }
  free(parent);
  FILE *file = fopen(path, "ab");
  if (file == NULL) {
    muon_print_errno(path);
    return -1;
  }
  if (fwrite(content, 1, strlen(content), file) != strlen(content)) {
    fclose(file);
    return -1;
  }
  fclose(file);
  return 0;
}

int muon_get_file_size(const char *path, unsigned long long *size) {
  struct stat entry;
  if (muon_stat(path, &entry) != 0) {
    muon_print_errno(path);
    return -1;
  }
  *size = (unsigned long long)entry.st_size;
  return 0;
}

int muon_count_files_recursive(const char *path, size_t *file_count) {
  struct stat entry;
  if (muon_lstat(path, &entry) != 0) {
    muon_print_errno(path);
    return -1;
  }
  if (S_ISDIR(entry.st_mode)) {
    DIR *directory = opendir(path);
    if (directory == NULL) {
      muon_print_errno(path);
      return -1;
    }
    struct dirent *child;
    while ((child = readdir(directory)) != NULL) {
      if (strcmp(child->d_name, ".") == 0 || strcmp(child->d_name, "..") == 0) {
        continue;
      }
      char *child_path = muon_path_join(path, child->d_name);
      if (child_path == NULL ||
          muon_count_files_recursive(child_path, file_count) != 0) {
        free(child_path);
        closedir(directory);
        return -1;
      }
      free(child_path);
    }
    closedir(directory);
  } else if (S_ISREG(entry.st_mode)) {
    *file_count += 1;
  }
  return 0;
}

unsigned long long muon_current_unix_time(void) {
  const time_t now = time(NULL);
  return now < 0 ? 0 : (unsigned long long)now;
}

yyjson_doc *muon_json_read_file(const char *path) {
  char *content = muon_read_text_file(path);
  if (content == NULL) {
    return NULL;
  }
  yyjson_read_err error;
  yyjson_doc *document =
      yyjson_read_opts(content, strlen(content), YYJSON_READ_NOFLAG, NULL,
                       &error);
  if (document == NULL) {
    muon_print_error("Invalid JSON: %s: %s\n", path, error.msg);
  }
  free(content);
  return document;
}

char *muon_json_copy_string(yyjson_val *object, const char *key) {
  yyjson_val *value = yyjson_obj_get(object, key);
  const char *string_value =
      value != NULL && yyjson_is_str(value) ? yyjson_get_str(value) : NULL;
  return string_value == NULL ? NULL : muon_duplicate_string(string_value);
}

int muon_json_get_int(yyjson_val *object, const char *key, int *value) {
  yyjson_val *field = yyjson_obj_get(object, key);
  if (field == NULL || !yyjson_is_int(field)) {
    return -1;
  }
  const int64_t actual = yyjson_get_sint(field);
  if (actual < INT_MIN || actual > INT_MAX) {
    return -1;
  }
  *value = (int)actual;
  return 0;
}

int muon_json_get_uint64(yyjson_val *object, const char *key,
                         unsigned long long *value) {
  yyjson_val *field = yyjson_obj_get(object, key);
  if (field == NULL || !yyjson_is_uint(field)) {
    return -1;
  }
  *value = (unsigned long long)yyjson_get_uint(field);
  return 0;
}

int muon_json_get_string_array(yyjson_val *object, const char *key,
                               char ***values, size_t *count) {
  *values = NULL;
  *count = 0;
  yyjson_val *array = yyjson_obj_get(object, key);
  if (array == NULL || !yyjson_is_arr(array)) {
    return -1;
  }
  const size_t array_size = yyjson_arr_size(array);
  if (array_size == 0) {
    return 0;
  }
  char **result = (char **)calloc(array_size, sizeof(char *));
  if (result == NULL) {
    return -1;
  }
  size_t index = 0;
  size_t max = 0;
  yyjson_val *entry = NULL;
  yyjson_arr_foreach(array, index, max, entry) {
    if (!yyjson_is_str(entry)) {
      muon_free_string_array(result, index);
      return -1;
    }
    result[index] = muon_duplicate_string(yyjson_get_str(entry));
    if (result[index] == NULL) {
      muon_free_string_array(result, index);
      return -1;
    }
  }
  *values = result;
  *count = array_size;
  return 0;
}

void muon_sha1_update_bytes(SHA1_CTX *context, const void *data, size_t size) {
  const uint8_t *cursor = (const uint8_t *)data;
  while (size > 0) {
    const unsigned int chunk =
        size > (size_t)UINT_MAX ? UINT_MAX : (unsigned int)size;
    SHA1Update(context, cursor, chunk);
    cursor += chunk;
    size -= chunk;
  }
}

void muon_sha1_update_string(SHA1_CTX *context, const char *value) {
  muon_sha1_update_bytes(context, value, strlen(value));
}

void muon_sha1_digest_to_hex(const uint8_t digest[SHA1_DIGEST_LENGTH],
                             char output[SHA1_DIGEST_STRING_LENGTH]) {
  for (int index = 0; index < SHA1_DIGEST_LENGTH; index += 1) {
    snprintf(output + index * 2, 3, "%02x", digest[index]);
  }
  output[SHA1_DIGEST_STRING_LENGTH - 1] = '\0';
}

static int muon_sha1_file_digest(const char *path,
                                 uint8_t digest[SHA1_DIGEST_LENGTH]) {
  const int input = muon_open(path, MUON_OPEN_READ_FLAGS, 0);
  if (input < 0) {
    muon_print_errno(path);
    return -1;
  }
  SHA1_CTX context;
  SHA1Init(&context);
  uint8_t buffer[64 * 1024];
  for (;;) {
    const MuonSSize read_size = muon_read(input, buffer, sizeof(buffer));
    if (read_size < 0) {
      muon_print_errno(path);
      muon_close(input);
      return -1;
    }
    if (read_size == 0) {
      break;
    }
    muon_sha1_update_bytes(&context, buffer, (size_t)read_size);
  }
  if (muon_close(input) != 0) {
    muon_print_errno(path);
    return -1;
  }
  SHA1Final(digest, &context);
  return 0;
}

int muon_sha1_file_hex(const char *path,
                       char output[SHA1_DIGEST_STRING_LENGTH]) {
  uint8_t digest[SHA1_DIGEST_LENGTH];
  if (muon_sha1_file_digest(path, digest) != 0) {
    return -1;
  }
  muon_sha1_digest_to_hex(digest, output);
  return 0;
}

static void sha1_update_uint64(SHA1_CTX *context, uint64_t value) {
  uint8_t buffer[8];
  for (int index = 0; index < 8; index += 1) {
    buffer[7 - index] = (uint8_t)(value >> (index * 8));
  }
  muon_sha1_update_bytes(context, buffer, sizeof(buffer));
}

static void sha1_update_record_string(SHA1_CTX *context, const char *value) {
  sha1_update_uint64(context, (uint64_t)strlen(value));
  muon_sha1_update_string(context, value);
}

typedef struct {
  char **values;
  size_t count;
  size_t capacity;
} NameList;

static void name_list_free(NameList *list) {
  muon_free_string_array(list->values, list->count);
  list->values = NULL;
  list->count = 0;
  list->capacity = 0;
}

static int name_list_add(NameList *list, const char *value) {
  if (list->count == list->capacity) {
    const size_t next_capacity = list->capacity == 0 ? 16 : list->capacity * 2;
    char **next =
        (char **)realloc(list->values, sizeof(char *) * next_capacity);
    if (next == NULL) {
      return -1;
    }
    list->values = next;
    list->capacity = next_capacity;
  }
  list->values[list->count] = muon_duplicate_string(value);
  if (list->values[list->count] == NULL) {
    return -1;
  }
  list->count += 1;
  return 0;
}

static int compare_string_pointers(const void *left, const void *right) {
  char *const *left_value = (char *const *)left;
  char *const *right_value = (char *const *)right;
  return strcmp(*left_value, *right_value);
}

static int read_sorted_directory_names(const char *path, NameList *list) {
  DIR *directory = opendir(path);
  if (directory == NULL) {
    muon_print_errno(path);
    return -1;
  }
  struct dirent *child;
  while ((child = readdir(directory)) != NULL) {
    if (strcmp(child->d_name, ".") == 0 || strcmp(child->d_name, "..") == 0) {
      continue;
    }
    if (name_list_add(list, child->d_name) != 0) {
      closedir(directory);
      return -1;
    }
  }
  closedir(directory);
  qsort(list->values, list->count, sizeof(char *), compare_string_pointers);
  return 0;
}

int muon_fingerprint_path_recursive(
    const char *path, const char *relative,
    char fingerprint[SHA1_DIGEST_STRING_LENGTH]) {
  struct stat entry;
  if (muon_lstat(path, &entry) != 0) {
    muon_print_errno(path);
    return -1;
  }
  SHA1_CTX context;
  SHA1Init(&context);
  sha1_update_record_string(&context, relative);
  sha1_update_uint64(&context, (uint64_t)(entry.st_mode & 0777));
  if (S_ISDIR(entry.st_mode)) {
    sha1_update_record_string(&context, "directory");
    NameList children = {0};
    if (read_sorted_directory_names(path, &children) != 0) {
      name_list_free(&children);
      return -1;
    }
    for (size_t index = 0; index < children.count; index += 1) {
      char *child_path = muon_path_join(path, children.values[index]);
      char *child_relative =
          relative[0] == '\0' ? muon_duplicate_string(children.values[index])
                              : muon_path_join(relative, children.values[index]);
      char child_fingerprint[SHA1_DIGEST_STRING_LENGTH];
      if (child_path == NULL || child_relative == NULL ||
          muon_fingerprint_path_recursive(child_path, child_relative,
                                          child_fingerprint) != 0) {
        free(child_path);
        free(child_relative);
        name_list_free(&children);
        return -1;
      }
      sha1_update_record_string(&context, children.values[index]);
      sha1_update_record_string(&context, child_fingerprint);
      free(child_path);
      free(child_relative);
    }
    name_list_free(&children);
  } else if (S_ISREG(entry.st_mode)) {
    uint8_t content_digest[SHA1_DIGEST_LENGTH];
    if (muon_sha1_file_digest(path, content_digest) != 0) {
      return -1;
    }
    sha1_update_record_string(&context, "file");
    muon_sha1_update_bytes(&context, content_digest, sizeof(content_digest));
  } else {
    sha1_update_record_string(&context, "other");
    sha1_update_uint64(&context, (uint64_t)entry.st_size);
  }
  uint8_t digest[SHA1_DIGEST_LENGTH];
  SHA1Final(digest, &context);
  muon_sha1_digest_to_hex(digest, fingerprint);
  return 0;
}

int muon_fingerprint_directory_contents(
    const char *path, char fingerprint[SHA1_DIGEST_STRING_LENGTH]) {
  return muon_fingerprint_path_recursive(path, "", fingerprint);
}

char *muon_create_ready_content(const char *muon_fingerprint,
                                const char *cef_fingerprint) {
  const int size = snprintf(NULL, 0,
                            "{\"ready\":true,\"muonFingerprint\":\"%s\","
                            "\"cefFingerprint\":\"%s\"}\n",
                            muon_fingerprint, cef_fingerprint);
  if (size < 0) {
    return NULL;
  }
  char *result = (char *)malloc((size_t)size + 1);
  if (result == NULL) {
    return NULL;
  }
  snprintf(result, (size_t)size + 1,
           "{\"ready\":true,\"muonFingerprint\":\"%s\","
           "\"cefFingerprint\":\"%s\"}\n",
           muon_fingerprint, cef_fingerprint);
  return result;
}

int muon_ready_file_matches(const char *ready_path,
                            const char *expected_content) {
  char *actual = muon_read_text_file(ready_path);
  if (actual == NULL) {
    return 0;
  }
  const int matches = strcmp(actual, expected_content) == 0;
  free(actual);
  return matches;
}

static int archive_path_has_drive_prefix(const char *path) {
  return isalpha((unsigned char)path[0]) && path[1] == ':';
}

static int archive_path_is_safe_relative(const char *path) {
  if (path[0] == '\0' || is_path_separator(path[0]) ||
      archive_path_has_drive_prefix(path)) {
    return 0;
  }
  const char *segment_start = path;
  for (const char *cursor = path;; cursor += 1) {
    if (*cursor != '/' && *cursor != '\0') {
      continue;
    }
    const size_t segment_size = (size_t)(cursor - segment_start);
    if (segment_size == 2 && segment_start[0] == '.' &&
        segment_start[1] == '.') {
      return 0;
    }
    if (*cursor == '\0') {
      return 1;
    }
    segment_start = cursor + 1;
  }
}

static char *archive_entry_relative_path(const char *path, int strip_components,
                                         int *skip_entry) {
  *skip_entry = 0;
  if (path == NULL || path[0] == '\0') {
    *skip_entry = 1;
    return NULL;
  }
  char *normalized = muon_duplicate_path_string(path);
  if (normalized == NULL) {
    return NULL;
  }
  if (is_path_separator(normalized[0]) ||
      archive_path_has_drive_prefix(normalized)) {
    muon_print_error("Unsafe archive entry path: %s\n", path);
    free(normalized);
    return NULL;
  }
  char *cursor = normalized;
  for (int index = 0; index < strip_components; index += 1) {
    while (*cursor == '/') {
      cursor += 1;
    }
    char *separator = strchr(cursor, '/');
    if (separator == NULL) {
      *skip_entry = 1;
      free(normalized);
      return NULL;
    }
    cursor = separator + 1;
  }
  while (*cursor == '/') {
    cursor += 1;
  }
  size_t length = strlen(cursor);
  while (length > 0 && cursor[length - 1] == '/') {
    length -= 1;
    cursor[length] = '\0';
  }
  if (cursor[0] == '\0') {
    *skip_entry = 1;
    free(normalized);
    return NULL;
  }
  if (!archive_path_is_safe_relative(cursor)) {
    muon_print_error("Unsafe archive entry path: %s\n", path);
    free(normalized);
    return NULL;
  }
  char *result = muon_duplicate_string(cursor);
  free(normalized);
  return result;
}

static int archive_print_error(struct archive *reader, const char *prefix) {
  const char *message = archive_error_string(reader);
  muon_print_error("%s: %s\n", prefix, message == NULL ? "archive error" : message);
  return -1;
}

static int extract_archive_file(struct archive *reader, struct archive_entry *entry,
                                const char *destination, int mode,
                                size_t *file_count,
                                MuonPrepareProgressCallback progress_callback,
                                void *progress_user_data,
                                MuonPrepareProgressPhase phase,
                                const char *status) {
  char *parent = muon_parent_directory(destination);
  if (parent == NULL) {
    return -1;
  }
  if (muon_ensure_directory(parent) != 0) {
    free(parent);
    return -1;
  }
  free(parent);
  const int output =
      muon_open(destination, MUON_OPEN_WRITE_FLAGS, mode & 0777);
  if (output < 0) {
    muon_print_errno(destination);
    return -1;
  }
  char buffer[64 * 1024];
  for (;;) {
    const la_ssize_t read_size =
        archive_read_data(reader, buffer, sizeof(buffer));
    if (read_size < 0) {
      archive_print_error(reader, "Failed to read archive data");
      muon_close(output);
      return -1;
    }
    if (read_size == 0) {
      break;
    }
    la_ssize_t written = 0;
    while (written < read_size) {
      const MuonSSize write_size =
          muon_write(output, buffer + written, (size_t)(read_size - written));
      if (write_size < 0) {
        muon_print_errno(destination);
        muon_close(output);
        return -1;
      }
      written += write_size;
    }
  }
  if (muon_close(output) != 0) {
    muon_print_errno(destination);
    return -1;
  }
  if (muon_chmod(destination, mode & 0777) != 0) {
    muon_print_errno(destination);
    return -1;
  }
  if (file_count != NULL) {
    *file_count += 1;
  }
  muon_report_progress(progress_callback, progress_user_data, phase, status,
                       file_count == NULL ? 0 : (unsigned long long)*file_count,
                       0, 0);
  (void)entry;
  return 0;
}

int muon_extract_tar_bz2_archive_progress(
    const char *archive_path, const char *destination, int strip_components,
    size_t *file_count, MuonPrepareProgressCallback progress_callback,
    void *progress_user_data, MuonPrepareProgressPhase phase,
    const char *status) {
  if (file_count != NULL) {
    *file_count = 0;
  }
  if (muon_ensure_directory(destination) != 0) {
    return -1;
  }
  struct archive *reader = archive_read_new();
  if (reader == NULL) {
    return -1;
  }
  if (archive_read_support_filter_bzip2(reader) != ARCHIVE_OK ||
      archive_read_support_format_tar(reader) != ARCHIVE_OK ||
      archive_read_open_filename(reader, archive_path, 64 * 1024) !=
          ARCHIVE_OK) {
    archive_print_error(reader, archive_path);
    archive_read_free(reader);
    return -1;
  }
  int result = 0;
  struct archive_entry *entry = NULL;
  for (;;) {
    const int archive_status = archive_read_next_header(reader, &entry);
    if (archive_status == ARCHIVE_EOF) {
      break;
    }
    if (archive_status != ARCHIVE_OK) {
      result = archive_print_error(reader, "Failed to read archive header");
      break;
    }
    const char *path = archive_entry_pathname_utf8(entry);
    if (path == NULL) {
      path = archive_entry_pathname(entry);
    }
    int skip_entry = 0;
    char *relative =
        archive_entry_relative_path(path, strip_components, &skip_entry);
    if (relative == NULL) {
      if (skip_entry) {
        archive_read_data_skip(reader);
        continue;
      }
      result = -1;
      break;
    }
    char *entry_destination = muon_path_join(destination, relative);
    free(relative);
    if (entry_destination == NULL) {
      result = -1;
      break;
    }
    const int file_type = archive_entry_filetype(entry);
    int mode = (int)archive_entry_perm(entry);
    if (mode == 0) {
      mode = S_ISDIR(file_type) ? 0777 : 0666;
    }
    if (S_ISDIR(file_type)) {
      if (muon_ensure_directory(entry_destination) != 0 ||
          muon_chmod(entry_destination, mode & 0777) != 0) {
        muon_print_errno(entry_destination);
        free(entry_destination);
        result = -1;
        break;
      }
    } else if (S_ISREG(file_type)) {
      if (extract_archive_file(reader, entry, entry_destination, mode,
                               file_count, progress_callback,
                               progress_user_data, phase, status) != 0) {
        free(entry_destination);
        result = -1;
        break;
      }
    } else {
      archive_read_data_skip(reader);
    }
    free(entry_destination);
  }
  if (archive_read_free(reader) != ARCHIVE_OK && result == 0) {
    result = -1;
  }
  return result;
}

int muon_extract_tar_bz2_archive(const char *archive_path,
                                 const char *destination,
                                 int strip_components,
                                 size_t *file_count) {
  return muon_extract_tar_bz2_archive_progress(
      archive_path, destination, strip_components, file_count, NULL, NULL,
      MUON_PREPARE_PROGRESS_PHASE_INSTALLING, "");
}

static int read_archive_entry_text(struct archive *reader, char **content) {
  size_t capacity = 4096;
  size_t size = 0;
  char *result = (char *)malloc(capacity);
  if (result == NULL) {
    return -1;
  }
  for (;;) {
    if (capacity - size < 4096) {
      const size_t next_capacity = capacity * 2;
      char *next = (char *)realloc(result, next_capacity);
      if (next == NULL) {
        free(result);
        return -1;
      }
      result = next;
      capacity = next_capacity;
    }
    const la_ssize_t read_size =
        archive_read_data(reader, result + size, capacity - size - 1);
    if (read_size < 0) {
      free(result);
      return archive_print_error(reader, "Failed to read archive data");
    }
    if (read_size == 0) {
      break;
    }
    size += (size_t)read_size;
  }
  result[size] = '\0';
  *content = result;
  return 0;
}

char *muon_read_tar_bz2_text_file(const char *archive_path,
                                  const char *relative_path,
                                  int strip_components) {
  struct archive *reader = archive_read_new();
  if (reader == NULL) {
    return NULL;
  }
  if (archive_read_support_filter_bzip2(reader) != ARCHIVE_OK ||
      archive_read_support_format_tar(reader) != ARCHIVE_OK ||
      archive_read_open_filename(reader, archive_path, 64 * 1024) !=
          ARCHIVE_OK) {
    archive_print_error(reader, archive_path);
    archive_read_free(reader);
    return NULL;
  }
  char *result = NULL;
  struct archive_entry *entry = NULL;
  for (;;) {
    const int status = archive_read_next_header(reader, &entry);
    if (status == ARCHIVE_EOF) {
      break;
    }
    if (status != ARCHIVE_OK) {
      archive_print_error(reader, "Failed to read archive header");
      break;
    }
    const char *path = archive_entry_pathname_utf8(entry);
    if (path == NULL) {
      path = archive_entry_pathname(entry);
    }
    int skip_entry = 0;
    char *relative =
        archive_entry_relative_path(path, strip_components, &skip_entry);
    if (relative == NULL) {
      if (skip_entry) {
        archive_read_data_skip(reader);
        continue;
      }
      break;
    }
    const int matches = strcmp(relative, relative_path) == 0;
    free(relative);
    if (!matches) {
      archive_read_data_skip(reader);
      continue;
    }
    if (!S_ISREG(archive_entry_filetype(entry))) {
      archive_read_data_skip(reader);
      break;
    }
    if (read_archive_entry_text(reader, &result) != 0) {
      result = NULL;
    }
    break;
  }
  archive_read_free(reader);
  return result;
}

static void report_file_progress(
    const char *path, unsigned long long total,
    MuonPrepareProgressCallback progress_callback, void *progress_user_data,
    MuonPrepareProgressPhase phase, const char *status) {
  unsigned long long size = 0;
  if (progress_callback == NULL || total == 0 ||
      !muon_path_exists(path) || muon_get_file_size(path, &size) != 0) {
    return;
  }
  if (size > total) {
    size = total;
  }
  muon_report_progress(progress_callback, progress_user_data, phase, status,
                       size, total, 1);
}

#ifndef _WIN32
static void wait_for_progress_poll(void) {
  struct timespec delay;
  delay.tv_sec = 0;
  delay.tv_nsec = 100 * 1000 * 1000;
  nanosleep(&delay, NULL);
}
#endif

int muon_run_process(char *const argv[]) {
#ifdef _WIN32
  const intptr_t status =
      _spawnvp(_P_WAIT, argv[0], (const char *const *)argv);
  if (status < 0) {
    muon_print_errno(argv[0]);
    return -1;
  }
  if (status != 0) {
    muon_print_error("Command failed: %s\n", argv[0]);
    return -1;
  }
  return 0;
#else
  pid_t pid = fork();
  if (pid < 0) {
    muon_print_errno("fork");
    return -1;
  }
  if (pid == 0) {
    if (g_quiet) {
      const int null_output = open("/dev/null", O_WRONLY);
      if (null_output >= 0) {
        dup2(null_output, STDOUT_FILENO);
        dup2(null_output, STDERR_FILENO);
        if (null_output > STDERR_FILENO) {
          close(null_output);
        }
      }
    }
    execvp(argv[0], argv);
    muon_print_errno(argv[0]);
    _exit(127);
  }
  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    muon_print_errno("waitpid");
    return -1;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    muon_print_error("Command failed: %s\n", argv[0]);
    return -1;
  }
  return 0;
#endif
}

int muon_run_process_with_file_progress(
    char *const argv[], const char *progress_path, unsigned long long total,
    MuonPrepareProgressCallback progress_callback, void *progress_user_data,
    MuonPrepareProgressPhase phase, const char *status) {
  muon_report_progress(progress_callback, progress_user_data, phase, status, 0,
                       total, total != 0);
#ifdef _WIN32
  const intptr_t process_handle_value =
      _spawnvp(_P_NOWAIT, argv[0], (const char *const *)argv);
  if (process_handle_value < 0) {
    muon_print_errno(argv[0]);
    return -1;
  }
  HANDLE process_handle = (HANDLE)process_handle_value;
  for (;;) {
    const DWORD wait_result =
        WaitForSingleObject(process_handle, MUON_PROGRESS_POLL_MILLISECONDS);
    report_file_progress(progress_path, total, progress_callback,
                         progress_user_data, phase, status);
    if (wait_result == WAIT_OBJECT_0) {
      break;
    }
    if (wait_result != WAIT_TIMEOUT) {
      CloseHandle(process_handle);
      muon_print_error("Failed to wait for command: %s\n", argv[0]);
      return -1;
    }
  }
  DWORD exit_code = 0;
  if (!GetExitCodeProcess(process_handle, &exit_code)) {
    CloseHandle(process_handle);
    muon_print_error("Failed to read command exit code: %s\n", argv[0]);
    return -1;
  }
  CloseHandle(process_handle);
  if (exit_code != 0) {
    muon_print_error("Command failed: %s\n", argv[0]);
    return -1;
  }
  return 0;
#else
  pid_t pid = fork();
  if (pid < 0) {
    muon_print_errno("fork");
    return -1;
  }
  if (pid == 0) {
    const int null_output = open("/dev/null", O_WRONLY);
    if (null_output >= 0) {
      dup2(null_output, STDOUT_FILENO);
      dup2(null_output, STDERR_FILENO);
      if (null_output > STDERR_FILENO) {
        close(null_output);
      }
    }
    execvp(argv[0], argv);
    muon_print_errno(argv[0]);
    _exit(127);
  }
  int wait_status = 0;
  for (;;) {
    const pid_t wait_result = waitpid(pid, &wait_status, WNOHANG);
    if (wait_result < 0) {
      muon_print_errno("waitpid");
      return -1;
    }
    if (wait_result == pid) {
      break;
    }
    report_file_progress(progress_path, total, progress_callback,
                         progress_user_data, phase, status);
    wait_for_progress_poll();
  }
  report_file_progress(progress_path, total, progress_callback,
                       progress_user_data, phase, status);
  if (!WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != 0) {
    muon_print_error("Command failed: %s\n", argv[0]);
    return -1;
  }
  return 0;
#endif
}

static void wait_for_lock(void) {
#ifdef _WIN32
  Sleep(100);
#else
  struct timespec delay;
  delay.tv_sec = 0;
  delay.tv_nsec = 100 * 1000 * 1000;
  nanosleep(&delay, NULL);
#endif
}

int muon_acquire_lock_with_progress(
    const char *lock_path, MuonPrepareProgressCallback progress_callback,
    void *progress_user_data, MuonPrepareProgressPhase phase,
    const char *status) {
  char *parent = muon_parent_directory(lock_path);
  if (parent == NULL) {
    return -1;
  }
  if (muon_ensure_directory(parent) != 0) {
    free(parent);
    return -1;
  }
  free(parent);
  while (muon_mkdir(lock_path, 0777) != 0) {
    if (errno != EEXIST) {
      muon_print_errno(lock_path);
      return -1;
    }
    muon_report_progress(progress_callback, progress_user_data, phase, status,
                         0, 0, 0);
    wait_for_lock();
  }
  return 0;
}

int muon_acquire_lock(const char *lock_path) {
  return muon_acquire_lock_with_progress(
      lock_path, NULL, NULL, MUON_PREPARE_PROGRESS_PHASE_INSTALLING, "");
}

void muon_release_lock(const char *lock_path) { muon_rmdir(lock_path); }

char *muon_create_temporary_path(const char *parent, const char *key) {
  const int size = snprintf(NULL, 0, "%s/.%s.tmp.%ld.%ld", parent, key,
                            (long)muon_getpid(), (long)time(NULL));
  if (size < 0) {
    return NULL;
  }
  char *result = (char *)malloc((size_t)size + 1);
  if (result == NULL) {
    return NULL;
  }
  snprintf(result, (size_t)size + 1, "%s/.%s.tmp.%ld.%ld", parent, key,
           (long)muon_getpid(), (long)time(NULL));
  return result;
}

int muon_atomic_replace(const char *source, const char *destination) {
#ifdef _WIN32
  if (!MoveFileExA(source, destination,
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    muon_print_errno(destination);
    return -1;
  }
  return 0;
#else
  if (rename(source, destination) != 0) {
    muon_print_errno(destination);
    return -1;
  }
  return 0;
#endif
}
