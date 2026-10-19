// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

#ifndef MUON_PREPARE_COMMON_H
#define MUON_PREPARE_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>

#ifdef _WIN32
#include "muon_windows_path.h"

/**
 * Converts a UTF-16 string to newly allocated, strict UTF-8.
 * @param value Null-terminated UTF-16 input.
 * @return Owned string to free, or NULL with errno set on failure.
 */
char *muon_utf8_from_wide(const wchar_t *value);

/**
 * Converts a UTF-8 string to newly allocated, strict UTF-16.
 * @param value Null-terminated UTF-8 input.
 * @return Owned string to free, or NULL with errno set on failure.
 */
wchar_t *muon_wide_from_utf8(const char *value);

/**
 * Runs an entry point with owned UTF-8 arguments converted from Windows argv.
 * @param argc Argument count, including the executable name.
 * @param argv Null-terminated UTF-16 argument array from wmain.
 * @param entry Entry point which borrows the converted arguments during the call.
 * @return Entry point result, or EXIT_FAILURE if conversion or allocation fails.
 */
int muon_windows_main(int argc, wchar_t **argv, int (*entry)(int, char **));

/**
 * Resolves a builder path for a Windows filesystem operation.
 * @param path Null-terminated UTF-8 path.
 * @param usage File or tree operation; tree operations preserve child names.
 * @param result Receives an owned path to destroy with muon_windows_path_destroy.
 * @return 0 on success, or -1 with errno and GetLastError set on failure.
 */
int muon_windows_io_path(const char *path, muon_windows_path_usage usage,
                         muon_windows_path *result);
#endif

/**
 * Reads an environment variable without a Windows ANSI-codepage conversion.
 * @param name UTF-8 environment variable name.
 * @param value Receives an owned UTF-8 string to free, or NULL if unset/empty.
 * @return 0 on success, or -1 with errno set on failure and an empty output.
 */
int muon_get_environment(const char *name, char **value);

/** Opaque directory enumeration, released with muon_closedir. */
typedef struct MuonDirectory MuonDirectory;

/**
 * Opens a directory for exact-name enumeration.
 * @param path UTF-8 directory path.
 * @return Owned iterator, or NULL with errno set on failure.
 */
MuonDirectory *muon_opendir(const char *path);

/**
 * Reads the next exact UTF-8 entry name, including dot entries if present.
 * @param directory Open iterator.
 * @return Borrowed name valid until the next read/close, or NULL at end/error.
 * @remarks A read error is retained and returned by muon_closedir.
 */
const char *muon_readdir(MuonDirectory *directory);

/**
 * Closes and frees an iterator, reporting any preceding read failure.
 * @param directory Open iterator.
 * @return 0 on success, or -1 with errno set on read/close failure.
 */
int muon_closedir(MuonDirectory *directory);

/**
 * Builds a path to an entry without normalizing its exact name.
 * @param directory Open iterator whose resolved directory is used as the base.
 * @param name Exact UTF-8 entry name.
 * @return Owned path to free, or NULL on failure.
 */
char *muon_directory_child_path(const MuonDirectory *directory, const char *name);

/**
 * Appends an exact child name to a resolved filesystem parent.
 * @param parent UTF-8 parent path, resolved before appending the child.
 * @param name Exact UTF-8 entry name, without separators.
 * @return Owned path to free, or NULL on failure.
 */
char *muon_path_join_exact(const char *parent, const char *name);

/**
 * Opens a file using UTF-8 paths on every platform.
 * @param path File path.
 * @param mode Standard fopen mode.
 * @return Stream to fclose, or NULL with errno set on failure.
 */
FILE *muon_fopen(const char *path, const char *mode);

/**
 * Reads file metadata with UTF-8 paths and large file sizes.
 * @param path File or directory path.
 * @param entry Receives metadata on success.
 * @return 0 on success, or -1 with errno set on failure.
 */
int muon_stat(const char *path, struct stat *entry);

/**
 * Updates platform file mode bits using a UTF-8 path.
 * @param path File or directory path.
 * @param mode Requested mode bits; Windows uses its CRT read/write permissions.
 * @return 0 on success, or -1 with errno set on failure.
 */
int muon_chmod(const char *path, int mode);

struct archive;
/**
 * Opens an archive input using the platform's Unicode filesystem path API.
 * @param reader Configured libarchive reader, owned and freed by the caller.
 * @param path UTF-8 archive filename, resolved once before opening.
 * @return A libarchive status, with its error message populated on failure.
 * @remarks The reader owns the opened file and closes it during archive_read_free.
 */
int muon_archive_read_open_file(struct archive *reader, const char *path);

/**
 * Renames a filesystem entry with the platform's ordinary rename semantics.
 * @param source Existing UTF-8 path.
 * @param destination Destination UTF-8 path.
 * @return 0 on success, or -1 with errno set on failure.
 */
int muon_rename(const char *source, const char *destination);

#include "prepare_progress.h"
#include "sha1.h"
#include "sha2.h"
#include "yyjson.h"

void muon_set_quiet(int quiet);
void muon_print_error(const char *format, ...);
void muon_print_errno(const char *message);
void muon_log_message(const char *format, ...);
void muon_report_progress(MuonPrepareProgressCallback callback,
                          void *user_data,
                          MuonPrepareProgressPhase phase,
                          const char *status,
                          unsigned long long current,
                          unsigned long long total,
                          int determinate);

void muon_free_string_array(char **values, size_t count);
char *muon_duplicate_string(const char *value);
char *muon_substring(const char *start, size_t length);
void muon_normalize_path_separators(char *path);
char *muon_duplicate_path_string(const char *value);

char *muon_path_join(const char *left, const char *right);
char *muon_path_join3(const char *first, const char *second,
                      const char *third);
char *muon_parent_directory(const char *path);
int muon_path_exists(const char *path);
int muon_ensure_directory(const char *path);
int muon_copy_file(const char *source, const char *destination, int mode);
int muon_copy_file_with_source_mode(const char *source,
                                    const char *destination);
int muon_copy_file_with_source_mode_progress(
    const char *source, const char *destination,
    MuonPrepareProgressCallback progress_callback, void *progress_user_data,
    MuonPrepareProgressPhase phase, const char *status);
int muon_copy_directory_contents(const char *source, const char *destination,
                                 size_t *file_count);
int muon_copy_directory_contents_progress(
    const char *source, const char *destination, size_t *file_count,
    MuonPrepareProgressCallback progress_callback, void *progress_user_data,
    MuonPrepareProgressPhase phase, const char *status);
int muon_remove_recursive(const char *path);
char *muon_read_text_file(const char *path);
int muon_write_text_file(const char *path, const char *content);
int muon_append_text_file(const char *path, const char *content);
int muon_get_file_size(const char *path, unsigned long long *size);
int muon_count_files_recursive(const char *path, size_t *file_count);
unsigned long long muon_current_unix_time(void);

yyjson_doc *muon_json_read_file(const char *path);
char *muon_json_copy_string(yyjson_val *object, const char *key);
int muon_json_get_int(yyjson_val *object, const char *key, int *value);
int muon_json_get_uint64(yyjson_val *object, const char *key,
                         unsigned long long *value);
int muon_json_get_string_array(yyjson_val *object, const char *key,
                               char ***values, size_t *count);

/**
 * Adds bytes to an initialized SHA-256 calculation.
 *
 * @param context Initialized SHA-256 calculation state.
 * @param data Bytes to add.
 * @param size Number of bytes to add.
 */
void muon_sha256_update_bytes(SHA256_CTX *context, const void *data,
                              size_t size);

/**
 * Adds a null-terminated string to an initialized SHA-256 calculation.
 *
 * @param context Initialized SHA-256 calculation state.
 * @param value String whose bytes are added without the null terminator.
 */
void muon_sha256_update_string(SHA256_CTX *context, const char *value);

/**
 * Encodes a SHA-256 digest as lowercase hexadecimal.
 *
 * @param digest SHA-256 digest bytes.
 * @param output Receives 64 hexadecimal characters and a null terminator.
 */
void muon_sha256_digest_to_hex(
    const uint8_t digest[SHA256_DIGEST_LENGTH],
    char output[SHA256_DIGEST_STRING_LENGTH]);

/**
 * Calculates the SHA-256 digest of a file.
 *
 * @param path Path to the file.
 * @param output Receives 64 lowercase hexadecimal characters and a null
 *     terminator.
 * @return 0 on success, or -1 when the file cannot be read.
 */
int muon_sha256_file_hex(const char *path,
                         char output[SHA256_DIGEST_STRING_LENGTH]);

/**
 * Calculates the SHA-1 digest required by the CEF artifact catalog.
 *
 * @param path Path to the CEF artifact archive.
 * @param output Receives 40 lowercase hexadecimal characters and a null
 *     terminator.
 * @return 0 on success, or -1 when the file cannot be read.
 */
int muon_sha1_file_hex(const char *path,
                       char output[SHA1_DIGEST_STRING_LENGTH]);

/**
 * Calculates the recursive SHA-256 fingerprint of a path.
 *
 * @param path Path to fingerprint.
 * @param relative Stable relative path recorded in the fingerprint.
 * @param fingerprint Receives 64 lowercase hexadecimal characters and a null
 *     terminator.
 * @return 0 on success, or -1 when the path cannot be read.
 */
int muon_fingerprint_path_recursive(
    const char *path, const char *relative,
    char fingerprint[SHA256_DIGEST_STRING_LENGTH]);

/**
 * Calculates the recursive SHA-256 fingerprint of directory contents.
 *
 * @param path Directory whose contents are fingerprinted.
 * @param fingerprint Receives 64 lowercase hexadecimal characters and a null
 *     terminator.
 * @return 0 on success, or -1 when the directory cannot be read.
 */
int muon_fingerprint_directory_contents(
    const char *path, char fingerprint[SHA256_DIGEST_STRING_LENGTH]);
char *muon_create_ready_content(const char *muon_fingerprint,
                                const char *cef_fingerprint);
int muon_ready_file_matches(const char *ready_path,
                            const char *expected_content);
int muon_extract_tar_bz2_archive(const char *archive_path,
                                 const char *destination,
                                 int strip_components,
                                 size_t *file_count);
int muon_extract_tar_bz2_archive_progress(
    const char *archive_path, const char *destination, int strip_components,
    size_t *file_count, MuonPrepareProgressCallback progress_callback,
    void *progress_user_data, MuonPrepareProgressPhase phase,
    const char *status);
char *muon_read_tar_bz2_text_file(const char *archive_path,
                                  const char *relative_path,
                                  int strip_components);

int muon_run_process(char *const argv[]);
int muon_run_process_allow_failure(char *const argv[]);
int muon_run_process_with_file_progress(
    char *const argv[], const char *progress_path, unsigned long long total,
    MuonPrepareProgressCallback progress_callback, void *progress_user_data,
    MuonPrepareProgressPhase phase, const char *status);
int muon_run_process_with_file_progress_allow_failure(
    char *const argv[], const char *progress_path, unsigned long long total,
    MuonPrepareProgressCallback progress_callback, void *progress_user_data,
    MuonPrepareProgressPhase phase, const char *status);
int muon_acquire_lock(const char *lock_path);
int muon_acquire_lock_with_progress(
    const char *lock_path, MuonPrepareProgressCallback progress_callback,
    void *progress_user_data, MuonPrepareProgressPhase phase,
    const char *status);
void muon_release_lock(const char *lock_path);
char *muon_create_temporary_path(const char *parent, const char *key);
int muon_atomic_replace(const char *source, const char *destination);

#endif
