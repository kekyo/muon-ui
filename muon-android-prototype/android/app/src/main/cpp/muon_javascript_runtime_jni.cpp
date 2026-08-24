/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include <jni.h>

#include <android/log.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
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
  std::atomic<bool>* stop_requested = nullptr;
  std::atomic<std::int64_t> execution_deadline_nanoseconds{0};
};

struct MuonJavaScriptSession {
  std::string runtime_id;
  int file_descriptor = -1;
  int control_file_descriptor = -1;
  std::mutex descriptor_mutex;
  std::atomic<bool> stop_requested{false};
};

static std::mutex g_sessions_mutex;
static std::condition_variable g_sessions_changed;
static std::unordered_map<std::string, std::shared_ptr<MuonJavaScriptSession>>
    g_sessions;

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

static void free_host(MuonJavaScriptHost* host) {
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
  std::lock_guard<std::mutex> lock(session->descriptor_mutex);
  if (session->file_descriptor >= 0) {
    close(session->file_descriptor);
    session->file_descriptor = -1;
  }
  if (session->control_file_descriptor >= 0) {
    close(session->control_file_descriptor);
    session->control_file_descriptor = -1;
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
            "\"node:url\",\"abort\"]}";
        initialized = write_frame(host.file_descriptor, handshake);
      }

      while (initialized && !session->stop_requested.load()) {
        if (!process_timers(&host) || !process_protocol_promises(&host)) {
          break;
        }
        if (should_shutdown(&host)) {
          break;
        }
        pollfd descriptor{};
        descriptor.fd = host.file_descriptor;
        descriptor.events = POLLIN;
        auto poll_result =
            poll(&descriptor, 1, timer_poll_timeout(&host));
        if (poll_result < 0) {
          if (errno == EINTR) {
            continue;
          }
          break;
        }
        if (poll_result == 0) {
          continue;
        }
        if ((descriptor.revents & POLLIN) != 0) {
          std::string message;
          if (!read_frame(host.file_descriptor, &message) ||
              !handle_protocol_message(&host, message)) {
            break;
          }
        }
        if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 &&
            (descriptor.revents & POLLIN) == 0) {
          break;
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

  auto control_file_descriptor = dup(file_descriptor);
  if (control_file_descriptor < 0) {
    throw_illegal_state(
        environment,
        std::string("Unable to duplicate the JavaScript protocol socket: ") +
            std::strerror(errno));
    return;
  }
  auto session = std::make_shared<MuonJavaScriptSession>();
  session->runtime_id = runtime_id_value;
  session->file_descriptor = file_descriptor;
  session->control_file_descriptor = control_file_descriptor;
  {
    std::lock_guard<std::mutex> lock(g_sessions_mutex);
    if (g_sessions.contains(runtime_id_value)) {
      close(control_file_descriptor);
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
