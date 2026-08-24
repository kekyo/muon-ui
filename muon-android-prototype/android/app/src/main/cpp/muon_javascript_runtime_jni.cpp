/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include <jni.h>

#include <android/log.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "quickjs.h"

static constexpr const char* kLogTag = "muon-quickjs";
static constexpr const char* kQuickJsVersion = "2026-06-04";
static constexpr std::size_t kMemoryLimit = 64U * 1024U * 1024U;
static constexpr std::size_t kStackLimit = 1024U * 1024U;
static constexpr std::uint32_t kMaximumFrameLength = 16U * 1024U * 1024U;
static constexpr std::int64_t kExecutionLimitMilliseconds = 2000;
static constexpr std::int64_t kMaximumTimerMilliseconds = 60000;
static constexpr std::size_t kNetworkReadBufferSize = 64U * 1024U;

struct MuonJavaScriptSession;

struct MuonNetworkAddress {
  std::string address;
  int family = 0;
};

struct MuonDnsResult {
  std::int64_t identifier = 0;
  std::string hostname;
  std::vector<MuonNetworkAddress> addresses;
  std::string code;
  std::string message;
};

struct MuonHostEvent {
  std::int64_t identifier = 0;
  std::string type;
  std::int64_t operation_identifier = 0;
  std::vector<std::uint8_t> data;
  std::vector<MuonNetworkAddress> addresses;
  std::string hostname;
  std::string address;
  std::string family;
  int port = 0;
  std::string local_address;
  std::string local_family;
  int local_port = 0;
  std::string code;
  std::string message;
  std::string syscall;
  std::string payload;
};

struct MuonTcpWrite {
  std::int64_t operation_identifier = 0;
  std::vector<std::uint8_t> data;
  std::size_t offset = 0;
};

struct MuonTcpSocket {
  std::int64_t identifier = 0;
  int file_descriptor = -1;
  bool connecting = true;
  bool paused = false;
  bool read_ended = false;
  bool end_requested = false;
  bool write_ended = false;
  std::uint64_t bytes_read = 0;
  std::uint64_t bytes_written = 0;
  std::string remote_address;
  std::string remote_family;
  int remote_port = 0;
  std::deque<MuonTcpWrite> writes;
};

struct MuonPendingPromise {
  JSValue promise = JS_UNDEFINED;
};

struct MuonTimer {
  std::int64_t identifier = 0;
  std::chrono::steady_clock::time_point deadline;
};

struct MuonJavaScriptHost {
  int file_descriptor = -1;
  std::filesystem::path filesystem_root;
  JSRuntime* runtime = nullptr;
  JSContext* context = nullptr;
  std::vector<MuonPendingPromise> pending_promises;
  std::vector<MuonTimer> timers;
  std::shared_ptr<MuonJavaScriptSession> session;
  std::unordered_map<std::int64_t, MuonTcpSocket> tcp_sockets;
  std::vector<MuonHostEvent> host_events;
  std::atomic<bool>* stop_requested = nullptr;
  std::atomic<std::int64_t> execution_deadline_nanoseconds{0};
};

struct MuonJavaScriptSession {
  std::string runtime_id;
  int file_descriptor = -1;
  int control_file_descriptor = -1;
  int event_file_descriptor = -1;
  std::mutex descriptor_mutex;
  std::mutex dns_result_mutex;
  std::deque<MuonDnsResult> dns_results;
  bool accepts_dns_results = true;
  std::mutex external_event_mutex;
  std::deque<MuonHostEvent> external_events;
  bool accepts_external_events = true;
  std::atomic<bool> stop_requested{false};
};

struct MuonDnsRequest {
  std::weak_ptr<MuonJavaScriptSession> session;
  std::int64_t identifier = 0;
  std::string hostname;
  int family = 0;
};

struct MuonDnsWorker {
  std::mutex mutex;
  std::condition_variable changed;
  std::deque<MuonDnsRequest> requests;
  bool stopping = false;
  std::thread thread;

  MuonDnsWorker();
  ~MuonDnsWorker();
};

static std::mutex g_sessions_mutex;
static std::condition_variable g_sessions_changed;
static std::unordered_map<std::string, std::shared_ptr<MuonJavaScriptSession>>
    g_sessions;
static std::mutex g_java_http_mutex;
static JavaVM* g_java_virtual_machine = nullptr;
static jclass g_java_http_client_class = nullptr;
static jmethodID g_java_http_start_method = nullptr;
static jmethodID g_java_http_cancel_method = nullptr;
static jmethodID g_java_http_handle_data_method = nullptr;
static jmethodID g_java_http_resume_method = nullptr;
static jmethodID g_java_http_cancel_runtime_method = nullptr;

static std::string dns_error_code(int result) {
  if (result == EAI_NONAME) {
    return "ENOTFOUND";
  }
#ifdef EAI_NODATA
  if (result == EAI_NODATA) {
    return "ENOTFOUND";
  }
#endif
  if (result == EAI_AGAIN) {
    return "EAI_AGAIN";
  }
  if (result == EAI_MEMORY) {
    return "ENOMEM";
  }
  if (result == EAI_FAMILY) {
    return "EAI_FAMILY";
  }
  if (result == EAI_SYSTEM && errno != 0) {
    return std::string("E") + std::to_string(errno);
  }
  return "EAI_FAIL";
}

static void notify_session_event(
    const std::shared_ptr<MuonJavaScriptSession>& session) {
  std::uint64_t increment = 1;
  while (write(session->event_file_descriptor, &increment,
               sizeof(increment)) < 0) {
    if (errno == EINTR) {
      continue;
    }
    break;
  }
}

static MuonDnsResult resolve_dns_request(const MuonDnsRequest& request) {
  MuonDnsResult result;
  result.identifier = request.identifier;
  result.hostname = request.hostname;
  addrinfo hints{};
  hints.ai_family = request.family == 4   ? AF_INET
                    : request.family == 6 ? AF_INET6
                                          : AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* addresses = nullptr;
  auto status = getaddrinfo(request.hostname.c_str(), nullptr, &hints,
                            &addresses);
  if (status != 0) {
    result.code = dns_error_code(status);
    result.message = std::string("getaddrinfo ") + request.hostname + ": " +
                     gai_strerror(status);
    return result;
  }
  for (auto* current = addresses; current != nullptr;
       current = current->ai_next) {
    auto family = current->ai_family == AF_INET    ? 4
                  : current->ai_family == AF_INET6 ? 6
                                                   : 0;
    if (family == 0) {
      continue;
    }
    char numeric[INET6_ADDRSTRLEN]{};
    const void* source =
        family == 4
            ? static_cast<const void*>(
                  &reinterpret_cast<const sockaddr_in*>(current->ai_addr)
                       ->sin_addr)
            : static_cast<const void*>(
                  &reinterpret_cast<const sockaddr_in6*>(current->ai_addr)
                       ->sin6_addr);
    if (inet_ntop(current->ai_family, source, numeric, sizeof(numeric)) ==
        nullptr) {
      continue;
    }
    auto duplicate = std::find_if(
        result.addresses.begin(), result.addresses.end(),
        [numeric, family](const MuonNetworkAddress& candidate) {
          return candidate.family == family && candidate.address == numeric;
        });
    if (duplicate == result.addresses.end()) {
      result.addresses.push_back(MuonNetworkAddress{numeric, family});
    }
  }
  freeaddrinfo(addresses);
  if (result.addresses.empty()) {
    result.code = "ENOTFOUND";
    result.message = "getaddrinfo returned no usable address for " +
                     request.hostname;
  }
  return result;
}

static void run_dns_worker(MuonDnsWorker* worker) {
  while (true) {
    MuonDnsRequest request;
    {
      std::unique_lock<std::mutex> lock(worker->mutex);
      worker->changed.wait(lock, [worker]() {
        return worker->stopping || !worker->requests.empty();
      });
      if (worker->stopping && worker->requests.empty()) {
        return;
      }
      request = std::move(worker->requests.front());
      worker->requests.pop_front();
    }
    auto result = resolve_dns_request(request);
    auto session = request.session.lock();
    if (session == nullptr || session->stop_requested.load()) {
      continue;
    }
    {
      std::lock_guard<std::mutex> lock(session->dns_result_mutex);
      if (!session->accepts_dns_results) {
        continue;
      }
      session->dns_results.push_back(std::move(result));
      notify_session_event(session);
    }
  }
}

MuonDnsWorker::MuonDnsWorker() : thread(run_dns_worker, this) {}

MuonDnsWorker::~MuonDnsWorker() {
  {
    std::lock_guard<std::mutex> lock(mutex);
    stopping = true;
  }
  changed.notify_all();
  if (thread.joinable()) {
    thread.join();
  }
}

static MuonDnsWorker& dns_worker() {
  static MuonDnsWorker worker;
  return worker;
}

static void enqueue_dns_request(MuonDnsRequest request) {
  auto& worker = dns_worker();
  {
    std::lock_guard<std::mutex> lock(worker.mutex);
    worker.requests.push_back(std::move(request));
  }
  worker.changed.notify_one();
}

static void log_error(const std::string& message) {
  __android_log_write(ANDROID_LOG_ERROR, kLogTag, message.c_str());
}

static std::int64_t steady_nanoseconds() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

static std::string get_jni_string(JNIEnv* environment, jstring value) {
  if (value == nullptr) {
    return {};
  }
  const char* contents = environment->GetStringUTFChars(value, nullptr);
  if (contents == nullptr) {
    return {};
  }
  auto result = std::string(contents);
  environment->ReleaseStringUTFChars(value, contents);
  return result;
}

static void throw_illegal_state(JNIEnv* environment,
                                const std::string& message) {
  auto exception_class =
      environment->FindClass("java/lang/IllegalStateException");
  if (exception_class != nullptr) {
    environment->ThrowNew(exception_class, message.c_str());
  }
}

static bool initialize_java_http_bridge(JNIEnv* environment) {
  std::lock_guard<std::mutex> lock(g_java_http_mutex);
  if (g_java_http_client_class != nullptr) {
    return true;
  }
  JavaVM* virtual_machine = nullptr;
  if (environment->GetJavaVM(&virtual_machine) != JNI_OK) {
    throw_illegal_state(environment, "Unable to access the Android Java VM.");
    return false;
  }
  auto local_class =
      environment->FindClass("dev/muon/prototype/MuonJavaScriptHttpClient");
  if (local_class == nullptr) {
    environment->ExceptionClear();
    throw_illegal_state(environment,
                        "Unable to load the Android HTTP client bridge.");
    return false;
  }
  auto global_class = reinterpret_cast<jclass>(
      environment->NewGlobalRef(local_class));
  environment->DeleteLocalRef(local_class);
  if (global_class == nullptr) {
    throw_illegal_state(environment,
                        "Unable to retain the Android HTTP client bridge.");
    return false;
  }
  auto start = environment->GetStaticMethodID(
      global_class, "start",
      "(Ljava/lang/String;JLjava/lang/String;Ljava/lang/String;"
      "Ljava/lang/String;[BIILjava/lang/String;)V");
  auto cancel = environment->GetStaticMethodID(
      global_class, "cancel", "(Ljava/lang/String;J)Z");
  auto handle_data = environment->GetStaticMethodID(
      global_class, "handleData", "(Ljava/lang/String;JZ)Z");
  auto resume = environment->GetStaticMethodID(
      global_class, "resume", "(Ljava/lang/String;J)Z");
  auto cancel_runtime = environment->GetStaticMethodID(
      global_class, "cancelRuntime", "(Ljava/lang/String;)V");
  if (start == nullptr || cancel == nullptr || handle_data == nullptr ||
      resume == nullptr || cancel_runtime == nullptr) {
    environment->ExceptionClear();
    environment->DeleteGlobalRef(global_class);
    throw_illegal_state(
        environment, "The Android HTTP client bridge has an invalid API.");
    return false;
  }
  g_java_virtual_machine = virtual_machine;
  g_java_http_client_class = global_class;
  g_java_http_start_method = start;
  g_java_http_cancel_method = cancel;
  g_java_http_handle_data_method = handle_data;
  g_java_http_resume_method = resume;
  g_java_http_cancel_runtime_method = cancel_runtime;
  return true;
}

static JNIEnv* attach_java_environment(bool* attached) {
  *attached = false;
  if (g_java_virtual_machine == nullptr) {
    return nullptr;
  }
  JNIEnv* environment = nullptr;
  auto status = g_java_virtual_machine->GetEnv(
      reinterpret_cast<void**>(&environment), JNI_VERSION_1_6);
  if (status == JNI_OK) {
    return environment;
  }
  if (status != JNI_EDETACHED ||
      g_java_virtual_machine->AttachCurrentThread(&environment, nullptr) !=
          JNI_OK) {
    return nullptr;
  }
  *attached = true;
  return environment;
}

static bool finish_java_http_call(JNIEnv* environment, bool attached) {
  auto succeeded = !environment->ExceptionCheck();
  if (!succeeded) {
    environment->ExceptionClear();
  }
  if (attached) {
    g_java_virtual_machine->DetachCurrentThread();
  }
  return succeeded;
}

static bool read_exact(int file_descriptor, void* destination,
                       std::size_t length) {
  auto* bytes = static_cast<std::uint8_t*>(destination);
  std::size_t offset = 0;
  while (offset < length) {
    auto result = read(file_descriptor, bytes + offset, length - offset);
    if (result == 0) {
      return false;
    }
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    offset += static_cast<std::size_t>(result);
  }
  return true;
}

static bool write_exact(int file_descriptor, const void* source,
                        std::size_t length) {
  const auto* bytes = static_cast<const std::uint8_t*>(source);
  std::size_t offset = 0;
  while (offset < length) {
    auto result = write(file_descriptor, bytes + offset, length - offset);
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    offset += static_cast<std::size_t>(result);
  }
  return true;
}

static bool read_frame(int file_descriptor, std::string* destination) {
  std::uint32_t encoded_length = 0;
  if (!read_exact(file_descriptor, &encoded_length, sizeof(encoded_length))) {
    return false;
  }
  auto length = ntohl(encoded_length);
  if (length == 0 || length > kMaximumFrameLength) {
    return false;
  }
  destination->resize(length);
  return read_exact(file_descriptor, destination->data(), length);
}

static bool write_frame(int file_descriptor, const std::string& source) {
  if (source.empty() || source.size() > kMaximumFrameLength) {
    return false;
  }
  auto encoded_length = htonl(static_cast<std::uint32_t>(source.size()));
  return write_exact(file_descriptor, &encoded_length,
                     sizeof(encoded_length)) &&
         write_exact(file_descriptor, source.data(), source.size());
}

static bool get_js_string(JSContext* context, JSValueConst value,
                          std::string* destination) {
  std::size_t length = 0;
  const char* contents = JS_ToCStringLen(context, &length, value);
  if (contents == nullptr) {
    return false;
  }
  destination->assign(contents, length);
  JS_FreeCString(context, contents);
  return true;
}

static std::string take_exception(JSContext* context) {
  auto exception = JS_GetException(context);
  std::string message;
  if (!get_js_string(context, exception, &message)) {
    message = "Unknown QuickJS exception";
  }
  auto stack = JS_GetPropertyStr(context, exception, "stack");
  std::string stack_text;
  if (!JS_IsException(stack) && !JS_IsUndefined(stack) &&
      get_js_string(context, stack, &stack_text) && !stack_text.empty()) {
    message.append("\n");
    message.append(stack_text);
  }
  JS_FreeValue(context, stack);
  JS_FreeValue(context, exception);
  return message;
}

static MuonJavaScriptHost* require_host(JSContext* context) {
  auto* host = static_cast<MuonJavaScriptHost*>(JS_GetContextOpaque(context));
  if (host == nullptr) {
    JS_ThrowInternalError(context, "JavaScript runtime host is unavailable");
  }
  return host;
}

static JSValue get_host_module(JSContext* context, const char* module_name) {
  auto global = JS_GetGlobalObject(context);
  auto getter = JS_GetPropertyStr(context, global, "__muonGetHostModule");
  if (!JS_IsFunction(context, getter)) {
    JS_FreeValue(context, getter);
    JS_FreeValue(context, global);
    return JS_ThrowInternalError(context,
                                 "JavaScript host module registry is unavailable");
  }
  auto argument = JS_NewString(context, module_name);
  auto module = JS_Call(context, getter, global, 1, &argument);
  JS_FreeValue(context, argument);
  JS_FreeValue(context, getter);
  JS_FreeValue(context, global);
  return module;
}

static int for_each_host_module_export(
    JSContext* context, JSValueConst module,
    const std::function<int(const char*, JSAtom)>& callback) {
  JSPropertyEnum* properties = nullptr;
  std::uint32_t property_count = 0;
  if (JS_GetOwnPropertyNames(context, &properties, &property_count, module,
                             JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
    return -1;
  }
  auto result = 0;
  for (std::uint32_t index = 0; index < property_count; ++index) {
    const char* name = JS_AtomToCString(context, properties[index].atom);
    if (name == nullptr) {
      result = -1;
      break;
    }
    if (std::strcmp(name, "default") != 0) {
      result = callback(name, properties[index].atom);
    }
    JS_FreeCString(context, name);
    if (result < 0) {
      break;
    }
  }
  JS_FreePropertyEnum(context, properties, property_count);
  return result;
}

static int initialize_host_module(JSContext* context, JSModuleDef* definition) {
  // The loader stores the JavaScript module object until QuickJS instantiates
  // the synthetic module, when its properties can become ESM exports.
  auto module = JS_GetModulePrivateValue(context, definition);
  if (JS_IsException(module)) {
    return -1;
  }
  auto default_export = JS_GetPropertyStr(context, module, "default");
  if (JS_IsException(default_export)) {
    JS_FreeValue(context, module);
    return -1;
  }
  if (JS_IsUndefined(default_export)) {
    JS_FreeValue(context, default_export);
    default_export = JS_DupValue(context, module);
  }
  auto result =
      JS_SetModuleExport(context, definition, "default", default_export);
  if (result >= 0) {
    result = for_each_host_module_export(
        context, module, [&](const char* name, JSAtom atom) {
          auto value = JS_GetProperty(context, module, atom);
          if (JS_IsException(value)) {
            return -1;
          }
          return JS_SetModuleExport(context, definition, name, value);
        });
  }
  JS_FreeValue(context, module);
  return result;
}

static JSModuleDef* load_host_module(JSContext* context,
                                     const char* module_name, void* opaque) {
  (void)opaque;
  auto module = get_host_module(context, module_name);
  if (JS_IsException(module)) {
    return nullptr;
  }
  if (!JS_IsObject(module)) {
    JS_FreeValue(context, module);
    JS_ThrowTypeError(context, "Host module '%s' is not an object", module_name);
    return nullptr;
  }
  auto* definition =
      JS_NewCModule(context, module_name, initialize_host_module);
  if (definition == nullptr ||
      JS_AddModuleExport(context, definition, "default") < 0 ||
      for_each_host_module_export(
          context, module, [&](const char* name, JSAtom atom) {
            (void)atom;
            return JS_AddModuleExport(context, definition, name);
          }) < 0) {
    JS_FreeValue(context, module);
    return nullptr;
  }
  JS_SetModulePrivateValue(context, definition, module);
  return definition;
}

static bool resolve_filesystem_path(JSContext* context,
                                    MuonJavaScriptHost* host,
                                    JSValueConst value,
                                    std::filesystem::path* destination,
                                    bool allow_root) {
  std::string source;
  if (!get_js_string(context, value, &source)) {
    return false;
  }
  std::replace(source.begin(), source.end(), '\\', '/');
  auto input = std::filesystem::path(source);
  if (input.is_absolute()) {
    input = input.relative_path();
  }
  std::filesystem::path relative;
  for (const auto& component : input) {
    auto text = component.string();
    if (text.empty() || text == ".") {
      continue;
    }
    if (text == "..") {
      JS_ThrowInternalError(context,
                            "EACCES: path escapes the JavaScript filesystem root");
      return false;
    }
    relative /= component;
  }
  if (relative.empty() && !allow_root) {
    JS_ThrowInternalError(context,
                          "EACCES: the JavaScript filesystem root is protected");
    return false;
  }
  *destination = host->filesystem_root / relative;
  return true;
}

static JSValue throw_filesystem_error(JSContext* context, const char* code,
                                      const std::filesystem::path& path,
                                      const std::error_code& error) {
  auto message = std::string(code) + ": " + path.string();
  if (error) {
    message.append(": ");
    message.append(error.message());
  }
  return JS_ThrowInternalError(context, "%s", message.c_str());
}

static JSValue js_post_message(JSContext* context, JSValueConst this_value,
                               int argument_count,
                               JSValueConst* arguments) {
  (void)this_value;
  auto* host = require_host(context);
  if (host == nullptr) {
    return JS_EXCEPTION;
  }
  if (argument_count != 1) {
    return JS_ThrowTypeError(context, "postMessage requires one argument");
  }
  std::string message;
  if (!get_js_string(context, arguments[0], &message)) {
    return JS_EXCEPTION;
  }
  if (!write_frame(host->file_descriptor, message)) {
    host->stop_requested->store(true);
    return JS_ThrowInternalError(context,
                                 "JavaScript protocol socket is unavailable");
  }
  return JS_UNDEFINED;
}

static JSValue js_schedule_timer(JSContext* context, JSValueConst this_value,
                                 int argument_count,
                                 JSValueConst* arguments) {
  (void)this_value;
  auto* host = require_host(context);
  if (host == nullptr) {
    return JS_EXCEPTION;
  }
  if (argument_count != 2) {
    return JS_ThrowTypeError(context,
                             "scheduleTimer requires an identifier and delay");
  }
  std::int64_t identifier = 0;
  if (JS_ToInt64(context, &identifier, arguments[0]) < 0) {
    return JS_EXCEPTION;
  }
  if (identifier <= 0) {
    return JS_ThrowRangeError(context,
                              "Timer identifier must be a positive integer");
  }
  std::int64_t delay = 0;
  if (JS_ToInt64(context, &delay, arguments[1]) < 0) {
    return JS_EXCEPTION;
  }
  if (delay < 0 || delay > kMaximumTimerMilliseconds) {
    return JS_ThrowRangeError(context,
                              "Timer delay must be between 0 and 60000 ms");
  }
  auto duplicate = std::find_if(
      host->timers.begin(), host->timers.end(),
      [identifier](const MuonTimer& timer) {
        return timer.identifier == identifier;
      });
  if (duplicate != host->timers.end()) {
    return JS_ThrowInternalError(context, "Timer identifier is already active");
  }
  host->timers.push_back(MuonTimer{
      identifier,
      std::chrono::steady_clock::now() + std::chrono::milliseconds(delay),
  });
  return JS_UNDEFINED;
}

static JSValue js_cancel_timer(JSContext* context, JSValueConst this_value,
                               int argument_count,
                               JSValueConst* arguments) {
  (void)this_value;
  auto* host = require_host(context);
  if (host == nullptr) {
    return JS_EXCEPTION;
  }
  if (argument_count != 1) {
    return JS_ThrowTypeError(context, "cancelTimer requires an identifier");
  }
  std::int64_t identifier = 0;
  if (JS_ToInt64(context, &identifier, arguments[0]) < 0) {
    return JS_EXCEPTION;
  }
  auto timer = std::find_if(
      host->timers.begin(), host->timers.end(),
      [identifier](const MuonTimer& candidate) {
        return candidate.identifier == identifier;
      });
  if (timer == host->timers.end()) {
    return JS_FALSE;
  }
  host->timers.erase(timer);
  return JS_TRUE;
}

static std::string socket_error_code(int error) {
  switch (error) {
    case EACCES:
      return "EACCES";
    case EADDRINUSE:
      return "EADDRINUSE";
    case EADDRNOTAVAIL:
      return "EADDRNOTAVAIL";
    case EAFNOSUPPORT:
      return "EAFNOSUPPORT";
    case ECONNABORTED:
      return "ECONNABORTED";
    case ECONNREFUSED:
      return "ECONNREFUSED";
    case ECONNRESET:
      return "ECONNRESET";
    case EHOSTUNREACH:
      return "EHOSTUNREACH";
    case EINTR:
      return "EINTR";
    case EINVAL:
      return "EINVAL";
    case EMFILE:
      return "EMFILE";
    case ENETUNREACH:
      return "ENETUNREACH";
    case ENOBUFS:
      return "ENOBUFS";
    case ENOMEM:
      return "ENOMEM";
    case ENOTCONN:
      return "ENOTCONN";
    case ETIMEDOUT:
      return "ETIMEDOUT";
    case EPIPE:
      return "EPIPE";
    default:
      return "EIO";
  }
}

static bool get_positive_identifier(JSContext* context, JSValueConst value,
                                    std::int64_t* destination) {
  std::int64_t identifier = 0;
  if (JS_ToInt64(context, &identifier, value) < 0) {
    return false;
  }
  if (identifier <= 0) {
    JS_ThrowRangeError(context, "Host operation identifier must be positive");
    return false;
  }
  *destination = identifier;
  return true;
}

static void queue_socket_error(MuonJavaScriptHost* host,
                               std::int64_t identifier, int error,
                               const char* syscall) {
  MuonHostEvent event;
  event.identifier = identifier;
  event.type = "error";
  event.code = socket_error_code(error);
  event.message = event.code + ": " + syscall + ": " + std::strerror(error);
  event.syscall = syscall;
  host->host_events.push_back(std::move(event));
}

static bool format_socket_address(const sockaddr_storage& storage,
                                  std::string* address, std::string* family,
                                  int* port) {
  char numeric[INET6_ADDRSTRLEN]{};
  if (storage.ss_family == AF_INET) {
    const auto* value = reinterpret_cast<const sockaddr_in*>(&storage);
    if (inet_ntop(AF_INET, &value->sin_addr, numeric, sizeof(numeric)) ==
        nullptr) {
      return false;
    }
    *address = numeric;
    *family = "IPv4";
    *port = ntohs(value->sin_port);
    return true;
  }
  if (storage.ss_family == AF_INET6) {
    const auto* value = reinterpret_cast<const sockaddr_in6*>(&storage);
    if (inet_ntop(AF_INET6, &value->sin6_addr, numeric, sizeof(numeric)) ==
        nullptr) {
      return false;
    }
    *address = numeric;
    *family = "IPv6";
    *port = ntohs(value->sin6_port);
    return true;
  }
  return false;
}

static void queue_socket_connected(MuonJavaScriptHost* host,
                                   MuonTcpSocket* socket) {
  MuonHostEvent event;
  event.identifier = socket->identifier;
  event.type = "connect";
  event.address = socket->remote_address;
  event.family = socket->remote_family;
  event.port = socket->remote_port;
  sockaddr_storage local{};
  socklen_t local_length = sizeof(local);
  if (getsockname(socket->file_descriptor,
                  reinterpret_cast<sockaddr*>(&local), &local_length) == 0) {
    format_socket_address(local, &event.local_address, &event.local_family,
                          &event.local_port);
  }
  host->host_events.push_back(std::move(event));
}

static JSValue js_is_ip(JSContext* context, JSValueConst this_value,
                        int argument_count, JSValueConst* arguments) {
  (void)this_value;
  if (argument_count != 1) {
    return JS_ThrowTypeError(context, "isIP requires one address");
  }
  std::string address;
  if (!get_js_string(context, arguments[0], &address)) {
    return JS_EXCEPTION;
  }
  in_addr ipv4{};
  if (inet_pton(AF_INET, address.c_str(), &ipv4) == 1) {
    return JS_NewInt32(context, 4);
  }
  in6_addr ipv6{};
  if (inet_pton(AF_INET6, address.c_str(), &ipv6) == 1) {
    return JS_NewInt32(context, 6);
  }
  return JS_NewInt32(context, 0);
}

static JSValue js_dns_lookup(JSContext* context, JSValueConst this_value,
                             int argument_count,
                             JSValueConst* arguments) {
  (void)this_value;
  auto* host = require_host(context);
  if (host == nullptr) {
    return JS_EXCEPTION;
  }
  if (argument_count != 3) {
    return JS_ThrowTypeError(
        context, "dnsLookup requires an identifier, hostname, and family");
  }
  std::int64_t identifier = 0;
  if (!get_positive_identifier(context, arguments[0], &identifier)) {
    return JS_EXCEPTION;
  }
  std::string hostname;
  if (!get_js_string(context, arguments[1], &hostname)) {
    return JS_EXCEPTION;
  }
  std::int32_t family = 0;
  if (JS_ToInt32(context, &family, arguments[2]) < 0) {
    return JS_EXCEPTION;
  }
  if (hostname.empty() || (family != 0 && family != 4 && family != 6)) {
    return JS_ThrowRangeError(context,
                              "dnsLookup hostname or family is invalid");
  }
  enqueue_dns_request(
      MuonDnsRequest{host->session, identifier, std::move(hostname), family});
  return JS_UNDEFINED;
}

static JSValue js_tcp_connect(JSContext* context, JSValueConst this_value,
                              int argument_count,
                              JSValueConst* arguments) {
  (void)this_value;
  auto* host = require_host(context);
  if (host == nullptr) {
    return JS_EXCEPTION;
  }
  if (argument_count != 4) {
    return JS_ThrowTypeError(
        context,
        "tcpConnect requires an identifier, address, port, and noDelay");
  }
  std::int64_t identifier = 0;
  if (!get_positive_identifier(context, arguments[0], &identifier)) {
    return JS_EXCEPTION;
  }
  if (host->tcp_sockets.contains(identifier)) {
    return JS_ThrowInternalError(context,
                                 "TCP socket identifier is already active");
  }
  std::string address;
  if (!get_js_string(context, arguments[1], &address)) {
    return JS_EXCEPTION;
  }
  std::int32_t port = 0;
  if (JS_ToInt32(context, &port, arguments[2]) < 0) {
    return JS_EXCEPTION;
  }
  if (port <= 0 || port > 65535) {
    return JS_ThrowRangeError(context, "TCP port must be between 1 and 65535");
  }
  auto no_delay = JS_ToBool(context, arguments[3]);
  if (no_delay < 0) {
    return JS_EXCEPTION;
  }

  sockaddr_storage destination{};
  socklen_t destination_length = 0;
  int domain = 0;
  std::string family;
  auto* ipv4 = reinterpret_cast<sockaddr_in*>(&destination);
  if (inet_pton(AF_INET, address.c_str(), &ipv4->sin_addr) == 1) {
    domain = AF_INET;
    family = "IPv4";
    ipv4->sin_family = AF_INET;
    ipv4->sin_port = htons(static_cast<std::uint16_t>(port));
    destination_length = sizeof(sockaddr_in);
  } else {
    auto* ipv6 = reinterpret_cast<sockaddr_in6*>(&destination);
    if (inet_pton(AF_INET6, address.c_str(), &ipv6->sin6_addr) != 1) {
      return JS_ThrowTypeError(context,
                               "tcpConnect address must be IPv4 or IPv6");
    }
    domain = AF_INET6;
    family = "IPv6";
    ipv6->sin6_family = AF_INET6;
    ipv6->sin6_port = htons(static_cast<std::uint16_t>(port));
    destination_length = sizeof(sockaddr_in6);
  }
  auto file_descriptor =
      socket(domain, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
  if (file_descriptor < 0) {
    queue_socket_error(host, identifier, errno, "socket");
    return JS_UNDEFINED;
  }
  if (no_delay != 0) {
    int enabled = 1;
    if (setsockopt(file_descriptor, IPPROTO_TCP, TCP_NODELAY, &enabled,
                   sizeof(enabled)) < 0) {
      auto error = errno;
      close(file_descriptor);
      queue_socket_error(host, identifier, error, "setsockopt");
      return JS_UNDEFINED;
    }
  }
  MuonTcpSocket socket_state;
  socket_state.identifier = identifier;
  socket_state.file_descriptor = file_descriptor;
  socket_state.remote_address = address;
  socket_state.remote_family = family;
  socket_state.remote_port = port;
  auto connected =
      connect(file_descriptor, reinterpret_cast<sockaddr*>(&destination),
              destination_length);
  if (connected < 0 && errno != EINPROGRESS) {
    auto error = errno;
    close(file_descriptor);
    queue_socket_error(host, identifier, error, "connect");
    return JS_UNDEFINED;
  }
  socket_state.connecting = connected < 0;
  auto [iterator, inserted] =
      host->tcp_sockets.emplace(identifier, std::move(socket_state));
  (void)inserted;
  if (!iterator->second.connecting) {
    queue_socket_connected(host, &iterator->second);
  }
  return JS_UNDEFINED;
}

static JSValue js_tcp_write(JSContext* context, JSValueConst this_value,
                            int argument_count, JSValueConst* arguments) {
  (void)this_value;
  auto* host = require_host(context);
  if (host == nullptr) {
    return JS_EXCEPTION;
  }
  if (argument_count != 3) {
    return JS_ThrowTypeError(
        context,
        "tcpWrite requires a socket, operation identifier, and ArrayBuffer");
  }
  std::int64_t identifier = 0;
  std::int64_t operation_identifier = 0;
  if (!get_positive_identifier(context, arguments[0], &identifier) ||
      !get_positive_identifier(context, arguments[1],
                               &operation_identifier)) {
    return JS_EXCEPTION;
  }
  auto socket = host->tcp_sockets.find(identifier);
  if (socket == host->tcp_sockets.end() || socket->second.end_requested) {
    return JS_FALSE;
  }
  std::size_t length = 0;
  auto* contents = JS_GetArrayBuffer(context, &length, arguments[2]);
  if (contents == nullptr) {
    return JS_ThrowTypeError(context, "tcpWrite data must be an ArrayBuffer");
  }
  MuonTcpWrite write;
  write.operation_identifier = operation_identifier;
  write.data.assign(contents, contents + length);
  socket->second.writes.push_back(std::move(write));
  return JS_TRUE;
}

static JSValue js_tcp_end(JSContext* context, JSValueConst this_value,
                          int argument_count, JSValueConst* arguments) {
  (void)this_value;
  auto* host = require_host(context);
  if (host == nullptr) {
    return JS_EXCEPTION;
  }
  if (argument_count != 1) {
    return JS_ThrowTypeError(context, "tcpEnd requires a socket identifier");
  }
  std::int64_t identifier = 0;
  if (!get_positive_identifier(context, arguments[0], &identifier)) {
    return JS_EXCEPTION;
  }
  auto socket = host->tcp_sockets.find(identifier);
  if (socket == host->tcp_sockets.end()) {
    return JS_FALSE;
  }
  socket->second.end_requested = true;
  return JS_TRUE;
}

static JSValue js_tcp_close(JSContext* context, JSValueConst this_value,
                            int argument_count, JSValueConst* arguments) {
  (void)this_value;
  auto* host = require_host(context);
  if (host == nullptr) {
    return JS_EXCEPTION;
  }
  if (argument_count != 1) {
    return JS_ThrowTypeError(context, "tcpClose requires a socket identifier");
  }
  std::int64_t identifier = 0;
  if (!get_positive_identifier(context, arguments[0], &identifier)) {
    return JS_EXCEPTION;
  }
  auto socket = host->tcp_sockets.find(identifier);
  if (socket == host->tcp_sockets.end()) {
    return JS_FALSE;
  }
  close(socket->second.file_descriptor);
  host->tcp_sockets.erase(socket);
  return JS_TRUE;
}

static JSValue js_tcp_set_paused(JSContext* context,
                                 JSValueConst this_value,
                                 int argument_count,
                                 JSValueConst* arguments) {
  (void)this_value;
  auto* host = require_host(context);
  if (host == nullptr) {
    return JS_EXCEPTION;
  }
  if (argument_count != 2) {
    return JS_ThrowTypeError(
        context, "tcpSetPaused requires a socket identifier and state");
  }
  std::int64_t identifier = 0;
  if (!get_positive_identifier(context, arguments[0], &identifier)) {
    return JS_EXCEPTION;
  }
  auto paused = JS_ToBool(context, arguments[1]);
  if (paused < 0) {
    return JS_EXCEPTION;
  }
  auto socket = host->tcp_sockets.find(identifier);
  if (socket == host->tcp_sockets.end()) {
    return JS_FALSE;
  }
  socket->second.paused = paused != 0;
  return JS_TRUE;
}

static JSValue js_tcp_set_no_delay(JSContext* context,
                                   JSValueConst this_value,
                                   int argument_count,
                                   JSValueConst* arguments) {
  (void)this_value;
  auto* host = require_host(context);
  if (host == nullptr) {
    return JS_EXCEPTION;
  }
  if (argument_count != 2) {
    return JS_ThrowTypeError(
        context, "tcpSetNoDelay requires a socket identifier and state");
  }
  std::int64_t identifier = 0;
  if (!get_positive_identifier(context, arguments[0], &identifier)) {
    return JS_EXCEPTION;
  }
  auto enabled = JS_ToBool(context, arguments[1]);
  if (enabled < 0) {
    return JS_EXCEPTION;
  }
  auto socket = host->tcp_sockets.find(identifier);
  if (socket == host->tcp_sockets.end()) {
    return JS_FALSE;
  }
  int option = enabled != 0 ? 1 : 0;
  if (setsockopt(socket->second.file_descriptor, IPPROTO_TCP, TCP_NODELAY,
                 &option, sizeof(option)) < 0) {
    return JS_FALSE;
  }
  return JS_TRUE;
}

static JSValue js_tcp_set_keep_alive(JSContext* context,
                                     JSValueConst this_value,
                                     int argument_count,
                                     JSValueConst* arguments) {
  (void)this_value;
  auto* host = require_host(context);
  if (host == nullptr) {
    return JS_EXCEPTION;
  }
  if (argument_count != 3) {
    return JS_ThrowTypeError(
        context,
        "tcpSetKeepAlive requires a socket identifier, state, and delay");
  }
  std::int64_t identifier = 0;
  if (!get_positive_identifier(context, arguments[0], &identifier)) {
    return JS_EXCEPTION;
  }
  auto enabled = JS_ToBool(context, arguments[1]);
  std::int32_t initial_delay = 0;
  if (enabled < 0 || JS_ToInt32(context, &initial_delay, arguments[2]) < 0) {
    return JS_EXCEPTION;
  }
  if (initial_delay < 0) {
    return JS_ThrowRangeError(context,
                              "TCP keepalive delay must not be negative");
  }
  auto socket = host->tcp_sockets.find(identifier);
  if (socket == host->tcp_sockets.end()) {
    return JS_FALSE;
  }
  int option = enabled != 0 ? 1 : 0;
  if (setsockopt(socket->second.file_descriptor, SOL_SOCKET, SO_KEEPALIVE,
                 &option, sizeof(option)) < 0) {
    return JS_FALSE;
  }
#ifdef TCP_KEEPIDLE
  auto seconds = initial_delay / 1000;
  if (enabled != 0 && seconds > 0 &&
      setsockopt(socket->second.file_descriptor, IPPROTO_TCP, TCP_KEEPIDLE,
                 &seconds, sizeof(seconds)) < 0) {
    return JS_FALSE;
  }
#endif
  return JS_TRUE;
}

static JSValue js_http_start(JSContext* context, JSValueConst this_value,
                             int argument_count, JSValueConst* arguments) {
  (void)this_value;
  auto* host = require_host(context);
  if (host == nullptr) {
    return JS_EXCEPTION;
  }
  if (argument_count != 8) {
    return JS_ThrowTypeError(
        context,
        "httpStart requires an identifier, method, URL, headers, body, "
        "timeouts, and certificate authority");
  }
  std::int64_t identifier = 0;
  if (!get_positive_identifier(context, arguments[0], &identifier)) {
    return JS_EXCEPTION;
  }
  std::string method;
  std::string url;
  std::string headers;
  std::string certificate_authority;
  if (!get_js_string(context, arguments[1], &method) ||
      !get_js_string(context, arguments[2], &url) ||
      !get_js_string(context, arguments[3], &headers) ||
      !get_js_string(context, arguments[7], &certificate_authority)) {
    return JS_EXCEPTION;
  }
  std::size_t body_length = 0;
  auto* body = JS_GetArrayBuffer(context, &body_length, arguments[4]);
  if (body == nullptr) {
    return JS_ThrowTypeError(context, "HTTP request body must be an ArrayBuffer");
  }
  if (body_length > kMaximumFrameLength) {
    return JS_ThrowRangeError(context, "HTTP request body exceeds the limit");
  }
  std::int32_t connect_timeout = 0;
  std::int32_t read_timeout = 0;
  if (JS_ToInt32(context, &connect_timeout, arguments[5]) < 0 ||
      JS_ToInt32(context, &read_timeout, arguments[6]) < 0) {
    return JS_EXCEPTION;
  }
  if (connect_timeout < 0 || read_timeout < 0 ||
      connect_timeout > kMaximumTimerMilliseconds ||
      read_timeout > kMaximumTimerMilliseconds) {
    return JS_ThrowRangeError(context,
                              "HTTP timeout must be between 0 and 60000ms");
  }

  bool attached = false;
  auto* environment = attach_java_environment(&attached);
  if (environment == nullptr || g_java_http_client_class == nullptr) {
    return JS_ThrowInternalError(context,
                                 "Android HTTP client bridge is unavailable");
  }
  auto runtime_id =
      environment->NewStringUTF(host->session->runtime_id.c_str());
  auto java_method = environment->NewStringUTF(method.c_str());
  auto java_url = environment->NewStringUTF(url.c_str());
  auto java_headers = environment->NewStringUTF(headers.c_str());
  auto java_certificate_authority =
      environment->NewStringUTF(certificate_authority.c_str());
  auto java_body =
      environment->NewByteArray(static_cast<jsize>(body_length));
  if (java_body != nullptr && body_length > 0) {
    environment->SetByteArrayRegion(
        java_body, 0, static_cast<jsize>(body_length),
        reinterpret_cast<const jbyte*>(body));
  }
  if (runtime_id != nullptr && java_method != nullptr && java_url != nullptr &&
      java_headers != nullptr && java_certificate_authority != nullptr &&
      java_body != nullptr && !environment->ExceptionCheck()) {
    environment->CallStaticVoidMethod(
        g_java_http_client_class, g_java_http_start_method, runtime_id,
        static_cast<jlong>(identifier), java_method, java_url, java_headers,
        java_body, static_cast<jint>(connect_timeout),
        static_cast<jint>(read_timeout), java_certificate_authority);
  }
  if (java_body != nullptr) {
    environment->DeleteLocalRef(java_body);
  }
  if (java_certificate_authority != nullptr) {
    environment->DeleteLocalRef(java_certificate_authority);
  }
  if (java_headers != nullptr) {
    environment->DeleteLocalRef(java_headers);
  }
  if (java_url != nullptr) {
    environment->DeleteLocalRef(java_url);
  }
  if (java_method != nullptr) {
    environment->DeleteLocalRef(java_method);
  }
  if (runtime_id != nullptr) {
    environment->DeleteLocalRef(runtime_id);
  }
  if (!finish_java_http_call(environment, attached)) {
    return JS_ThrowInternalError(context,
                                 "Unable to start the Android HTTP request");
  }
  return JS_UNDEFINED;
}

static JSValue call_java_http_boolean(JSContext* context,
                                      MuonJavaScriptHost* host,
                                      jmethodID method,
                                      std::int64_t identifier,
                                      bool include_state, bool state) {
  bool attached = false;
  auto* environment = attach_java_environment(&attached);
  if (environment == nullptr || g_java_http_client_class == nullptr) {
    return JS_ThrowInternalError(context,
                                 "Android HTTP client bridge is unavailable");
  }
  auto runtime_id =
      environment->NewStringUTF(host->session->runtime_id.c_str());
  jboolean result = JNI_FALSE;
  if (runtime_id != nullptr && !environment->ExceptionCheck()) {
    if (include_state) {
      result = environment->CallStaticBooleanMethod(
          g_java_http_client_class, method, runtime_id,
          static_cast<jlong>(identifier), state ? JNI_TRUE : JNI_FALSE);
    } else {
      result = environment->CallStaticBooleanMethod(
          g_java_http_client_class, method, runtime_id,
          static_cast<jlong>(identifier));
    }
  }
  if (runtime_id != nullptr) {
    environment->DeleteLocalRef(runtime_id);
  }
  if (!finish_java_http_call(environment, attached)) {
    return JS_ThrowInternalError(context,
                                 "Android HTTP client operation failed");
  }
  return JS_NewBool(context, result != JNI_FALSE);
}

static JSValue js_http_cancel(JSContext* context, JSValueConst this_value,
                              int argument_count, JSValueConst* arguments) {
  (void)this_value;
  auto* host = require_host(context);
  if (host == nullptr) {
    return JS_EXCEPTION;
  }
  if (argument_count != 1) {
    return JS_ThrowTypeError(context,
                             "httpCancel requires an identifier");
  }
  std::int64_t identifier = 0;
  if (!get_positive_identifier(context, arguments[0], &identifier)) {
    return JS_EXCEPTION;
  }
  return call_java_http_boolean(context, host, g_java_http_cancel_method,
                                identifier, false, false);
}

static JSValue js_http_handle_data(JSContext* context,
                                   JSValueConst this_value,
                                   int argument_count,
                                   JSValueConst* arguments) {
  (void)this_value;
  auto* host = require_host(context);
  if (host == nullptr) {
    return JS_EXCEPTION;
  }
  if (argument_count != 2) {
    return JS_ThrowTypeError(
        context, "httpHandleData requires an identifier and pause state");
  }
  std::int64_t identifier = 0;
  if (!get_positive_identifier(context, arguments[0], &identifier)) {
    return JS_EXCEPTION;
  }
  auto paused = JS_ToBool(context, arguments[1]);
  if (paused < 0) {
    return JS_EXCEPTION;
  }
  return call_java_http_boolean(context, host,
                                g_java_http_handle_data_method, identifier,
                                true, paused != 0);
}

static JSValue js_http_resume(JSContext* context, JSValueConst this_value,
                              int argument_count, JSValueConst* arguments) {
  (void)this_value;
  auto* host = require_host(context);
  if (host == nullptr) {
    return JS_EXCEPTION;
  }
  if (argument_count != 1) {
    return JS_ThrowTypeError(context,
                             "httpResume requires an identifier");
  }
  std::int64_t identifier = 0;
  if (!get_positive_identifier(context, arguments[0], &identifier)) {
    return JS_EXCEPTION;
  }
  return call_java_http_boolean(context, host, g_java_http_resume_method,
                                identifier, false, false);
}

static JSValue js_fs_read_text(JSContext* context, JSValueConst this_value,
                               int argument_count,
                               JSValueConst* arguments) {
  (void)this_value;
  auto* host = require_host(context);
  if (host == nullptr) {
    return JS_EXCEPTION;
  }
  if (argument_count != 1) {
    return JS_ThrowTypeError(context, "readFile requires one path");
  }
  std::filesystem::path path;
  if (!resolve_filesystem_path(context, host, arguments[0], &path, true)) {
    return JS_EXCEPTION;
  }
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return throw_filesystem_error(context, "ENOENT", path, {});
  }
  std::string contents((std::istreambuf_iterator<char>(stream)),
                       std::istreambuf_iterator<char>());
  if (!stream.good() && !stream.eof()) {
    return throw_filesystem_error(context, "EIO", path, {});
  }
  return JS_NewStringLen(context, contents.data(), contents.size());
}

static JSValue js_fs_read_buffer(JSContext* context, JSValueConst this_value,
                                 int argument_count,
                                 JSValueConst* arguments) {
  (void)this_value;
  auto* host = require_host(context);
  if (host == nullptr) {
    return JS_EXCEPTION;
  }
  if (argument_count != 1) {
    return JS_ThrowTypeError(context, "readFile requires one path");
  }
  std::filesystem::path path;
  if (!resolve_filesystem_path(context, host, arguments[0], &path, true)) {
    return JS_EXCEPTION;
  }
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) {
    return throw_filesystem_error(context, "ENOENT", path, {});
  }
  auto end = stream.tellg();
  if (end < 0 ||
      static_cast<std::uint64_t>(end) >
          static_cast<std::uint64_t>(kMaximumFrameLength)) {
    return throw_filesystem_error(context, "EFBIG", path, {});
  }
  auto size = static_cast<std::size_t>(end);
  std::vector<std::uint8_t> contents(size);
  stream.seekg(0);
  if (size > 0) {
    stream.read(reinterpret_cast<char*>(contents.data()),
                static_cast<std::streamsize>(size));
    if (!stream) {
      return throw_filesystem_error(context, "EIO", path, {});
    }
  }
  return JS_NewArrayBufferCopy(context, contents.data(), contents.size());
}

static JSValue write_file_contents(JSContext* context,
                                   MuonJavaScriptHost* host,
                                   JSValueConst path_value,
                                   const void* contents, std::size_t length) {
  std::filesystem::path path;
  if (!resolve_filesystem_path(context, host, path_value, &path, false)) {
    return JS_EXCEPTION;
  }
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    return throw_filesystem_error(context, "ENOENT", path, {});
  }
  if (length > 0) {
    stream.write(static_cast<const char*>(contents),
                 static_cast<std::streamsize>(length));
  }
  if (!stream) {
    return throw_filesystem_error(context, "EIO", path, {});
  }
  return JS_UNDEFINED;
}

static JSValue js_fs_write_text(JSContext* context, JSValueConst this_value,
                                int argument_count,
                                JSValueConst* arguments) {
  (void)this_value;
  auto* host = require_host(context);
  if (host == nullptr) {
    return JS_EXCEPTION;
  }
  if (argument_count != 2) {
    return JS_ThrowTypeError(context, "writeFile requires a path and data");
  }
  std::size_t length = 0;
  const char* contents = JS_ToCStringLen(context, &length, arguments[1]);
  if (contents == nullptr) {
    return JS_EXCEPTION;
  }
  auto result =
      write_file_contents(context, host, arguments[0], contents, length);
  JS_FreeCString(context, contents);
  return result;
}

static JSValue js_fs_write_buffer(JSContext* context,
                                  JSValueConst this_value,
                                  int argument_count,
                                  JSValueConst* arguments) {
  (void)this_value;
  auto* host = require_host(context);
  if (host == nullptr) {
    return JS_EXCEPTION;
  }
  if (argument_count != 2) {
    return JS_ThrowTypeError(context, "writeFile requires a path and data");
  }
  std::size_t length = 0;
  auto* contents = JS_GetArrayBuffer(context, &length, arguments[1]);
  if (contents == nullptr) {
    return JS_ThrowTypeError(context, "writeFile data must be an ArrayBuffer");
  }
  return write_file_contents(context, host, arguments[0], contents, length);
}

static JSValue js_fs_mkdir(JSContext* context, JSValueConst this_value,
                           int argument_count, JSValueConst* arguments) {
  (void)this_value;
  auto* host = require_host(context);
  if (host == nullptr) {
    return JS_EXCEPTION;
  }
  if (argument_count != 2) {
    return JS_ThrowTypeError(context, "mkdir requires a path and recursive flag");
  }
  std::filesystem::path path;
  if (!resolve_filesystem_path(context, host, arguments[0], &path, true)) {
    return JS_EXCEPTION;
  }
  auto recursive = JS_ToBool(context, arguments[1]);
  if (recursive < 0) {
    return JS_EXCEPTION;
  }
  std::error_code error;
  if (recursive != 0) {
    std::filesystem::create_directories(path, error);
  } else {
    std::filesystem::create_directory(path, error);
  }
  if (error && error != std::errc::file_exists) {
    return throw_filesystem_error(context, "EIO", path, error);
  }
  return JS_UNDEFINED;
}

static JSValue js_fs_readdir(JSContext* context, JSValueConst this_value,
                             int argument_count,
                             JSValueConst* arguments) {
  (void)this_value;
  auto* host = require_host(context);
  if (host == nullptr) {
    return JS_EXCEPTION;
  }
  if (argument_count != 1) {
    return JS_ThrowTypeError(context, "readdir requires one path");
  }
  std::filesystem::path path;
  if (!resolve_filesystem_path(context, host, arguments[0], &path, true)) {
    return JS_EXCEPTION;
  }
  std::error_code error;
  std::vector<std::string> entries;
  for (std::filesystem::directory_iterator iterator(path, error), end;
       !error && iterator != end; iterator.increment(error)) {
    entries.push_back(iterator->path().filename().string());
  }
  if (error) {
    return throw_filesystem_error(context, "ENOENT", path, error);
  }
  std::sort(entries.begin(), entries.end());
  auto array = JS_NewArray(context);
  for (std::size_t index = 0; index < entries.size(); ++index) {
    if (JS_SetPropertyUint32(context, array,
                             static_cast<std::uint32_t>(index),
                             JS_NewString(context, entries[index].c_str())) <
        0) {
      JS_FreeValue(context, array);
      return JS_EXCEPTION;
    }
  }
  return array;
}

static JSValue js_fs_stat(JSContext* context, JSValueConst this_value,
                          int argument_count, JSValueConst* arguments) {
  (void)this_value;
  auto* host = require_host(context);
  if (host == nullptr) {
    return JS_EXCEPTION;
  }
  if (argument_count != 1) {
    return JS_ThrowTypeError(context, "stat requires one path");
  }
  std::filesystem::path path;
  if (!resolve_filesystem_path(context, host, arguments[0], &path, true)) {
    return JS_EXCEPTION;
  }
  std::error_code error;
  auto status = std::filesystem::status(path, error);
  if (error || status.type() == std::filesystem::file_type::not_found) {
    return throw_filesystem_error(context, "ENOENT", path, error);
  }
  auto is_file = std::filesystem::is_regular_file(status);
  auto is_directory = std::filesystem::is_directory(status);
  std::uintmax_t size = 0;
  if (is_file) {
    size = std::filesystem::file_size(path, error);
    if (error) {
      return throw_filesystem_error(context, "EIO", path, error);
    }
  }
  auto result = JS_NewObject(context);
  JS_SetPropertyStr(context, result, "size",
                    JS_NewFloat64(context, static_cast<double>(size)));
  JS_SetPropertyStr(context, result, "isFile", JS_NewBool(context, is_file));
  JS_SetPropertyStr(context, result, "isDirectory",
                    JS_NewBool(context, is_directory));
  return result;
}

static JSValue js_fs_access(JSContext* context, JSValueConst this_value,
                            int argument_count,
                            JSValueConst* arguments) {
  (void)this_value;
  auto* host = require_host(context);
  if (host == nullptr) {
    return JS_EXCEPTION;
  }
  if (argument_count != 1) {
    return JS_ThrowTypeError(context, "access requires one path");
  }
  std::filesystem::path path;
  if (!resolve_filesystem_path(context, host, arguments[0], &path, true)) {
    return JS_EXCEPTION;
  }
  std::error_code error;
  auto exists = std::filesystem::exists(path, error);
  if (error || !exists) {
    return throw_filesystem_error(context, "ENOENT", path, error);
  }
  return JS_UNDEFINED;
}

static JSValue js_fs_rename(JSContext* context, JSValueConst this_value,
                            int argument_count,
                            JSValueConst* arguments) {
  (void)this_value;
  auto* host = require_host(context);
  if (host == nullptr) {
    return JS_EXCEPTION;
  }
  if (argument_count != 2) {
    return JS_ThrowTypeError(context, "rename requires two paths");
  }
  std::filesystem::path source;
  std::filesystem::path destination;
  if (!resolve_filesystem_path(context, host, arguments[0], &source, false) ||
      !resolve_filesystem_path(context, host, arguments[1], &destination,
                               false)) {
    return JS_EXCEPTION;
  }
  std::error_code error;
  std::filesystem::rename(source, destination, error);
  if (error) {
    return throw_filesystem_error(context, "EIO", source, error);
  }
  return JS_UNDEFINED;
}

static JSValue js_fs_unlink(JSContext* context, JSValueConst this_value,
                            int argument_count,
                            JSValueConst* arguments) {
  (void)this_value;
  auto* host = require_host(context);
  if (host == nullptr) {
    return JS_EXCEPTION;
  }
  if (argument_count != 1) {
    return JS_ThrowTypeError(context, "unlink requires one path");
  }
  std::filesystem::path path;
  if (!resolve_filesystem_path(context, host, arguments[0], &path, false)) {
    return JS_EXCEPTION;
  }
  std::error_code error;
  auto removed = std::filesystem::remove(path, error);
  if (error || !removed) {
    return throw_filesystem_error(context, "ENOENT", path, error);
  }
  return JS_UNDEFINED;
}

static JSValue js_fs_rm(JSContext* context, JSValueConst this_value,
                        int argument_count, JSValueConst* arguments) {
  (void)this_value;
  auto* host = require_host(context);
  if (host == nullptr) {
    return JS_EXCEPTION;
  }
  if (argument_count != 3) {
    return JS_ThrowTypeError(context,
                             "rm requires a path, recursive flag, and force flag");
  }
  std::filesystem::path path;
  if (!resolve_filesystem_path(context, host, arguments[0], &path, false)) {
    return JS_EXCEPTION;
  }
  auto recursive = JS_ToBool(context, arguments[1]);
  auto force = JS_ToBool(context, arguments[2]);
  if (recursive < 0 || force < 0) {
    return JS_EXCEPTION;
  }
  std::error_code error;
  std::uintmax_t removed = recursive != 0
                               ? std::filesystem::remove_all(path, error)
                               : (std::filesystem::remove(path, error) ? 1U : 0U);
  if (error || (removed == 0 && force == 0)) {
    return throw_filesystem_error(context, "ENOENT", path, error);
  }
  return JS_UNDEFINED;
}

static bool install_host_function(JSContext* context, JSValue global,
                                  const char* name, JSCFunction* function,
                                  int argument_count) {
  auto value = JS_NewCFunction(context, function, name, argument_count);
  if (JS_IsException(value)) {
    return false;
  }
  return JS_SetPropertyStr(context, global, name, value) >= 0;
}

static bool install_host_functions(MuonJavaScriptHost* host) {
  auto global = JS_GetGlobalObject(host->context);
  auto installed =
      install_host_function(host->context, global, "__muonPostMessage",
                            js_post_message, 1) &&
      install_host_function(host->context, global, "__muonScheduleTimer",
                            js_schedule_timer, 2) &&
      install_host_function(host->context, global, "__muonCancelTimer",
                            js_cancel_timer, 1) &&
      install_host_function(host->context, global, "__muonIsIp", js_is_ip,
                            1) &&
      install_host_function(host->context, global, "__muonDnsLookup",
                            js_dns_lookup, 3) &&
      install_host_function(host->context, global, "__muonTcpConnect",
                            js_tcp_connect, 4) &&
      install_host_function(host->context, global, "__muonTcpWrite",
                            js_tcp_write, 3) &&
      install_host_function(host->context, global, "__muonTcpEnd", js_tcp_end,
                            1) &&
      install_host_function(host->context, global, "__muonTcpClose",
                            js_tcp_close, 1) &&
      install_host_function(host->context, global, "__muonTcpSetPaused",
                            js_tcp_set_paused, 2) &&
      install_host_function(host->context, global, "__muonTcpSetNoDelay",
                            js_tcp_set_no_delay, 2) &&
      install_host_function(host->context, global, "__muonTcpSetKeepAlive",
                            js_tcp_set_keep_alive, 3) &&
      install_host_function(host->context, global, "__muonHttpStart",
                            js_http_start, 8) &&
      install_host_function(host->context, global, "__muonHttpCancel",
                            js_http_cancel, 1) &&
      install_host_function(host->context, global, "__muonHttpHandleData",
                            js_http_handle_data, 2) &&
      install_host_function(host->context, global, "__muonHttpResume",
                            js_http_resume, 1) &&
      install_host_function(host->context, global, "__muonFsReadText",
                            js_fs_read_text, 1) &&
      install_host_function(host->context, global, "__muonFsReadBuffer",
                            js_fs_read_buffer, 1) &&
      install_host_function(host->context, global, "__muonFsWriteText",
                            js_fs_write_text, 2) &&
      install_host_function(host->context, global, "__muonFsWriteBuffer",
                            js_fs_write_buffer, 2) &&
      install_host_function(host->context, global, "__muonFsMkdir",
                            js_fs_mkdir, 2) &&
      install_host_function(host->context, global, "__muonFsReaddir",
                            js_fs_readdir, 1) &&
      install_host_function(host->context, global, "__muonFsStat", js_fs_stat,
                            1) &&
      install_host_function(host->context, global, "__muonFsAccess",
                            js_fs_access, 1) &&
      install_host_function(host->context, global, "__muonFsRename",
                            js_fs_rename, 2) &&
      install_host_function(host->context, global, "__muonFsUnlink",
                            js_fs_unlink, 1) &&
      install_host_function(host->context, global, "__muonFsRm", js_fs_rm,
                            3);
  JS_FreeValue(host->context, global);
  return installed;
}

static int interrupt_handler(JSRuntime* runtime, void* opaque) {
  (void)runtime;
  auto* host = static_cast<MuonJavaScriptHost*>(opaque);
  if (host->stop_requested->load()) {
    return 1;
  }
  auto deadline = host->execution_deadline_nanoseconds.load();
  return deadline != 0 && steady_nanoseconds() >= deadline ? 1 : 0;
}

static void begin_js_execution(MuonJavaScriptHost* host) {
  host->execution_deadline_nanoseconds.store(
      steady_nanoseconds() +
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::milliseconds(kExecutionLimitMilliseconds))
          .count());
}

static void end_js_execution(MuonJavaScriptHost* host) {
  host->execution_deadline_nanoseconds.store(0);
}

static JSValue create_host_event_payload(JSContext* context,
                                         const MuonHostEvent& event) {
  if (event.type == "data" || event.type == "httpData") {
    return JS_NewArrayBufferCopy(context, event.data.data(), event.data.size());
  }
  if (event.type == "httpResponse" || event.type == "httpError") {
    return JS_ParseJSON(context, event.payload.data(), event.payload.size(),
                        "<android-http-event>");
  }
  if (event.type == "write") {
    return JS_NewInt64(context, event.operation_identifier);
  }
  if (event.type == "end" || event.type == "httpEnd") {
    return JS_UNDEFINED;
  }
  auto payload = JS_NewObject(context);
  if (event.type == "dns") {
    JS_SetPropertyStr(context, payload, "hostname",
                      JS_NewString(context, event.hostname.c_str()));
    auto addresses = JS_NewArray(context);
    for (std::size_t index = 0; index < event.addresses.size(); ++index) {
      auto address = JS_NewObject(context);
      JS_SetPropertyStr(
          context, address, "address",
          JS_NewString(context, event.addresses[index].address.c_str()));
      JS_SetPropertyStr(context, address, "family",
                        JS_NewInt32(context, event.addresses[index].family));
      JS_SetPropertyUint32(context, addresses,
                           static_cast<std::uint32_t>(index), address);
    }
    JS_SetPropertyStr(context, payload, "addresses", addresses);
    return payload;
  }
  if (event.type == "connect") {
    JS_SetPropertyStr(context, payload, "address",
                      JS_NewString(context, event.address.c_str()));
    JS_SetPropertyStr(context, payload, "family",
                      JS_NewString(context, event.family.c_str()));
    JS_SetPropertyStr(context, payload, "port",
                      JS_NewInt32(context, event.port));
    JS_SetPropertyStr(context, payload, "localAddress",
                      JS_NewString(context, event.local_address.c_str()));
    JS_SetPropertyStr(context, payload, "localFamily",
                      JS_NewString(context, event.local_family.c_str()));
    JS_SetPropertyStr(context, payload, "localPort",
                      JS_NewInt32(context, event.local_port));
    return payload;
  }
  JS_SetPropertyStr(context, payload, "code",
                    JS_NewString(context, event.code.c_str()));
  JS_SetPropertyStr(context, payload, "message",
                    JS_NewString(context, event.message.c_str()));
  JS_SetPropertyStr(context, payload, "syscall",
                    JS_NewString(context, event.syscall.c_str()));
  JS_SetPropertyStr(context, payload, "hostname",
                    JS_NewString(context, event.hostname.c_str()));
  return payload;
}

static bool dispatch_host_events(MuonJavaScriptHost* host) {
  while (!host->host_events.empty()) {
    auto events = std::move(host->host_events);
    host->host_events.clear();
    for (const auto& event : events) {
      auto global = JS_GetGlobalObject(host->context);
      auto dispatcher =
          JS_GetPropertyStr(host->context, global, "__muonDispatchHostEvent");
      JS_FreeValue(host->context, global);
      if (!JS_IsFunction(host->context, dispatcher)) {
        JS_FreeValue(host->context, dispatcher);
        log_error("QuickJS host event dispatcher is unavailable");
        return false;
      }
      JSValue arguments[] = {
          JS_NewInt64(host->context, event.identifier),
          JS_NewString(host->context, event.type.c_str()),
          create_host_event_payload(host->context, event),
      };
      if (JS_IsException(arguments[2])) {
        JS_FreeValue(host->context, arguments[0]);
        JS_FreeValue(host->context, arguments[1]);
        JS_FreeValue(host->context, dispatcher);
        log_error("Unable to decode a QuickJS host event: " +
                  take_exception(host->context));
        return false;
      }
      begin_js_execution(host);
      auto result = JS_Call(host->context, dispatcher, JS_UNDEFINED, 3,
                            arguments);
      end_js_execution(host);
      for (auto& argument : arguments) {
        JS_FreeValue(host->context, argument);
      }
      JS_FreeValue(host->context, dispatcher);
      if (JS_IsException(result)) {
        log_error("QuickJS host event failed: " +
                  take_exception(host->context));
        return false;
      }
      JS_FreeValue(host->context, result);
    }
  }
  return true;
}

static void drain_event_file_descriptor(int file_descriptor) {
  std::uint64_t count = 0;
  while (read(file_descriptor, &count, sizeof(count)) < 0) {
    if (errno == EINTR) {
      continue;
    }
    break;
  }
}

static void collect_dns_results(MuonJavaScriptHost* host) {
  std::deque<MuonDnsResult> results;
  {
    std::lock_guard<std::mutex> lock(host->session->dns_result_mutex);
    results.swap(host->session->dns_results);
  }
  for (auto& result : results) {
    MuonHostEvent event;
    event.identifier = result.identifier;
    event.hostname = std::move(result.hostname);
    if (result.code.empty()) {
      event.type = "dns";
      event.addresses = std::move(result.addresses);
    } else {
      event.type = "error";
      event.code = std::move(result.code);
      event.message = std::move(result.message);
      event.syscall = "getaddrinfo";
    }
    host->host_events.push_back(std::move(event));
  }
}

static void collect_external_events(MuonJavaScriptHost* host) {
  std::deque<MuonHostEvent> events;
  {
    std::lock_guard<std::mutex> lock(host->session->external_event_mutex);
    events.swap(host->session->external_events);
  }
  while (!events.empty()) {
    host->host_events.push_back(std::move(events.front()));
    events.pop_front();
  }
}

static bool flush_tcp_writes(MuonJavaScriptHost* host,
                             MuonTcpSocket* socket) {
  while (!socket->writes.empty()) {
    auto& write = socket->writes.front();
    auto remaining = write.data.size() - write.offset;
    auto sent = send(socket->file_descriptor, write.data.data() + write.offset,
                     remaining, MSG_NOSIGNAL);
    if (sent < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return true;
      }
      queue_socket_error(host, socket->identifier, errno, "write");
      return false;
    }
    if (sent == 0 && remaining > 0) {
      queue_socket_error(host, socket->identifier, EPIPE, "write");
      return false;
    }
    write.offset += static_cast<std::size_t>(sent);
    socket->bytes_written += static_cast<std::uint64_t>(sent);
    if (write.offset == write.data.size()) {
      MuonHostEvent event;
      event.identifier = socket->identifier;
      event.type = "write";
      event.operation_identifier = write.operation_identifier;
      host->host_events.push_back(std::move(event));
      socket->writes.pop_front();
    }
  }
  return true;
}

static bool finish_tcp_output(MuonJavaScriptHost* host,
                              MuonTcpSocket* socket) {
  if (!socket->connecting && socket->end_requested &&
      socket->writes.empty() && !socket->write_ended) {
    if (shutdown(socket->file_descriptor, SHUT_WR) < 0 && errno != ENOTCONN) {
      queue_socket_error(host, socket->identifier, errno, "shutdown");
      return false;
    }
    socket->write_ended = true;
  }
  return !(socket->read_ended && socket->write_ended);
}

static bool read_tcp_input(MuonJavaScriptHost* host, MuonTcpSocket* socket) {
  std::vector<std::uint8_t> buffer(kNetworkReadBufferSize);
  while (true) {
    auto received = recv(socket->file_descriptor, buffer.data(), buffer.size(),
                         0);
    if (received > 0) {
      MuonHostEvent event;
      event.identifier = socket->identifier;
      event.type = "data";
      event.data.assign(buffer.begin(), buffer.begin() + received);
      host->host_events.push_back(std::move(event));
      socket->bytes_read += static_cast<std::uint64_t>(received);
      continue;
    }
    if (received == 0) {
      socket->read_ended = true;
      MuonHostEvent event;
      event.identifier = socket->identifier;
      event.type = "end";
      host->host_events.push_back(std::move(event));
      return !(socket->write_ended && socket->writes.empty());
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return true;
    }
    queue_socket_error(host, socket->identifier, errno, "read");
    return false;
  }
}

static bool process_tcp_poll_event(MuonJavaScriptHost* host,
                                   std::int64_t identifier, short revents) {
  auto iterator = host->tcp_sockets.find(identifier);
  if (iterator == host->tcp_sockets.end()) {
    return true;
  }
  auto* socket = &iterator->second;
  if ((revents & POLLNVAL) != 0) {
    queue_socket_error(host, identifier, EBADF, "poll");
    close(socket->file_descriptor);
    host->tcp_sockets.erase(iterator);
    return true;
  }
  if (socket->connecting &&
      (revents & (POLLOUT | POLLERR | POLLHUP)) != 0) {
    int error = 0;
    socklen_t error_length = sizeof(error);
    if (getsockopt(socket->file_descriptor, SOL_SOCKET, SO_ERROR, &error,
                   &error_length) < 0) {
      error = errno;
    }
    if (error != 0) {
      queue_socket_error(host, identifier, error, "connect");
      close(socket->file_descriptor);
      host->tcp_sockets.erase(iterator);
      return true;
    }
    socket->connecting = false;
    queue_socket_connected(host, socket);
  }
  if (!socket->connecting && (revents & POLLERR) != 0) {
    int error = 0;
    socklen_t error_length = sizeof(error);
    if (getsockopt(socket->file_descriptor, SOL_SOCKET, SO_ERROR, &error,
                   &error_length) < 0 || error == 0) {
      error = errno == 0 ? EIO : errno;
    }
    queue_socket_error(host, identifier, error, "socket");
    close(socket->file_descriptor);
    host->tcp_sockets.erase(iterator);
    return true;
  }
  if (!socket->connecting && (revents & POLLOUT) != 0 &&
      !flush_tcp_writes(host, socket)) {
    close(socket->file_descriptor);
    host->tcp_sockets.erase(iterator);
    return true;
  }
  if (!socket->connecting && !socket->paused && !socket->read_ended &&
      (revents & (POLLIN | POLLHUP)) != 0 &&
      !read_tcp_input(host, socket)) {
    close(socket->file_descriptor);
    host->tcp_sockets.erase(iterator);
    return true;
  }
  if (!finish_tcp_output(host, socket)) {
    close(socket->file_descriptor);
    host->tcp_sockets.erase(iterator);
  }
  return true;
}

static void append_tcp_poll_descriptors(
    const MuonJavaScriptHost* host, std::vector<pollfd>* descriptors,
    std::vector<std::int64_t>* identifiers) {
  for (const auto& [identifier, socket] : host->tcp_sockets) {
    short events = 0;
    if (socket.connecting || !socket.writes.empty()) {
      events |= POLLOUT;
    }
    if (!socket.connecting && !socket.paused && !socket.read_ended) {
      events |= POLLIN;
    }
    if (events == 0) {
      continue;
    }
    pollfd descriptor{};
    descriptor.fd = socket.file_descriptor;
    descriptor.events = events;
    descriptors->push_back(descriptor);
    identifiers->push_back(identifier);
  }
}

static void process_tcp_deferred_output(MuonJavaScriptHost* host) {
  auto iterator = host->tcp_sockets.begin();
  while (iterator != host->tcp_sockets.end()) {
    auto* socket = &iterator->second;
    if (!finish_tcp_output(host, socket)) {
      close(socket->file_descriptor);
      iterator = host->tcp_sockets.erase(iterator);
    } else {
      ++iterator;
    }
  }
}

static JSValue evaluate_source(MuonJavaScriptHost* host,
                               const std::string& source,
                               const char* filename, int flags) {
  begin_js_execution(host);
  auto result = JS_Eval(host->context, source.data(), source.size(), filename,
                        flags);
  end_js_execution(host);
  return result;
}

static bool execute_pending_jobs(MuonJavaScriptHost* host) {
  while (JS_IsJobPending(host->runtime)) {
    JSContext* job_context = nullptr;
    begin_js_execution(host);
    auto result = JS_ExecutePendingJob(host->runtime, &job_context);
    end_js_execution(host);
    if (result < 0) {
      log_error("QuickJS job failed: " + take_exception(
                                             job_context == nullptr
                                                 ? host->context
                                                 : job_context));
      return false;
    }
  }
  return true;
}

static bool settle_initial_promise(MuonJavaScriptHost* host, JSValue promise) {
  while (JS_PromiseState(host->context, promise) == JS_PROMISE_PENDING) {
    if (!execute_pending_jobs(host) || !JS_IsJobPending(host->runtime)) {
      break;
    }
  }
  auto state = JS_PromiseState(host->context, promise);
  if (state == JS_PROMISE_FULFILLED) {
    return true;
  }
  if (state == JS_PROMISE_REJECTED) {
    auto reason = JS_PromiseResult(host->context, promise);
    std::string diagnostic;
    if (!get_js_string(host->context, reason, &diagnostic)) {
      diagnostic = "QuickJS module evaluation rejected";
    }
    JS_FreeValue(host->context, reason);
    log_error(diagnostic);
  } else {
    log_error("QuickJS module evaluation did not settle");
  }
  return false;
}

static bool evaluate_runtime_sources(MuonJavaScriptHost* host,
                                     const std::string& runtime_source,
                                     const std::string& backend_source) {
  auto runtime_result = evaluate_source(host, runtime_source,
                                        "muon-javascript/runtime.js",
                                        JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(runtime_result)) {
    log_error("Unable to evaluate runtime.js: " +
              take_exception(host->context));
    return false;
  }
  JS_FreeValue(host->context, runtime_result);

  JS_SetModuleLoaderFunc(host->runtime, nullptr, load_host_module, host);

  auto module_result = evaluate_source(host, backend_source,
                                       "muon-javascript/backend.mjs",
                                       JS_EVAL_TYPE_MODULE);
  if (JS_IsException(module_result)) {
    log_error("Unable to evaluate backend.mjs: " +
              take_exception(host->context));
    return false;
  }
  auto settled = settle_initial_promise(host, module_result);
  JS_FreeValue(host->context, module_result);
  return settled;
}

static bool handle_protocol_message(MuonJavaScriptHost* host,
                                    const std::string& message) {
  auto global = JS_GetGlobalObject(host->context);
  auto handler =
      JS_GetPropertyStr(host->context, global, "__muonHandleMessage");
  JS_FreeValue(host->context, global);
  if (!JS_IsFunction(host->context, handler)) {
    JS_FreeValue(host->context, handler);
    log_error("QuickJS protocol handler is unavailable");
    return false;
  }
  auto argument =
      JS_NewStringLen(host->context, message.data(), message.size());
  begin_js_execution(host);
  auto result =
      JS_Call(host->context, handler, JS_UNDEFINED, 1, &argument);
  end_js_execution(host);
  JS_FreeValue(host->context, argument);
  JS_FreeValue(host->context, handler);
  if (JS_IsException(result)) {
    log_error("QuickJS protocol handler failed: " +
              take_exception(host->context));
    return false;
  }
  host->pending_promises.push_back(MuonPendingPromise{result});
  return true;
}

static bool process_timers(MuonJavaScriptHost* host) {
  while (true) {
    auto now = std::chrono::steady_clock::now();
    auto iterator = std::find_if(
        host->timers.begin(), host->timers.end(),
        [now](const MuonTimer& timer) { return timer.deadline <= now; });
    if (iterator == host->timers.end()) {
      return true;
    }

    auto identifier = iterator->identifier;
    host->timers.erase(iterator);
    auto global = JS_GetGlobalObject(host->context);
    auto dispatcher =
        JS_GetPropertyStr(host->context, global, "__muonDispatchTimer");
    JS_FreeValue(host->context, global);
    if (!JS_IsFunction(host->context, dispatcher)) {
      JS_FreeValue(host->context, dispatcher);
      log_error("QuickJS timer dispatcher is unavailable");
      return false;
    }
    auto argument = JS_NewInt64(host->context, identifier);
    begin_js_execution(host);
    auto result = JS_Call(host->context, dispatcher, JS_UNDEFINED, 1, &argument);
    end_js_execution(host);
    JS_FreeValue(host->context, argument);
    JS_FreeValue(host->context, dispatcher);
    if (JS_IsException(result)) {
      log_error("QuickJS timer callback failed: " +
                take_exception(host->context));
      return false;
    }
    JS_FreeValue(host->context, result);
    if (!execute_pending_jobs(host)) {
      return false;
    }

    // The callback and its promise jobs may add or cancel timers and reallocate
    // the vector. Restart the search after JavaScript execution.
  }
}

static bool process_protocol_promises(MuonJavaScriptHost* host) {
  if (!execute_pending_jobs(host)) {
    return false;
  }
  auto iterator = host->pending_promises.begin();
  while (iterator != host->pending_promises.end()) {
    auto state = JS_PromiseState(host->context, iterator->promise);
    if (state == JS_PROMISE_PENDING) {
      ++iterator;
      continue;
    }
    auto result = JS_PromiseResult(host->context, iterator->promise);
    if (state == JS_PROMISE_REJECTED) {
      std::string diagnostic;
      if (!get_js_string(host->context, result, &diagnostic)) {
        diagnostic = "QuickJS protocol promise rejected";
      }
      JS_FreeValue(host->context, result);
      JS_FreeValue(host->context, iterator->promise);
      host->pending_promises.erase(iterator);
      log_error(diagnostic);
      return false;
    }
    if (!JS_IsNull(result) && !JS_IsUndefined(result)) {
      std::string response;
      if (!get_js_string(host->context, result, &response) ||
          !write_frame(host->file_descriptor, response)) {
        JS_FreeValue(host->context, result);
        JS_FreeValue(host->context, iterator->promise);
        host->pending_promises.erase(iterator);
        return false;
      }
    }
    JS_FreeValue(host->context, result);
    JS_FreeValue(host->context, iterator->promise);
    iterator = host->pending_promises.erase(iterator);
  }
  return true;
}

static bool should_shutdown(MuonJavaScriptHost* host) {
  auto global = JS_GetGlobalObject(host->context);
  auto value =
      JS_GetPropertyStr(host->context, global, "__muonShouldShutdown");
  JS_FreeValue(host->context, global);
  auto result = JS_ToBool(host->context, value);
  JS_FreeValue(host->context, value);
  return result > 0 && host->pending_promises.empty();
}

static int timer_poll_timeout(const MuonJavaScriptHost* host) {
  if (!host->host_events.empty()) {
    return 0;
  }
  if (host->timers.empty()) {
    return -1;
  }
  auto earliest = std::min_element(
      host->timers.begin(), host->timers.end(),
      [](const MuonTimer& left, const MuonTimer& right) {
        return left.deadline < right.deadline;
      });
  auto remaining = earliest->deadline - std::chrono::steady_clock::now();
  if (remaining <= std::chrono::steady_clock::duration::zero()) {
    return 0;
  }
  auto milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
  if (milliseconds >= std::numeric_limits<int>::max()) {
    return std::numeric_limits<int>::max();
  }
  return static_cast<int>(milliseconds + 1);
}

static void cancel_java_http_runtime(MuonJavaScriptHost* host) {
  if (g_java_http_client_class == nullptr || host->session == nullptr) {
    return;
  }
  bool attached = false;
  auto* environment = attach_java_environment(&attached);
  if (environment == nullptr) {
    return;
  }
  auto runtime_id =
      environment->NewStringUTF(host->session->runtime_id.c_str());
  if (runtime_id != nullptr && !environment->ExceptionCheck()) {
    environment->CallStaticVoidMethod(g_java_http_client_class,
                                      g_java_http_cancel_runtime_method,
                                      runtime_id);
  }
  if (runtime_id != nullptr) {
    environment->DeleteLocalRef(runtime_id);
  }
  finish_java_http_call(environment, attached);
}

static void free_host(MuonJavaScriptHost* host) {
  cancel_java_http_runtime(host);
  for (auto& [identifier, socket] : host->tcp_sockets) {
    (void)identifier;
    close(socket.file_descriptor);
  }
  host->tcp_sockets.clear();
  if (host->context != nullptr) {
    for (auto& pending : host->pending_promises) {
      JS_FreeValue(host->context, pending.promise);
    }
    JS_FreeContext(host->context);
    host->context = nullptr;
  }
  if (host->runtime != nullptr) {
    JS_FreeRuntime(host->runtime);
    host->runtime = nullptr;
  }
}

static void close_session_descriptors(
    const std::shared_ptr<MuonJavaScriptSession>& session) {
  {
    std::lock_guard<std::mutex> lock(session->dns_result_mutex);
    session->accepts_dns_results = false;
    session->dns_results.clear();
  }
  {
    std::lock_guard<std::mutex> lock(session->external_event_mutex);
    session->accepts_external_events = false;
    session->external_events.clear();
  }
  std::lock_guard<std::mutex> lock(session->descriptor_mutex);
  if (session->file_descriptor >= 0) {
    close(session->file_descriptor);
    session->file_descriptor = -1;
  }
  if (session->control_file_descriptor >= 0) {
    close(session->control_file_descriptor);
    session->control_file_descriptor = -1;
  }
  if (session->event_file_descriptor >= 0) {
    close(session->event_file_descriptor);
    session->event_file_descriptor = -1;
  }
}

static void stop_session(
    const std::shared_ptr<MuonJavaScriptSession>& session) {
  session->stop_requested.store(true);
  std::lock_guard<std::mutex> lock(session->descriptor_mutex);
  if (session->control_file_descriptor >= 0) {
    shutdown(session->control_file_descriptor, SHUT_RDWR);
  }
}

static void remove_session(
    const std::shared_ptr<MuonJavaScriptSession>& session) {
  {
    std::lock_guard<std::mutex> lock(g_sessions_mutex);
    auto iterator = g_sessions.find(session->runtime_id);
    if (iterator != g_sessions.end() && iterator->second == session) {
      g_sessions.erase(iterator);
    }
  }
  g_sessions_changed.notify_all();
}

static void run_session(const std::shared_ptr<MuonJavaScriptSession>& session,
                        std::string runtime_source,
                        std::string backend_source,
                        std::string filesystem_root) {
  MuonJavaScriptHost host;
  host.file_descriptor = session->file_descriptor;
  host.filesystem_root = std::filesystem::path(filesystem_root);
  host.session = session;
  host.stop_requested = &session->stop_requested;
  host.runtime = JS_NewRuntime();
  if (host.runtime == nullptr) {
    log_error("Unable to create a QuickJS runtime");
  } else {
    JS_SetMemoryLimit(host.runtime, kMemoryLimit);
    JS_SetMaxStackSize(host.runtime, kStackLimit);
    JS_SetInterruptHandler(host.runtime, interrupt_handler, &host);
    host.context = JS_NewContext(host.runtime);
    if (host.context == nullptr) {
      log_error("Unable to create a QuickJS context");
    } else {
      JS_SetContextOpaque(host.context, &host);
      auto initialized = install_host_functions(&host) &&
                         evaluate_runtime_sources(&host, runtime_source,
                                                  backend_source);
      if (initialized) {
        const std::string handshake =
            "{\"kind\":\"handshake\",\"protocol\":\"muon-js/1\","
            "\"engine\":{\"name\":\"quickjs\",\"version\":\"" +
            std::string(kQuickJsVersion) +
            "\"},\"capabilities\":[\"esm\",\"multiple-runtimes\","
            "\"callbacks\",\"node:fs/promises\",\"node:fs\","
            "\"node:path\",\"node:events\",\"node:buffer\","
            "\"node:timers\",\"node:timers/promises\",\"node:stream\","
            "\"node:url\",\"node:dns\",\"node:net\",\"tcp\","
            "\"node:http\",\"fetch\",\"abort\"]}";
        initialized = write_frame(host.file_descriptor, handshake);
      }

      while (initialized && !session->stop_requested.load()) {
        collect_dns_results(&host);
        collect_external_events(&host);
        if (!dispatch_host_events(&host) || !process_timers(&host) ||
            !process_protocol_promises(&host)) {
          break;
        }
        process_tcp_deferred_output(&host);
        if (should_shutdown(&host)) {
          break;
        }
        std::vector<pollfd> descriptors;
        std::vector<std::int64_t> tcp_identifiers;
        pollfd protocol_descriptor{};
        protocol_descriptor.fd = host.file_descriptor;
        protocol_descriptor.events = POLLIN;
        descriptors.push_back(protocol_descriptor);
        pollfd event_descriptor{};
        event_descriptor.fd = session->event_file_descriptor;
        event_descriptor.events = POLLIN;
        descriptors.push_back(event_descriptor);
        append_tcp_poll_descriptors(&host, &descriptors, &tcp_identifiers);
        auto poll_result = poll(descriptors.data(), descriptors.size(),
                                timer_poll_timeout(&host));
        if (poll_result < 0) {
          if (errno == EINTR) {
            continue;
          }
          break;
        }
        if (poll_result == 0) {
          continue;
        }
        if ((descriptors[0].revents & POLLIN) != 0) {
          std::string message;
          if (!read_frame(host.file_descriptor, &message) ||
              !handle_protocol_message(&host, message)) {
            break;
          }
        }
        if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 &&
            (descriptors[0].revents & POLLIN) == 0) {
          break;
        }
        if ((descriptors[1].revents & POLLIN) != 0) {
          drain_event_file_descriptor(session->event_file_descriptor);
          collect_dns_results(&host);
          collect_external_events(&host);
        }
        for (std::size_t index = 0; index < tcp_identifiers.size(); ++index) {
          auto revents = descriptors[index + 2].revents;
          if (revents != 0 &&
              !process_tcp_poll_event(&host, tcp_identifiers[index], revents)) {
            initialized = false;
            break;
          }
        }
      }
    }
  }
  free_host(&host);
  close_session_descriptors(session);
  remove_session(session);
}

/**
 * Starts one independent QuickJS runtime for the supplied protocol socket.
 *
 * @param environment JNI environment for argument conversion and errors.
 * @param service_class MuonJavaScriptRuntimeService class object.
 * @param runtime_id Stable logical runtime identifier.
 * @param file_descriptor Service side of an Android socket pair.
 * @param runtime_source Packaged muon-js/1 runtime source.
 * @param backend_source Packaged ES module source.
 * @param filesystem_root App-private filesystem root.
 */
extern "C" JNIEXPORT void JNICALL
Java_dev_muon_prototype_MuonJavaScriptRuntimeService_nativeStart(
    JNIEnv* environment, jclass service_class, jstring runtime_id,
    jint file_descriptor, jstring runtime_source, jstring backend_source,
    jstring filesystem_root) {
  (void)service_class;
  auto runtime_id_value = get_jni_string(environment, runtime_id);
  auto runtime_source_value = get_jni_string(environment, runtime_source);
  auto backend_source_value = get_jni_string(environment, backend_source);
  auto filesystem_root_value = get_jni_string(environment, filesystem_root);
  if (environment->ExceptionCheck()) {
    return;
  }
  if (runtime_id_value.empty() || file_descriptor < 0 ||
      runtime_source_value.empty() || backend_source_value.empty() ||
      filesystem_root_value.empty()) {
    throw_illegal_state(environment,
                        "Invalid Android JavaScript runtime arguments.");
    return;
  }
  if (!initialize_java_http_bridge(environment)) {
    return;
  }

  auto control_file_descriptor = dup(file_descriptor);
  if (control_file_descriptor < 0) {
    throw_illegal_state(
        environment,
        std::string("Unable to duplicate the JavaScript protocol socket: ") +
            std::strerror(errno));
    return;
  }
  auto event_file_descriptor =
      eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (event_file_descriptor < 0) {
    close(control_file_descriptor);
    throw_illegal_state(
        environment,
        std::string("Unable to create the JavaScript host event handle: ") +
            std::strerror(errno));
    return;
  }
  auto session = std::make_shared<MuonJavaScriptSession>();
  session->runtime_id = runtime_id_value;
  session->file_descriptor = file_descriptor;
  session->control_file_descriptor = control_file_descriptor;
  session->event_file_descriptor = event_file_descriptor;
  {
    std::lock_guard<std::mutex> lock(g_sessions_mutex);
    if (g_sessions.contains(runtime_id_value)) {
      close(control_file_descriptor);
      close(event_file_descriptor);
      throw_illegal_state(environment,
                          "The JavaScript runtime id already exists.");
      return;
    }
    g_sessions.emplace(runtime_id_value, session);
  }
  try {
    std::thread(run_session, session, std::move(runtime_source_value),
                std::move(backend_source_value),
                std::move(filesystem_root_value))
        .detach();
  } catch (const std::exception& error) {
    {
      std::lock_guard<std::mutex> lock(g_sessions_mutex);
      g_sessions.erase(runtime_id_value);
    }
    close_session_descriptors(session);
    throw_illegal_state(
        environment,
        std::string("Unable to start the JavaScript runtime thread: ") +
            error.what());
  }
}

/**
 * Interrupts one logical JavaScript runtime.
 *
 * @param environment JNI environment, unused by this operation.
 * @param service_class MuonJavaScriptRuntimeService class object.
 * @param runtime_id Logical runtime identifier.
 */
extern "C" JNIEXPORT void JNICALL
Java_dev_muon_prototype_MuonJavaScriptRuntimeService_nativeShutdown(
    JNIEnv* environment, jclass service_class, jstring runtime_id) {
  (void)service_class;
  auto runtime_id_value = get_jni_string(environment, runtime_id);
  if (environment->ExceptionCheck()) {
    return;
  }
  std::shared_ptr<MuonJavaScriptSession> session;
  {
    std::lock_guard<std::mutex> lock(g_sessions_mutex);
    auto iterator = g_sessions.find(runtime_id_value);
    if (iterator != g_sessions.end()) {
      session = iterator->second;
    }
  }
  if (session != nullptr) {
    stop_session(session);
  }
}

/**
 * Interrupts all JavaScript runtimes and waits for their worker loops to exit.
 *
 * @param environment JNI environment, unused by this operation.
 * @param service_class MuonJavaScriptRuntimeService class object.
 */
extern "C" JNIEXPORT void JNICALL
Java_dev_muon_prototype_MuonJavaScriptRuntimeService_nativeShutdownAll(
    JNIEnv* environment, jclass service_class) {
  (void)environment;
  (void)service_class;
  std::vector<std::shared_ptr<MuonJavaScriptSession>> sessions;
  {
    std::lock_guard<std::mutex> lock(g_sessions_mutex);
    for (const auto& entry : g_sessions) {
      sessions.push_back(entry.second);
    }
  }
  for (const auto& session : sessions) {
    stop_session(session);
  }
  std::unique_lock<std::mutex> lock(g_sessions_mutex);
  g_sessions_changed.wait_for(lock, std::chrono::seconds(5),
                              []() { return g_sessions.empty(); });
}

/**
 * Queues one Android HTTP worker event for its owning QuickJS runtime.
 *
 * @param environment JNI environment used to copy event data.
 * @param client_class MuonJavaScriptHttpClient class object.
 * @param runtime_id Logical runtime identifier.
 * @param identifier HTTP operation identifier.
 * @param type Host event type.
 * @param payload JSON metadata for response and error events.
 * @param data Optional response body chunk.
 */
extern "C" JNIEXPORT void JNICALL
Java_dev_muon_prototype_MuonJavaScriptHttpClient_nativeOnHttpEvent(
    JNIEnv* environment, jclass client_class, jstring runtime_id,
    jlong identifier, jstring type, jstring payload, jbyteArray data) {
  (void)client_class;
  auto runtime_id_value = get_jni_string(environment, runtime_id);
  auto type_value = get_jni_string(environment, type);
  auto payload_value = get_jni_string(environment, payload);
  if (environment->ExceptionCheck() || runtime_id_value.empty() ||
      type_value.empty() || identifier <= 0) {
    return;
  }
  MuonHostEvent event;
  event.identifier = static_cast<std::int64_t>(identifier);
  event.type = std::move(type_value);
  event.payload = std::move(payload_value);
  if (data != nullptr) {
    auto length = environment->GetArrayLength(data);
    if (length > 0) {
      event.data.resize(static_cast<std::size_t>(length));
      environment->GetByteArrayRegion(
          data, 0, length, reinterpret_cast<jbyte*>(event.data.data()));
      if (environment->ExceptionCheck()) {
        return;
      }
    }
  }
  std::shared_ptr<MuonJavaScriptSession> session;
  {
    std::lock_guard<std::mutex> lock(g_sessions_mutex);
    auto iterator = g_sessions.find(runtime_id_value);
    if (iterator != g_sessions.end()) {
      session = iterator->second;
    }
  }
  if (session == nullptr || session->stop_requested.load()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(session->external_event_mutex);
    if (!session->accepts_external_events || session->stop_requested.load()) {
      return;
    }
    session->external_events.push_back(std::move(event));
    notify_session_event(session);
  }
}

/**
 * Returns the number of currently live logical JavaScript runtimes.
 *
 * @param environment JNI environment, unused by this operation.
 * @param service_class MuonJavaScriptRuntimeService class object.
 * @return Number of live QuickJS runtimes.
 */
extern "C" JNIEXPORT jint JNICALL
Java_dev_muon_prototype_MuonJavaScriptRuntimeService_nativeGetRuntimeCount(
    JNIEnv* environment, jclass service_class) {
  (void)environment;
  (void)service_class;
  std::lock_guard<std::mutex> lock(g_sessions_mutex);
  return static_cast<jint>(g_sessions.size());
}

/**
 * Returns the embedded QuickJS release version.
 *
 * @param environment JNI environment used to allocate the result string.
 * @param service_class MuonJavaScriptRuntimeService class object.
 * @return QuickJS release version.
 */
extern "C" JNIEXPORT jstring JNICALL
Java_dev_muon_prototype_MuonJavaScriptRuntimeService_nativeGetEngineVersion(
    JNIEnv* environment, jclass service_class) {
  (void)service_class;
  return environment->NewStringUTF(kQuickJsVersion);
}
