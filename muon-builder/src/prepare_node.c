// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "prepare_node.h"
#include "common.h"

static const char *kDefaultNodeDistUrl = "https://nodejs.org/dist";
static const char *kNodeChecksumFileName = "SHASUMS256.txt";
static const int kCurlDownloadAttemptCount = 4;

typedef enum {
  MUON_NODE_COMPARATOR_EQUAL,
  MUON_NODE_COMPARATOR_LESS,
  MUON_NODE_COMPARATOR_LESS_EQUAL,
  MUON_NODE_COMPARATOR_GREATER,
  MUON_NODE_COMPARATOR_GREATER_EQUAL
} MuonNodeComparatorOperator;

typedef struct {
  unsigned long long major;
  unsigned long long minor;
  unsigned long long patch;
  const char *prerelease;
  size_t prerelease_length;
} MuonNodeSemVersion;

typedef struct {
  MuonNodeComparatorOperator operator_value;
  MuonNodeSemVersion version;
} MuonParsedNodeComparator;

typedef struct {
  char **values;
  size_t count;
  size_t capacity;
} MuonNodeStringList;

typedef struct {
  char *version;
  char *lts;
} MuonNodeReleaseSelection;

typedef struct {
  const char *target;
  MuonNodeTargetInfo info;
} MuonNodeTargetMapping;

static const MuonNodeTargetMapping kNodeTargetMappings[] = {
    {"linux-amd64", {"linux-x64", "linux-x64", ".tar.gz", "bin/node"}},
    {"linux-armhf",
     {"linux-armv7l", "linux-armv7l", ".tar.gz", "bin/node"}},
    {"linux-arm64", {"linux-arm64", "linux-arm64", ".tar.gz", "bin/node"}},
    {"windows-i686",
     {"win-x86-zip", "win-x86", ".zip", "bin/node.exe"}},
    {"windows-amd64",
     {"win-x64-zip", "win-x64", ".zip", "bin/node.exe"}}};

static int is_ascii_digit(char value) {
  return value >= '0' && value <= '9';
}

static int is_ascii_alphanumeric(char value) {
  return is_ascii_digit(value) ||
         (value >= 'A' && value <= 'Z') ||
         (value >= 'a' && value <= 'z');
}

static int json_value_is_c_string(yyjson_val *value) {
  if (value == NULL || !yyjson_is_str(value)) {
    return 0;
  }
  const char *text = yyjson_get_str(value);
  return text != NULL && strlen(text) == yyjson_get_len(value);
}

static int object_get_single_value(yyjson_val *object, const char *name,
                                   yyjson_val **result) {
  size_t match_count = 0;
  yyjson_val *matched = NULL;
  size_t index = 0;
  size_t max = 0;
  yyjson_val *key = NULL;
  yyjson_val *value = NULL;
  yyjson_obj_foreach(object, index, max, key, value) {
    if (json_value_is_c_string(key) &&
        strcmp(yyjson_get_str(key), name) == 0) {
      match_count += 1;
      matched = value;
    }
  }
  if (match_count != 1) {
    return -1;
  }
  *result = matched;
  return 0;
}

static int parse_semver_number(const char **cursor,
                               unsigned long long *value) {
  const char *start = *cursor;
  if (!is_ascii_digit(*start)) {
    return -1;
  }
  if (*start == '0' && is_ascii_digit(start[1])) {
    return -1;
  }
  unsigned long long number = 0;
  while (is_ascii_digit(**cursor)) {
    const unsigned int digit = (unsigned int)(**cursor - '0');
    if (number > (ULLONG_MAX - digit) / 10ULL) {
      return -1;
    }
    number = number * 10ULL + digit;
    *cursor += 1;
  }
  *value = number;
  return 0;
}

static int prerelease_identifier_is_numeric(const char *value,
                                            size_t length) {
  if (length == 0) {
    return 0;
  }
  for (size_t index = 0; index < length; index += 1) {
    if (!is_ascii_digit(value[index])) {
      return 0;
    }
  }
  return 1;
}

static int validate_prerelease(const char *value, size_t length) {
  size_t identifier_start = 0;
  for (size_t index = 0; index <= length; index += 1) {
    const int at_end = index == length;
    const char character = at_end ? '.' : value[index];
    if (!at_end &&
        !(is_ascii_alphanumeric(character) || character == '-' ||
          character == '.')) {
      return -1;
    }
    if (character != '.') {
      continue;
    }
    const size_t identifier_length = index - identifier_start;
    if (identifier_length == 0) {
      return -1;
    }
    if (prerelease_identifier_is_numeric(value + identifier_start,
                                         identifier_length) &&
        identifier_length > 1 && value[identifier_start] == '0') {
      return -1;
    }
    identifier_start = index + 1;
  }
  return 0;
}

static int parse_semver(const char *text, int require_v,
                        MuonNodeSemVersion *version) {
  memset(version, 0, sizeof(*version));
  if (text == NULL) {
    return -1;
  }
  const char *cursor = text;
  if (require_v) {
    if (*cursor != 'v') {
      return -1;
    }
    cursor += 1;
  } else if (*cursor == 'v') {
    return -1;
  }
  if (parse_semver_number(&cursor, &version->major) != 0 ||
      *cursor != '.') {
    return -1;
  }
  cursor += 1;
  if (parse_semver_number(&cursor, &version->minor) != 0 ||
      *cursor != '.') {
    return -1;
  }
  cursor += 1;
  if (parse_semver_number(&cursor, &version->patch) != 0) {
    return -1;
  }
  if (*cursor == '\0') {
    return 0;
  }
  if (*cursor != '-') {
    return -1;
  }
  cursor += 1;
  version->prerelease = cursor;
  version->prerelease_length = strlen(cursor);
  if (version->prerelease_length == 0 ||
      validate_prerelease(version->prerelease,
                          version->prerelease_length) != 0) {
    return -1;
  }
  return 0;
}

static int compare_identifier(const char *left, size_t left_length,
                              const char *right, size_t right_length) {
  const int left_numeric =
      prerelease_identifier_is_numeric(left, left_length);
  const int right_numeric =
      prerelease_identifier_is_numeric(right, right_length);
  if (left_numeric && right_numeric) {
    if (left_length < right_length) {
      return -1;
    }
    if (left_length > right_length) {
      return 1;
    }
  } else if (left_numeric != right_numeric) {
    return left_numeric ? -1 : 1;
  }
  const size_t shared_length =
      left_length < right_length ? left_length : right_length;
  const int shared_result = memcmp(left, right, shared_length);
  if (shared_result < 0) {
    return -1;
  }
  if (shared_result > 0) {
    return 1;
  }
  if (left_length < right_length) {
    return -1;
  }
  if (left_length > right_length) {
    return 1;
  }
  return 0;
}

static int compare_prerelease(const MuonNodeSemVersion *left,
                              const MuonNodeSemVersion *right) {
  if (left->prerelease_length == 0 && right->prerelease_length == 0) {
    return 0;
  }
  if (left->prerelease_length == 0) {
    return 1;
  }
  if (right->prerelease_length == 0) {
    return -1;
  }

  size_t left_offset = 0;
  size_t right_offset = 0;
  while (left_offset < left->prerelease_length &&
         right_offset < right->prerelease_length) {
    size_t left_end = left_offset;
    size_t right_end = right_offset;
    while (left_end < left->prerelease_length &&
           left->prerelease[left_end] != '.') {
      left_end += 1;
    }
    while (right_end < right->prerelease_length &&
           right->prerelease[right_end] != '.') {
      right_end += 1;
    }
    const int result = compare_identifier(
        left->prerelease + left_offset, left_end - left_offset,
        right->prerelease + right_offset, right_end - right_offset);
    if (result != 0) {
      return result;
    }
    left_offset =
        left_end < left->prerelease_length ? left_end + 1 : left_end;
    right_offset =
        right_end < right->prerelease_length ? right_end + 1 : right_end;
  }
  if (left_offset < left->prerelease_length) {
    return 1;
  }
  if (right_offset < right->prerelease_length) {
    return -1;
  }
  return 0;
}

static int compare_semver(const MuonNodeSemVersion *left,
                          const MuonNodeSemVersion *right) {
  if (left->major != right->major) {
    return left->major < right->major ? -1 : 1;
  }
  if (left->minor != right->minor) {
    return left->minor < right->minor ? -1 : 1;
  }
  if (left->patch != right->patch) {
    return left->patch < right->patch ? -1 : 1;
  }
  return compare_prerelease(left, right);
}

static int parse_comparator(const char *text,
                            MuonParsedNodeComparator *comparator) {
  memset(comparator, 0, sizeof(*comparator));
  if (text == NULL || text[0] == '\0') {
    return -1;
  }
  const char *version_text = text;
  if (strncmp(text, "<=", 2) == 0) {
    comparator->operator_value = MUON_NODE_COMPARATOR_LESS_EQUAL;
    version_text += 2;
  } else if (strncmp(text, ">=", 2) == 0) {
    comparator->operator_value = MUON_NODE_COMPARATOR_GREATER_EQUAL;
    version_text += 2;
  } else if (text[0] == '<') {
    comparator->operator_value = MUON_NODE_COMPARATOR_LESS;
    version_text += 1;
  } else if (text[0] == '>') {
    comparator->operator_value = MUON_NODE_COMPARATOR_GREATER;
    version_text += 1;
  } else if (text[0] == '=') {
    return -1;
  } else {
    comparator->operator_value = MUON_NODE_COMPARATOR_EQUAL;
  }
  return parse_semver(version_text, 0, &comparator->version);
}

static int version_satisfies_comparator(
    const MuonNodeSemVersion *version,
    const MuonParsedNodeComparator *comparator) {
  const int comparison = compare_semver(version, &comparator->version);
  switch (comparator->operator_value) {
    case MUON_NODE_COMPARATOR_EQUAL:
      return comparison == 0;
    case MUON_NODE_COMPARATOR_LESS:
      return comparison < 0;
    case MUON_NODE_COMPARATOR_LESS_EQUAL:
      return comparison <= 0;
    case MUON_NODE_COMPARATOR_GREATER:
      return comparison > 0;
    case MUON_NODE_COMPARATOR_GREATER_EQUAL:
      return comparison >= 0;
  }
  return 0;
}

static int version_satisfies_requirement(
    const MuonNodeSemVersion *version,
    const MuonNodeRuntimeRequirement *requirement) {
  for (size_t set_index = 0; set_index < requirement->set_count;
       set_index += 1) {
    const MuonNodeComparatorSet *set = &requirement->sets[set_index];
    int set_matches = 1;
    for (size_t comparator_index = 0; comparator_index < set->count;
         comparator_index += 1) {
      MuonParsedNodeComparator comparator;
      if (parse_comparator(set->comparators[comparator_index], &comparator) !=
              0 ||
          !version_satisfies_comparator(version, &comparator)) {
        set_matches = 0;
        break;
      }
    }
    if (set_matches) {
      return 1;
    }
  }
  return 0;
}

int muon_prepare_get_node_target_info(const char *target,
                                      MuonNodeTargetInfo *info) {
  if (target == NULL || info == NULL) {
    return -1;
  }
  for (size_t index = 0;
       index < sizeof(kNodeTargetMappings) / sizeof(kNodeTargetMappings[0]);
       index += 1) {
    if (strcmp(target, kNodeTargetMappings[index].target) == 0) {
      *info = kNodeTargetMappings[index].info;
      return 0;
    }
  }
  muon_print_error("Unsupported Node.js runtime target: %s\n", target);
  memset(info, 0, sizeof(*info));
  return -1;
}

void muon_prepare_free_node_runtime_requirement(
    MuonNodeRuntimeRequirement *requirement) {
  if (requirement == NULL) {
    return;
  }
  if (requirement->sets != NULL) {
    for (size_t set_index = 0; set_index < requirement->set_count;
         set_index += 1) {
      muon_free_string_array(requirement->sets[set_index].comparators,
                             requirement->sets[set_index].count);
    }
  }
  free(requirement->sets);
  free(requirement->engine_range);
  memset(requirement, 0, sizeof(*requirement));
}

int muon_prepare_parse_node_runtime_requirement(
    const char *json, MuonNodeRuntimeRequirement *requirement) {
  if (requirement == NULL) {
    return -1;
  }
  memset(requirement, 0, sizeof(*requirement));
  if (json == NULL) {
    muon_print_error("Node.js runtime requirement JSON is missing.\n");
    return -1;
  }

  yyjson_read_err read_error;
  yyjson_doc *document = yyjson_read_opts(
      (char *)json, strlen(json), YYJSON_READ_NOFLAG, NULL, &read_error);
  yyjson_val *root =
      document == NULL ? NULL : yyjson_doc_get_root(document);
  if (root == NULL || !yyjson_is_obj(root) || yyjson_obj_size(root) != 3) {
    muon_print_error("Invalid Node.js runtime requirement JSON%s%s.\n",
                     document == NULL ? ": " : "",
                     document == NULL && read_error.msg != NULL
                         ? read_error.msg
                         : "");
    yyjson_doc_free(document);
    return -1;
  }

  yyjson_val *required_value = NULL;
  yyjson_val *engine_range_value = NULL;
  yyjson_val *sets_value = NULL;
  if (object_get_single_value(root, "required", &required_value) != 0 ||
      object_get_single_value(root, "engineRange", &engine_range_value) != 0 ||
      object_get_single_value(root, "comparatorSets", &sets_value) != 0 ||
      !yyjson_is_bool(required_value) ||
      !json_value_is_c_string(engine_range_value) ||
      yyjson_get_len(engine_range_value) == 0 || !yyjson_is_arr(sets_value) ||
      yyjson_arr_size(sets_value) == 0) {
    muon_print_error("Invalid Node.js runtime requirement schema.\n");
    yyjson_doc_free(document);
    return -1;
  }

  requirement->required = yyjson_get_bool(required_value) ? 1 : 0;
  requirement->engine_range =
      muon_duplicate_string(yyjson_get_str(engine_range_value));
  requirement->set_count = yyjson_arr_size(sets_value);
  requirement->sets = (MuonNodeComparatorSet *)calloc(
      requirement->set_count, sizeof(MuonNodeComparatorSet));
  if (requirement->engine_range == NULL || requirement->sets == NULL) {
    yyjson_doc_free(document);
    muon_prepare_free_node_runtime_requirement(requirement);
    return -1;
  }

  size_t set_index = 0;
  size_t set_max = 0;
  yyjson_val *set_value = NULL;
  yyjson_arr_foreach(sets_value, set_index, set_max, set_value) {
    if (!yyjson_is_arr(set_value)) {
      muon_print_error(
          "Node.js runtime comparatorSets entries must be arrays.\n");
      yyjson_doc_free(document);
      muon_prepare_free_node_runtime_requirement(requirement);
      return -1;
    }
    MuonNodeComparatorSet *set = &requirement->sets[set_index];
    set->count = yyjson_arr_size(set_value);
    if (set->count == 0) {
      continue;
    }
    set->comparators =
        (char **)calloc(set->count, sizeof(char *));
    if (set->comparators == NULL) {
      yyjson_doc_free(document);
      muon_prepare_free_node_runtime_requirement(requirement);
      return -1;
    }
    size_t comparator_index = 0;
    size_t comparator_max = 0;
    yyjson_val *comparator_value = NULL;
    yyjson_arr_foreach(set_value, comparator_index, comparator_max,
                       comparator_value) {
      MuonParsedNodeComparator parsed;
      if (!json_value_is_c_string(comparator_value) ||
          yyjson_get_len(comparator_value) == 0 ||
          parse_comparator(yyjson_get_str(comparator_value), &parsed) != 0) {
        muon_print_error("Invalid normalized Node.js comparator.\n");
        yyjson_doc_free(document);
        muon_prepare_free_node_runtime_requirement(requirement);
        return -1;
      }
      set->comparators[comparator_index] =
          muon_duplicate_string(yyjson_get_str(comparator_value));
      if (set->comparators[comparator_index] == NULL) {
        yyjson_doc_free(document);
        muon_prepare_free_node_runtime_requirement(requirement);
        return -1;
      }
    }
  }

  yyjson_doc_free(document);
  return 0;
}

static int file_has_download_progress(const char *path) {
  if (!muon_path_exists(path)) {
    return 0;
  }
  unsigned long long size = 0;
  return muon_get_file_size(path, &size) == 0 && size > 0;
}

static int run_curl_download(
    const char *url, const char *destination, const char *curl_flags,
    const char *status, MuonPrepareProgressCallback progress_callback,
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
      muon_print_error("Retrying Node.js download: attempt %d/%d\n",
                       attempt + 1, kCurlDownloadAttemptCount);
    }
    const int result =
        progress_callback == NULL
            ? (final_attempt ? muon_run_process(argv)
                             : muon_run_process_allow_failure(argv))
            : (final_attempt
                   ? muon_run_process_with_file_progress(
                         argv, destination, 0, progress_callback,
                         progress_user_data,
                         MUON_PREPARE_PROGRESS_PHASE_DOWNLOADING, status)
                   : muon_run_process_with_file_progress_allow_failure(
                         argv, destination, 0, progress_callback,
                         progress_user_data,
                         MUON_PREPARE_PROGRESS_PHASE_DOWNLOADING, status));
    if (result == 0) {
      return 0;
    }
    if (resume) {
      muon_remove_recursive(destination);
    }
  }
  return -1;
}

static int copy_url_to_file(
    const char *url, const char *destination, const char *status,
    MuonPrepareProgressCallback progress_callback, void *progress_user_data) {
  if (strncmp(url, "http://", 7) == 0 ||
      strncmp(url, "https://", 8) == 0) {
    return run_curl_download(url, destination,
                             progress_callback == NULL ? "-fL" : "-fsSL",
                             status, progress_callback, progress_user_data);
  }
  const char *source =
      strncmp(url, "file://", 7) == 0 ? url + 7 : url;
  if (progress_callback == NULL) {
    return muon_copy_file_with_source_mode(source, destination);
  }
  muon_report_progress(progress_callback, progress_user_data,
                       MUON_PREPARE_PROGRESS_PHASE_DOWNLOADING, status, 0, 0,
                       0);
  return muon_copy_file_with_source_mode_progress(
      source, destination, progress_callback, progress_user_data,
      MUON_PREPARE_PROGRESS_PHASE_DOWNLOADING, status);
}

static const char *get_node_dist_url(void) {
  const char *override = getenv("MUON_NODE_DIST_URL");
  return override == NULL || override[0] == '\0' ? kDefaultNodeDistUrl
                                                 : override;
}

static char *create_node_dist_url(const char *first, const char *second) {
  const char *base = get_node_dist_url();
  size_t base_length = strlen(base);
  while (base_length > 1 && base[base_length - 1] == '/') {
    base_length -= 1;
  }
  const size_t first_length =
      first == NULL || first[0] == '\0' ? 0 : strlen(first);
  const size_t second_length =
      second == NULL || second[0] == '\0' ? 0 : strlen(second);
  const size_t separator_count =
      (first_length == 0 ? 0 : 1) + (second_length == 0 ? 0 : 1);
  char *result = (char *)malloc(base_length + first_length + second_length +
                                separator_count + 1);
  if (result == NULL) {
    return NULL;
  }
  size_t offset = 0;
  memcpy(result + offset, base, base_length);
  offset += base_length;
  if (first_length != 0) {
    result[offset++] = '/';
    memcpy(result + offset, first, first_length);
    offset += first_length;
  }
  if (second_length != 0) {
    result[offset++] = '/';
    memcpy(result + offset, second, second_length);
    offset += second_length;
  }
  result[offset] = '\0';
  return result;
}

int muon_prepare_ensure_node_catalog_cache_with_status_progress(
    const char *cache_dir, int force, int *updated,
    MuonPrepareProgressCallback progress_callback, void *progress_user_data) {
  if (updated != NULL) {
    *updated = 0;
  }
  if (cache_dir == NULL || muon_ensure_directory(cache_dir) != 0) {
    return -1;
  }
  char *catalog_path =
      muon_path_join(cache_dir, MUON_PREPARE_NODE_CATALOG_FILE_NAME);
  char *temporary_path = muon_create_temporary_path(
      cache_dir, MUON_PREPARE_NODE_CATALOG_FILE_NAME);
  char *catalog_url = create_node_dist_url(NULL, "index.json");
  if (catalog_path == NULL || temporary_path == NULL || catalog_url == NULL) {
    free(catalog_path);
    free(temporary_path);
    free(catalog_url);
    return -1;
  }
  if (!force && muon_path_exists(catalog_path)) {
    free(catalog_path);
    free(temporary_path);
    free(catalog_url);
    return 0;
  }

  muon_remove_recursive(temporary_path);
  const int copy_result = copy_url_to_file(
      catalog_url, temporary_path, "Checking for Node.js updates...",
      progress_callback, progress_user_data);
  int result = -1;
  if (copy_result == 0) {
    result = muon_atomic_replace(temporary_path, catalog_path);
    if (result == 0 && updated != NULL) {
      *updated = 1;
    }
  } else if (muon_path_exists(catalog_path)) {
    result = 0;
  }
  if (result != 0 || copy_result != 0) {
    muon_remove_recursive(temporary_path);
  }
  free(catalog_path);
  free(temporary_path);
  free(catalog_url);
  return result;
}

void muon_prepare_free_node_artifact(MuonNodeArtifact *artifact) {
  if (artifact == NULL) {
    return;
  }
  free(artifact->version);
  free(artifact->catalog_file);
  free(artifact->archive_target);
  free(artifact->file_name);
  free(artifact->url);
  free(artifact->sha256);
  free(artifact->lts);
  memset(artifact, 0, sizeof(*artifact));
}

static void free_string_list(MuonNodeStringList *list) {
  if (list == NULL) {
    return;
  }
  muon_free_string_array(list->values, list->count);
  memset(list, 0, sizeof(*list));
}

static int string_list_add_unique(MuonNodeStringList *list,
                                  const char *value) {
  for (size_t index = 0; index < list->count; index += 1) {
    if (strcmp(list->values[index], value) == 0) {
      return 1;
    }
  }
  if (list->count == list->capacity) {
    const size_t next_capacity = list->capacity == 0 ? 32 : list->capacity * 2;
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

static void free_release_selection(MuonNodeReleaseSelection *selection) {
  if (selection == NULL) {
    return;
  }
  free(selection->version);
  free(selection->lts);
  memset(selection, 0, sizeof(*selection));
}

static int compare_catalog_versions(const char *left, const char *right) {
  MuonNodeSemVersion left_version;
  MuonNodeSemVersion right_version;
  if (parse_semver(left, 1, &left_version) != 0 ||
      parse_semver(right, 1, &right_version) != 0) {
    return 0;
  }
  return compare_semver(&left_version, &right_version);
}

static int set_release_selection(MuonNodeReleaseSelection *selection,
                                 const char *version, const char *lts) {
  char *next_version = muon_duplicate_string(version);
  char *next_lts = lts == NULL ? NULL : muon_duplicate_string(lts);
  if (next_version == NULL || (lts != NULL && next_lts == NULL)) {
    free(next_version);
    free(next_lts);
    return -1;
  }
  free(selection->version);
  free(selection->lts);
  selection->version = next_version;
  selection->lts = next_lts;
  return 0;
}

static int update_release_selection(MuonNodeReleaseSelection *selection,
                                    const char *version, const char *lts) {
  if (selection->version != NULL &&
      compare_catalog_versions(version, selection->version) <= 0) {
    return 0;
  }
  return set_release_selection(selection, version, lts);
}

static int validate_catalog_files(yyjson_val *files,
                                  const char *catalog_file,
                                  int *contains_target) {
  if (files == NULL || !yyjson_is_arr(files)) {
    return -1;
  }
  *contains_target = 0;
  const size_t count = yyjson_arr_size(files);
  if (count == 0) {
    return -1;
  }
  for (size_t index = 0; index < count; index += 1) {
    yyjson_val *entry = yyjson_arr_get(files, index);
    if (!json_value_is_c_string(entry) || yyjson_get_len(entry) == 0) {
      return -1;
    }
    const char *value = yyjson_get_str(entry);
    for (size_t previous = 0; previous < index; previous += 1) {
      yyjson_val *previous_entry = yyjson_arr_get(files, previous);
      if (strcmp(value, yyjson_get_str(previous_entry)) == 0) {
        return -1;
      }
    }
    if (strcmp(value, catalog_file) == 0) {
      *contains_target = 1;
    }
  }
  return 0;
}

static int resolve_catalog_entry(
    yyjson_val *entry, const MuonNodeTargetInfo *target_info,
    const MuonNodeRuntimeRequirement *requirement,
    MuonNodeStringList *seen_versions, MuonNodeReleaseSelection *best_any,
    MuonNodeReleaseSelection *best_lts) {
  if (entry == NULL || !yyjson_is_obj(entry)) {
    return -1;
  }
  yyjson_val *version_value = NULL;
  yyjson_val *files_value = NULL;
  yyjson_val *lts_value = NULL;
  if (object_get_single_value(entry, "version", &version_value) != 0 ||
      object_get_single_value(entry, "files", &files_value) != 0 ||
      object_get_single_value(entry, "lts", &lts_value) != 0 ||
      !json_value_is_c_string(version_value)) {
    return -1;
  }
  const char *version_text = yyjson_get_str(version_value);
  MuonNodeSemVersion version;
  if (parse_semver(version_text, 1, &version) != 0 ||
      version.prerelease_length != 0) {
    return -1;
  }
  const int seen_result = string_list_add_unique(seen_versions, version_text);
  if (seen_result != 0) {
    if (seen_result > 0) {
      muon_print_error("Duplicate Node.js catalog version: %s\n",
                       version_text);
    }
    return -1;
  }

  const char *lts = NULL;
  if (json_value_is_c_string(lts_value) && yyjson_get_len(lts_value) != 0) {
    lts = yyjson_get_str(lts_value);
  } else if (!yyjson_is_bool(lts_value) || yyjson_get_bool(lts_value)) {
    return -1;
  }
  int contains_target = 0;
  if (validate_catalog_files(files_value, target_info->catalog_file,
                             &contains_target) != 0) {
    return -1;
  }
  if (!contains_target ||
      !version_satisfies_requirement(&version, requirement)) {
    return 0;
  }
  if (update_release_selection(best_any, version_text, lts) != 0) {
    return -1;
  }
  if (lts != NULL &&
      update_release_selection(best_lts, version_text, lts) != 0) {
    return -1;
  }
  return 0;
}

static char *create_node_archive_file_name(
    const char *version, const MuonNodeTargetInfo *target_info) {
  const int length =
      snprintf(NULL, 0, "node-%s-%s%s", version, target_info->archive_target,
               target_info->archive_suffix);
  if (length < 0) {
    return NULL;
  }
  char *file_name = (char *)malloc((size_t)length + 1);
  if (file_name == NULL) {
    return NULL;
  }
  snprintf(file_name, (size_t)length + 1, "node-%s-%s%s", version,
           target_info->archive_target, target_info->archive_suffix);
  return file_name;
}

static int set_node_artifact(
    const MuonNodeReleaseSelection *selection,
    const MuonNodeTargetInfo *target_info, MuonNodeArtifact *artifact) {
  memset(artifact, 0, sizeof(*artifact));
  artifact->version = muon_duplicate_string(selection->version);
  artifact->catalog_file =
      muon_duplicate_string(target_info->catalog_file);
  artifact->archive_target =
      muon_duplicate_string(target_info->archive_target);
  artifact->file_name =
      create_node_archive_file_name(selection->version, target_info);
  artifact->url =
      artifact->file_name == NULL
          ? NULL
          : create_node_dist_url(selection->version, artifact->file_name);
  artifact->lts =
      selection->lts == NULL ? NULL : muon_duplicate_string(selection->lts);
  if (artifact->version == NULL || artifact->catalog_file == NULL ||
      artifact->archive_target == NULL || artifact->file_name == NULL ||
      artifact->url == NULL ||
      (selection->lts != NULL && artifact->lts == NULL)) {
    muon_prepare_free_node_artifact(artifact);
    return -1;
  }
  return 0;
}

int muon_prepare_resolve_node_artifact(
    const char *cache_dir, const char *target,
    const MuonNodeRuntimeRequirement *requirement, MuonNodeArtifact *artifact) {
  if (cache_dir == NULL || target == NULL || requirement == NULL ||
      artifact == NULL) {
    return -1;
  }
  memset(artifact, 0, sizeof(*artifact));
  MuonNodeTargetInfo target_info;
  if (muon_prepare_get_node_target_info(target, &target_info) != 0) {
    return -1;
  }

  char *catalog_path =
      muon_path_join(cache_dir, MUON_PREPARE_NODE_CATALOG_FILE_NAME);
  yyjson_doc *catalog =
      catalog_path == NULL ? NULL : muon_json_read_file(catalog_path);
  yyjson_val *root =
      catalog == NULL ? NULL : yyjson_doc_get_root(catalog);
  if (catalog_path == NULL || root == NULL || !yyjson_is_arr(root)) {
    muon_print_error("Invalid cached Node.js release index.\n");
    free(catalog_path);
    yyjson_doc_free(catalog);
    return -1;
  }

  MuonNodeStringList seen_versions = {0};
  MuonNodeReleaseSelection best_any = {0};
  MuonNodeReleaseSelection best_lts = {0};
  int result = 0;
  size_t index = 0;
  size_t max = 0;
  yyjson_val *entry = NULL;
  yyjson_arr_foreach(root, index, max, entry) {
    if (resolve_catalog_entry(entry, &target_info, requirement, &seen_versions,
                              &best_any, &best_lts) != 0) {
      muon_print_error("Invalid Node.js release index entry.\n");
      result = -1;
      break;
    }
  }

  const MuonNodeReleaseSelection *selected =
      best_lts.version == NULL ? &best_any : &best_lts;
  if (result == 0 && selected->version == NULL) {
    muon_print_error(
        "No Node.js release matches target %s and engines.node %s.\n", target,
        requirement->engine_range == NULL ? "(unknown)"
                                          : requirement->engine_range);
    result = -1;
  }
  if (result == 0) {
    result = set_node_artifact(selected, &target_info, artifact);
  }

  free(catalog_path);
  yyjson_doc_free(catalog);
  free_string_list(&seen_versions);
  free_release_selection(&best_any);
  free_release_selection(&best_lts);
  return result;
}

static int ensure_node_checksum_cache(
    const char *cache_dir, const MuonNodeArtifact *artifact, int force,
    char **checksum_path, MuonPrepareProgressCallback progress_callback,
    void *progress_user_data) {
  *checksum_path = NULL;
  char *checksums_root =
      muon_path_join(cache_dir, "node-checksums");
  char *version_directory =
      checksums_root == NULL
          ? NULL
          : muon_path_join(checksums_root, artifact->version);
  char *final_path =
      version_directory == NULL
          ? NULL
          : muon_path_join(version_directory, kNodeChecksumFileName);
  char *temporary_path =
      version_directory == NULL
          ? NULL
          : muon_create_temporary_path(version_directory,
                                       kNodeChecksumFileName);
  char *url =
      create_node_dist_url(artifact->version, kNodeChecksumFileName);
  if (checksums_root == NULL || version_directory == NULL ||
      final_path == NULL || temporary_path == NULL || url == NULL ||
      muon_ensure_directory(version_directory) != 0) {
    free(checksums_root);
    free(version_directory);
    free(final_path);
    free(temporary_path);
    free(url);
    return -1;
  }
  if (!force && muon_path_exists(final_path)) {
    *checksum_path = final_path;
    free(checksums_root);
    free(version_directory);
    free(temporary_path);
    free(url);
    return 0;
  }

  muon_remove_recursive(temporary_path);
  const int copy_result = copy_url_to_file(
      url, temporary_path, "Downloading Node.js checksums...",
      progress_callback, progress_user_data);
  int result = -1;
  if (copy_result == 0) {
    result = muon_atomic_replace(temporary_path, final_path);
  } else if (muon_path_exists(final_path)) {
    result = 0;
  }
  if (result == 0) {
    *checksum_path = final_path;
  } else {
    free(final_path);
  }
  if (result != 0 || copy_result != 0) {
    muon_remove_recursive(temporary_path);
  }
  free(checksums_root);
  free(version_directory);
  free(temporary_path);
  free(url);
  return result;
}

static int is_lowercase_sha256(const char *value, size_t length) {
  if (length != 64) {
    return 0;
  }
  for (size_t index = 0; index < length; index += 1) {
    const char character = value[index];
    if (!is_ascii_digit(character) &&
        !(character >= 'a' && character <= 'f')) {
      return 0;
    }
  }
  return 1;
}

static int select_artifact_checksum(const char *checksum_path,
                                    MuonNodeArtifact *artifact) {
  char *content = muon_read_text_file(checksum_path);
  if (content == NULL) {
    return -1;
  }
  unsigned long long content_size = 0;
  if (muon_get_file_size(checksum_path, &content_size) != 0 ||
      content_size != (unsigned long long)strlen(content)) {
    muon_print_error("Invalid Node.js SHASUMS256 file contents.\n");
    free(content);
    return -1;
  }
  char selected_sha256[65] = {0};
  size_t selected_count = 0;
  const char *cursor = content;
  int result = 0;
  while (*cursor != '\0') {
    const char *newline = strchr(cursor, '\n');
    const char *line_end =
        newline == NULL ? cursor + strlen(cursor) : newline;
    const size_t line_length = (size_t)(line_end - cursor);
    if (line_length < 67 || !is_lowercase_sha256(cursor, 64) ||
        cursor[64] != ' ' || cursor[65] != ' ') {
      result = -1;
      break;
    }
    const char *file_name = cursor + 66;
    const size_t file_name_length = line_length - 66;
    if (file_name_length == 0 ||
        memchr(file_name, '\r', file_name_length) != NULL) {
      result = -1;
      break;
    }
    if (strlen(artifact->file_name) == file_name_length &&
        memcmp(file_name, artifact->file_name, file_name_length) == 0) {
      memcpy(selected_sha256, cursor, 64);
      selected_sha256[64] = '\0';
      selected_count += 1;
    }
    cursor = newline == NULL ? line_end : newline + 1;
  }
  if (result == 0 && selected_count != 1) {
    result = -1;
  }
  if (result == 0) {
    char *sha256 = muon_duplicate_string(selected_sha256);
    if (sha256 == NULL) {
      result = -1;
    } else {
      free(artifact->sha256);
      artifact->sha256 = sha256;
    }
  }
  if (result != 0) {
    muon_print_error(
        "Invalid Node.js SHASUMS256 entry for artifact: %s\n",
        artifact->file_name);
  }
  free(content);
  return result;
}

static int verify_node_archive_sha256(const char *path,
                                      const char *expected) {
  char actual[SHA256_DIGEST_STRING_LENGTH];
  if (expected == NULL || muon_sha256_file_hex(path, actual) != 0) {
    return -1;
  }
  if (strcmp(actual, expected) != 0) {
    muon_print_error(
        "Node.js archive SHA-256 mismatch: expected %s, got %s\n", expected,
        actual);
    return -1;
  }
  return 0;
}

int muon_prepare_ensure_node_archive_cache_progress(
    const char *cache_dir, MuonNodeArtifact *artifact, int force,
    char **archive_path, MuonPrepareProgressCallback progress_callback,
    void *progress_user_data) {
  if (archive_path == NULL) {
    return -1;
  }
  *archive_path = NULL;
  if (cache_dir == NULL || artifact == NULL || artifact->version == NULL ||
      artifact->file_name == NULL || artifact->url == NULL) {
    return -1;
  }

  char *checksum_path = NULL;
  if (ensure_node_checksum_cache(cache_dir, artifact, force, &checksum_path,
                                 progress_callback, progress_user_data) != 0) {
    return -1;
  }
  if (select_artifact_checksum(checksum_path, artifact) != 0) {
    muon_remove_recursive(checksum_path);
    free(checksum_path);
    return -1;
  }

  char *artifacts_root = muon_path_join3(cache_dir, "artifacts", "node");
  char *version_directory =
      artifacts_root == NULL
          ? NULL
          : muon_path_join(artifacts_root, artifact->version);
  char *final_path =
      version_directory == NULL
          ? NULL
          : muon_path_join(version_directory, artifact->file_name);
  char *temporary_path =
      version_directory == NULL
          ? NULL
          : muon_create_temporary_path(version_directory,
                                       artifact->file_name);
  if (artifacts_root == NULL || version_directory == NULL ||
      final_path == NULL || temporary_path == NULL ||
      muon_ensure_directory(version_directory) != 0) {
    free(checksum_path);
    free(artifacts_root);
    free(version_directory);
    free(final_path);
    free(temporary_path);
    return -1;
  }

  if (!force && muon_path_exists(final_path) &&
      verify_node_archive_sha256(final_path, artifact->sha256) == 0) {
    *archive_path = final_path;
    free(checksum_path);
    free(artifacts_root);
    free(version_directory);
    free(temporary_path);
    return 0;
  }
  if (muon_path_exists(final_path) &&
      muon_remove_recursive(final_path) != 0) {
    free(checksum_path);
    free(artifacts_root);
    free(version_directory);
    free(final_path);
    free(temporary_path);
    return -1;
  }
  muon_remove_recursive(temporary_path);

  muon_print_error("Downloading Node.js binary: version=%s target=%s\n",
                   artifact->version, artifact->archive_target);
  const int copy_result = copy_url_to_file(
      artifact->url, temporary_path, "Downloading Node.js runtime...",
      progress_callback, progress_user_data);
  int result = -1;
  if (copy_result == 0) {
    muon_report_progress(progress_callback, progress_user_data,
                         MUON_PREPARE_PROGRESS_PHASE_VERIFYING,
                         "Verifying Node.js runtime...", 0, 0, 0);
    if (verify_node_archive_sha256(temporary_path, artifact->sha256) == 0 &&
        muon_atomic_replace(temporary_path, final_path) == 0) {
      *archive_path = final_path;
      result = 0;
      muon_print_error(
          "Node.js binary downloaded: version=%s target=%s\n",
          artifact->version, artifact->archive_target);
    }
  }
  if (result != 0) {
    muon_remove_recursive(temporary_path);
    muon_remove_recursive(final_path);
    free(final_path);
  }
  free(checksum_path);
  free(artifacts_root);
  free(version_directory);
  free(temporary_path);
  return result;
}
