// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

#include <ctype.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "prepare.h"
#include "prepare_cef.h"
#include "bootstrap_config.h"
#include "common.h"
#include "muon_runtime_info_generated.h"

#if defined(__has_include)
#if __has_include("version.h")
#include "version.h"
#endif
#endif

#ifndef MUON_BUILDER_VERSION
#define MUON_BUILDER_VERSION "0.0.0"
#endif

#ifndef MUON_BUILDER_GIT_COMMIT_HASH
#define MUON_BUILDER_GIT_COMMIT_HASH "unknown"
#endif

typedef struct {
  char *muon_path;
  char *cef_path;
  char *stage_dir;
  char *output_dir;
  char *target;
  char *cef_version;
  char *cef_version_policy;
  int has_cef_version_policy;
  char *cef_exact_version;
  int has_cef_exact_version;
  char *bootstrap_config_dir;
  char *cache_dir;
  unsigned long long catalog_refresh_interval_seconds;
  int has_catalog_refresh_interval_seconds;
  unsigned long long last_catalog_update_unix;
  int update_requested;
  unsigned long long update_requested_at_unix;
  int write_bootstrap_config;
  int catalog_updated;
  int force;
  int quiet;
  int json;
  MuonPrepareProgressCallback progress_callback;
  void *progress_user_data;
  int progress_emitted;
} PrepareOptions;

typedef struct {
  char *stage_path;
  char *muon_path;
  char *cef_path;
  int cache_hit;
} PrepareResult;

typedef struct {
  char **values;
  size_t count;
  size_t capacity;
} PrepareNameList;

static const char *kEmptyFingerprint = "0000000000000000000000000000000000000000";

static const char *muon_cef_target_from_public_target(const char *target) {
  if (target == NULL) {
    return NULL;
  }
  if (strcmp(target, "linux-amd64") == 0) {
    return "linux64";
  }
  if (strcmp(target, "linux-armhf") == 0) {
    return "linuxarm";
  }
  if (strcmp(target, "linux-arm64") == 0) {
    return "linuxarm64";
  }
  if (strcmp(target, "windows-i686") == 0) {
    return "windows32";
  }
  if (strcmp(target, "windows-amd64") == 0) {
    return "windows64";
  }
  return NULL;
}

static int validate_public_target(const char *target) {
  if (muon_cef_target_from_public_target(target) != NULL) {
    return 0;
  }
  muon_print_error("Unsupported Muon prepare target: %s\n",
                   target == NULL ? "(null)" : target);
  return -1;
}

static void prepare_report_progress(const PrepareOptions *options,
                                    MuonPrepareProgressPhase phase,
                                    const char *status,
                                    unsigned long long current,
                                    unsigned long long total,
                                    int determinate) {
  if (options->progress_callback != NULL) {
    ((PrepareOptions *)options)->progress_emitted = 1;
  } else if (status != NULL && status[0] != '\0') {
    muon_log_message("%s", status);
  }
  muon_report_progress(options->progress_callback, options->progress_user_data,
                       phase, status, current, total, determinate);
}

static void forward_prepare_progress(const MuonPrepareProgress *progress,
                                     void *user_data) {
  PrepareOptions *options = (PrepareOptions *)user_data;
  options->progress_emitted = 1;
  options->progress_callback(progress, options->progress_user_data);
}

static MuonPrepareProgressCallback get_prepare_progress_callback(
    const PrepareOptions *options) {
  return options->progress_callback == NULL ? NULL : forward_prepare_progress;
}

static void *get_prepare_progress_user_data(const PrepareOptions *options) {
  return options->progress_callback == NULL ? NULL : (void *)options;
}

static void set_default_bootstrap_options(PrepareOptions *options) {
  options->cef_version_policy = muon_duplicate_string("tested");
  options->cef_exact_version = muon_duplicate_string("");
  options->catalog_refresh_interval_seconds =
      MUON_BOOTSTRAP_DEFAULT_CATALOG_REFRESH_INTERVAL_SECONDS;
}

static int is_quiet_requested(int argc, char **argv) {
  for (int index = 1; index < argc; index += 1) {
    if (strcmp(argv[index], "--quiet") == 0 || strcmp(argv[index], "-q") == 0) {
      return 1;
    }
  }
  return 0;
}

static char *find_muon_stage_project_root(const char *stage_dir) {
  if (strcmp(stage_dir, ".muon") == 0 ||
      strncmp(stage_dir, ".muon/", 6) == 0) {
    return muon_duplicate_string(".");
  }
  const char *cursor = stage_dir;
  while ((cursor = strstr(cursor, "/.muon")) != NULL) {
    const char next = cursor[6];
    if (next == '\0' || next == '/') {
      if (cursor == stage_dir) {
        return muon_duplicate_string("/");
      }
#ifdef _WIN32
      if (cursor == stage_dir + 2 && isalpha((unsigned char)stage_dir[0]) &&
          stage_dir[1] == ':') {
        return muon_substring(stage_dir, 3);
      }
#endif
      return muon_substring(stage_dir, (size_t)(cursor - stage_dir));
    }
    cursor += 1;
  }
  return NULL;
}

static const char *const k_muon_gitignore_entries[] = {
    ".muon/",
    "dist-muon/",
    "artifacts/",
};

static int gitignore_line_equals(const char *line_start,
                                 const char *line_end,
                                 const char *value) {
  const size_t line_length = (size_t)(line_end - line_start);
  const size_t value_length = strlen(value);
  return line_length == value_length &&
         strncmp(line_start, value, value_length) == 0;
}

static int gitignore_line_matches_entry(const char *line_start,
                                        const char *line_end,
                                        const char *entry) {
  if (strcmp(entry, ".muon/") == 0) {
    return gitignore_line_equals(line_start, line_end, ".muon/") ||
           gitignore_line_equals(line_start, line_end, "/.muon/") ||
           gitignore_line_equals(line_start, line_end, ".muon") ||
           gitignore_line_equals(line_start, line_end, "/.muon");
  }
  if (strcmp(entry, "dist-muon/") == 0) {
    return gitignore_line_equals(line_start, line_end, "dist-muon/") ||
           gitignore_line_equals(line_start, line_end, "/dist-muon/") ||
           gitignore_line_equals(line_start, line_end, "dist-muon") ||
           gitignore_line_equals(line_start, line_end, "/dist-muon") ||
           gitignore_line_equals(line_start, line_end, "dist-muon/*") ||
           gitignore_line_equals(line_start, line_end, "/dist-muon/*");
  }
  if (strcmp(entry, "artifacts/") == 0) {
    return gitignore_line_equals(line_start, line_end, "artifacts/") ||
           gitignore_line_equals(line_start, line_end, "/artifacts/") ||
           gitignore_line_equals(line_start, line_end, "artifacts") ||
           gitignore_line_equals(line_start, line_end, "/artifacts");
  }
  return gitignore_line_equals(line_start, line_end, entry);
}

static int gitignore_has_entry(const char *content, const char *entry) {
  const char *cursor = content;
  while (*cursor != '\0') {
    const char *line_start = cursor;
    const char *line_limit = cursor;
    while (*line_limit != '\0' && *line_limit != '\n') {
      line_limit += 1;
    }
    const char *line_end = line_limit;
    while (line_start < line_end &&
           isspace((unsigned char)*line_start)) {
      line_start += 1;
    }
    while (line_end > line_start &&
           isspace((unsigned char)line_end[-1])) {
      line_end -= 1;
    }
    if (gitignore_line_matches_entry(line_start, line_end, entry)) {
      return 1;
    }
    cursor = *line_limit == '\0' ? line_limit : line_limit + 1;
  }
  return 0;
}

static int append_gitignore_entry(const char *gitignore_path,
                                  const char *entry,
                                  int *needs_newline) {
  const size_t prefix_length = *needs_newline ? 1 : 0;
  const size_t entry_length = strlen(entry);
  char *content = malloc(prefix_length + entry_length + 2);
  if (content == NULL) {
    return -1;
  }
  char *cursor = content;
  if (*needs_newline) {
    *cursor = '\n';
    cursor += 1;
  }
  memcpy(cursor, entry, entry_length);
  cursor += entry_length;
  *cursor = '\n';
  cursor += 1;
  *cursor = '\0';
  const int result = muon_append_text_file(gitignore_path, content);
  free(content);
  if (result == 0) {
    *needs_newline = 0;
  }
  return result;
}

static int ensure_muon_gitignore_entry(const char *stage_dir) {
  char *project_root = find_muon_stage_project_root(stage_dir);
  if (project_root == NULL) {
    return 0;
  }
  char *gitignore_path = muon_path_join(project_root, ".gitignore");
  free(project_root);
  if (gitignore_path == NULL) {
    return -1;
  }
  char *content = NULL;
  if (muon_path_exists(gitignore_path)) {
    content = muon_read_text_file(gitignore_path);
    if (content == NULL) {
      free(gitignore_path);
      return -1;
    }
  }
  int needs_newline =
      content != NULL && content[0] != '\0' &&
      content[strlen(content) - 1] != '\n';
  int result = 0;
  for (size_t index = 0;
       index < sizeof(k_muon_gitignore_entries) /
                   sizeof(k_muon_gitignore_entries[0]);
       index += 1) {
    const char *entry = k_muon_gitignore_entries[index];
    if (content != NULL && gitignore_has_entry(content, entry)) {
      continue;
    }
    result = append_gitignore_entry(gitignore_path, entry, &needs_newline);
    if (result != 0) {
      break;
    }
  }
  free(content);
  free(gitignore_path);
  return result;
}

static const MuonRuntimeInfo *get_embedded_runtime_info(void) {
#if MUON_RUNTIME_INFO_AVAILABLE
  if (kMuonRuntimeInfo.name == NULL || kMuonRuntimeInfo.target == NULL ||
      kMuonRuntimeInfo.cef_target == NULL ||
      kMuonRuntimeInfo.cef_reference_version == NULL ||
      kMuonRuntimeInfo.cef_reference_distribution == NULL ||
      kMuonRuntimeInfo.cef_reference_artifact.file_name == NULL ||
      kMuonRuntimeInfo.cef_reference_artifact.url == NULL ||
      kMuonRuntimeInfo.cef_reference_artifact.sha1 == NULL ||
      kMuonRuntimeInfo.cef_reference_artifact.size == 0) {
    muon_print_error("Invalid embedded Muon runtime metadata.\n");
    return NULL;
  }
  return &kMuonRuntimeInfo;
#else
  muon_print_error(
      "Muon runtime metadata is not embedded in this muon-builder build.\n");
  return NULL;
#endif
}

static char *sanitize_key(const char *value) {
  char *result = muon_duplicate_string(value);
  if (result == NULL) {
    return NULL;
  }
  for (char *cursor = result; *cursor != '\0'; cursor += 1) {
    if (!isalnum((unsigned char)*cursor) && *cursor != '.' && *cursor != '_' &&
        *cursor != '-') {
      *cursor = '_';
    }
  }
  return result;
}

static int extract_archive(
    const char *archive_path, const char *destination, size_t *file_count,
    MuonPrepareProgressCallback progress_callback, void *progress_user_data) {
  if (muon_ensure_directory(destination) != 0) {
    return -1;
  }
  char *temporary_directory =
      muon_create_temporary_path(destination, "cef-extract");
  if (temporary_directory == NULL || muon_ensure_directory(temporary_directory) != 0) {
    free(temporary_directory);
    return -1;
  }
  if (muon_extract_tar_bz2_archive_progress(
          archive_path, temporary_directory, 1, NULL, progress_callback,
          progress_user_data, MUON_PREPARE_PROGRESS_PHASE_INSTALLING,
          "Installing CEF runtime...") != 0) {
    muon_remove_recursive(temporary_directory);
    free(temporary_directory);
    return -1;
  }
  char *release_path = muon_path_join(temporary_directory, "Release");
  char *resource_path = muon_path_join(temporary_directory, "Resources");
  if (release_path == NULL || resource_path == NULL) {
    muon_remove_recursive(temporary_directory);
    free(release_path);
    free(resource_path);
    free(temporary_directory);
    return -1;
  }
  const int has_release = muon_path_exists(release_path);
  const int has_resources = muon_path_exists(resource_path);
  int result = 0;
  if (has_release || has_resources) {
    if (has_release) {
      result = muon_copy_directory_contents_progress(
          release_path, destination, file_count, progress_callback,
          progress_user_data, MUON_PREPARE_PROGRESS_PHASE_INSTALLING,
          "Installing CEF runtime...");
    }
    if (result == 0 && has_resources) {
      result = muon_copy_directory_contents_progress(
          resource_path, destination, file_count, progress_callback,
          progress_user_data, MUON_PREPARE_PROGRESS_PHASE_INSTALLING,
          "Installing CEF runtime...");
    }
  } else {
    result = muon_copy_directory_contents_progress(
        temporary_directory, destination, file_count, progress_callback,
        progress_user_data, MUON_PREPARE_PROGRESS_PHASE_INSTALLING,
        "Installing CEF runtime...");
  }
  if (muon_remove_recursive(temporary_directory) != 0) {
    result = -1;
  }
  free(release_path);
  free(resource_path);
  free(temporary_directory);
  return result;
}

static char *default_cache_dir(void) {
  const char *override = getenv("MUON_CACHE_DIR");
  if (override != NULL && override[0] != '\0') {
    return muon_duplicate_path_string(override);
  }
#ifdef _WIN32
  const char *local_app_data = getenv("LOCALAPPDATA");
  if (local_app_data != NULL && local_app_data[0] != '\0') {
    char *normalized = muon_duplicate_path_string(local_app_data);
    char *result = normalized == NULL ? NULL : muon_path_join(normalized, "muon");
    free(normalized);
    return result;
  }
  const char *user_profile = getenv("USERPROFILE");
  if (user_profile != NULL && user_profile[0] != '\0') {
    char *normalized = muon_duplicate_path_string(user_profile);
    char *result = normalized == NULL ? NULL : muon_path_join3(normalized, ".cache", "muon");
    free(normalized);
    return result;
  }
#endif
  const char *xdg = getenv("XDG_CACHE_HOME");
  if (xdg != NULL && xdg[0] != '\0') {
    char *normalized = muon_duplicate_path_string(xdg);
    char *result = normalized == NULL ? NULL : muon_path_join(normalized, "muon");
    free(normalized);
    return result;
  }
  const char *home = getenv("HOME");
  if (home == NULL || home[0] == '\0') {
    return muon_duplicate_string(".muon-cache");
  }
  char *normalized = muon_duplicate_path_string(home);
  char *result = normalized == NULL ? NULL : muon_path_join3(normalized, ".cache", "muon");
  free(normalized);
  return result;
}

static int bootstrap_config_exists(const char *runtime_dir) {
  char *path = muon_path_join(runtime_dir, MUON_BOOTSTRAP_CONFIG_FILE_NAME);
  if (path == NULL) {
    return 0;
  }
  const int exists = muon_path_exists(path);
  free(path);
  return exists;
}

static int apply_bootstrap_config(PrepareOptions *options,
                                  const MuonBootstrapConfig *config) {
  char *policy = muon_duplicate_string(config->cef_version_policy);
  char *exact_version = muon_duplicate_string(config->cef_exact_version);
  if (policy == NULL || exact_version == NULL) {
    free(policy);
    free(exact_version);
    return -1;
  }
  free(options->cef_version_policy);
  free(options->cef_exact_version);
  options->cef_version_policy = policy;
  options->cef_exact_version = exact_version;
  options->has_cef_version_policy = config->has_cef_version_policy;
  options->has_cef_exact_version = config->has_cef_exact_version;
  options->catalog_refresh_interval_seconds =
      config->catalog_refresh_interval_seconds;
  options->has_catalog_refresh_interval_seconds =
      config->has_catalog_refresh_interval_seconds;
  options->last_catalog_update_unix = config->last_catalog_update_unix;
  options->update_requested = config->update_requested;
  options->update_requested_at_unix = config->update_requested_at_unix;
  return 0;
}

static int read_bootstrap_config_with_embedded_default(
    const char *runtime_dir,
    MuonBootstrapConfig *config) {
  char *default_version_policy = NULL;
  if (muon_bootstrap_get_embedded_default_version_policy(
          &default_version_policy) != 0) {
    return -1;
  }
  const int result = muon_bootstrap_config_read_with_default(
      runtime_dir, default_version_policy, config);
  free(default_version_policy);
  return result;
}

static int apply_embedded_default_policy_if_needed(PrepareOptions *options) {
  char *default_version_policy = NULL;
  if (muon_bootstrap_get_embedded_default_version_policy(
          &default_version_policy) != 0) {
    return -1;
  }
  if (default_version_policy == NULL || options->has_cef_version_policy) {
    free(default_version_policy);
    return 0;
  }
  char *policy = muon_duplicate_string(default_version_policy);
  free(default_version_policy);
  if (policy == NULL) {
    return -1;
  }
  free(options->cef_version_policy);
  options->cef_version_policy = policy;
  return 0;
}

static int load_bootstrap_config_if_present(PrepareOptions *options) {
  const char *config_dir = NULL;
  if (options->stage_dir != NULL && bootstrap_config_exists(options->stage_dir)) {
    config_dir = options->stage_dir;
  } else if (bootstrap_config_exists(options->muon_path)) {
    config_dir = options->muon_path;
  }
  if (config_dir == NULL) {
    return apply_embedded_default_policy_if_needed(options);
  }
  MuonBootstrapConfig config;
  if (read_bootstrap_config_with_embedded_default(config_dir, &config) != 0) {
    return -1;
  }
  const int result = apply_bootstrap_config(options, &config);
  muon_bootstrap_config_free(&config);
  if (result == 0) {
    options->write_bootstrap_config = 1;
    options->bootstrap_config_dir = muon_duplicate_string(config_dir);
    if (options->bootstrap_config_dir == NULL) {
      return -1;
    }
  }
  return result;
}

static int save_bootstrap_config_if_needed(const PrepareOptions *options) {
  if (!options->write_bootstrap_config) {
    return 0;
  }
  const char *config_dir =
      options->bootstrap_config_dir == NULL ? options->muon_path
                                            : options->bootstrap_config_dir;
  MuonBootstrapConfig config;
  muon_bootstrap_config_init_defaults(&config);
  if (config.cef_version_policy == NULL || config.cef_exact_version == NULL) {
    muon_bootstrap_config_free(&config);
    return -1;
  }
  free(config.cef_version_policy);
  free(config.cef_exact_version);
  config.cef_version_policy =
      muon_duplicate_string(options->cef_version_policy);
  config.cef_exact_version = muon_duplicate_string(options->cef_exact_version);
  config.has_cef_version_policy = options->has_cef_version_policy;
  config.has_cef_exact_version = options->has_cef_exact_version;
  config.catalog_refresh_interval_seconds =
      options->catalog_refresh_interval_seconds;
  config.has_catalog_refresh_interval_seconds =
      options->has_catalog_refresh_interval_seconds;
  config.last_catalog_update_unix = options->last_catalog_update_unix;
  config.update_requested = options->update_requested;
  config.update_requested_at_unix = options->update_requested_at_unix;
  if (config.cef_version_policy == NULL || config.cef_exact_version == NULL) {
    muon_bootstrap_config_free(&config);
    return -1;
  }
  const int result = muon_bootstrap_config_write(config_dir, &config);
  muon_bootstrap_config_free(&config);
  return result;
}

static int catalog_exists(const char *cache_dir) {
  char *catalog_path = muon_path_join(cache_dir, "catalog.json");
  if (catalog_path == NULL) {
    return 0;
  }
  const int exists = muon_path_exists(catalog_path);
  free(catalog_path);
  return exists;
}

static int policy_uses_catalog(const PrepareOptions *options) {
  return strcmp(options->cef_version_policy, "tested") != 0;
}

static int catalog_refresh_due(const PrepareOptions *options) {
  if (!catalog_exists(options->cache_dir)) {
    return 1;
  }
  if (options->force || options->update_requested) {
    return 1;
  }
  if (options->catalog_refresh_interval_seconds == 0) {
    return 0;
  }
  const unsigned long long now = muon_current_unix_time();
  return options->last_catalog_update_unix == 0 ||
         now >= options->last_catalog_update_unix +
                    options->catalog_refresh_interval_seconds;
}

static int ensure_catalog_cache(const PrepareOptions *options,
                                int catalog_required) {
  PrepareOptions *mutable_options = (PrepareOptions *)options;
  mutable_options->catalog_updated = 0;
  if (!catalog_refresh_due(options)) {
    return 0;
  }
  int updated = 0;
  if (options->progress_callback != NULL) {
    mutable_options->progress_emitted = 1;
  }
  const int result = muon_prepare_ensure_catalog_cache_with_status_progress(
      options->cache_dir, 1, &updated, get_prepare_progress_callback(options),
      get_prepare_progress_user_data(options));
  mutable_options->catalog_updated = updated;
  if (updated) {
    mutable_options->last_catalog_update_unix = muon_current_unix_time();
    mutable_options->update_requested = 0;
    mutable_options->update_requested_at_unix = 0;
  }
  return result == 0 || (!catalog_required && catalog_exists(options->cache_dir))
             ? 0
             : -1;
}

static MuonCefReference create_cef_reference(
    const MuonRuntimeInfo *runtime_info) {
  MuonCefReference reference;
  memset(&reference, 0, sizeof(reference));
  reference.version = runtime_info->cef_reference_version;
  reference.target = runtime_info->cef_target;
  reference.distribution = runtime_info->cef_reference_distribution;
  reference.api_version = runtime_info->cef_reference_api_version;
  reference.api_hash = runtime_info->cef_reference_api_hash;
  reference.artifact_file_name =
      runtime_info->cef_reference_artifact.file_name;
  reference.artifact_url = runtime_info->cef_reference_artifact.url;
  reference.artifact_sha1 = runtime_info->cef_reference_artifact.sha1;
  reference.artifact_size = runtime_info->cef_reference_artifact.size;
  return reference;
}

static int ensure_cef_archive_cache(const PrepareOptions *options,
                                    const MuonRuntimeInfo *runtime_info,
                                    char **cef_path) {
  const MuonCefReference reference = create_cef_reference(runtime_info);
  MuonCefArtifact artifact;
  if (muon_prepare_ensure_cef_archive_cache_for_policy_progress(
          options->cache_dir, &reference, options->cef_version_policy,
          options->cef_exact_version, options->force, &artifact, cef_path,
          get_prepare_progress_callback(options),
          get_prepare_progress_user_data(options)) != 0) {
    return -1;
  }
  muon_prepare_free_cef_artifact(&artifact);
  return 0;
}

static int copy_cef_source(
    const char *cef_path, const char *destination, size_t *file_count,
    MuonPrepareProgressCallback progress_callback, void *progress_user_data) {
  if (!muon_path_exists(cef_path)) {
    muon_print_error("CEF path does not exist: %s\n", cef_path);
    return -1;
  }
  char *release_path = muon_path_join(cef_path, "Release");
  char *resource_path = muon_path_join(cef_path, "Resources");
  if (release_path == NULL || resource_path == NULL) {
    free(release_path);
    free(resource_path);
    return -1;
  }
  const int has_release = muon_path_exists(release_path);
  const int has_resources = muon_path_exists(resource_path);
  int result = 0;
  if (has_release || has_resources) {
    if (has_release) {
      result = muon_copy_directory_contents_progress(
          release_path, destination, file_count, progress_callback,
          progress_user_data, MUON_PREPARE_PROGRESS_PHASE_INSTALLING,
          "Installing CEF runtime...");
    }
    if (result == 0 && has_resources) {
      result = muon_copy_directory_contents_progress(
          resource_path, destination, file_count, progress_callback,
          progress_user_data, MUON_PREPARE_PROGRESS_PHASE_INSTALLING,
          "Installing CEF runtime...");
    }
  } else {
    result = muon_copy_directory_contents_progress(
        cef_path, destination, file_count, progress_callback,
        progress_user_data, MUON_PREPARE_PROGRESS_PHASE_INSTALLING,
        "Installing CEF runtime...");
  }
  free(release_path);
  free(resource_path);
  return result;
}

static void finalize_sha1_hex(SHA1_CTX *context,
                              char output[SHA1_DIGEST_STRING_LENGTH]) {
  uint8_t digest[SHA1_DIGEST_LENGTH];
  SHA1Final(digest, context);
  muon_sha1_digest_to_hex(digest, output);
}

static void prepare_name_list_free(PrepareNameList *list) {
  muon_free_string_array(list->values, list->count);
  list->values = NULL;
  list->count = 0;
  list->capacity = 0;
}

static int prepare_name_list_add(PrepareNameList *list, const char *value) {
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

static int compare_prepare_name_pointers(const void *left, const void *right) {
  char *const *left_value = (char *const *)left;
  char *const *right_value = (char *const *)right;
  return strcmp(*left_value, *right_value);
}

static int read_sorted_prepare_names(const char *path, PrepareNameList *list) {
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
    if (prepare_name_list_add(list, child->d_name) != 0) {
      closedir(directory);
      return -1;
    }
  }
  closedir(directory);
  qsort(list->values, list->count, sizeof(char *),
        compare_prepare_name_pointers);
  return 0;
}

static int staging_root_entry_is_cef_payload(const char *name) {
  static const char *const cef_payload_entries[] = {
      "chrome-sandbox",
      "chrome_100_percent.pak",
      "chrome_200_percent.pak",
      "chrome_elf.dll",
      "d3dcompiler_47.dll",
      "dxcompiler.dll",
      "dxil.dll",
      "icudtl.dat",
      "libEGL.dll",
      "libEGL.so",
      "libGLESv2.dll",
      "libGLESv2.so",
      "libcef.dll",
      "libcef.so",
      "libvk_swiftshader.so",
      "libvulkan.so.1",
      "locales",
      "resources.pak",
      "snapshot_blob.bin",
      "swiftshader",
      "v8_context_snapshot.bin",
      "vk_swiftshader.dll",
      "vk_swiftshader_icd.json",
      "vulkan-1.dll",
  };
  for (size_t index = 0;
       index < sizeof(cef_payload_entries) / sizeof(cef_payload_entries[0]);
       index += 1) {
    if (strcmp(name, cef_payload_entries[index]) == 0) {
      return 1;
    }
  }
  return 0;
}

static int staging_root_entry_is_generated_runtime_state(const char *name) {
  static const char *const generated_entries[] = {
      ".muon-ready.json",
      ".muon-test-config",
      "muon-bootstrap.ini",
      "muon-cef.log",
      "muon-close-debug.log",
  };
  for (size_t index = 0;
       index < sizeof(generated_entries) / sizeof(generated_entries[0]);
       index += 1) {
    if (strcmp(name, generated_entries[index]) == 0) {
      return 1;
    }
  }
  return 0;
}

static int staging_relative_path_should_skip(const char *relative) {
  const char *separator = strchr(relative, '/');
  const size_t root_length =
      separator == NULL ? strlen(relative) : (size_t)(separator - relative);
  char *root = muon_substring(relative, root_length);
  if (root == NULL) {
    return 0;
  }
  const int result = staging_root_entry_is_cef_payload(root) ||
                     staging_root_entry_is_generated_runtime_state(root);
  free(root);
  return result;
}

static int fingerprint_staging_muon_source_recursive(
    const char *path, const char *relative,
    char fingerprint[SHA1_DIGEST_STRING_LENGTH]) {
  struct stat entry;
  if (stat(path, &entry) != 0) {
    muon_print_errno(path);
    return -1;
  }
  SHA1_CTX context;
  SHA1Init(&context);
  muon_sha1_update_string(&context, relative);
  muon_sha1_update_string(&context, ":");
  if (S_ISDIR(entry.st_mode)) {
    muon_sha1_update_string(&context, "directory");
    PrepareNameList children = {0};
    if (read_sorted_prepare_names(path, &children) != 0) {
      prepare_name_list_free(&children);
      return -1;
    }
    for (size_t index = 0; index < children.count; index += 1) {
      char *child_path = muon_path_join(path, children.values[index]);
      char *child_relative =
          relative[0] == '\0' ? muon_duplicate_string(children.values[index])
                              : muon_path_join(relative, children.values[index]);
      if (child_path == NULL || child_relative == NULL) {
        free(child_path);
        free(child_relative);
        prepare_name_list_free(&children);
        return -1;
      }
      if (staging_relative_path_should_skip(child_relative)) {
        free(child_path);
        free(child_relative);
        continue;
      }
      char child_fingerprint[SHA1_DIGEST_STRING_LENGTH];
      if (fingerprint_staging_muon_source_recursive(
              child_path, child_relative, child_fingerprint) != 0) {
        free(child_path);
        free(child_relative);
        prepare_name_list_free(&children);
        return -1;
      }
      muon_sha1_update_string(&context, children.values[index]);
      muon_sha1_update_string(&context, child_fingerprint);
      free(child_path);
      free(child_relative);
    }
    prepare_name_list_free(&children);
  } else if (S_ISREG(entry.st_mode)) {
    char content_fingerprint[SHA1_DIGEST_STRING_LENGTH];
    if (muon_sha1_file_hex(path, content_fingerprint) != 0) {
      return -1;
    }
    muon_sha1_update_string(&context, "file");
    muon_sha1_update_string(&context, content_fingerprint);
  } else {
    muon_sha1_update_string(&context, "other");
  }
  finalize_sha1_hex(&context, fingerprint);
  return 0;
}

static int fingerprint_staging_muon_source(
    const char *path, char fingerprint[SHA1_DIGEST_STRING_LENGTH]) {
  return fingerprint_staging_muon_source_recursive(path, "", fingerprint);
}

static int copy_staging_muon_source_recursive(
    const char *source, const char *destination, const char *relative,
    size_t *file_count) {
  struct stat entry;
  if (stat(source, &entry) != 0) {
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
      char *child_relative =
          relative[0] == '\0' ? muon_duplicate_string(child->d_name)
                              : muon_path_join(relative, child->d_name);
      if (child_source == NULL || child_destination == NULL ||
          child_relative == NULL) {
        free(child_source);
        free(child_destination);
        free(child_relative);
        closedir(directory);
        return -1;
      }
      if (!staging_relative_path_should_skip(child_relative) &&
          copy_staging_muon_source_recursive(child_source, child_destination,
                                             child_relative, file_count) != 0) {
        free(child_source);
        free(child_destination);
        free(child_relative);
        closedir(directory);
        return -1;
      }
      free(child_source);
      free(child_destination);
      free(child_relative);
    }
    closedir(directory);
    return 0;
  }
  if (S_ISREG(entry.st_mode)) {
    if (muon_copy_file(source, destination, (int)entry.st_mode) != 0) {
      return -1;
    }
    if (file_count != NULL) {
      *file_count += 1;
    }
  }
  return 0;
}

static int copy_staging_muon_source(const char *source,
                                    const char *destination,
                                    size_t *file_count) {
  return copy_staging_muon_source_recursive(source, destination, "", file_count);
}

static int fingerprint_cef_source(const char *cef_path,
                                  char fingerprint[SHA1_DIGEST_STRING_LENGTH]) {
  char *release_path = muon_path_join(cef_path, "Release");
  char *resource_path = muon_path_join(cef_path, "Resources");
  if (release_path == NULL || resource_path == NULL) {
    free(release_path);
    free(resource_path);
    return -1;
  }
  const int has_release = muon_path_exists(release_path);
  const int has_resources = muon_path_exists(resource_path);
  int result = 0;
  if (has_release || has_resources) {
    SHA1_CTX context;
    SHA1Init(&context);
    if (has_release) {
      char release_fingerprint[SHA1_DIGEST_STRING_LENGTH];
      result =
          muon_fingerprint_directory_contents(release_path, release_fingerprint);
      muon_sha1_update_string(&context, "Release");
      muon_sha1_update_string(&context, release_fingerprint);
    }
    if (result == 0 && has_resources) {
      char resource_fingerprint[SHA1_DIGEST_STRING_LENGTH];
      result =
          muon_fingerprint_directory_contents(resource_path, resource_fingerprint);
      muon_sha1_update_string(&context, "Resources");
      muon_sha1_update_string(&context, resource_fingerprint);
    }
    if (result == 0) {
      finalize_sha1_hex(&context, fingerprint);
    }
  } else {
    result = muon_fingerprint_directory_contents(cef_path, fingerprint);
  }
  free(release_path);
  free(resource_path);
  return result;
}

static int fingerprint_archive_metadata(
    const char *archive_path, char fingerprint[SHA1_DIGEST_STRING_LENGTH]) {
  struct stat entry;
  if (stat(archive_path, &entry) != 0) {
    muon_print_errno(archive_path);
    return -1;
  }
  SHA1_CTX context;
  SHA1Init(&context);
  muon_sha1_update_string(&context, archive_path);
  char metadata[128];
  snprintf(metadata, sizeof(metadata), ":%llu:%lld:%d",
           (unsigned long long)entry.st_size, (long long)entry.st_mtime,
           (int)(entry.st_mode & 0777));
  muon_sha1_update_string(&context, metadata);
  finalize_sha1_hex(&context, fingerprint);
  return 0;
}

static int fingerprint_staging_cef_source(
    const char *cef_path, int cef_is_archive,
    char fingerprint[SHA1_DIGEST_STRING_LENGTH]) {
  return cef_is_archive ? fingerprint_archive_metadata(cef_path, fingerprint)
                        : fingerprint_cef_source(cef_path, fingerprint);
}

static int set_prepare_result(PrepareResult *result, const char *stage_path,
                              const char *muon_path, const char *cef_path,
                              int cache_hit) {
  result->stage_path =
      stage_path == NULL ? NULL : muon_duplicate_string(stage_path);
  result->muon_path = muon_duplicate_string(muon_path);
  result->cef_path = muon_duplicate_string(cef_path);
  result->cache_hit = cache_hit;
  if (result->muon_path == NULL || result->cef_path == NULL ||
      (stage_path != NULL && result->stage_path == NULL)) {
    return -1;
  }
  return 0;
}

static char *create_hidden_lock_path(const char *parent, const char *prefix,
                                     const char *key) {
  const int size = snprintf(NULL, 0, "%s/.%s-%s.lock", parent, prefix, key);
  if (size < 0) {
    return NULL;
  }
  char *result = (char *)malloc((size_t)size + 1);
  if (result == NULL) {
    return NULL;
  }
  snprintf(result, (size_t)size + 1, "%s/.%s-%s.lock", parent, prefix, key);
  return result;
}

static int prepare_staging(const PrepareOptions *options,
                           const MuonRuntimeInfo *runtime_info,
                           const char *cef_path, int cef_is_archive,
                           PrepareResult *result) {
  if (options->stage_dir == NULL) {
    return set_prepare_result(result, NULL, options->muon_path, cef_path, 0);
  }
  char muon_fingerprint[SHA1_DIGEST_STRING_LENGTH];
  char cef_fingerprint[SHA1_DIGEST_STRING_LENGTH];
  if (fingerprint_staging_muon_source(options->muon_path, muon_fingerprint) !=
          0 ||
      fingerprint_staging_cef_source(cef_path, cef_is_archive,
                                     cef_fingerprint) != 0) {
    return -1;
  }
  char *ready_content =
      muon_create_ready_content(muon_fingerprint, cef_fingerprint);
  char *ready_path = muon_path_join(options->stage_dir, ".muon-ready.json");
  char *parent = muon_parent_directory(options->stage_dir);
  char *raw_key = sanitize_key(options->stage_dir);
  char *lock_name =
      parent == NULL || raw_key == NULL
          ? NULL
          : create_hidden_lock_path(parent, "muon-stage", raw_key);
  if (ready_content == NULL || ready_path == NULL || parent == NULL ||
      raw_key == NULL || lock_name == NULL) {
    free(ready_content);
    free(ready_path);
    free(parent);
    free(raw_key);
    free(lock_name);
    return -1;
  }
  if (!options->force && muon_path_exists(ready_path) &&
      muon_ready_file_matches(ready_path, ready_content)) {
    const int set_result = set_prepare_result(result, options->stage_dir,
                                              options->muon_path, cef_path, 1);
    free(ready_content);
    free(ready_path);
    free(parent);
    free(raw_key);
    free(lock_name);
    return set_result;
  }
  if (muon_ensure_directory(parent) != 0 ||
      muon_acquire_lock_with_progress(
          lock_name, get_prepare_progress_callback(options),
          get_prepare_progress_user_data(options),
          MUON_PREPARE_PROGRESS_PHASE_INSTALLING,
          "Installing CEF runtime...") != 0) {
    free(ready_content);
    free(ready_path);
    free(parent);
    free(raw_key);
    free(lock_name);
    return -1;
  }
  if (ensure_muon_gitignore_entry(options->stage_dir) != 0) {
    muon_release_lock(lock_name);
    free(ready_content);
    free(ready_path);
    free(parent);
    free(raw_key);
    free(lock_name);
    return -1;
  }
  if (!options->force && muon_path_exists(ready_path) &&
      muon_ready_file_matches(ready_path, ready_content)) {
    muon_release_lock(lock_name);
    const int set_result = set_prepare_result(result, options->stage_dir,
                                              options->muon_path, cef_path, 1);
    free(ready_content);
    free(ready_path);
    free(parent);
    free(raw_key);
    free(lock_name);
    return set_result;
  }
  char *temporary_directory = muon_create_temporary_path(parent, raw_key);
  char *temporary_ready =
      temporary_directory == NULL ? NULL : muon_path_join(temporary_directory, ".muon-ready.json");
  if (temporary_directory == NULL || temporary_ready == NULL ||
      muon_ensure_directory(temporary_directory) != 0) {
    muon_remove_recursive(temporary_directory);
    muon_release_lock(lock_name);
    free(ready_content);
    free(ready_path);
    free(parent);
    free(raw_key);
    free(lock_name);
    free(temporary_directory);
    free(temporary_ready);
    return -1;
  }
  size_t cef_file_count = 0;
  size_t muon_file_count = 0;
  prepare_report_progress(options, MUON_PREPARE_PROGRESS_PHASE_INSTALLING,
                          "Installing CEF runtime...", 0, 0, 0);
  if (copy_staging_muon_source(options->muon_path, temporary_directory,
                               &muon_file_count) != 0) {
    muon_remove_recursive(temporary_directory);
    muon_release_lock(lock_name);
    free(ready_content);
    free(ready_path);
    free(parent);
    free(raw_key);
    free(lock_name);
    free(temporary_directory);
    free(temporary_ready);
    return -1;
  }
  muon_log_message("Muon files copied to staging: files=%llu",
              (unsigned long long)muon_file_count);
  if ((cef_is_archive
           ? extract_archive(cef_path, temporary_directory, &cef_file_count,
                             get_prepare_progress_callback(options),
                             get_prepare_progress_user_data(options))
           : copy_cef_source(cef_path, temporary_directory,
                             &cef_file_count,
                             get_prepare_progress_callback(options),
                             get_prepare_progress_user_data(options))) != 0) {
    muon_remove_recursive(temporary_directory);
    muon_release_lock(lock_name);
    free(ready_content);
    free(ready_path);
    free(parent);
    free(raw_key);
    free(lock_name);
    free(temporary_directory);
    free(temporary_ready);
    return -1;
  }
  muon_log_message("CEF files copied to staging: version=%s files=%llu",
              runtime_info->cef_reference_version,
              (unsigned long long)cef_file_count);
  if (muon_write_text_file(temporary_ready, ready_content) != 0) {
    muon_remove_recursive(temporary_directory);
    muon_release_lock(lock_name);
    free(ready_content);
    free(ready_path);
    free(parent);
    free(raw_key);
    free(lock_name);
    free(temporary_directory);
    free(temporary_ready);
    return -1;
  }
  prepare_report_progress(options, MUON_PREPARE_PROGRESS_PHASE_FINALIZING,
                          "Starting Muon...", 0, 0, 0);
  if (muon_path_exists(options->stage_dir) &&
      muon_remove_recursive(options->stage_dir) != 0) {
    muon_remove_recursive(temporary_directory);
    muon_release_lock(lock_name);
    free(ready_content);
    free(ready_path);
    free(parent);
    free(raw_key);
    free(lock_name);
    free(temporary_directory);
    free(temporary_ready);
    return -1;
  }
  if (rename(temporary_directory, options->stage_dir) != 0) {
    muon_print_errno(options->stage_dir);
    muon_remove_recursive(temporary_directory);
    muon_release_lock(lock_name);
    free(ready_content);
    free(ready_path);
    free(parent);
    free(raw_key);
    free(lock_name);
    free(temporary_directory);
    free(temporary_ready);
    return -1;
  }
  muon_release_lock(lock_name);
  const int set_result =
      set_prepare_result(result, options->stage_dir, options->muon_path,
                         cef_path, 0);
  free(ready_content);
  free(ready_path);
  free(parent);
  free(raw_key);
  free(lock_name);
  free(temporary_directory);
  free(temporary_ready);
  return set_result;
}

static int prepare_cef_in_place(const PrepareOptions *options,
                                const MuonRuntimeInfo *runtime_info,
                                const char *cef_path, int cef_is_archive,
                                PrepareResult *result) {
  char cef_fingerprint[SHA1_DIGEST_STRING_LENGTH];
  if ((cef_is_archive
           ? muon_fingerprint_path_recursive(cef_path, "", cef_fingerprint)
           : fingerprint_cef_source(cef_path, cef_fingerprint)) != 0) {
    return -1;
  }
  char *ready_content = muon_create_ready_content(kEmptyFingerprint, cef_fingerprint);
  char *ready_path = muon_path_join(options->muon_path, ".muon-cef-ready.json");
  char *raw_key = sanitize_key(options->muon_path);
  char *lock_name =
      raw_key == NULL
          ? NULL
          : create_hidden_lock_path(options->muon_path, "muon-cef", raw_key);
  if (ready_content == NULL || ready_path == NULL || raw_key == NULL ||
      lock_name == NULL) {
    free(ready_content);
    free(ready_path);
    free(raw_key);
    free(lock_name);
    return -1;
  }
  if (!options->force && muon_path_exists(ready_path) &&
      muon_ready_file_matches(ready_path, ready_content)) {
    const int set_result = set_prepare_result(result, options->muon_path,
                                              options->muon_path, cef_path, 1);
    free(ready_content);
    free(ready_path);
    free(raw_key);
    free(lock_name);
    return set_result;
  }
  if (muon_acquire_lock_with_progress(
          lock_name, get_prepare_progress_callback(options),
          get_prepare_progress_user_data(options),
          MUON_PREPARE_PROGRESS_PHASE_INSTALLING,
          "Installing CEF runtime...") != 0) {
    free(ready_content);
    free(ready_path);
    free(raw_key);
    free(lock_name);
    return -1;
  }
  if (!options->force && muon_path_exists(ready_path) &&
      muon_ready_file_matches(ready_path, ready_content)) {
    muon_release_lock(lock_name);
    const int set_result = set_prepare_result(result, options->muon_path,
                                              options->muon_path, cef_path, 1);
    free(ready_content);
    free(ready_path);
    free(raw_key);
    free(lock_name);
    return set_result;
  }
  size_t cef_file_count = 0;
  prepare_report_progress(options, MUON_PREPARE_PROGRESS_PHASE_INSTALLING,
                          "Installing CEF runtime...", 0, 0, 0);
  if ((cef_is_archive
           ? extract_archive(cef_path, options->muon_path, &cef_file_count,
                             get_prepare_progress_callback(options),
                             get_prepare_progress_user_data(options))
           : copy_cef_source(cef_path, options->muon_path,
                             &cef_file_count,
                             get_prepare_progress_callback(options),
                             get_prepare_progress_user_data(options))) != 0) {
    muon_release_lock(lock_name);
    free(ready_content);
    free(ready_path);
    free(raw_key);
    free(lock_name);
    return -1;
  }
  muon_log_message("CEF files copied to runtime: version=%s files=%llu",
              runtime_info->cef_reference_version,
              (unsigned long long)cef_file_count);
  if (muon_write_text_file(ready_path, ready_content) != 0) {
    muon_release_lock(lock_name);
    free(ready_content);
    free(ready_path);
    free(raw_key);
    free(lock_name);
    return -1;
  }
  prepare_report_progress(options, MUON_PREPARE_PROGRESS_PHASE_FINALIZING,
                          "Starting Muon...", 0, 0, 0);
  muon_release_lock(lock_name);
  const int set_result = set_prepare_result(result, options->muon_path,
                                            options->muon_path, cef_path, 0);
  free(ready_content);
  free(ready_path);
  free(raw_key);
  free(lock_name);
  return set_result;
}

static int print_json_document(yyjson_mut_doc *document) {
  char *json = yyjson_mut_write(document,
                                YYJSON_WRITE_PRETTY_TWO_SPACES |
                                    YYJSON_WRITE_NEWLINE_AT_END,
                                NULL);
  if (json == NULL) {
    return -1;
  }
  fputs(json, stdout);
  free(json);
  return 0;
}

static void print_result_json(const PrepareResult *result) {
  yyjson_mut_doc *document = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *root = document == NULL ? NULL : yyjson_mut_obj(document);
  if (document == NULL || root == NULL) {
    yyjson_mut_doc_free(document);
    printf("{}\n");
    return;
  }
  yyjson_mut_doc_set_root(document, root);
  int ok = 1;
  if (result->stage_path != NULL) {
    ok = ok && yyjson_mut_obj_add_strcpy(document, root, "stagePath",
                                         result->stage_path);
  }
  ok = ok &&
       yyjson_mut_obj_add_strcpy(document, root, "muonPath", result->muon_path) &&
       yyjson_mut_obj_add_strcpy(document, root, "cefPath", result->cef_path) &&
       yyjson_mut_obj_add_bool(document, root, "cacheHit",
                               result->cache_hit != 0);
  if (!ok || print_json_document(document) != 0) {
    printf("{}\n");
  }
  yyjson_mut_doc_free(document);
}

static void print_cef_result_json(const char *cef_path, const char *archive_path,
                                  const char *public_target,
                                  const MuonCefArtifact *artifact) {
  yyjson_mut_doc *document = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *root = document == NULL ? NULL : yyjson_mut_obj(document);
  yyjson_mut_val *artifact_object =
      root == NULL ? NULL : yyjson_mut_obj_add_obj(document, root, "artifact");
  if (document == NULL || root == NULL || artifact_object == NULL) {
    yyjson_mut_doc_free(document);
    printf("{}\n");
    return;
  }
  yyjson_mut_doc_set_root(document, root);
  const int ok =
      yyjson_mut_obj_add_strcpy(document, root, "cefPath", cef_path) &&
      yyjson_mut_obj_add_strcpy(document, root, "archivePath", archive_path) &&
      yyjson_mut_obj_add_strcpy(document, root, "version", artifact->version) &&
      yyjson_mut_obj_add_strcpy(document, root, "target", public_target) &&
      yyjson_mut_obj_add_strcpy(document, root, "cefTarget",
                                artifact->target) &&
      yyjson_mut_obj_add_strcpy(document, root, "distribution",
                                artifact->distribution) &&
      yyjson_mut_obj_add_strcpy(document, artifact_object, "fileName",
                                artifact->file_name) &&
      yyjson_mut_obj_add_strcpy(document, artifact_object, "url",
                                artifact->url) &&
      yyjson_mut_obj_add_strcpy(document, artifact_object, "sha1",
                                artifact->sha1) &&
      yyjson_mut_obj_add_uint(document, artifact_object, "size",
                              artifact->size);
  if (!ok || print_json_document(document) != 0) {
    printf("{}\n");
  }
  yyjson_mut_doc_free(document);
}

static void print_usage(void) {
  muon_print_error(
      "Usage: muon-builder <command> [options]\n"
      "       muon-builder runtime --muon-path <path> [--cef-path <path>] "
      "[--stage-dir <path>] [--target <target>] [--cache-dir <path>] "
      "[--force] [--quiet|-q] [--json]\n"
      "       muon-builder buildtime --version <cefVersion> --target <target> "
      "--output-dir <path> [--cache-dir <path>] [--force] [--quiet|-q] "
      "[--json]\n"
      "       muon-builder resource --input <pe> --updates-json <json> "
      "--output <pe> [--quiet|-q]\n");
}

static int parse_runtime_arguments(int argc, char **argv, int start_index,
                                   PrepareOptions *options) {
  memset(options, 0, sizeof(*options));
  set_default_bootstrap_options(options);
  for (int index = start_index; index < argc; index += 1) {
    if (strcmp(argv[index], "--muon-path") == 0 && index + 1 < argc) {
      options->muon_path = argv[++index];
    } else if (strcmp(argv[index], "--cef-path") == 0 && index + 1 < argc) {
      options->cef_path = argv[++index];
    } else if (strcmp(argv[index], "--stage-dir") == 0 && index + 1 < argc) {
      options->stage_dir = argv[++index];
    } else if (strcmp(argv[index], "--target") == 0 && index + 1 < argc) {
      options->target = argv[++index];
    } else if (strcmp(argv[index], "--cache-dir") == 0 && index + 1 < argc) {
      options->cache_dir = argv[++index];
    } else if (strcmp(argv[index], "--force") == 0) {
      options->force = 1;
    } else if (strcmp(argv[index], "--quiet") == 0 ||
               strcmp(argv[index], "-q") == 0) {
      options->quiet = 1;
    } else if (strcmp(argv[index], "--json") == 0) {
      options->json = 1;
    } else if (strcmp(argv[index], "--help") == 0) {
      print_usage();
      exit(0);
    } else {
      muon_print_error("Unknown option: %s\n", argv[index]);
      return -1;
    }
  }
  if (options->muon_path == NULL) {
    muon_print_error("--muon-path is required.\n");
    return -1;
  }
  if (options->cache_dir == NULL) {
    options->cache_dir = default_cache_dir();
  }
  if (options->target == NULL) {
    options->target = MUON_PREPARE_TARGET_NAME;
  }
  if (validate_public_target(options->target) != 0) {
    return -1;
  }
  muon_normalize_path_separators(options->muon_path);
  muon_normalize_path_separators(options->cef_path);
  muon_normalize_path_separators(options->stage_dir);
  muon_normalize_path_separators(options->cache_dir);
  if (options->cache_dir == NULL || options->cef_version_policy == NULL ||
      options->cef_exact_version == NULL) {
    return -1;
  }
  return load_bootstrap_config_if_present(options);
}

static int parse_buildtime_arguments(int argc, char **argv, int start_index,
                                     PrepareOptions *options) {
  memset(options, 0, sizeof(*options));
  set_default_bootstrap_options(options);
  for (int index = start_index; index < argc; index += 1) {
    if (strcmp(argv[index], "--version") == 0 && index + 1 < argc) {
      options->cef_version = argv[++index];
    } else if (strcmp(argv[index], "--target") == 0 && index + 1 < argc) {
      options->target = argv[++index];
    } else if (strcmp(argv[index], "--output-dir") == 0 && index + 1 < argc) {
      options->output_dir = argv[++index];
    } else if (strcmp(argv[index], "--cache-dir") == 0 && index + 1 < argc) {
      options->cache_dir = argv[++index];
    } else if (strcmp(argv[index], "--force") == 0) {
      options->force = 1;
    } else if (strcmp(argv[index], "--quiet") == 0 ||
               strcmp(argv[index], "-q") == 0) {
      options->quiet = 1;
    } else if (strcmp(argv[index], "--json") == 0) {
      options->json = 1;
    } else if (strcmp(argv[index], "--help") == 0) {
      print_usage();
      exit(0);
    } else {
      muon_print_error("Unknown option: %s\n", argv[index]);
      return -1;
    }
  }
  if (options->cef_version == NULL) {
    muon_print_error("--version is required.\n");
    return -1;
  }
  if (options->output_dir == NULL) {
    muon_print_error("--output-dir is required.\n");
    return -1;
  }
  if (options->cache_dir == NULL) {
    options->cache_dir = default_cache_dir();
  }
  if (options->target == NULL) {
    options->target = MUON_PREPARE_TARGET_NAME;
  }
  if (validate_public_target(options->target) != 0) {
    return -1;
  }
  muon_normalize_path_separators(options->output_dir);
  muon_normalize_path_separators(options->cache_dir);
  return options->cache_dir == NULL || options->cef_version_policy == NULL ||
                 options->cef_exact_version == NULL
             ? -1
             : 0;
}

int muon_prepare_in_place_with_progress(
    const char *muon_path, const char *target, const char *cache_dir, int force,
    int quiet, MuonPrepareProgressCallback progress_callback,
    void *progress_user_data) {
  setvbuf(stderr, NULL, _IONBF, 0);
  muon_set_quiet(0);
  PrepareOptions options;
  memset(&options, 0, sizeof(options));
  options.muon_path = muon_duplicate_path_string(muon_path);
  options.target =
      target == NULL || target[0] == '\0'
          ? muon_duplicate_string(MUON_PREPARE_TARGET_NAME)
          : muon_duplicate_string(target);
  options.cache_dir =
      cache_dir == NULL || cache_dir[0] == '\0'
          ? default_cache_dir()
          : muon_duplicate_path_string(cache_dir);
  options.force = force;
  options.quiet = quiet;
  options.progress_callback = progress_callback;
  options.progress_user_data = progress_user_data;
  set_default_bootstrap_options(&options);
  if (options.muon_path == NULL || options.target == NULL ||
      options.cache_dir == NULL || options.cef_version_policy == NULL ||
      options.cef_exact_version == NULL ||
      validate_public_target(options.target) != 0) {
    free(options.muon_path);
    free(options.target);
    free(options.cache_dir);
    free(options.cef_version_policy);
    free(options.cef_exact_version);
    return 1;
  }
  MuonBootstrapConfig bootstrap_config;
  if (read_bootstrap_config_with_embedded_default(options.muon_path,
                                                 &bootstrap_config) != 0) {
    free(options.muon_path);
    free(options.target);
    free(options.cache_dir);
    free(options.cef_version_policy);
    free(options.cef_exact_version);
    return 1;
  }
  if (apply_bootstrap_config(&options, &bootstrap_config) != 0) {
    muon_bootstrap_config_free(&bootstrap_config);
    free(options.muon_path);
    free(options.target);
    free(options.cache_dir);
    free(options.cef_version_policy);
    free(options.cef_exact_version);
    return 1;
  }
  muon_bootstrap_config_free(&bootstrap_config);
  options.write_bootstrap_config = 1;
  const MuonRuntimeInfo *runtime_info = get_embedded_runtime_info();
  PrepareResult result = {0};
  char *cef_path = NULL;
  int exit_code = 1;
  if (runtime_info == NULL) {
    goto cleanup_paths;
  }
  if (strcmp(runtime_info->target, options.target) != 0) {
    muon_print_error("Target mismatch: runtime=%s requested=%s\n",
                     runtime_info->target, options.target);
    goto cleanup_paths;
  }
  if (muon_ensure_directory(options.cache_dir) != 0) {
    goto cleanup_paths;
  }
  muon_set_quiet(quiet);
  const int catalog_required = strcmp(options.cef_version_policy, "exact") == 0;
  if ((policy_uses_catalog(&options) || options.force ||
       options.update_requested) &&
      ensure_catalog_cache(&options, catalog_required) != 0) {
    if (catalog_required) {
      goto cleanup_paths;
    }
    muon_log_message("CEF catalog cache skipped.");
  }
  if (ensure_cef_archive_cache(&options, runtime_info, &cef_path) != 0) {
    goto cleanup_paths;
  }
  if (prepare_cef_in_place(&options, runtime_info, cef_path, 1, &result) != 0) {
    goto cleanup_paths;
  }
  if (save_bootstrap_config_if_needed(&options) != 0) {
    goto cleanup_paths;
  }
  exit_code = 0;
cleanup_paths:
  if (options.progress_emitted) {
    prepare_report_progress(
        &options,
        exit_code == 0 ? MUON_PREPARE_PROGRESS_PHASE_DONE
                       : MUON_PREPARE_PROGRESS_PHASE_FAILED,
        exit_code == 0 ? "Starting Muon..." : "Failed to prepare CEF.", 0, 0,
        0);
  }
  free(cef_path);
  free(result.stage_path);
  free(result.muon_path);
  free(result.cef_path);
  free(options.muon_path);
  free(options.target);
  free(options.cache_dir);
  free(options.cef_version_policy);
  free(options.cef_exact_version);
  free(options.bootstrap_config_dir);
  return exit_code;
}

int muon_prepare_in_place(const char *muon_path, const char *target,
                          const char *cache_dir, int force, int quiet) {
  return muon_prepare_in_place_with_progress(muon_path, target, cache_dir,
                                             force, quiet, NULL, NULL);
}

int muon_prepare_staged_with_progress(
    const char *muon_path, const char *stage_dir, const char *target,
    const char *cache_dir, int force, int quiet,
    MuonPrepareProgressCallback progress_callback, void *progress_user_data) {
  setvbuf(stderr, NULL, _IONBF, 0);
  muon_set_quiet(0);
  PrepareOptions options;
  memset(&options, 0, sizeof(options));
  options.muon_path = muon_duplicate_path_string(muon_path);
  options.stage_dir = muon_duplicate_path_string(stage_dir);
  options.target =
      target == NULL || target[0] == '\0'
          ? muon_duplicate_string(MUON_PREPARE_TARGET_NAME)
          : muon_duplicate_string(target);
  options.cache_dir =
      cache_dir == NULL || cache_dir[0] == '\0'
          ? default_cache_dir()
          : muon_duplicate_path_string(cache_dir);
  options.force = force;
  options.quiet = quiet;
  options.progress_callback = progress_callback;
  options.progress_user_data = progress_user_data;
  set_default_bootstrap_options(&options);
  if (options.muon_path == NULL || options.stage_dir == NULL ||
      options.target == NULL || options.cache_dir == NULL ||
      options.cef_version_policy == NULL || options.cef_exact_version == NULL ||
      validate_public_target(options.target) != 0 ||
      load_bootstrap_config_if_present(&options) != 0) {
    free(options.muon_path);
    free(options.stage_dir);
    free(options.target);
    free(options.cache_dir);
    free(options.cef_version_policy);
    free(options.cef_exact_version);
    free(options.bootstrap_config_dir);
    return 1;
  }
  free(options.bootstrap_config_dir);
  options.bootstrap_config_dir = muon_duplicate_string(options.stage_dir);
  options.write_bootstrap_config = 1;
  if (options.bootstrap_config_dir == NULL) {
    free(options.muon_path);
    free(options.stage_dir);
    free(options.target);
    free(options.cache_dir);
    free(options.cef_version_policy);
    free(options.cef_exact_version);
    return 1;
  }
  const MuonRuntimeInfo *runtime_info = get_embedded_runtime_info();
  PrepareResult result = {0};
  char *cef_path = NULL;
  int exit_code = 1;
  if (runtime_info == NULL) {
    goto cleanup_paths;
  }
  if (strcmp(runtime_info->target, options.target) != 0) {
    muon_print_error("Target mismatch: runtime=%s requested=%s\n",
                     runtime_info->target, options.target);
    goto cleanup_paths;
  }
  if (muon_ensure_directory(options.cache_dir) != 0) {
    goto cleanup_paths;
  }
  muon_set_quiet(quiet);
  const int catalog_required = strcmp(options.cef_version_policy, "exact") == 0;
  if ((policy_uses_catalog(&options) || options.force ||
       options.update_requested) &&
      ensure_catalog_cache(&options, catalog_required) != 0) {
    if (catalog_required) {
      goto cleanup_paths;
    }
    muon_log_message("CEF catalog cache skipped.");
  }
  if (ensure_cef_archive_cache(&options, runtime_info, &cef_path) != 0) {
    goto cleanup_paths;
  }
  if (prepare_staging(&options, runtime_info, cef_path, 1, &result) != 0) {
    goto cleanup_paths;
  }
  if (save_bootstrap_config_if_needed(&options) != 0) {
    goto cleanup_paths;
  }
  exit_code = 0;
cleanup_paths:
  if (options.progress_emitted) {
    prepare_report_progress(
        &options,
        exit_code == 0 ? MUON_PREPARE_PROGRESS_PHASE_DONE
                       : MUON_PREPARE_PROGRESS_PHASE_FAILED,
        exit_code == 0 ? "Starting Muon..." : "Failed to prepare CEF.", 0, 0,
        0);
  }
  free(cef_path);
  free(result.stage_path);
  free(result.muon_path);
  free(result.cef_path);
  free(options.muon_path);
  free(options.stage_dir);
  free(options.target);
  free(options.cache_dir);
  free(options.cef_version_policy);
  free(options.cef_exact_version);
  free(options.bootstrap_config_dir);
  return exit_code;
}

static int run_runtime_command(int argc, char **argv, int start_index) {
  PrepareOptions options;
  if (parse_runtime_arguments(argc, argv, start_index, &options) != 0) {
    print_usage();
    return 1;
  }
  const MuonRuntimeInfo *runtime_info = get_embedded_runtime_info();
  PrepareResult result = {0};
  char *cef_path = NULL;
  int exit_code = 1;
  int cef_is_archive = 0;
  if (runtime_info == NULL) {
    goto cleanup_paths;
  }
  if (strcmp(runtime_info->target, options.target) != 0) {
    muon_print_error("Target mismatch: runtime=%s requested=%s\n",
            runtime_info->target, options.target);
    goto cleanup_paths;
  }
  if (options.cef_path != NULL) {
    cef_path = muon_duplicate_string(options.cef_path);
    if (cef_path == NULL) {
      goto cleanup_paths;
    }
  } else {
    cef_is_archive = 1;
  }
  if (muon_ensure_directory(options.cache_dir) != 0) {
    goto cleanup_paths;
  }
  const int catalog_required = strcmp(options.cef_version_policy, "exact") == 0;
  if (cef_is_archive &&
      (policy_uses_catalog(&options) || options.force ||
       options.update_requested) &&
      ensure_catalog_cache(&options, catalog_required) != 0) {
    if (catalog_required) {
      goto cleanup_paths;
    }
    muon_log_message("CEF catalog cache skipped.");
  }
  if (cef_path == NULL &&
      ensure_cef_archive_cache(&options, runtime_info, &cef_path) != 0) {
    goto cleanup_paths;
  }
  if (prepare_staging(&options, runtime_info, cef_path, cef_is_archive,
                      &result) != 0) {
    goto cleanup_paths;
  }
  if (save_bootstrap_config_if_needed(&options) != 0) {
    goto cleanup_paths;
  }
  if (options.json) {
    print_result_json(&result);
  } else if (result.stage_path != NULL) {
    printf("%s\n", result.stage_path);
  } else {
    printf("%s\n", result.cef_path);
  }
  exit_code = 0;
cleanup_paths:
  free(cef_path);
  free(result.stage_path);
  free(result.muon_path);
  free(result.cef_path);
  free(options.cef_version_policy);
  free(options.cef_exact_version);
  free(options.bootstrap_config_dir);
  return exit_code;
}

static int run_buildtime_command(int argc, char **argv, int start_index) {
  PrepareOptions options;
  if (parse_buildtime_arguments(argc, argv, start_index, &options) != 0) {
    print_usage();
    return 1;
  }
  int exit_code = 1;
  MuonCefArtifact artifact;
  memset(&artifact, 0, sizeof(artifact));
  char *archive_path = NULL;
  const char *cef_target = muon_cef_target_from_public_target(options.target);
  if (cef_target == NULL) {
    goto cleanup_artifact;
  }
  if (muon_ensure_directory(options.cache_dir) != 0) {
    goto cleanup_artifact;
  }
  if (muon_prepare_ensure_catalog_cache(options.cache_dir, options.force) != 0) {
    goto cleanup_artifact;
  }
  if (muon_prepare_resolve_cef_artifact(options.cache_dir,
                                        options.cef_version, cef_target,
                                        "minimal", &artifact) != 0) {
    goto cleanup_artifact;
  }
  if (muon_prepare_ensure_cef_artifact_cache(options.cache_dir, &artifact,
                                             options.force,
                                             &archive_path) != 0) {
    goto cleanup_artifact;
  }
  size_t cef_file_count = 0;
  if (muon_prepare_extract_cef_archive_full(archive_path, options.output_dir,
                                            options.force,
                                            &cef_file_count) != 0) {
    goto cleanup_artifact;
  }
  muon_log_message("CEF files extracted: version=%s target=%s files=%llu",
              artifact.version, options.target,
              (unsigned long long)cef_file_count);
  if (options.json) {
    print_cef_result_json(options.output_dir, archive_path, options.target,
                          &artifact);
  } else {
    printf("%s\n", options.output_dir);
  }
  exit_code = 0;
cleanup_artifact:
  muon_prepare_free_cef_artifact(&artifact);
  free(archive_path);
  free(options.cef_version_policy);
  free(options.cef_exact_version);
  return exit_code;
}

int muon_prepare_main(int argc, char **argv) {
  setvbuf(stderr, NULL, _IONBF, 0);
  muon_set_quiet(is_quiet_requested(argc, argv));
  muon_log_message("muon-builder: %s-%s: Started.", MUON_BUILDER_VERSION,
              MUON_BUILDER_GIT_COMMIT_HASH);
  if (argc < 2 || strcmp(argv[1], "--help") == 0) {
    print_usage();
    return argc < 2 ? 1 : 0;
  }
  if (strcmp(argv[1], "runtime") == 0) {
    return run_runtime_command(argc, argv, 2);
  }
  if (strcmp(argv[1], "buildtime") == 0) {
    return run_buildtime_command(argc, argv, 2);
  }
  muon_print_error("Unknown command: %s\n", argv[1]);
  print_usage();
  return 1;
}
