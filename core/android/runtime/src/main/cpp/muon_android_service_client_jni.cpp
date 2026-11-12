/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "muon_android_dispatcher.h"
#include "muon_android_jni_utf.h"
#include "muon_android_service_transport.h"
#include "services/muon_service_client.h"
#include <map>
#include <optional>
#include <utility>

// Each Java page owns one native client. Its holder survives Java close until
// every transport task and completed coroutine has left the shared dispatcher.
struct MuonAndroidServiceClient {
  JNIEnv* env = nullptr;
  jobject owner = nullptr;
  jmethodID post_method = nullptr;
  jmethodID result_method = nullptr;
  jmethodID disconnected_method = nullptr;
  jmethodID closed_method = nullptr;
  std::shared_ptr<MuonAndroidServiceClient>* holder = nullptr;
  std::shared_ptr<cardio::dispatcher_host_android_auto> dispatcher;
  std::optional<cardio::promise_source<MuonServiceTransport>> endpoint;
  std::unique_ptr<MuonServiceClient> client;
  std::map<jint, std::optional<cardio::promise<void>>> calls;
  std::optional<cardio::promise<void>> drain;
  std::map<uint64_t, std::function<void()>> tasks;
  uint64_t next_task = 1;
  bool closing = false;
  bool drained = false;

  ~MuonAndroidServiceClient() { if (owner) env->DeleteGlobalRef(owner); }

  void Post(std::function<void()> task) {
    const auto id = next_task++;
    tasks.emplace(id, std::move(task));
    env->CallVoidMethod(owner, post_method, reinterpret_cast<jlong>(holder), static_cast<jlong>(id));
  }
};

static std::shared_ptr<MuonAndroidServiceClient> Client(jlong handle) {
  return *reinterpret_cast<std::shared_ptr<MuonAndroidServiceClient>*>(handle);
}

static void Dispose(const std::shared_ptr<MuonAndroidServiceClient>& state) {
  if (!state->closing || !state->drained || !state->calls.empty() || !state->tasks.empty()) return;
  state->client.reset();
  state->endpoint.reset();
  state->dispatcher.reset();
  state->env->CallVoidMethod(state->owner, state->closed_method);
  state->env->DeleteGlobalRef(state->owner);
  state->owner = nullptr;
  delete std::exchange(state->holder, nullptr);
}

static cardio::promise<void> Invoke(std::shared_ptr<MuonAndroidServiceClient> state,
    jint request, std::string json) {
  auto result = std::string{};
  auto failure = std::string{};
  try { result = co_await state->client->Call("rpc", std::move(json)); }
  catch (const std::exception& error) { failure = error.what(); }
  state->Post([state, request, result = std::move(result), failure = std::move(failure)] {
    state->calls.erase(request);
    if (state->closing) return;
    const auto value = failure.empty() ? MuonAndroidNewJavaString(state->env, result) : nullptr;
    const auto error = failure.empty() ? nullptr : MuonAndroidNewJavaString(state->env, failure);
    state->env->CallVoidMethod(state->owner, state->result_method, request, value, error);
    if (value) state->env->DeleteLocalRef(value);
    if (error) state->env->DeleteLocalRef(error);
  });
}

static cardio::promise<void> Drain(std::shared_ptr<MuonAndroidServiceClient> state) {
  co_await state->client->Close();
  // Close settles I/O before returning. Handler.post also allows the currently
  // returning cardio coroutine frames to be destroyed before the final lease.
  state->Post([state] { state->drain.reset(); state->drained = true; });
}

extern "C" JNIEXPORT jlong JNICALL
Java_dev_muon_runtime_MuonServiceClient_nativeCreateRpc(JNIEnv* env, jclass, jobject owner) {
  try {
    const auto state = std::make_shared<MuonAndroidServiceClient>();
    state->env = env;
    state->owner = env->NewGlobalRef(owner);
    if (!state->owner) throw std::bad_alloc();
    const auto type = env->GetObjectClass(owner);
    state->post_method = env->GetMethodID(type, "postRpcTask", "(JJ)V");
    state->result_method = env->GetMethodID(type, "onRpcResult", "(ILjava/lang/String;Ljava/lang/String;)V");
    state->closed_method = env->GetMethodID(type, "onRpcClosed", "()V");
    state->disconnected_method = env->GetMethodID(type, "onRpcDisconnected", "()V");
    env->DeleteLocalRef(type);
    state->dispatcher = AcquireMuonAndroidDispatcher();
    state->endpoint.emplace();
    const auto weak = std::weak_ptr<MuonAndroidServiceClient>(state);
    state->client = std::make_unique<MuonServiceClient>(
        [weak](cardio::cancellation) -> cardio::promise<MuonServiceTransport> {
      const auto current = weak.lock();
      co_return co_await current->endpoint->get_promise();
    });
    auto holder = std::make_unique<std::shared_ptr<MuonAndroidServiceClient>>(state);
    state->holder = holder.get();
    return reinterpret_cast<jlong>(holder.release());
  } catch (const std::exception& error) {
    if (!env->ExceptionCheck()) {
      const auto type = env->FindClass("java/lang/IllegalStateException");
      if (type) { env->ThrowNew(type, error.what()); env->DeleteLocalRef(type); }
    }
    return 0;
  }
}

extern "C" JNIEXPORT void JNICALL
Java_dev_muon_runtime_MuonServiceClient_nativeAttachRpc(JNIEnv* env, jclass, jlong handle, jint descriptor) {
  try {
    auto transport = AdoptMuonAndroidServiceTransport(descriptor);
    const auto state = Client(handle);
    const auto weak = std::weak_ptr<MuonAndroidServiceClient>(state);
    const auto close = transport.close;
    transport.close = [weak, close] {
      close();
      if (const auto current = weak.lock(); current && !current->closing) {
        current->Post([current] {
          if (!current->closing) current->env->CallVoidMethod(current->owner, current->disconnected_method);
        });
      }
    };
    state->endpoint->resolve(std::move(transport));
  } catch (const std::exception& error) {
    if (!env->ExceptionCheck()) {
      const auto type = env->FindClass("java/lang/IllegalStateException");
      if (type) { env->ThrowNew(type, error.what()); env->DeleteLocalRef(type); }
    }
  }
}

extern "C" JNIEXPORT void JNICALL
Java_dev_muon_runtime_MuonServiceClient_nativeRpc(JNIEnv* env, jclass, jlong handle, jint request, jstring json) {
  const auto state = Client(handle);
  if (state->closing) return;
  auto text = MuonAndroidReadJavaString(env, json);
  if (env->ExceptionCheck()) return;
  const auto entry = state->calls.emplace(request, std::nullopt).first;
  entry->second.emplace(Invoke(state, request, std::move(text)));
}

extern "C" JNIEXPORT void JNICALL
Java_dev_muon_runtime_MuonServiceClient_nativeRunRpcTask(JNIEnv*, jclass, jlong handle, jlong identity) {
  const auto state = Client(handle);
  const auto found = state->tasks.find(static_cast<uint64_t>(identity));
  if (found == state->tasks.end()) return;
  auto task = std::move(found->second);
  state->tasks.erase(found);
  task();
  task = nullptr;
  Dispose(state);
}

extern "C" JNIEXPORT void JNICALL
Java_dev_muon_runtime_MuonServiceClient_nativeCloseRpc(JNIEnv*, jclass, jlong handle) {
  const auto state = Client(handle);
  if (state->closing) return;
  state->closing = true;
  state->endpoint->try_reject(std::make_exception_ptr(cardio::canceled_exception()));
  state->drain.emplace(Drain(state));
}
