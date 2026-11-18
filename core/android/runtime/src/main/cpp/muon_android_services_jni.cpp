/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include <jni.h>
#include "muon_android_jni_utf.h"
#include <android/log.h>
#include <cardio.h>

#include "services/muon_native_service_host.h"
#include "services/muon_native_service_library.h"
#include "services/muon_service_controller.h"
#include "services/muon_service_broker.h"
#include "services/muon_quickjs_service_host.h"
#include "services/muon_quickjs_service_plugins.h"
#include "services/muon_service_execution_router.h"
#include "muon_android_service_peer.h"
#include "muon_android_service_plugins.h"

#include <map>
#include <charconv>
#include <memory>
#include <set>
#include <string>
#include <utility>

static std::string ReadString(JNIEnv* env, jstring value) {
  return MuonAndroidReadJavaString(env, value);
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
  JNIEnv* env = nullptr;
  jobject owner = nullptr;
  jmethodID post_method;
  jmethodID file_method;
  jmethodID delay_method;
  jmethodID cancel_method;
  jmethodID start_method;
  jmethodID stopped_method;
  jmethodID event_method;
  jmethodID save_method;
  jmethodID disconnected_method;
  std::unique_ptr<cardio::dispatcher_host_android_auto> dispatcher;
  std::unique_ptr<MuonServicePluginPool> plugins;
  std::unique_ptr<MuonQuickJsServicePlugins> plugin_startup;
  std::set<std::string> plugin_services;
  std::unique_ptr<MuonNativeServiceHost> host;
  std::unique_ptr<MuonQuickJsServiceHost> javascript;
  std::unique_ptr<MuonServiceController> controller;
  std::unique_ptr<MuonServiceBroker> broker;
  // Java retains this host until the process exits. Every peer closes on its
  // Binder owner's release and drains against this still-live broker/dispatcher.
  std::map<std::string, std::shared_ptr<MuonAndroidServicePeer>> peers;
  std::map<std::string, MuonNativeServiceSpec> specs;
  std::map<std::string, size_t> runtimes;
  std::map<std::pair<std::string, uint64_t>, std::function<void(const std::string&)>> starts;
  std::map<uint64_t, std::function<void()>> tasks;
  std::map<uint64_t, std::function<void(const std::string&)>> saves;
  uint64_t next_task = 1;
  uint64_t next_save = 1;

  ~MuonAndroidServices() {
    if (owner) env->DeleteGlobalRef(owner);
  }

  void Post(std::function<void()> task) {
    const auto id = next_task++;
    tasks.emplace(id, std::move(task));
    // Handler.post always defers; cardio continuations may otherwise run inline.
    env->CallVoidMethod(owner, post_method, static_cast<jlong>(id));
  }

  std::function<void()> Delay(std::chrono::milliseconds delay, std::function<void()> task) {
    const auto id = next_task++;
    tasks.emplace(id, std::move(task));
    env->CallVoidMethod(owner, delay_method, static_cast<jlong>(id), static_cast<jlong>(delay.count()));
    return [this, id] {
      tasks.erase(id);
      env->CallVoidMethod(owner, cancel_method, static_cast<jlong>(id));
    };
  }

  void ServiceCallback(jmethodID method, const std::string& id, uint64_t generation) {
    const auto name = MuonAndroidNewJavaString(env, id.c_str());
    env->CallVoidMethod(owner, method, name, static_cast<jlong>(generation));
    env->DeleteLocalRef(name);
  }

  void Notify(const MuonServiceEvent& event) {
    if (event.kind == MuonServiceEventKind::Invalidated && event.reference && broker)
      broker->Release(*event.reference);
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
    const auto value = MuonAndroidNewJavaString(env, (json + "}").c_str());
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

struct AndroidFileTask {
  std::function<void()> work;
  cardio::promise_source<std::string> finished;
};

extern "C" JNIEXPORT void JNICALL
Java_dev_muon_runtime_MuonServiceProcess_nativeFileWork(JNIEnv*, jclass, jlong handle) {
  auto job = std::unique_ptr<AndroidFileTask>(reinterpret_cast<AndroidFileTask*>(handle));
  std::string error;
  try { job->work(); } catch (const std::exception& exception) { error = exception.what(); }
  job->finished.resolve(std::move(error));
}

static cardio::promise<void> SubmitFile(MuonAndroidServices* state, std::function<void()> work,
                                        std::function<void(std::string)> complete) {
  auto job = std::make_unique<AndroidFileTask>();
  job->work = std::move(work);
  auto finished = job->finished.get_promise();
  auto* raw = job.release();
  state->env->CallVoidMethod(state->owner, state->file_method, reinterpret_cast<jlong>(raw));
  if (state->env->ExceptionCheck()) {
    state->env->ExceptionClear();
    delete raw;
    complete("Android file executor rejected work");
    co_return;
  }
  complete(co_await finished);
}

extern "C" JNIEXPORT jlong JNICALL
Java_dev_muon_runtime_MuonServiceProcess_nativeCreate(
    JNIEnv* env, jclass, jobject owner, jobjectArray ids, jobjectArray runtimes, jobjectArray libraries,
    jobjectArray directories, jobjectArray io_policies, jbooleanArray autostarts, jintArray overrides,
    jobjectArray configurations, jobjectArray plugin_grants, jstring plugin_registry) {
  try {
  auto runtime = std::make_unique<MuonAndroidServices>();
  runtime->env = env;
  runtime->owner = env->NewGlobalRef(owner);
  const auto type = env->GetObjectClass(owner);
  runtime->file_method = env->GetMethodID(type, "submitFileWork", "(J)V");
  runtime->post_method = env->GetMethodID(type, "postTask", "(J)V");
  runtime->delay_method = env->GetMethodID(type, "delayTask", "(JJ)V");
  runtime->cancel_method = env->GetMethodID(type, "cancelTask", "(J)V");
  runtime->start_method = env->GetMethodID(type, "startComponent", "(Ljava/lang/String;J)V");
  runtime->stopped_method = env->GetMethodID(type, "stopComponent", "(Ljava/lang/String;J)V");
  runtime->event_method = env->GetMethodID(type, "onEvent", "(Ljava/lang/String;)V");
  runtime->save_method = env->GetMethodID(type, "saveAutostart", "(Ljava/lang/String;ZJ)V");
  runtime->disconnected_method = env->GetMethodID(type, "onRpcDisconnected", "(Ljava/lang/String;)V");
  env->DeleteLocalRef(type);
  runtime->dispatcher = std::make_unique<cardio::dispatcher_host_android_auto>();
  auto* state = runtime.get();
  const auto log = [](const std::string& id, muon_log_level level, const std::string& text) {
        const int priorities[] = {ANDROID_LOG_DEBUG, ANDROID_LOG_INFO, ANDROID_LOG_WARN,
                                  ANDROID_LOG_ERROR, ANDROID_LOG_FATAL};
        const auto index = static_cast<unsigned>(level);
        __android_log_write(index < 5 ? priorities[index] : ANDROID_LOG_INFO,
                            "MuonNativeService", (id + ": " + text).c_str());
      };
  runtime->host = std::make_unique<MuonNativeServiceHost>(MuonNativeServicePlatform{
      LoadMuonNativeServiceFactory,
      [state](std::function<void()> task) { state->Post(std::move(task)); }, log,
      [state](std::chrono::milliseconds delay, std::function<void()> task) { return state->Delay(delay, std::move(task)); }});
  runtime->javascript = std::make_unique<MuonQuickJsServiceHost>(MuonQuickJsServicePlatform{
      [state](std::function<void()> task) { state->Post(std::move(task)); },
      [state](std::chrono::milliseconds delay, std::function<void()> task) {
        return state->Delay(delay, std::move(task));
      }, log,
      [state](const std::string& id, uint64_t generation, const std::string& error) {
        state->controller->RuntimeExited(id, generation, error);
        state->ServiceCallback(state->stopped_method, id, generation);
      }, {64 * 1024 * 1024, 1024 * 1024, std::chrono::seconds(2)},
      std::chrono::seconds(10),
      [state](const MuonQuickJsServiceSpec& spec, uint64_t generation,
          MuonQuickJsEngine& engine, std::function<void()> wake,
          std::function<void(const std::string&)> fault) {
        return state->plugin_startup
            ? state->plugin_startup->Attach(spec.id, generation, engine, std::move(wake), std::move(fault))
            : MuonQuickJsServiceExtension{};
      }});
  std::vector<MuonServiceDefinition> definitions;
  std::vector<MuonServicePluginDefinition> plugin_definitions;
  const auto count = env->GetArrayLength(ids);
  for (jsize index = 0; index < count; ++index) {
    const auto read = [env, index](jobjectArray values) {
      const auto value = static_cast<jstring>(env->GetObjectArrayElement(values, index));
      const auto text = ReadString(env, value);
      env->DeleteLocalRef(value);
      return text;
    };
    MuonNativeServiceSpec spec{read(ids), read(libraries), read(directories), {}};
    spec.io.policy = read(io_policies);
    spec.io.submit = [state](std::function<void()> work, std::function<void(std::string)> complete) {
      cardio::fire_and_forget(SubmitFile(state, std::move(work), std::move(complete)));
    };
    runtime->runtimes.emplace(spec.id, read(runtimes) == "quickjs" ? 1 : 0);
    const auto pairs = static_cast<jobjectArray>(env->GetObjectArrayElement(configurations, index));
    for (jsize entry = 0; entry < env->GetArrayLength(pairs); entry += 2) {
      const auto key = static_cast<jstring>(env->GetObjectArrayElement(pairs, entry));
      const auto value = static_cast<jstring>(env->GetObjectArrayElement(pairs, entry + 1));
      spec.config.emplace_back(ReadString(env, key), ReadString(env, value));
      env->DeleteLocalRef(key);
      env->DeleteLocalRef(value);
    }
    env->DeleteLocalRef(pairs);
    const auto grants = static_cast<jobjectArray>(env->GetObjectArrayElement(plugin_grants, index));
    MuonServicePluginDefinition plugin_definition{spec.id, {}};
    for (jsize grant = 0; grant < env->GetArrayLength(grants); ++grant) {
      const auto value = static_cast<jstring>(env->GetObjectArrayElement(grants, grant));
      plugin_definition.allow.push_back(ReadString(env, value));
      env->DeleteLocalRef(value);
    }
    env->DeleteLocalRef(grants);
    if (!plugin_definition.allow.empty()) {
      runtime->plugin_services.insert(spec.id);
      plugin_definitions.push_back(std::move(plugin_definition));
    }
    jboolean autostart = false;
    env->GetBooleanArrayRegion(autostarts, index, 1, &autostart);
    jint user = -1;
    env->GetIntArrayRegion(overrides, index, 1, &user);
    definitions.push_back({spec.id, autostart == JNI_TRUE,
        user < 0 ? std::nullopt : std::optional<bool>{user != 0}});
    runtime->specs.emplace(spec.id, std::move(spec));
  }
  runtime->plugins = CreateMuonAndroidServicePlugins(ReadString(env, plugin_registry),
      std::move(plugin_definitions), *runtime->dispatcher);
  if (runtime->plugins)
    runtime->plugin_startup = std::make_unique<MuonQuickJsServicePlugins>(*runtime->plugins,
        [state](std::chrono::milliseconds delay, std::function<void()> task) {
          return state->Delay(delay, std::move(task));
        });
  runtime->controller = std::make_unique<MuonServiceController>(definitions, MuonServiceBackend{
      [state](const MuonServiceSnapshot& snapshot, std::function<void(const std::string&)> complete) {
        state->starts.emplace(std::make_pair(snapshot.id, snapshot.generation), std::move(complete));
        state->ServiceCallback(state->start_method, snapshot.id, snapshot.generation);
      },
      [state](const MuonServiceSnapshot& snapshot, std::function<void()> complete) {
        auto stopped = [state, snapshot, complete = std::move(complete)]() {
          state->ServiceCallback(state->stopped_method, snapshot.id, snapshot.generation);
          complete();
        };
        if (state->runtimes.at(snapshot.id) == 1)
          state->javascript->Stop(snapshot.id, snapshot.generation, std::move(stopped), snapshot.stop);
        else state->host->Stop(snapshot.id, snapshot.generation, std::move(stopped), snapshot.stop);
      },
      [state](const std::string& id, bool enabled, std::function<void(const std::string&)> complete) {
        const auto task = state->next_save++;
        state->saves.emplace(task, std::move(complete));
        const auto name = MuonAndroidNewJavaString(state->env, id.c_str());
        state->env->CallVoidMethod(state->owner, state->save_method, name,
            enabled ? JNI_TRUE : JNI_FALSE, static_cast<jlong>(task));
        state->env->DeleteLocalRef(name);
      },
      [state](const MuonServiceEvent& event) { state->Notify(event); },
      [state](std::function<void()> task) { state->Post(std::move(task)); }});
  runtime->broker = std::make_unique<MuonServiceBroker>(*runtime->controller,
      RouteMuonServiceExecutions({runtime->host->Execution(), runtime->javascript->Execution()},
          [state](const std::string& id) { return state->runtimes.at(id); }));
  return reinterpret_cast<jlong>(runtime.release());
  } catch (const std::exception& error) {
    const auto type = env->FindClass("java/lang/IllegalStateException");
    const auto constructor = env->GetMethodID(type, "<init>", "(Ljava/lang/String;)V");
    const auto message = MuonAndroidNewJavaString(env, error.what());
    const auto failure = static_cast<jthrowable>(env->NewObject(type, constructor, message));
    env->Throw(failure);
    env->DeleteLocalRef(failure);
    env->DeleteLocalRef(message);
    env->DeleteLocalRef(type);
    return 0;
  }
}

extern "C" JNIEXPORT void JNICALL
Java_dev_muon_runtime_MuonServiceProcess_nativeOpenRpc(
    JNIEnv* env, jclass, jlong handle, jstring owner, jint descriptor) {
  auto* state = Runtime(handle);
  const auto identity = ReadString(env, owner);
  try {
    auto peer = OpenMuonAndroidServicePeer(descriptor, identity, *state->broker,
        [state](std::function<void()> task) { state->Post(std::move(task)); },
        [state, identity] {
      const auto name = MuonAndroidNewJavaString(state->env, identity);
      state->env->CallVoidMethod(state->owner, state->disconnected_method, name);
      state->env->DeleteLocalRef(name);
    });
    // Java accepts only one stream for the lifetime of each authenticated owner.
    state->peers.emplace(identity, std::move(peer));
  } catch (const std::exception& error) {
    const auto type = env->FindClass("java/lang/IllegalStateException");
    if (type) { env->ThrowNew(type, error.what()); env->DeleteLocalRef(type); }
  }
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
  return operation == 0 ? nullptr : MuonAndroidNewJavaString(env, std::to_string(operation).c_str());
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
  return reference ? MuonAndroidNewJavaString(env, ReferenceJson(*reference).c_str()) : nullptr;
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
  auto* state = Runtime(handle);
  const auto value = MuonServiceReference{reference_id, ReadString(env, id), ReadString(env, owner), version};
  const auto released = state->controller->Release(value);
  state->broker->Release(value);
  return released ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_dev_muon_runtime_MuonServiceProcess_nativeReleaseOwner(
    JNIEnv* env, jclass, jlong handle, jstring owner) {
  auto* state = Runtime(handle);
  const auto identity = ReadString(env, owner);
  const auto found = state->peers.find(identity);
  if (found != state->peers.end()) {
    const auto peer = found->second;
    state->peers.erase(found);
    peer->close();
  }
  state->broker->CloseOwner(identity);
}

extern "C" JNIEXPORT jstring JNICALL
Java_dev_muon_runtime_MuonServiceProcess_nativeSnapshot(JNIEnv* env, jclass, jlong handle, jstring id) {
  const auto snapshot = Runtime(handle)->controller->GetSnapshot(ReadString(env, id));
  return snapshot ? MuonAndroidNewJavaString(env, SnapshotJson(*snapshot).c_str()) : nullptr;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_dev_muon_runtime_MuonServiceProcess_nativeForegroundReady(
    JNIEnv* env, jclass, jlong handle, jstring name, jlong generation, jstring source, jlong remaining) {
  auto* state = Runtime(handle);
  const auto id = ReadString(env, name);
  const auto version = static_cast<uint64_t>(generation);
  if (!state->starts.contains({id, version})) return JNI_FALSE;
  const auto snapshot = state->controller->GetSnapshot(id);
  if (!snapshot || snapshot->generation != version || !snapshot->error.empty()) {
    state->CompleteStart(id, version, snapshot ? snapshot->error : "Service startup was invalidated");
    return JNI_FALSE;
  }
  auto complete = [state, id, version](const std::string& error) {
    state->CompleteStart(id, version, error);
  };
  const auto spec = MuonServiceSpecForGeneration(state->specs.at(id), *snapshot);
  if (state->runtimes.at(id) == 1) {
    if (!source || remaining <= 0) {
      complete("Service source startup deadline exceeded");
      return JNI_FALSE;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(remaining);
    MuonQuickJsServiceSpec script{id, spec.library_path, ReadString(env, source), spec.data_directory,
        spec.config, deadline, spec.start_reason, spec.signal, spec.io};
    // The lifecycle waits for startup to settle before dispatching Stop. This
    // includes native pool drainage and cannot create an orphaned late heap.
    if (state->plugin_services.contains(id))
      state->plugin_startup->Start(*state->javascript, script, version, std::move(complete));
    else state->javascript->Start(script, version, std::move(complete));
  } else state->host->Start(spec, version, std::move(complete));
  return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_dev_muon_runtime_MuonServiceProcess_nativeStartFailed(
    JNIEnv* env, jclass, jlong handle, jstring id, jlong generation, jstring error) {
  Runtime(handle)->CompleteStart(ReadString(env, id), static_cast<uint64_t>(generation), ReadString(env, error));
}

extern "C" JNIEXPORT void JNICALL
Java_dev_muon_runtime_MuonServiceProcess_nativeRuntimeStop(
    JNIEnv* env, jclass, jlong handle, jstring id, jlong generation, jstring error, jboolean timeout) {
  const auto detail = ReadString(env, error);
  Runtime(handle)->controller->RequestRuntimeStop(ReadString(env, id),
      static_cast<uint64_t>(generation), detail, {timeout ? MUON_SERVICE_STOP_EXECUTION_TIMEOUT : MUON_SERVICE_STOP_HOST, detail});
}

extern "C" JNIEXPORT void JNICALL
Java_dev_muon_runtime_MuonServiceProcess_nativeSessionEnding(JNIEnv*, jclass, jlong handle) {
  Runtime(handle)->controller->Shutdown("Android device shutdown", {MUON_SERVICE_STOP_SESSION_ENDING, "shutdown"});
}
