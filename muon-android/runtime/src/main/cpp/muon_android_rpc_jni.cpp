/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include <jni.h>

#include "muon_android_process_runtime.h"
#include "plugins/muon_plugin_policy.h"
#include "rpc/muon_rpc_host.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

struct MuonAndroidResolvedRoute {
  uint32_t function_id = 0;
  MuonRpcRouteKind kind = MuonRpcRouteKind::Plugin;
};

struct MuonAndroidRpcHost {
  JavaVM* virtual_machine = nullptr;
  jobject bridge = nullptr;
  jmethodID send_text_result = nullptr;
  jmethodID send_binary_result = nullptr;
  jmethodID schedule_delay = nullptr;
  jmethodID cancel_delay = nullptr;
  jmethodID cancel_all_delays = nullptr;
  jmethodID invoke_platform_function = nullptr;
  jmethodID cancel_platform_call = nullptr;
  jmethodID cancel_all_platform_calls = nullptr;
  jmethodID deliver_runtime_probe = nullptr;
  jmethodID settle_runtime_probe = nullptr;
  jmethodID native_host_ready = nullptr;
  jmethodID native_host_startup_failed = nullptr;
  jclass native_argument_class = nullptr;
  jfieldID native_argument_kind = nullptr;
  jfieldID native_argument_boolean_value = nullptr;
  jfieldID native_argument_number_value = nullptr;
  jfieldID native_argument_string_value = nullptr;
  jfieldID native_argument_attachment = nullptr;
  jfieldID native_argument_function_kind = nullptr;
  jfieldID native_argument_renderer_context_id = nullptr;
  jfieldID native_argument_function_id = nullptr;
  jfieldID native_argument_proxy_id = nullptr;
  jfieldID native_argument_lease_token = nullptr;
  MuonRpcOwner owner;
  std::shared_ptr<MuonRpcHost> host;
  std::map<std::string, MuonAndroidResolvedRoute> routes_by_path;
  std::string renderer_metadata_json;
  bool startup_failed = false;
  std::map<uint32_t, MuonRpcHostCompletion> delayed_completions;
  std::map<uint32_t, MuonRpcHostCompletion> platform_completions;
};

// The controller contains no live Android resources after the last session
// stops. Android may unload the library from a non-Looper thread, so its small
// process-lifetime allocation is intentionally reclaimed by process teardown.
static MuonAndroidProcessRuntimeController* process_runtime = nullptr;
static JavaVM* process_virtual_machine = nullptr;
static jclass process_bridge_class = nullptr;
static jmethodID schedule_runtime_stop_completion = nullptr;
#if defined(MUON_TEST_BUILD)
static std::string pending_runtime_startup_fault;
#endif

static constexpr size_t kBinaryHeaderLength = 16;
static constexpr jint kPlatformResultVoid = 0;
static constexpr jint kPlatformResultString = 1;
static constexpr jint kPlatformResultUnsignedInteger = 2;
static constexpr jint kPlatformResultBoolean = 3;
static constexpr jint kPlatformResultBinary = 4;
static constexpr jint kNativeArgumentNull = 0;
static constexpr jint kNativeArgumentBoolean = 1;
static constexpr jint kNativeArgumentNumber = 2;
static constexpr jint kNativeArgumentString = 3;
static constexpr jint kNativeArgumentBinary = 4;
static constexpr jint kNativeArgumentFunction = 5;
static constexpr jint kNativeFunctionRendererSource = 1;
static constexpr jint kNativeFunctionPluginProxy = 2;
static constexpr jint kNativeCallPlugin = 0;
static constexpr jint kNativeCallPluginProxy = 1;
static constexpr uint8_t kBinaryFrameResult = 2;
static constexpr uint8_t kBinaryFrameRendererArgument = 3;

static const std::vector<std::string> kFilesystemFunctionPaths = {
    "muon.fs.readFile",       "muon.fs.writeFile",
    "muon.fs.readTextFile",   "muon.fs.writeTextFile",
    "muon.fs.stat",           "muon.fs.lstat",
    "muon.fs.exists",         "muon.fs.access",
    "muon.fs.readdir",        "muon.fs.mkdir",
    "muon.fs.rm",             "muon.fs.unlink",
    "muon.fs.rmdir",          "muon.fs.rename",
    "muon.fs.copyFile",       "muon.fs.appendFile",
    "muon.fs.appendTextFile", "muon.fs.truncate",
    "muon.fs.realpath",       "muon.fs.readlink",
    "muon.fs.symlink",        "muon.fs.watch",
};

static const std::vector<std::string> kPlatformFunctionPaths = {
    "muon.environments.getConfigValues",
    "prototype.fail",
    "prototype.delay",
    "prototype.echoBinary",
    "muon.environments.getVariables",
    "muon.environments.getProcessId",
    "muon.environments.getRuntimeInfo",
    "muon.browser.reload",
    "muon.browser.toggleFullscreen",
    "muon.browser.enterFullscreen",
    "muon.browser.exitFullscreen",
    "muon.browser.zoomIn",
    "muon.browser.zoomOut",
    "muon.browser.resetZoom",
    "muon.browser.close",
};

static MuonAndroidRpcHost* GetAndroidRpcHost(jlong handle) {
  return reinterpret_cast<MuonAndroidRpcHost*>(
      static_cast<intptr_t>(handle));
}

static jlong GetAndroidRpcHandle(MuonAndroidRpcHost* host) {
  return static_cast<jlong>(reinterpret_cast<intptr_t>(host));
}

static JNIEnv* GetAndroidEnvironment(MuonAndroidRpcHost* state) {
  if (state == nullptr || state->virtual_machine == nullptr) {
    return nullptr;
  }
  auto* environment = static_cast<JNIEnv*>(nullptr);
  if (state->virtual_machine->GetEnv(
          reinterpret_cast<void**>(&environment), JNI_VERSION_1_6) != JNI_OK) {
    return nullptr;
  }
  return environment;
}

static bool ScheduleRuntimeStopCompletion() {
  if (process_virtual_machine == nullptr || process_bridge_class == nullptr ||
      schedule_runtime_stop_completion == nullptr) {
    return false;
  }
  auto* environment = static_cast<JNIEnv*>(nullptr);
  if (process_virtual_machine->GetEnv(
          reinterpret_cast<void**>(&environment), JNI_VERSION_1_6) != JNI_OK) {
    return false;
  }
  environment->CallStaticVoidMethod(process_bridge_class,
                                    schedule_runtime_stop_completion);
  if (environment->ExceptionCheck()) {
    environment->ExceptionClear();
    return false;
  }
  return true;
}

static std::string GetJavaString(JNIEnv* environment, jstring value) {
  if (value == nullptr) {
    return {};
  }
  const auto* characters = environment->GetStringUTFChars(value, nullptr);
  if (characters == nullptr) {
    return {};
  }
  auto result = std::string(characters);
  environment->ReleaseStringUTFChars(value, characters);
  return result;
}

static void ThrowIllegalState(JNIEnv* environment,
                              const std::string& diagnostic) {
  const auto exception_class =
      environment->FindClass("java/lang/IllegalStateException");
  if (exception_class != nullptr) {
    environment->ThrowNew(exception_class, diagnostic.c_str());
    environment->DeleteLocalRef(exception_class);
  }
}

static void AppendJsonString(const std::string& value, std::string* output) {
  output->push_back('"');
  for (const auto character : value) {
    const auto byte = static_cast<unsigned char>(character);
    switch (character) {
      case '"':
        output->append("\\\"");
        break;
      case '\\':
        output->append("\\\\");
        break;
      case '\b':
        output->append("\\b");
        break;
      case '\f':
        output->append("\\f");
        break;
      case '\n':
        output->append("\\n");
        break;
      case '\r':
        output->append("\\r");
        break;
      case '\t':
        output->append("\\t");
        break;
      default:
        if (byte < 0x20) {
          static constexpr char kHexDigits[] = "0123456789abcdef";
          output->append("\\u00");
          output->push_back(kHexDigits[(byte >> 4) & 0x0f]);
          output->push_back(kHexDigits[byte & 0x0f]);
        } else {
          output->push_back(character);
        }
        break;
    }
  }
  output->push_back('"');
}

static std::string CreateProcessDiagnosticsJson(
    const MuonAndroidProcessRuntimeDiagnostics& diagnostics) {
  auto json = std::string{"{\"runtimeState\":"};
  AppendJsonString(diagnostics.runtime_state, &json);
  json.append(",\"generation\":");
  json.append(std::to_string(diagnostics.generation));
  json.append(",\"createdDispatcherHosts\":");
  json.append(std::to_string(diagnostics.created_dispatcher_hosts));
  json.append(",\"destroyedDispatcherHosts\":");
  json.append(std::to_string(diagnostics.destroyed_dispatcher_hosts));
  json.append(",\"liveDispatcherHosts\":");
  json.append(std::to_string(diagnostics.live_dispatcher_hosts));
  json.append(",\"activeSessions\":");
  json.append(std::to_string(diagnostics.active_sessions));
  json.append(",\"runtimeFileDescriptors\":");
  json.append(std::to_string(diagnostics.runtime_file_descriptors));
  json.append(",\"leakedDispatcherFileDescriptors\":");
  json.append(
      std::to_string(diagnostics.leaked_dispatcher_file_descriptors));
  json.append(",\"outstandingProbes\":");
  json.append(std::to_string(diagnostics.outstanding_probes));
  json.append(",\"suppressedProbeResults\":");
  json.append(std::to_string(diagnostics.suppressed_probe_results));
  json.append(",\"openedLibraryHandles\":");
  json.append(std::to_string(diagnostics.opened_library_handles));
  json.append(",\"closedLibraryHandles\":");
  json.append(std::to_string(diagnostics.closed_library_handles));
  json.append(",\"liveLibraryHandles\":");
  json.append(std::to_string(diagnostics.live_library_handles));
  json.append(",\"deferredLibraryHandles\":");
  json.append(std::to_string(diagnostics.deferred_library_handles));
  json.append(",\"lastClosedLibraries\":[");
  for (auto index = size_t{0};
       index < diagnostics.last_closed_libraries.size(); ++index) {
    if (index != 0) {
      json.push_back(',');
    }
    AppendJsonString(diagnostics.last_closed_libraries[index], &json);
  }
  json.append("],\"functionOwnerSources\":");
  json.append(std::to_string(diagnostics.function_owner_sources));
  json.append(",\"functionGlobalSources\":");
  json.append(std::to_string(diagnostics.function_global_sources));
  json.append(",\"functionGlobalBorrows\":");
  json.append(std::to_string(diagnostics.function_global_borrows));
  json.append(",\"functionGlobalProxies\":");
  json.append(std::to_string(diagnostics.function_global_proxies));
  json.append(",\"functionGlobalProxyLeases\":");
  json.append(std::to_string(diagnostics.function_global_proxy_leases));
  json.append(",\"pendingRendererFunctionCalls\":");
  json.append(std::to_string(diagnostics.pending_renderer_function_calls));
  json.append(",\"trafficTasksPending\":");
  json.append(diagnostics.traffic_tasks_pending ? "true" : "false");
  json.append(",\"ffiClosuresEnabled\":");
  json.append(diagnostics.ffi_closures_enabled ? "true" : "false");
  json.append(",\"ffiClosureAlloc\":");
  json.append(std::to_string(diagnostics.ffi_closure_alloc));
  json.append(",\"ffiClosureFree\":");
  json.append(std::to_string(diagnostics.ffi_closure_free));
  json.append(",\"ffiClosureLive\":");
  json.append(std::to_string(diagnostics.ffi_closure_live));
  json.append(",\"ffiClosureHighWater\":");
  json.append(std::to_string(diagnostics.ffi_closure_high_water));
  json.append(",\"closureExecutable\":");
  json.append(diagnostics.closure_executable ? "true" : "false");
  json.append(",\"closureWritable\":");
  json.append(diagnostics.closure_writable ? "true" : "false");
  json.append(",\"closureMappingPermissions\":");
  AppendJsonString(diagnostics.closure_mapping_permissions, &json);
  json.append(",\"ownerThread\":");
  json.append(diagnostics.owner_thread ? "true" : "false");
  json.push_back('}');
  return json;
}

template <typename Number>
static void AppendJsonFloatingPoint(Number value, std::string* output) {
  auto buffer = std::array<char, 64>{};
  const auto encoded = std::to_chars(
      buffer.data(), buffer.data() + buffer.size(), value,
      std::chars_format::general, std::numeric_limits<Number>::max_digits10);
  if (encoded.ec == std::errc()) {
    output->append(buffer.data(), encoded.ptr);
  } else {
    output->append("0");
  }
}

static void AppendTypeMetadataJson(const MuonTypeMetadata& type,
                                   std::string* output) {
  output->append("{\"type\":");
  AppendJsonString(GetMuonValueTypeName(type.type), output);
  if (type.type == MUON_TYPE_FUNCTION) {
    output->append(",\"args\":[");
    for (auto index = size_t{0}; index < type.function_arg_types.size();
         ++index) {
      if (index != 0) {
        output->push_back(',');
      }
      AppendTypeMetadataJson(type.function_arg_types[index], output);
    }
    output->append("],\"returnType\":");
    if (type.function_return_type.size() == 1) {
      AppendTypeMetadataJson(type.function_return_type[0], output);
    } else {
      output->append("null");
    }
  }
  output->push_back('}');
}

static bool AppendFunctionReferenceJson(
    const MuonRpcFunctionReference& function,
    std::string* output) {
  if (output == nullptr || function.type.type != MUON_TYPE_FUNCTION) {
    return false;
  }
  output->append("{\"type\":\"function\",\"kind\":");
  if (function.kind == MuonRpcFunctionKind::RendererSource) {
    if (function.renderer_context_id <= 0 || function.function_id <= 0) {
      return false;
    }
    output->append("\"renderer-source\",\"rendererContextId\":");
    output->append(std::to_string(function.renderer_context_id));
    output->append(",\"functionId\":");
    output->append(std::to_string(function.function_id));
  } else {
    if (function.proxy_id == 0 || function.lease_token.empty()) {
      return false;
    }
    output->append("\"plugin-proxy\",\"proxyId\":");
    output->append(std::to_string(function.proxy_id));
    output->append(",\"leaseToken\":");
    AppendJsonString(function.lease_token, output);
  }
  output->push_back('}');
  return true;
}

static std::string CreateRendererMetadataJson(
    const MuonAndroidPluginCatalog& catalog,
    int context_id) {
  auto output = std::string{"{\"version\":1,\"contextId\":"};
  output.append(std::to_string(context_id));
  output.append(",\"mode\":\"simple\",\"namespaces\":[");
  for (auto namespace_index = size_t{0};
       namespace_index < catalog.namespaces.size(); ++namespace_index) {
    if (namespace_index != 0) {
      output.push_back(',');
    }
    const auto& plugin_namespace = catalog.namespaces[namespace_index];
    output.append("{\"namespace\":");
    AppendJsonString(plugin_namespace.plugin_namespace, &output);
    output.append(",\"setupScript\":");
    AppendJsonString(plugin_namespace.setup_script, &output);
    output.append(",\"allowedFunctions\":[");
    for (auto function_index = size_t{0};
         function_index < plugin_namespace.allowed_function_names.size();
         ++function_index) {
      if (function_index != 0) {
        output.push_back(',');
      }
      AppendJsonString(
          plugin_namespace.allowed_function_names[function_index], &output);
    }
    output.append("]}");
  }
  output.append("],\"functions\":[");
  for (auto function_index = size_t{0};
       function_index < catalog.functions.size(); ++function_index) {
    if (function_index != 0) {
      output.push_back(',');
    }
    const auto& function = catalog.functions[function_index];
    const auto path = CreateMuonFunctionPublicPath(function);
    output.append("{\"id\":");
    output.append(std::to_string(function.id));
    output.append(",\"namespace\":");
    AppendJsonString(function.plugin_namespace, &output);
    output.append(",\"name\":");
    AppendJsonString(function.js_name, &output);
    output.append(",\"publicName\":");
    AppendJsonString(function.public_name.empty() ? function.js_name
                                                   : function.public_name,
                     &output);
    output.append(",\"capabilityId\":");
    const auto capability =
        catalog.capability_ids_by_function_path.find(path);
    AppendJsonString(
        capability == catalog.capability_ids_by_function_path.end()
            ? std::string{}
            : capability->second,
        &output);
    output.append(",\"args\":[");
    for (auto argument_index = size_t{0};
         argument_index < function.arg_types.size(); ++argument_index) {
      if (argument_index != 0) {
        output.push_back(',');
      }
      AppendTypeMetadataJson(function.arg_types[argument_index], &output);
    }
    output.append("],\"returnType\":");
    AppendTypeMetadataJson(function.return_type, &output);
    output.push_back('}');
  }
  output.append("]}");
  return output;
}

static void SendTextResult(MuonAndroidRpcHost* state,
                           const MuonRpcCallResult& result) {
  auto* environment = GetAndroidEnvironment(state);
  if (environment == nullptr || state->bridge == nullptr) {
    return;
  }
  auto message = std::string{"{\"version\":1,\"type\":\"result\",\"callId\":"};
  message.append(std::to_string(result.call_id));
  auto success = result.success;
  auto error_message = result.error_message;
  if (success &&
      ((result.value.type.type == MUON_TYPE_F32 &&
        !std::isfinite(result.value.f32_value)) ||
       (result.value.type.type == MUON_TYPE_F64 &&
        !std::isfinite(result.value.f64_value)))) {
    success = false;
    error_message = "Cannot encode a non-finite plugin result";
  }
  if (success &&
      result.value.type.type == MUON_TYPE_BUFFER_VIEW) {
    success = false;
    error_message = "Unsupported Android plugin result type";
  }
  if (success && result.value.type.type == MUON_TYPE_FUNCTION &&
      !result.value.is_null &&
      (result.value.function.kind == MuonRpcFunctionKind::RendererSource
           ? result.value.function.renderer_context_id <= 0 ||
                 result.value.function.function_id <= 0
           : result.value.function.proxy_id == 0 ||
                 result.value.function.lease_token.empty())) {
    success = false;
    error_message = "Invalid Android plugin function result";
  }
  if (!success) {
    message.append(",\"success\":false,\"error\":");
    AppendJsonString(error_message, &message);
    message.push_back('}');
  } else {
    message.append(",\"success\":true,\"valueType\":");
    AppendJsonString(GetMuonValueTypeName(result.value.type.type), &message);
    message.append(",\"value\":");
    switch (result.value.type.type) {
      case MUON_TYPE_VOID:
        message.append("null");
        break;
      case MUON_TYPE_BOOL:
        message.append(result.value.bool_value ? "true" : "false");
        break;
      case MUON_TYPE_I8:
        message.append(std::to_string(result.value.i8_value));
        break;
      case MUON_TYPE_U8:
        message.append(std::to_string(result.value.u8_value));
        break;
      case MUON_TYPE_I16:
        message.append(std::to_string(result.value.i16_value));
        break;
      case MUON_TYPE_U16:
        message.append(std::to_string(result.value.u16_value));
        break;
      case MUON_TYPE_I32:
        message.append(std::to_string(result.value.i32_value));
        break;
      case MUON_TYPE_U32:
        message.append(std::to_string(result.value.u32_value));
        break;
      case MUON_TYPE_I64:
        AppendJsonString(std::to_string(result.value.i64_value), &message);
        break;
      case MUON_TYPE_U64:
        AppendJsonString(std::to_string(result.value.u64_value), &message);
        break;
      case MUON_TYPE_F32:
        AppendJsonFloatingPoint(result.value.f32_value, &message);
        break;
      case MUON_TYPE_F64:
        AppendJsonFloatingPoint(result.value.f64_value, &message);
        break;
      case MUON_TYPE_POINTER:
        message.append(std::to_string(result.value.pointer_value));
        break;
      case MUON_TYPE_STRING:
        if (result.value.is_null) {
          message.append("null");
        } else {
          AppendJsonString(result.value.string_value, &message);
        }
        break;
      case MUON_TYPE_FUNCTION:
        if (result.value.is_null) {
          message.append("null");
        } else if (!AppendFunctionReferenceJson(
                       result.value.function, &message)) {
          message.append("null");
        }
        break;
      default:
        message.append("null");
        break;
    }
    message.push_back('}');
  }

  const auto java_message = environment->NewStringUTF(message.c_str());
  if (java_message == nullptr) {
    return;
  }
  environment->CallVoidMethod(state->bridge, state->send_text_result,
                              java_message);
  environment->DeleteLocalRef(java_message);
}

static void WriteBigEndianUint32(uint32_t value,
                                 size_t offset,
                                 std::vector<uint8_t>* bytes) {
  (*bytes)[offset] = static_cast<uint8_t>((value >> 24) & 0xff);
  (*bytes)[offset + 1] = static_cast<uint8_t>((value >> 16) & 0xff);
  (*bytes)[offset + 2] = static_cast<uint8_t>((value >> 8) & 0xff);
  (*bytes)[offset + 3] = static_cast<uint8_t>(value & 0xff);
}

static bool SendJavaTextMessage(MuonAndroidRpcHost* state,
                                const std::string& message,
                                std::string* error_message) {
  auto* environment = GetAndroidEnvironment(state);
  if (environment == nullptr || state == nullptr || state->bridge == nullptr ||
      state->send_text_result == nullptr) {
    if (error_message != nullptr) {
      *error_message = "Android WebView text transport is unavailable";
    }
    return false;
  }
  const auto java_message = environment->NewStringUTF(message.c_str());
  if (java_message == nullptr) {
    if (environment->ExceptionCheck()) {
      environment->ExceptionClear();
    }
    if (error_message != nullptr) {
      *error_message = "Could not encode an Android WebView message";
    }
    return false;
  }
  environment->CallVoidMethod(state->bridge, state->send_text_result,
                              java_message);
  environment->DeleteLocalRef(java_message);
  if (environment->ExceptionCheck()) {
    environment->ExceptionClear();
    if (error_message != nullptr) {
      *error_message = "Could not send an Android WebView message";
    }
    return false;
  }
  return true;
}

static bool SendJavaBinaryMessage(MuonAndroidRpcHost* state,
                                  uint8_t frame_kind,
                                  uint32_t call_id,
                                  uint32_t attachment,
                                  const MuonRpcBinary& binary,
                                  std::string* error_message) {
  auto* environment = GetAndroidEnvironment(state);
  if (environment == nullptr || state == nullptr || state->bridge == nullptr ||
      state->send_binary_result == nullptr || !IsValidMuonRpcBinary(binary) ||
      binary.size >
          static_cast<size_t>(std::numeric_limits<jsize>::max()) -
              kBinaryHeaderLength) {
    if (error_message != nullptr) {
      *error_message = "Android WebView binary transport is unavailable";
    }
    return false;
  }
  auto frame = std::vector<uint8_t>(kBinaryHeaderLength + binary.size);
  frame[0] = 'M';
  frame[1] = 'R';
  frame[2] = 'P';
  frame[3] = 'C';
  frame[4] = 1;
  frame[5] = frame_kind;
  WriteBigEndianUint32(call_id, 8, &frame);
  WriteBigEndianUint32(attachment, 12, &frame);
  if (binary.size != 0) {
    std::memcpy(frame.data() + kBinaryHeaderLength,
                GetMuonRpcBinaryData(binary), binary.size);
  }
  const auto java_frame =
      environment->NewByteArray(static_cast<jsize>(frame.size()));
  if (java_frame == nullptr) {
    if (environment->ExceptionCheck()) {
      environment->ExceptionClear();
    }
    if (error_message != nullptr) {
      *error_message = "Could not encode an Android WebView binary message";
    }
    return false;
  }
  environment->SetByteArrayRegion(
      java_frame, 0, static_cast<jsize>(frame.size()),
      reinterpret_cast<const jbyte*>(frame.data()));
  environment->CallVoidMethod(state->bridge, state->send_binary_result,
                              java_frame);
  environment->DeleteLocalRef(java_frame);
  if (environment->ExceptionCheck()) {
    environment->ExceptionClear();
    if (error_message != nullptr) {
      *error_message = "Could not send an Android WebView binary message";
    }
    return false;
  }
  return true;
}

static void SendBinaryResult(MuonAndroidRpcHost* state,
                             const MuonRpcCallResult& result) {
  auto error_message = std::string{};
  (void)SendJavaBinaryMessage(
      state, kBinaryFrameResult, result.call_id, 0, result.value.binary,
      &error_message);
}

static void SendResult(MuonAndroidRpcHost* state,
                       const MuonRpcCallResult& result) {
  if (result.success && result.value.type.type == MUON_TYPE_BUFFER_VIEW) {
    SendBinaryResult(state, result);
  } else {
    SendTextResult(state, result);
  }
}

static bool AppendRpcValueJson(
    const MuonRpcValue& value,
    std::vector<MuonRpcBinary>* binary_attachments,
    std::string* output,
                               std::string* error_message) {
  if (binary_attachments == nullptr || output == nullptr ||
      error_message == nullptr) {
    return false;
  }
  switch (value.type.type) {
    case MUON_TYPE_VOID:
      output->append("null");
      return true;
    case MUON_TYPE_BOOL:
      output->append(value.bool_value ? "true" : "false");
      return true;
    case MUON_TYPE_I8:
      output->append(std::to_string(value.i8_value));
      return true;
    case MUON_TYPE_U8:
      output->append(std::to_string(value.u8_value));
      return true;
    case MUON_TYPE_I16:
      output->append(std::to_string(value.i16_value));
      return true;
    case MUON_TYPE_U16:
      output->append(std::to_string(value.u16_value));
      return true;
    case MUON_TYPE_I32:
      output->append(std::to_string(value.i32_value));
      return true;
    case MUON_TYPE_U32:
      output->append(std::to_string(value.u32_value));
      return true;
    case MUON_TYPE_I64:
      AppendJsonString(std::to_string(value.i64_value), output);
      return true;
    case MUON_TYPE_U64:
      AppendJsonString(std::to_string(value.u64_value), output);
      return true;
    case MUON_TYPE_F32:
      if (!std::isfinite(value.f32_value)) {
        *error_message = "Cannot encode a non-finite renderer f32 argument";
        return false;
      }
      AppendJsonFloatingPoint(value.f32_value, output);
      return true;
    case MUON_TYPE_F64:
      if (!std::isfinite(value.f64_value)) {
        *error_message = "Cannot encode a non-finite renderer f64 argument";
        return false;
      }
      AppendJsonFloatingPoint(value.f64_value, output);
      return true;
    case MUON_TYPE_POINTER:
      output->append(std::to_string(value.pointer_value));
      return true;
    case MUON_TYPE_STRING:
      if (value.is_null) {
        output->append("null");
      } else {
        AppendJsonString(value.string_value, output);
      }
      return true;
    case MUON_TYPE_FUNCTION:
      if (value.is_null) {
        output->append("null");
        return true;
      }
      if (!AppendFunctionReferenceJson(value.function, output)) {
        *error_message = "Cannot encode an invalid renderer function argument";
        return false;
      }
      return true;
    case MUON_TYPE_BUFFER_VIEW: {
      if (!IsValidMuonRpcBinary(value.binary) ||
          binary_attachments->size() >
              static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
        *error_message = "Cannot encode an invalid renderer buffer argument";
        return false;
      }
      output->append("{\"type\":\"binary\",\"attachment\":");
      output->append(std::to_string(binary_attachments->size()));
      output->append(",\"byteLength\":");
      output->append(std::to_string(value.binary.size));
      output->push_back('}');
      binary_attachments->push_back(value.binary);
      return true;
    }
    default:
      *error_message = "Unsupported renderer function argument type";
      return false;
  }
}

static bool SendRuntimeMessage(MuonAndroidRpcHost* state,
                               const MuonRpcMessage& message,
                               std::string* error_message) {
  if (state == nullptr || error_message == nullptr) {
    return false;
  }
  error_message->clear();
  if (const auto* call =
          std::get_if<MuonRpcRendererFunctionCall>(&message)) {
    auto output = std::string{
        "{\"version\":1,\"type\":\"renderer-function-call\",\"callId\":"};
    output.append(std::to_string(call->call_id));
    output.append(",\"functionId\":");
    output.append(std::to_string(call->function_id));
    output.append(",\"expectsResult\":");
    output.append(call->expects_result ? "true" : "false");
    output.append(",\"functionType\":");
    AppendTypeMetadataJson(call->function_type, &output);
    output.append(",\"arguments\":[");
    auto binary_attachments = std::vector<MuonRpcBinary>{};
    for (auto index = size_t{0}; index < call->arguments.size(); ++index) {
      if (index != 0) {
        output.push_back(',');
      }
      if (!AppendRpcValueJson(call->arguments[index], &binary_attachments,
                              &output, error_message)) {
        return false;
      }
    }
    output.append("]}");
    if (!SendJavaTextMessage(state, output, error_message)) {
      return false;
    }
    for (auto index = size_t{0}; index < binary_attachments.size(); ++index) {
      if (!SendJavaBinaryMessage(
              state, kBinaryFrameRendererArgument, call->call_id,
              static_cast<uint32_t>(index), binary_attachments[index],
              error_message)) {
        return false;
      }
    }
    return true;
  }
  if (const auto* lease =
          std::get_if<MuonRpcRendererFunctionLease>(&message)) {
    auto output = std::string{
        "{\"version\":1,\"type\":\"renderer-function-lease\",\"functionId\":"};
    output.append(std::to_string(lease->function_id));
    output.append(",\"leaseToken\":");
    AppendJsonString(lease->lease_token, &output);
    output.append(",\"acquire\":");
    output.append(lease->acquire ? "true" : "false");
    output.push_back('}');
    return SendJavaTextMessage(state, output, error_message);
  }
  if (const auto* consumed =
          std::get_if<MuonRpcRendererFunctionResultConsumed>(&message)) {
    auto output = std::string{
        "{\"version\":1,\"type\":\"renderer-function-result-consumed\",\"callId\":"};
    output.append(std::to_string(consumed->call_id));
    output.push_back('}');
    return SendJavaTextMessage(state, output, error_message);
  }
  *error_message = "Unsupported Android runtime message";
  return false;
}

static void DeliverRuntimeProbe(MuonAndroidRpcHost* state, uint32_t mask) {
  auto* environment = GetAndroidEnvironment(state);
  if (environment == nullptr || state->bridge == nullptr ||
      state->deliver_runtime_probe == nullptr) {
    return;
  }
  environment->CallVoidMethod(state->bridge, state->deliver_runtime_probe,
                              static_cast<jint>(mask));
  if (environment->ExceptionCheck()) {
    environment->ExceptionClear();
  }
}

static void SettleRuntimeProbe(MuonAndroidRpcHost* state, bool delivered) {
  auto* environment = GetAndroidEnvironment(state);
  if (environment == nullptr || state->bridge == nullptr ||
      state->settle_runtime_probe == nullptr) {
    return;
  }
  environment->CallVoidMethod(
      state->bridge, state->settle_runtime_probe,
      delivered ? static_cast<jboolean>(JNI_TRUE)
                : static_cast<jboolean>(JNI_FALSE));
  if (environment->ExceptionCheck()) {
    environment->ExceptionClear();
  }
}

static void CompleteWithFailure(const MuonRpcCallRequest& request,
                                const std::string& diagnostic,
                                MuonRpcHostCompletion completion) {
  MuonRpcCallResult result;
  result.owner = request.owner;
  result.call_id = request.call_id;
  result.success = false;
  result.error_message = diagnostic;
  completion(result);
}

static void CompleteRetainedPlatformFailure(
    MuonAndroidRpcHost* state,
    const MuonRpcCallRequest& request,
    const std::string& diagnostic) {
  const auto iterator = state->platform_completions.find(request.call_id);
  if (iterator == state->platform_completions.end()) {
    return;
  }
  auto completion = std::move(iterator->second);
  state->platform_completions.erase(iterator);
  CompleteWithFailure(request, diagnostic, std::move(completion));
}

static jobjectArray CreateJavaBinaryArguments(
    MuonAndroidRpcHost* state,
    const MuonRpcCallRequest& request) {
  auto* environment = GetAndroidEnvironment(state);
  if (environment == nullptr || request.arguments.empty() ||
      request.arguments.size() - 1 >
          static_cast<size_t>(std::numeric_limits<jsize>::max())) {
    return nullptr;
  }
  const auto byte_array_class = environment->FindClass("[B");
  if (byte_array_class == nullptr) {
    return nullptr;
  }
  const auto result = environment->NewObjectArray(
      static_cast<jsize>(request.arguments.size() - 1), byte_array_class,
      nullptr);
  environment->DeleteLocalRef(byte_array_class);
  if (result == nullptr) {
    return nullptr;
  }

  for (auto index = size_t{1}; index < request.arguments.size(); ++index) {
    const auto& argument = request.arguments[index];
    if (argument.type.type != MUON_TYPE_BUFFER_VIEW ||
        !IsValidMuonRpcBinary(argument.binary) ||
        argument.binary.size >
            static_cast<size_t>(std::numeric_limits<jsize>::max())) {
      environment->DeleteLocalRef(result);
      return nullptr;
    }
    const auto bytes = environment->NewByteArray(
        static_cast<jsize>(argument.binary.size));
    if (bytes == nullptr) {
      environment->DeleteLocalRef(result);
      return nullptr;
    }
    if (argument.binary.size != 0) {
      environment->SetByteArrayRegion(
          bytes, 0, static_cast<jsize>(argument.binary.size),
          static_cast<const jbyte*>(GetMuonRpcBinaryData(argument.binary)));
    }
    environment->SetObjectArrayElement(
        result, static_cast<jsize>(index - 1), bytes);
    environment->DeleteLocalRef(bytes);
  }
  return result;
}

static void InvokePlatformFunction(MuonAndroidRpcHost* state,
                                   const MuonRpcCallRequest& request,
                                   MuonRpcHostCompletion completion) {
  auto* environment = GetAndroidEnvironment(state);
  if (request.capability.function_path == "prototype.fail") {
    CompleteWithFailure(request, "prototype failure", std::move(completion));
    return;
  }
  if (request.capability.function_path == "prototype.delay") {
    state->delayed_completions.emplace(request.call_id,
                                       std::move(completion));
    if (environment != nullptr) {
      environment->CallVoidMethod(
          state->bridge, state->schedule_delay,
          static_cast<jint>(request.call_id));
    }
    return;
  }
  if (request.capability.function_path == "prototype.echoBinary") {
    if (request.arguments.size() != 2 ||
        request.arguments[1].type.type != MUON_TYPE_BUFFER_VIEW ||
        !IsValidMuonRpcBinary(request.arguments[1].binary)) {
      CompleteWithFailure(request, "prototype binary argument is invalid",
                          std::move(completion));
      return;
    }
    MuonRpcCallResult result;
    result.owner = request.owner;
    result.call_id = request.call_id;
    result.success = true;
    result.value = request.arguments[1];
    completion(result);
    return;
  }

  if (environment == nullptr || state->bridge == nullptr ||
      request.arguments.empty() ||
      request.arguments[0].type.type != MUON_TYPE_STRING ||
      state->invoke_platform_function == nullptr) {
    CompleteWithFailure(request, "Android platform service is unavailable",
                        std::move(completion));
    return;
  }
  if (state->platform_completions.find(request.call_id) !=
      state->platform_completions.end()) {
    CompleteWithFailure(request, "Duplicate Android platform call",
                        std::move(completion));
    return;
  }
  state->platform_completions.emplace(request.call_id,
                                      std::move(completion));

  const auto function_path =
      environment->NewStringUTF(request.capability.function_path.c_str());
  const auto arguments_json = environment->NewStringUTF(
      request.arguments[0].string_value.c_str());
  const auto binary_arguments = CreateJavaBinaryArguments(state, request);
  if (function_path == nullptr || arguments_json == nullptr ||
      binary_arguments == nullptr) {
    if (function_path != nullptr) {
      environment->DeleteLocalRef(function_path);
    }
    if (arguments_json != nullptr) {
      environment->DeleteLocalRef(arguments_json);
    }
    if (binary_arguments != nullptr) {
      environment->DeleteLocalRef(binary_arguments);
    }
    if (environment->ExceptionCheck()) {
      environment->ExceptionClear();
    }
    CompleteRetainedPlatformFailure(
        state, request, "Could not create Android platform arguments");
    return;
  }

  environment->CallVoidMethod(
      state->bridge, state->invoke_platform_function,
      static_cast<jint>(request.call_id), function_path, arguments_json,
      binary_arguments);
  environment->DeleteLocalRef(function_path);
  environment->DeleteLocalRef(arguments_json);
  environment->DeleteLocalRef(binary_arguments);
  if (environment->ExceptionCheck()) {
    environment->ExceptionClear();
    CompleteRetainedPlatformFailure(
        state, request, "Android platform function invocation failed");
  }
}

static void CancelPlatformCall(MuonAndroidRpcHost* state,
                               const MuonRpcCallCancel& cancel) {
  auto* environment = GetAndroidEnvironment(state);
  const auto delayed = state->delayed_completions.erase(cancel.call_id) != 0;
  const auto platform = state->platform_completions.erase(cancel.call_id) != 0;
  if (environment != nullptr && delayed) {
    environment->CallVoidMethod(state->bridge, state->cancel_delay,
                                static_cast<jint>(cancel.call_id));
  }
  if (environment != nullptr && platform) {
    environment->CallVoidMethod(
        state->bridge, state->cancel_platform_call,
        static_cast<jint>(cancel.call_id));
  }
}

static void ReleasePlatformContext(MuonAndroidRpcHost* state) {
  auto* environment = GetAndroidEnvironment(state);
  state->delayed_completions.clear();
  state->platform_completions.clear();
  if (environment != nullptr) {
    environment->CallVoidMethod(state->bridge, state->cancel_all_delays);
    environment->CallVoidMethod(state->bridge,
                                state->cancel_all_platform_calls);
  }
}

static bool ResolveFunctionRoute(const MuonAndroidRpcHost* state,
                                 const std::string& function_path,
                                 MuonAndroidResolvedRoute* route) {
  if (state == nullptr || route == nullptr) {
    return false;
  }
  const auto iterator = state->routes_by_path.find(function_path);
  if (iterator == state->routes_by_path.end()) {
    return false;
  }
  *route = iterator->second;
  return true;
}

static bool ParseInt64(const std::string& source, int64_t* value) {
  if (value == nullptr || source.empty()) {
    return false;
  }
  const auto parsed = std::from_chars(
      source.data(), source.data() + source.size(), *value);
  return parsed.ec == std::errc() &&
         parsed.ptr == source.data() + source.size();
}

static bool ParseUInt64(const std::string& source, uint64_t* value) {
  if (value == nullptr || source.empty()) {
    return false;
  }
  const auto parsed = std::from_chars(
      source.data(), source.data() + source.size(), *value);
  return parsed.ec == std::errc() &&
         parsed.ptr == source.data() + source.size();
}

static bool DecodeBinaryArgument(JNIEnv* environment,
                                 jobjectArray binary_arguments,
                                 jint attachment,
                                 MuonRpcValue* value) {
  if (environment == nullptr || binary_arguments == nullptr ||
      value == nullptr || attachment < 0 ||
      attachment >= environment->GetArrayLength(binary_arguments)) {
    return false;
  }
  const auto binary_argument = static_cast<jbyteArray>(
      environment->GetObjectArrayElement(binary_arguments, attachment));
  if (binary_argument == nullptr) {
    return false;
  }
  const auto size = environment->GetArrayLength(binary_argument);
  auto storage = CreateMuonRpcOwnedBuffer(static_cast<size_t>(size));
  if (!storage || (size != 0 && storage->GetData() == nullptr)) {
    environment->DeleteLocalRef(binary_argument);
    return false;
  }
  if (size != 0) {
    environment->GetByteArrayRegion(
        binary_argument, 0, size, static_cast<jbyte*>(storage->GetData()));
  }
  environment->DeleteLocalRef(binary_argument);
  if (environment->ExceptionCheck()) {
    environment->ExceptionClear();
    return false;
  }
  value->binary.storage = std::move(storage);
  value->binary.size = static_cast<size_t>(size);
  return true;
}

static bool DecodePluginArguments(
    MuonAndroidRpcHost* state,
    JNIEnv* environment,
    MuonRpcCallRequest* request,
    jobjectArray native_arguments,
    jobjectArray binary_arguments,
    std::string* error_message,
    const std::vector<MuonTypeMetadata>* supplied_expected_types) {
  if (state == nullptr || environment == nullptr || request == nullptr ||
      native_arguments == nullptr || error_message == nullptr ||
      process_runtime == nullptr) {
    return false;
  }
  request->arguments.clear();
  error_message->clear();
  auto expected_types = std::vector<MuonTypeMetadata>{};
  if (supplied_expected_types == nullptr) {
    if (!process_runtime->GetCallArgumentTypes(
            *request, &expected_types, error_message)) {
      return false;
    }
  } else {
    expected_types = *supplied_expected_types;
  }
  if (environment->GetArrayLength(native_arguments) !=
      static_cast<jsize>(expected_types.size())) {
    *error_message = "Invalid argument count";
    return false;
  }

  request->arguments.resize(expected_types.size());
  for (auto index = size_t{0}; index < expected_types.size(); ++index) {
    const auto encoded = environment->GetObjectArrayElement(
        native_arguments, static_cast<jsize>(index));
    if (encoded == nullptr ||
        !environment->IsInstanceOf(encoded, state->native_argument_class)) {
      if (encoded != nullptr) {
        environment->DeleteLocalRef(encoded);
      }
      *error_message = "Invalid encoded plugin argument";
      request->arguments.clear();
      return false;
    }
    const auto kind = environment->GetIntField(
        encoded, state->native_argument_kind);
    const auto number = environment->GetDoubleField(
        encoded, state->native_argument_number_value);
    auto& value = request->arguments[index];
    value.type = expected_types[index];
    auto valid = true;
    switch (expected_types[index].type) {
      case MUON_TYPE_BOOL:
        valid = kind == kNativeArgumentBoolean;
        if (valid) {
          value.bool_value = environment->GetBooleanField(
                                 encoded,
                                 state->native_argument_boolean_value) ==
                             JNI_TRUE;
        }
        break;
      case MUON_TYPE_I8:
        valid = kind == kNativeArgumentNumber && std::isfinite(number) &&
                std::trunc(number) == number &&
                number >= std::numeric_limits<int8_t>::min() &&
                number <= std::numeric_limits<int8_t>::max();
        if (valid) {
          value.i8_value = static_cast<int8_t>(number);
        }
        break;
      case MUON_TYPE_U8:
        valid = kind == kNativeArgumentNumber && std::isfinite(number) &&
                std::trunc(number) == number && number >= 0.0 &&
                number <= std::numeric_limits<uint8_t>::max();
        if (valid) {
          value.u8_value = static_cast<uint8_t>(number);
        }
        break;
      case MUON_TYPE_I16:
        valid = kind == kNativeArgumentNumber && std::isfinite(number) &&
                std::trunc(number) == number &&
                number >= std::numeric_limits<int16_t>::min() &&
                number <= std::numeric_limits<int16_t>::max();
        if (valid) {
          value.i16_value = static_cast<int16_t>(number);
        }
        break;
      case MUON_TYPE_U16:
        valid = kind == kNativeArgumentNumber && std::isfinite(number) &&
                std::trunc(number) == number && number >= 0.0 &&
                number <= std::numeric_limits<uint16_t>::max();
        if (valid) {
          value.u16_value = static_cast<uint16_t>(number);
        }
        break;
      case MUON_TYPE_I32:
        valid = kind == kNativeArgumentNumber && std::isfinite(number) &&
                std::trunc(number) == number &&
                number >= std::numeric_limits<int32_t>::min() &&
                number <= std::numeric_limits<int32_t>::max();
        if (valid) {
          value.i32_value = static_cast<int32_t>(number);
        }
        break;
      case MUON_TYPE_U32:
        valid = kind == kNativeArgumentNumber && std::isfinite(number) &&
                std::trunc(number) == number && number >= 0.0 &&
                number <= std::numeric_limits<uint32_t>::max();
        if (valid) {
          value.u32_value = static_cast<uint32_t>(number);
        }
        break;
      case MUON_TYPE_I64: {
        const auto encoded_string = static_cast<jstring>(
            environment->GetObjectField(encoded,
                                        state->native_argument_string_value));
        const auto source = GetJavaString(environment, encoded_string);
        if (encoded_string != nullptr) {
          environment->DeleteLocalRef(encoded_string);
        }
        valid = kind == kNativeArgumentString &&
                ParseInt64(source, &value.i64_value);
        break;
      }
      case MUON_TYPE_U64: {
        const auto encoded_string = static_cast<jstring>(
            environment->GetObjectField(encoded,
                                        state->native_argument_string_value));
        const auto source = GetJavaString(environment, encoded_string);
        if (encoded_string != nullptr) {
          environment->DeleteLocalRef(encoded_string);
        }
        valid = kind == kNativeArgumentString &&
                ParseUInt64(source, &value.u64_value);
        break;
      }
      case MUON_TYPE_F32:
        valid = kind == kNativeArgumentNumber && std::isfinite(number) &&
                number >= -std::numeric_limits<float>::max() &&
                number <= std::numeric_limits<float>::max();
        if (valid) {
          value.f32_value = static_cast<float>(number);
        }
        break;
      case MUON_TYPE_F64:
        valid = kind == kNativeArgumentNumber && std::isfinite(number);
        if (valid) {
          value.f64_value = number;
        }
        break;
      case MUON_TYPE_POINTER:
        valid = kind == kNativeArgumentNull ||
                (kind == kNativeArgumentNumber && std::isfinite(number) &&
                 std::trunc(number) == number && number >= 0.0 &&
                 number < std::ldexp(
                              1.0, std::numeric_limits<uintptr_t>::digits));
        if (valid && kind == kNativeArgumentNumber) {
          value.pointer_value = static_cast<uintptr_t>(number);
        }
        break;
      case MUON_TYPE_STRING:
        if (kind == kNativeArgumentNull) {
          value.is_null = true;
          break;
        }
        if (kind == kNativeArgumentString) {
          const auto encoded_string = static_cast<jstring>(
              environment->GetObjectField(
                  encoded, state->native_argument_string_value));
          valid = encoded_string != nullptr;
          if (valid) {
            value.string_value = GetJavaString(environment, encoded_string);
            valid = value.string_value.find('\0') == std::string::npos;
          }
          if (encoded_string != nullptr) {
            environment->DeleteLocalRef(encoded_string);
          }
        } else {
          valid = false;
        }
        break;
      case MUON_TYPE_BUFFER_VIEW:
        valid = kind == kNativeArgumentBinary &&
                DecodeBinaryArgument(
                    environment, binary_arguments,
                    environment->GetIntField(
                        encoded, state->native_argument_attachment),
                    &value);
        break;
      case MUON_TYPE_FUNCTION:
        if (kind == kNativeArgumentNull) {
          value.is_null = true;
          break;
        }
        if (kind != kNativeArgumentFunction) {
          valid = false;
          break;
        }
        value.function.type = expected_types[index];
        switch (environment->GetIntField(
            encoded, state->native_argument_function_kind)) {
          case kNativeFunctionRendererSource:
            value.function.kind = MuonRpcFunctionKind::RendererSource;
            value.function.renderer_context_id = environment->GetIntField(
                encoded, state->native_argument_renderer_context_id);
            value.function.function_id = environment->GetIntField(
                encoded, state->native_argument_function_id);
            valid = value.function.renderer_context_id ==
                        request->owner.context_id &&
                    value.function.function_id > 0;
            break;
          case kNativeFunctionPluginProxy: {
            value.function.kind = MuonRpcFunctionKind::PluginProxy;
            const auto proxy_id = environment->GetIntField(
                encoded, state->native_argument_proxy_id);
            const auto lease_token = static_cast<jstring>(
                environment->GetObjectField(
                    encoded, state->native_argument_lease_token));
            value.function.proxy_id =
                proxy_id > 0 ? static_cast<uint32_t>(proxy_id) : 0;
            value.function.lease_token =
                GetJavaString(environment, lease_token);
            if (lease_token != nullptr) {
              environment->DeleteLocalRef(lease_token);
            }
            valid = value.function.proxy_id != 0 &&
                    !value.function.lease_token.empty();
            break;
          }
          default:
            valid = false;
            break;
        }
        break;
      case MUON_TYPE_VOID:
        valid = kind == kNativeArgumentNull;
        break;
      default:
        valid = false;
        break;
    }
    environment->DeleteLocalRef(encoded);
    if (!valid) {
      *error_message = "Invalid " +
                       std::string(GetMuonValueTypeName(
                           expected_types[index].type)) +
                       " argument";
      request->arguments.clear();
      return false;
    }
  }
  return true;
}

static bool InitializeHost(MuonAndroidRpcHost* state,
                           std::string* error_message) {
  if (state == nullptr || error_message == nullptr ||
      process_runtime == nullptr) {
    return false;
  }
  auto plugin_catalog = MuonAndroidPluginCatalog{};
  if (!process_runtime->GetPluginCatalog(&plugin_catalog, error_message)) {
    return false;
  }
  auto environment_policy = std::shared_ptr<MuonPluginPolicy>{};
  if (!CreateMuonPluginPolicy(
          {"muon.environments.getVariables",
           "muon.environments.getConfigValues",
           "muon.environments.getProcessId",
           "muon.environments.getRuntimeInfo"},
          &environment_policy, error_message)) {
    return false;
  }
  auto filesystem_policy = std::shared_ptr<MuonPluginPolicy>{};
  if (!CreateMuonPluginPolicy(kFilesystemFunctionPaths, &filesystem_policy,
                              error_message)) {
    return false;
  }
  auto browser_policy = std::shared_ptr<MuonPluginPolicy>{};
  if (!CreateMuonPluginPolicy(
          {"muon.browser.reload", "muon.browser.toggleFullscreen",
           "muon.browser.enterFullscreen", "muon.browser.exitFullscreen",
           "muon.browser.zoomIn", "muon.browser.zoomOut",
           "muon.browser.resetZoom", "muon.browser.close"},
          &browser_policy, error_message)) {
    return false;
  }
  auto prototype_policy = std::shared_ptr<MuonPluginPolicy>{};
  if (!CreateMuonPluginPolicy({"prototype.*"}, &prototype_policy,
                              error_message)) {
    return false;
  }

  state->routes_by_path.clear();
  auto routes = std::vector<MuonRpcFunctionRoute>{};
  routes.reserve(plugin_catalog.functions.size() +
                 kPlatformFunctionPaths.size() +
                 kFilesystemFunctionPaths.size());
  auto used_function_ids = std::set<uint32_t>{};
  auto maximum_plugin_function_id = uint32_t{0};
  for (const auto& function : plugin_catalog.functions) {
    const auto path = CreateMuonFunctionPublicPath(function);
    if (path.empty() || !used_function_ids.insert(function.id).second ||
        !state->routes_by_path
             .emplace(path, MuonAndroidResolvedRoute{
                                function.id, MuonRpcRouteKind::Plugin})
             .second) {
      *error_message = "Invalid Android plugin route: " + path;
      return false;
    }
    routes.push_back(
        {function.id, path, MuonRpcRouteKind::Plugin});
    maximum_plugin_function_id =
        std::max(maximum_plugin_function_id, function.id);
  }

  auto platform_paths = kPlatformFunctionPaths;
  platform_paths.insert(platform_paths.end(), kFilesystemFunctionPaths.begin(),
                        kFilesystemFunctionPaths.end());
  auto next_platform_function_id =
      static_cast<uint64_t>(maximum_plugin_function_id) + 1;
  for (const auto& path : platform_paths) {
    if (next_platform_function_id >
        std::numeric_limits<uint32_t>::max()) {
      *error_message = "Android RPC function id space is exhausted";
      return false;
    }
    const auto function_id =
        static_cast<uint32_t>(next_platform_function_id);
    next_platform_function_id += 1;
    if (!used_function_ids.insert(function_id).second ||
        !state->routes_by_path
             .emplace(path, MuonAndroidResolvedRoute{
                                function_id, MuonRpcRouteKind::Platform})
             .second) {
      *error_message = "Duplicate Android RPC function route: " + path;
      return false;
    }
    routes.push_back(
        {function_id, path, MuonRpcRouteKind::Platform});
  }

  auto policies = plugin_catalog.capability_policies;
  const auto add_policy = [&policies, error_message](
                              const std::string& id,
                              std::shared_ptr<MuonPluginPolicy> policy) {
    if (!policies.emplace(id, std::move(policy)).second) {
      *error_message = "Duplicate Android RPC capability id: " + id;
      return false;
    }
    return true;
  };
  if (!add_policy("environment-capability", environment_policy) ||
      !add_policy("browser-capability", browser_policy) ||
      !add_policy("fs-capability", filesystem_policy) ||
      !add_policy("prototype-capability", prototype_policy)) {
    return false;
  }
  MuonRpcHostServices services;
  services.invoke_plugin =
      [](const MuonRpcCallRequest& request, MuonRpcHostCompletion completion) {
        if (process_runtime == nullptr) {
          CompleteWithFailure(request,
                              "Android native plugin runtime is unavailable",
                              std::move(completion));
          return;
        }
        process_runtime->Invoke(request, std::move(completion));
      };
  services.invoke_platform =
      [state](const MuonRpcCallRequest& request,
              MuonRpcHostCompletion completion) {
        InvokePlatformFunction(state, request, std::move(completion));
      };
  services.cancel_call = [state](const MuonRpcCallCancel& cancel) {
    CancelPlatformCall(state, cancel);
  };
  services.release_plugin_proxy = [](const MuonRpcPluginProxyRelease& release) {
    if (process_runtime != nullptr) {
      process_runtime->ReleasePluginFunctionProxy(release);
    }
  };
  services.release_context = [state](const MuonRpcContextReleased& release) {
    ReleasePlatformContext(state);
    if (process_runtime != nullptr) {
      process_runtime->ReleaseSessionContext(release.owner);
    }
  };
  services.send_result = [state](const MuonRpcCallResult& result) {
    SendResult(state, result);
  };
  if (!CreateMuonRpcHost(MuonRpcHostMode::Validate, routes, policies,
                         std::move(services), &state->host, error_message)) {
    return false;
  }
  state->renderer_metadata_json =
      CreateRendererMetadataJson(plugin_catalog, state->owner.context_id);
  return true;
}

static void NotifyAndroidRpcHostStartupFailure(
    MuonAndroidRpcHost* state,
    const std::string& diagnostic) {
  if (state == nullptr || state->startup_failed || state->host ||
      state->bridge == nullptr ||
      state->native_host_startup_failed == nullptr) {
    return;
  }
  state->startup_failed = true;
  auto* environment = GetAndroidEnvironment(state);
  if (environment == nullptr) {
    return;
  }
  const auto message = diagnostic.empty()
                           ? "Could not start the Android native runtime"
                           : diagnostic;
  const auto java_message = environment->NewStringUTF(message.c_str());
  if (java_message == nullptr) {
    if (environment->ExceptionCheck()) {
      environment->ExceptionClear();
    }
    return;
  }
  environment->CallVoidMethod(
      state->bridge, state->native_host_startup_failed, java_message);
  environment->DeleteLocalRef(java_message);
  if (environment->ExceptionCheck()) {
    environment->ExceptionClear();
  }
}

static void CompleteWaitingAndroidRpcHostStartup(
    MuonAndroidRpcHost* state) {
  if (state == nullptr || state->startup_failed || state->host) {
    return;
  }
  auto error_message = std::string{};
  if (!InitializeHost(state, &error_message)) {
    NotifyAndroidRpcHostStartupFailure(state, error_message);
    return;
  }
  auto* environment = GetAndroidEnvironment(state);
  if (environment == nullptr || state->bridge == nullptr ||
      state->native_host_ready == nullptr) {
    return;
  }
  const auto metadata =
      environment->NewStringUTF(state->renderer_metadata_json.c_str());
  if (metadata == nullptr) {
    if (environment->ExceptionCheck()) {
      environment->ExceptionClear();
    }
    return;
  }
  environment->CallVoidMethod(state->bridge, state->native_host_ready,
                              metadata);
  environment->DeleteLocalRef(metadata);
  if (environment->ExceptionCheck()) {
    environment->ExceptionClear();
  }
}

/** Creates the CEF-independent host used by one WebView context. */
extern "C" JNIEXPORT jlong JNICALL
Java_dev_muon_runtime_MuonRpcBridge_nativeCreateHost(
    JNIEnv* environment,
    jclass bridge_type,
    jobject bridge) {
  if (bridge == nullptr) {
    ThrowIllegalState(environment, "The Android RPC bridge is required");
    return 0;
  }
  auto state = std::make_unique<MuonAndroidRpcHost>();
  if (environment->GetJavaVM(&state->virtual_machine) != JNI_OK) {
    ThrowIllegalState(environment, "Could not access the Android Java VM");
    return 0;
  }
  state->bridge = environment->NewGlobalRef(bridge);
  const auto bridge_class = environment->GetObjectClass(bridge);
  if (state->bridge == nullptr || bridge_class == nullptr) {
    if (state->bridge != nullptr) {
      environment->DeleteGlobalRef(state->bridge);
    }
    ThrowIllegalState(environment, "Could not retain the Android RPC bridge");
    return 0;
  }
  state->send_text_result = environment->GetMethodID(
      bridge_class, "onNativeTextResult", "(Ljava/lang/String;)V");
  state->send_binary_result = environment->GetMethodID(
      bridge_class, "onNativeBinaryResult", "([B)V");
  state->schedule_delay = environment->GetMethodID(
      bridge_class, "scheduleNativeDelay", "(I)V");
  state->cancel_delay = environment->GetMethodID(
      bridge_class, "cancelNativeDelay", "(I)V");
  state->cancel_all_delays = environment->GetMethodID(
      bridge_class, "cancelAllNativeDelays", "()V");
  state->invoke_platform_function = environment->GetMethodID(
      bridge_class, "invokePlatformFunction",
      "(ILjava/lang/String;Ljava/lang/String;[[B)V");
  state->cancel_platform_call = environment->GetMethodID(
      bridge_class, "cancelPlatformCall", "(I)V");
  state->cancel_all_platform_calls = environment->GetMethodID(
      bridge_class, "cancelAllPlatformCalls", "()V");
  state->deliver_runtime_probe = environment->GetMethodID(
      bridge_class, "onNativeRuntimeProbeResult", "(I)V");
  state->settle_runtime_probe = environment->GetMethodID(
      bridge_class, "onNativeRuntimeProbeSettled", "(Z)V");
  state->native_host_ready = environment->GetMethodID(
      bridge_class, "onNativeHostReady", "(Ljava/lang/String;)V");
  state->native_host_startup_failed = environment->GetMethodID(
      bridge_class, "onNativeHostStartupFailed", "(Ljava/lang/String;)V");
  const auto local_native_argument_class = environment->FindClass(
      "dev/muon/runtime/MuonRpcBridge$NativeArgument");
  if (local_native_argument_class != nullptr) {
    state->native_argument_class = static_cast<jclass>(
        environment->NewGlobalRef(local_native_argument_class));
    state->native_argument_kind = environment->GetFieldID(
        local_native_argument_class, "kind", "I");
    state->native_argument_boolean_value = environment->GetFieldID(
        local_native_argument_class, "booleanValue", "Z");
    state->native_argument_number_value = environment->GetFieldID(
        local_native_argument_class, "numberValue", "D");
    state->native_argument_string_value = environment->GetFieldID(
        local_native_argument_class, "stringValue", "Ljava/lang/String;");
    state->native_argument_attachment = environment->GetFieldID(
        local_native_argument_class, "attachment", "I");
    state->native_argument_function_kind = environment->GetFieldID(
        local_native_argument_class, "functionKind", "I");
    state->native_argument_renderer_context_id = environment->GetFieldID(
        local_native_argument_class, "rendererContextId", "I");
    state->native_argument_function_id = environment->GetFieldID(
        local_native_argument_class, "functionId", "I");
    state->native_argument_proxy_id = environment->GetFieldID(
        local_native_argument_class, "proxyId", "I");
    state->native_argument_lease_token = environment->GetFieldID(
        local_native_argument_class, "leaseToken", "Ljava/lang/String;");
    environment->DeleteLocalRef(local_native_argument_class);
  }
  environment->DeleteLocalRef(bridge_class);
  if (state->send_text_result == nullptr ||
      state->send_binary_result == nullptr || state->schedule_delay == nullptr ||
      state->cancel_delay == nullptr || state->cancel_all_delays == nullptr ||
      state->invoke_platform_function == nullptr ||
      state->cancel_platform_call == nullptr ||
      state->cancel_all_platform_calls == nullptr ||
      state->deliver_runtime_probe == nullptr ||
      state->settle_runtime_probe == nullptr ||
      state->native_host_ready == nullptr ||
      state->native_host_startup_failed == nullptr ||
      state->native_argument_class == nullptr ||
      state->native_argument_kind == nullptr ||
      state->native_argument_boolean_value == nullptr ||
      state->native_argument_number_value == nullptr ||
      state->native_argument_string_value == nullptr ||
      state->native_argument_attachment == nullptr ||
      state->native_argument_function_kind == nullptr ||
      state->native_argument_renderer_context_id == nullptr ||
      state->native_argument_function_id == nullptr ||
      state->native_argument_proxy_id == nullptr ||
      state->native_argument_lease_token == nullptr) {
    if (environment->ExceptionCheck()) {
      environment->ExceptionClear();
    }
    if (state->native_argument_class != nullptr) {
      environment->DeleteGlobalRef(state->native_argument_class);
    }
    environment->DeleteGlobalRef(state->bridge);
    return 0;
  }

  auto error_message = std::string{};
  if (process_runtime == nullptr) {
    process_virtual_machine = state->virtual_machine;
    process_bridge_class = static_cast<jclass>(
        environment->NewGlobalRef(bridge_type));
    if (process_bridge_class != nullptr) {
      schedule_runtime_stop_completion = environment->GetStaticMethodID(
          process_bridge_class, "scheduleNativeRuntimeStopCompletion", "()V");
    }
    if (process_bridge_class == nullptr ||
        schedule_runtime_stop_completion == nullptr) {
      if (process_bridge_class != nullptr) {
        environment->DeleteGlobalRef(process_bridge_class);
      }
      process_virtual_machine = nullptr;
      process_bridge_class = nullptr;
      schedule_runtime_stop_completion = nullptr;
      environment->DeleteGlobalRef(state->native_argument_class);
      environment->DeleteGlobalRef(state->bridge);
      ThrowIllegalState(environment,
                        "Could not initialize the Android process runtime");
      return 0;
    }
    process_runtime =
        new MuonAndroidProcessRuntimeController(ScheduleRuntimeStopCompletion);
  }
#if defined(MUON_TEST_BUILD)
  if (!pending_runtime_startup_fault.empty()) {
    const auto fault = std::exchange(pending_runtime_startup_fault, {});
    if (!process_runtime->SetStartupFaultForTest(fault, &error_message)) {
      environment->DeleteGlobalRef(state->native_argument_class);
      environment->DeleteGlobalRef(state->bridge);
      ThrowIllegalState(
          environment,
          error_message.empty() ? "Could not set Android startup test fault"
                                : error_message);
      return 0;
    }
  }
#endif
  MuonAndroidProcessSessionCallbacks callbacks;
  callbacks.runtime_ready = [target = state.get()]() {
    CompleteWaitingAndroidRpcHostStartup(target);
  };
  callbacks.runtime_failed = [target = state.get()](
                                 const std::string& diagnostic) {
    NotifyAndroidRpcHostStartupFailure(target, diagnostic);
  };
  callbacks.send_message = [target = state.get()](
                               const MuonRpcMessage& message,
                               std::string* send_error) {
    return SendRuntimeMessage(target, message, send_error);
  };
  callbacks.deliver_runtime_probe = [target = state.get()](uint32_t mask) {
    DeliverRuntimeProbe(target, mask);
  };
  callbacks.settle_runtime_probe = [target = state.get()](bool delivered) {
    SettleRuntimeProbe(target, delivered);
  };
  auto runtime_ready = false;
  if (!process_runtime->RegisterSession(
          std::move(callbacks), &state->owner, &runtime_ready,
          &error_message)) {
    environment->DeleteGlobalRef(state->native_argument_class);
    environment->DeleteGlobalRef(state->bridge);
    ThrowIllegalState(
        environment,
        error_message.empty() ? "Could not start the Android native runtime"
                              : error_message);
    return 0;
  }
  if (runtime_ready && !InitializeHost(state.get(), &error_message)) {
    process_runtime->UnregisterSession(state->owner, false);
    environment->DeleteGlobalRef(state->native_argument_class);
    environment->DeleteGlobalRef(state->bridge);
    ThrowIllegalState(environment, error_message);
    return 0;
  }
  return GetAndroidRpcHandle(state.release());
}

/** Returns whether a waiting RPC host has attached to a running runtime. */
extern "C" JNIEXPORT jboolean JNICALL
Java_dev_muon_runtime_MuonRpcBridge_nativeIsHostReady(
    JNIEnv*,
    jclass,
    jlong handle) {
  const auto* state = GetAndroidRpcHost(handle);
  return state != nullptr && state->host && !state->startup_failed
             ? JNI_TRUE
             : JNI_FALSE;
}

/** Returns plugin metadata captured before the WebView document starts. */
extern "C" JNIEXPORT jstring JNICALL
Java_dev_muon_runtime_MuonRpcBridge_nativeGetRendererMetadata(
    JNIEnv* environment,
    jclass,
    jlong handle) {
  const auto* state = GetAndroidRpcHost(handle);
  if (state == nullptr || state->renderer_metadata_json.empty()) {
    return environment->NewStringUTF("{}");
  }
  return environment->NewStringUTF(state->renderer_metadata_json.c_str());
}

/** Dispatches one decoded JavaScript call to the native RPC host. */
extern "C" JNIEXPORT void JNICALL
Java_dev_muon_runtime_MuonRpcBridge_nativeDispatchCall(
    JNIEnv* environment,
    jclass,
    jlong handle,
    jint call_id,
    jint call_kind,
    jstring capability_id,
    jstring function_path,
    jint proxy_id,
    jstring proxy_lease_token,
    jstring arguments_json,
    jobjectArray native_arguments,
    jobjectArray binary_arguments) {
  auto* state = GetAndroidRpcHost(handle);
  if (state == nullptr || !state->host || call_id <= 0 ||
      (call_kind != kNativeCallPlugin &&
       call_kind != kNativeCallPluginProxy)) {
    return;
  }
  if (process_runtime != nullptr) {
    process_runtime->ActivateSession(state->owner);
  }
  const auto path = GetJavaString(environment, function_path);
  MuonRpcCallRequest request;
  request.owner = state->owner;
  request.call_id = static_cast<uint32_t>(call_id);
  request.kind = call_kind == kNativeCallPluginProxy
                     ? MuonRpcCallKind::PluginProxy
                     : MuonRpcCallKind::Plugin;
  request.capability.id = GetJavaString(environment, capability_id);
  request.capability.function_path = path;
  auto route = MuonAndroidResolvedRoute{};
  const auto has_route = request.kind == MuonRpcCallKind::Plugin &&
                         ResolveFunctionRoute(state, path, &route);
  if (request.kind == MuonRpcCallKind::PluginProxy) {
    request.function_id = proxy_id > 0
                              ? static_cast<uint32_t>(proxy_id)
                              : 0;
    request.proxy_lease_token =
        GetJavaString(environment, proxy_lease_token);
  } else if (has_route) {
    request.function_id = route.function_id;
  } else {
    request.function_id = std::numeric_limits<uint32_t>::max();
  }

  if (request.kind == MuonRpcCallKind::PluginProxy ||
      (has_route && route.kind == MuonRpcRouteKind::Plugin)) {
    auto error_message = std::string{};
    if (!DecodePluginArguments(
            state, environment, &request, native_arguments,
            binary_arguments, &error_message, nullptr)) {
      MuonRpcCallResult result;
      result.owner = request.owner;
      result.call_id = request.call_id;
      result.success = false;
      result.error_message = error_message.empty()
                                 ? "Invalid Android plugin arguments"
                                 : error_message;
      SendResult(state, result);
      return;
    }
    state->host->HandleMessage(MuonRpcMessage{std::move(request)});
    return;
  }

  MuonRpcValue json_value;
  json_value.type = CreateMuonPrimitiveType(MUON_TYPE_STRING);
  json_value.string_value = GetJavaString(environment, arguments_json);
  request.arguments.push_back(std::move(json_value));

  const auto binary_count = binary_arguments == nullptr
                                ? jsize{0}
                                : environment->GetArrayLength(binary_arguments);
  for (auto index = jsize{0}; index < binary_count; ++index) {
    const auto binary_argument = static_cast<jbyteArray>(
        environment->GetObjectArrayElement(binary_arguments, index));
    if (binary_argument == nullptr) {
      continue;
    }
    const auto size = environment->GetArrayLength(binary_argument);
    auto storage = CreateMuonRpcOwnedBuffer(static_cast<size_t>(size));
    if (size != 0) {
      environment->GetByteArrayRegion(
          binary_argument, 0, size,
          static_cast<jbyte*>(storage->GetData()));
    }
    MuonRpcValue value;
    value.type = CreateMuonPrimitiveType(MUON_TYPE_BUFFER_VIEW);
    value.binary.storage = std::move(storage);
    value.binary.size = static_cast<size_t>(size);
    request.arguments.push_back(std::move(value));
    environment->DeleteLocalRef(binary_argument);
  }

  state->host->HandleMessage(MuonRpcMessage{std::move(request)});
}

/** Completes a plugin-initiated call to a renderer-owned function. */
extern "C" JNIEXPORT void JNICALL
Java_dev_muon_runtime_MuonRpcBridge_nativeCompleteRendererFunctionCall(
    JNIEnv* environment,
    jclass,
    jlong handle,
    jint call_id,
    jboolean success,
    jstring error,
    jobjectArray native_result,
    jobjectArray binary_arguments) {
  auto* state = GetAndroidRpcHost(handle);
  if (state == nullptr || process_runtime == nullptr || call_id <= 0) {
    return;
  }
  MuonRpcRendererFunctionResult result;
  result.owner = state->owner;
  result.call_id = static_cast<uint32_t>(call_id);
  if (success != JNI_TRUE) {
    result.success = false;
    result.error_message = GetJavaString(environment, error);
    if (result.error_message.empty()) {
      result.error_message = "Renderer function failed";
    }
    process_runtime->CompleteRendererFunctionCall(result);
    return;
  }

  auto return_type = CreateMuonPrimitiveType(MUON_TYPE_VOID);
  if (!process_runtime->GetRendererFunctionReturnType(
          state->owner, result.call_id, &return_type)) {
    return;
  }
  MuonRpcCallRequest decoded_result;
  decoded_result.owner = state->owner;
  decoded_result.call_id = result.call_id;
  auto expected_types = std::vector<MuonTypeMetadata>{return_type};
  auto error_message = std::string{};
  if (!DecodePluginArguments(
          state, environment, &decoded_result, native_result,
          binary_arguments, &error_message, &expected_types) ||
      decoded_result.arguments.size() != 1) {
    result.success = false;
    result.error_message = error_message.empty()
                               ? "Invalid renderer function result"
                               : error_message;
  } else {
    result.success = true;
    result.value = std::move(decoded_result.arguments[0]);
  }
  process_runtime->CompleteRendererFunctionCall(result);
}

/** Releases one JavaScript wrapper lease for a plugin function proxy. */
extern "C" JNIEXPORT void JNICALL
Java_dev_muon_runtime_MuonRpcBridge_nativeReleasePluginProxy(
    JNIEnv* environment,
    jclass,
    jlong handle,
    jint proxy_id,
    jstring lease_token) {
  auto* state = GetAndroidRpcHost(handle);
  if (state == nullptr || !state->host || proxy_id <= 0) {
    return;
  }
  MuonRpcPluginProxyRelease release;
  release.owner = state->owner;
  release.proxy_id = static_cast<uint32_t>(proxy_id);
  release.lease_token = GetJavaString(environment, lease_token);
  state->host->HandleMessage(MuonRpcMessage{release});
}

/** Cancels one native invocation retained by the WebView context. */
extern "C" JNIEXPORT void JNICALL
Java_dev_muon_runtime_MuonRpcBridge_nativeCancelCall(
    JNIEnv*,
    jclass,
    jlong handle,
    jint call_id) {
  auto* state = GetAndroidRpcHost(handle);
  if (state == nullptr || !state->host || call_id <= 0) {
    return;
  }
  MuonRpcCallCancel cancel;
  cancel.owner = state->owner;
  cancel.call_id = static_cast<uint32_t>(call_id);
  state->host->HandleMessage(MuonRpcMessage{cancel});
}

/** Completes a delayed call scheduled on the Android main looper. */
extern "C" JNIEXPORT void JNICALL
Java_dev_muon_runtime_MuonRpcBridge_nativeCompleteDelayedCall(
    JNIEnv*,
    jclass,
    jlong handle,
    jint call_id) {
  auto* state = GetAndroidRpcHost(handle);
  if (state == nullptr || call_id <= 0) {
    return;
  }
  const auto iterator =
      state->delayed_completions.find(static_cast<uint32_t>(call_id));
  if (iterator == state->delayed_completions.end()) {
    return;
  }
  auto completion = std::move(iterator->second);
  state->delayed_completions.erase(iterator);
  MuonRpcCallResult result;
  result.owner = state->owner;
  result.call_id = static_cast<uint32_t>(call_id);
  result.success = true;
  result.value.type = CreateMuonPrimitiveType(MUON_TYPE_STRING);
  result.value.string_value = "completed";
  completion(result);
}

/** Completes an Android service invocation retained by the RPC host. */
extern "C" JNIEXPORT void JNICALL
Java_dev_muon_runtime_MuonRpcBridge_nativeCompletePlatformCall(
    JNIEnv* environment,
    jclass,
    jlong handle,
    jint call_id,
    jint result_kind,
    jstring string_value,
    jlong unsigned_integer_value,
    jboolean boolean_value,
    jbyteArray binary_value,
    jstring error) {
  auto* state = GetAndroidRpcHost(handle);
  if (state == nullptr || call_id <= 0) {
    return;
  }
  const auto iterator =
      state->platform_completions.find(static_cast<uint32_t>(call_id));
  if (iterator == state->platform_completions.end()) {
    return;
  }
  auto completion = std::move(iterator->second);
  state->platform_completions.erase(iterator);

  MuonRpcCallResult result;
  result.owner = state->owner;
  result.call_id = static_cast<uint32_t>(call_id);
  if (error != nullptr) {
    result.success = false;
    result.error_message = GetJavaString(environment, error);
  } else {
    result.success = true;
    switch (result_kind) {
      case kPlatformResultVoid:
        result.value.type = CreateMuonPrimitiveType(MUON_TYPE_VOID);
        break;
      case kPlatformResultString:
        if (string_value == nullptr) {
          result.success = false;
          result.error_message = "Android string result is missing";
        } else {
          result.value.type = CreateMuonPrimitiveType(MUON_TYPE_STRING);
          result.value.string_value = GetJavaString(environment, string_value);
        }
        break;
      case kPlatformResultUnsignedInteger:
        if (unsigned_integer_value < 0 ||
            static_cast<uint64_t>(unsigned_integer_value) >
                std::numeric_limits<uint32_t>::max()) {
          result.success = false;
          result.error_message = "Android unsigned integer result is invalid";
        } else {
          result.value.type = CreateMuonPrimitiveType(MUON_TYPE_U32);
          result.value.u32_value =
              static_cast<uint32_t>(unsigned_integer_value);
        }
        break;
      case kPlatformResultBoolean:
        result.value.type = CreateMuonPrimitiveType(MUON_TYPE_BOOL);
        result.value.bool_value = boolean_value == JNI_TRUE;
        break;
      case kPlatformResultBinary:
        if (binary_value == nullptr) {
          result.success = false;
          result.error_message = "Android binary result is missing";
        } else {
          const auto size = environment->GetArrayLength(binary_value);
          auto storage = CreateMuonRpcOwnedBuffer(static_cast<size_t>(size));
          if (size != 0) {
            environment->GetByteArrayRegion(
                binary_value, 0, size,
                static_cast<jbyte*>(storage->GetData()));
          }
          result.value.type = CreateMuonPrimitiveType(MUON_TYPE_BUFFER_VIEW);
          result.value.binary.storage = std::move(storage);
          result.value.binary.size = static_cast<size_t>(size);
        }
        break;
      default:
        result.success = false;
        result.error_message = "Android platform result kind is invalid";
        break;
    }
  }

  completion(result);
}

/** Releases all calls owned by the current WebView JavaScript context. */
extern "C" JNIEXPORT void JNICALL
Java_dev_muon_runtime_MuonRpcBridge_nativeReleaseContext(
    JNIEnv*,
    jclass,
    jlong handle) {
  auto* state = GetAndroidRpcHost(handle);
  if (state == nullptr || !state->host) {
    return;
  }
  MuonRpcContextReleased release;
  release.owner = state->owner;
  state->host->HandleMessage(MuonRpcMessage{release});
}

/** Starts the packaged cardio integration probe for one WebView session. */
extern "C" JNIEXPORT void JNICALL
Java_dev_muon_runtime_MuonRpcBridge_nativeStartRuntimeProbe(
    JNIEnv*,
    jclass,
    jlong handle) {
  auto* state = GetAndroidRpcHost(handle);
  if (state == nullptr || process_runtime == nullptr) {
    return;
  }
  process_runtime->ActivateSession(state->owner);
  process_runtime->StartRuntimeProbe(state->owner);
}

/** Finalizes asynchronous plugin stop from a later Java main Looper task. */
extern "C" JNIEXPORT void JNICALL
Java_dev_muon_runtime_MuonRpcBridge_nativeCompleteRuntimeStop(
    JNIEnv*,
    jclass) {
  if (process_runtime != nullptr) {
    process_runtime->CompletePendingStop();
  }
}

/** Returns process runtime lifecycle diagnostics as a JSON object. */
extern "C" JNIEXPORT jstring JNICALL
Java_dev_muon_runtime_MuonRpcBridge_nativeGetRuntimeDiagnostics(
    JNIEnv* environment,
    jclass,
    jlong handle) {
  const auto* state = GetAndroidRpcHost(handle);
  if (state == nullptr || process_runtime == nullptr) {
    return environment->NewStringUTF("{}");
  }
  const auto json =
      CreateProcessDiagnosticsJson(process_runtime->GetDiagnostics());
  return environment->NewStringUTF(json.c_str());
}

/** Returns process diagnostics without requiring a live WebView host. */
extern "C" JNIEXPORT jstring JNICALL
Java_dev_muon_runtime_MuonRpcBridge_nativeGetProcessRuntimeDiagnosticsForTest(
    JNIEnv* environment,
    jclass) {
  if (process_runtime == nullptr) {
    return environment->NewStringUTF(
        "{\"runtimeState\":\"idle\",\"liveDispatcherHosts\":0}");
  }
  const auto json =
      CreateProcessDiagnosticsJson(process_runtime->GetDiagnostics());
  return environment->NewStringUTF(json.c_str());
}

/** Selects one debug-only packaged-plugin startup fault for the next run. */
extern "C" JNIEXPORT jboolean JNICALL
Java_dev_muon_runtime_MuonRpcBridge_nativeSetRuntimeStartupFaultForTest(
    JNIEnv* environment,
    jclass,
    jstring fault_value) {
#if defined(MUON_TEST_BUILD)
  const auto fault = GetJavaString(environment, fault_value);
  if (fault.empty()) {
    return JNI_FALSE;
  }
  if (process_runtime == nullptr) {
    static const auto known_faults = std::set<std::string>{
        "missing-library",
        "missing-entry",
        "init-failure",
        "invalid-metadata",
        "duplicate-path",
        "allow-mismatch",
    };
    if (known_faults.find(fault) == known_faults.end()) {
      return JNI_FALSE;
    }
    pending_runtime_startup_fault = fault;
    return JNI_TRUE;
  }
  auto error_message = std::string{};
  return process_runtime->SetStartupFaultForTest(fault, &error_message)
             ? JNI_TRUE
             : JNI_FALSE;
#else
  (void)environment;
  (void)fault_value;
  return JNI_FALSE;
#endif
}

/** Returns the number of calls retained by the native RPC host. */
extern "C" JNIEXPORT jint JNICALL
Java_dev_muon_runtime_MuonRpcBridge_nativeGetPendingCallCount(
    JNIEnv*,
    jclass,
    jlong handle) {
  const auto* state = GetAndroidRpcHost(handle);
  if (state == nullptr || !state->host) {
    return 0;
  }
  return static_cast<jint>(std::min(
      state->host->GetPendingCallCount(),
      static_cast<size_t>(std::numeric_limits<jint>::max())));
}

/** Destroys one native RPC host after its JavaScript context was released. */
extern "C" JNIEXPORT void JNICALL
Java_dev_muon_runtime_MuonRpcBridge_nativeDestroyHost(
    JNIEnv* environment,
    jclass,
    jlong handle,
    jboolean preserve_runtime) {
  auto* state = GetAndroidRpcHost(handle);
  if (state == nullptr) {
    return;
  }
  state->host.reset();
  state->delayed_completions.clear();
  state->platform_completions.clear();
  if (process_runtime != nullptr) {
    process_runtime->UnregisterSession(
        state->owner, preserve_runtime == JNI_TRUE);
  }
  if (state->bridge != nullptr) {
    environment->DeleteGlobalRef(state->bridge);
    state->bridge = nullptr;
  }
  if (state->native_argument_class != nullptr) {
    environment->DeleteGlobalRef(state->native_argument_class);
    state->native_argument_class = nullptr;
  }
  delete state;
}
