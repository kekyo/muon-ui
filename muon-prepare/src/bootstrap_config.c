// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

#include "bootstrap_config.h"
#include "common.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MUON_BOOTSTRAP_EMBEDDED_CONFIG_SLOT_SIZE (64 * 1024)

#if defined(__GNUC__) || defined(__clang__)
#define MUON_BOOTSTRAP_EMBEDDED_CONFIG_USED __attribute__((used))
#else
#define MUON_BOOTSTRAP_EMBEDDED_CONFIG_USED
#endif

static const unsigned char kBootstrapEmbeddedConfigMarker[] = {
    0x6d, 0x75, 0x6f, 0x6e, 0x2d, 0x62, 0x6f, 0x6f,
    0x74, 0x73, 0x74, 0x72, 0x61, 0x70, 0x3a, 0x65,
    0x6d, 0x62, 0x65, 0x64, 0x2d, 0x63, 0x6f, 0x6e,
    0x66, 0x69, 0x67, 0x3a, 0x76, 0x31, 0x00, 0x5d};

MUON_BOOTSTRAP_EMBEDDED_CONFIG_USED
unsigned char kMuonBootstrapEmbeddedConfigSlot
    [MUON_BOOTSTRAP_EMBEDDED_CONFIG_SLOT_SIZE] = {
        0x6d, 0x75, 0x6f, 0x6e, 0x2d, 0x62, 0x6f, 0x6f,
        0x74, 0x73, 0x74, 0x72, 0x61, 0x70, 0x3a, 0x65,
        0x6d, 0x62, 0x65, 0x64, 0x2d, 0x63, 0x6f, 0x6e,
        0x66, 0x69, 0x67, 0x3a, 0x76, 0x31, 0x00, 0x5d};

typedef struct {
  const unsigned char *bytes;
  size_t size;
  size_t offset;
} MuonBootstrapTlvReader;

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
  if (memcmp(kMuonBootstrapEmbeddedConfigSlot, kBootstrapEmbeddedConfigMarker,
             sizeof(kBootstrapEmbeddedConfigMarker)) != 0) {
    return 0;
  }
  for (size_t index = sizeof(kBootstrapEmbeddedConfigMarker);
       index < MUON_BOOTSTRAP_EMBEDDED_CONFIG_SLOT_SIZE; index += 1) {
    if (kMuonBootstrapEmbeddedConfigSlot[index] != 0) {
      return 0;
    }
  }
  return 1;
}

static int tlv_read_var_uint(MuonBootstrapTlvReader *reader,
                             unsigned long long *value) {
  unsigned long long result = 0;
  unsigned int shift = 0;
  while (reader->offset < reader->size && shift < 64) {
    const unsigned char byte = reader->bytes[reader->offset++];
    result |= ((unsigned long long)(byte & 0x7f)) << shift;
    if ((byte & 0x80) == 0) {
      *value = result;
      return 0;
    }
    shift += 7;
  }
  return -1;
}

static int tlv_skip_value(MuonBootstrapTlvReader *reader);

static int tlv_skip_counted_bytes(MuonBootstrapTlvReader *reader) {
  unsigned long long length = 0;
  if (tlv_read_var_uint(reader, &length) != 0 ||
      length > (unsigned long long)(reader->size - reader->offset)) {
    return -1;
  }
  reader->offset += (size_t)length;
  return 0;
}

static int tlv_skip_array(MuonBootstrapTlvReader *reader) {
  unsigned long long count = 0;
  if (tlv_read_var_uint(reader, &count) != 0) {
    return -1;
  }
  for (unsigned long long index = 0; index < count; index += 1) {
    if (tlv_skip_value(reader) != 0) {
      return -1;
    }
  }
  return 0;
}

static int tlv_skip_object(MuonBootstrapTlvReader *reader) {
  unsigned long long count = 0;
  if (tlv_read_var_uint(reader, &count) != 0) {
    return -1;
  }
  for (unsigned long long index = 0; index < count; index += 1) {
    if (tlv_skip_counted_bytes(reader) != 0 ||
        tlv_skip_value(reader) != 0) {
      return -1;
    }
  }
  return 0;
}

static int tlv_skip_value(MuonBootstrapTlvReader *reader) {
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
      return tlv_skip_array(reader);
    case 7:
      return tlv_skip_object(reader);
    default:
      return -1;
  }
}

static int tlv_read_raw_key(MuonBootstrapTlvReader *reader,
                            const char **key,
                            size_t *key_length) {
  unsigned long long length = 0;
  if (tlv_read_var_uint(reader, &length) != 0 ||
      length > (unsigned long long)(reader->size - reader->offset)) {
    return -1;
  }
  *key = (const char *)(reader->bytes + reader->offset);
  *key_length = (size_t)length;
  reader->offset += (size_t)length;
  return 0;
}

static int tlv_key_equals(const char *key,
                          size_t key_length,
                          const char *expected) {
  const size_t expected_length = strlen(expected);
  return key_length == expected_length &&
         memcmp(key, expected, key_length) == 0;
}

static int tlv_read_object_value_count(MuonBootstrapTlvReader *reader,
                                       unsigned long long *count) {
  if (reader->offset >= reader->size || reader->bytes[reader->offset++] != 7) {
    return -1;
  }
  return tlv_read_var_uint(reader, count);
}

static int tlv_read_string_value(MuonBootstrapTlvReader *reader,
                                 char **value) {
  if (reader->offset >= reader->size || reader->bytes[reader->offset++] != 4) {
    return -1;
  }
  unsigned long long length = 0;
  if (tlv_read_var_uint(reader, &length) != 0 ||
      length > (unsigned long long)(reader->size - reader->offset)) {
    return -1;
  }
  char *result =
      muon_substring((const char *)(reader->bytes + reader->offset),
                     (size_t)length);
  if (result == NULL) {
    return -1;
  }
  reader->offset += (size_t)length;
  *value = result;
  return 0;
}

static int tlv_read_bootstrap_default_version_policy(
    MuonBootstrapTlvReader *reader,
    unsigned long long count,
    char **policy) {
  for (unsigned long long index = 0; index < count; index += 1) {
    const char *key = NULL;
    size_t key_length = 0;
    if (tlv_read_raw_key(reader, &key, &key_length) != 0) {
      muon_print_error("Invalid embedded muon bootstrap config.\n");
      return -1;
    }
    if (tlv_key_equals(key, key_length, "defaultVersionPolicy")) {
      char *value = NULL;
      if (tlv_read_string_value(reader, &value) != 0) {
        muon_print_error(
            "muon.json bootstrap.defaultVersionPolicy must be a string.\n");
        return -1;
      }
      if (!is_valid_policy(value)) {
        muon_print_error(
            "muon.json bootstrap.defaultVersionPolicy has unknown value: %s\n",
            value);
        free(value);
        return -1;
      }
      *policy = value;
      return 1;
    }
    if (tlv_skip_value(reader) != 0) {
      muon_print_error("Invalid embedded muon bootstrap config.\n");
      return -1;
    }
  }
  return 0;
}

int muon_bootstrap_get_embedded_default_version_policy(char **policy) {
  if (policy == NULL) {
    return -1;
  }
  *policy = NULL;
  if (embedded_slot_is_empty()) {
    *policy = muon_duplicate_string("tested");
    return *policy == NULL ? -1 : 0;
  }

  MuonBootstrapTlvReader reader = {
      kMuonBootstrapEmbeddedConfigSlot,
      MUON_BOOTSTRAP_EMBEDDED_CONFIG_SLOT_SIZE,
      0};
  unsigned long long count = 0;
  if (tlv_read_object_value_count(&reader, &count) != 0) {
    muon_print_error("Invalid embedded muon bootstrap config.\n");
    return -1;
  }
  for (unsigned long long index = 0; index < count; index += 1) {
    const char *key = NULL;
    size_t key_length = 0;
    if (tlv_read_raw_key(&reader, &key, &key_length) != 0) {
      muon_print_error("Invalid embedded muon bootstrap config.\n");
      return -1;
    }
    if (tlv_key_equals(key, key_length, "bootstrap")) {
      unsigned long long bootstrap_count = 0;
      if (tlv_read_object_value_count(&reader, &bootstrap_count) != 0) {
        muon_print_error("muon.json bootstrap must be an object.\n");
        return -1;
      }
      const int result = tlv_read_bootstrap_default_version_policy(
          &reader, bootstrap_count, policy);
      if (result != 0) {
        return result < 0 ? -1 : 0;
      }
      continue;
    }
    if (tlv_skip_value(&reader) != 0) {
      muon_print_error("Invalid embedded muon bootstrap config.\n");
      return -1;
    }
  }
  *policy = muon_duplicate_string("tested");
  return *policy == NULL ? -1 : 0;
}

void muon_bootstrap_config_init_defaults(MuonBootstrapConfig *config) {
  memset(config, 0, sizeof(*config));
  config->cef_version_policy = muon_duplicate_string("tested");
  config->cef_exact_version = muon_duplicate_string("");
  config->catalog_refresh_interval_seconds =
      MUON_BOOTSTRAP_DEFAULT_CATALOG_REFRESH_INTERVAL_SECONDS;
}

void muon_bootstrap_config_free(MuonBootstrapConfig *config) {
  if (config == NULL) {
    return;
  }
  free(config->cef_version_policy);
  free(config->cef_exact_version);
  memset(config, 0, sizeof(*config));
}

int muon_bootstrap_config_validate(const MuonBootstrapConfig *config) {
  if (config->cef_version_policy == NULL ||
      !is_valid_policy(config->cef_version_policy)) {
    muon_print_error("Invalid CEF version policy in muon-bootstrap.ini.\n");
    return -1;
  }
  if (config->cef_exact_version == NULL) {
    muon_print_error("Invalid exactVersion in muon-bootstrap.ini.\n");
    return -1;
  }
  if (strcmp(config->cef_version_policy, "exact") == 0 &&
      config->cef_exact_version[0] == '\0') {
    muon_print_error("exactVersion is required for exact CEF policy.\n");
    return -1;
  }
  return 0;
}

static int apply_entry(MuonBootstrapConfig *config, const char *section,
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
  if (strcmp(section, "cef") == 0 &&
      strcmp(key, "catalogRefreshIntervalSeconds") == 0) {
    if (parse_uint64(value, &config->catalog_refresh_interval_seconds) != 0) {
      return -1;
    }
    config->has_catalog_refresh_interval_seconds = 1;
    return 0;
  }
  if (strcmp(section, "cef") == 0 &&
      strcmp(key, "lastCatalogUpdateUnix") == 0) {
    return parse_uint64(value, &config->last_catalog_update_unix);
  }
  if (strcmp(section, "update") == 0 && strcmp(key, "requested") == 0) {
    return parse_bool(value, &config->update_requested);
  }
  if (strcmp(section, "update") == 0 && strcmp(key, "requestedAtUnix") == 0) {
    return parse_uint64(value, &config->update_requested_at_unix);
  }
  return 0;
}

int muon_bootstrap_config_read(const char *runtime_dir,
                               MuonBootstrapConfig *config) {
  return muon_bootstrap_config_read_with_default(runtime_dir, "tested", config);
}

int muon_bootstrap_config_read_with_default(const char *runtime_dir,
                                            const char *default_version_policy,
                                            MuonBootstrapConfig *config) {
  muon_bootstrap_config_init_defaults(config);
  if (config->cef_version_policy == NULL ||
      config->cef_exact_version == NULL) {
    muon_bootstrap_config_free(config);
    return -1;
  }
  char *path = muon_path_join(runtime_dir, MUON_BOOTSTRAP_CONFIG_FILE_NAME);
  if (path == NULL) {
    muon_bootstrap_config_free(config);
    return -1;
  }
  if (!muon_path_exists(path)) {
    free(path);
    if (!config->has_cef_version_policy &&
        set_string(&config->cef_version_policy,
                   default_version_policy == NULL ? "tested"
                                                  : default_version_policy) !=
            0) {
      muon_bootstrap_config_free(config);
      return -1;
    }
    if (muon_bootstrap_config_validate(config) != 0) {
      muon_bootstrap_config_free(config);
      return -1;
    }
    return 0;
  }
  char *content = muon_read_text_file(path);
  free(path);
  if (content == NULL) {
    muon_bootstrap_config_free(config);
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
      muon_bootstrap_config_free(config);
      return -1;
    }
  }
  free(content);
  if (!config->has_cef_version_policy &&
      set_string(&config->cef_version_policy,
                 default_version_policy == NULL ? "tested"
                                                : default_version_policy) != 0) {
    muon_bootstrap_config_free(config);
    return -1;
  }
  if (muon_bootstrap_config_validate(config) != 0) {
    muon_bootstrap_config_free(config);
    return -1;
  }
  return 0;
}

int muon_bootstrap_config_write(const char *runtime_dir,
                                const MuonBootstrapConfig *config) {
  if (muon_bootstrap_config_validate(config) != 0) {
    return -1;
  }
  char *path = muon_path_join(runtime_dir, MUON_BOOTSTRAP_CONFIG_FILE_NAME);
  char *temporary_path =
      path == NULL ? NULL
                   : muon_create_temporary_path(runtime_dir,
                                                MUON_BOOTSTRAP_CONFIG_FILE_NAME);
  if (path == NULL || temporary_path == NULL) {
    free(path);
    free(temporary_path);
    return -1;
  }
  int size = snprintf(NULL, 0, "[cef]\n");
  if (config->has_cef_version_policy) {
    size += snprintf(NULL, 0, "versionPolicy=%s\n",
                     config->cef_version_policy);
  }
  if (config->has_cef_exact_version) {
    size +=
        snprintf(NULL, 0, "exactVersion=%s\n", config->cef_exact_version);
  }
  if (config->has_catalog_refresh_interval_seconds) {
    size += snprintf(NULL, 0, "catalogRefreshIntervalSeconds=%llu\n",
                     config->catalog_refresh_interval_seconds);
  }
  size += snprintf(NULL, 0,
                   "lastCatalogUpdateUnix=%llu\n"
                   "\n"
                   "[update]\n"
                   "requested=%s\n"
                   "requestedAtUnix=%llu\n",
                   config->last_catalog_update_unix,
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
#define MUON_WRITE_BOOTSTRAP_CONFIG(...)                                      \
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
  MUON_WRITE_BOOTSTRAP_CONFIG("[cef]\n");
  if (config->has_cef_version_policy) {
    MUON_WRITE_BOOTSTRAP_CONFIG("versionPolicy=%s\n",
                                config->cef_version_policy);
  }
  if (config->has_cef_exact_version) {
    MUON_WRITE_BOOTSTRAP_CONFIG("exactVersion=%s\n",
                                config->cef_exact_version);
  }
  if (config->has_catalog_refresh_interval_seconds) {
    MUON_WRITE_BOOTSTRAP_CONFIG("catalogRefreshIntervalSeconds=%llu\n",
                                config->catalog_refresh_interval_seconds);
  }
  MUON_WRITE_BOOTSTRAP_CONFIG(
      "lastCatalogUpdateUnix=%llu\n"
      "\n"
      "[update]\n"
      "requested=%s\n"
      "requestedAtUnix=%llu\n",
      config->last_catalog_update_unix,
      config->update_requested ? "true" : "false",
      config->update_requested_at_unix);
#undef MUON_WRITE_BOOTSTRAP_CONFIG
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
