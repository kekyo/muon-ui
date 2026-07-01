// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

#ifndef MUON_PREPARE_COMMON_H
#define MUON_PREPARE_COMMON_H

#include <stddef.h>
#include <stdint.h>

#include "prepare_progress.h"
#include "sha1.h"
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

void muon_sha1_update_bytes(SHA1_CTX *context, const void *data, size_t size);
void muon_sha1_update_string(SHA1_CTX *context, const char *value);
void muon_sha1_digest_to_hex(const uint8_t digest[SHA1_DIGEST_LENGTH],
                             char output[SHA1_DIGEST_STRING_LENGTH]);
int muon_sha1_file_hex(const char *path,
                       char output[SHA1_DIGEST_STRING_LENGTH]);
int muon_fingerprint_path_recursive(const char *path, const char *relative,
                                    char fingerprint[SHA1_DIGEST_STRING_LENGTH]);
int muon_fingerprint_directory_contents(const char *path,
                                        char fingerprint[SHA1_DIGEST_STRING_LENGTH]);
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
