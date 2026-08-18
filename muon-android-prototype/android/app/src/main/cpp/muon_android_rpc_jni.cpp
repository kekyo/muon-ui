/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include <jni.h>

#include "plugins/muon_plugin_policy.h"
#include "rpc/muon_rpc_host.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

struct MuonAndroidRpcHost {
  jobject bridge = nullptr;
  jmethodID send_text_result = nullptr;
  jmethodID send_binary_result = nullptr;
  jmethodID schedule_delay = nullptr;
  jmethodID cancel_delay = nullptr;
  jmethodID cancel_all_delays = nullptr;
  JNIEnv* environment = nullptr;
  MuonRpcOwner owner = {1, "main", 1};
  std::shared_ptr<MuonRpcHost> host;
  std::map<uint32_t, MuonRpcHostCompletion> delayed_completions;
};

static constexpr uint32_t kGetConfigFunctionId = 1;
static constexpr uint32_t kFailFunctionId = 2;
static constexpr uint32_t kDelayFunctionId = 3;
static constexpr uint32_t kEchoBinaryFunctionId = 4;
static constexpr size_t kBinaryHeaderLength = 16;

static MuonAndroidRpcHost* GetAndroidRpcHost(jlong handle) {
  return reinterpret_cast<MuonAndroidRpcHost*>(
      static_cast<intptr_t>(handle));
}

static jlong GetAndroidRpcHandle(MuonAndroidRpcHost* host) {
  return static_cast<jlong>(reinterpret_cast<intptr_t>(host));
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

static void SendTextResult(MuonAndroidRpcHost* state,
                           const MuonRpcCallResult& result) {
  if (state->environment == nullptr || state->bridge == nullptr) {
    return;
  }
  auto message = std::string{"{\"version\":1,\"type\":\"result\",\"callId\":"};
  message.append(std::to_string(result.call_id));
  if (!result.success) {
    message.append(",\"success\":false,\"error\":");
    AppendJsonString(result.error_message, &message);
    message.push_back('}');
  } else {
    message.append(",\"success\":true,\"value\":");
    if (result.value.type.type == MUON_TYPE_STRING && !result.value.is_null) {
      AppendJsonString(result.value.string_value, &message);
    } else {
      message.append("null");
    }
    message.push_back('}');
  }

  const auto java_message = state->environment->NewStringUTF(message.c_str());
  if (java_message == nullptr) {
    return;
  }
  state->environment->CallVoidMethod(state->bridge, state->send_text_result,
                                     java_message);
  state->environment->DeleteLocalRef(java_message);
}

static void WriteBigEndianUint32(uint32_t value,
                                 size_t offset,
                                 std::vector<uint8_t>* bytes) {
  (*bytes)[offset] = static_cast<uint8_t>((value >> 24) & 0xff);
  (*bytes)[offset + 1] = static_cast<uint8_t>((value >> 16) & 0xff);
  (*bytes)[offset + 2] = static_cast<uint8_t>((value >> 8) & 0xff);
  (*bytes)[offset + 3] = static_cast<uint8_t>(value & 0xff);
}

static void SendBinaryResult(MuonAndroidRpcHost* state,
                             const MuonRpcCallResult& result) {
  if (state->environment == nullptr || state->bridge == nullptr ||
      !IsValidMuonRpcBinary(result.value.binary) ||
      result.value.binary.size >
          static_cast<size_t>(std::numeric_limits<jsize>::max()) -
              kBinaryHeaderLength) {
    return;
  }

  auto frame = std::vector<uint8_t>(kBinaryHeaderLength +
                                    result.value.binary.size);
  frame[0] = 'M';
  frame[1] = 'R';
  frame[2] = 'P';
  frame[3] = 'C';
  frame[4] = 1;
  frame[5] = 2;
  WriteBigEndianUint32(result.call_id, 8, &frame);
  WriteBigEndianUint32(0, 12, &frame);
  if (result.value.binary.size != 0) {
    std::memcpy(frame.data() + kBinaryHeaderLength,
                GetMuonRpcBinaryData(result.value.binary),
                result.value.binary.size);
  }

  const auto java_frame =
      state->environment->NewByteArray(static_cast<jsize>(frame.size()));
  if (java_frame == nullptr) {
    return;
  }
  state->environment->SetByteArrayRegion(
      java_frame, 0, static_cast<jsize>(frame.size()),
      reinterpret_cast<const jbyte*>(frame.data()));
  state->environment->CallVoidMethod(state->bridge, state->send_binary_result,
                                     java_frame);
  state->environment->DeleteLocalRef(java_frame);
}

static void SendResult(MuonAndroidRpcHost* state,
                       const MuonRpcCallResult& result) {
  if (result.success && result.value.type.type == MUON_TYPE_BUFFER_VIEW) {
    SendBinaryResult(state, result);
  } else {
    SendTextResult(state, result);
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

static void InvokePlatformFunction(MuonAndroidRpcHost* state,
                                   const MuonRpcCallRequest& request,
                                   MuonRpcHostCompletion completion) {
  if (request.function_id == kGetConfigFunctionId) {
    MuonRpcCallResult result;
    result.owner = request.owner;
    result.call_id = request.call_id;
    result.success = true;
    result.value.type = CreateMuonPrimitiveType(MUON_TYPE_STRING);
    result.value.string_value =
        "{\"channel\":\"android\",\"backend\":\"webview\"}";
    completion(result);
    return;
  }
  if (request.function_id == kFailFunctionId) {
    CompleteWithFailure(request, "prototype failure", std::move(completion));
    return;
  }
  if (request.function_id == kDelayFunctionId) {
    state->delayed_completions.emplace(request.call_id,
                                       std::move(completion));
    if (state->environment != nullptr) {
      state->environment->CallVoidMethod(
          state->bridge, state->schedule_delay,
          static_cast<jint>(request.call_id));
    }
    return;
  }
  if (request.function_id == kEchoBinaryFunctionId) {
    if (request.arguments.size() != 1 ||
        request.arguments[0].type.type != MUON_TYPE_BUFFER_VIEW ||
        !IsValidMuonRpcBinary(request.arguments[0].binary)) {
      CompleteWithFailure(request, "prototype binary argument is invalid",
                          std::move(completion));
      return;
    }
    MuonRpcCallResult result;
    result.owner = request.owner;
    result.call_id = request.call_id;
    result.success = true;
    result.value = request.arguments[0];
    completion(result);
    return;
  }
  CompleteWithFailure(request, "Unknown Android prototype RPC function",
                      std::move(completion));
}

static void CancelPlatformCall(MuonAndroidRpcHost* state,
                               const MuonRpcCallCancel& cancel) {
  state->delayed_completions.erase(cancel.call_id);
  if (state->environment != nullptr) {
    state->environment->CallVoidMethod(state->bridge, state->cancel_delay,
                                       static_cast<jint>(cancel.call_id));
  }
}

static void ReleasePlatformContext(MuonAndroidRpcHost* state) {
  state->delayed_completions.clear();
  if (state->environment != nullptr) {
    state->environment->CallVoidMethod(state->bridge,
                                       state->cancel_all_delays);
  }
}

static bool ResolveFunctionId(const std::string& function_path,
                              uint32_t* function_id) {
  if (function_path == "muon.environments.getConfigValues") {
    *function_id = kGetConfigFunctionId;
    return true;
  }
  if (function_path == "prototype.fail") {
    *function_id = kFailFunctionId;
    return true;
  }
  if (function_path == "prototype.delay") {
    *function_id = kDelayFunctionId;
    return true;
  }
  if (function_path == "prototype.echoBinary") {
    *function_id = kEchoBinaryFunctionId;
    return true;
  }
  return false;
}

static bool InitializeHost(MuonAndroidRpcHost* state,
                           std::string* error_message) {
  auto environment_policy = std::shared_ptr<MuonPluginPolicy>{};
  if (!CreateMuonPluginPolicy({"muon.environments.getConfigValues"},
                              &environment_policy, error_message)) {
    return false;
  }
  auto prototype_policy = std::shared_ptr<MuonPluginPolicy>{};
  if (!CreateMuonPluginPolicy({"prototype.*"}, &prototype_policy,
                              error_message)) {
    return false;
  }

  const auto routes = std::vector<MuonRpcFunctionRoute>{
      {kGetConfigFunctionId, "muon.environments.getConfigValues",
       MuonRpcRouteKind::Platform},
      {kFailFunctionId, "prototype.fail", MuonRpcRouteKind::Platform},
      {kDelayFunctionId, "prototype.delay", MuonRpcRouteKind::Platform},
      {kEchoBinaryFunctionId, "prototype.echoBinary",
       MuonRpcRouteKind::Platform},
  };
  const auto policies =
      std::map<std::string, std::shared_ptr<MuonPluginPolicy>>{
          {"environment-capability", environment_policy},
          {"prototype-capability", prototype_policy},
      };
  MuonRpcHostServices services;
  services.invoke_plugin =
      [](const MuonRpcCallRequest& request,
         MuonRpcHostCompletion completion) {
        CompleteWithFailure(request,
                            "Android prototype plugin routes are unavailable",
                            std::move(completion));
      };
  services.invoke_platform =
      [state](const MuonRpcCallRequest& request,
              MuonRpcHostCompletion completion) {
        InvokePlatformFunction(state, request, std::move(completion));
      };
  services.cancel_call = [state](const MuonRpcCallCancel& cancel) {
    CancelPlatformCall(state, cancel);
  };
  services.release_plugin_proxy = [](const MuonRpcPluginProxyRelease&) {};
  services.release_context = [state](const MuonRpcContextReleased&) {
    ReleasePlatformContext(state);
  };
  services.send_result = [state](const MuonRpcCallResult& result) {
    SendResult(state, result);
  };
  return CreateMuonRpcHost(MuonRpcHostMode::Validate, routes, policies,
                           std::move(services), &state->host, error_message);
}

/** Creates the CEF-independent host used by one WebView context. */
extern "C" JNIEXPORT jlong JNICALL
Java_dev_muon_prototype_MuonRpcBridge_nativeCreateHost(
    JNIEnv* environment,
    jclass,
    jobject bridge) {
  if (bridge == nullptr) {
    ThrowIllegalState(environment, "The Android RPC bridge is required");
    return 0;
  }
  auto state = std::make_unique<MuonAndroidRpcHost>();
  state->bridge = environment->NewGlobalRef(bridge);
  const auto bridge_class = environment->GetObjectClass(bridge);
  if (state->bridge == nullptr || bridge_class == nullptr) {
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
  environment->DeleteLocalRef(bridge_class);
  if (state->send_text_result == nullptr ||
      state->send_binary_result == nullptr || state->schedule_delay == nullptr ||
      state->cancel_delay == nullptr || state->cancel_all_delays == nullptr) {
    environment->DeleteGlobalRef(state->bridge);
    return 0;
  }

  auto error_message = std::string{};
  if (!InitializeHost(state.get(), &error_message)) {
    environment->DeleteGlobalRef(state->bridge);
    ThrowIllegalState(environment, error_message);
    return 0;
  }
  return GetAndroidRpcHandle(state.release());
}

/** Dispatches one decoded JavaScript call to the native RPC host. */
extern "C" JNIEXPORT void JNICALL
Java_dev_muon_prototype_MuonRpcBridge_nativeDispatchCall(
    JNIEnv* environment,
    jclass,
    jlong handle,
    jint call_id,
    jstring capability_id,
    jstring function_path,
    jbyteArray binary_argument) {
  auto* state = GetAndroidRpcHost(handle);
  if (state == nullptr || !state->host || call_id <= 0) {
    return;
  }
  const auto path = GetJavaString(environment, function_path);
  MuonRpcCallRequest request;
  request.owner = state->owner;
  request.call_id = static_cast<uint32_t>(call_id);
  request.capability.id = GetJavaString(environment, capability_id);
  request.capability.function_path = path;
  if (!ResolveFunctionId(path, &request.function_id)) {
    request.function_id = std::numeric_limits<uint32_t>::max();
  }
  if (binary_argument != nullptr) {
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
  }

  state->environment = environment;
  state->host->HandleMessage(MuonRpcMessage{std::move(request)});
  state->environment = nullptr;
}

/** Cancels one native invocation retained by the WebView context. */
extern "C" JNIEXPORT void JNICALL
Java_dev_muon_prototype_MuonRpcBridge_nativeCancelCall(
    JNIEnv* environment,
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
  state->environment = environment;
  state->host->HandleMessage(MuonRpcMessage{cancel});
  state->environment = nullptr;
}

/** Completes a delayed call scheduled on the Android main looper. */
extern "C" JNIEXPORT void JNICALL
Java_dev_muon_prototype_MuonRpcBridge_nativeCompleteDelayedCall(
    JNIEnv* environment,
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
  state->environment = environment;
  completion(result);
  state->environment = nullptr;
}

/** Releases all calls owned by the current WebView JavaScript context. */
extern "C" JNIEXPORT void JNICALL
Java_dev_muon_prototype_MuonRpcBridge_nativeReleaseContext(
    JNIEnv* environment,
    jclass,
    jlong handle) {
  auto* state = GetAndroidRpcHost(handle);
  if (state == nullptr || !state->host) {
    return;
  }
  MuonRpcContextReleased release;
  release.owner = state->owner;
  state->environment = environment;
  state->host->HandleMessage(MuonRpcMessage{release});
  state->environment = nullptr;
}

/** Returns the number of calls retained by the native RPC host. */
extern "C" JNIEXPORT jint JNICALL
Java_dev_muon_prototype_MuonRpcBridge_nativeGetPendingCallCount(
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
Java_dev_muon_prototype_MuonRpcBridge_nativeDestroyHost(
    JNIEnv* environment,
    jclass,
    jlong handle) {
  auto* state = GetAndroidRpcHost(handle);
  if (state == nullptr) {
    return;
  }
  state->host.reset();
  state->delayed_completions.clear();
  if (state->bridge != nullptr) {
    environment->DeleteGlobalRef(state->bridge);
    state->bridge = nullptr;
  }
  delete state;
}
