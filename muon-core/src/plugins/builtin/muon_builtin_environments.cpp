/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "plugins/builtin/muon_builtin_environments.h"

#include "config/muon_autostart.h"
#include "config/muon_startup.h"

#include "include/cef_api_hash.h"
#include "include/cef_version_info.h"
#include "muon_runtime_info_generated.h"
#include "yyjson.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if !defined(_WIN32)
extern char** environ;
#endif

namespace muon_internal {

static const muon_type_descriptor type_string = {
    MUON_TYPE_STRING,
    nullptr,
};

static const muon_type_descriptor type_void = {
    MUON_TYPE_VOID,
    nullptr,
};

static const muon_type_descriptor type_bool = {
    MUON_TYPE_BOOL,
    nullptr,
};

static const muon_type_descriptor type_u32 = {
    MUON_TYPE_U32,
    nullptr,
};

static const muon_type_descriptor set_autostart_args[] = {
    type_bool,
};

static constexpr char kRuntimeInfoKey[] = "cefRuntime";
static constexpr char kRuntimeInfoVersionKey[] = "version";
static constexpr char kRuntimeInfoApiVersionKey[] = "apiVersion";
static constexpr char kRuntimeInfoApiHashKey[] = "apiHash";

static void AppendJsonHex(std::string* target, uint8_t value) {
  constexpr char kHex[] = "0123456789abcdef";
  target->push_back(kHex[(value >> 4) & 0x0f]);
  target->push_back(kHex[value & 0x0f]);
}

static void AppendJsonString(std::string* target, std::string_view value) {
  target->push_back('"');
  for (const auto character : value) {
    const auto byte = static_cast<uint8_t>(character);
    switch (character) {
      case '"':
        *target += "\\\"";
        break;
      case '\\':
        *target += "\\\\";
        break;
      case '\b':
        *target += "\\b";
        break;
      case '\f':
        *target += "\\f";
        break;
      case '\n':
        *target += "\\n";
        break;
      case '\r':
        *target += "\\r";
        break;
      case '\t':
        *target += "\\t";
        break;
      default:
        if (byte < 0x20) {
          *target += "\\u00";
          AppendJsonHex(target, byte);
        } else {
          target->push_back(character);
        }
        break;
    }
  }
  target->push_back('"');
}

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

static std::string CreateJsonStringArray(
    const std::vector<std::string>& values) {
  std::string json = "[";
  for (auto index = size_t{0}; index < values.size(); ++index) {
    if (index > 0) {
      json += ",";
    }
    AppendJsonString(&json, values[index]);
  }
  json += "]";
  return json;
}

static std::string CreateJsonStringObject(
    const std::vector<std::pair<std::string, std::string>>& entries) {
  std::string json = "{";
  for (auto index = size_t{0}; index < entries.size(); ++index) {
    if (index > 0) {
      json += ",";
    }
    AppendJsonString(&json, entries[index].first);
    json += ":";
    AppendJsonString(&json, entries[index].second);
  }
  json += "}";
  return json;
}

static std::string CreateCefRuntimeVersion(const cef_version_info_t& info,
                                           const char* commit_hash) {
  const auto hash = std::string(commit_hash);
  const auto short_hash = hash.substr(0, 7);
  return std::to_string(info.cef_version_major) + "." +
         std::to_string(info.cef_version_minor) + "." +
         std::to_string(info.cef_version_patch) + "+g" + short_hash +
         "+chromium-" + std::to_string(info.chrome_version_major) + "." +
         std::to_string(info.chrome_version_minor) + "." +
         std::to_string(info.chrome_version_build) + "." +
         std::to_string(info.chrome_version_patch);
}

static bool AddRuntimeString(yyjson_mut_doc* document,
                             yyjson_mut_val* object,
                             const char* key,
                             const std::string& value) {
  return yyjson_mut_obj_add_strncpy(
      document, object, key, value.data(), value.size());
}

static bool AddRuntimeString(yyjson_mut_doc* document,
                             yyjson_mut_val* object,
                             const char* key,
                             const char* value) {
  return value != nullptr &&
         yyjson_mut_obj_add_strcpy(document, object, key, value);
}

static bool CreateRuntimeInfoJson(std::string* result,
                                  std::string* error_message) {
  auto* document = yyjson_mut_doc_new(nullptr);
  if (document == nullptr) {
    *error_message = "Failed to create muon runtime info JSON";
    return false;
  }

  auto* root = yyjson_mut_obj(document);
  auto* muon_core = yyjson_mut_obj(document);
  auto* cef_reference = yyjson_mut_obj(document);
  auto* cef_artifact = yyjson_mut_obj(document);
  auto* core_payload = yyjson_mut_arr(document);
  if (root == nullptr || muon_core == nullptr || cef_reference == nullptr ||
      cef_artifact == nullptr || core_payload == nullptr) {
    yyjson_mut_doc_free(document);
    *error_message = "Failed to allocate muon runtime info JSON";
    return false;
  }
  yyjson_mut_doc_set_root(document, root);

  cef_version_info_t version_info = {};
  version_info.size = sizeof(version_info);
  cef_version_info_all(&version_info);
  const auto* api_hash = cef_api_hash(CEF_API_VERSION, 0);
  const auto* commit_hash = cef_api_hash(CEF_API_VERSION, 2);
  if (api_hash == nullptr || commit_hash == nullptr ||
      std::strlen(commit_hash) < 7) {
    yyjson_mut_doc_free(document);
    *error_message = "Failed to read CEF runtime version information";
    return false;
  }

  for (auto index = size_t{0}; index < kMuonRuntimeInfo.core_payload_count;
       ++index) {
    const auto* payload_item = kMuonRuntimeInfo.core_payload[index];
    if (payload_item == nullptr ||
        !yyjson_mut_arr_add_strcpy(document, core_payload, payload_item)) {
      yyjson_mut_doc_free(document);
      *error_message = "Failed to build muon runtime payload JSON";
      return false;
    }
  }

  auto* runtime = yyjson_mut_obj(document);
  if (runtime == nullptr ||
      !AddRuntimeString(document, runtime, kRuntimeInfoVersionKey,
                        CreateCefRuntimeVersion(version_info, commit_hash)) ||
      !yyjson_mut_obj_add_int(document, runtime, kRuntimeInfoApiVersionKey,
                              cef_api_version()) ||
      !yyjson_mut_obj_add_strcpy(document, runtime,
                                 kRuntimeInfoApiHashKey, api_hash) ||
      !AddRuntimeString(document, muon_core, kRuntimeInfoVersionKey,
                        kMuonRuntimeInfo.muon_core_version) ||
      !AddRuntimeString(document, muon_core, "gitCommitHash",
                        kMuonRuntimeInfo.muon_core_git_commit_hash) ||
      !AddRuntimeString(document, cef_artifact, "fileName",
                        kMuonRuntimeInfo.cef_reference_artifact.file_name) ||
      !AddRuntimeString(document, cef_artifact, "url",
                        kMuonRuntimeInfo.cef_reference_artifact.url) ||
      !AddRuntimeString(document, cef_artifact, "sha1",
                        kMuonRuntimeInfo.cef_reference_artifact.sha1) ||
      !yyjson_mut_obj_add_uint(document, cef_artifact, "size",
                               kMuonRuntimeInfo.cef_reference_artifact.size) ||
      !AddRuntimeString(document, cef_reference, kRuntimeInfoVersionKey,
                        kMuonRuntimeInfo.cef_reference_version) ||
      !AddRuntimeString(document, cef_reference, "distribution",
                        kMuonRuntimeInfo.cef_reference_distribution) ||
      !yyjson_mut_obj_add_int(
          document, cef_reference, kRuntimeInfoApiVersionKey,
          kMuonRuntimeInfo.cef_reference_api_version) ||
      !AddRuntimeString(document, cef_reference, kRuntimeInfoApiHashKey,
                        kMuonRuntimeInfo.cef_reference_api_hash) ||
      !yyjson_mut_obj_add_val(document, cef_reference, "artifact",
                              cef_artifact) ||
      !AddRuntimeString(document, root, "name", kMuonRuntimeInfo.name) ||
      !AddRuntimeString(document, root, "executableName",
                        kMuonRuntimeInfo.executable_name) ||
      !AddRuntimeString(document, root, "target", kMuonRuntimeInfo.target) ||
      !yyjson_mut_obj_add_val(document, root, "muonCore", muon_core) ||
      !yyjson_mut_obj_add_val(document, root, "cefReference",
                              cef_reference) ||
      !yyjson_mut_obj_add_val(document, root, kRuntimeInfoKey, runtime) ||
      !yyjson_mut_obj_add_val(document, root, "corePayload",
                              core_payload)) {
    yyjson_mut_doc_free(document);
    *error_message = "Failed to build muon runtime info JSON";
    return false;
  }

  size_t json_size = 0;
  auto* json = yyjson_mut_write(document, YYJSON_WRITE_NOFLAG, &json_size);
  yyjson_mut_doc_free(document);
  if (json == nullptr) {
    *error_message = "Failed to serialize muon runtime info JSON";
    return false;
  }
  result->assign(json, json_size);
  std::free(json);
  return true;
}

#if defined(_WIN32)

static bool WideToUtf8(const wchar_t* source, std::string* target) {
  if (source == nullptr || target == nullptr) {
    return false;
  }
  const auto required = WideCharToMultiByte(
      CP_UTF8, 0, source, -1, nullptr, 0, nullptr, nullptr);
  if (required <= 0) {
    return false;
  }
  std::string converted;
  converted.resize(static_cast<size_t>(required));
  if (required > 1 &&
      WideCharToMultiByte(CP_UTF8, 0, source, -1, converted.data(), required,
                          nullptr, nullptr) <= 0) {
    return false;
  }
  converted.resize(static_cast<size_t>(required - 1));
  *target = std::move(converted);
  return true;
}

static std::vector<std::pair<std::string, std::string>> GetEnvironmentEntries() {
  std::vector<std::pair<std::string, std::string>> entries;
  auto* block = GetEnvironmentStringsW();
  if (block == nullptr) {
    return entries;
  }
  for (auto* entry = block; *entry != L'\0'; entry += std::wcslen(entry) + 1) {
    const auto* separator = std::wcschr(entry, L'=');
    if (separator == nullptr || separator == entry) {
      continue;
    }
    std::wstring key_wide(entry, separator - entry);
    std::wstring value_wide(separator + 1);
    std::string key;
    std::string value;
    if (WideToUtf8(key_wide.c_str(), &key) &&
        WideToUtf8(value_wide.c_str(), &value)) {
      entries.emplace_back(std::move(key), std::move(value));
    }
  }
  FreeEnvironmentStringsW(block);
  return entries;
}

#else

static std::vector<std::pair<std::string, std::string>> GetEnvironmentEntries() {
  std::vector<std::pair<std::string, std::string>> entries;
  if (environ == nullptr) {
    return entries;
  }
  for (auto index = size_t{0}; environ[index] != nullptr; ++index) {
    const auto* entry = environ[index];
    const auto* separator = std::strchr(entry, '=');
    if (separator == nullptr) {
      entries.emplace_back(entry, "");
      continue;
    }
    entries.emplace_back(std::string(entry, separator - entry), separator + 1);
  }
  return entries;
}

#endif

extern "C" void muon_builtin_environments_get_variables(
    muon_completion_func completion) {
  CompleteString(completion, CreateJsonStringObject(GetEnvironmentEntries()));
}

extern "C" void muon_builtin_environments_get_command_line(
    muon_completion_func completion) {
  CompleteString(completion,
                 CreateJsonStringArray(GetMuonStartupCommandLine()));
}

extern "C" void muon_builtin_environments_get_process_id(
    muon_completion_func completion) {
#if defined(_WIN32)
  const auto result = static_cast<uint32_t>(GetCurrentProcessId());
#else
  const auto result = static_cast<uint32_t>(getpid());
#endif
  completion(&result, nullptr);
}

extern "C" void muon_builtin_environments_get_runtime_info(
    muon_completion_func completion) {
  std::string runtime_info;
  std::string error_message;
  if (!CreateRuntimeInfoJson(&runtime_info, &error_message)) {
    CompleteError(completion, error_message);
    return;
  }
  CompleteString(completion, runtime_info);
}

extern "C" void muon_builtin_environments_get_autostart(
    muon_completion_func completion) {
  MuonAutostartOptions options;
  std::string error_message;
  if (!CreateDefaultMuonAutostartOptions(&options, &error_message)) {
    CompleteError(completion, error_message);
    return;
  }

  MuonAutostartStatus status = kMuonAutostartStatusUnknown;
  if (!GetMuonAutostartStatus(options, &status, &error_message)) {
    CompleteError(completion, error_message);
    return;
  }
  switch (status) {
    case kMuonAutostartStatusDisabled:
      CompleteString(completion, "false");
      return;
    case kMuonAutostartStatusEnabled:
      CompleteString(completion, "true");
      return;
    case kMuonAutostartStatusUnknown:
      CompleteString(completion, "null");
      return;
  }
  CompleteString(completion, "null");
}

extern "C" void muon_builtin_environments_set_autostart(
    muon_completion_func completion,
    bool enabled) {
  MuonAutostartOptions options;
  std::string error_message;
  if (!CreateDefaultMuonAutostartOptions(&options, &error_message)) {
    CompleteError(completion, error_message);
    return;
  }
  if (!SetMuonAutostart(options, enabled, &error_message)) {
    CompleteError(completion, error_message);
    return;
  }
  CompleteVoid(completion);
}

static const muon_plugin_function_metadata get_variables_function = {
    "__getVariables",
    reinterpret_cast<muon_native_function>(
        &muon_builtin_environments_get_variables),
    {0, nullptr, &type_string},
    "getVariables",
};

static const muon_plugin_function_metadata get_command_line_function = {
    "__getCommandLine",
    reinterpret_cast<muon_native_function>(
        &muon_builtin_environments_get_command_line),
    {0, nullptr, &type_string},
    "getCommandLine",
};

static const muon_plugin_function_metadata get_process_id_function = {
    "__getProcessId",
    reinterpret_cast<muon_native_function>(
        &muon_builtin_environments_get_process_id),
    {0, nullptr, &type_u32},
    "getProcessId",
};

static const muon_plugin_function_metadata get_runtime_info_function = {
    "__getRuntimeInfo",
    reinterpret_cast<muon_native_function>(
        &muon_builtin_environments_get_runtime_info),
    {0, nullptr, &type_string},
    "getRuntimeInfo",
};

static const muon_plugin_function_metadata get_autostart_function = {
    "__getAutostart",
    reinterpret_cast<muon_native_function>(
        &muon_builtin_environments_get_autostart),
    {0, nullptr, &type_string},
    "getAutostart",
};

static const muon_plugin_function_metadata set_autostart_function = {
    "__setAutostart",
    reinterpret_cast<muon_native_function>(
        &muon_builtin_environments_set_autostart),
    {1, set_autostart_args, &type_void},
    "setAutostart",
};

static const muon_plugin_function_metadata* const environment_functions[] = {
    &get_variables_function,
    &get_command_line_function,
    &get_process_id_function,
    &get_runtime_info_function,
    &get_autostart_function,
    &set_autostart_function,
    nullptr,
};

static constexpr char environment_setup_script[] = R"JS(
const parseNativeJson = async (source) => JSON.parse(await source);
const properties = {};
if (isAllowed("getVariables")) {
  properties.getVariables = {
    enumerable: true,
    configurable: false,
    writable: false,
    value: async () => parseNativeJson(namespace.__getVariables()),
  };
}
if (isAllowed("getCommandLine")) {
  properties.getCommandLine = {
    enumerable: true,
    configurable: false,
    writable: false,
    value: async () => parseNativeJson(namespace.__getCommandLine()),
  };
}
if (isAllowed("getProcessId")) {
  properties.getProcessId = {
    enumerable: true,
    configurable: false,
    writable: false,
    value: async () => namespace.__getProcessId(),
  };
}
if (isAllowed("getRuntimeInfo")) {
  properties.getRuntimeInfo = {
    enumerable: true,
    configurable: false,
    writable: false,
    value: async () => parseNativeJson(namespace.__getRuntimeInfo()),
  };
}
if (isAllowed("getAutostart")) {
  properties.getAutostart = {
    enumerable: true,
    configurable: false,
    writable: false,
    value: async () => {
      const value = await parseNativeJson(namespace.__getAutostart());
      return value === null ? undefined : value;
    },
  };
}
if (isAllowed("setAutostart")) {
  properties.setAutostart = {
    enumerable: true,
    configurable: false,
    writable: false,
    value: async (enabled) => namespace.__setAutostart(enabled),
  };
}
Object.defineProperties(namespace, properties);
)JS";

}  // namespace muon_internal

const muon_plugin_namespace kMuonBuiltinEnvironmentsNamespace = {
    "muon.environments",
    muon_internal::environment_setup_script,
    muon_internal::environment_functions,
};

static const muon_plugin_namespace* const environment_namespaces[] = {
    &kMuonBuiltinEnvironmentsNamespace,
    nullptr,
};

static const muon_plugin_metadata environment_metadata = {
    environment_namespaces,
};

const muon_plugin_metadata* GetMuonBuiltinEnvironmentsPluginMetadata() {
  return &environment_metadata;
}
