// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

#include <ctype.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>
#endif

#include "prepare.h"
#include "prepare_cef.h"
#include "prepare_node.h"
#include "launcher_config.h"
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
  int initialized;
#ifdef _WIN32
  CRITICAL_SECTION mutex;
#else
  pthread_mutex_t mutex;
#endif
} PrepareProgressGate;

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
  char *launcher_config_dir;
  char *cache_dir;
  int owns_cache_dir;
  MuonNodeRuntimeRequirement node_runtime_requirement;
  int has_node_runtime_requirement;
  unsigned long long catalog_refresh_interval_seconds;
  unsigned long long cef_last_catalog_update_unix;
  unsigned long long node_last_catalog_update_unix;
  int update_requested;
  unsigned long long update_requested_at_unix;
  int write_launcher_config;
  int force;
  int quiet;
  int json;
  int progress_json;
  MuonPrepareProgressCallback progress_callback;
  void *progress_user_data;
  int progress_emitted;
  PrepareProgressGate progress_gate;
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

typedef struct {
  int applicable;
  int due;
  int updated;
} CatalogRefreshOutcome;

typedef struct {
  PrepareOptions *options;
  MuonNodeArtifact artifact;
  char *archive_path;
  CatalogRefreshOutcome catalog;
  int result;
} NodePrepareTask;

typedef struct {
  char *parent;
  char *raw_key;
  char *lock_path;
  int acquired;
} PrepareRuntimeTransaction;

static const char *kEmptyFingerprint =
    "0000000000000000000000000000000000000000000000000000000000000000";

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
  muon_print_error("Unsupported muon prepare target: %s\n",
                   target == NULL ? "(null)" : target);
  return -1;
}

static int prepare_progress_gate_initialize(PrepareOptions *options) {
  if (options->progress_callback == NULL) {
    return 1;
  }
#ifdef _WIN32
  InitializeCriticalSection(&options->progress_gate.mutex);
  options->progress_gate.initialized = 1;
  return 1;
#else
  const int result = pthread_mutex_init(&options->progress_gate.mutex, NULL);
  if (result != 0) {
    muon_log_message(
        "Progress serialization is unavailable; runtime downloads will run "
        "serially.");
    return 0;
  }
  options->progress_gate.initialized = 1;
  return 1;
#endif
}

static void prepare_progress_gate_lock(PrepareOptions *options) {
  if (!options->progress_gate.initialized) {
    return;
  }
#ifdef _WIN32
  EnterCriticalSection(&options->progress_gate.mutex);
#else
  pthread_mutex_lock(&options->progress_gate.mutex);
#endif
}

static void prepare_progress_gate_unlock(PrepareOptions *options) {
  if (!options->progress_gate.initialized) {
    return;
  }
#ifdef _WIN32
  LeaveCriticalSection(&options->progress_gate.mutex);
#else
  pthread_mutex_unlock(&options->progress_gate.mutex);
#endif
}

static void prepare_progress_gate_dispose(PrepareOptions *options) {
  if (!options->progress_gate.initialized) {
    return;
  }
#ifdef _WIN32
  DeleteCriticalSection(&options->progress_gate.mutex);
#else
  pthread_mutex_destroy(&options->progress_gate.mutex);
#endif
  options->progress_gate.initialized = 0;
}

static void forward_prepare_progress(const MuonPrepareProgress *progress,
                                     void *user_data);

static void prepare_report_progress(const PrepareOptions *options,
                                    MuonPrepareProgressPhase phase,
                                    const char *status,
                                    unsigned long long current,
                                    unsigned long long total,
                                    int determinate) {
  if (options->progress_callback != NULL) {
    MuonPrepareProgress progress;
    progress.phase = phase;
    progress.status = status;
    progress.current = current;
    progress.total = total;
    progress.determinate = determinate;
    forward_prepare_progress(&progress, (void *)options);
  } else if (status != NULL && status[0] != '\0') {
    muon_log_message("%s", status);
  }
}

static void forward_prepare_progress(const MuonPrepareProgress *progress,
                                     void *user_data) {
  PrepareOptions *options = (PrepareOptions *)user_data;
  prepare_progress_gate_lock(options);
  options->progress_emitted = 1;
  options->progress_callback(progress, options->progress_user_data);
  prepare_progress_gate_unlock(options);
}

static MuonPrepareProgressCallback get_prepare_progress_callback(
    const PrepareOptions *options) {
  return options->progress_callback == NULL ? NULL : forward_prepare_progress;
}

static void *get_prepare_progress_user_data(const PrepareOptions *options) {
  return options->progress_callback == NULL ? NULL : (void *)options;
}

static void set_default_launcher_options(PrepareOptions *options) {
  options->cef_version_policy = muon_duplicate_string("tested");
  options->cef_exact_version = muon_duplicate_string("");
  options->catalog_refresh_interval_seconds =
      MUON_LAUNCHER_DEFAULT_CATALOG_REFRESH_INTERVAL_SECONDS;
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
    muon_print_error("Invalid embedded muon runtime metadata.\n");
    return NULL;
  }
  return &kMuonRuntimeInfo;
#else
  muon_print_error(
      "muon runtime metadata is not embedded in this muon-builder build.\n");
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
  size_t extracted_file_count = 0;
  if (muon_extract_tar_bz2_archive_progress(
          archive_path, temporary_directory, 1, &extracted_file_count,
          progress_callback,
          progress_user_data, MUON_PREPARE_PROGRESS_PHASE_INSTALLING,
          "Extracting CEF runtime...") != 0) {
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
  if (result == 0 && file_count != NULL) {
    muon_report_progress(progress_callback, progress_user_data,
                         MUON_PREPARE_PROGRESS_PHASE_INSTALLING,
                         "Installing CEF runtime...",
                         (unsigned long long)*file_count, 0, 0);
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

static int launcher_config_exists(const char *runtime_dir) {
  char *path = muon_path_join(runtime_dir, MUON_LAUNCHER_CONFIG_FILE_NAME);
  if (path == NULL) {
    return 0;
  }
  const int exists = muon_path_exists(path);
  free(path);
  return exists;
}

static int apply_launcher_config(PrepareOptions *options,
                                  const MuonLauncherConfig *config) {
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
  options->cef_last_catalog_update_unix =
      config->cef_last_catalog_update_unix;
  options->node_last_catalog_update_unix =
      config->node_last_catalog_update_unix;
  options->update_requested = config->update_requested;
  options->update_requested_at_unix = config->update_requested_at_unix;
  return 0;
}

static int read_launcher_config_with_embedded_default(
    const char *runtime_dir,
    MuonLauncherConfig *config) {
  char *default_version_policy = NULL;
  if (muon_launcher_get_embedded_default_version_policy(
          &default_version_policy) != 0) {
    return -1;
  }
  const int result = muon_launcher_config_read_with_default(
      runtime_dir, default_version_policy, config);
  free(default_version_policy);
  return result;
}

static int apply_embedded_default_policy_if_needed(PrepareOptions *options) {
  char *default_version_policy = NULL;
  if (muon_launcher_get_embedded_default_version_policy(
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

static int load_launcher_config_if_present(PrepareOptions *options) {
  const char *config_dir = NULL;
  if (options->stage_dir != NULL && launcher_config_exists(options->stage_dir)) {
    config_dir = options->stage_dir;
  } else if (launcher_config_exists(options->muon_path)) {
    config_dir = options->muon_path;
  }
  if (config_dir == NULL) {
    return apply_embedded_default_policy_if_needed(options);
  }
  MuonLauncherConfig config;
  if (read_launcher_config_with_embedded_default(config_dir, &config) != 0) {
    return -1;
  }
  const int result = apply_launcher_config(options, &config);
  muon_launcher_config_free(&config);
  if (result == 0) {
    options->write_launcher_config = 1;
    options->launcher_config_dir = muon_duplicate_string(config_dir);
    if (options->launcher_config_dir == NULL) {
      return -1;
    }
  }
  return result;
}

static int save_launcher_config_to_directory(const PrepareOptions *options,
                                             const char *config_dir) {
  if (!options->write_launcher_config) {
    return 0;
  }
  MuonLauncherConfig config;
  muon_launcher_config_init_defaults(&config);
  if (config.cef_version_policy == NULL || config.cef_exact_version == NULL) {
    muon_launcher_config_free(&config);
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
  config.cef_last_catalog_update_unix =
      options->cef_last_catalog_update_unix;
  config.node_last_catalog_update_unix =
      options->node_last_catalog_update_unix;
  config.update_requested = options->update_requested;
  config.update_requested_at_unix = options->update_requested_at_unix;
  if (config.cef_version_policy == NULL || config.cef_exact_version == NULL) {
    muon_launcher_config_free(&config);
    return -1;
  }
  const int result = muon_launcher_config_write(config_dir, &config);
  muon_launcher_config_free(&config);
  return result;
}

static int save_launcher_config_if_needed(const PrepareOptions *options) {
  const char *config_dir =
      options->launcher_config_dir == NULL ? options->muon_path
                                            : options->launcher_config_dir;
  return save_launcher_config_to_directory(options, config_dir);
}

static int cef_catalog_exists(const char *cache_dir) {
  char *catalog_path =
      muon_path_join(cache_dir, MUON_PREPARE_CEF_CATALOG_FILE_NAME);
  if (catalog_path == NULL) {
    return 0;
  }
  const int exists = muon_path_exists(catalog_path);
  free(catalog_path);
  return exists;
}

static int node_catalog_exists(const char *cache_dir) {
  char *catalog_path =
      muon_path_join(cache_dir, MUON_PREPARE_NODE_CATALOG_FILE_NAME);
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

static int catalog_refresh_due(const PrepareOptions *options, int exists,
                               unsigned long long last_update_unix) {
  if (!exists) {
    return 1;
  }
  if (options->force || options->update_requested) {
    return 1;
  }
  if (options->catalog_refresh_interval_seconds == 0) {
    return 0;
  }
  const unsigned long long now = muon_current_unix_time();
  return last_update_unix == 0 || now < last_update_unix ||
         now - last_update_unix >=
             options->catalog_refresh_interval_seconds;
}

static int ensure_cef_catalog_cache(const PrepareOptions *options,
                                    int catalog_required,
                                    CatalogRefreshOutcome *outcome) {
  PrepareOptions *mutable_options = (PrepareOptions *)options;
  outcome->due =
      catalog_refresh_due(options, cef_catalog_exists(options->cache_dir),
                          options->cef_last_catalog_update_unix);
  if (!outcome->due) {
    return 0;
  }
  int updated = 0;
  const int result =
      muon_prepare_ensure_cef_catalog_cache_with_status_progress(
          options->cache_dir, 1, &updated,
          get_prepare_progress_callback(options),
          get_prepare_progress_user_data(options));
  if (updated) {
    outcome->updated = 1;
    mutable_options->cef_last_catalog_update_unix = muon_current_unix_time();
  }
  if (result == 0 ||
      (!catalog_required && cef_catalog_exists(options->cache_dir))) {
    return 0;
  }
  return -1;
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

static int ensure_node_archive_cache(const PrepareOptions *options,
                                     MuonNodeArtifact *artifact,
                                     char **node_archive_path,
                                     CatalogRefreshOutcome *catalog) {
  memset(artifact, 0, sizeof(*artifact));
  *node_archive_path = NULL;
  if (!options->has_node_runtime_requirement ||
      !options->node_runtime_requirement.required) {
    return 0;
  }

  catalog->applicable = 1;
  catalog->due =
      catalog_refresh_due(options, node_catalog_exists(options->cache_dir),
                          options->node_last_catalog_update_unix);
  int updated = 0;
  int result = 0;
  if (catalog->due) {
    result = muon_prepare_ensure_node_catalog_cache_with_status_progress(
        options->cache_dir, 1, &updated,
        get_prepare_progress_callback(options),
        get_prepare_progress_user_data(options));
    if (updated) {
      catalog->updated = 1;
      ((PrepareOptions *)options)->node_last_catalog_update_unix =
          muon_current_unix_time();
    }
  }
  if (result == 0) {
    result = muon_prepare_resolve_node_artifact(
        options->cache_dir, options->target,
        &options->node_runtime_requirement, artifact);
  }
  if (result == 0) {
    result = muon_prepare_ensure_node_archive_cache_progress(
        options->cache_dir, artifact, options->force, node_archive_path,
        get_prepare_progress_callback(options),
        get_prepare_progress_user_data(options));
  }
  return result;
}

static void run_node_prepare_task(NodePrepareTask *task) {
  task->result = ensure_node_archive_cache(
      task->options, &task->artifact, &task->archive_path, &task->catalog);
}

#ifdef _WIN32
static unsigned __stdcall run_node_prepare_thread(void *argument) {
  run_node_prepare_task((NodePrepareTask *)argument);
  return 0;
}
#else
static void *run_node_prepare_thread(void *argument) {
  run_node_prepare_task((NodePrepareTask *)argument);
  return NULL;
}
#endif

static int prepare_cef_archive_input(
    PrepareOptions *options, const MuonRuntimeInfo *runtime_info,
    int cef_is_archive, char **cef_path, CatalogRefreshOutcome *catalog) {
  const int catalog_required =
      strcmp(options->cef_version_policy, "exact") == 0;
  catalog->applicable =
      cef_is_archive &&
      (policy_uses_catalog(options) || options->force ||
       options->update_requested);
  if (catalog->applicable &&
      ensure_cef_catalog_cache(options, catalog_required, catalog) != 0) {
    if (catalog_required) {
      return -1;
    }
    muon_log_message("CEF catalog cache skipped.");
  }
  if (*cef_path == NULL &&
      ensure_cef_archive_cache(options, runtime_info, cef_path) != 0) {
    return -1;
  }
  return 0;
}

static void clear_catalog_update_request_if_satisfied(
    PrepareOptions *options, const CatalogRefreshOutcome *cef_catalog,
    const CatalogRefreshOutcome *node_catalog) {
  if (!options->update_requested) {
    return;
  }
  const int any_applicable =
      cef_catalog->applicable || node_catalog->applicable;
  const int every_applicable_updated =
      (!cef_catalog->applicable || cef_catalog->updated) &&
      (!node_catalog->applicable || node_catalog->updated);
  if (any_applicable && every_applicable_updated) {
    options->update_requested = 0;
    options->update_requested_at_unix = 0;
  }
}

static int prepare_runtime_archive_inputs(
    PrepareOptions *options, const MuonRuntimeInfo *runtime_info,
    int cef_is_archive, char **cef_path, int allow_parallel,
    NodePrepareTask *node_task) {
  memset(node_task, 0, sizeof(*node_task));
  node_task->options = options;
  CatalogRefreshOutcome cef_catalog = {0};
  const int node_required =
      options->has_node_runtime_requirement &&
      options->node_runtime_requirement.required;
  int thread_started = 0;
  int thread_join_result = 0;
#ifdef _WIN32
  HANDLE thread = NULL;
  if (allow_parallel && node_required) {
    const uintptr_t thread_value =
        _beginthreadex(NULL, 0, run_node_prepare_thread, node_task, 0, NULL);
    thread = thread_value == 0 ? NULL : (HANDLE)thread_value;
    thread_started = thread != NULL;
  }
#else
  pthread_t thread;
  if (allow_parallel && node_required) {
    thread_started =
        pthread_create(&thread, NULL, run_node_prepare_thread, node_task) == 0;
  }
#endif

  const int cef_result = prepare_cef_archive_input(
      options, runtime_info, cef_is_archive, cef_path, &cef_catalog);
  if (thread_started) {
#ifdef _WIN32
    const DWORD wait_result = WaitForSingleObject(thread, INFINITE);
    if (wait_result != WAIT_OBJECT_0) {
      muon_print_error("Failed to wait for Node.js preparation thread: %lu\n",
                       (unsigned long)GetLastError());
      abort();
    }
    CloseHandle(thread);
#else
    const int join_result = pthread_join(thread, NULL);
    if (join_result != 0) {
      muon_print_error("Failed to join Node.js preparation thread: %s\n",
                       strerror(join_result));
      abort();
    }
#endif
  } else if (node_required) {
    run_node_prepare_task(node_task);
  }

  if (cef_result != 0 || thread_join_result != 0 ||
      node_task->result != 0) {
    return -1;
  }
  clear_catalog_update_request_if_satisfied(
      options, &cef_catalog, &node_task->catalog);
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
  if (result == 0 && file_count != NULL) {
    muon_report_progress(progress_callback, progress_user_data,
                         MUON_PREPARE_PROGRESS_PHASE_INSTALLING,
                         "Installing CEF runtime...",
                         (unsigned long long)*file_count, 0, 0);
  }
  return result;
}

static void finalize_sha256_hex(SHA256_CTX *context,
                                char output[SHA256_DIGEST_STRING_LENGTH]) {
  uint8_t digest[SHA256_DIGEST_LENGTH];
  SHA256_Final(digest, context);
  muon_sha256_digest_to_hex(digest, output);
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
  static const char *const runtime_lock_prefix = ".muon-runtime-";
  static const char *const lock_suffix = ".lock";
  const size_t name_length = strlen(name);
  const size_t prefix_length = strlen(runtime_lock_prefix);
  const size_t suffix_length = strlen(lock_suffix);
  if (name_length >= prefix_length + suffix_length &&
      strncmp(name, runtime_lock_prefix, prefix_length) == 0 &&
      strcmp(name + name_length - suffix_length, lock_suffix) == 0) {
    return 1;
  }
  static const char *const generated_entries[] = {
      ".muon-ready.json",
      ".muon-runtime-ready.json",
      ".muon-cef-ready.json",
      ".muon-test-config",
      "muon-launcher.ini",
      "muon-cef.log",
      "muon-close-debug.log",
      "muon-runtime-helper",
      "runtimes",
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
    char fingerprint[SHA256_DIGEST_STRING_LENGTH]) {
  struct stat entry;
  if (stat(path, &entry) != 0) {
    muon_print_errno(path);
    return -1;
  }
  SHA256_CTX context;
  SHA256_Init(&context);
  muon_sha256_update_string(&context, relative);
  muon_sha256_update_string(&context, ":");
  if (S_ISDIR(entry.st_mode)) {
    muon_sha256_update_string(&context, "directory");
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
      char child_fingerprint[SHA256_DIGEST_STRING_LENGTH];
      if (fingerprint_staging_muon_source_recursive(
              child_path, child_relative, child_fingerprint) != 0) {
        free(child_path);
        free(child_relative);
        prepare_name_list_free(&children);
        return -1;
      }
      muon_sha256_update_string(&context, children.values[index]);
      muon_sha256_update_string(&context, child_fingerprint);
      free(child_path);
      free(child_relative);
    }
    prepare_name_list_free(&children);
  } else if (S_ISREG(entry.st_mode)) {
    char content_fingerprint[SHA256_DIGEST_STRING_LENGTH];
    if (muon_sha256_file_hex(path, content_fingerprint) != 0) {
      return -1;
    }
    muon_sha256_update_string(&context, "file");
    muon_sha256_update_string(&context, content_fingerprint);
  } else {
    muon_sha256_update_string(&context, "other");
  }
  finalize_sha256_hex(&context, fingerprint);
  return 0;
}

static int fingerprint_staging_muon_source(
    const char *path, char fingerprint[SHA256_DIGEST_STRING_LENGTH]) {
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
                                  char fingerprint[SHA256_DIGEST_STRING_LENGTH]) {
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
    SHA256_CTX context;
    SHA256_Init(&context);
    if (has_release) {
      char release_fingerprint[SHA256_DIGEST_STRING_LENGTH];
      result =
          muon_fingerprint_directory_contents(release_path, release_fingerprint);
      muon_sha256_update_string(&context, "Release");
      muon_sha256_update_string(&context, release_fingerprint);
    }
    if (result == 0 && has_resources) {
      char resource_fingerprint[SHA256_DIGEST_STRING_LENGTH];
      result =
          muon_fingerprint_directory_contents(resource_path, resource_fingerprint);
      muon_sha256_update_string(&context, "Resources");
      muon_sha256_update_string(&context, resource_fingerprint);
    }
    if (result == 0) {
      finalize_sha256_hex(&context, fingerprint);
    }
  } else {
    result = muon_fingerprint_directory_contents(cef_path, fingerprint);
  }
  free(release_path);
  free(resource_path);
  return result;
}

static int fingerprint_archive_metadata(
    const char *archive_path, char fingerprint[SHA256_DIGEST_STRING_LENGTH]) {
  struct stat entry;
  if (stat(archive_path, &entry) != 0) {
    muon_print_errno(archive_path);
    return -1;
  }
  SHA256_CTX context;
  SHA256_Init(&context);
  muon_sha256_update_string(&context, archive_path);
  char metadata[128];
  snprintf(metadata, sizeof(metadata), ":%llu:%lld:%d",
           (unsigned long long)entry.st_size, (long long)entry.st_mtime,
           (int)(entry.st_mode & 0777));
  muon_sha256_update_string(&context, metadata);
  finalize_sha256_hex(&context, fingerprint);
  return 0;
}

static int fingerprint_staging_cef_source(
    const char *cef_path, int cef_is_archive,
    char fingerprint[SHA256_DIGEST_STRING_LENGTH]) {
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

static void dispose_prepare_runtime_transaction(
    PrepareRuntimeTransaction *transaction) {
  if (transaction->acquired) {
    muon_release_lock(transaction->lock_path);
  }
  free(transaction->parent);
  free(transaction->raw_key);
  free(transaction->lock_path);
  memset(transaction, 0, sizeof(*transaction));
}

static int begin_prepare_runtime_transaction(
    const PrepareOptions *options, const char *parent, const char *prefix,
    const char *key_source, PrepareRuntimeTransaction *transaction) {
  memset(transaction, 0, sizeof(*transaction));
  transaction->parent = muon_duplicate_string(parent);
  transaction->raw_key = sanitize_key(key_source);
  transaction->lock_path =
      transaction->parent == NULL || transaction->raw_key == NULL
          ? NULL
          : create_hidden_lock_path(transaction->parent, prefix,
                                    transaction->raw_key);
  if (transaction->parent == NULL || transaction->raw_key == NULL ||
      transaction->lock_path == NULL ||
      muon_acquire_lock_with_progress(
          transaction->lock_path, get_prepare_progress_callback(options),
          get_prepare_progress_user_data(options),
          MUON_PREPARE_PROGRESS_PHASE_INSTALLING,
          "Installing CEF runtime...") != 0) {
    dispose_prepare_runtime_transaction(transaction);
    return -1;
  }
  transaction->acquired = 1;
  return 0;
}

static int begin_staged_prepare_runtime_transaction(
    const PrepareOptions *options, PrepareRuntimeTransaction *transaction) {
  char *parent = muon_parent_directory(options->stage_dir);
  if (parent == NULL) {
    return -1;
  }
  const int result = begin_prepare_runtime_transaction(
      options, parent, "muon-stage", options->stage_dir, transaction);
  free(parent);
  return result;
}

static int begin_in_place_prepare_runtime_transaction(
    const PrepareOptions *options, PrepareRuntimeTransaction *transaction) {
  return begin_prepare_runtime_transaction(
      options, options->muon_path, "muon-runtime", options->muon_path,
      transaction);
}

static char *create_runtime_ready_content(const char *muon_fingerprint,
                                          const char *cef_fingerprint,
                                          const char *node_fingerprint) {
  const int size = snprintf(
      NULL, 0,
      "{\"ready\":true,\"muonFingerprint\":\"%s\","
      "\"cefFingerprint\":\"%s\",\"nodeFingerprint\":\"%s\"}\n",
      muon_fingerprint, cef_fingerprint, node_fingerprint);
  if (size < 0) {
    return NULL;
  }
  char *result = (char *)malloc((size_t)size + 1);
  if (result == NULL) {
    return NULL;
  }
  snprintf(result, (size_t)size + 1,
           "{\"ready\":true,\"muonFingerprint\":\"%s\","
           "\"cefFingerprint\":\"%s\",\"nodeFingerprint\":\"%s\"}\n",
           muon_fingerprint, cef_fingerprint, node_fingerprint);
  return result;
}

static int prepare_staging_locked(
    const PrepareOptions *options, const MuonRuntimeInfo *runtime_info,
    const char *cef_path, int cef_is_archive, const char *node_archive_path,
    const MuonNodeArtifact *node_artifact,
    const PrepareRuntimeTransaction *transaction, PrepareResult *result) {
  char muon_fingerprint[SHA256_DIGEST_STRING_LENGTH];
  char cef_fingerprint[SHA256_DIGEST_STRING_LENGTH];
  if (fingerprint_staging_muon_source(options->muon_path, muon_fingerprint) !=
          0 ||
      fingerprint_staging_cef_source(cef_path, cef_is_archive,
                                     cef_fingerprint) != 0) {
    return -1;
  }
  const char *node_fingerprint =
      node_archive_path == NULL ? kEmptyFingerprint : node_artifact->sha256;
  if (node_fingerprint == NULL) {
    return -1;
  }
  char *ready_content = create_runtime_ready_content(
      muon_fingerprint, cef_fingerprint, node_fingerprint);
  char *ready_path =
      muon_path_join(options->stage_dir, ".muon-runtime-ready.json");
  char *temporary_directory = NULL;
  char *temporary_ready = NULL;
  int temporary_published = 0;
  int prepare_result = -1;
  if (ready_content == NULL || ready_path == NULL ||
      transaction == NULL || !transaction->acquired) {
    goto cleanup;
  }
  if (ensure_muon_gitignore_entry(options->stage_dir) != 0) {
    goto cleanup;
  }
  if (!options->force && muon_path_exists(ready_path) &&
      muon_ready_file_matches(ready_path, ready_content)) {
    if (save_launcher_config_to_directory(options, options->stage_dir) != 0) {
      goto cleanup;
    }
    prepare_result = set_prepare_result(
        result, options->stage_dir, options->muon_path, cef_path, 1);
    goto cleanup;
  }
  temporary_directory = muon_create_temporary_path(
      transaction->parent, transaction->raw_key);
  temporary_ready =
      temporary_directory == NULL
          ? NULL
          : muon_path_join(temporary_directory,
                           ".muon-runtime-ready.json");
  if (temporary_directory == NULL || temporary_ready == NULL ||
      muon_ensure_directory(temporary_directory) != 0) {
    goto cleanup;
  }
  size_t cef_file_count = 0;
  size_t muon_file_count = 0;
  size_t node_file_count = 0;
  prepare_report_progress(options, MUON_PREPARE_PROGRESS_PHASE_INSTALLING,
                          "Installing CEF runtime...", 0, 0, 0);
  if (copy_staging_muon_source(options->muon_path, temporary_directory,
                               &muon_file_count) != 0) {
    goto cleanup;
  }
  muon_log_message("muon files copied to staging: files=%llu",
                   (unsigned long long)muon_file_count);
  if ((cef_is_archive
           ? extract_archive(cef_path, temporary_directory, &cef_file_count,
                             get_prepare_progress_callback(options),
                             get_prepare_progress_user_data(options))
           : copy_cef_source(cef_path, temporary_directory, &cef_file_count,
                             get_prepare_progress_callback(options),
                             get_prepare_progress_user_data(options))) != 0) {
    goto cleanup;
  }
  muon_log_message("CEF files copied to staging: version=%s files=%llu",
                   runtime_info->cef_reference_version,
                   (unsigned long long)cef_file_count);
  if (node_archive_path != NULL &&
      muon_prepare_install_node_runtime_progress(
          node_archive_path, options->target, node_artifact,
          temporary_directory, &node_file_count,
          get_prepare_progress_callback(options),
          get_prepare_progress_user_data(options)) != 0) {
    goto cleanup;
  }
  if (node_archive_path != NULL) {
    muon_log_message("Node.js files copied to staging: version=%s files=%llu",
                     node_artifact->version,
                     (unsigned long long)node_file_count);
  }
  if (save_launcher_config_to_directory(options, temporary_directory) != 0 ||
      muon_write_text_file(temporary_ready, ready_content) != 0) {
    goto cleanup;
  }
  prepare_report_progress(options, MUON_PREPARE_PROGRESS_PHASE_FINALIZING,
                          "Starting muon...", 0, 0, 0);
  if (muon_path_exists(options->stage_dir) &&
      muon_remove_recursive(options->stage_dir) != 0) {
    goto cleanup;
  }
  if (rename(temporary_directory, options->stage_dir) != 0) {
    muon_print_errno(options->stage_dir);
    goto cleanup;
  }
  temporary_published = 1;
  prepare_result =
      set_prepare_result(result, options->stage_dir, options->muon_path,
                         cef_path, 0);

cleanup:
  if (!temporary_published) {
    muon_remove_recursive(temporary_directory);
  }
  free(ready_content);
  free(ready_path);
  free(temporary_directory);
  free(temporary_ready);
  return prepare_result;
}

static int prepare_runtime_in_place_locked(
    const PrepareOptions *options, const MuonRuntimeInfo *runtime_info,
    const char *cef_path, int cef_is_archive, const char *node_archive_path,
    const MuonNodeArtifact *node_artifact, PrepareResult *result) {
  char cef_fingerprint[SHA256_DIGEST_STRING_LENGTH];
  if ((cef_is_archive
           ? muon_fingerprint_path_recursive(cef_path, "", cef_fingerprint)
           : fingerprint_cef_source(cef_path, cef_fingerprint)) != 0) {
    return -1;
  }
  const char *node_fingerprint =
      node_archive_path == NULL ? kEmptyFingerprint : node_artifact->sha256;
  if (node_fingerprint == NULL) {
    return -1;
  }
  char *ready_content = create_runtime_ready_content(
      kEmptyFingerprint, cef_fingerprint, node_fingerprint);
  char *ready_path =
      muon_path_join(options->muon_path, ".muon-runtime-ready.json");
  if (ready_content == NULL || ready_path == NULL) {
    free(ready_content);
    free(ready_path);
    return -1;
  }
  if (!options->force && muon_path_exists(ready_path) &&
      muon_ready_file_matches(ready_path, ready_content)) {
    const int set_result =
        save_launcher_config_to_directory(options, options->muon_path) == 0
            ? set_prepare_result(result, options->muon_path,
                                 options->muon_path, cef_path, 1)
            : -1;
    free(ready_content);
    free(ready_path);
    return set_result;
  }
  int prepare_result = -1;
  if (muon_remove_recursive(ready_path) != 0) {
    goto cleanup;
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
    goto cleanup;
  }
  muon_log_message("CEF files copied to runtime: version=%s files=%llu",
                   runtime_info->cef_reference_version,
                   (unsigned long long)cef_file_count);
  size_t node_file_count = 0;
  int node_result = 0;
  if (node_archive_path != NULL) {
    node_result = muon_prepare_install_node_runtime_progress(
        node_archive_path, options->target, node_artifact,
        options->muon_path, &node_file_count,
        get_prepare_progress_callback(options),
        get_prepare_progress_user_data(options));
  } else {
    char *runtimes_path = muon_path_join(options->muon_path, "runtimes");
    char *node_path =
        runtimes_path == NULL ? NULL : muon_path_join(runtimes_path, "node");
    node_result =
        node_path == NULL ? -1 : muon_remove_recursive(node_path);
    free(runtimes_path);
    free(node_path);
  }
  if (node_result != 0) {
    goto cleanup;
  }
  if (node_archive_path != NULL) {
    muon_log_message("Node.js files copied to runtime: version=%s files=%llu",
                     node_artifact->version,
                     (unsigned long long)node_file_count);
  }
  if (save_launcher_config_to_directory(options, options->muon_path) != 0 ||
      muon_write_text_file(ready_path, ready_content) != 0) {
    goto cleanup;
  }
  prepare_report_progress(options, MUON_PREPARE_PROGRESS_PHASE_FINALIZING,
                          "Starting muon...", 0, 0, 0);
  prepare_result = set_prepare_result(result, options->muon_path,
                                      options->muon_path, cef_path, 0);

cleanup:
  free(ready_content);
  free(ready_path);
  return prepare_result;
}

static int configure_staged_launcher_state(PrepareOptions *options) {
  if (load_launcher_config_if_present(options) != 0) {
    return -1;
  }
  char *launcher_config_dir =
      muon_duplicate_path_string(options->stage_dir);
  if (launcher_config_dir == NULL) {
    return -1;
  }
  free(options->launcher_config_dir);
  options->launcher_config_dir = launcher_config_dir;
  options->write_launcher_config = 1;
  return 0;
}

static int prepare_staged_runtime_transaction(
    PrepareOptions *options, const MuonRuntimeInfo *runtime_info,
    int cef_is_archive, char **cef_path, int allow_parallel,
    NodePrepareTask *node_task, PrepareResult *result) {
  PrepareRuntimeTransaction transaction = {0};
  int prepare_result = -1;
  if (begin_staged_prepare_runtime_transaction(options, &transaction) != 0) {
    return -1;
  }
  if (configure_staged_launcher_state(options) != 0) {
    goto cleanup;
  }
  muon_set_quiet(options->quiet);
  if (prepare_runtime_archive_inputs(options, runtime_info, cef_is_archive,
                                     cef_path, allow_parallel, node_task) != 0 ||
      prepare_staging_locked(
          options, runtime_info, *cef_path, cef_is_archive,
          node_task->archive_path, &node_task->artifact, &transaction,
          result) != 0) {
    goto cleanup;
  }
  prepare_result = 0;

cleanup:
  dispose_prepare_runtime_transaction(&transaction);
  return prepare_result;
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

static const char *progress_phase_name(MuonPrepareProgressPhase phase) {
  switch (phase) {
  case MUON_PREPARE_PROGRESS_PHASE_CHECKING:
    return "checking";
  case MUON_PREPARE_PROGRESS_PHASE_DOWNLOADING:
    return "downloading";
  case MUON_PREPARE_PROGRESS_PHASE_VERIFYING:
    return "verifying";
  case MUON_PREPARE_PROGRESS_PHASE_INSTALLING:
    return "installing";
  case MUON_PREPARE_PROGRESS_PHASE_FINALIZING:
    return "finalizing";
  case MUON_PREPARE_PROGRESS_PHASE_DONE:
    return "done";
  case MUON_PREPARE_PROGRESS_PHASE_FAILED:
    return "failed";
  }
  return "unknown";
}

static void print_progress_json(const MuonPrepareProgress *progress,
                                void *user_data) {
  (void)user_data;
  yyjson_mut_doc *document = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *root = document == NULL ? NULL : yyjson_mut_obj(document);
  if (document == NULL || root == NULL) {
    yyjson_mut_doc_free(document);
    return;
  }
  yyjson_mut_doc_set_root(document, root);
  const int ok =
      yyjson_mut_obj_add_strcpy(document, root, "phase",
                                progress_phase_name(progress->phase)) &&
      yyjson_mut_obj_add_strcpy(
          document, root, "status",
          progress->status == NULL ? "" : progress->status) &&
      yyjson_mut_obj_add_uint(document, root, "current", progress->current) &&
      yyjson_mut_obj_add_uint(document, root, "total", progress->total) &&
      yyjson_mut_obj_add_bool(document, root, "determinate",
                              progress->determinate);
  char *json =
      ok ? yyjson_mut_write(document, YYJSON_WRITE_NEWLINE_AT_END, NULL) : NULL;
  if (json != NULL) {
    fwrite(json, 1, strlen(json), stderr);
    free(json);
  }
  yyjson_mut_doc_free(document);
  fflush(stderr);
}

static void print_usage(void) {
  muon_print_error(
      "Usage: muon-builder <command> [options]\n"
      "       muon-builder runtime --muon-path <path> [--cef-path <path>] "
      "[--stage-dir <path>] [--target <target>] [--cache-dir <path>] "
      "[--node-runtime-requirement <json>] "
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
  set_default_launcher_options(options);
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
    } else if (strcmp(argv[index], "--node-runtime-requirement") == 0 &&
               index + 1 < argc) {
      if (options->has_node_runtime_requirement) {
        muon_print_error("--node-runtime-requirement may only be specified "
                         "once.\n");
        return -1;
      }
      if (muon_prepare_parse_node_runtime_requirement(
              argv[++index], &options->node_runtime_requirement) != 0) {
        return -1;
      }
      options->has_node_runtime_requirement = 1;
    } else if (strcmp(argv[index], "--force") == 0) {
      options->force = 1;
    } else if (strcmp(argv[index], "--quiet") == 0 ||
               strcmp(argv[index], "-q") == 0) {
      options->quiet = 1;
    } else if (strcmp(argv[index], "--json") == 0) {
      options->json = 1;
    } else if (strcmp(argv[index], "--progress-json") == 0) {
      options->progress_json = 1;
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
    options->owns_cache_dir = 1;
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
  if (options->stage_dir != NULL) {
    return 0;
  }
  return load_launcher_config_if_present(options);
}

static int parse_buildtime_arguments(int argc, char **argv, int start_index,
                                     PrepareOptions *options) {
  memset(options, 0, sizeof(*options));
  set_default_launcher_options(options);
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
    options->owns_cache_dir = 1;
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
  PrepareOptions options = {0};
  PrepareResult result = {0};
  NodePrepareTask node_task = {0};
  PrepareRuntimeTransaction transaction = {0};
  char *cef_path = NULL;
  int exit_code = 1;
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
  set_default_launcher_options(&options);
  if (options.muon_path == NULL || options.target == NULL ||
      options.cache_dir == NULL || options.cef_version_policy == NULL ||
      options.cef_exact_version == NULL ||
      validate_public_target(options.target) != 0) {
    goto cleanup_paths;
  }
  if (muon_launcher_get_embedded_node_runtime_requirement(
          &options.node_runtime_requirement,
          &options.has_node_runtime_requirement) != 0) {
    goto cleanup_paths;
  }
  const MuonRuntimeInfo *runtime_info = get_embedded_runtime_info();
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
  const int allow_parallel = prepare_progress_gate_initialize(&options);
  if (begin_in_place_prepare_runtime_transaction(&options, &transaction) !=
      0) {
    goto cleanup_paths;
  }
  MuonLauncherConfig launcher_config;
  if (read_launcher_config_with_embedded_default(options.muon_path,
                                                 &launcher_config) != 0) {
    goto cleanup_paths;
  }
  if (apply_launcher_config(&options, &launcher_config) != 0) {
    muon_launcher_config_free(&launcher_config);
    goto cleanup_paths;
  }
  muon_launcher_config_free(&launcher_config);
  options.write_launcher_config = 1;
  muon_set_quiet(quiet);
  if (prepare_runtime_archive_inputs(&options, runtime_info, 1, &cef_path,
                                     allow_parallel, &node_task) != 0) {
    goto cleanup_paths;
  }
  if (prepare_runtime_in_place_locked(
          &options, runtime_info, cef_path, 1, node_task.archive_path,
          &node_task.artifact, &result) != 0) {
    goto cleanup_paths;
  }
  exit_code = 0;
cleanup_paths:
  dispose_prepare_runtime_transaction(&transaction);
  if (options.progress_emitted) {
    prepare_report_progress(
        &options,
        exit_code == 0 ? MUON_PREPARE_PROGRESS_PHASE_DONE
                       : MUON_PREPARE_PROGRESS_PHASE_FAILED,
        exit_code == 0 ? "Starting muon..." : "Failed to prepare runtime.", 0,
        0, 0);
  }
  prepare_progress_gate_dispose(&options);
  muon_prepare_free_node_artifact(&node_task.artifact);
  free(node_task.archive_path);
  free(cef_path);
  free(result.stage_path);
  free(result.muon_path);
  free(result.cef_path);
  free(options.muon_path);
  free(options.target);
  free(options.cache_dir);
  free(options.cef_version_policy);
  free(options.cef_exact_version);
  free(options.launcher_config_dir);
  muon_prepare_free_node_runtime_requirement(
      &options.node_runtime_requirement);
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
  PrepareOptions options = {0};
  PrepareResult result = {0};
  NodePrepareTask node_task = {0};
  char *cef_path = NULL;
  int exit_code = 1;
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
  set_default_launcher_options(&options);
  if (options.muon_path == NULL || options.stage_dir == NULL ||
      options.target == NULL || options.cache_dir == NULL ||
      options.cef_version_policy == NULL || options.cef_exact_version == NULL ||
      validate_public_target(options.target) != 0) {
    goto cleanup_paths;
  }
  if (muon_launcher_get_embedded_node_runtime_requirement(
          &options.node_runtime_requirement,
          &options.has_node_runtime_requirement) != 0) {
    goto cleanup_paths;
  }
  const MuonRuntimeInfo *runtime_info = get_embedded_runtime_info();
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
  const int allow_parallel = prepare_progress_gate_initialize(&options);
  if (prepare_staged_runtime_transaction(
          &options, runtime_info, 1, &cef_path, allow_parallel, &node_task,
          &result) != 0) {
    goto cleanup_paths;
  }
  exit_code = 0;
cleanup_paths:
  if (options.progress_emitted) {
    prepare_report_progress(
        &options,
        exit_code == 0 ? MUON_PREPARE_PROGRESS_PHASE_DONE
                       : MUON_PREPARE_PROGRESS_PHASE_FAILED,
        exit_code == 0 ? "Starting muon..." : "Failed to prepare runtime.", 0,
        0, 0);
  }
  prepare_progress_gate_dispose(&options);
  muon_prepare_free_node_artifact(&node_task.artifact);
  free(node_task.archive_path);
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
  free(options.launcher_config_dir);
  muon_prepare_free_node_runtime_requirement(
      &options.node_runtime_requirement);
  return exit_code;
}

static int run_runtime_command(int argc, char **argv, int start_index) {
  PrepareOptions options = {0};
  if (parse_runtime_arguments(argc, argv, start_index, &options) != 0) {
    if (options.owns_cache_dir) {
      free(options.cache_dir);
    }
    free(options.cef_version_policy);
    free(options.cef_exact_version);
    free(options.launcher_config_dir);
    muon_prepare_free_node_runtime_requirement(
        &options.node_runtime_requirement);
    print_usage();
    return 1;
  }
  if (options.progress_json && !options.quiet) {
    options.progress_callback = print_progress_json;
    options.progress_user_data = NULL;
  }
  const MuonRuntimeInfo *runtime_info = get_embedded_runtime_info();
  PrepareResult result = {0};
  char *cef_path = NULL;
  NodePrepareTask node_task = {0};
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
  const int allow_parallel = prepare_progress_gate_initialize(&options);
  if (options.stage_dir != NULL) {
    if (prepare_staged_runtime_transaction(
            &options, runtime_info, cef_is_archive, &cef_path, allow_parallel,
            &node_task, &result) != 0) {
      goto cleanup_paths;
    }
  } else {
    if (prepare_runtime_archive_inputs(
            &options, runtime_info, cef_is_archive, &cef_path, allow_parallel,
            &node_task) != 0 ||
        set_prepare_result(&result, NULL, options.muon_path, cef_path, 0) != 0 ||
        save_launcher_config_if_needed(&options) != 0) {
      goto cleanup_paths;
    }
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
  prepare_progress_gate_dispose(&options);
  muon_prepare_free_node_artifact(&node_task.artifact);
  free(node_task.archive_path);
  free(cef_path);
  free(result.stage_path);
  free(result.muon_path);
  free(result.cef_path);
  if (options.owns_cache_dir) {
    free(options.cache_dir);
  }
  free(options.cef_version_policy);
  free(options.cef_exact_version);
  free(options.launcher_config_dir);
  muon_prepare_free_node_runtime_requirement(
      &options.node_runtime_requirement);
  return exit_code;
}

static int run_buildtime_command(int argc, char **argv, int start_index) {
  PrepareOptions options = {0};
  if (parse_buildtime_arguments(argc, argv, start_index, &options) != 0) {
    if (options.owns_cache_dir) {
      free(options.cache_dir);
    }
    free(options.cef_version_policy);
    free(options.cef_exact_version);
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
  if (muon_prepare_ensure_cef_catalog_cache(options.cache_dir,
                                            options.force) != 0) {
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
  if (options.owns_cache_dir) {
    free(options.cache_dir);
  }
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
