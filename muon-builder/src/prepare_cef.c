// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <strings.h>
#else
#define strcasecmp _stricmp
#endif

#include "prepare_cef.h"
#include "common.h"

static const char *kDefaultCefCatalogUrl =
    "https://cef-builds.spotifycdn.com/index.json";
static const char *kEmptyFingerprint =
    "0000000000000000000000000000000000000000000000000000000000000000";
static const int kCurlDownloadAttemptCount = 4;

static const char *display_target_from_cef_target(const char *target) {
  if (target == NULL) {
    return "(null)";
  }
  if (strcmp(target, "linux64") == 0) {
    return "linux-amd64";
  }
  if (strcmp(target, "linuxarm") == 0) {
    return "linux-armhf";
  }
  if (strcmp(target, "linuxarm64") == 0) {
    return "linux-arm64";
  }
  if (strcmp(target, "windows32") == 0) {
    return "windows-i686";
  }
  if (strcmp(target, "windows64") == 0) {
    return "windows-amd64";
  }
  return target;
}

void muon_prepare_set_cef_quiet(int quiet) { muon_set_quiet(quiet); }

static int verify_sha1(const char *path, const char *expected) {
  char actual[SHA1_DIGEST_STRING_LENGTH];
  if (muon_sha1_file_hex(path, actual) != 0) {
    return -1;
  }
  if (strcasecmp(actual, expected) != 0) {
    muon_print_error("CEF archive sha1 mismatch: expected %s, got %s\n", expected,
                actual);
    return -1;
  }
  return 0;
}

static int verify_size(const char *path, unsigned long long expected) {
  unsigned long long actual = 0;
  if (muon_get_file_size(path, &actual) != 0) {
    return -1;
  }
  if (actual != expected) {
    muon_print_error("CEF archive size mismatch: expected %llu, got %llu\n",
                     expected, actual);
    return -1;
  }
  return 0;
}

static int file_size_equals(const char *path, unsigned long long expected) {
  if (!muon_path_exists(path)) {
    return 0;
  }
  unsigned long long actual = 0;
  return muon_get_file_size(path, &actual) == 0 && actual == expected;
}

static int file_has_download_progress(const char *path) {
  if (!muon_path_exists(path)) {
    return 0;
  }
  unsigned long long size = 0;
  return muon_get_file_size(path, &size) == 0 && size > 0;
}

static int run_curl_download(const char *url, const char *destination,
                             const char *curl_flags,
                             unsigned long long expected_size,
                             const char *status,
                             MuonPrepareProgressCallback progress_callback,
                             void *progress_user_data) {
  for (int attempt = 0; attempt < kCurlDownloadAttemptCount; attempt += 1) {
    const int final_attempt = attempt + 1 == kCurlDownloadAttemptCount;
    const int resume = attempt > 0 && file_has_download_progress(destination);
    char *fresh_argv[] = {"curl", (char *)curl_flags, "-o",
                          (char *)destination, (char *)url, NULL};
    char *resume_argv[] = {"curl", (char *)curl_flags, "-C", "-",
                           "-o",   (char *)destination, (char *)url, NULL};
    char **argv = resume ? resume_argv : fresh_argv;
    if (attempt > 0) {
      muon_print_error("Retrying download: attempt %d/%d\n", attempt + 1,
                       kCurlDownloadAttemptCount);
    }
    const int result =
        progress_callback == NULL
            ? (final_attempt ? muon_run_process(argv)
                             : muon_run_process_allow_failure(argv))
            : (final_attempt
                   ? muon_run_process_with_file_progress(
                         argv, destination, expected_size, progress_callback,
                         progress_user_data,
                         MUON_PREPARE_PROGRESS_PHASE_DOWNLOADING, status)
                   : muon_run_process_with_file_progress_allow_failure(
                         argv, destination, expected_size, progress_callback,
                         progress_user_data,
                         MUON_PREPARE_PROGRESS_PHASE_DOWNLOADING, status));
    if (result == 0 ||
        (expected_size != 0 && file_size_equals(destination, expected_size))) {
      return 0;
    }
    if (resume) {
      muon_remove_recursive(destination);
    }
  }
  return -1;
}

static int copy_url_to_file(const char *url, const char *destination) {
  if (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0) {
    return run_curl_download(url, destination, "-fL", 0, NULL, NULL, NULL);
  }
  const char *source = url;
  if (strncmp(url, "file://", 7) == 0) {
    source = url + 7;
  }
  return muon_copy_file_with_source_mode(source, destination);
}

static int copy_url_to_file_progress(
    const char *url, const char *destination, unsigned long long size,
    const char *status,
    MuonPrepareProgressCallback progress_callback, void *progress_user_data) {
  if (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0) {
    return run_curl_download(url, destination, "-fsSL", size, status,
                             progress_callback, progress_user_data);
  }
  const char *source = url;
  if (strncmp(url, "file://", 7) == 0) {
    source = url + 7;
  }
  muon_report_progress(progress_callback, progress_user_data,
                       MUON_PREPARE_PROGRESS_PHASE_DOWNLOADING, status, 0,
                       size, size != 0);
  return muon_copy_file_with_source_mode_progress(
      source, destination, progress_callback, progress_user_data,
      MUON_PREPARE_PROGRESS_PHASE_DOWNLOADING, status);
}

static const char *get_catalog_url(void) {
  const char *url = getenv("MUON_CEF_CATALOG_URL");
  return url == NULL || url[0] == '\0' ? kDefaultCefCatalogUrl : url;
}

static char *url_encode_artifact_name(const char *file_name) {
  size_t size = 1;
  for (const char *cursor = file_name; *cursor != '\0'; cursor += 1) {
    size += *cursor == '+' ? 3 : 1;
  }
  char *result = (char *)malloc(size);
  if (result == NULL) {
    return NULL;
  }
  char *output = result;
  for (const char *cursor = file_name; *cursor != '\0'; cursor += 1) {
    if (*cursor == '+') {
      memcpy(output, "%2B", 3);
      output += 3;
    } else {
      *output++ = *cursor;
    }
  }
  *output = '\0';
  return result;
}

static char *resolve_artifact_url(const char *file_name) {
  const char *catalog_url = get_catalog_url();
  const char *slash = strrchr(catalog_url, '/');
#ifdef _WIN32
  const char *backslash = strrchr(catalog_url, '\\');
  if (backslash != NULL && (slash == NULL || backslash > slash)) {
    slash = backslash;
  }
#endif
  const size_t base_size =
      slash == NULL ? 0 : (size_t)(slash + 1 - catalog_url);
  const int is_http = strncmp(catalog_url, "http://", 7) == 0 ||
                      strncmp(catalog_url, "https://", 8) == 0;
  char *artifact_name =
      is_http ? url_encode_artifact_name(file_name) : muon_duplicate_string(file_name);
  if (artifact_name == NULL) {
    return NULL;
  }
  char *result = (char *)malloc(base_size + strlen(artifact_name) + 1);
  if (result == NULL) {
    free(artifact_name);
    return NULL;
  }
  if (base_size != 0) {
    memcpy(result, catalog_url, base_size);
  }
  memcpy(result + base_size, artifact_name, strlen(artifact_name) + 1);
  free(artifact_name);
  return result;
}

void muon_prepare_free_cef_artifact(MuonCefArtifact *artifact) {
  if (artifact == NULL) {
    return;
  }
  free(artifact->version);
  free(artifact->target);
  free(artifact->distribution);
  free(artifact->file_name);
  free(artifact->url);
  free(artifact->sha1);
  memset(artifact, 0, sizeof(*artifact));
}

int muon_prepare_ensure_catalog_cache(const char *cache_dir, int force) {
  return muon_prepare_ensure_catalog_cache_with_status(cache_dir, force, NULL);
}

int muon_prepare_ensure_catalog_cache_with_status(const char *cache_dir,
                                                  int force,
                                                  int *updated) {
  return muon_prepare_ensure_catalog_cache_with_status_progress(
      cache_dir, force, updated, NULL, NULL);
}

int muon_prepare_ensure_catalog_cache_with_status_progress(
    const char *cache_dir, int force, int *updated,
    MuonPrepareProgressCallback progress_callback, void *progress_user_data) {
  if (updated != NULL) {
    *updated = 0;
  }
  if (muon_ensure_directory(cache_dir) != 0) {
    return -1;
  }
  char *catalog_path = muon_path_join(cache_dir, "catalog.json");
  char *temporary_path = muon_create_temporary_path(cache_dir, "catalog.json");
  if (catalog_path == NULL || temporary_path == NULL) {
    free(catalog_path);
    free(temporary_path);
    return -1;
  }
  if (!force && muon_path_exists(catalog_path)) {
    free(catalog_path);
    free(temporary_path);
    return 0;
  }
  int result = 0;
  const int copy_result =
      progress_callback == NULL
          ? copy_url_to_file(get_catalog_url(), temporary_path)
          : copy_url_to_file_progress(get_catalog_url(), temporary_path, 0,
                                      "Checking for CEF updates...",
                                      progress_callback, progress_user_data);
  if (copy_result == 0) {
    result = muon_atomic_replace(temporary_path, catalog_path);
    if (result == 0 && updated != NULL) {
      *updated = 1;
    }
    if (result != 0) {
      muon_remove_recursive(temporary_path);
    }
  } else if (muon_path_exists(catalog_path)) {
    muon_remove_recursive(temporary_path);
    result = 0;
  } else {
    muon_remove_recursive(temporary_path);
    result = -1;
  }
  free(catalog_path);
  free(temporary_path);
  return result;
}

static int set_artifact_from_reference(const MuonCefReference *reference,
                                       MuonCefArtifact *artifact) {
  memset(artifact, 0, sizeof(*artifact));
  artifact->version = muon_duplicate_string(reference->version);
  artifact->target = muon_duplicate_string(reference->target);
  artifact->distribution = muon_duplicate_string(reference->distribution);
  artifact->file_name = muon_duplicate_string(reference->artifact_file_name);
  artifact->url = muon_duplicate_string(reference->artifact_url);
  artifact->sha1 = muon_duplicate_string(reference->artifact_sha1);
  artifact->size = reference->artifact_size;
  if (artifact->version == NULL || artifact->target == NULL ||
      artifact->distribution == NULL || artifact->file_name == NULL ||
      artifact->url == NULL || artifact->sha1 == NULL) {
    muon_prepare_free_cef_artifact(artifact);
    return -1;
  }
  return 0;
}

static int set_artifact_from_file(const char *version, const char *target,
                                  const char *distribution,
                                  yyjson_val *file_object,
                                  MuonCefArtifact *artifact) {
  char *file_name = muon_json_copy_string(file_object, "name");
  char *sha1 = muon_json_copy_string(file_object, "sha1");
  unsigned long long size = 0;
  char *url = file_name == NULL ? NULL : resolve_artifact_url(file_name);
  if (file_name == NULL || sha1 == NULL || url == NULL ||
      muon_json_get_uint64(file_object, "size", &size) != 0) {
    free(file_name);
    free(sha1);
    free(url);
    return -1;
  }
  artifact->version = muon_duplicate_string(version);
  artifact->target = muon_duplicate_string(target);
  artifact->distribution = muon_duplicate_string(distribution);
  artifact->file_name = file_name;
  artifact->url = url;
  artifact->sha1 = sha1;
  artifact->size = size;
  if (artifact->version == NULL || artifact->target == NULL ||
      artifact->distribution == NULL) {
    muon_prepare_free_cef_artifact(artifact);
    return -1;
  }
  return 0;
}

int muon_prepare_resolve_cef_artifact(const char *cache_dir,
                                      const char *version,
                                      const char *target,
                                      const char *distribution,
                                      MuonCefArtifact *artifact) {
  memset(artifact, 0, sizeof(*artifact));
  char *catalog_path = muon_path_join(cache_dir, "catalog.json");
  yyjson_doc *catalog =
      catalog_path == NULL ? NULL : muon_json_read_file(catalog_path);
  yyjson_val *root = catalog == NULL ? NULL : yyjson_doc_get_root(catalog);
  yyjson_val *target_object =
      root == NULL ? NULL : yyjson_obj_get(root, target);
  yyjson_val *versions =
      target_object == NULL ? NULL : yyjson_obj_get(target_object, "versions");
  if (catalog_path == NULL || catalog == NULL || target_object == NULL ||
      versions == NULL || !yyjson_is_arr(versions)) {
    muon_print_error("CEF catalog does not contain target: %s\n",
                     display_target_from_cef_target(target));
    free(catalog_path);
    yyjson_doc_free(catalog);
    return -1;
  }
  int result = -1;
  size_t version_index = 0;
  size_t version_max = 0;
  yyjson_val *version_object = NULL;
  yyjson_arr_foreach(versions, version_index, version_max, version_object) {
    yyjson_val *cef_version_value =
        yyjson_obj_get(version_object, "cef_version");
    const char *cef_version =
        yyjson_is_str(cef_version_value) ? yyjson_get_str(cef_version_value)
                                         : NULL;
    if (cef_version != NULL && strcmp(cef_version, version) == 0) {
      yyjson_val *files = yyjson_obj_get(version_object, "files");
      size_t file_index = 0;
      size_t file_max = 0;
      yyjson_val *file_object = NULL;
      yyjson_arr_foreach(files, file_index, file_max, file_object) {
        yyjson_val *type_value = yyjson_obj_get(file_object, "type");
        const char *type =
            yyjson_is_str(type_value) ? yyjson_get_str(type_value) : NULL;
        if (type != NULL && strcmp(type, distribution) == 0) {
          result = set_artifact_from_file(version, target, distribution,
                                          file_object, artifact);
          break;
        }
      }
    }
    if (result == 0) {
      break;
    }
  }
  if (result != 0) {
    muon_print_error("CEF catalog does not contain %s %s %s artifact.\n",
                     version, display_target_from_cef_target(target),
                     distribution);
  }
  free(catalog_path);
  yyjson_doc_free(catalog);
  return result;
}

typedef struct {
  MuonCefArtifact artifact;
  char *chromium_version;
  char *last_modified;
} MuonCefCandidate;

typedef struct {
  MuonCefCandidate *values;
  size_t count;
  size_t capacity;
} MuonCefCandidateList;

static void free_candidate(MuonCefCandidate *candidate) {
  muon_prepare_free_cef_artifact(&candidate->artifact);
  free(candidate->chromium_version);
  free(candidate->last_modified);
  memset(candidate, 0, sizeof(*candidate));
}

static void free_candidate_list(MuonCefCandidateList *list) {
  if (list == NULL) {
    return;
  }
  for (size_t index = 0; index < list->count; index += 1) {
    free_candidate(&list->values[index]);
  }
  free(list->values);
  memset(list, 0, sizeof(*list));
}

static int candidate_list_add(MuonCefCandidateList *list,
                              const char *version,
                              const char *target,
                              const char *distribution,
                              const char *chromium_version,
                              yyjson_val *file_object) {
  if (list->count == list->capacity) {
    const size_t next_capacity = list->capacity == 0 ? 16 : list->capacity * 2;
    MuonCefCandidate *next = (MuonCefCandidate *)realloc(
        list->values, sizeof(MuonCefCandidate) * next_capacity);
    if (next == NULL) {
      return -1;
    }
    memset(next + list->capacity, 0,
           sizeof(MuonCefCandidate) * (next_capacity - list->capacity));
    list->values = next;
    list->capacity = next_capacity;
  }
  MuonCefCandidate *candidate = &list->values[list->count];
  memset(candidate, 0, sizeof(*candidate));
  if (set_artifact_from_file(version, target, distribution, file_object,
                             &candidate->artifact) != 0) {
    return -1;
  }
  yyjson_val *last_modified_value = yyjson_obj_get(file_object, "last_modified");
  const char *last_modified = yyjson_is_str(last_modified_value)
                                  ? yyjson_get_str(last_modified_value)
                                  : "";
  candidate->chromium_version = muon_duplicate_string(chromium_version);
  candidate->last_modified = muon_duplicate_string(last_modified);
  if (candidate->chromium_version == NULL ||
      candidate->last_modified == NULL) {
    free_candidate(candidate);
    return -1;
  }
  list->count += 1;
  return 0;
}

static char *copy_major_version(const char *version) {
  const char *dot = strchr(version, '.');
  if (dot == NULL) {
    return muon_duplicate_string(version);
  }
  return muon_substring(version, (size_t)(dot - version));
}

static int same_major_version(const char *left, const char *right) {
  char *left_major = copy_major_version(left);
  char *right_major = copy_major_version(right);
  const int result = left_major != NULL && right_major != NULL &&
                     strcmp(left_major, right_major) == 0;
  free(left_major);
  free(right_major);
  return result;
}

static unsigned long long read_version_number(const char **cursor) {
  while (**cursor != '\0' && !isdigit((unsigned char)**cursor)) {
    *cursor += 1;
  }
  unsigned long long result = 0;
  while (isdigit((unsigned char)**cursor)) {
    result = result * 10ULL + (unsigned long long)(**cursor - '0');
    *cursor += 1;
  }
  return result;
}

static int compare_numbered_versions(const char *left, const char *right) {
  const char *left_cursor = left;
  const char *right_cursor = right;
  while (*left_cursor != '\0' || *right_cursor != '\0') {
    const unsigned long long left_number = read_version_number(&left_cursor);
    const unsigned long long right_number = read_version_number(&right_cursor);
    if (left_number < right_number) {
      return -1;
    }
    if (left_number > right_number) {
      return 1;
    }
    if (*left_cursor != '\0') {
      left_cursor += 1;
    }
    if (*right_cursor != '\0') {
      right_cursor += 1;
    }
  }
  return strcmp(left, right);
}

static int compare_candidates_newest_first(const void *left,
                                           const void *right) {
  const MuonCefCandidate *left_candidate = (const MuonCefCandidate *)left;
  const MuonCefCandidate *right_candidate = (const MuonCefCandidate *)right;
  int comparison = compare_numbered_versions(left_candidate->chromium_version,
                                             right_candidate->chromium_version);
  if (comparison != 0) {
    return -comparison;
  }
  comparison = compare_numbered_versions(left_candidate->artifact.version,
                                         right_candidate->artifact.version);
  if (comparison != 0) {
    return -comparison;
  }
  return -strcmp(left_candidate->last_modified,
                 right_candidate->last_modified);
}

static int collect_policy_candidates(const char *cache_dir,
                                     const MuonCefReference *reference,
                                     int require_same_major,
                                     MuonCefCandidateList *list) {
  memset(list, 0, sizeof(*list));
  char *catalog_path = muon_path_join(cache_dir, "catalog.json");
  yyjson_doc *catalog =
      catalog_path == NULL ? NULL : muon_json_read_file(catalog_path);
  yyjson_val *root = catalog == NULL ? NULL : yyjson_doc_get_root(catalog);
  yyjson_val *target_object =
      root == NULL ? NULL : yyjson_obj_get(root, reference->target);
  yyjson_val *versions =
      target_object == NULL ? NULL : yyjson_obj_get(target_object, "versions");
  if (catalog_path == NULL || catalog == NULL || target_object == NULL ||
      versions == NULL || !yyjson_is_arr(versions)) {
    free(catalog_path);
    yyjson_doc_free(catalog);
    return -1;
  }
  int result = 0;
  size_t version_index = 0;
  size_t version_max = 0;
  yyjson_val *version_object = NULL;
  yyjson_arr_foreach(versions, version_index, version_max, version_object) {
    yyjson_val *cef_version_value =
        yyjson_obj_get(version_object, "cef_version");
    yyjson_val *channel_value = yyjson_obj_get(version_object, "channel");
    yyjson_val *chromium_value =
        yyjson_obj_get(version_object, "chromium_version");
    const char *cef_version =
        yyjson_is_str(cef_version_value) ? yyjson_get_str(cef_version_value)
                                         : NULL;
    const char *channel =
        yyjson_is_str(channel_value) ? yyjson_get_str(channel_value) : NULL;
    const char *chromium_version = yyjson_is_str(chromium_value)
                                       ? yyjson_get_str(chromium_value)
                                       : "";
    if (cef_version == NULL || channel == NULL ||
        strcmp(channel, "stable") != 0 ||
        (require_same_major &&
         !same_major_version(cef_version, reference->version))) {
      continue;
    }
    yyjson_val *files = yyjson_obj_get(version_object, "files");
    if (files == NULL || !yyjson_is_arr(files)) {
      continue;
    }
    size_t file_index = 0;
    size_t file_max = 0;
    yyjson_val *file_object = NULL;
    yyjson_arr_foreach(files, file_index, file_max, file_object) {
      yyjson_val *type_value = yyjson_obj_get(file_object, "type");
      const char *type =
          yyjson_is_str(type_value) ? yyjson_get_str(type_value) : NULL;
      if (type != NULL && strcmp(type, reference->distribution) == 0 &&
          candidate_list_add(list, cef_version, reference->target,
                             reference->distribution, chromium_version,
                             file_object) != 0) {
        result = -1;
        break;
      }
    }
    if (result != 0) {
      break;
    }
  }
  if (result == 0 && list->count > 1) {
    qsort(list->values, list->count, sizeof(MuonCefCandidate),
          compare_candidates_newest_first);
  }
  free(catalog_path);
  yyjson_doc_free(catalog);
  if (result != 0) {
    free_candidate_list(list);
  }
  return result;
}

static char *extract_platform_api_hash(const char *content,
                                       int api_version,
                                       const char *target) {
  char pattern[64];
  snprintf(pattern, sizeof(pattern), "CEF_API_HASH_%d", api_version);
  const char *line = strstr(content, pattern);
  if (line == NULL) {
    return NULL;
  }
  const char *line_end = strchr(line, '\n');
  if (line_end == NULL) {
    line_end = line + strlen(line);
  }
  const int hash_index = strncmp(target, "windows", 7) == 0 ? 0 : 2;
  int current_index = 0;
  for (const char *cursor = line; cursor < line_end; cursor += 1) {
    if (*cursor != '"') {
      continue;
    }
    const char *start = cursor + 1;
    const char *end = strchr(start, '"');
    if (end == NULL || end > line_end) {
      return NULL;
    }
    if (current_index == hash_index) {
      return muon_substring(start, (size_t)(end - start));
    }
    current_index += 1;
    cursor = end;
  }
  return NULL;
}

static int archive_matches_reference_api(const char *archive_path,
                                         const MuonCefReference *reference) {
  char *api_versions = muon_read_tar_bz2_text_file(
      archive_path, "include/cef_api_versions.h", 1);
  if (api_versions == NULL) {
    muon_print_error("CEF archive does not contain cef_api_versions.h: %s\n",
                     archive_path);
    return 0;
  }
  char *api_hash =
      extract_platform_api_hash(api_versions, reference->api_version,
                                reference->target);
  free(api_versions);
  if (api_hash == NULL) {
    muon_print_error("CEF archive does not contain API hash: version=%d\n",
                     reference->api_version);
    return 0;
  }
  const int matches = strcmp(api_hash, reference->api_hash) == 0;
  if (!matches) {
    muon_print_error("CEF archive API hash mismatch: expected %s, got %s\n",
                     reference->api_hash, api_hash);
  }
  free(api_hash);
  return matches;
}

static int ensure_tested_archive(const char *cache_dir,
                                 const MuonCefReference *reference,
                                 int force,
                                 MuonCefArtifact *artifact,
                                 char **archive_path,
                                 MuonPrepareProgressCallback progress_callback,
                                 void *progress_user_data) {
  if (set_artifact_from_reference(reference, artifact) != 0) {
    return -1;
  }
  if (muon_prepare_ensure_cef_artifact_cache_progress(
          cache_dir, artifact, force, archive_path, progress_callback,
          progress_user_data) != 0) {
    muon_prepare_free_cef_artifact(artifact);
    return -1;
  }
  return 0;
}

static int ensure_latest_policy_archive(const char *cache_dir,
                                        const MuonCefReference *reference,
                                        int require_same_major,
                                        int force,
                                        MuonCefArtifact *artifact,
                                        char **archive_path,
                                        MuonPrepareProgressCallback progress_callback,
                                        void *progress_user_data) {
  MuonCefCandidateList candidates;
  if (collect_policy_candidates(cache_dir, reference, require_same_major,
                                &candidates) != 0) {
    return ensure_tested_archive(cache_dir, reference, force, artifact,
                                 archive_path, progress_callback,
                                 progress_user_data);
  }
  for (size_t index = 0; index < candidates.count; index += 1) {
    MuonCefCandidate *candidate = &candidates.values[index];
    char *candidate_archive_path = NULL;
    if (muon_prepare_ensure_cef_artifact_cache_progress(
            cache_dir, &candidate->artifact, force, &candidate_archive_path,
            progress_callback, progress_user_data) != 0) {
      free(candidate_archive_path);
      continue;
    }
    if (archive_matches_reference_api(candidate_archive_path, reference)) {
      *artifact = candidate->artifact;
      memset(&candidate->artifact, 0, sizeof(candidate->artifact));
      *archive_path = candidate_archive_path;
      free_candidate_list(&candidates);
      return 0;
    }
    free(candidate_archive_path);
  }
  free_candidate_list(&candidates);
  return ensure_tested_archive(cache_dir, reference, force, artifact,
                               archive_path, progress_callback,
                               progress_user_data);
}

int muon_prepare_ensure_cef_archive_cache_for_policy(
    const char *cache_dir, const MuonCefReference *reference,
    const char *policy, const char *exact_version, int force,
    MuonCefArtifact *artifact, char **archive_path) {
  return muon_prepare_ensure_cef_archive_cache_for_policy_progress(
      cache_dir, reference, policy, exact_version, force, artifact,
      archive_path, NULL, NULL);
}

int muon_prepare_ensure_cef_archive_cache_for_policy_progress(
    const char *cache_dir, const MuonCefReference *reference,
    const char *policy, const char *exact_version, int force,
    MuonCefArtifact *artifact, char **archive_path,
    MuonPrepareProgressCallback progress_callback, void *progress_user_data) {
  memset(artifact, 0, sizeof(*artifact));
  *archive_path = NULL;
  if (strcmp(policy, "tested") == 0) {
    return ensure_tested_archive(cache_dir, reference, force, artifact,
                                 archive_path, progress_callback,
                                 progress_user_data);
  }
  if (strcmp(policy, "same-major-latest") == 0) {
    return ensure_latest_policy_archive(cache_dir, reference, 1, force,
                                        artifact, archive_path,
                                        progress_callback, progress_user_data);
  }
  if (strcmp(policy, "compat-latest") == 0) {
    return ensure_latest_policy_archive(cache_dir, reference, 0, force,
                                        artifact, archive_path,
                                        progress_callback, progress_user_data);
  }
  if (strcmp(policy, "exact") == 0) {
    if (exact_version == NULL || exact_version[0] == '\0') {
      muon_print_error("exactVersion is required for exact CEF policy.\n");
      return -1;
    }
    if (strcmp(exact_version, reference->version) == 0) {
      return ensure_tested_archive(cache_dir, reference, force, artifact,
                                   archive_path, progress_callback,
                                   progress_user_data);
    }
    if (muon_prepare_resolve_cef_artifact(cache_dir, exact_version,
                                          reference->target,
                                          reference->distribution,
                                          artifact) != 0) {
      return -1;
    }
    if (muon_prepare_ensure_cef_artifact_cache_progress(
            cache_dir, artifact, force, archive_path, progress_callback,
            progress_user_data) != 0) {
      muon_prepare_free_cef_artifact(artifact);
      return -1;
    }
    if (!archive_matches_reference_api(*archive_path, reference)) {
      free(*archive_path);
      *archive_path = NULL;
      muon_prepare_free_cef_artifact(artifact);
      return -1;
    }
    return 0;
  }
  muon_print_error("Invalid CEF version policy: %s\n", policy);
  return -1;
}

int muon_prepare_ensure_cef_artifact_cache(
    const char *cache_dir, const MuonCefArtifact *artifact, int force,
    char **archive_path) {
  return muon_prepare_ensure_cef_artifact_cache_progress(
      cache_dir, artifact, force, archive_path, NULL, NULL);
}

int muon_prepare_ensure_cef_artifact_cache_progress(
    const char *cache_dir, const MuonCefArtifact *artifact, int force,
    char **archive_path, MuonPrepareProgressCallback progress_callback,
    void *progress_user_data) {
  *archive_path = NULL;
  char *artifacts_dir = muon_path_join(cache_dir, "artifacts");
  char *final_path =
      artifacts_dir == NULL ? NULL : muon_path_join(artifacts_dir, artifact->file_name);
  char *temporary_path =
      artifacts_dir == NULL
          ? NULL
          : muon_create_temporary_path(artifacts_dir, artifact->file_name);
  if (artifacts_dir == NULL || final_path == NULL || temporary_path == NULL) {
    free(artifacts_dir);
    free(final_path);
    free(temporary_path);
    return -1;
  }
  if (muon_ensure_directory(artifacts_dir) != 0) {
    free(artifacts_dir);
    free(final_path);
    free(temporary_path);
    return -1;
  }
  if (!force && muon_path_exists(final_path) &&
      verify_sha1(final_path, artifact->sha1) == 0 &&
      verify_size(final_path, artifact->size) == 0) {
    *archive_path = final_path;
    free(artifacts_dir);
    free(temporary_path);
    return 0;
  }
  muon_print_error("Downloading CEF binary: version=%s target=%s distribution=%s\n",
              artifact->version,
              display_target_from_cef_target(artifact->target),
              artifact->distribution);
  int result = -1;
  const int copy_result =
      progress_callback == NULL
          ? copy_url_to_file(artifact->url, temporary_path)
          : copy_url_to_file_progress(artifact->url, temporary_path,
                                      artifact->size,
                                      "Downloading CEF runtime...",
                                      progress_callback, progress_user_data);
  if (copy_result == 0 &&
      (muon_report_progress(progress_callback, progress_user_data,
                            MUON_PREPARE_PROGRESS_PHASE_VERIFYING,
                            "Verifying download...", 0, 0, 0),
       verify_sha1(temporary_path, artifact->sha1)) == 0 &&
      verify_size(temporary_path, artifact->size) == 0 &&
      muon_atomic_replace(temporary_path, final_path) == 0) {
    muon_print_error("CEF binary downloaded: version=%s target=%s distribution=%s\n",
                artifact->version,
                display_target_from_cef_target(artifact->target),
                artifact->distribution);
    *archive_path = final_path;
    result = 0;
  } else {
    muon_remove_recursive(temporary_path);
  }
  free(artifacts_dir);
  free(temporary_path);
  if (result != 0) {
    free(final_path);
  }
  return result;
}

static int count_cef_output_files(const char *output_dir, size_t *file_count) {
  if (muon_count_files_recursive(output_dir, file_count) != 0) {
    return -1;
  }
  char *ready_path = muon_path_join(output_dir, ".muon-cef-ready.json");
  if (ready_path == NULL) {
    return -1;
  }
  if (muon_path_exists(ready_path) && *file_count > 0) {
    *file_count -= 1;
  }
  free(ready_path);
  return 0;
}

int muon_prepare_extract_cef_archive_full(const char *archive_path,
                                          const char *output_dir, int force,
                                          size_t *file_count) {
  *file_count = 0;
  char cef_fingerprint[SHA256_DIGEST_STRING_LENGTH];
  if (muon_fingerprint_path_recursive(archive_path, "", cef_fingerprint) != 0) {
    return -1;
  }
  char *ready_content =
      muon_create_ready_content(kEmptyFingerprint, cef_fingerprint);
  char *ready_path = muon_path_join(output_dir, ".muon-cef-ready.json");
  if (ready_content == NULL || ready_path == NULL) {
    free(ready_content);
    free(ready_path);
    return -1;
  }
  if (!force && muon_path_exists(ready_path) &&
      muon_ready_file_matches(ready_path, ready_content)) {
    const int result = count_cef_output_files(output_dir, file_count);
    free(ready_content);
    free(ready_path);
    return result;
  }
  char *parent = muon_parent_directory(output_dir);
  char *temporary_directory =
      parent == NULL ? NULL : muon_create_temporary_path(parent, "cef-full");
  char *temporary_ready =
      temporary_directory == NULL
          ? NULL
          : muon_path_join(temporary_directory, ".muon-cef-ready.json");
  if (parent == NULL || temporary_directory == NULL ||
      temporary_ready == NULL ||
      muon_ensure_directory(parent) != 0 ||
      muon_ensure_directory(temporary_directory) != 0) {
    free(parent);
    free(temporary_directory);
    free(temporary_ready);
    free(ready_content);
    free(ready_path);
    return -1;
  }
  if (muon_extract_tar_bz2_archive(archive_path, temporary_directory, 1,
                                   file_count) != 0 ||
      muon_write_text_file(temporary_ready, ready_content) != 0) {
    muon_remove_recursive(temporary_directory);
    free(parent);
    free(temporary_directory);
    free(temporary_ready);
    free(ready_content);
    free(ready_path);
    return -1;
  }
  if (muon_path_exists(output_dir) && muon_remove_recursive(output_dir) != 0) {
    muon_remove_recursive(temporary_directory);
    free(parent);
    free(temporary_directory);
    free(temporary_ready);
    free(ready_content);
    free(ready_path);
    return -1;
  }
  if (rename(temporary_directory, output_dir) != 0) {
    muon_print_errno(output_dir);
    muon_remove_recursive(temporary_directory);
    free(parent);
    free(temporary_directory);
    free(temporary_ready);
    free(ready_content);
    free(ready_path);
    return -1;
  }
  free(parent);
  free(temporary_directory);
  free(temporary_ready);
  free(ready_content);
  free(ready_path);
  return 0;
}
