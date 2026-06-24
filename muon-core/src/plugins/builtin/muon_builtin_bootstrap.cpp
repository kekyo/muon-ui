/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "plugins/builtin/muon_builtin_bootstrap.h"

#include "config/muon_paths.h"
#include "muon_string_helpers.h"
#include "yyjson.h"

#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace muon_internal {

static const muon_type_descriptor type_void = {
    MUON_TYPE_VOID,
    nullptr,
};

static const muon_type_descriptor type_string = {
    MUON_TYPE_STRING,
    nullptr,
};

static const muon_type_descriptor settings_args[] = {
    type_string,
};

static constexpr auto kBootstrapConfigFileName = "muon-bootstrap.ini";
static constexpr auto kDefaultCatalogRefreshIntervalSeconds =
    uint64_t{604800};

struct BootstrapSettings {
  std::string cef_version_policy = "tested";
  bool has_cef_version_policy = false;
  std::string cef_exact_version;
  bool has_cef_exact_version = false;
  uint64_t catalog_refresh_interval_seconds =
      kDefaultCatalogRefreshIntervalSeconds;
  bool has_catalog_refresh_interval_seconds = false;
  uint64_t last_catalog_update_unix = 0;
  bool update_requested = false;
  uint64_t update_requested_at_unix = 0;
};

enum class JsonPatchFieldState {
  Absent,
  Null,
  Value,
};

static std::string g_default_version_policy = "tested";

static uint64_t CurrentTimeSeconds();

static void CompleteString(muon_completion_func completion,
                           const std::string& result) {
  const auto* pointer = result.c_str();
  completion(&pointer, nullptr);
}

static void CompleteVoid(muon_completion_func completion) {
  completion(nullptr, nullptr);
}

static void CompleteError(muon_completion_func completion,
                          const std::string& message) {
  completion(nullptr, message.c_str());
}

static bool IsValidPolicy(std::string_view value) {
  return value == "tested" || value == "same-major-latest" ||
         value == "compat-latest" || value == "exact";
}

static std::string GetDefaultVersionPolicy() {
  return IsValidPolicy(g_default_version_policy) ? g_default_version_policy
                                                 : "tested";
}

static bool ParseUint64(const std::string& value, uint64_t* result) {
  if (value.empty()) {
    return false;
  }
  auto parsed = uint64_t{0};
  for (const auto character : value) {
    if (!std::isdigit(static_cast<unsigned char>(character))) {
      return false;
    }
    const auto digit = static_cast<uint64_t>(character - '0');
    if (parsed > (UINT64_MAX - digit) / 10U) {
      return false;
    }
    parsed = parsed * 10U + digit;
  }
  *result = parsed;
  return true;
}

static bool ParseBool(const std::string& value, bool* result) {
  if (value == "true" || value == "1") {
    *result = true;
    return true;
  }
  if (value == "false" || value == "0") {
    *result = false;
    return true;
  }
  return false;
}

static bool ValidateSettings(const BootstrapSettings& settings,
                             std::string* error_message) {
  if (!IsValidPolicy(settings.cef_version_policy)) {
    *error_message = "Invalid CEF version policy";
    return false;
  }
  if (settings.cef_version_policy == "exact" &&
      settings.cef_exact_version.empty()) {
    *error_message = "cefExactVersion is required for exact CEF policy";
    return false;
  }
  error_message->clear();
  return true;
}

static std::filesystem::path GetBootstrapConfigPath() {
  return GetMuonExecutableDirectory() / kBootstrapConfigFileName;
}

static bool ApplyEntry(BootstrapSettings* settings,
                       const std::string& section,
                       const std::string& key,
                       const std::string& value,
                       std::string* error_message) {
  if (section == "cef" && key == "versionPolicy") {
    if (!IsValidPolicy(value)) {
      *error_message = "Invalid CEF version policy";
      return false;
    }
    settings->cef_version_policy = value;
    settings->has_cef_version_policy = true;
    return true;
  }
  if (section == "cef" && key == "exactVersion") {
    settings->cef_exact_version = value;
    settings->has_cef_exact_version = true;
    return true;
  }
  if (section == "cef" && key == "catalogRefreshIntervalSeconds") {
    if (!ParseUint64(value, &settings->catalog_refresh_interval_seconds)) {
      *error_message = "Invalid catalogRefreshIntervalSeconds";
      return false;
    }
    settings->has_catalog_refresh_interval_seconds = true;
    return true;
  }
  if (section == "cef" && key == "lastCatalogUpdateUnix") {
    if (!ParseUint64(value, &settings->last_catalog_update_unix)) {
      *error_message = "Invalid lastCatalogUpdateUnix";
      return false;
    }
    return true;
  }
  if (section == "update" && key == "requested") {
    if (!ParseBool(value, &settings->update_requested)) {
      *error_message = "Invalid requested";
      return false;
    }
    return true;
  }
  if (section == "update" && key == "requestedAtUnix") {
    if (!ParseUint64(value, &settings->update_requested_at_unix)) {
      *error_message = "Invalid requestedAtUnix";
      return false;
    }
    return true;
  }
  return true;
}

static bool ReadSettings(BootstrapSettings* settings,
                         std::string* error_message) {
  *settings = BootstrapSettings{};
  settings->cef_version_policy = GetDefaultVersionPolicy();
  const auto path = GetBootstrapConfigPath();
  if (!std::filesystem::exists(path)) {
    return ValidateSettings(*settings, error_message);
  }
  std::ifstream input(path);
  if (!input) {
    *error_message = "Failed to read muon-bootstrap.ini";
    return false;
  }
  std::string section;
  std::string line;
  while (std::getline(input, line)) {
    auto entry = TrimAscii(line);
    if (entry.empty() || entry[0] == '#' || entry[0] == ';') {
      continue;
    }
    if (entry.front() == '[' && entry.back() == ']') {
      section = TrimAscii(std::string_view(entry).substr(1, entry.size() - 2));
      continue;
    }
    const auto equals = entry.find('=');
    if (equals == std::string::npos) {
      continue;
    }
    if (!ApplyEntry(settings, section,
                    TrimAscii(std::string_view(entry).substr(0, equals)),
                    TrimAscii(std::string_view(entry).substr(equals + 1)),
                    error_message)) {
      return false;
    }
  }
  return ValidateSettings(*settings, error_message);
}

static std::string CreateSettingsIni(const BootstrapSettings& settings) {
  std::ostringstream output;
  output << "[cef]\n";
  if (settings.has_cef_version_policy) {
    output << "versionPolicy=" << settings.cef_version_policy << "\n";
  }
  if (settings.has_cef_exact_version) {
    output << "exactVersion=" << settings.cef_exact_version << "\n";
  }
  if (settings.has_catalog_refresh_interval_seconds) {
    output << "catalogRefreshIntervalSeconds="
           << settings.catalog_refresh_interval_seconds << "\n";
  }
  output << "lastCatalogUpdateUnix=" << settings.last_catalog_update_unix
         << "\n\n";
  output << "[update]\n";
  output << "requested=" << (settings.update_requested ? "true" : "false")
         << "\n";
  output << "requestedAtUnix=" << settings.update_requested_at_unix << "\n";
  return output.str();
}

static bool WriteSettings(const BootstrapSettings& settings,
                          std::string* error_message) {
  if (!ValidateSettings(settings, error_message)) {
    return false;
  }
  const auto path = GetBootstrapConfigPath();
  const auto temporary_path =
      path.parent_path() /
      (path.filename().string() + ".tmp." +
       std::to_string(CurrentTimeSeconds()));
  std::ofstream output(temporary_path, std::ios::binary | std::ios::trunc);
  if (!output) {
    *error_message = "Failed to write muon-bootstrap.ini";
    return false;
  }
  output << CreateSettingsIni(settings);
  output.close();
  if (!output) {
    std::error_code remove_error;
    std::filesystem::remove(temporary_path, remove_error);
    *error_message = "Failed to write muon-bootstrap.ini";
    return false;
  }
  std::error_code error;
  std::filesystem::rename(temporary_path, path, error);
  if (error) {
    std::filesystem::remove(path, error);
    error.clear();
    std::filesystem::rename(temporary_path, path, error);
  }
  if (error) {
    std::error_code remove_error;
    std::filesystem::remove(temporary_path, remove_error);
    *error_message = "Failed to replace muon-bootstrap.ini";
    return false;
  }
  error_message->clear();
  return true;
}

static uint64_t CurrentTimeSeconds() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

static bool AddString(yyjson_mut_doc* document,
                      yyjson_mut_val* object,
                      const char* key,
                      const std::string& value) {
  return yyjson_mut_obj_add_strncpy(document, object, key, value.data(),
                                    value.size());
}

static bool CreateSettingsJson(const BootstrapSettings& settings,
                               std::string* result,
                               std::string* error_message) {
  auto* document = yyjson_mut_doc_new(nullptr);
  auto* root = document == nullptr ? nullptr : yyjson_mut_obj(document);
  if (document == nullptr || root == nullptr) {
    yyjson_mut_doc_free(document);
    *error_message = "Failed to create bootstrap settings JSON";
    return false;
  }
  yyjson_mut_doc_set_root(document, root);
  if (!AddString(document, root, "cefVersionPolicy",
                 settings.cef_version_policy) ||
      !AddString(document, root, "cefExactVersion",
                 settings.cef_exact_version) ||
      !yyjson_mut_obj_add_uint(document, root,
                               "catalogRefreshIntervalSeconds",
                               settings.catalog_refresh_interval_seconds)) {
    yyjson_mut_doc_free(document);
    *error_message = "Failed to build bootstrap settings JSON";
    return false;
  }
  size_t json_size = 0;
  auto* json = yyjson_mut_write(document, YYJSON_WRITE_NOFLAG, &json_size);
  yyjson_mut_doc_free(document);
  if (json == nullptr) {
    *error_message = "Failed to serialize bootstrap settings JSON";
    return false;
  }
  result->assign(json, json_size);
  std::free(json);
  error_message->clear();
  return true;
}

static bool ReadJsonString(yyjson_val* object,
                           const char* key,
                           std::string* value,
                           JsonPatchFieldState* state,
                           std::string* error_message) {
  auto* field = yyjson_obj_get(object, key);
  if (field == nullptr) {
    *state = JsonPatchFieldState::Absent;
    return true;
  }
  if (yyjson_is_null(field)) {
    *state = JsonPatchFieldState::Null;
    return true;
  }
  if (!yyjson_is_str(field)) {
    *error_message = std::string(key) + " must be a string";
    return false;
  }
  *state = JsonPatchFieldState::Value;
  *value = yyjson_get_str(field);
  return true;
}

static bool ReadJsonUint64(yyjson_val* object,
                           const char* key,
                           uint64_t* value,
                           JsonPatchFieldState* state,
                           std::string* error_message) {
  auto* field = yyjson_obj_get(object, key);
  if (field == nullptr) {
    *state = JsonPatchFieldState::Absent;
    return true;
  }
  if (yyjson_is_null(field)) {
    *state = JsonPatchFieldState::Null;
    return true;
  }
  if (!yyjson_is_uint(field)) {
    *error_message = std::string(key) + " must be a non-negative integer";
    return false;
  }
  *state = JsonPatchFieldState::Value;
  *value = yyjson_get_uint(field);
  return true;
}

static bool ApplySettingsPatch(BootstrapSettings* settings,
                               const char* patch_json,
                               std::string* error_message) {
  std::string patch(patch_json);
  yyjson_read_err error;
  auto* document = yyjson_read_opts(patch.data(), patch.size(),
                                    YYJSON_READ_NOFLAG, nullptr, &error);
  auto* root = document == nullptr ? nullptr : yyjson_doc_get_root(document);
  if (document == nullptr || root == nullptr || !yyjson_is_obj(root)) {
    yyjson_doc_free(document);
    *error_message = "Bootstrap settings patch must be a JSON object";
    return false;
  }
  auto state = JsonPatchFieldState::Absent;
  std::string string_value;
  uint64_t uint_value = 0;
  if (!ReadJsonString(root, "cefVersionPolicy", &string_value, &state,
                      error_message)) {
    yyjson_doc_free(document);
    return false;
  }
  if (state == JsonPatchFieldState::Value) {
    settings->cef_version_policy = string_value;
    settings->has_cef_version_policy = true;
  } else if (state == JsonPatchFieldState::Null) {
    settings->cef_version_policy = GetDefaultVersionPolicy();
    settings->has_cef_version_policy = false;
  }
  if (!ReadJsonString(root, "cefExactVersion", &string_value, &state,
                      error_message)) {
    yyjson_doc_free(document);
    return false;
  }
  if (state == JsonPatchFieldState::Value) {
    settings->cef_exact_version = string_value;
    settings->has_cef_exact_version = true;
  } else if (state == JsonPatchFieldState::Null) {
    settings->cef_exact_version.clear();
    settings->has_cef_exact_version = false;
  }
  if (!ReadJsonUint64(root, "catalogRefreshIntervalSeconds", &uint_value,
                      &state, error_message)) {
    yyjson_doc_free(document);
    return false;
  }
  if (state == JsonPatchFieldState::Value) {
    settings->catalog_refresh_interval_seconds = uint_value;
    settings->has_catalog_refresh_interval_seconds = true;
  } else if (state == JsonPatchFieldState::Null) {
    settings->catalog_refresh_interval_seconds =
        kDefaultCatalogRefreshIntervalSeconds;
    settings->has_catalog_refresh_interval_seconds = false;
  }
  yyjson_doc_free(document);
  return ValidateSettings(*settings, error_message);
}

extern "C" void muon_builtin_bootstrap_get_settings(
    muon_completion_func completion) {
  BootstrapSettings settings;
  std::string error_message;
  if (!ReadSettings(&settings, &error_message)) {
    CompleteError(completion, error_message);
    return;
  }
  std::string json;
  if (!CreateSettingsJson(settings, &json, &error_message)) {
    CompleteError(completion, error_message);
    return;
  }
  CompleteString(completion, json);
}

extern "C" void muon_builtin_bootstrap_set_settings(
    muon_completion_func completion,
    const char* settings_json) {
  BootstrapSettings settings;
  std::string error_message;
  if (settings_json == nullptr ||
      !ReadSettings(&settings, &error_message) ||
      !ApplySettingsPatch(&settings, settings_json, &error_message) ||
      !WriteSettings(settings, &error_message)) {
    CompleteError(completion, error_message);
    return;
  }
  CompleteVoid(completion);
}

extern "C" void muon_builtin_bootstrap_trigger_update(
    muon_completion_func completion) {
  BootstrapSettings settings;
  std::string error_message;
  if (!ReadSettings(&settings, &error_message)) {
    CompleteError(completion, error_message);
    return;
  }
  settings.update_requested = true;
  settings.update_requested_at_unix = CurrentTimeSeconds();
  if (!WriteSettings(settings, &error_message)) {
    CompleteError(completion, error_message);
    return;
  }
  CompleteVoid(completion);
}

static const muon_plugin_function_metadata get_settings_function = {
    "__getSettings",
    reinterpret_cast<muon_native_function>(
        &muon_builtin_bootstrap_get_settings),
    {0, nullptr, &type_string},
    "getSettings",
};

static const muon_plugin_function_metadata set_settings_function = {
    "__setSettings",
    reinterpret_cast<muon_native_function>(
        &muon_builtin_bootstrap_set_settings),
    {1, settings_args, &type_void},
    "setSettings",
};

static const muon_plugin_function_metadata trigger_update_function = {
    "__triggerUpdate",
    reinterpret_cast<muon_native_function>(
        &muon_builtin_bootstrap_trigger_update),
    {0, nullptr, &type_void},
    "triggerUpdate",
};

static const muon_plugin_function_metadata* const bootstrap_functions[] = {
    &get_settings_function,
    &set_settings_function,
    &trigger_update_function,
    nullptr,
};

static constexpr char bootstrap_setup_script[] = R"JS(
const parseNativeJson = async (source) => JSON.parse(await source);
const properties = {};
if (isAllowed("getSettings")) {
  properties.getSettings = {
    enumerable: true,
    configurable: false,
    writable: false,
    value: async () => parseNativeJson(namespace.__getSettings()),
  };
}
if (isAllowed("setSettings")) {
  properties.setSettings = {
    enumerable: true,
    configurable: false,
    writable: false,
    value: async (settings) => namespace.__setSettings(JSON.stringify(settings ?? {})),
  };
}
if (isAllowed("triggerUpdate")) {
  properties.triggerUpdate = {
    enumerable: true,
    configurable: false,
    writable: false,
    value: async () => namespace.__triggerUpdate(),
  };
}
Object.defineProperties(namespace, properties);
)JS";

}  // namespace muon_internal

void InitializeMuonBuiltinBootstrap(const std::string& default_version_policy) {
  muon_internal::g_default_version_policy =
      muon_internal::IsValidPolicy(default_version_policy)
          ? default_version_policy
          : "tested";
}

const muon_plugin_namespace kMuonBuiltinBootstrapNamespace = {
    "muon.bootstrap",
    muon_internal::bootstrap_setup_script,
    muon_internal::bootstrap_functions,
};

static const muon_plugin_namespace* const bootstrap_namespaces[] = {
    &kMuonBuiltinBootstrapNamespace,
    nullptr,
};

static const muon_plugin_metadata bootstrap_metadata = {
    bootstrap_namespaces,
};

const muon_plugin_metadata* GetMuonBuiltinBootstrapPluginMetadata() {
  return &bootstrap_metadata;
}
