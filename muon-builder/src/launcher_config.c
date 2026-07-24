// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

#include "launcher_config.h"
#include "common.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MUON_LAUNCHER_EMBEDDED_CONFIG_SLOT_SIZE (64 * 1024)
#define MUON_LAUNCHER_TLV_MAX_NESTING_DEPTH 64U

#if defined(__GNUC__) || defined(__clang__)
#define MUON_LAUNCHER_EMBEDDED_CONFIG_USED __attribute__((used))
#else
#define MUON_LAUNCHER_EMBEDDED_CONFIG_USED
#endif

static const unsigned char kLauncherEmbeddedConfigMarker[] = {
    0x6d, 0x75, 0x6f, 0x6e, 0x2d, 0x6c, 0x61, 0x75,
    0x6e, 0x63, 0x68, 0x65, 0x72, 0x3a, 0x65, 0x6d,
    0x62, 0x65, 0x64, 0x2d, 0x63, 0x6f, 0x6e, 0x66,
    0x69, 0x67, 0x3a, 0x76, 0x31, 0x00, 0x5d};

MUON_LAUNCHER_EMBEDDED_CONFIG_USED
unsigned char kMuonLauncherEmbeddedConfigSlot
    [MUON_LAUNCHER_EMBEDDED_CONFIG_SLOT_SIZE] = {
        0x6d, 0x75, 0x6f, 0x6e, 0x2d, 0x6c, 0x61, 0x75,
        0x6e, 0x63, 0x68, 0x65, 0x72, 0x3a, 0x65, 0x6d,
        0x62, 0x65, 0x64, 0x2d, 0x63, 0x6f, 0x6e, 0x66,
        0x69, 0x67, 0x3a, 0x76, 0x31, 0x00, 0x5d};

typedef struct {
  const unsigned char *bytes;
  size_t size;
  size_t offset;
} MuonLauncherTlvReader;

static char *trim(char *value) {
  while (isspace((unsigned char)*value)) {
    value += 1;
  }
  char *end = value + strlen(value);
  while (end > value && isspace((unsigned char)end[-1])) {
    end -= 1;
  }
  *end = '\0';
  return value;
}

static int parse_bool(const char *value, int *result) {
  if (strcmp(value, "true") == 0 || strcmp(value, "1") == 0) {
    *result = 1;
    return 0;
  }
  if (strcmp(value, "false") == 0 || strcmp(value, "0") == 0) {
    *result = 0;
    return 0;
  }
  return -1;
}

static int parse_uint64(const char *value, unsigned long long *result) {
  if (value[0] == '\0') {
    return -1;
  }
  unsigned long long parsed = 0;
  for (const char *cursor = value; *cursor != '\0'; cursor += 1) {
    if (!isdigit((unsigned char)*cursor)) {
      return -1;
    }
    const unsigned long long digit = (unsigned long long)(*cursor - '0');
    if (parsed > (~0ULL - digit) / 10ULL) {
      return -1;
    }
    parsed = parsed * 10ULL + digit;
  }
  *result = parsed;
  return 0;
}

static int set_string(char **target, const char *value) {
  char *next = muon_duplicate_string(value);
  if (next == NULL) {
    return -1;
  }
  free(*target);
  *target = next;
  return 0;
}

static int is_valid_policy(const char *value) {
  return strcmp(value, "tested") == 0 ||
         strcmp(value, "same-major-latest") == 0 ||
         strcmp(value, "compat-latest") == 0 ||
         strcmp(value, "exact") == 0;
}

static int embedded_slot_is_empty(void) {
  if (memcmp(kMuonLauncherEmbeddedConfigSlot, kLauncherEmbeddedConfigMarker,
             sizeof(kLauncherEmbeddedConfigMarker)) != 0) {
    return 0;
  }
  for (size_t index = sizeof(kLauncherEmbeddedConfigMarker);
       index < MUON_LAUNCHER_EMBEDDED_CONFIG_SLOT_SIZE; index += 1) {
    if (kMuonLauncherEmbeddedConfigSlot[index] != 0) {
      return 0;
    }
  }
  return 1;
}

static int tlv_read_var_uint(MuonLauncherTlvReader *reader,
                             unsigned long long *value) {
  unsigned long long result = 0;
  unsigned int shift = 0;
  while (reader->offset < reader->size && shift <= 63) {
    const unsigned char byte = reader->bytes[reader->offset++];
    const unsigned char payload = byte & 0x7f;
    if ((shift == 63 && payload > 1) ||
        (shift != 0 && payload == 0 && (byte & 0x80) == 0)) {
      return -1;
    }
    result |= ((unsigned long long)payload) << shift;
    if ((byte & 0x80) == 0) {
      *value = result;
      return 0;
    }
    shift += 7;
  }
  return -1;
}

static int tlv_skip_value_with_depth(MuonLauncherTlvReader *reader,
                                     unsigned int depth);

static int tlv_read_sized_count(MuonLauncherTlvReader *reader,
                                size_t minimum_encoded_size,
                                size_t *count) {
  unsigned long long raw_count = 0;
  if (tlv_read_var_uint(reader, &raw_count) != 0 ||
      raw_count > (unsigned long long)SIZE_MAX) {
    return -1;
  }
  const size_t result = (size_t)raw_count;
  const size_t remaining = reader->size - reader->offset;
  if (minimum_encoded_size != 0 &&
      result > remaining / minimum_encoded_size) {
    return -1;
  }
  *count = result;
  return 0;
}

static int tlv_read_counted_bytes(MuonLauncherTlvReader *reader,
                                  const unsigned char **bytes,
                                  size_t *length) {
  unsigned long long raw_length = 0;
  if (tlv_read_var_uint(reader, &raw_length) != 0 ||
      raw_length > (unsigned long long)SIZE_MAX ||
      (size_t)raw_length > reader->size - reader->offset) {
    return -1;
  }
  *bytes = reader->bytes + reader->offset;
  *length = (size_t)raw_length;
  reader->offset += *length;
  return 0;
}

static int tlv_skip_counted_bytes(MuonLauncherTlvReader *reader) {
  const unsigned char *bytes = NULL;
  size_t length = 0;
  return tlv_read_counted_bytes(reader, &bytes, &length);
}

static int tlv_skip_array(MuonLauncherTlvReader *reader,
                          unsigned int child_depth) {
  size_t count = 0;
  if (tlv_read_sized_count(reader, 1, &count) != 0) {
    return -1;
  }
  for (size_t index = 0; index < count; index += 1) {
    if (tlv_skip_value_with_depth(reader, child_depth) != 0) {
      return -1;
    }
  }
  return 0;
}

static int tlv_skip_object(MuonLauncherTlvReader *reader,
                           unsigned int child_depth) {
  size_t count = 0;
  if (tlv_read_sized_count(reader, 2, &count) != 0) {
    return -1;
  }
  for (size_t index = 0; index < count; index += 1) {
    if (tlv_skip_counted_bytes(reader) != 0 ||
        tlv_skip_value_with_depth(reader, child_depth) != 0) {
      return -1;
    }
  }
  return 0;
}

static int tlv_skip_value_with_depth(MuonLauncherTlvReader *reader,
                                     unsigned int depth) {
  if (reader->offset >= reader->size) {
    return -1;
  }
  const unsigned char tag = reader->bytes[reader->offset++];
  switch (tag) {
    case 0:
    case 1:
    case 2:
      return 0;
    case 3: {
      unsigned long long ignored = 0;
      return tlv_read_var_uint(reader, &ignored);
    }
    case 4:
    case 5:
      return tlv_skip_counted_bytes(reader);
    case 6:
      if (depth >= MUON_LAUNCHER_TLV_MAX_NESTING_DEPTH) {
        return -1;
      }
      return tlv_skip_array(reader, depth + 1);
    case 7:
      if (depth >= MUON_LAUNCHER_TLV_MAX_NESTING_DEPTH) {
        return -1;
      }
      return tlv_skip_object(reader, depth + 1);
    default:
      return -1;
  }
}

static int tlv_skip_value(MuonLauncherTlvReader *reader) {
  return tlv_skip_value_with_depth(reader, 0);
}

static int tlv_read_raw_key(MuonLauncherTlvReader *reader,
                            const char **key,
                            size_t *key_length) {
  const unsigned char *bytes = NULL;
  size_t length = 0;
  if (tlv_read_counted_bytes(reader, &bytes, &length) != 0 ||
      memchr(bytes, '\0', length) != NULL) {
    return -1;
  }
  *key = (const char *)bytes;
  *key_length = length;
  return 0;
}

static int tlv_key_equals(const char *key,
                          size_t key_length,
                          const char *expected) {
  const size_t expected_length = strlen(expected);
  return key_length == expected_length &&
         memcmp(key, expected, key_length) == 0;
}

static int tlv_read_object_value_count(MuonLauncherTlvReader *reader,
                                       size_t *count) {
  if (reader->offset >= reader->size || reader->bytes[reader->offset++] != 7) {
    return -1;
  }
  return tlv_read_sized_count(reader, 2, count);
}

static int tlv_read_array_value_count(MuonLauncherTlvReader *reader,
                                      size_t *count) {
  if (reader->offset >= reader->size || reader->bytes[reader->offset++] != 6) {
    return -1;
  }
  return tlv_read_sized_count(reader, 1, count);
}

static int tlv_read_string_value(MuonLauncherTlvReader *reader,
                                 char **value) {
  if (reader->offset >= reader->size || reader->bytes[reader->offset++] != 4) {
    return -1;
  }
  const unsigned char *bytes = NULL;
  size_t length = 0;
  if (tlv_read_counted_bytes(reader, &bytes, &length) != 0 ||
      memchr(bytes, '\0', length) != NULL) {
    return -1;
  }
  char *result = muon_substring((const char *)bytes, length);
  if (result == NULL) {
    return -1;
  }
  *value = result;
  return 0;
}

static int tlv_read_launcher_default_version_policy(
    MuonLauncherTlvReader *reader,
    size_t count,
    char **policy) {
  for (size_t index = 0; index < count; index += 1) {
    const char *key = NULL;
    size_t key_length = 0;
    if (tlv_read_raw_key(reader, &key, &key_length) != 0) {
      muon_print_error("Invalid embedded muon launcher config.\n");
      return -1;
    }
    if (tlv_key_equals(key, key_length, "defaultVersionPolicy")) {
      char *value = NULL;
      if (tlv_read_string_value(reader, &value) != 0) {
        muon_print_error(
            "muon.json launcher.defaultVersionPolicy must be a string.\n");
        return -1;
      }
      if (!is_valid_policy(value)) {
        muon_print_error(
            "muon.json launcher.defaultVersionPolicy has unknown value: %s\n",
            value);
        free(value);
        return -1;
      }
      *policy = value;
      return 1;
    }
    if (tlv_skip_value(reader) != 0) {
      muon_print_error("Invalid embedded muon launcher config.\n");
      return -1;
    }
  }
  return 0;
}

static int tlv_read_launcher_app_id(MuonLauncherTlvReader *reader,
                                     size_t count,
                                     char **app_id) {
  for (size_t index = 0; index < count; index += 1) {
    const char *key = NULL;
    size_t key_length = 0;
    if (tlv_read_raw_key(reader, &key, &key_length) != 0) {
      muon_print_error("Invalid embedded muon launcher config.\n");
      return -1;
    }
    if (tlv_key_equals(key, key_length, "appId")) {
      char *value = NULL;
      if (tlv_read_string_value(reader, &value) != 0) {
        muon_print_error("muon.json launcher.appId must be a string.\n");
        return -1;
      }
      *app_id = value;
      return 1;
    }
    if (tlv_skip_value(reader) != 0) {
      muon_print_error("Invalid embedded muon launcher config.\n");
      return -1;
    }
  }
  return 0;
}

static int tlv_read_boolean_value(MuonLauncherTlvReader *reader,
                                  int *value) {
  if (reader->offset >= reader->size) {
    return -1;
  }
  const unsigned char tag = reader->bytes[reader->offset++];
  if (tag == 1) {
    *value = 0;
    return 0;
  }
  if (tag == 2) {
    *value = 1;
    return 0;
  }
  return -1;
}

static int tlv_read_node_comparator_sets(
    MuonLauncherTlvReader *reader,
    MuonNodeRuntimeRequirement *requirement) {
  size_t set_count = 0;
  if (tlv_read_array_value_count(reader, &set_count) != 0 ||
      set_count == 0 ||
      set_count > SIZE_MAX / sizeof(MuonNodeComparatorSet)) {
    return -1;
  }
  requirement->sets = (MuonNodeComparatorSet *)calloc(
      set_count, sizeof(MuonNodeComparatorSet));
  if (requirement->sets == NULL) {
    return -1;
  }
  requirement->set_count = set_count;
  for (size_t set_index = 0; set_index < set_count; set_index += 1) {
    MuonNodeComparatorSet *set = &requirement->sets[set_index];
    if (tlv_read_array_value_count(reader, &set->count) != 0) {
      return -1;
    }
    if (set->count == 0) {
      continue;
    }
    if (set->count > SIZE_MAX / sizeof(char *)) {
      return -1;
    }
    set->comparators = (char **)calloc(set->count, sizeof(char *));
    if (set->comparators == NULL) {
      return -1;
    }
    for (size_t comparator_index = 0; comparator_index < set->count;
         comparator_index += 1) {
      if (tlv_read_string_value(
              reader, &set->comparators[comparator_index]) != 0 ||
          set->comparators[comparator_index][0] == '\0') {
        return -1;
      }
    }
  }
  return 0;
}

static int tlv_read_node_runtime_requirement(
    MuonLauncherTlvReader *reader,
    size_t count,
    MuonNodeRuntimeRequirement *requirement) {
  if (count != 3) {
    return -1;
  }
  int has_required = 0;
  int has_engine_range = 0;
  int has_comparator_sets = 0;
  for (size_t index = 0; index < count; index += 1) {
    const char *key = NULL;
    size_t key_length = 0;
    if (tlv_read_raw_key(reader, &key, &key_length) != 0) {
      return -1;
    }
    if (tlv_key_equals(key, key_length, "required")) {
      if (has_required ||
          tlv_read_boolean_value(reader, &requirement->required) != 0) {
        return -1;
      }
      has_required = 1;
      continue;
    }
    if (tlv_key_equals(key, key_length, "engineRange")) {
      if (has_engine_range ||
          tlv_read_string_value(reader, &requirement->engine_range) != 0 ||
          requirement->engine_range[0] == '\0') {
        return -1;
      }
      has_engine_range = 1;
      continue;
    }
    if (tlv_key_equals(key, key_length, "comparatorSets")) {
      if (has_comparator_sets ||
          tlv_read_node_comparator_sets(reader, requirement) != 0) {
        return -1;
      }
      has_comparator_sets = 1;
      continue;
    }
    return -1;
  }
  return !has_required || !has_engine_range || !has_comparator_sets ? -1 : 0;
}

static int tlv_read_launcher_node_runtime(
    MuonLauncherTlvReader *reader,
    size_t count,
    MuonNodeRuntimeRequirement *requirement,
    int *present) {
  int node_runtime_seen = 0;
  for (size_t index = 0; index < count; index += 1) {
    const char *key = NULL;
    size_t key_length = 0;
    if (tlv_read_raw_key(reader, &key, &key_length) != 0) {
      return -1;
    }
    if (tlv_key_equals(key, key_length, "nodeRuntime")) {
      size_t requirement_count = 0;
      if (node_runtime_seen ||
          tlv_read_object_value_count(reader, &requirement_count) != 0 ||
          tlv_read_node_runtime_requirement(
              reader, requirement_count, requirement) != 0) {
        return -1;
      }
      node_runtime_seen = 1;
      *present = 1;
      continue;
    }
    if (tlv_skip_value(reader) != 0) {
      return -1;
    }
  }
  return 0;
}

int muon_launcher_get_embedded_default_version_policy(char **policy) {
  if (policy == NULL) {
    return -1;
  }
  *policy = NULL;
  if (embedded_slot_is_empty()) {
    *policy = muon_duplicate_string("tested");
    return *policy == NULL ? -1 : 0;
  }

  MuonLauncherTlvReader reader = {
      kMuonLauncherEmbeddedConfigSlot,
      MUON_LAUNCHER_EMBEDDED_CONFIG_SLOT_SIZE,
      0};
  size_t count = 0;
  if (tlv_read_object_value_count(&reader, &count) != 0) {
    muon_print_error("Invalid embedded muon launcher config.\n");
    return -1;
  }
  for (size_t index = 0; index < count; index += 1) {
    const char *key = NULL;
    size_t key_length = 0;
    if (tlv_read_raw_key(&reader, &key, &key_length) != 0) {
      muon_print_error("Invalid embedded muon launcher config.\n");
      return -1;
    }
    if (tlv_key_equals(key, key_length, "launcher")) {
      size_t launcher_count = 0;
      if (tlv_read_object_value_count(&reader, &launcher_count) != 0) {
        muon_print_error("muon.json launcher must be an object.\n");
        return -1;
      }
      const int result = tlv_read_launcher_default_version_policy(
          &reader, launcher_count, policy);
      if (result != 0) {
        return result < 0 ? -1 : 0;
      }
      continue;
    }
    if (tlv_skip_value(&reader) != 0) {
      muon_print_error("Invalid embedded muon launcher config.\n");
      return -1;
    }
  }
  *policy = muon_duplicate_string("tested");
  return *policy == NULL ? -1 : 0;
}

int muon_launcher_get_embedded_app_id(char **app_id) {
  if (app_id == NULL) {
    return -1;
  }
  *app_id = NULL;
  if (embedded_slot_is_empty()) {
    return 0;
  }

  MuonLauncherTlvReader reader = {
      kMuonLauncherEmbeddedConfigSlot,
      MUON_LAUNCHER_EMBEDDED_CONFIG_SLOT_SIZE,
      0};
  size_t count = 0;
  if (tlv_read_object_value_count(&reader, &count) != 0) {
    muon_print_error("Invalid embedded muon launcher config.\n");
    return -1;
  }
  for (size_t index = 0; index < count; index += 1) {
    const char *key = NULL;
    size_t key_length = 0;
    if (tlv_read_raw_key(&reader, &key, &key_length) != 0) {
      muon_print_error("Invalid embedded muon launcher config.\n");
      return -1;
    }
    if (tlv_key_equals(key, key_length, "launcher")) {
      size_t launcher_count = 0;
      if (tlv_read_object_value_count(&reader, &launcher_count) != 0) {
        muon_print_error("muon.json launcher must be an object.\n");
        return -1;
      }
      const int result =
          tlv_read_launcher_app_id(&reader, launcher_count, app_id);
      if (result != 0) {
        return result < 0 ? -1 : 0;
      }
      continue;
    }
    if (tlv_skip_value(&reader) != 0) {
      muon_print_error("Invalid embedded muon launcher config.\n");
      return -1;
    }
  }
  return 0;
}

int muon_launcher_get_embedded_node_runtime_requirement(
    MuonNodeRuntimeRequirement *requirement, int *present) {
  if (requirement == NULL) {
    return -1;
  }
  memset(requirement, 0, sizeof(*requirement));
  if (present == NULL) {
    return -1;
  }
  *present = 0;
  if (embedded_slot_is_empty()) {
    return 0;
  }

  MuonLauncherTlvReader reader = {
      kMuonLauncherEmbeddedConfigSlot,
      MUON_LAUNCHER_EMBEDDED_CONFIG_SLOT_SIZE,
      0};
  size_t count = 0;
  int launcher_seen = 0;
  if (tlv_read_object_value_count(&reader, &count) != 0) {
    goto invalid;
  }
  for (size_t index = 0; index < count; index += 1) {
    const char *key = NULL;
    size_t key_length = 0;
    if (tlv_read_raw_key(&reader, &key, &key_length) != 0) {
      goto invalid;
    }
    if (tlv_key_equals(key, key_length, "launcher")) {
      size_t launcher_count = 0;
      if (launcher_seen ||
          tlv_read_object_value_count(&reader, &launcher_count) != 0 ||
          tlv_read_launcher_node_runtime(
              &reader, launcher_count, requirement, present) != 0) {
        goto invalid;
      }
      launcher_seen = 1;
      continue;
    }
    if (tlv_skip_value(&reader) != 0) {
      goto invalid;
    }
  }
  if (*present &&
      muon_prepare_validate_node_runtime_requirement(requirement) != 0) {
    muon_prepare_free_node_runtime_requirement(requirement);
    *present = 0;
    return -1;
  }
  /*
   * A populated fixed slot has random padding after the counted root value.
   * Completing the root object is therefore the only valid end condition.
   */
  return 0;

invalid:
  muon_print_error(
      "Invalid embedded muon launcher.nodeRuntime configuration.\n");
  muon_prepare_free_node_runtime_requirement(requirement);
  *present = 0;
  return -1;
}

void muon_launcher_config_init_defaults(MuonLauncherConfig *config) {
  memset(config, 0, sizeof(*config));
  config->cef_version_policy = muon_duplicate_string("tested");
  config->cef_exact_version = muon_duplicate_string("");
  config->catalog_refresh_interval_seconds =
      MUON_LAUNCHER_DEFAULT_CATALOG_REFRESH_INTERVAL_SECONDS;
}

void muon_launcher_config_free(MuonLauncherConfig *config) {
  if (config == NULL) {
    return;
  }
  free(config->cef_version_policy);
  free(config->cef_exact_version);
  memset(config, 0, sizeof(*config));
}

int muon_launcher_config_validate(const MuonLauncherConfig *config) {
  if (config->cef_version_policy == NULL ||
      !is_valid_policy(config->cef_version_policy)) {
    muon_print_error("Invalid CEF version policy in muon-launcher.ini.\n");
    return -1;
  }
  if (config->cef_exact_version == NULL) {
    muon_print_error("Invalid exactVersion in muon-launcher.ini.\n");
    return -1;
  }
  if (strcmp(config->cef_version_policy, "exact") == 0 &&
      config->cef_exact_version[0] == '\0') {
    muon_print_error("exactVersion is required for exact CEF policy.\n");
    return -1;
  }
  return 0;
}

static int apply_entry(MuonLauncherConfig *config, const char *section,
                       const char *key, const char *value) {
  if (strcmp(section, "cef") == 0 && strcmp(key, "versionPolicy") == 0) {
    if (!is_valid_policy(value)) {
      muon_print_error("Invalid CEF version policy: %s\n", value);
      return -1;
    }
    if (set_string(&config->cef_version_policy, value) != 0) {
      return -1;
    }
    config->has_cef_version_policy = 1;
    return 0;
  }
  if (strcmp(section, "cef") == 0 && strcmp(key, "exactVersion") == 0) {
    if (set_string(&config->cef_exact_version, value) != 0) {
      return -1;
    }
    config->has_cef_exact_version = 1;
    return 0;
  }
  if (strcmp(section, "runtime") == 0 &&
      strcmp(key, "catalogRefreshIntervalSeconds") == 0) {
    if (parse_uint64(value, &config->catalog_refresh_interval_seconds) != 0) {
      return -1;
    }
    return 0;
  }
  if (strcmp(section, "cef") == 0 &&
      strcmp(key, "lastCatalogUpdateUnix") == 0) {
    return parse_uint64(value, &config->cef_last_catalog_update_unix);
  }
  if (strcmp(section, "node") == 0 &&
      strcmp(key, "lastCatalogUpdateUnix") == 0) {
    return parse_uint64(value, &config->node_last_catalog_update_unix);
  }
  if (strcmp(section, "update") == 0 && strcmp(key, "requested") == 0) {
    return parse_bool(value, &config->update_requested);
  }
  if (strcmp(section, "update") == 0 && strcmp(key, "requestedAtUnix") == 0) {
    return parse_uint64(value, &config->update_requested_at_unix);
  }
  return 0;
}

int muon_launcher_config_read(const char *runtime_dir,
                               MuonLauncherConfig *config) {
  return muon_launcher_config_read_with_default(runtime_dir, "tested", config);
}

int muon_launcher_config_read_with_default(const char *runtime_dir,
                                            const char *default_version_policy,
                                            MuonLauncherConfig *config) {
  muon_launcher_config_init_defaults(config);
  if (config->cef_version_policy == NULL ||
      config->cef_exact_version == NULL) {
    muon_launcher_config_free(config);
    return -1;
  }
  char *path = muon_path_join(runtime_dir, MUON_LAUNCHER_CONFIG_FILE_NAME);
  if (path == NULL) {
    muon_launcher_config_free(config);
    return -1;
  }
  if (!muon_path_exists(path)) {
    free(path);
    if (!config->has_cef_version_policy &&
        set_string(&config->cef_version_policy,
                   default_version_policy == NULL ? "tested"
                                                  : default_version_policy) !=
            0) {
      muon_launcher_config_free(config);
      return -1;
    }
    if (muon_launcher_config_validate(config) != 0) {
      muon_launcher_config_free(config);
      return -1;
    }
    return 0;
  }
  char *content = muon_read_text_file(path);
  free(path);
  if (content == NULL) {
    muon_launcher_config_free(config);
    return -1;
  }
  char section[32] = "";
  char *cursor = content;
  while (*cursor != '\0') {
    char *line = cursor;
    while (*cursor != '\0' && *cursor != '\n') {
      cursor += 1;
    }
    if (*cursor == '\n') {
      *cursor = '\0';
      cursor += 1;
    }
    char *entry = trim(line);
    if (entry[0] == '\0' || entry[0] == '#' || entry[0] == ';') {
      continue;
    }
    const size_t length = strlen(entry);
    if (entry[0] == '[' && length > 2 && entry[length - 1] == ']') {
      entry[length - 1] = '\0';
      snprintf(section, sizeof(section), "%s", trim(entry + 1));
      continue;
    }
    char *equals = strchr(entry, '=');
    if (equals == NULL) {
      continue;
    }
    *equals = '\0';
    if (apply_entry(config, section, trim(entry), trim(equals + 1)) != 0) {
      free(content);
      muon_launcher_config_free(config);
      return -1;
    }
  }
  free(content);
  if (!config->has_cef_version_policy &&
      set_string(&config->cef_version_policy,
                 default_version_policy == NULL ? "tested"
                                                : default_version_policy) != 0) {
    muon_launcher_config_free(config);
    return -1;
  }
  if (muon_launcher_config_validate(config) != 0) {
    muon_launcher_config_free(config);
    return -1;
  }
  return 0;
}

int muon_launcher_config_write(const char *runtime_dir,
                                const MuonLauncherConfig *config) {
  if (muon_launcher_config_validate(config) != 0) {
    return -1;
  }
  char *path = muon_path_join(runtime_dir, MUON_LAUNCHER_CONFIG_FILE_NAME);
  char *temporary_path =
      path == NULL ? NULL
                   : muon_create_temporary_path(runtime_dir,
                                                MUON_LAUNCHER_CONFIG_FILE_NAME);
  if (path == NULL || temporary_path == NULL) {
    free(path);
    free(temporary_path);
    return -1;
  }
  int size =
      snprintf(NULL, 0,
               "[runtime]\n"
               "catalogRefreshIntervalSeconds=%llu\n"
               "\n"
               "[cef]\n",
               config->catalog_refresh_interval_seconds);
  if (config->has_cef_version_policy) {
    size += snprintf(NULL, 0, "versionPolicy=%s\n",
                     config->cef_version_policy);
  }
  if (config->has_cef_exact_version) {
    size +=
        snprintf(NULL, 0, "exactVersion=%s\n", config->cef_exact_version);
  }
  size += snprintf(NULL, 0,
                   "lastCatalogUpdateUnix=%llu\n"
                   "\n"
                   "[node]\n"
                   "lastCatalogUpdateUnix=%llu\n"
                   "\n"
                   "[update]\n"
                   "requested=%s\n"
                   "requestedAtUnix=%llu\n",
                   config->cef_last_catalog_update_unix,
                   config->node_last_catalog_update_unix,
                   config->update_requested ? "true" : "false",
                   config->update_requested_at_unix);
  if (size < 0) {
    free(path);
    free(temporary_path);
    return -1;
  }
  char *content = (char *)malloc((size_t)size + 1);
  if (content == NULL) {
    free(path);
    free(temporary_path);
    return -1;
  }
  char *output = content;
  size_t remaining = (size_t)size + 1;
#define MUON_WRITE_LAUNCHER_CONFIG(...)                                      \
  do {                                                                        \
    const int written = snprintf(output, remaining, __VA_ARGS__);             \
    if (written < 0 || (size_t)written >= remaining) {                        \
      free(content);                                                          \
      free(path);                                                             \
      free(temporary_path);                                                   \
      return -1;                                                              \
    }                                                                         \
    output += written;                                                        \
    remaining -= (size_t)written;                                             \
  } while (0)
  MUON_WRITE_LAUNCHER_CONFIG(
      "[runtime]\n"
      "catalogRefreshIntervalSeconds=%llu\n"
      "\n"
      "[cef]\n",
      config->catalog_refresh_interval_seconds);
  if (config->has_cef_version_policy) {
    MUON_WRITE_LAUNCHER_CONFIG("versionPolicy=%s\n",
                                config->cef_version_policy);
  }
  if (config->has_cef_exact_version) {
    MUON_WRITE_LAUNCHER_CONFIG("exactVersion=%s\n",
                                config->cef_exact_version);
  }
  MUON_WRITE_LAUNCHER_CONFIG(
      "lastCatalogUpdateUnix=%llu\n"
      "\n"
      "[node]\n"
      "lastCatalogUpdateUnix=%llu\n"
      "\n"
      "[update]\n"
      "requested=%s\n"
      "requestedAtUnix=%llu\n",
      config->cef_last_catalog_update_unix,
      config->node_last_catalog_update_unix,
      config->update_requested ? "true" : "false",
      config->update_requested_at_unix);
#undef MUON_WRITE_LAUNCHER_CONFIG
  const int result = muon_write_text_file(temporary_path, content) == 0 &&
                             muon_atomic_replace(temporary_path, path) == 0
                         ? 0
                         : -1;
  if (result != 0) {
    muon_remove_recursive(temporary_path);
  }
  free(content);
  free(path);
  free(temporary_path);
  return result;
}
