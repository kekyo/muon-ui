/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 */
#include "muon_android_service_http.h"
#include "muon_android_jni_utf.h"
#include "services/muon_service_files.h"
#include "services/muon_service_http.h"

static JNIEnv* Environment(JavaVM* vm) {
  JNIEnv* env = nullptr;
  // Both the owner Looper and the platform executor are Java-owned threads.
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK)
    throw MuonServiceIoError("network-error", "Android service executor is detached");
  return env;
}
struct HttpJavaObject {
  JavaVM* vm;
  jobject object;
  HttpJavaObject(JNIEnv* env, jobject value) : object(env->NewGlobalRef(value)) { env->GetJavaVM(&vm); }
  ~HttpJavaObject() { Environment(vm)->DeleteGlobalRef(object); }
};
struct AndroidHttpApi {
  HttpJavaObject type;
  jmethodID create, send, cancel;
  jfieldID code, message, headers, status;
  AndroidHttpApi(JNIEnv* env, jclass java_type) : type(env, java_type),
      create(env->GetMethodID(java_type, "<init>", "()V")),
      send(env->GetMethodID(java_type, "send", "(Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;[BI)[B")),
      cancel(env->GetMethodID(java_type, "cancel", "()V")),
      code(env->GetFieldID(java_type, "code", "Ljava/lang/String;")), message(env->GetFieldID(java_type, "message", "Ljava/lang/String;")),
      headers(env->GetFieldID(java_type, "headers", "[Ljava/lang/String;")), status(env->GetFieldID(java_type, "status", "I")) {}
};
static MuonServiceHttpResult AndroidHttp(const AndroidHttpApi& api, const MuonServiceHttpHop& hop, MuonServiceIoCancellation& cancel) {
  auto* env = Environment(api.type.vm);
  if (env->PushLocalFrame(32) != JNI_OK) throw std::bad_alloc();
  struct Frame { JNIEnv* env; ~Frame() { env->PopLocalFrame(nullptr); } } frame{env};
  auto transfer = std::make_shared<HttpJavaObject>(env, env->NewObject(static_cast<jclass>(api.type.object), api.create));
  cancel.Bind([transfer, method = api.cancel] {
    auto* current = Environment(transfer->vm);
    current->CallVoidMethod(transfer->object, method);
    if (current->ExceptionCheck()) current->ExceptionClear();
  });
  cancel.Check();
  const auto fields = env->NewObjectArray(static_cast<jsize>(hop.headers.size() * 2), env->FindClass("java/lang/String"), nullptr);
  jsize index = 0;
  for (const auto& [key, value] : hop.headers) {
    for (const auto& item : {key, value}) {
      const auto field = MuonAndroidNewJavaString(env, item);
      env->SetObjectArrayElement(fields, index++, field); env->DeleteLocalRef(field);
    }
  }
  const auto body = env->NewByteArray(static_cast<jsize>(hop.body.size()));
  if (!hop.body.empty()) env->SetByteArrayRegion(body, 0, static_cast<jsize>(hop.body.size()), reinterpret_cast<const jbyte*>(hop.body.data()));
  const auto bytes = static_cast<jbyteArray>(env->CallObjectMethod(transfer->object, api.send,
      MuonAndroidNewJavaString(env, hop.origin + hop.target), MuonAndroidNewJavaString(env, hop.method), fields, body, hop.timeout_ms));
  if (env->ExceptionCheck()) { env->ExceptionClear(); throw MuonServiceIoError("network-error", "Android HTTP transport failed"); }
  cancel.Check();
  const auto code = static_cast<jstring>(env->GetObjectField(transfer->object, api.code));
  if (code) throw MuonServiceIoError(MuonAndroidReadJavaString(env, code),
      MuonAndroidReadJavaString(env, static_cast<jstring>(env->GetObjectField(transfer->object, api.message))));
  MuonServiceHttpResult result;
  result.status = env->GetIntField(transfer->object, api.status);
  const auto response_fields = static_cast<jobjectArray>(env->GetObjectField(transfer->object, api.headers));
  for (jsize i = 0, n = env->GetArrayLength(response_fields); i < n; i += 2) {
    const auto key = static_cast<jstring>(env->GetObjectArrayElement(response_fields, i));
    const auto value = static_cast<jstring>(env->GetObjectArrayElement(response_fields, i + 1));
    AddMuonServiceHttpHeader(result, MuonAndroidReadJavaString(env, key), MuonAndroidReadJavaString(env, value));
    env->DeleteLocalRef(key); env->DeleteLocalRef(value);
  }
  result.body.resize(static_cast<size_t>(env->GetArrayLength(bytes)));
  if (!result.body.empty()) env->GetByteArrayRegion(bytes, 0, static_cast<jsize>(result.body.size()), reinterpret_cast<jbyte*>(result.body.data()));
  return result;
}
MuonServiceHttpTransport CreateAndroidMuonServiceHttp(JNIEnv* env) {
  const auto type = env->FindClass("dev/muon/runtime/MuonServiceHttp");
  if (!type) throw std::runtime_error("Missing Android service HTTP transport");
  const auto api = std::make_shared<AndroidHttpApi>(env, type);
  env->DeleteLocalRef(type);
  return [api](const MuonServiceHttpHop& hop, MuonServiceIoCancellation& cancel) { return AndroidHttp(*api, hop, cancel); };
}
