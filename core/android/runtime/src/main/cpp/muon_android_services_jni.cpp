/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include <jni.h>
#include <android/log.h>
#include <cardio.h>

#include "services/muon_native_service_host.h"
#include "services/muon_native_service_library.h"
#include "services/muon_service_controller.h"

#include <map>
#include <charconv>
#include <memory>
#include <string>
#include <utility>

static std::string ReadString(JNIEnv* env, jstring value) {
  if (value == nullptr) return {};
  const char* text = env->GetStringUTFChars(value, nullptr);
  const std::string result = text == nullptr ? "" : text;
  if (text != nullptr) env->ReleaseStringUTFChars(value, text);
  return result;
}

static std::string Quote(const std::string& value) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string result = "\"";
  for (const unsigned char character : value) {
    if (character == '"' || character == '\\') {
      result += '\\';
      result += static_cast<char>(character);
    } else if (character < 32) {
      result += "\\u00";
      result += hex[character >> 4];
      result += hex[character & 15];
    } else {
      result += static_cast<char>(character);
    }
  }
  return result + '"';
}

static std::string SnapshotJson(const MuonServiceSnapshot& state) {
  static const char* names[] = {"stopped", "starting", "running", "stopping", "failed"};
  return "{\"id\":" + Quote(state.id) + ",\"state\":" +
      Quote(names[static_cast<int>(state.state)]) + ",\"generation\":" +
      Quote(std::to_string(state.generation)) + ",\"revision\":" +
      Quote(std::to_string(state.revision)) + ",\"autostart\":" +
      (state.autostart ? "true" : "false") + ",\"resident\":" +
      (state.resident ? "true" : "false") + ",\"clientReferences\":" +
      std::to_string(state.client_references) + ",\"error\":" + Quote(state.error) + "}";
}

static std::string ReferenceJson(const MuonServiceReference& reference) {
  return "{\"serviceId\":" + Quote(reference.service_id) + ",\"refId\":" +
      Quote(std::to_string(reference.id)) + ",\"generation\":" +
      Quote(std::to_string(reference.generation)) + "}";
}

// This object belongs to the private Service process, independently of WebViews.
// Java constructs it once on the main Looper and retains it for that process.
struct MuonAndroidServices {
  JNIEnv* env;
  jobject owner;
  jmethodID post_method;
  jmethodID start_method;
  jmethodID stopped_method;
  jmethodID event_method;
  jmethodID save_method;
  std::unique_ptr<cardio::dispatcher_host_android_auto> dispatcher;
  std::unique_ptr<MuonNativeServiceHost> host;
  std::unique_ptr<MuonServiceController> controller;
  std::map<std::string, MuonNativeServiceSpec> specs;
  std::map<std::pair<std::string, uint64_t>, std::function<void(const std::string&)>> starts;
  std::map<uint64_t, std::function<void()>> tasks;
  std::map<uint64_t, std::function<void(const std::string&)>> saves;
  uint64_t next_task = 1;
  uint64_t next_save = 1;

  void Post(std::function<void()> task) {
    const auto id = next_task++;
    tasks.emplace(id, std::move(task));
    // Handler.post always defers; cardio continuations may otherwise run inline.
    env->CallVoidMethod(owner, post_method, static_cast<jlong>(id));
  }

  void ServiceCallback(jmethodID method, const std::string& id, uint64_t generation) {
    const auto name = env->NewStringUTF(id.c_str());
    env->CallVoidMethod(owner, method, name, static_cast<jlong>(generation));
    env->DeleteLocalRef(name);
  }

  void Notify(const MuonServiceEvent& event) {
    std::string kind;
    switch (event.kind) {
      case MuonServiceEventKind::Acquired: kind = "acquired"; break;
      case MuonServiceEventKind::AcquireFailed: kind = "acquireFailed"; break;
      case MuonServiceEventKind::Invalidated: kind = "invalidated"; break;
      case MuonServiceEventKind::Changed: kind = "changed"; break;
      case MuonServiceEventKind::AutostartResult: kind = "autostartResult"; break;
      default: return;
    }
    std::string json = "{\"kind\":" + Quote(kind) + ",\"snapshot\":" +
        SnapshotJson(event.snapshot) + ",\"error\":" + Quote(event.error);
    if (event.kind == MuonServiceEventKind::AutostartResult)
      json += ",\"operationId\":" + Quote(std::to_string(event.operation_id)) +
          ",\"success\":" + (event.success ? "true" : "false");
    if (event.reference) {
      json += ",\"owner\":" + Quote(event.reference->owner_id) +
          ",\"reference\":" + ReferenceJson(*event.reference);
    }
    const auto value = env->NewStringUTF((json + "}").c_str());
    env->CallVoidMethod(owner, event_method, value);
    env->DeleteLocalRef(value);
  }

  void CompleteStart(const std::string& id, uint64_t generation, const std::string& error) {
    const auto found = starts.find({id, generation});
    if (found == starts.end()) return;
    auto complete = std::move(found->second);
    starts.erase(found);
    if (!error.empty()) ServiceCallback(stopped_method, id, generation);
    complete(error);
  }
};

static MuonAndroidServices* Runtime(jlong handle) {
  return reinterpret_cast<MuonAndroidServices*>(handle);
}

extern "C" JNIEXPORT jlong JNICALL
Java_dev_muon_runtime_MuonServiceProcess_nativeCreate(
    JNIEnv* env, jclass, jobject owner, jobjectArray ids, jobjectArray libraries,
    jobjectArray directories, jbooleanArray autostarts, jintArray overrides,
    jobjectArray configurations) {
  auto runtime = std::make_unique<MuonAndroidServices>();
  runtime->env = env;
  runtime->owner = env->NewGlobalRef(owner);
  const auto type = env->GetObjectClass(owner);
  runtime->post_method = env->GetMethodID(type, "postTask", "(J)V");
  runtime->start_method = env->GetMethodID(type, "startComponent", "(Ljava/lang/String;J)V");
  runtime->stopped_method = env->GetMethodID(type, "stopComponent", "(Ljava/lang/String;J)V");
  runtime->event_method = env->GetMethodID(type, "onEvent", "(Ljava/lang/String;)V");
  runtime->save_method = env->GetMethodID(type, "saveAutostart", "(Ljava/lang/String;ZJ)V");
  env->DeleteLocalRef(type);
  runtime->dispatcher = std::make_unique<cardio::dispatcher_host_android_auto>();
  auto* state = runtime.get();
  runtime->host = std::make_unique<MuonNativeServiceHost>(MuonNativeServicePlatform{
      LoadMuonNativeServiceFactory,
      [state](std::function<void()> task) { state->Post(std::move(task)); },
      [](const std::string& id, muon_log_level level, const std::string& text) {
        const int priorities[] = {ANDROID_LOG_DEBUG, ANDROID_LOG_INFO, ANDROID_LOG_WARN,
                                  ANDROID_LOG_ERROR, ANDROID_LOG_FATAL};
        const auto index = static_cast<unsigned>(level);
        __android_log_write(index < 5 ? priorities[index] : ANDROID_LOG_INFO,
                            "MuonNativeService", (id + ": " + text).c_str());
      }});
  std::vector<MuonServiceDefinition> definitions;
  const auto count = env->GetArrayLength(ids);
  for (jsize index = 0; index < count; ++index) {
    const auto read = [env, index](jobjectArray values) {
      const auto value = static_cast<jstring>(env->GetObjectArrayElement(values, index));
      const auto text = ReadString(env, value);
      env->DeleteLocalRef(value);
      return text;
    };
    MuonNativeServiceSpec spec{read(ids), read(libraries), read(directories), {}};
    const auto pairs = static_cast<jobjectArray>(env->GetObjectArrayElement(configurations, index));
    for (jsize entry = 0; entry < env->GetArrayLength(pairs); entry += 2) {
      const auto key = static_cast<jstring>(env->GetObjectArrayElement(pairs, entry));
      const auto value = static_cast<jstring>(env->GetObjectArrayElement(pairs, entry + 1));
      spec.config.emplace_back(ReadString(env, key), ReadString(env, value));
      env->DeleteLocalRef(key);
      env->DeleteLocalRef(value);
    }
    env->DeleteLocalRef(pairs);
    jboolean autostart = false;
    env->GetBooleanArrayRegion(autostarts, index, 1, &autostart);
    jint user = -1;
    env->GetIntArrayRegion(overrides, index, 1, &user);
    definitions.push_back({spec.id, autostart == JNI_TRUE,
        user < 0 ? std::nullopt : std::optional<bool>{user != 0}});
    runtime->specs.emplace(spec.id, std::move(spec));
  }
  runtime->controller = std::make_unique<MuonServiceController>(definitions, MuonServiceBackend{
      [state](const MuonServiceSnapshot& snapshot, std::function<void(const std::string&)> complete) {
        state->starts.emplace(std::make_pair(snapshot.id, snapshot.generation), std::move(complete));
        state->ServiceCallback(state->start_method, snapshot.id, snapshot.generation);
      },
      [state](const MuonServiceSnapshot& snapshot, std::function<void()> complete) {
        state->host->Stop(snapshot.id, snapshot.generation, [state, snapshot, complete = std::move(complete)]() {
          state->ServiceCallback(state->stopped_method, snapshot.id, snapshot.generation);
          complete();
        });
      },
      [state](const std::string& id, bool enabled, std::function<void(const std::string&)> complete) {
        const auto task = state->next_save++;
        state->saves.emplace(task, std::move(complete));
        const auto name = state->env->NewStringUTF(id.c_str());
        state->env->CallVoidMethod(state->owner, state->save_method, name,
            enabled ? JNI_TRUE : JNI_FALSE, static_cast<jlong>(task));
        state->env->DeleteLocalRef(name);
      },
      [state](const MuonServiceEvent& event) { state->Notify(event); },
      [state](std::function<void()> task) { state->Post(std::move(task)); }});
  return reinterpret_cast<jlong>(runtime.release());
}

extern "C" JNIEXPORT void JNICALL
Java_dev_muon_runtime_MuonServiceProcess_nativeRunTask(JNIEnv*, jclass, jlong handle, jlong id) {
  auto* state = Runtime(handle);
  const auto found = state->tasks.find(static_cast<uint64_t>(id));
  if (found == state->tasks.end()) return;
  auto task = std::move(found->second);
  state->tasks.erase(found);
  task();
}

extern "C" JNIEXPORT void JNICALL
Java_dev_muon_runtime_MuonServiceProcess_nativeActivate(JNIEnv*, jclass, jlong handle) {
  Runtime(handle)->controller->Activate();
}

extern "C" JNIEXPORT jstring JNICALL
Java_dev_muon_runtime_MuonServiceProcess_nativeSetAutostart(
    JNIEnv* env, jclass, jlong handle, jstring id, jboolean enabled) {
  const auto operation = Runtime(handle)->controller->SetAutostart(ReadString(env, id), enabled == JNI_TRUE);
  return operation == 0 ? nullptr : env->NewStringUTF(std::to_string(operation).c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_dev_muon_runtime_MuonServiceProcess_nativeSaved(
    JNIEnv* env, jclass, jlong handle, jlong task, jstring error) {
  auto* state = Runtime(handle);
  const auto found = state->saves.find(static_cast<uint64_t>(task));
  if (found == state->saves.end()) return;
  auto complete = std::move(found->second);
  state->saves.erase(found);
  complete(ReadString(env, error));
}

extern "C" JNIEXPORT jstring JNICALL
Java_dev_muon_runtime_MuonServiceProcess_nativeAcquire(
    JNIEnv* env, jclass, jlong handle, jstring id, jstring owner) {
  const auto reference = Runtime(handle)->controller->Acquire(ReadString(env, id), ReadString(env, owner));
  return reference ? env->NewStringUTF(ReferenceJson(*reference).c_str()) : nullptr;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_dev_muon_runtime_MuonServiceProcess_nativeRelease(
    JNIEnv* env, jclass, jlong handle, jstring id, jstring owner, jstring reference, jstring generation) {
  uint64_t reference_id = 0;
  uint64_t version = 0;
  const auto parse = [env](jstring input, uint64_t& output) {
    const auto text = ReadString(env, input);
    const auto end = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), end, output);
    return parsed.ec == std::errc{} && parsed.ptr == end;
  };
  if (!parse(reference, reference_id) || !parse(generation, version)) return JNI_FALSE;
  return Runtime(handle)->controller->Release({reference_id, ReadString(env, id),
      ReadString(env, owner), version}) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_dev_muon_runtime_MuonServiceProcess_nativeReleaseOwner(
    JNIEnv* env, jclass, jlong handle, jstring owner) {
  Runtime(handle)->controller->ReleaseOwner(ReadString(env, owner));
}

extern "C" JNIEXPORT jstring JNICALL
Java_dev_muon_runtime_MuonServiceProcess_nativeSnapshot(JNIEnv* env, jclass, jlong handle, jstring id) {
  const auto snapshot = Runtime(handle)->controller->GetSnapshot(ReadString(env, id));
  return snapshot ? env->NewStringUTF(SnapshotJson(*snapshot).c_str()) : nullptr;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_dev_muon_runtime_MuonServiceProcess_nativeForegroundReady(
    JNIEnv* env, jclass, jlong handle, jstring name, jlong generation) {
  auto* state = Runtime(handle);
  const auto id = ReadString(env, name);
  const auto version = static_cast<uint64_t>(generation);
  if (!state->starts.contains({id, version})) return JNI_FALSE;
  state->host->Start(state->specs.at(id), version, [state, id, version](const std::string& error) {
    state->CompleteStart(id, version, error);
  });
  return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_dev_muon_runtime_MuonServiceProcess_nativeStartFailed(
    JNIEnv* env, jclass, jlong handle, jstring id, jlong generation, jstring error) {
  Runtime(handle)->CompleteStart(ReadString(env, id), static_cast<uint64_t>(generation), ReadString(env, error));
}

extern "C" JNIEXPORT void JNICALL
Java_dev_muon_runtime_MuonServiceProcess_nativeRuntimeStop(
    JNIEnv* env, jclass, jlong handle, jstring id, jlong generation, jstring error) {
  Runtime(handle)->controller->RequestRuntimeStop(ReadString(env, id),
      static_cast<uint64_t>(generation), ReadString(env, error));
}
