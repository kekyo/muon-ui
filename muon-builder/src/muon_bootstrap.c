// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <direct.h>
#include <process.h>
#include <windows.h>
#else
#include <limits.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "bootstrap_progress.h"
#include "bootstrap_config.h"
#include "common.h"
#include "prepare.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MUON_RECYCLE_EXIT_CODE 88
#define MUON_DESKTOP_CONFIG_FILE_NAME "muon-desktop.json"
#define MUON_DESKTOP_ICON_FILE_NAME "muon-desktop-icon.png"
#define MUON_INSTALL_CONFIG_FILE_NAME "muon-install.json"
#define MUON_BOOTSTRAP_APP_ID_ENVIRONMENT "MUON_BOOTSTRAP_APP_ID"
#define MUON_CEF_SANDBOX_ENVIRONMENT "MUON_CEF_SANDBOX"
#define MUON_LAUNCH_SOURCE_NORMAL_ARGUMENT "--muon-launch-from=normal"

static int is_path_separator(char value) {
  return value == '/' || value == '\\';
}

static void normalize_path_separators(char *path) {
  if (path == NULL) {
    return;
  }
  for (char *cursor = path; *cursor != '\0'; cursor += 1) {
    if (*cursor == '\\') {
      *cursor = '/';
    }
  }
}

static char *duplicate_string(const char *value) {
  const size_t size = strlen(value) + 1;
  char *result = (char *)malloc(size);
  if (result == NULL) {
    return NULL;
  }
  memcpy(result, value, size);
  return result;
}

static char *path_join(const char *left, const char *right) {
  const int needs_separator = !is_path_separator(left[strlen(left) - 1]);
  const size_t size = strlen(left) + strlen(right) + (needs_separator ? 2 : 1);
  char *result = (char *)malloc(size);
  if (result == NULL) {
    return NULL;
  }
  snprintf(result, size, needs_separator ? "%s/%s" : "%s%s", left, right);
  return result;
}

static char *parent_directory(const char *path) {
  const char *slash = strrchr(path, '/');
  if (slash == NULL) {
    return duplicate_string(".");
  }
  if (slash == path) {
    return duplicate_string("/");
  }
#ifdef _WIN32
  if (slash == path + 2 && ((path[0] >= 'A' && path[0] <= 'Z') ||
                            (path[0] >= 'a' && path[0] <= 'z')) &&
      path[1] == ':') {
    char *root = (char *)malloc(4);
    if (root == NULL) {
      return NULL;
    }
    memcpy(root, path, 3);
    root[3] = '\0';
    return root;
  }
#endif
  const size_t length = (size_t)(slash - path);
  char *result = (char *)malloc(length + 1);
  if (result == NULL) {
    return NULL;
  }
  memcpy(result, path, length);
  result[length] = '\0';
  return result;
}

static char *file_stem(const char *path) {
  const char *start = path;
  const char *slash = strrchr(path, '/');
  if (slash != NULL) {
    start = slash + 1;
  }
  const char *end = start + strlen(start);
#ifdef _WIN32
  if (end - start > 4 && strcmp(end - 4, ".exe") == 0) {
    end -= 4;
  }
#endif
  if (end <= start) {
    return duplicate_string("muon-app");
  }
  return muon_substring(start, (size_t)(end - start));
}

static char *sanitize_app_id(const char *value) {
  char *result = duplicate_string(value);
  if (result == NULL) {
    return NULL;
  }
  for (char *cursor = result; *cursor != '\0'; cursor += 1) {
    const int is_digit = *cursor >= '0' && *cursor <= '9';
    const int is_lower = *cursor >= 'a' && *cursor <= 'z';
    const int is_upper = *cursor >= 'A' && *cursor <= 'Z';
    if (!is_digit && !is_lower && !is_upper && *cursor != '.' &&
        *cursor != '_' && *cursor != '-') {
      *cursor = '.';
    }
  }
  char *start = result;
  while (*start == '.') {
    start += 1;
  }
  char *end = start + strlen(start);
  while (end > start && end[-1] == '.') {
    end -= 1;
  }
  if (end == start) {
    free(result);
    return duplicate_string("muon-app");
  }
  char *trimmed = muon_substring(start, (size_t)(end - start));
  free(result);
  return trimmed;
}

static char *resolve_app_id(const char *bootstrap_path) {
  char *embedded = NULL;
  if (muon_bootstrap_get_embedded_app_id(&embedded) != 0) {
    return NULL;
  }
  char *source =
      embedded != NULL && embedded[0] != '\0' ? embedded : file_stem(bootstrap_path);
  char *result = source == NULL ? NULL : sanitize_app_id(source);
  free(embedded);
  if (source != embedded) {
    free(source);
  }
  return result;
}

static char *get_bootstrap_path(const char *argv0) {
#ifdef _WIN32
  (void)argv0;
  char buffer[PATH_MAX];
  const DWORD length = GetModuleFileNameA(NULL, buffer, (DWORD)sizeof(buffer));
  if (length == 0 || length >= sizeof(buffer)) {
    return NULL;
  }
  buffer[length] = '\0';
  normalize_path_separators(buffer);
  return duplicate_string(buffer);
#else
  char buffer[PATH_MAX];
  const ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
  if (length >= 0) {
    buffer[length] = '\0';
    normalize_path_separators(buffer);
    return duplicate_string(buffer);
  }
  char *fallback = duplicate_string(argv0);
  normalize_path_separators(fallback);
  return fallback;
#endif
}

static const char *get_default_target(void) {
  return MUON_PREPARE_TARGET_NAME;
}

static const char *get_core_executable_name(void) {
#ifdef _WIN32
  return "muon-core.exe";
#else
  return "muon-core";
#endif
}

static char *get_default_state_home(void) {
#ifdef _WIN32
  const char *local_app_data = getenv("LOCALAPPDATA");
  if (local_app_data != NULL && local_app_data[0] != '\0') {
    return muon_duplicate_path_string(local_app_data);
  }
  const char *user_profile = getenv("USERPROFILE");
  if (user_profile != NULL && user_profile[0] != '\0') {
    char *normalized = muon_duplicate_path_string(user_profile);
    char *result =
        normalized == NULL
            ? NULL
            : muon_path_join3(normalized, "AppData", "Local");
    free(normalized);
    return result;
  }
#else
  const char *xdg_state_home = getenv("XDG_STATE_HOME");
  if (xdg_state_home != NULL && xdg_state_home[0] != '\0') {
    return muon_duplicate_path_string(xdg_state_home);
  }
  const char *home = getenv("HOME");
  if (home != NULL && home[0] != '\0') {
    char *normalized = muon_duplicate_path_string(home);
    char *result =
        normalized == NULL
            ? NULL
            : muon_path_join3(normalized, ".local", "state");
    free(normalized);
    return result;
  }
#endif
  return muon_duplicate_path_string(".muon-state");
}

static char *create_state_runtime_dir(const char *app_id,
                                      const char *target) {
  (void)target;
  char *state_home = get_default_state_home();
  char *app_root = state_home == NULL ? NULL : muon_path_join(state_home, app_id);
  char *runtime_dir = app_root == NULL ? NULL : muon_path_join(app_root, "runtime");
  free(state_home);
  free(app_root);
  return runtime_dir;
}

#ifndef _WIN32
typedef struct {
  char *content;
  size_t length;
  size_t capacity;
} MuonBootstrapStringBuilder;

typedef struct {
  char *desktop_id;
  char *name;
  char *comment;
  char **categories;
  size_t category_count;
  int startup_notify;
  char *icon_file_name;
} MuonDesktopConfig;

typedef struct {
  int is_deb;
  int is_system_setuid;
  char *launcher_path;
  char *system_runtime_path;
  char *privileged_prepare_path;
} MuonInstallConfig;

static void string_builder_init(MuonBootstrapStringBuilder *builder) {
  builder->content = NULL;
  builder->length = 0;
  builder->capacity = 0;
}

static void string_builder_free(MuonBootstrapStringBuilder *builder) {
  free(builder->content);
  string_builder_init(builder);
}

static int string_builder_reserve(MuonBootstrapStringBuilder *builder,
                                  size_t additional) {
  if (additional > SIZE_MAX - builder->length - 1) {
    return -1;
  }
  const size_t required = builder->length + additional + 1;
  if (required <= builder->capacity) {
    return 0;
  }
  size_t capacity = builder->capacity == 0 ? 128 : builder->capacity;
  while (capacity < required) {
    if (capacity > SIZE_MAX / 2) {
      capacity = required;
      break;
    }
    capacity *= 2;
  }
  char *content = (char *)realloc(builder->content, capacity);
  if (content == NULL) {
    return -1;
  }
  builder->content = content;
  builder->capacity = capacity;
  return 0;
}

static int string_builder_append_n(MuonBootstrapStringBuilder *builder,
                                   const char *value,
                                   size_t length) {
  if (string_builder_reserve(builder, length) != 0) {
    return -1;
  }
  memcpy(builder->content + builder->length, value, length);
  builder->length += length;
  builder->content[builder->length] = '\0';
  return 0;
}

static int string_builder_append(MuonBootstrapStringBuilder *builder,
                                 const char *value) {
  return string_builder_append_n(builder, value, strlen(value));
}

static int string_builder_append_char(MuonBootstrapStringBuilder *builder,
                                      char value) {
  return string_builder_append_n(builder, &value, 1);
}

static char *string_builder_take(MuonBootstrapStringBuilder *builder) {
  if (builder->content == NULL) {
    return duplicate_string("");
  }
  char *content = builder->content;
  builder->content = NULL;
  builder->length = 0;
  builder->capacity = 0;
  return content;
}

static int is_safe_desktop_file_name(const char *value) {
  return value != NULL && value[0] != '\0' && strchr(value, '/') == NULL &&
         strchr(value, '\\') == NULL;
}

static const char *file_name_from_path(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash == NULL ? path : slash + 1;
}

static char *get_default_data_home(void) {
  const char *xdg_data_home = getenv("XDG_DATA_HOME");
  if (xdg_data_home != NULL && xdg_data_home[0] != '\0') {
    return muon_duplicate_path_string(xdg_data_home);
  }
  const char *home = getenv("HOME");
  if (home != NULL && home[0] != '\0') {
    char *normalized = muon_duplicate_path_string(home);
    char *result =
        normalized == NULL
            ? NULL
            : muon_path_join3(normalized, ".local", "share");
    free(normalized);
    return result;
  }
  return muon_duplicate_path_string(".muon-data");
}

static char *create_user_desktop_entry_path(const char *desktop_id) {
  char *data_home = get_default_data_home();
  char *applications_dir =
      data_home == NULL ? NULL : muon_path_join(data_home, "applications");
  char *desktop_file_name = NULL;
  char *desktop_path = NULL;
  if (applications_dir != NULL) {
    const size_t size = strlen(desktop_id) + strlen(".desktop") + 1;
    desktop_file_name = (char *)malloc(size);
    if (desktop_file_name != NULL) {
      snprintf(desktop_file_name, size, "%s.desktop", desktop_id);
      desktop_path = muon_path_join(applications_dir, desktop_file_name);
    }
  }
  free(data_home);
  free(applications_dir);
  free(desktop_file_name);
  return desktop_path;
}

static char *escape_desktop_string(const char *value) {
  MuonBootstrapStringBuilder builder;
  string_builder_init(&builder);
  for (const char *cursor = value; *cursor != '\0'; cursor += 1) {
    const char escaped =
        *cursor == '\n' ? 'n'
        : *cursor == '\r' ? 'r'
        : *cursor == '\t' ? 't'
        : *cursor == '\\' ? '\\'
                          : '\0';
    if (escaped != '\0') {
      if (string_builder_append_char(&builder, '\\') != 0 ||
          string_builder_append_char(&builder, escaped) != 0) {
        string_builder_free(&builder);
        return NULL;
      }
    } else if (string_builder_append_char(&builder, *cursor) != 0) {
      string_builder_free(&builder);
      return NULL;
    }
  }
  return string_builder_take(&builder);
}

static char *quote_desktop_exec_argument(const char *value) {
  MuonBootstrapStringBuilder builder;
  string_builder_init(&builder);
  if (string_builder_append_char(&builder, '"') != 0) {
    string_builder_free(&builder);
    return NULL;
  }
  for (const char *cursor = value; *cursor != '\0'; cursor += 1) {
    if (*cursor == '"' || *cursor == '\\' || *cursor == '$' ||
        *cursor == '`') {
      if (string_builder_append_char(&builder, '\\') != 0) {
        string_builder_free(&builder);
        return NULL;
      }
    }
    if (string_builder_append_char(&builder, *cursor) != 0) {
      string_builder_free(&builder);
      return NULL;
    }
  }
  if (string_builder_append_char(&builder, '"') != 0) {
    string_builder_free(&builder);
    return NULL;
  }
  return string_builder_take(&builder);
}

static char *create_desktop_exec_command(const char *launcher_path) {
  char *quoted_launcher = quote_desktop_exec_argument(launcher_path);
  if (quoted_launcher == NULL) {
    return NULL;
  }
  const size_t size = strlen(quoted_launcher) + 1 +
                      strlen(MUON_LAUNCH_SOURCE_NORMAL_ARGUMENT) + 1;
  char *result = (char *)malloc(size);
  if (result != NULL) {
    snprintf(result, size, "%s %s", quoted_launcher,
             MUON_LAUNCH_SOURCE_NORMAL_ARGUMENT);
  }
  free(quoted_launcher);
  return result;
}

static int append_desktop_entry_key_value(MuonBootstrapStringBuilder *builder,
                                          const char *key,
                                          const char *value) {
  char *escaped = escape_desktop_string(value);
  if (escaped == NULL) {
    return -1;
  }
  const int result = string_builder_append(builder, key) != 0 ||
                             string_builder_append_char(builder, '=') != 0 ||
                             string_builder_append(builder, escaped) != 0 ||
                             string_builder_append_char(builder, '\n') != 0
                         ? -1
                         : 0;
  free(escaped);
  return result;
}

static char *create_desktop_categories(char **categories, size_t count) {
  MuonBootstrapStringBuilder builder;
  string_builder_init(&builder);
  for (size_t index = 0; index < count; index += 1) {
    char *escaped = escape_desktop_string(categories[index]);
    if (escaped == NULL ||
        string_builder_append(&builder, escaped) != 0 ||
        string_builder_append_char(&builder, ';') != 0) {
      free(escaped);
      string_builder_free(&builder);
      return NULL;
    }
    free(escaped);
  }
  return string_builder_take(&builder);
}

static char *create_desktop_entry_content(const MuonDesktopConfig *desktop,
                                          const char *exec,
                                          const char *try_exec,
                                          const char *icon) {
  MuonBootstrapStringBuilder builder;
  string_builder_init(&builder);
  if (string_builder_append(&builder, "[Desktop Entry]\n") != 0 ||
      string_builder_append(&builder, "Type=Application\n") != 0 ||
      append_desktop_entry_key_value(&builder, "Name", desktop->name) != 0) {
    string_builder_free(&builder);
    return NULL;
  }
  if (desktop->comment[0] != '\0' &&
      append_desktop_entry_key_value(&builder, "Comment", desktop->comment) != 0) {
    string_builder_free(&builder);
    return NULL;
  }
  if (string_builder_append(&builder, "Exec=") != 0 ||
      string_builder_append(&builder, exec) != 0 ||
      string_builder_append_char(&builder, '\n') != 0 ||
      append_desktop_entry_key_value(&builder, "TryExec", try_exec) != 0 ||
      append_desktop_entry_key_value(&builder, "Icon", icon) != 0 ||
      string_builder_append(&builder, "Terminal=false\n") != 0) {
    string_builder_free(&builder);
    return NULL;
  }
  if (desktop->category_count > 0) {
    char *categories =
        create_desktop_categories(desktop->categories, desktop->category_count);
    if (categories == NULL ||
        string_builder_append(&builder, "Categories=") != 0 ||
        string_builder_append(&builder, categories) != 0 ||
        string_builder_append_char(&builder, '\n') != 0) {
      free(categories);
      string_builder_free(&builder);
      return NULL;
    }
    free(categories);
  }
  if (string_builder_append(
          &builder,
          desktop->startup_notify ? "StartupNotify=true\n"
                                  : "StartupNotify=false\n") != 0 ||
      append_desktop_entry_key_value(&builder, "StartupWMClass",
                                     desktop->desktop_id) != 0 ||
      string_builder_append(&builder, "X-Muon-Managed=true\n") != 0) {
    string_builder_free(&builder);
    return NULL;
  }
  return string_builder_take(&builder);
}

static int write_desktop_entry(const char *path,
                               const MuonDesktopConfig *desktop,
                               const char *launcher_path,
                               const char *try_exec,
                               const char *icon) {
  char *exec = create_desktop_exec_command(launcher_path);
  char *content =
      exec == NULL ? NULL : create_desktop_entry_content(desktop, exec, try_exec, icon);
  const int result =
      content == NULL ? -1 : muon_write_text_file(path, content);
  free(exec);
  free(content);
  return result;
}

static void desktop_config_init(MuonDesktopConfig *config) {
  memset(config, 0, sizeof(*config));
  config->startup_notify = 1;
}

static void desktop_config_free(MuonDesktopConfig *config) {
  free(config->desktop_id);
  free(config->name);
  free(config->comment);
  muon_free_string_array(config->categories, config->category_count);
  free(config->icon_file_name);
  desktop_config_init(config);
}

static int read_optional_string_field(yyjson_val *object,
                                      const char *key,
                                      const char *fallback,
                                      int allow_empty,
                                      char **output) {
  yyjson_val *value = yyjson_obj_get(object, key);
  if (value == NULL) {
    *output = duplicate_string(fallback);
    return *output == NULL ? -1 : 0;
  }
  if (!yyjson_is_str(value)) {
    fprintf(stderr, "muon-bootstrap: %s must be a string.\n", key);
    return -1;
  }
  const char *string_value = yyjson_get_str(value);
  if (!allow_empty && string_value[0] == '\0') {
    *output = duplicate_string(fallback);
    return *output == NULL ? -1 : 0;
  }
  *output = duplicate_string(string_value);
  return *output == NULL ? -1 : 0;
}

static int read_required_non_empty_string_field(yyjson_val *object,
                                                const char *key,
                                                char **output) {
  yyjson_val *value = yyjson_obj_get(object, key);
  if (!yyjson_is_str(value) || yyjson_get_str(value)[0] == '\0') {
    fprintf(stderr, "muon-bootstrap: %s must be a non-empty string.\n", key);
    return -1;
  }
  *output = duplicate_string(yyjson_get_str(value));
  return *output == NULL ? -1 : 0;
}

static int install_config_path_is_absolute(const char *path) {
#ifdef _WIN32
  return path != NULL &&
         ((path[0] >= 'A' && path[0] <= 'Z') ||
          (path[0] >= 'a' && path[0] <= 'z')) &&
         path[1] == ':' && (path[2] == '/' || path[2] == '\\');
#else
  return path != NULL && path[0] == '/';
#endif
}

static int read_optional_bool_field(yyjson_val *object,
                                    const char *key,
                                    int fallback,
                                    int *output) {
  yyjson_val *value = yyjson_obj_get(object, key);
  if (value == NULL) {
    *output = fallback;
    return 0;
  }
  if (!yyjson_is_bool(value)) {
    fprintf(stderr, "muon-bootstrap: %s must be a boolean.\n", key);
    return -1;
  }
  *output = yyjson_get_bool(value) ? 1 : 0;
  return 0;
}

static int read_desktop_categories(yyjson_val *object,
                                   MuonDesktopConfig *config) {
  yyjson_val *array = yyjson_obj_get(object, "categories");
  if (array == NULL) {
    config->categories = (char **)calloc(1, sizeof(char *));
    if (config->categories == NULL) {
      return -1;
    }
    config->categories[0] = duplicate_string("Utility");
    if (config->categories[0] == NULL) {
      return -1;
    }
    config->category_count = 1;
    return 0;
  }
  if (!yyjson_is_arr(array)) {
    fprintf(stderr, "muon-bootstrap: categories must be an array.\n");
    return -1;
  }
  const size_t count = yyjson_arr_size(array);
  if (count == 0) {
    return 0;
  }
  config->categories = (char **)calloc(count, sizeof(char *));
  if (config->categories == NULL) {
    return -1;
  }
  config->category_count = count;
  size_t index = 0;
  size_t max = 0;
  yyjson_val *entry = NULL;
  yyjson_arr_foreach(array, index, max, entry) {
    if (!yyjson_is_str(entry) || yyjson_get_str(entry)[0] == '\0') {
      fprintf(stderr,
              "muon-bootstrap: categories entries must be non-empty strings.\n");
      return -1;
    }
    config->categories[index] = duplicate_string(yyjson_get_str(entry));
    if (config->categories[index] == NULL) {
      return -1;
    }
  }
  return 0;
}

static int read_desktop_config(const char *runtime_dir,
                               const char *app_id,
                               MuonDesktopConfig *config) {
  desktop_config_init(config);
  char *path = muon_path_join(runtime_dir, MUON_DESKTOP_CONFIG_FILE_NAME);
  if (path == NULL) {
    return -1;
  }
  if (!muon_path_exists(path)) {
    free(path);
    return 1;
  }
  yyjson_doc *document = muon_json_read_file(path);
  free(path);
  if (document == NULL) {
    return -1;
  }
  yyjson_val *root = yyjson_doc_get_root(document);
  if (!yyjson_is_obj(root)) {
    yyjson_doc_free(document);
    fprintf(stderr, "muon-bootstrap: muon-desktop.json must be an object.\n");
    return -1;
  }
  int result = read_optional_string_field(root, "desktopId", app_id, 0,
                                          &config->desktop_id) ||
               read_optional_string_field(root, "name", app_id, 0,
                                          &config->name) ||
               read_optional_string_field(root, "comment", "", 1,
                                          &config->comment) ||
               read_optional_string_field(root, "iconFileName",
                                          MUON_DESKTOP_ICON_FILE_NAME, 0,
                                          &config->icon_file_name) ||
               read_optional_bool_field(root, "startupNotify", 1,
                                        &config->startup_notify) ||
               read_desktop_categories(root, config);
  yyjson_doc_free(document);
  if (result != 0 ||
      !is_safe_desktop_file_name(config->desktop_id) ||
      !is_safe_desktop_file_name(config->icon_file_name)) {
    fprintf(stderr, "muon-bootstrap: invalid muon-desktop.json.\n");
    desktop_config_free(config);
    return -1;
  }
  return 0;
}

static void install_config_init(MuonInstallConfig *config) {
  config->is_deb = 0;
  config->is_system_setuid = 0;
  config->launcher_path = NULL;
  config->system_runtime_path = NULL;
  config->privileged_prepare_path = NULL;
}

static void install_config_free(MuonInstallConfig *config) {
  free(config->launcher_path);
  free(config->system_runtime_path);
  free(config->privileged_prepare_path);
  install_config_init(config);
}

static int read_install_config(const char *runtime_dir, MuonInstallConfig *config) {
  install_config_init(config);
  char *path = muon_path_join(runtime_dir, MUON_INSTALL_CONFIG_FILE_NAME);
  if (path == NULL) {
    return -1;
  }
  if (!muon_path_exists(path)) {
    free(path);
    return 0;
  }
  yyjson_doc *document = muon_json_read_file(path);
  free(path);
  if (document == NULL) {
    return -1;
  }
  yyjson_val *root = yyjson_doc_get_root(document);
  yyjson_val *type = yyjson_is_obj(root) ? yyjson_obj_get(root, "type") : NULL;
  if (!yyjson_is_str(type)) {
    yyjson_doc_free(document);
    fprintf(stderr, "muon-bootstrap: muon-install.json type must be a string.\n");
    return -1;
  }
  if (strcmp(yyjson_get_str(type), "deb") != 0) {
    yyjson_doc_free(document);
    return 0;
  }
  yyjson_val *launcher_path = yyjson_obj_get(root, "launcherPath");
  if (!yyjson_is_str(launcher_path) || yyjson_get_str(launcher_path)[0] == '\0') {
    yyjson_doc_free(document);
    fprintf(stderr,
            "muon-bootstrap: muon-install.json launcherPath must be a string.\n");
    return -1;
  }
  config->launcher_path = duplicate_string(yyjson_get_str(launcher_path));
  config->is_deb = config->launcher_path != NULL ? 1 : 0;
  yyjson_val *runtime_mode = yyjson_obj_get(root, "runtimeMode");
  if (runtime_mode != NULL) {
    if (!yyjson_is_str(runtime_mode)) {
      yyjson_doc_free(document);
      fprintf(stderr,
              "muon-bootstrap: muon-install.json runtimeMode must be a string.\n");
      return -1;
    }
    const char *runtime_mode_string = yyjson_get_str(runtime_mode);
    if (strcmp(runtime_mode_string, "system-setuid") == 0) {
      config->is_system_setuid = 1;
      if (read_required_non_empty_string_field(root, "systemRuntimePath",
                                               &config->system_runtime_path) !=
              0 ||
          read_required_non_empty_string_field(root, "privilegedPreparePath",
                                               &config->privileged_prepare_path) !=
              0 ||
          !install_config_path_is_absolute(config->system_runtime_path) ||
          !install_config_path_is_absolute(config->privileged_prepare_path)) {
        yyjson_doc_free(document);
        fprintf(stderr,
                "muon-bootstrap: invalid system-setuid muon-install.json.\n");
        return -1;
      }
    } else if (strcmp(runtime_mode_string, "user") != 0 &&
               strcmp(runtime_mode_string, "disabled") != 0) {
      yyjson_doc_free(document);
      fprintf(stderr,
              "muon-bootstrap: unsupported muon-install.json runtimeMode.\n");
      return -1;
    }
  }
  yyjson_doc_free(document);
  return config->launcher_path == NULL ||
                 (config->is_system_setuid &&
                  (config->system_runtime_path == NULL ||
                   config->privileged_prepare_path == NULL))
             ? -1
             : 0;
}

static int desktop_entry_is_muon_managed(const char *path) {
  if (!muon_path_exists(path)) {
    return 0;
  }
  char *content = muon_read_text_file(path);
  if (content == NULL) {
    return -1;
  }
  const int managed = strstr(content, "X-Muon-Managed=true") != NULL;
  free(content);
  return managed;
}

static int update_linux_desktop_entry(const char *runtime_dir,
                                      const char *bootstrap_path,
                                      const char *app_id) {
  MuonDesktopConfig desktop;
  const int desktop_result = read_desktop_config(runtime_dir, app_id, &desktop);
  if (desktop_result != 0) {
    return desktop_result < 0 ? -1 : 0;
  }

  MuonInstallConfig install;
  if (read_install_config(runtime_dir, &install) != 0) {
    desktop_config_free(&desktop);
    return -1;
  }

  char *desktop_path = create_user_desktop_entry_path(desktop.desktop_id);
  int result = 0;
  if (desktop_path == NULL) {
    result = -1;
  } else if (install.is_deb) {
    const int managed = desktop_entry_is_muon_managed(desktop_path);
    if (managed < 0) {
      result = -1;
    } else if (managed) {
      result = write_desktop_entry(desktop_path, &desktop, install.launcher_path,
                                   install.launcher_path, desktop.desktop_id);
    }
  } else {
    const char *launcher_name = file_name_from_path(bootstrap_path);
    char *runtime_launcher_path = muon_path_join(runtime_dir, launcher_name);
    char *icon_path = runtime_launcher_path == NULL
                          ? NULL
                          : muon_path_join(runtime_dir, desktop.icon_file_name);
    if (runtime_launcher_path == NULL || icon_path == NULL) {
      result = -1;
    } else {
      result = write_desktop_entry(desktop_path, &desktop, runtime_launcher_path,
                                   runtime_launcher_path, icon_path);
    }
    free(runtime_launcher_path);
    free(icon_path);
  }

  free(desktop_path);
  install_config_free(&install);
  desktop_config_free(&desktop);
  return result;
}
#else
typedef struct {
  int is_deb;
  int is_system_setuid;
  char *launcher_path;
  char *system_runtime_path;
  char *privileged_prepare_path;
} MuonInstallConfig;

static void install_config_init(MuonInstallConfig *config) {
  memset(config, 0, sizeof(*config));
}

static void install_config_free(MuonInstallConfig *config) {
  install_config_init(config);
}

static int read_install_config(const char *runtime_dir,
                               MuonInstallConfig *config) {
  (void)runtime_dir;
  install_config_init(config);
  return 0;
}

static int update_linux_desktop_entry(const char *runtime_dir,
                                      const char *bootstrap_path,
                                      const char *app_id) {
  (void)runtime_dir;
  (void)bootstrap_path;
  (void)app_id;
  return 0;
}
#endif

static int should_prepare_staged_runtime(const char *source_runtime_dir,
                                         const char *runtime_dir) {
  return strcmp(source_runtime_dir, runtime_dir) != 0;
}

#ifndef _WIN32
static int validate_privileged_prepare_path(const char *path) {
  struct stat entry;
  if (stat(path, &entry) != 0) {
    perror(path);
    return -1;
  }
  if (!S_ISREG(entry.st_mode) || entry.st_uid != 0 ||
      (entry.st_mode & S_ISUID) == 0 || (entry.st_mode & 0111) == 0) {
    fprintf(stderr,
            "muon-bootstrap: privileged prepare helper must be root-owned and setuid executable: %s\n",
            path);
    return -1;
  }
  return 0;
}

static int run_privileged_prepare_helper(const char *path) {
  if (validate_privileged_prepare_path(path) != 0) {
    return 1;
  }
  const pid_t child = fork();
  if (child < 0) {
    perror("fork");
    return 1;
  }
  if (child == 0) {
    char *const helper_argv[] = {(char *)path, NULL};
    execv(path, helper_argv);
    const int error_code = errno;
    perror(path);
    _exit(error_code == ENOENT ? 127 : 126);
  }
  int status = 0;
  while (waitpid(child, &status, 0) < 0) {
    if (errno != EINTR) {
      perror("waitpid");
      return 1;
    }
  }
  if (WIFEXITED(status)) {
    const int code = WEXITSTATUS(status);
    if (code != 0) {
      fprintf(stderr, "muon-bootstrap: privileged prepare helper failed: %d\n",
              code);
    }
    return code;
  }
  if (WIFSIGNALED(status)) {
    const int code = 128 + WTERMSIG(status);
    fprintf(stderr, "muon-bootstrap: privileged prepare helper signaled: %d\n",
            code);
    return code;
  }
  return 1;
}
#endif

static char **create_core_argv(const char *core_path, int argc, char **argv) {
  char **core_argv = (char **)calloc((size_t)argc + 1, sizeof(char *));
  if (core_argv == NULL) {
    return NULL;
  }
  core_argv[0] = (char *)core_path;
  for (int index = 1; index < argc; index += 1) {
    core_argv[index] = argv[index];
  }
  core_argv[argc] = NULL;
  return core_argv;
}

static int launch_core(const char *runtime_dir, const char *core_path,
                       const char *app_id, int enable_cef_sandbox, int argc,
                       char **argv) {
  char **core_argv = create_core_argv(core_path, argc, argv);
  if (core_argv == NULL) {
    fprintf(stderr, "muon-bootstrap: failed to allocate arguments.\n");
    return 1;
  }
#ifdef _WIN32
  if (_chdir(runtime_dir) != 0) {
    perror(runtime_dir);
    free(core_argv);
    return 1;
  }
  if (_putenv_s(MUON_BOOTSTRAP_APP_ID_ENVIRONMENT, app_id) != 0) {
    fprintf(stderr, "muon-bootstrap: failed to set %s.\n",
            MUON_BOOTSTRAP_APP_ID_ENVIRONMENT);
    free(core_argv);
    return 1;
  }
  if (_putenv_s(MUON_CEF_SANDBOX_ENVIRONMENT,
                enable_cef_sandbox ? "1" : "") != 0) {
    fprintf(stderr, "muon-bootstrap: failed to set %s.\n",
            MUON_CEF_SANDBOX_ENVIRONMENT);
    free(core_argv);
    return 1;
  }
  const intptr_t status =
      _spawnv(_P_WAIT, core_path, (const char *const *)core_argv);
  if (status < 0) {
    perror(core_path);
    free(core_argv);
    return 127;
  }
  free(core_argv);
  return (int)status;
#else
  if (chdir(runtime_dir) != 0) {
    perror(runtime_dir);
    free(core_argv);
    return 1;
  }
  const pid_t child = fork();
  if (child < 0) {
    perror("fork");
    free(core_argv);
    return 1;
  }
  if (child == 0) {
    if (setenv(MUON_BOOTSTRAP_APP_ID_ENVIRONMENT, app_id, 1) != 0) {
      perror("setenv");
      _exit(1);
    }
    if (enable_cef_sandbox) {
      if (setenv(MUON_CEF_SANDBOX_ENVIRONMENT, "1", 1) != 0) {
        perror("setenv");
        _exit(1);
      }
    } else {
      unsetenv(MUON_CEF_SANDBOX_ENVIRONMENT);
    }
    execv(core_path, core_argv);
    const int error_code = errno;
    perror(core_path);
    _exit(error_code == ENOENT ? 127 : 126);
  }

  free(core_argv);
  int status = 0;
  while (waitpid(child, &status, 0) < 0) {
    if (errno != EINTR) {
      perror("waitpid");
      return 1;
    }
  }
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  if (WIFSIGNALED(status)) {
    return 128 + WTERMSIG(status);
  }
  return 1;
#endif
}

static void on_prepare_progress(const MuonPrepareProgress *progress,
                                void *user_data) {
  muon_bootstrap_progress_update((MuonBootstrapProgress *)user_data, progress);
}

int main(int argc, char **argv) {
  char *bootstrap_path = get_bootstrap_path(argv[0]);
  char *source_runtime_dir =
      bootstrap_path == NULL ? NULL : parent_directory(bootstrap_path);
  char *app_id = bootstrap_path == NULL ? NULL : resolve_app_id(bootstrap_path);
  MuonInstallConfig install;
  install_config_init(&install);
  if (source_runtime_dir != NULL && read_install_config(source_runtime_dir, &install) != 0) {
    fprintf(stderr, "muon-bootstrap: failed to read install metadata.\n");
    free(bootstrap_path);
    free(source_runtime_dir);
    free(app_id);
    install_config_free(&install);
    return 1;
  }
  char *runtime_dir = NULL;
  if (install.is_system_setuid) {
    runtime_dir = duplicate_string(install.system_runtime_path);
  } else {
    runtime_dir =
        app_id == NULL ? NULL : create_state_runtime_dir(app_id, get_default_target());
  }
  char *core_path = runtime_dir == NULL
                        ? NULL
                        : path_join(runtime_dir, get_core_executable_name());
  if (bootstrap_path == NULL || source_runtime_dir == NULL || app_id == NULL ||
      runtime_dir == NULL || core_path == NULL) {
    fprintf(stderr, "muon-bootstrap: failed to resolve runtime directory.\n");
    free(bootstrap_path);
    free(source_runtime_dir);
    free(app_id);
    free(runtime_dir);
    free(core_path);
    install_config_free(&install);
    return 1;
  }
  const char *cache_dir = getenv("MUON_CACHE_DIR");
  int exit_code = 0;
  do {
    if (install.is_system_setuid) {
#ifdef _WIN32
      fprintf(stderr, "muon-bootstrap: system-setuid runtime is unsupported on Windows.\n");
      free(bootstrap_path);
      free(source_runtime_dir);
      free(app_id);
      free(runtime_dir);
      free(core_path);
      install_config_free(&install);
      return 1;
#else
      if (run_privileged_prepare_helper(install.privileged_prepare_path) != 0) {
        free(bootstrap_path);
        free(source_runtime_dir);
        free(app_id);
        free(runtime_dir);
        free(core_path);
        install_config_free(&install);
        return 1;
      }
#endif
    } else if (should_prepare_staged_runtime(source_runtime_dir, runtime_dir)) {
      MuonBootstrapProgress progress;
      muon_bootstrap_progress_init(&progress);
      const int has_progress = muon_bootstrap_progress_is_available(&progress);
      if (muon_prepare_staged_with_progress(
              source_runtime_dir, runtime_dir, get_default_target(), cache_dir, 0,
              has_progress, has_progress ? on_prepare_progress : NULL,
              has_progress ? &progress : NULL) != 0) {
        if (has_progress) {
          muon_bootstrap_progress_fail(&progress);
        }
        muon_bootstrap_progress_dispose(&progress);
        free(bootstrap_path);
        free(source_runtime_dir);
        free(app_id);
        free(runtime_dir);
        free(core_path);
        return 1;
      }
      muon_bootstrap_progress_dispose(&progress);
    }
    if (update_linux_desktop_entry(runtime_dir, bootstrap_path, app_id) != 0) {
      fprintf(stderr, "muon-bootstrap: failed to update desktop entry.\n");
    }
    exit_code = launch_core(runtime_dir, core_path, app_id,
                            install.is_system_setuid, argc, argv);
  } while (exit_code == MUON_RECYCLE_EXIT_CODE);
  free(bootstrap_path);
  free(source_runtime_dir);
  free(app_id);
  free(runtime_dir);
  free(core_path);
  install_config_free(&install);
  return exit_code;
}
