// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "muon_plugin_api.h"

#ifdef _WIN32
#define MUON_PLUGIN_LIBRARY_EXTENSION ".dll"
#else
#define MUON_PLUGIN_LIBRARY_EXTENSION ".so"
#endif

typedef struct {
  muon_plugin_config_entry *entries;
  size_t count;
} PluginConfigEntries;

typedef struct {
  char **items;
  size_t count;
  size_t capacity;
} StringList;

static int print_json_document(yyjson_mut_doc *document) {
  char *json = yyjson_mut_write(document, YYJSON_WRITE_PRETTY, NULL);
  if (json == NULL) {
    return -1;
  }
  fputs(json, stdout);
  fputc('\n', stdout);
  free(json);
  return 0;
}

static int is_valid_js_identifier(const char *name) {
  if (name == NULL || name[0] == '\0') {
    return 0;
  }
  const unsigned char first = (unsigned char)name[0];
  if (!(isalpha(first) != 0 || name[0] == '_' || name[0] == '$')) {
    return 0;
  }
  for (size_t index = 1; name[index] != '\0'; index += 1) {
    const unsigned char current = (unsigned char)name[index];
    if (!(isalnum(current) != 0 || name[index] == '_' || name[index] == '$')) {
      return 0;
    }
  }
  return 1;
}

static int is_valid_namespace(const char *plugin_namespace) {
  if (plugin_namespace == NULL || plugin_namespace[0] == '\0') {
    return 0;
  }
  size_t segment_count = 0;
  const char *segment = plugin_namespace;
  for (;;) {
    const char *dot = strchr(segment, '.');
    const size_t length =
        dot == NULL ? strlen(segment) : (size_t)(dot - segment);
    if (length == 0) {
      return 0;
    }
    char *name = muon_substring(segment, length);
    if (name == NULL) {
      return 0;
    }
    const int valid = is_valid_js_identifier(name);
    free(name);
    if (!valid) {
      return 0;
    }
    segment_count += 1;
    if (dot == NULL) {
      break;
    }
    segment = dot + 1;
  }
  return segment_count >= 2;
}

static const char *get_public_function_name(
    const muon_plugin_function_metadata *function) {
  return function->filter_name == NULL ? function->js_name
                                       : function->filter_name;
}

static int validate_function_metadata(
    const muon_plugin_function_metadata *function,
    const char *plugin_name,
    const char *plugin_namespace) {
  if (function == NULL || function->js_name == NULL ||
      function->native_func == NULL) {
    fprintf(stderr, "invalid plugin function metadata: %s\n", plugin_name);
    return -1;
  }
  if (!is_valid_js_identifier(function->js_name)) {
    fprintf(stderr, "invalid plugin function name: %s.%s\n", plugin_namespace,
            function->js_name);
    return -1;
  }
  if (function->filter_name != NULL &&
      !is_valid_js_identifier(function->filter_name)) {
    fprintf(stderr, "invalid plugin function filter name: %s.%s\n",
            plugin_namespace, function->filter_name);
    return -1;
  }
  return 0;
}

static char *create_public_function_path(const char *plugin_namespace,
                                         const char *function_name) {
  const size_t size = strlen(plugin_namespace) + strlen(function_name) + 2;
  char *result = (char *)malloc(size);
  if (result == NULL) {
    return NULL;
  }
  snprintf(result, size, "%s.%s", plugin_namespace, function_name);
  return result;
}

static void string_list_clear(StringList *list) {
  if (list == NULL) {
    return;
  }
  muon_free_string_array(list->items, list->count);
  list->items = NULL;
  list->count = 0;
  list->capacity = 0;
}

static int string_list_contains(const StringList *list, const char *value) {
  for (size_t index = 0; index < list->count; index += 1) {
    if (strcmp(list->items[index], value) == 0) {
      return 1;
    }
  }
  return 0;
}

static int string_list_add_unique(StringList *list, char *value) {
  if (string_list_contains(list, value)) {
    free(value);
    return 0;
  }
  if (list->count == list->capacity) {
    const size_t next_capacity = list->capacity == 0 ? 8 : list->capacity * 2;
    char **next_items =
        (char **)realloc(list->items, sizeof(char *) * next_capacity);
    if (next_items == NULL) {
      free(value);
      return -1;
    }
    list->items = next_items;
    list->capacity = next_capacity;
  }
  list->items[list->count] = value;
  list->count += 1;
  return 0;
}

static int collect_metadata_functions(const muon_plugin_metadata *metadata,
                                      const char *plugin_name,
                                      StringList *functions) {
  if (metadata == NULL) {
    fprintf(stderr, "plugin declined loading: %s\n", plugin_name);
    return -1;
  }
  if (metadata->namespaces == NULL) {
    return 0;
  }

  for (const muon_plugin_namespace *const *namespace_entry =
           metadata->namespaces;
       *namespace_entry != NULL; namespace_entry += 1) {
    const muon_plugin_namespace *plugin_namespace = *namespace_entry;
    if (plugin_namespace->plugin_namespace == NULL ||
        !is_valid_namespace(plugin_namespace->plugin_namespace)) {
      fprintf(stderr, "invalid plugin namespace: %s\n", plugin_name);
      return -1;
    }
    if (plugin_namespace->functions == NULL) {
      continue;
    }

    for (const muon_plugin_function_metadata *const *function_entry =
             plugin_namespace->functions;
         *function_entry != NULL; function_entry += 1) {
      const muon_plugin_function_metadata *function = *function_entry;
      if (validate_function_metadata(function, plugin_name,
                                     plugin_namespace->plugin_namespace) != 0) {
        return -1;
      }
      char *path = create_public_function_path(
          plugin_namespace->plugin_namespace,
          get_public_function_name(function));
      if (path == NULL || string_list_add_unique(functions, path) != 0) {
        fprintf(stderr, "failed to allocate function catalog entry\n");
        free(path);
        return -1;
      }
    }
  }
  return 0;
}

static int hex_digit_value(char value) {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'a' && value <= 'f') {
    return value - 'a' + 10;
  }
  if (value >= 'A' && value <= 'F') {
    return value - 'A' + 10;
  }
  return -1;
}

static uint8_t *decode_hex_bytes(const char *hex, size_t *size) {
  const size_t hex_length = strlen(hex);
  if (hex_length % 2 != 0) {
    return NULL;
  }
  uint8_t *bytes = (uint8_t *)malloc(hex_length / 2);
  if (bytes == NULL && hex_length != 0) {
    return NULL;
  }
  for (size_t index = 0; index < hex_length; index += 2) {
    const int high = hex_digit_value(hex[index]);
    const int low = hex_digit_value(hex[index + 1]);
    if (high < 0 || low < 0) {
      free(bytes);
      return NULL;
    }
    bytes[index / 2] = (uint8_t)((high << 4) | low);
  }
  *size = hex_length / 2;
  return bytes;
}

static int sha256_file_with_salt_hex(
    const char *path,
    const uint8_t *salt,
    size_t salt_size,
    char output[SHA256_DIGEST_STRING_LENGTH]) {
  FILE *file = fopen(path, "rb");
  if (file == NULL) {
    return -1;
  }

  SHA256_CTX context;
  if (!SHA256_Init(&context)) {
    fclose(file);
    return -1;
  }

  uint8_t buffer[8192];
  for (;;) {
    const size_t read_size = fread(buffer, 1, sizeof(buffer), file);
    if (read_size > 0 && !SHA256_Update(&context, buffer, read_size)) {
      fclose(file);
      return -1;
    }
    if (read_size < sizeof(buffer)) {
      if (ferror(file)) {
        fclose(file);
        return -1;
      }
      break;
    }
  }
  fclose(file);

  if (salt_size > 0 && !SHA256_Update(&context, salt, salt_size)) {
    return -1;
  }

  uint8_t digest[SHA256_DIGEST_LENGTH];
  if (!SHA256_Final(digest, &context)) {
    return -1;
  }
  muon_sha256_digest_to_hex(digest, output);
  return 0;
}

static int verify_plugin_signature(const char *path,
                                   yyjson_val *plugin_object,
                                   const char *plugin_name) {
  yyjson_val *signature_value = yyjson_obj_get(plugin_object, "signature");
  if (signature_value == NULL) {
    return 0;
  }
  yyjson_val *salt_value = yyjson_obj_get(plugin_object, "salt");
  if (!yyjson_is_str(signature_value) || !yyjson_is_str(salt_value)) {
    fprintf(stderr, "plugin signature requires signature and salt: %s\n",
            plugin_name);
    return -1;
  }
  const char *expected_signature = yyjson_get_str(signature_value);
  const char *salt_hex = yyjson_get_str(salt_value);
  if (strlen(expected_signature) != SHA256_DIGEST_STRING_LENGTH - 1) {
    fprintf(stderr, "invalid plugin signature: %s\n", plugin_name);
    return -1;
  }

  size_t salt_size = 0;
  uint8_t *salt = decode_hex_bytes(salt_hex, &salt_size);
  if (salt == NULL) {
    fprintf(stderr, "invalid plugin signature salt: %s\n", plugin_name);
    return -1;
  }

  char actual_signature[SHA256_DIGEST_STRING_LENGTH];
  const int failed =
      sha256_file_with_salt_hex(path, salt, salt_size, actual_signature);
  free(salt);
  if (failed != 0) {
    fprintf(stderr, "failed to calculate plugin signature: %s\n", path);
    return -1;
  }
  if (strcmp(expected_signature, actual_signature) != 0) {
    fprintf(stderr, "plugin signature mismatch: %s expected %s actual %s\n",
            path, expected_signature, actual_signature);
    return -1;
  }
  return 0;
}

static void clear_plugin_config_entries(PluginConfigEntries *config) {
  free(config->entries);
  config->entries = NULL;
  config->count = 0;
}

static int read_plugin_config_entries(yyjson_val *plugin_object,
                                      PluginConfigEntries *config) {
  config->entries = NULL;
  config->count = 0;

  yyjson_val *config_object = yyjson_obj_get(plugin_object, "config");
  if (config_object == NULL) {
    return 0;
  }
  if (!yyjson_is_obj(config_object)) {
    fprintf(stderr, "plugin config must be an object\n");
    return -1;
  }

  config->count = yyjson_obj_size(config_object);
  if (config->count == 0) {
    return 0;
  }
  config->entries =
      (muon_plugin_config_entry *)calloc(config->count, sizeof(config->entries[0]));
  if (config->entries == NULL) {
    fprintf(stderr, "failed to allocate plugin config entries\n");
    return -1;
  }

  size_t index = 0;
  size_t max = 0;
  yyjson_val *key = NULL;
  yyjson_val *value = NULL;
  yyjson_obj_foreach(config_object, index, max, key, value) {
    if (!yyjson_is_str(key) || !yyjson_is_str(value)) {
      clear_plugin_config_entries(config);
      fprintf(stderr, "plugin config values must be strings\n");
      return -1;
    }
    config->entries[index].key = yyjson_get_str(key);
    config->entries[index].value = yyjson_get_str(value);
  }
  return 0;
}

#ifdef _WIN32
static wchar_t *utf8_to_wide(const char *value) {
  const int length = MultiByteToWideChar(CP_UTF8, 0, value, -1, NULL, 0);
  if (length <= 0) {
    return NULL;
  }
  wchar_t *wide = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)length);
  if (wide == NULL) {
    return NULL;
  }
  if (MultiByteToWideChar(CP_UTF8, 0, value, -1, wide, length) <= 0) {
    free(wide);
    return NULL;
  }
  return wide;
}

static void *open_plugin_library(const char *path) {
  wchar_t *wide_path = utf8_to_wide(path);
  if (wide_path == NULL) {
    return NULL;
  }
  HMODULE handle = LoadLibraryW(wide_path);
  free(wide_path);
  return (void *)handle;
}

static muon_init_plugin_func get_muon_init_plugin_symbol(void *handle) {
  union {
    FARPROC proc;
    muon_init_plugin_func function;
  } symbol;
  symbol.proc = GetProcAddress((HMODULE)handle, "muon_init_plugin");
  return symbol.function;
}

static void close_plugin_library(void *handle) {
  if (handle != NULL) {
    FreeLibrary((HMODULE)handle);
  }
}
#else
static void *open_plugin_library(const char *path) {
  return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}

static muon_init_plugin_func get_muon_init_plugin_symbol(void *handle) {
  union {
    void *object;
    muon_init_plugin_func function;
  } symbol;
  symbol.object = dlsym(handle, "muon_init_plugin");
  return symbol.function;
}

static void close_plugin_library(void *handle) {
  if (handle != NULL) {
    dlclose(handle);
  }
}
#endif

static uint8_t stub_register_pure_function(
    const muon_function_signature *signature,
    muon_user_function function,
    muon_native_function *out_function,
    muon_error_buffer *error) {
  (void)signature;
  (void)error;
  if (function == NULL || out_function == NULL) {
    return 0;
  }
  *out_function = (muon_native_function)function;
  return 1;
}

static uint8_t stub_register_closure(
    const muon_function_signature *signature,
    muon_user_function function,
    void *state,
    muon_finalize_user_data finalize_state,
    muon_native_function *out_function,
    muon_error_buffer *error) {
  (void)signature;
  (void)state;
  (void)finalize_state;
  (void)error;
  if (function == NULL || out_function == NULL) {
    return 0;
  }
  *out_function = (muon_native_function)function;
  return 1;
}

static uint8_t stub_create_completion_function(
    const muon_type_descriptor *return_type,
    muon_completion_callback callback,
    void *user_data,
    muon_completion_func *out_completion,
    muon_error_buffer *error) {
  (void)return_type;
  (void)callback;
  (void)user_data;
  (void)out_completion;
  if (error != NULL && error->message != NULL && error->message_capacity > 0) {
    snprintf(error->message, error->message_capacity,
             "completion helpers are unavailable during inspection");
  }
  return 0;
}

static uint8_t stub_retain_function(muon_native_function retained_func) {
  return retained_func == NULL ? 0 : 1;
}

static void stub_release_function(muon_native_function released_func) {
  (void)released_func;
}

static uint8_t stub_allocate_shared_buffer(uintptr_t size,
                                           muon_buffer_view *out_view,
                                           muon_shared_buffer_handle *out_handle,
                                           muon_error_buffer *error) {
  (void)error;
  if (out_view == NULL || out_handle == NULL) {
    return 0;
  }
  void *data = calloc(1, size == 0 ? 1 : (size_t)size);
  if (data == NULL) {
    return 0;
  }
  out_view->data = data;
  out_view->size = size;
  *out_handle = (muon_shared_buffer_handle)data;
  return 1;
}

static void stub_release_shared_buffer(muon_shared_buffer_handle handle) {
  free((void *)handle);
}

static void stub_log_message(muon_log_level level, const char *message) {
  (void)level;
  (void)message;
}

static const muon_plugin_helpers inspector_helpers = {
    stub_register_pure_function,
    stub_register_closure,
    stub_retain_function,
    stub_release_function,
    stub_allocate_shared_buffer,
    stub_release_shared_buffer,
    stub_create_completion_function,
    stub_log_message,
};

static int inspect_plugin(const char *plugin_directory,
                          yyjson_val *plugin_object,
                          yyjson_mut_doc *output_document,
                          yyjson_mut_val *output_plugins) {
  yyjson_val *name_value = yyjson_obj_get(plugin_object, "name");
  if (!yyjson_is_str(name_value) || yyjson_get_str(name_value)[0] == '\0') {
    fprintf(stderr, "plugin name must be a non-empty string\n");
    return -1;
  }

  const char *plugin_name = yyjson_get_str(name_value);
  char *file_name = (char *)malloc(strlen(plugin_name) +
                                   strlen(MUON_PLUGIN_LIBRARY_EXTENSION) + 1);
  if (file_name == NULL) {
    fprintf(stderr, "failed to allocate plugin file name\n");
    return -1;
  }
  sprintf(file_name, "%s%s", plugin_name, MUON_PLUGIN_LIBRARY_EXTENSION);
  char *path = muon_path_join(plugin_directory, file_name);
  free(file_name);
  if (path == NULL) {
    fprintf(stderr, "failed to allocate plugin path\n");
    return -1;
  }
  if (!muon_path_exists(path)) {
    fprintf(stderr, "plugin file not found: %s\n", path);
    free(path);
    return -1;
  }
  if (verify_plugin_signature(path, plugin_object, plugin_name) != 0) {
    free(path);
    return -1;
  }

  void *handle = open_plugin_library(path);
  if (handle == NULL) {
    fprintf(stderr, "failed to load plugin: %s\n", path);
    free(path);
    return -1;
  }

  const muon_init_plugin_func init_plugin =
      get_muon_init_plugin_symbol(handle);
  if (init_plugin == NULL) {
    fprintf(stderr, "plugin is missing muon_init_plugin: %s\n", path);
    close_plugin_library(handle);
    free(path);
    return -1;
  }

  PluginConfigEntries config;
  if (read_plugin_config_entries(plugin_object, &config) != 0) {
    close_plugin_library(handle);
    free(path);
    return -1;
  }
  const muon_plugin_init_context context = {
      &inspector_helpers,
      plugin_name,
      (uint32_t)config.count,
      config.count == 0 ? NULL : config.entries,
  };
  const muon_plugin_metadata *metadata = init_plugin(&context);

  StringList functions = {0};
  const int collect_failed =
      collect_metadata_functions(metadata, plugin_name, &functions);
  clear_plugin_config_entries(&config);
  close_plugin_library(handle);
  free(path);
  if (collect_failed != 0) {
    string_list_clear(&functions);
    return -1;
  }

  yyjson_mut_val *plugin_output = yyjson_mut_obj(output_document);
  yyjson_mut_val *function_array = yyjson_mut_arr(output_document);
  if (plugin_output == NULL || function_array == NULL ||
      !yyjson_mut_obj_add_strcpy(output_document, plugin_output, "name",
                                 plugin_name) ||
      !yyjson_mut_obj_add_val(output_document, plugin_output, "functions",
                              function_array) ||
      !yyjson_mut_arr_add_val(output_plugins, plugin_output)) {
    string_list_clear(&functions);
    fprintf(stderr, "failed to build inspector output\n");
    return -1;
  }
  for (size_t index = 0; index < functions.count; index += 1) {
    if (!yyjson_mut_arr_add_strcpy(output_document, function_array,
                                   functions.items[index])) {
      string_list_clear(&functions);
      fprintf(stderr, "failed to build inspector output\n");
      return -1;
    }
  }
  string_list_clear(&functions);
  return 0;
}

static int inspect_plugins(yyjson_val *input, yyjson_mut_doc *output_document) {
  yyjson_val *path_value = yyjson_obj_get(input, "path");
  yyjson_val *plugins = yyjson_obj_get(input, "plugins");
  if (!yyjson_is_str(path_value) || yyjson_get_str(path_value)[0] == '\0' ||
      !yyjson_is_arr(plugins)) {
    fprintf(stderr, "inspector input requires path and plugins\n");
    return -1;
  }

  yyjson_mut_val *root = yyjson_mut_obj(output_document);
  yyjson_mut_val *output_plugins = yyjson_mut_arr(output_document);
  if (root == NULL || output_plugins == NULL ||
      !yyjson_mut_obj_add_val(output_document, root, "plugins",
                              output_plugins)) {
    fprintf(stderr, "failed to build inspector output\n");
    return -1;
  }
  yyjson_mut_doc_set_root(output_document, root);

  size_t index = 0;
  size_t max = 0;
  yyjson_val *plugin_object = NULL;
  yyjson_arr_foreach(plugins, index, max, plugin_object) {
    if (!yyjson_is_obj(plugin_object) ||
        inspect_plugin(yyjson_get_str(path_value), plugin_object,
                       output_document, output_plugins) != 0) {
      return -1;
    }
  }
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "Usage: muon-plugin-inspector <input-json>\n");
    return 1;
  }

  yyjson_doc *input_document = muon_json_read_file(argv[1]);
  yyjson_val *input =
      input_document == NULL ? NULL : yyjson_doc_get_root(input_document);
  if (!yyjson_is_obj(input)) {
    yyjson_doc_free(input_document);
    fprintf(stderr, "failed to read inspector input: %s\n", argv[1]);
    return 1;
  }

  yyjson_mut_doc *output_document = yyjson_mut_doc_new(NULL);
  if (output_document == NULL) {
    yyjson_doc_free(input_document);
    fprintf(stderr, "failed to allocate inspector output\n");
    return 1;
  }

  const int failed = inspect_plugins(input, output_document) != 0 ||
                     print_json_document(output_document) != 0;
  yyjson_mut_doc_free(output_document);
  yyjson_doc_free(input_document);
  return failed ? 1 : 0;
}
