/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "plugins/builtin/muon_builtin_executor.h"

#include "muon_cardio_post.h"
#include "muon_json_helpers.h"
#include "plugins/builtin/muon_builtin_completion.h"
#include "plugins/builtin/muon_builtin_environment_helpers.h"
#include "plugins/muon_type_metadata.h"
#include "yyjson.h"

#if defined(_WIN32)
#include "muon_windows_job_process.h"

#include <windows.h>
#else
#if defined(__linux__)
#include "config/muon_paths.h"
#include "process/muon_linux_executor_supervisor.h"
#endif

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#if defined(__linux__)
#include <sys/ioctl.h>
#endif
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if !defined(_WIN32)
extern char** environ;
#endif

namespace muon_internal {

struct RunOptions {
  std::string command;
  std::vector<std::string> args;
  std::string cwd;
  bool has_cwd = false;
  std::map<std::string, std::string> env;
  bool has_env = false;
  bool daemon = false;
};

enum class SpawnRpcOperation {
  kStart,
  kWriteStdin,
  kCloseStdin,
  kWait,
  kKill,
  kDispose,
};

struct SpawnRpcRequest {
  SpawnRpcOperation operation = SpawnRpcOperation::kStart;
  uint32_t handle_id = 0;
  RunOptions options;
  bool capture_stdout = true;
  bool capture_stderr = true;
};

enum class LibraryRpcOperation {
  kLoad,
  kGetFunction,
  kCall,
  kRelease,
};

struct LibraryRpcRequest {
  LibraryRpcOperation operation = LibraryRpcOperation::kLoad;
  uint32_t library_id = 0;
  uint32_t function_id = 0;
  std::string path;
  std::string name;
  std::vector<MuonTypeMetadata> arg_types;
  std::vector<std::string> arg_type_names;
  MuonTypeMetadata return_type = CreateMuonPrimitiveType(MUON_TYPE_VOID);
  std::string return_type_name = "void";
  yyjson_val* args = nullptr;
  yyjson_val* buffer_views = nullptr;
};

struct ExecutorCommand {
  enum class Kind {
    kWriteStdin,
    kCloseStdin,
  };

  Kind kind = Kind::kWriteStdin;
  std::vector<uint8_t> data;
  muon_completion_func completion = nullptr;
};

struct StdinWrite {
  std::vector<uint8_t> data;
  size_t offset = 0;
  muon_completion_func completion = nullptr;
};

struct ExecutorProcess {
  uint32_t handle_id = 0;
  uint32_t process_id = 0;
  int renderer_context_id = 0;
  const muon_plugin_helpers* helpers = nullptr;
  cardio::dispatcher* dispatcher = nullptr;
  bool capture_stdout = true;
  bool capture_stderr = true;
  bool daemon = false;

  std::mutex mutex;
  std::deque<ExecutorCommand> commands;
  std::vector<muon_completion_func> wait_completions;
  bool stdin_close_requested = false;
  bool stdin_closed = false;
  bool exited = false;
  bool disposed = false;
  bool callbacks_released = false;
  bool runtime_available = true;
  int32_t exit_code = -1;
  std::string failure_message;
  std::vector<uint8_t> stdout_data;
  std::vector<uint8_t> stderr_data;
  size_t pending_output_callbacks = 0;
  muon_native_function owner_callback = nullptr;
  muon_native_function stdout_callback = nullptr;
  muon_native_function stderr_callback = nullptr;

#if defined(_WIN32)
  MuonWindowsJobProcess windows_process{};
  HANDLE io_cancel_event = nullptr;
  HANDLE stdin_write = nullptr;
  HANDLE stdout_read = nullptr;
  HANDLE stderr_read = nullptr;
  std::condition_variable command_cv;
  std::condition_variable io_threads_cv;
  size_t io_threads_running = 0;
  bool io_cancel_requested = false;
  std::string io_cancel_error;
#elif defined(__linux__)
  pid_t supervisor_process_id = -1;
  pid_t target_process_group_id = -1;
  bool termination_requested = false;
  bool process_group_force_kill_issued = false;
  bool supervisor_cleanup_delegated = false;
  int control_fd = -1;
  int stdin_fd = -1;
  int stdout_fd = -1;
  int stderr_fd = -1;
  int wake_read_fd = -1;
  int wake_write_fd = -1;
#else
  pid_t child = -1;
  int stdin_fd = -1;
  int stdout_fd = -1;
  int stderr_fd = -1;
  int wake_read_fd = -1;
  int wake_write_fd = -1;
  bool wait_status_ready = false;
  bool wait_failed = false;
  int wait_status = 0;
#endif
};

struct AdhocFunction {
  uint32_t function_id = 0;
  std::string name;
  std::vector<MuonTypeMetadata> arg_types;
  std::vector<std::string> arg_type_names;
  MuonTypeMetadata return_type = CreateMuonPrimitiveType(MUON_TYPE_VOID);
  std::string return_type_name;
  std::shared_ptr<MuonFunctionSignatureStorage> signature_storage;
  muon_native_function registered_function = nullptr;
  tra_ffic_function_ref function_ref = {};
};

struct AdhocLibrary {
  uint32_t library_id = 0;
  int renderer_context_id = 0;
  const muon_plugin_helpers* helpers = nullptr;
  cardio::dispatcher* dispatcher = nullptr;
  void* handle = nullptr;
  std::mutex mutex;
  uint32_t next_function_id = 1;
  std::map<uint32_t, std::shared_ptr<AdhocFunction>> functions;
  std::vector<muon_completion_func> release_completions;
  bool release_requested = false;
  bool closed = false;
  bool runtime_available = true;
  size_t active_calls = 0;
  tra_ffic_task_queue traffic_queue = {};
  tra_ffic_side caller_side = {};
  tra_ffic_side callee_side = {};
  bool traffic_initialized = false;
};

struct ExecutorRuntime {
  const muon_plugin_helpers* helpers = nullptr;
  cardio::dispatcher* dispatcher = nullptr;
  std::mutex mutex;
  uint32_t next_handle_id = 1;
  uint32_t next_library_id = 1;
  std::map<uint32_t, std::shared_ptr<ExecutorProcess>> processes;
  std::map<uint32_t, std::shared_ptr<AdhocLibrary>> libraries;
  bool shutting_down = false;
};

struct OutputCallbackCompletionState {
  const muon_plugin_helpers* helpers = nullptr;
  std::shared_ptr<ExecutorProcess> process;
  muon_completion_func completion = nullptr;
};

struct AdhocDecodedArgs {
  std::vector<tra_ffic_value> values;
  std::vector<std::string> string_storage;
  std::vector<std::vector<uint8_t>> buffer_storage;
};

struct AdhocCallState {
  std::shared_ptr<AdhocLibrary> library;
  std::shared_ptr<AdhocFunction> function;
  AdhocDecodedArgs args;
  muon_completion_func completion = nullptr;
  std::string result_json;
  std::string error_message;
};

static std::unique_ptr<ExecutorRuntime> g_executor_runtime;
static std::mutex g_executor_runtime_mutex;

static void LogExecutorMessage(
    const std::shared_ptr<ExecutorProcess>& process,
    muon_log_level level,
    const std::string& message);

static void CompleteWaitersIfReady(
    const std::shared_ptr<ExecutorProcess>& process);

static const muon_type_descriptor type_void = {
    MUON_TYPE_VOID,
    nullptr,
};

static const muon_type_descriptor type_string = {
    MUON_TYPE_STRING,
    nullptr,
};

static const muon_type_descriptor type_u32 = {
    MUON_TYPE_U32,
    nullptr,
};

static const muon_type_descriptor type_buffer_view = {
    MUON_TYPE_BUFFER_VIEW,
    nullptr,
};

static const muon_function_signature owner_callback_signature = {
    0,
    nullptr,
    &type_void,
};

static const muon_type_descriptor owner_callback_type = {
    MUON_TYPE_FUNCTION,
    &owner_callback_signature,
};

static const muon_type_descriptor output_callback_args[] = {
    type_buffer_view,
};

static const muon_function_signature output_callback_signature = {
    1,
    output_callback_args,
    &type_void,
};

static const muon_type_descriptor output_callback_type = {
    MUON_TYPE_FUNCTION,
    &output_callback_signature,
};

static const muon_type_descriptor spawn_rpc_args[] = {
    type_string,
    type_buffer_view,
    type_u32,
    owner_callback_type,
    output_callback_type,
    output_callback_type,
};

static const muon_type_descriptor library_rpc_args[] = {
    type_string,
    type_buffer_view,
    type_u32,
};

static bool ContainsNul(std::string_view value) {
  return value.find('\0') != std::string_view::npos;
}

static bool ReadRequiredString(yyjson_val* object,
                               const char* key,
                               std::string* target,
                               std::string* error_message) {
  const auto value = yyjson_obj_get(object, key);
  if (!yyjson_is_str(value)) {
    *error_message = std::string(key) + " is required";
    return false;
  }
  target->assign(yyjson_get_str(value), yyjson_get_len(value));
  if (target->empty()) {
    *error_message = std::string(key) + " is required";
    return false;
  }
  if (ContainsNul(*target)) {
    *error_message = std::string(key) + " must not contain NUL";
    return false;
  }
  return true;
}

static bool ReadOptionalString(yyjson_val* object,
                               const char* key,
                               std::string* target,
                               bool* has_value,
                               std::string* error_message) {
  const auto value = yyjson_obj_get(object, key);
  *has_value = false;
  target->clear();
  if (value == nullptr || yyjson_is_null(value)) {
    return true;
  }
  if (!yyjson_is_str(value)) {
    *error_message = std::string(key) + " must be a string";
    return false;
  }
  target->assign(yyjson_get_str(value), yyjson_get_len(value));
  if (ContainsNul(*target)) {
    *error_message = std::string(key) + " must not contain NUL";
    return false;
  }
  *has_value = true;
  return true;
}

static bool ReadStringArray(yyjson_val* object,
                            const char* key,
                            std::vector<std::string>* target,
                            std::string* error_message) {
  target->clear();
  const auto value = yyjson_obj_get(object, key);
  if (value == nullptr || yyjson_is_null(value)) {
    return true;
  }
  if (!yyjson_is_arr(value)) {
    *error_message = std::string(key) + " must be an array";
    return false;
  }
  const auto size = yyjson_arr_size(value);
  target->reserve(size);
  for (auto index = size_t{0}; index < size; ++index) {
    const auto entry = yyjson_arr_get(value, index);
    if (!yyjson_is_str(entry)) {
      *error_message = std::string(key) + " entries must be strings";
      return false;
    }
    std::string string_entry(yyjson_get_str(entry), yyjson_get_len(entry));
    if (ContainsNul(string_entry)) {
      *error_message = std::string(key) + " entries must not contain NUL";
      return false;
    }
    target->push_back(std::move(string_entry));
  }
  return true;
}

static bool ReadEnvironmentObject(yyjson_val* object,
                                  RunOptions* options,
                                  std::string* error_message) {
  const auto value = yyjson_obj_get(object, "env");
  options->env.clear();
  options->has_env = false;
  if (value == nullptr || yyjson_is_null(value)) {
    return true;
  }
  if (!yyjson_is_obj(value)) {
    *error_message = "env must be an object";
    return false;
  }
  options->has_env = true;
  yyjson_val* key = nullptr;
  yyjson_val* entry = nullptr;
  size_t index = 0;
  size_t max = 0;
  yyjson_obj_foreach(value, index, max, key, entry) {
    if (!yyjson_is_str(key) || !yyjson_is_str(entry)) {
      *error_message = "env entries must be strings";
      return false;
    }
    std::string name(yyjson_get_str(key), yyjson_get_len(key));
    std::string env_value(yyjson_get_str(entry), yyjson_get_len(entry));
    if (name.empty() || name.find('=') != std::string::npos ||
        ContainsNul(name)) {
      *error_message = "env keys must be non-empty names without '=' or NUL";
      return false;
    }
    if (ContainsNul(env_value)) {
      *error_message = "env values must not contain NUL";
      return false;
    }
    options->env[std::move(name)] = std::move(env_value);
  }
  return true;
}

static bool ReadOptionalBool(yyjson_val* object,
                             const char* key,
                             bool default_value,
                             bool* target,
                             std::string* error_message) {
  const auto value = yyjson_obj_get(object, key);
  if (value == nullptr || yyjson_is_null(value)) {
    *target = default_value;
    return true;
  }
  if (!yyjson_is_bool(value)) {
    *error_message = std::string(key) + " must be a boolean";
    return false;
  }
  *target = yyjson_get_bool(value);
  return true;
}

static bool ReadHandleId(yyjson_val* object,
                         uint32_t* handle_id,
                         std::string* error_message) {
  const auto value = yyjson_obj_get(object, "handleId");
  if (!yyjson_is_uint(value)) {
    *error_message = "handleId is required";
    return false;
  }
  const auto parsed = yyjson_get_uint(value);
  if (parsed == 0 || parsed > UINT32_MAX) {
    *error_message = "handleId is invalid";
    return false;
  }
  *handle_id = static_cast<uint32_t>(parsed);
  return true;
}

static bool ParseStartOptions(yyjson_val* value,
                              RunOptions* options,
                              std::string* error_message) {
  if (!yyjson_is_obj(value)) {
    *error_message = "options must be an object";
    return false;
  }
  const auto daemon = yyjson_obj_get(value, "daemon");
  if (daemon != nullptr && !yyjson_is_bool(daemon)) {
    *error_message = "daemon must be a boolean";
    return false;
  }
  options->daemon = daemon == nullptr ? false : yyjson_get_bool(daemon);
  return ReadRequiredString(value, "command", &options->command,
                            error_message) &&
         ReadStringArray(value, "args", &options->args, error_message) &&
         ReadOptionalString(value, "cwd", &options->cwd, &options->has_cwd,
                            error_message) &&
         ReadEnvironmentObject(value, options, error_message);
}

static bool ParseSpawnRpcRequest(const char* request_json,
                                 SpawnRpcRequest* request,
                                 std::string* error_message) {
  if (request_json == nullptr) {
    *error_message = "Request JSON is required";
    return false;
  }
  yyjson_read_err read_error = {};
  auto* document = yyjson_read_opts(
      const_cast<char*>(request_json), std::strlen(request_json),
      YYJSON_READ_NOFLAG, nullptr, &read_error);
  if (document == nullptr) {
    *error_message = "Request JSON is invalid";
    return false;
  }

  auto success = false;
  auto* root = yyjson_doc_get_root(document);
  if (!yyjson_is_obj(root)) {
    *error_message = "Request JSON root must be an object";
  } else {
    const auto op = yyjson_obj_get(root, "op");
    if (!yyjson_is_str(op)) {
      *error_message = "op is required";
    } else {
      const std::string op_name(yyjson_get_str(op), yyjson_get_len(op));
      if (op_name == "start") {
        request->operation = SpawnRpcOperation::kStart;
        success = ParseStartOptions(yyjson_obj_get(root, "options"),
                                    &request->options, error_message) &&
                  ReadOptionalBool(root, "captureStdout", true,
                                   &request->capture_stdout, error_message) &&
                  ReadOptionalBool(root, "captureStderr", true,
                                   &request->capture_stderr, error_message);
      } else if (op_name == "writeStdin") {
        request->operation = SpawnRpcOperation::kWriteStdin;
        success = ReadHandleId(root, &request->handle_id, error_message);
      } else if (op_name == "closeStdin") {
        request->operation = SpawnRpcOperation::kCloseStdin;
        success = ReadHandleId(root, &request->handle_id, error_message);
      } else if (op_name == "wait") {
        request->operation = SpawnRpcOperation::kWait;
        success = ReadHandleId(root, &request->handle_id, error_message);
      } else if (op_name == "kill") {
        request->operation = SpawnRpcOperation::kKill;
        success = ReadHandleId(root, &request->handle_id, error_message);
      } else if (op_name == "dispose") {
        request->operation = SpawnRpcOperation::kDispose;
        success = ReadHandleId(root, &request->handle_id, error_message);
      } else {
        *error_message = "op is unsupported";
      }
    }
  }
  yyjson_doc_free(document);
  return success;
}

static std::string Base64Encode(const std::vector<uint8_t>& data) {
  static constexpr char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string encoded;
  encoded.reserve(((data.size() + 2) / 3) * 4);
  auto index = size_t{0};
  while (index + 3 <= data.size()) {
    const auto value = (static_cast<uint32_t>(data[index]) << 16) |
                       (static_cast<uint32_t>(data[index + 1]) << 8) |
                       static_cast<uint32_t>(data[index + 2]);
    encoded.push_back(alphabet[(value >> 18) & 0x3f]);
    encoded.push_back(alphabet[(value >> 12) & 0x3f]);
    encoded.push_back(alphabet[(value >> 6) & 0x3f]);
    encoded.push_back(alphabet[value & 0x3f]);
    index += 3;
  }
  const auto remaining = data.size() - index;
  if (remaining == 1) {
    const auto value = static_cast<uint32_t>(data[index]) << 16;
    encoded.push_back(alphabet[(value >> 18) & 0x3f]);
    encoded.push_back(alphabet[(value >> 12) & 0x3f]);
    encoded.push_back('=');
    encoded.push_back('=');
  } else if (remaining == 2) {
    const auto value = (static_cast<uint32_t>(data[index]) << 16) |
                       (static_cast<uint32_t>(data[index + 1]) << 8);
    encoded.push_back(alphabet[(value >> 18) & 0x3f]);
    encoded.push_back(alphabet[(value >> 12) & 0x3f]);
    encoded.push_back(alphabet[(value >> 6) & 0x3f]);
    encoded.push_back('=');
  }
  return encoded;
}

static std::string GetExecutorTrafficError(const tra_ffic_error& error) {
  return error.message[0] == '\0' ? "tra-ffic operation failed"
                                  : error.message;
}

static bool ParseUint64Text(std::string_view source, uint64_t* value) {
  if (value == nullptr || source.empty()) {
    return false;
  }
  uint64_t parsed = 0;
  const auto* begin = source.data();
  const auto* end = begin + source.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc() || result.ptr != end) {
    return false;
  }
  *value = parsed;
  return true;
}

static bool ParseInt64Text(std::string_view source, int64_t* value) {
  if (value == nullptr || source.empty()) {
    return false;
  }
  int64_t parsed = 0;
  const auto* begin = source.data();
  const auto* end = begin + source.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc() || result.ptr != end) {
    return false;
  }
  *value = parsed;
  return true;
}

static bool ReadAdhocUint64(yyjson_val* value,
                            uint64_t max_value,
                            uint64_t* target) {
  if (target == nullptr || value == nullptr) {
    return false;
  }
  uint64_t parsed = 0;
  if (yyjson_is_uint(value)) {
    parsed = yyjson_get_uint(value);
  } else if (yyjson_is_str(value)) {
    const auto source =
        std::string_view(yyjson_get_str(value), yyjson_get_len(value));
    if (!ParseUint64Text(source, &parsed)) {
      return false;
    }
  } else {
    return false;
  }
  if (parsed > max_value) {
    return false;
  }
  *target = parsed;
  return true;
}

static bool ReadAdhocInt64(yyjson_val* value,
                           int64_t min_value,
                           int64_t max_value,
                           int64_t* target) {
  if (target == nullptr || value == nullptr) {
    return false;
  }
  int64_t parsed = 0;
  if (yyjson_is_sint(value)) {
    parsed = yyjson_get_sint(value);
  } else if (yyjson_is_uint(value)) {
    const auto unsigned_value = yyjson_get_uint(value);
    if (unsigned_value >
        static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      return false;
    }
    parsed = static_cast<int64_t>(unsigned_value);
  } else if (yyjson_is_str(value)) {
    const auto source =
        std::string_view(yyjson_get_str(value), yyjson_get_len(value));
    if (!ParseInt64Text(source, &parsed)) {
      return false;
    }
  } else {
    return false;
  }
  if (parsed < min_value || parsed > max_value) {
    return false;
  }
  *target = parsed;
  return true;
}

static bool ReadAdhocDouble(yyjson_val* value, double* target) {
  if (target == nullptr || value == nullptr) {
    return false;
  }
  if (yyjson_is_real(value)) {
    *target = yyjson_get_real(value);
    return std::isfinite(*target);
  }
  if (yyjson_is_sint(value)) {
    *target = static_cast<double>(yyjson_get_sint(value));
    return true;
  }
  if (yyjson_is_uint(value)) {
    *target = static_cast<double>(yyjson_get_uint(value));
    return true;
  }
  return false;
}

static bool ConvertAdhocTypeName(std::string_view source,
                                 bool allow_void,
                                 MuonTypeMetadata* type,
                                 std::string* canonical_name,
                                 std::string* error_message) {
  if (type == nullptr || canonical_name == nullptr) {
    return false;
  }
  auto value_type = MUON_TYPE_VOID;
  std::string name(source);
  if (name == "voidType") {
    name = "void";
  } else if (name == "stringType") {
    name = "string";
  }
  if (name == "void") {
    value_type = MUON_TYPE_VOID;
  } else if (name == "bool") {
    value_type = MUON_TYPE_BOOL;
  } else if (name == "int8") {
    value_type = MUON_TYPE_I8;
  } else if (name == "uint8") {
    value_type = MUON_TYPE_U8;
  } else if (name == "int16") {
    value_type = MUON_TYPE_I16;
  } else if (name == "uint16") {
    value_type = MUON_TYPE_U16;
  } else if (name == "int32") {
    value_type = MUON_TYPE_I32;
  } else if (name == "uint32") {
    value_type = MUON_TYPE_U32;
  } else if (name == "int64") {
    value_type = MUON_TYPE_I64;
  } else if (name == "uint64") {
    value_type = MUON_TYPE_U64;
  } else if (name == "float32") {
    value_type = MUON_TYPE_F32;
  } else if (name == "float64") {
    value_type = MUON_TYPE_F64;
  } else if (name == "string") {
    value_type = MUON_TYPE_STRING;
  } else if (name == "pointer") {
    value_type = MUON_TYPE_POINTER;
  } else if (name == "bufferView") {
    value_type = MUON_TYPE_BUFFER_VIEW;
  } else if (name == "usize") {
    value_type = sizeof(size_t) >= 8 ? MUON_TYPE_U64 : MUON_TYPE_U32;
  } else {
    if (error_message != nullptr) {
      *error_message = "Unsupported adhoc type";
    }
    return false;
  }
  if (!allow_void && value_type == MUON_TYPE_VOID) {
    if (error_message != nullptr) {
      *error_message = "Void type is not valid here";
    }
    return false;
  }
  *type = CreateMuonPrimitiveType(value_type);
  *canonical_name = name;
  return true;
}

static bool ReadAdhocType(yyjson_val* value,
                          bool allow_void,
                          MuonTypeMetadata* type,
                          std::string* canonical_name,
                          std::string* error_message) {
  yyjson_val* type_value = value;
  if (yyjson_is_obj(value)) {
    type_value = yyjson_obj_get(value, "name");
    if (type_value == nullptr) {
      type_value = yyjson_obj_get(value, "type");
    }
  }
  if (!yyjson_is_str(type_value)) {
    *error_message = "Adhoc type must be a type descriptor";
    return false;
  }
  const auto name =
      std::string_view(yyjson_get_str(type_value), yyjson_get_len(type_value));
  return ConvertAdhocTypeName(name, allow_void, type, canonical_name,
                              error_message);
}

static bool ReadLibraryHandleId(yyjson_val* object,
                                uint32_t* library_id,
                                std::string* error_message) {
  const auto value = yyjson_obj_get(object, "libraryId");
  if (!yyjson_is_uint(value)) {
    *error_message = "libraryId is required";
    return false;
  }
  const auto parsed = yyjson_get_uint(value);
  if (parsed == 0 || parsed > UINT32_MAX) {
    *error_message = "libraryId is invalid";
    return false;
  }
  *library_id = static_cast<uint32_t>(parsed);
  return true;
}

static bool ReadFunctionHandleId(yyjson_val* object,
                                 uint32_t* function_id,
                                 std::string* error_message) {
  const auto value = yyjson_obj_get(object, "functionId");
  if (!yyjson_is_uint(value)) {
    *error_message = "functionId is required";
    return false;
  }
  const auto parsed = yyjson_get_uint(value);
  if (parsed == 0 || parsed > UINT32_MAX) {
    *error_message = "functionId is invalid";
    return false;
  }
  *function_id = static_cast<uint32_t>(parsed);
  return true;
}

static bool ParseAdhocSignature(yyjson_val* value,
                                LibraryRpcRequest* request,
                                std::string* error_message) {
  if (!yyjson_is_obj(value)) {
    *error_message = "signature must be an object";
    return false;
  }
  const auto args = yyjson_obj_get(value, "argTypes");
  if (!yyjson_is_arr(args)) {
    *error_message = "signature.argTypes must be an array";
    return false;
  }
  request->arg_types.clear();
  request->arg_type_names.clear();
  request->arg_types.resize(yyjson_arr_size(args));
  request->arg_type_names.resize(yyjson_arr_size(args));
  for (auto index = size_t{0}; index < yyjson_arr_size(args); ++index) {
    if (!ReadAdhocType(yyjson_arr_get(args, index), false,
                       &request->arg_types[index],
                       &request->arg_type_names[index], error_message)) {
      return false;
    }
  }
  const auto return_type = yyjson_obj_get(value, "returnType");
  if (!ReadAdhocType(return_type, true, &request->return_type,
                     &request->return_type_name, error_message)) {
    return false;
  }
  return true;
}

static bool ParseLibraryRpcRequestRoot(yyjson_val* root,
                                       LibraryRpcRequest* request,
                                       std::string* error_message) {
  if (!yyjson_is_obj(root)) {
    *error_message = "Request JSON root must be an object";
    return false;
  }
  const auto op = yyjson_obj_get(root, "op");
  if (!yyjson_is_str(op)) {
    *error_message = "op is required";
    return false;
  }
  const std::string op_name(yyjson_get_str(op), yyjson_get_len(op));
  if (op_name == "load") {
    request->operation = LibraryRpcOperation::kLoad;
    return ReadRequiredString(root, "path", &request->path, error_message);
  }
  if (op_name == "getFunction") {
    request->operation = LibraryRpcOperation::kGetFunction;
    return ReadLibraryHandleId(root, &request->library_id, error_message) &&
           ReadRequiredString(root, "name", &request->name, error_message) &&
           ParseAdhocSignature(yyjson_obj_get(root, "signature"), request,
                               error_message);
  }
  if (op_name == "call") {
    request->operation = LibraryRpcOperation::kCall;
    request->args = yyjson_obj_get(root, "args");
    request->buffer_views = yyjson_obj_get(root, "bufferViews");
    if (!yyjson_is_arr(request->args)) {
      *error_message = "args must be an array";
      return false;
    }
    if (request->buffer_views != nullptr &&
        !yyjson_is_arr(request->buffer_views)) {
      *error_message = "bufferViews must be an array";
      return false;
    }
    return ReadLibraryHandleId(root, &request->library_id, error_message) &&
           ReadFunctionHandleId(root, &request->function_id, error_message);
  }
  if (op_name == "release") {
    request->operation = LibraryRpcOperation::kRelease;
    return ReadLibraryHandleId(root, &request->library_id, error_message);
  }
  *error_message = "op is unsupported";
  return false;
}

static bool FindAdhocBufferView(yyjson_val* buffer_views,
                                size_t arg_index,
                                size_t* offset,
                                size_t* size,
                                std::string* error_message) {
  if (offset == nullptr || size == nullptr) {
    return false;
  }
  if (!yyjson_is_arr(buffer_views)) {
    *error_message = "buffer_view argument is missing";
    return false;
  }
  for (auto index = size_t{0}; index < yyjson_arr_size(buffer_views); ++index) {
    const auto entry = yyjson_arr_get(buffer_views, index);
    if (!yyjson_is_obj(entry)) {
      *error_message = "bufferViews entries must be objects";
      return false;
    }
    uint64_t raw_index = 0;
    uint64_t raw_offset = 0;
    uint64_t raw_size = 0;
    if (!ReadAdhocUint64(yyjson_obj_get(entry, "argIndex"), SIZE_MAX,
                         &raw_index) ||
        !ReadAdhocUint64(yyjson_obj_get(entry, "offset"), SIZE_MAX,
                         &raw_offset) ||
        !ReadAdhocUint64(yyjson_obj_get(entry, "size"), SIZE_MAX,
                         &raw_size)) {
      *error_message = "bufferViews entry is invalid";
      return false;
    }
    if (raw_index == arg_index) {
      *offset = static_cast<size_t>(raw_offset);
      *size = static_cast<size_t>(raw_size);
      return true;
    }
  }
  *error_message = "buffer_view argument is missing";
  return false;
}

static bool DecodeAdhocCallArguments(const AdhocFunction& function,
                                     yyjson_val* args,
                                     yyjson_val* buffer_views,
                                     const muon_buffer_view& data,
                                     AdhocDecodedArgs* decoded,
                                     std::string* error_message) {
  if (decoded == nullptr || !yyjson_is_arr(args) ||
      yyjson_arr_size(args) != function.arg_types.size()) {
    *error_message = "Invalid adhoc argument count";
    return false;
  }
  decoded->values.clear();
  decoded->string_storage.clear();
  decoded->buffer_storage.clear();
  decoded->values.resize(function.arg_types.size());
  decoded->string_storage.reserve(function.arg_types.size());
  decoded->buffer_storage.reserve(function.arg_types.size());
  const auto* data_begin = static_cast<const uint8_t*>(data.data);
  for (auto index = size_t{0}; index < function.arg_types.size(); ++index) {
    const auto argument = yyjson_arr_get(args, index);
    const auto type = function.arg_types[index].type;
    switch (type) {
      case MUON_TYPE_BOOL:
        if (!yyjson_is_bool(argument)) {
          *error_message = "Invalid bool argument";
          return false;
        }
        decoded->values[index] = tra_ffic_value_bool(yyjson_get_bool(argument));
        break;
      case MUON_TYPE_I8: {
        int64_t value = 0;
        if (!ReadAdhocInt64(argument, INT8_MIN, INT8_MAX, &value)) {
          *error_message = "Invalid int8 argument";
          return false;
        }
        decoded->values[index] = tra_ffic_value_int8(static_cast<int8_t>(value));
        break;
      }
      case MUON_TYPE_U8: {
        uint64_t value = 0;
        if (!ReadAdhocUint64(argument, UINT8_MAX, &value)) {
          *error_message = "Invalid uint8 argument";
          return false;
        }
        decoded->values[index] =
            tra_ffic_value_uint8(static_cast<uint8_t>(value));
        break;
      }
      case MUON_TYPE_I16: {
        int64_t value = 0;
        if (!ReadAdhocInt64(argument, INT16_MIN, INT16_MAX, &value)) {
          *error_message = "Invalid int16 argument";
          return false;
        }
        decoded->values[index] =
            tra_ffic_value_int16(static_cast<int16_t>(value));
        break;
      }
      case MUON_TYPE_U16: {
        uint64_t value = 0;
        if (!ReadAdhocUint64(argument, UINT16_MAX, &value)) {
          *error_message = "Invalid uint16 argument";
          return false;
        }
        decoded->values[index] =
            tra_ffic_value_uint16(static_cast<uint16_t>(value));
        break;
      }
      case MUON_TYPE_I32: {
        int64_t value = 0;
        if (!ReadAdhocInt64(argument, INT32_MIN, INT32_MAX, &value)) {
          *error_message = "Invalid int32 argument";
          return false;
        }
        decoded->values[index] =
            tra_ffic_value_int32(static_cast<int32_t>(value));
        break;
      }
      case MUON_TYPE_U32: {
        uint64_t value = 0;
        if (!ReadAdhocUint64(argument, UINT32_MAX, &value)) {
          *error_message = "Invalid uint32 argument";
          return false;
        }
        decoded->values[index] =
            tra_ffic_value_uint32(static_cast<uint32_t>(value));
        break;
      }
      case MUON_TYPE_I64: {
        int64_t value = 0;
        if (!ReadAdhocInt64(argument, INT64_MIN, INT64_MAX, &value)) {
          *error_message = "Invalid int64 argument";
          return false;
        }
        decoded->values[index] = tra_ffic_value_int64(value);
        break;
      }
      case MUON_TYPE_U64: {
        uint64_t value = 0;
        if (!ReadAdhocUint64(argument, UINT64_MAX, &value)) {
          *error_message = "Invalid uint64 argument";
          return false;
        }
        decoded->values[index] = tra_ffic_value_uint64(value);
        break;
      }
      case MUON_TYPE_F32: {
        double value = 0.0;
        if (!ReadAdhocDouble(argument, &value) ||
            value < -static_cast<double>(std::numeric_limits<float>::max()) ||
            value > static_cast<double>(std::numeric_limits<float>::max())) {
          *error_message = "Invalid float32 argument";
          return false;
        }
        decoded->values[index] =
            tra_ffic_value_float(static_cast<float>(value));
        break;
      }
      case MUON_TYPE_F64: {
        double value = 0.0;
        if (!ReadAdhocDouble(argument, &value)) {
          *error_message = "Invalid float64 argument";
          return false;
        }
        decoded->values[index] = tra_ffic_value_double(value);
        break;
      }
      case MUON_TYPE_STRING:
        if (yyjson_is_null(argument)) {
          decoded->values[index] = tra_ffic_value_string(nullptr);
          break;
        }
        if (!yyjson_is_str(argument)) {
          *error_message = "Invalid string argument";
          return false;
        }
        decoded->string_storage.emplace_back(yyjson_get_str(argument),
                                             yyjson_get_len(argument));
        if (ContainsNul(decoded->string_storage.back())) {
          *error_message = "String argument must not contain NUL";
          return false;
        }
        decoded->values[index] =
            tra_ffic_value_string(decoded->string_storage.back().c_str());
        break;
      case MUON_TYPE_POINTER: {
        uint64_t value = 0;
        if (yyjson_is_null(argument)) {
          value = 0;
        } else if (!ReadAdhocUint64(
                       argument,
                       static_cast<uint64_t>(
                           std::numeric_limits<uintptr_t>::max()),
                       &value)) {
          *error_message = "Invalid pointer argument";
          return false;
        }
        decoded->values[index] = tra_ffic_value_pointer(
            reinterpret_cast<void*>(static_cast<uintptr_t>(value)));
        break;
      }
      case MUON_TYPE_BUFFER_VIEW: {
        size_t offset = 0;
        size_t size = 0;
        if (!FindAdhocBufferView(buffer_views, index, &offset, &size,
                                 error_message)) {
          return false;
        }
        if ((size > 0 && data_begin == nullptr) || offset > data.size ||
            size > data.size - offset) {
          *error_message = "buffer_view data is invalid";
          return false;
        }
        auto& storage = decoded->buffer_storage.emplace_back();
        if (size > 0) {
          storage.assign(data_begin + offset, data_begin + offset + size);
        }
        decoded->values[index] = tra_ffic_value_buffer_view(
            storage.empty() ? nullptr : storage.data(),
            static_cast<uintptr_t>(storage.size()));
        break;
      }
      case MUON_TYPE_VOID:
      case MUON_TYPE_FUNCTION:
      default:
        *error_message = "Unsupported adhoc argument type";
        return false;
    }
  }
  return true;
}

static std::string CreateAdhocCallResultJson(
    const tra_ffic_result& result,
    const MuonTypeMetadata& return_type,
    std::string* error_message) {
  if (!result.success) {
    if (error_message != nullptr) {
      *error_message = result.error_message;
    }
    return {};
  }
  std::string json = "{";
  switch (return_type.type) {
    case MUON_TYPE_VOID:
      break;
    case MUON_TYPE_BOOL:
      json += "\"value\":";
      json += result.value.as.bool_value ? "true" : "false";
      break;
    case MUON_TYPE_I8:
      json += "\"value\":";
      json += std::to_string(result.value.as.int8_value);
      break;
    case MUON_TYPE_U8:
      json += "\"value\":";
      json += std::to_string(result.value.as.uint8_value);
      break;
    case MUON_TYPE_I16:
      json += "\"value\":";
      json += std::to_string(result.value.as.int16_value);
      break;
    case MUON_TYPE_U16:
      json += "\"value\":";
      json += std::to_string(result.value.as.uint16_value);
      break;
    case MUON_TYPE_I32:
      json += "\"value\":";
      json += std::to_string(result.value.as.int32_value);
      break;
    case MUON_TYPE_U32:
      json += "\"value\":";
      json += std::to_string(result.value.as.uint32_value);
      break;
    case MUON_TYPE_I64:
      json += "\"value\":";
      AppendJsonString(&json, std::to_string(result.value.as.int64_value));
      break;
    case MUON_TYPE_U64:
      json += "\"value\":";
      AppendJsonString(&json, std::to_string(result.value.as.uint64_value));
      break;
    case MUON_TYPE_F32:
      if (!std::isfinite(result.value.as.float_value)) {
        if (error_message != nullptr) {
          *error_message = "Adhoc function returned a non-finite float32 value";
        }
        return {};
      }
      json += "\"value\":";
      json += std::to_string(result.value.as.float_value);
      break;
    case MUON_TYPE_F64:
      if (!std::isfinite(result.value.as.double_value)) {
        if (error_message != nullptr) {
          *error_message = "Adhoc function returned a non-finite float64 value";
        }
        return {};
      }
      json += "\"value\":";
      json += std::to_string(result.value.as.double_value);
      break;
    case MUON_TYPE_STRING:
      json += "\"value\":";
      if (result.value.as.string_value == nullptr) {
        json += "null";
      } else {
        AppendJsonString(&json, result.value.as.string_value);
      }
      break;
    case MUON_TYPE_POINTER:
      json += "\"pointer\":";
      AppendJsonString(
          &json,
          std::to_string(reinterpret_cast<uintptr_t>(
              result.value.as.pointer_value)));
      break;
    case MUON_TYPE_BUFFER_VIEW: {
      const auto& view = result.value.as.buffer_view_value;
      if (view.data == nullptr && view.size != 0) {
        if (error_message != nullptr) {
          *error_message = "Adhoc function returned an invalid buffer_view";
        }
        return {};
      }
      const auto* begin = static_cast<const uint8_t*>(view.data);
      auto bytes = std::vector<uint8_t>();
      if (view.size > 0) {
        bytes.assign(begin, begin + view.size);
      }
      json += "\"base64\":";
      AppendJsonString(&json, Base64Encode(bytes));
      break;
    }
    case MUON_TYPE_FUNCTION:
    default:
      if (error_message != nullptr) {
        *error_message = "Unsupported adhoc return type";
      }
      return {};
  }
  json += "}";
  return json;
}

static void CaptureAdhocCallResult(void* raw_state,
                                   const tra_ffic_result* result) {
  auto* state = static_cast<AdhocCallState*>(raw_state);
  if (state == nullptr) {
    return;
  }
  if (result == nullptr) {
    state->error_message = "Adhoc function did not produce a result";
    return;
  }
  state->result_json = CreateAdhocCallResultJson(
      *result, state->function->return_type, &state->error_message);
}

static void CloseAdhocDynamicLibrary(void* handle) {
  if (handle == nullptr) {
    return;
  }
#if defined(_WIN32)
  FreeLibrary(static_cast<HMODULE>(handle));
#else
  dlclose(handle);
#endif
}

static void* OpenAdhocDynamicLibrary(const std::string& path,
                                     std::string* error_message) {
#if defined(_WIN32)
  std::wstring wide_path;
  if (!MuonUtf8ToWide(path, &wide_path)) {
    *error_message = "Library path is not valid UTF-8";
    return nullptr;
  }
  auto* handle = LoadLibraryW(wide_path.c_str());
  if (handle == nullptr) {
    *error_message = "Failed to load library";
  }
  return handle;
#else
  dlerror();
  auto* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (handle == nullptr) {
    const auto* message = dlerror();
    *error_message =
        message == nullptr ? "Failed to load library" : message;
  }
  return handle;
#endif
}

static void* GetAdhocDynamicLibrarySymbol(void* handle,
                                          const std::string& name,
                                          std::string* error_message) {
  if (handle == nullptr) {
    *error_message = "Library handle is unavailable";
    return nullptr;
  }
#if defined(_WIN32)
  auto* symbol = reinterpret_cast<void*>(
      GetProcAddress(static_cast<HMODULE>(handle), name.c_str()));
  if (symbol == nullptr) {
    *error_message = "Library symbol is unavailable";
  }
  return symbol;
#else
  dlerror();
  auto* symbol = dlsym(handle, name.c_str());
  const auto* message = dlerror();
  if (message != nullptr) {
    *error_message = message;
    return nullptr;
  }
  if (symbol == nullptr) {
    *error_message = "Library symbol is unavailable";
  }
  return symbol;
#endif
}

static std::string CreateStartResultJson(uint32_t handle_id,
                                         uint32_t process_id) {
  std::string json = "{\"handleId\":";
  json += std::to_string(handle_id);
  json += ",\"processId\":";
  json += std::to_string(process_id);
  json += "}";
  return json;
}

static std::string CreateWaitResultJson(const ExecutorProcess& process) {
  std::string json = "{\"processId\":";
  json += std::to_string(process.process_id);
  json += ",\"exitCode\":";
  json += std::to_string(process.exit_code);
  if (process.capture_stdout) {
    json += ",\"stdoutBase64\":";
    AppendJsonString(&json, Base64Encode(process.stdout_data));
  }
  if (process.capture_stderr) {
    json += ",\"stderrBase64\":";
    AppendJsonString(&json, Base64Encode(process.stderr_data));
  }
  json += "}";
  return json;
}

static void CompleteStringOnDispatcher(
    const std::shared_ptr<ExecutorProcess>& process,
    muon_completion_func completion,
    std::string result) {
  if (completion == nullptr) {
    return;
  }
  if (!process || !process->runtime_available ||
      process->dispatcher == nullptr) {
    return;
  }
  FireAndForgetOnDispatcher(
      process->dispatcher,
      [completion, result = std::move(result)]() {
        CompleteMuonString(completion, result);
      });
}

static void CompleteEmptyJsonOnDispatcher(
    const std::shared_ptr<ExecutorProcess>& process,
    muon_completion_func completion) {
  if (completion == nullptr) {
    return;
  }
  if (!process || !process->runtime_available ||
      process->dispatcher == nullptr) {
    return;
  }
  FireAndForgetOnDispatcher(
      process->dispatcher,
      [completion]() { CompleteMuonString(completion, "{}"); });
}

static void CompleteErrorOnDispatcher(
    const std::shared_ptr<ExecutorProcess>& process,
    muon_completion_func completion,
    std::string error_message) {
  if (completion == nullptr) {
    return;
  }
  if (!process || !process->runtime_available ||
      process->dispatcher == nullptr) {
    return;
  }
  FireAndForgetOnDispatcher(
      process->dispatcher,
      [completion, error_message = std::move(error_message)]() {
        CompleteMuonError(completion, error_message);
      });
}

static void LogExecutorMessage(const std::shared_ptr<ExecutorProcess>& process,
                               muon_log_level level,
                               const std::string& message) {
  if (!process || process->helpers == nullptr ||
      process->helpers->__log_message_impl == nullptr) {
    return;
  }
  process->helpers->__log_message_impl(level, message.c_str());
}

static void CompleteOutputCallback(void* raw_state,
                                   const muon_completion_error* error) {
  auto* state = static_cast<OutputCallbackCompletionState*>(raw_state);
  if (state == nullptr) {
    return;
  }
  if (error != nullptr && state->helpers != nullptr &&
      state->helpers->__log_message_impl != nullptr) {
    const auto message =
        error->message[0] == '\0'
            ? "executor stream callback failed"
            : std::string("executor stream callback failed: ") +
                  error->message;
    state->helpers->__log_message_impl(MUON_LOG_LEVEL_WARNING,
                                       message.c_str());
  }
  if (state->helpers != nullptr &&
      state->helpers->__release_plugin_function_pointer_impl != nullptr &&
      state->completion != nullptr) {
    state->helpers->release_plugin_function_pointer(state->completion);
  }
  auto process = state->process;
  delete state;
  if (process) {
    {
      std::lock_guard<std::mutex> lock(process->mutex);
      if (process->pending_output_callbacks > 0) {
        process->pending_output_callbacks -= 1;
      }
    }
    CompleteWaitersIfReady(process);
  }
}

static void InvokeOutputCallbackOnDispatcher(
    const std::shared_ptr<ExecutorProcess>& process,
    bool is_stdout,
    std::vector<uint8_t> chunk) {
  if (!process || chunk.empty() || !process->runtime_available ||
      process->dispatcher == nullptr) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(process->mutex);
    process->pending_output_callbacks += 1;
  }
  FireAndForgetOnDispatcher(
      process->dispatcher,
      [process, is_stdout, chunk = std::move(chunk)]() mutable {
        muon_native_function callback = nullptr;
        {
          std::lock_guard<std::mutex> lock(process->mutex);
          callback =
              is_stdout ? process->stdout_callback : process->stderr_callback;
        }
        if (callback == nullptr || process->helpers == nullptr ||
            process->helpers->__create_completion_function_impl == nullptr ||
            process->helpers->__release_plugin_function_pointer_impl ==
                nullptr) {
          {
            std::lock_guard<std::mutex> lock(process->mutex);
            if (process->pending_output_callbacks > 0) {
              process->pending_output_callbacks -= 1;
            }
          }
          CompleteWaitersIfReady(process);
          return;
        }

        auto* completion_state = new OutputCallbackCompletionState;
        completion_state->helpers = process->helpers;
        completion_state->process = process;
        char error_storage[MUON_COMPLETION_ERROR_MESSAGE_CAPACITY] = "";
        auto error = muon_error_buffer{
            error_storage,
            static_cast<uint32_t>(sizeof(error_storage)),
        };
        if (!process->helpers->create_completion_function(
                &type_void, &CompleteOutputCallback, completion_state,
                &completion_state->completion, &error)) {
          LogExecutorMessage(
              process,
              MUON_LOG_LEVEL_WARNING,
              error_storage[0] == '\0'
                  ? "executor stream callback completion registration failed"
                  : error_storage);
          delete completion_state;
          {
            std::lock_guard<std::mutex> lock(process->mutex);
            if (process->pending_output_callbacks > 0) {
              process->pending_output_callbacks -= 1;
            }
          }
          CompleteWaitersIfReady(process);
          return;
        }

        const auto view = muon_buffer_view{
            chunk.empty() ? nullptr : static_cast<void*>(chunk.data()),
            static_cast<uintptr_t>(chunk.size()),
        };
        using OutputCallbackFunction = void (*)(muon_completion_func,
                                                muon_buffer_view);
        reinterpret_cast<OutputCallbackFunction>(callback)(
            completion_state->completion, view);
      });
}

static void ReleaseProcessCallbacks(
    const std::shared_ptr<ExecutorProcess>& process) {
  if (!process) {
    return;
  }
  std::vector<muon_native_function> callbacks;
  {
    std::lock_guard<std::mutex> lock(process->mutex);
    if (process->callbacks_released) {
      return;
    }
    process->callbacks_released = true;
    if (process->owner_callback != nullptr) {
      callbacks.push_back(process->owner_callback);
      process->owner_callback = nullptr;
    }
    if (process->stdout_callback != nullptr) {
      callbacks.push_back(process->stdout_callback);
      process->stdout_callback = nullptr;
    }
    if (process->stderr_callback != nullptr) {
      callbacks.push_back(process->stderr_callback);
      process->stderr_callback = nullptr;
    }
  }
  if (callbacks.empty() || process->helpers == nullptr ||
      process->helpers->__release_plugin_function_pointer_impl == nullptr ||
      process->dispatcher == nullptr || !process->runtime_available) {
    return;
  }
  FireAndForgetOnDispatcher(
      process->dispatcher,
      [helpers = process->helpers, callbacks = std::move(callbacks)]() {
        for (const auto callback : callbacks) {
          helpers->release_plugin_function_pointer(callback);
        }
      });
}

static void CompleteWaiters(
    const std::shared_ptr<ExecutorProcess>& process) {
  std::vector<muon_completion_func> wait_completions;
  std::string result_json;
  std::string failure_message;
  {
    std::lock_guard<std::mutex> lock(process->mutex);
    wait_completions.swap(process->wait_completions);
    failure_message = process->failure_message;
    if (failure_message.empty()) {
      result_json = CreateWaitResultJson(*process);
    }
  }
  for (const auto completion : wait_completions) {
    if (failure_message.empty()) {
      CompleteStringOnDispatcher(process, completion, result_json);
    } else {
      CompleteErrorOnDispatcher(process, completion, failure_message);
    }
  }
}

static void CompleteWaitersIfReady(
    const std::shared_ptr<ExecutorProcess>& process) {
  auto ready = false;
  {
    std::lock_guard<std::mutex> lock(process->mutex);
    ready = process->exited && process->pending_output_callbacks == 0;
  }
  if (ready) {
    CompleteWaiters(process);
  }
}

static void MarkProcessExited(const std::shared_ptr<ExecutorProcess>& process,
                              int32_t exit_code,
                              const std::string& failure_message) {
  std::deque<ExecutorCommand> pending_commands;
  auto should_complete = false;
  {
    std::lock_guard<std::mutex> lock(process->mutex);
    if (process->exited) {
      return;
    }
    process->exited = true;
    process->exit_code = exit_code;
    process->failure_message = failure_message;
    pending_commands.swap(process->commands);
    should_complete = true;
  }
  if (should_complete) {
    for (const auto& command : pending_commands) {
      if (command.completion != nullptr) {
        CompleteErrorOnDispatcher(process, command.completion,
                                  "executor process has exited");
      }
    }
    CompleteWaitersIfReady(process);
  }
}

static bool RetainCallback(const muon_plugin_helpers* helpers,
                           muon_native_function callback,
                           std::string* error_message) {
  if (callback == nullptr) {
    return true;
  }
  if (helpers == nullptr ||
      helpers->__retain_plugin_function_pointer_impl == nullptr) {
    *error_message = "Function retain helper is unavailable";
    return false;
  }
  if (!helpers->retain_plugin_function_pointer(callback)) {
    *error_message = "Failed to retain renderer callback";
    return false;
  }
  return true;
}

static bool RetainProcessCallbacks(
    const muon_plugin_helpers* helpers,
    muon_native_function owner_callback,
    muon_native_function stdout_callback,
    muon_native_function stderr_callback,
    std::string* error_message) {
  if (!RetainCallback(helpers, owner_callback, error_message)) {
    return false;
  }
  if (!RetainCallback(helpers, stdout_callback, error_message)) {
    if (helpers != nullptr &&
        helpers->__release_plugin_function_pointer_impl != nullptr) {
      helpers->release_plugin_function_pointer(owner_callback);
    }
    return false;
  }
  if (!RetainCallback(helpers, stderr_callback, error_message)) {
    if (helpers != nullptr &&
        helpers->__release_plugin_function_pointer_impl != nullptr) {
      helpers->release_plugin_function_pointer(owner_callback);
      helpers->release_plugin_function_pointer(stdout_callback);
    }
    return false;
  }
  return true;
}

static ExecutorRuntime* GetExecutorRuntime() {
  std::lock_guard<std::mutex> lock(g_executor_runtime_mutex);
  return g_executor_runtime.get();
}

static std::shared_ptr<ExecutorProcess> FindExecutorProcess(
    uint32_t handle_id) {
  std::lock_guard<std::mutex> runtime_lock(g_executor_runtime_mutex);
  auto* runtime = g_executor_runtime.get();
  if (runtime == nullptr) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(runtime->mutex);
  const auto iterator = runtime->processes.find(handle_id);
  return iterator == runtime->processes.end() ? nullptr : iterator->second;
}

static std::shared_ptr<ExecutorProcess> RemoveExecutorProcess(
    uint32_t handle_id) {
  std::lock_guard<std::mutex> runtime_lock(g_executor_runtime_mutex);
  auto* runtime = g_executor_runtime.get();
  if (runtime == nullptr) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(runtime->mutex);
  const auto iterator = runtime->processes.find(handle_id);
  if (iterator == runtime->processes.end()) {
    return nullptr;
  }
  auto process = iterator->second;
  runtime->processes.erase(iterator);
  return process;
}

static std::shared_ptr<AdhocLibrary> FindAdhocLibrary(uint32_t library_id) {
  std::lock_guard<std::mutex> runtime_lock(g_executor_runtime_mutex);
  auto* runtime = g_executor_runtime.get();
  if (runtime == nullptr) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(runtime->mutex);
  const auto iterator = runtime->libraries.find(library_id);
  return iterator == runtime->libraries.end() ? nullptr : iterator->second;
}

static std::shared_ptr<AdhocLibrary> RemoveAdhocLibrary(uint32_t library_id) {
  std::lock_guard<std::mutex> runtime_lock(g_executor_runtime_mutex);
  auto* runtime = g_executor_runtime.get();
  if (runtime == nullptr) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(runtime->mutex);
  const auto iterator = runtime->libraries.find(library_id);
  if (iterator == runtime->libraries.end()) {
    return nullptr;
  }
  auto library = iterator->second;
  runtime->libraries.erase(iterator);
  return library;
}

static void FinalizeAdhocLibrary(
    const std::shared_ptr<AdhocLibrary>& library) {
  if (!library) {
    return;
  }
  std::vector<muon_native_function> functions;
  void* handle = nullptr;
  auto traffic_initialized = false;
  {
    std::lock_guard<std::mutex> lock(library->mutex);
    if (library->closed) {
      return;
    }
    library->closed = true;
    for (const auto& entry : library->functions) {
      if (entry.second && entry.second->registered_function != nullptr) {
        functions.push_back(entry.second->registered_function);
        entry.second->registered_function = nullptr;
      }
    }
    library->functions.clear();
    handle = library->handle;
    library->handle = nullptr;
    traffic_initialized = library->traffic_initialized;
    library->traffic_initialized = false;
  }
  for (const auto function : functions) {
    tra_ffic_error error;
    (void)tra_ffic_function_release(function, &error);
  }
  if (traffic_initialized) {
    tra_ffic_side_destroy(&library->callee_side);
    tra_ffic_side_destroy(&library->caller_side);
    tra_ffic_task_drain_finalization(&library->traffic_queue);
    tra_ffic_task_queue_destroy(&library->traffic_queue);
  }
  CloseAdhocDynamicLibrary(handle);
}

static void CompleteAdhocReleaseWaiters(
    const std::shared_ptr<AdhocLibrary>& library) {
  if (!library) {
    return;
  }
  std::vector<muon_completion_func> completions;
  {
    std::lock_guard<std::mutex> lock(library->mutex);
    completions.swap(library->release_completions);
  }
  for (const auto completion : completions) {
    if (library->runtime_available && library->dispatcher != nullptr) {
      FireAndForgetOnDispatcher(
          library->dispatcher,
          [completion]() { CompleteMuonString(completion, "{}"); });
    }
  }
}

static void TryFinalizeReleasedAdhocLibrary(
    const std::shared_ptr<AdhocLibrary>& library) {
  auto should_finalize = false;
  {
    std::lock_guard<std::mutex> lock(library->mutex);
    should_finalize =
        library->release_requested && library->active_calls == 0 &&
        !library->closed;
  }
  if (!should_finalize) {
    return;
  }
  FinalizeAdhocLibrary(library);
  CompleteAdhocReleaseWaiters(library);
}

static void FinishAdhocCall(const std::shared_ptr<AdhocLibrary>& library) {
  if (!library) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(library->mutex);
    if (library->active_calls > 0) {
      library->active_calls -= 1;
    }
  }
  TryFinalizeReleasedAdhocLibrary(library);
}

static void ReleaseAdhocLibrary(
    const std::shared_ptr<AdhocLibrary>& library,
    muon_completion_func completion) {
  if (!library) {
    if (completion != nullptr) {
      CompleteMuonString(completion, "{}");
    }
    return;
  }
  auto should_finalize = false;
  auto should_complete = false;
  {
    std::lock_guard<std::mutex> lock(library->mutex);
    library->release_requested = true;
    if (library->active_calls == 0 && !library->closed) {
      should_finalize = true;
      should_complete = completion != nullptr;
    } else if (library->active_calls > 0 && completion != nullptr) {
      library->release_completions.push_back(completion);
    } else if (library->closed) {
      should_complete = completion != nullptr;
    }
  }
  if (should_finalize) {
    FinalizeAdhocLibrary(library);
  }
  if (should_complete) {
    CompleteMuonString(completion, "{}");
  }
}

static bool CopyBufferView(const muon_buffer_view& data,
                           std::vector<uint8_t>* target,
                           std::string* error_message) {
  target->clear();
  if (data.data == nullptr) {
    if (data.size == 0) {
      return true;
    }
    *error_message = "data buffer is invalid";
    return false;
  }
  const auto* begin = static_cast<const uint8_t*>(data.data);
  target->assign(begin, begin + data.size);
  return true;
}

#if defined(_WIN32)

static std::wstring QuoteWindowsArgument(const std::wstring& argument) {
  if (argument.empty()) {
    return L"\"\"";
  }
  auto needs_quotes = false;
  for (const auto character : argument) {
    if (character == L' ' || character == L'\t' || character == L'"') {
      needs_quotes = true;
      break;
    }
  }
  if (!needs_quotes) {
    return argument;
  }

  std::wstring quoted = L"\"";
  auto backslashes = size_t{0};
  for (const auto character : argument) {
    if (character == L'\\') {
      ++backslashes;
      continue;
    }
    if (character == L'"') {
      quoted.append(backslashes * 2 + 1, L'\\');
      quoted.push_back(character);
      backslashes = 0;
      continue;
    }
    quoted.append(backslashes, L'\\');
    backslashes = 0;
    quoted.push_back(character);
  }
  quoted.append(backslashes * 2, L'\\');
  quoted.push_back(L'"');
  return quoted;
}

static bool CreateWindowsEnvironmentBlock(const RunOptions& options,
                                          std::vector<wchar_t>* block,
                                          std::string* error_message) {
  block->clear();
  if (!options.has_env) {
    return true;
  }

  std::map<std::string, std::string> merged;
  for (auto entry : GetMuonEnvironmentEntries()) {
    merged[std::move(entry.first)] = std::move(entry.second);
  }
  for (const auto& entry : options.env) {
    merged[entry.first] = entry.second;
  }

  for (const auto& entry : merged) {
    std::wstring key;
    std::wstring value;
    if (!MuonUtf8ToWide(entry.first, &key) ||
        !MuonUtf8ToWide(entry.second, &value)) {
      *error_message = "env entries must be valid UTF-8";
      return false;
    }
    block->insert(block->end(), key.begin(), key.end());
    block->push_back(L'=');
    block->insert(block->end(), value.begin(), value.end());
    block->push_back(L'\0');
  }
  block->push_back(L'\0');
  return true;
}

static void CloseWindowsHandle(HANDLE* handle) {
  if (*handle != nullptr) {
    CloseHandle(*handle);
    *handle = nullptr;
  }
}

static HANDLE TakeWindowsHandle(HANDLE* handle) {
  const auto taken = *handle;
  *handle = nullptr;
  return taken;
}

static bool DuplicateWindowsHandle(HANDLE source,
                                   HANDLE* duplicate,
                                   std::string* error_message) {
  *duplicate = nullptr;
  if (DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(),
                      duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS) == FALSE) {
    *error_message = "Failed to duplicate process I/O cancellation handle";
    return false;
  }
  return true;
}

static std::wstring CreateWindowsPipeName() {
  static std::atomic<uint64_t> next_pipe_id{1};
  auto name = std::wstring(L"\\\\.\\pipe\\muon-executor-");
  name += std::to_wstring(GetCurrentProcessId());
  name.push_back(L'-');
  name += std::to_wstring(next_pipe_id.fetch_add(1));
  return name;
}

// Anonymous pipe handles cannot be opened for overlapped I/O. The child keeps
// a synchronous inheritable client handle while the owning parent I/O thread
// uses the overlapped server handle.
static bool CreateWindowsOverlappedPipe(bool parent_reads,
                                        HANDLE* parent_handle,
                                        HANDLE* child_handle,
                                        std::string* error_message) {
  *parent_handle = nullptr;
  *child_handle = nullptr;
  const auto pipe_name = CreateWindowsPipeName();
  const auto parent_access =
      (parent_reads ? PIPE_ACCESS_INBOUND : PIPE_ACCESS_OUTBOUND) |
      FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE;
  auto parent = CreateNamedPipeW(
      pipe_name.c_str(), parent_access,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 64 * 1024,
      64 * 1024, 0, nullptr);
  if (parent == INVALID_HANDLE_VALUE) {
    *error_message = "Failed to create process pipe";
    return false;
  }

  auto connect_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (connect_event == nullptr) {
    CloseHandle(parent);
    *error_message = "Failed to create process pipe connection event";
    return false;
  }
  OVERLAPPED connect_overlapped = {};
  connect_overlapped.hEvent = connect_event;
  const auto connect_started = ConnectNamedPipe(parent, &connect_overlapped);
  auto connect_error =
      connect_started == FALSE ? GetLastError() : ERROR_SUCCESS;
  if (connect_started == FALSE && connect_error != ERROR_IO_PENDING &&
      connect_error != ERROR_PIPE_CONNECTED) {
    CloseHandle(connect_event);
    CloseHandle(parent);
    *error_message = "Failed to connect process pipe";
    return false;
  }

  SECURITY_ATTRIBUTES security_attributes = {};
  security_attributes.nLength = sizeof(security_attributes);
  security_attributes.bInheritHandle = TRUE;
  const auto child_access = parent_reads ? GENERIC_WRITE : GENERIC_READ;
  auto child =
      CreateFileW(pipe_name.c_str(), child_access, 0, &security_attributes,
                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (child == INVALID_HANDLE_VALUE) {
    if (connect_error == ERROR_IO_PENDING) {
      (void)CancelIoEx(parent, &connect_overlapped);
      DWORD ignored = 0;
      (void)GetOverlappedResult(parent, &connect_overlapped, &ignored, TRUE);
    }
    CloseHandle(connect_event);
    CloseHandle(parent);
    *error_message = "Failed to open process pipe";
    return false;
  }

  if (connect_error == ERROR_IO_PENDING) {
    DWORD ignored = 0;
    if (GetOverlappedResult(parent, &connect_overlapped, &ignored, TRUE) ==
        FALSE) {
      CloseHandle(child);
      CloseHandle(connect_event);
      CloseHandle(parent);
      *error_message = "Failed to connect process pipe";
      return false;
    }
  }
  CloseHandle(connect_event);
  *parent_handle = parent;
  *child_handle = child;
  return true;
}

// The I/O threads own and close the three pipe handles. These fields are only
// non-owning references protected by the process mutex so cancellation never
// races a CloseHandle call from another thread.
static void CancelWindowsProcessIoLocked(
    const std::shared_ptr<ExecutorProcess>& process,
    const std::string& error_message) {
  process->io_cancel_requested = true;
  if (process->io_cancel_error.empty()) {
    process->io_cancel_error = error_message;
  }
  if (process->io_cancel_event != nullptr) {
    (void)SetEvent(process->io_cancel_event);
  }
  if (process->stdin_write != nullptr) {
    (void)CancelIoEx(process->stdin_write, nullptr);
  }
  if (process->stdout_read != nullptr) {
    (void)CancelIoEx(process->stdout_read, nullptr);
  }
  if (process->stderr_read != nullptr) {
    (void)CancelIoEx(process->stderr_read, nullptr);
  }
}

static void WaitForWindowsIoThreads(
    const std::shared_ptr<ExecutorProcess>& process) {
  std::unique_lock<std::mutex> lock(process->mutex);
  process->io_threads_cv.wait(
      lock, [&process]() { return process->io_threads_running == 0; });
}

enum class WindowsPipeKind {
  kStdin,
  kStdout,
  kStderr,
};

static void FinishWindowsIoThread(
    const std::shared_ptr<ExecutorProcess>& process,
    HANDLE pipe,
    WindowsPipeKind kind) {
  {
    std::lock_guard<std::mutex> lock(process->mutex);
    auto* process_pipe =
        kind == WindowsPipeKind::kStdin
            ? &process->stdin_write
            : (kind == WindowsPipeKind::kStdout
                   ? &process->stdout_read
                   : &process->stderr_read);
    if (*process_pipe == pipe) {
      *process_pipe = nullptr;
    }
    if (kind == WindowsPipeKind::kStdin) {
      process->stdin_closed = true;
    }
  }
  CloseWindowsHandle(&pipe);
  {
    std::lock_guard<std::mutex> lock(process->mutex);
    if (process->io_threads_running > 0) {
      process->io_threads_running -= 1;
    }
  }
  process->io_threads_cv.notify_all();
}

static void AppendProcessOutput(const std::shared_ptr<ExecutorProcess>& process,
                                bool is_stdout,
                                const uint8_t* data,
                                size_t size) {
  if (size == 0) {
    return;
  }
  auto chunk = std::vector<uint8_t>(data, data + size);
  auto callback_enabled = false;
  {
    std::lock_guard<std::mutex> lock(process->mutex);
    if (is_stdout) {
      if (process->capture_stdout) {
        process->stdout_data.insert(process->stdout_data.end(), data,
                                    data + size);
      }
      callback_enabled = process->stdout_callback != nullptr;
    } else {
      if (process->capture_stderr) {
        process->stderr_data.insert(process->stderr_data.end(), data,
                                    data + size);
      }
      callback_enabled = process->stderr_callback != nullptr;
    }
  }
  if (callback_enabled) {
    InvokeOutputCallbackOnDispatcher(process, is_stdout, std::move(chunk));
  }
}

static void DrainWindowsPipeOutput(
    const std::shared_ptr<ExecutorProcess>& process,
    HANDLE pipe,
    HANDLE operation_event,
    bool is_stdout,
    std::vector<uint8_t>* buffer) {
  DWORD available = 0;
  if (PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) == FALSE) {
    return;
  }
  // Drain only the bytes already buffered when the root exited. A daemon
  // descendant may keep writing indefinitely after the root process is gone.
  while (available > 0) {
    const auto requested =
        std::min<DWORD>(available, static_cast<DWORD>(buffer->size()));
    (void)ResetEvent(operation_event);
    OVERLAPPED overlapped = {};
    overlapped.hEvent = operation_event;
    DWORD read_size = 0;
    if (ReadFile(pipe, buffer->data(), requested, nullptr,
                 &overlapped) == FALSE) {
      if (GetLastError() != ERROR_IO_PENDING ||
          GetOverlappedResult(pipe, &overlapped, &read_size, TRUE) == FALSE) {
        return;
      }
    } else if (GetOverlappedResult(
                   pipe, &overlapped, &read_size, FALSE) == FALSE) {
      return;
    }
    if (read_size == 0) {
      return;
    }
    AppendProcessOutput(process, is_stdout, buffer->data(),
                        static_cast<size_t>(read_size));
    available =
        read_size >= available ? 0 : available - read_size;
  }
}

static HANDLE CreateWindowsProcessOperationEvent() {
#if defined(MUON_TEST_BUILD)
  static std::atomic<bool> failure_injected{false};
  const auto* failure_requested =
      std::getenv("MUON_TEST_EXECUTOR_FAIL_OPERATION_EVENT_ONCE");
  if (failure_requested != nullptr &&
      std::strcmp(failure_requested, "1") == 0 &&
      !failure_injected.exchange(true)) {
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return nullptr;
  }
#endif
  return CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

static void RunWindowsPipeReader(std::shared_ptr<ExecutorProcess> process,
                                 HANDLE pipe,
                                 HANDLE cancel_event,
                                 HANDLE operation_event,
                                 bool is_stdout) {
  std::vector<uint8_t> buffer(4096);
  while (WaitForSingleObject(cancel_event, 0) != WAIT_OBJECT_0) {
    (void)ResetEvent(operation_event);
    OVERLAPPED overlapped = {};
    overlapped.hEvent = operation_event;
    DWORD read_size = 0;
    if (ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()),
                 nullptr, &overlapped) != FALSE) {
      if (GetOverlappedResult(
              pipe, &overlapped, &read_size, FALSE) == FALSE) {
        break;
      }
      if (read_size == 0) {
        break;
      }
      AppendProcessOutput(process, is_stdout, buffer.data(),
                          static_cast<size_t>(read_size));
      continue;
    }

    const auto read_error = GetLastError();
    if (read_error == ERROR_BROKEN_PIPE || read_error == ERROR_NO_DATA) {
      break;
    }
    if (read_error != ERROR_IO_PENDING) {
      break;
    }

    const HANDLE wait_handles[] = {
        operation_event,
        cancel_event,
    };
    const auto wait_result =
        WaitForMultipleObjects(2, wait_handles, FALSE, INFINITE);
    if (wait_result == WAIT_OBJECT_0 + 1) {
      (void)CancelIoEx(pipe, &overlapped);
      if (GetOverlappedResult(pipe, &overlapped, &read_size, TRUE) != FALSE &&
          read_size > 0) {
        AppendProcessOutput(process, is_stdout, buffer.data(),
                            static_cast<size_t>(read_size));
      }
      break;
    }
    if (wait_result != WAIT_OBJECT_0 ||
        GetOverlappedResult(pipe, &overlapped, &read_size, FALSE) == FALSE ||
        read_size == 0) {
      break;
    }
    AppendProcessOutput(process, is_stdout, buffer.data(),
                        static_cast<size_t>(read_size));
  }
  if (WaitForSingleObject(cancel_event, 0) == WAIT_OBJECT_0) {
    auto should_drain = false;
    {
      std::lock_guard<std::mutex> lock(process->mutex);
      should_drain = !process->disposed;
    }
    if (should_drain) {
      DrainWindowsPipeOutput(process, pipe, operation_event, is_stdout,
                             &buffer);
    }
  }
  CloseWindowsHandle(&operation_event);
  CloseWindowsHandle(&cancel_event);
  FinishWindowsIoThread(
      process, pipe,
      is_stdout ? WindowsPipeKind::kStdout
                : WindowsPipeKind::kStderr);
}

static void CompleteWriteCommand(
    const std::shared_ptr<ExecutorProcess>& process,
    const ExecutorCommand& command,
    const std::string& error_message) {
  if (command.completion == nullptr) {
    return;
  }
  if (error_message.empty()) {
    CompleteEmptyJsonOnDispatcher(process, command.completion);
  } else {
    CompleteErrorOnDispatcher(process, command.completion, error_message);
  }
}

static void RunWindowsCommandThread(std::shared_ptr<ExecutorProcess> process,
                                    HANDLE pipe,
                                    HANDLE cancel_event,
                                    HANDLE operation_event) {
  auto thread_error = std::string{};
  while (true) {
    ExecutorCommand command;
    {
      std::unique_lock<std::mutex> lock(process->mutex);
      process->command_cv.wait(lock, [&process]() {
        return !process->commands.empty() || process->exited ||
               process->disposed || process->io_cancel_requested;
      });
      if (process->io_cancel_requested) {
        thread_error =
            process->io_cancel_error.empty()
                ? "stdin is closed"
                : process->io_cancel_error;
        break;
      }
      if (process->commands.empty()) {
        if (process->exited || process->disposed) {
          thread_error = process->disposed
                             ? "executor process is disposed"
                             : "executor process has exited";
          break;
        }
        continue;
      }
      command = std::move(process->commands.front());
      process->commands.pop_front();
    }

    {
      std::lock_guard<std::mutex> lock(process->mutex);
      if (process->io_cancel_requested) {
        thread_error =
            process->io_cancel_error.empty()
                ? "stdin is closed"
                : process->io_cancel_error;
      }
    }
    if (!thread_error.empty()) {
      CompleteWriteCommand(process, command, thread_error);
      break;
    }

    if (command.kind == ExecutorCommand::Kind::kCloseStdin) {
      CompleteWriteCommand(process, command, "");
      thread_error = "stdin is closed";
      break;
    }

    auto error_message = std::string{};
    if (!command.data.empty()) {
      auto offset = size_t{0};
      while (offset < command.data.size()) {
        if (WaitForSingleObject(cancel_event, 0) == WAIT_OBJECT_0) {
          std::lock_guard<std::mutex> lock(process->mutex);
          error_message =
              process->io_cancel_error.empty()
                  ? "stdin is closed"
                  : process->io_cancel_error;
          break;
        }
        const auto remaining = std::min<size_t>(
            command.data.size() - offset, std::numeric_limits<DWORD>::max());
        (void)ResetEvent(operation_event);
        OVERLAPPED overlapped = {};
        overlapped.hEvent = operation_event;
        DWORD written = 0;
        if (WriteFile(pipe, command.data.data() + offset,
                      static_cast<DWORD>(remaining), nullptr,
                      &overlapped) == FALSE) {
          const auto write_error = GetLastError();
          if (write_error != ERROR_IO_PENDING) {
            error_message =
                write_error == ERROR_BROKEN_PIPE ||
                        write_error == ERROR_NO_DATA
                    ? "stdin is closed"
                    : "Failed to write stdin";
            break;
          }
          const HANDLE wait_handles[] = {
              operation_event,
              cancel_event,
          };
          const auto wait_result =
              WaitForMultipleObjects(2, wait_handles, FALSE, INFINITE);
          if (wait_result == WAIT_OBJECT_0 + 1) {
            (void)CancelIoEx(pipe, &overlapped);
            DWORD ignored = 0;
            (void)GetOverlappedResult(pipe, &overlapped, &ignored, TRUE);
            std::lock_guard<std::mutex> lock(process->mutex);
            error_message =
                process->io_cancel_error.empty()
                    ? "stdin is closed"
                    : process->io_cancel_error;
            break;
          }
          if (wait_result != WAIT_OBJECT_0 ||
              GetOverlappedResult(pipe, &overlapped, &written, FALSE) ==
                  FALSE) {
            error_message = "Failed to write stdin";
            break;
          }
        } else if (GetOverlappedResult(
                       pipe, &overlapped, &written, FALSE) == FALSE) {
          error_message = "Failed to write stdin";
          break;
        }
        if (written == 0) {
          error_message = "Failed to write stdin";
          break;
        }
        offset += static_cast<size_t>(written);
      }
    }
    {
      std::lock_guard<std::mutex> lock(process->mutex);
      if (error_message.empty() && process->io_cancel_requested) {
        error_message =
            process->io_cancel_error.empty()
                ? "stdin is closed"
                : process->io_cancel_error;
      }
    }
    CompleteWriteCommand(process, command, error_message);
    if (!error_message.empty()) {
      thread_error = error_message;
      break;
    }
  }

  std::deque<ExecutorCommand> pending_commands;
  {
    std::lock_guard<std::mutex> lock(process->mutex);
    pending_commands.swap(process->commands);
  }
  if (thread_error.empty()) {
    thread_error = "stdin is closed";
  }
  for (const auto& command : pending_commands) {
    CompleteWriteCommand(process, command, thread_error);
  }
  CloseWindowsHandle(&operation_event);
  CloseWindowsHandle(&cancel_event);
  FinishWindowsIoThread(process, pipe, WindowsPipeKind::kStdin);
}

static void RunWindowsWaitThread(std::shared_ptr<ExecutorProcess> process,
                                 HANDLE cancel_event) {
  const auto waited_process = process->windows_process.process;
  HANDLE wait_handles[2] = {
      waited_process,
      cancel_event,
  };
  const auto wait_result =
      WaitForMultipleObjects(2, wait_handles, FALSE, INFINITE);
  if (wait_result == WAIT_OBJECT_0 + 1) {
    auto process_handle = HANDLE{nullptr};
    {
      std::lock_guard<std::mutex> lock(process->mutex);
      if (process->windows_process.process == waited_process) {
        process_handle =
            TakeWindowsHandle(&process->windows_process.process);
      }
    }
    CloseWindowsHandle(&process_handle);
    CloseWindowsHandle(&cancel_event);
    return;
  }

  DWORD exit_code = 0;
  auto failure_message = std::string{};
  if (wait_result != WAIT_OBJECT_0 ||
      GetExitCodeProcess(waited_process, &exit_code) == FALSE) {
    failure_message = "Failed to wait for process";
  }
  auto process_handle = HANDLE{nullptr};
  {
    std::lock_guard<std::mutex> lock(process->mutex);
    if (!process->daemon && process->windows_process.job != nullptr) {
      (void)TerminateMuonWindowsJobProcess(
          &process->windows_process, 1u);
    }
    CancelWindowsProcessIoLocked(process, "executor process has exited");
  }
  process->command_cv.notify_all();
  WaitForWindowsIoThreads(process);
  {
    std::lock_guard<std::mutex> lock(process->mutex);
    if (process->windows_process.process == waited_process) {
      process_handle =
          TakeWindowsHandle(&process->windows_process.process);
    }
  }
  CloseWindowsHandle(&process_handle);
  CloseWindowsHandle(&cancel_event);
  MarkProcessExited(
      process, static_cast<int32_t>(exit_code), failure_message);
}

static bool StartPlatformProcess(const RunOptions& options,
                                 const std::shared_ptr<ExecutorProcess>& process,
                                 std::string* error_message) {
  HANDLE stdin_read = nullptr;
  HANDLE stdin_write = nullptr;
  HANDLE stdout_read = nullptr;
  HANDLE stdout_write = nullptr;
  HANDLE stderr_read = nullptr;
  HANDLE stderr_write = nullptr;
  auto close_pipe_handles = [&]() {
    CloseWindowsHandle(&stdin_read);
    CloseWindowsHandle(&stdin_write);
    CloseWindowsHandle(&stdout_read);
    CloseWindowsHandle(&stdout_write);
    CloseWindowsHandle(&stderr_read);
    CloseWindowsHandle(&stderr_write);
  };
  if (!CreateWindowsOverlappedPipe(
          false, &stdin_write, &stdin_read, error_message) ||
      !CreateWindowsOverlappedPipe(
          true, &stdout_read, &stdout_write, error_message) ||
      !CreateWindowsOverlappedPipe(
          true, &stderr_read, &stderr_write, error_message)) {
    close_pipe_handles();
    return false;
  }

  std::wstring command;
  if (!MuonUtf8ToWide(options.command, &command)) {
    close_pipe_handles();
    *error_message = "Command is not valid UTF-8";
    return false;
  }
  std::wstring command_line = QuoteWindowsArgument(command);
  for (const auto& arg : options.args) {
    std::wstring wide_arg;
    if (!MuonUtf8ToWide(arg, &wide_arg)) {
      close_pipe_handles();
      *error_message = "Argument is not valid UTF-8";
      return false;
    }
    command_line.push_back(L' ');
    command_line += QuoteWindowsArgument(wide_arg);
  }
  std::vector<wchar_t> mutable_command_line(
      command_line.begin(), command_line.end());
  mutable_command_line.push_back(L'\0');

  std::wstring cwd;
  const wchar_t* cwd_pointer = nullptr;
  if (options.has_cwd) {
    if (!MuonUtf8ToWide(options.cwd, &cwd)) {
      close_pipe_handles();
      *error_message = "cwd is not valid UTF-8";
      return false;
    }
    cwd_pointer = cwd.c_str();
  }
  std::vector<wchar_t> environment_block;
  void* environment_pointer = nullptr;
  DWORD creation_flags = 0;
  if (!CreateWindowsEnvironmentBlock(options, &environment_block,
                                     error_message)) {
    close_pipe_handles();
    return false;
  }
  if (options.has_env) {
    environment_pointer = environment_block.data();
    creation_flags |= CREATE_UNICODE_ENVIRONMENT;
  }
  if (options.daemon) {
    creation_flags |= CREATE_NO_WINDOW;
  }

  auto io_cancel_event =
      CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (io_cancel_event == nullptr) {
    close_pipe_handles();
    *error_message = "Failed to create process I/O cancellation";
    return false;
  }
  HANDLE stdin_cancel_event = nullptr;
  HANDLE stdout_cancel_event = nullptr;
  HANDLE stderr_cancel_event = nullptr;
  HANDLE wait_cancel_event = nullptr;
  auto close_cancel_handles = [&]() {
    CloseWindowsHandle(&io_cancel_event);
    CloseWindowsHandle(&stdin_cancel_event);
    CloseWindowsHandle(&stdout_cancel_event);
    CloseWindowsHandle(&stderr_cancel_event);
    CloseWindowsHandle(&wait_cancel_event);
  };
  if (!DuplicateWindowsHandle(io_cancel_event, &stdin_cancel_event,
                              error_message) ||
      !DuplicateWindowsHandle(io_cancel_event, &stdout_cancel_event,
                              error_message) ||
      !DuplicateWindowsHandle(io_cancel_event, &stderr_cancel_event,
                              error_message) ||
      !DuplicateWindowsHandle(io_cancel_event, &wait_cancel_event,
                              error_message)) {
    close_cancel_handles();
    close_pipe_handles();
    return false;
  }

  auto stdin_operation_event = CreateWindowsProcessOperationEvent();
  auto stdout_operation_event = CreateWindowsProcessOperationEvent();
  auto stderr_operation_event = CreateWindowsProcessOperationEvent();
  auto close_operation_events = [&]() {
    CloseWindowsHandle(&stdin_operation_event);
    CloseWindowsHandle(&stdout_operation_event);
    CloseWindowsHandle(&stderr_operation_event);
  };
  if (stdin_operation_event == nullptr ||
      stdout_operation_event == nullptr ||
      stderr_operation_event == nullptr) {
    close_operation_events();
    close_cancel_handles();
    close_pipe_handles();
    *error_message = "Failed to create process I/O worker event";
    return false;
  }

  const HANDLE inherited_handles[] = {
      stdin_read,
      stdout_write,
      stderr_write,
  };
  MuonWindowsJobProcessLaunchOptions launch_options;
  launch_options.command_line = mutable_command_line.data();
  launch_options.current_directory = cwd_pointer;
  launch_options.environment = environment_pointer;
  launch_options.creation_flags = creation_flags;
  launch_options.startup_info.dwFlags = STARTF_USESTDHANDLES;
  launch_options.startup_info.hStdInput = stdin_read;
  launch_options.startup_info.hStdOutput = stdout_write;
  launch_options.startup_info.hStdError = stderr_write;
  launch_options.inherited_handles = inherited_handles;
  launch_options.inherited_handle_count = 3;
  launch_options.lifetime =
      options.daemon
          ? MuonWindowsJobProcessLifetime::Detached
          : MuonWindowsJobProcessLifetime::KillOnOwnerClose;
  auto windows_process = MuonWindowsJobProcess{};
  auto launch_error = MuonWindowsJobProcessLaunchError{};
  if (!LaunchMuonWindowsJobProcess(
          launch_options, &windows_process, &launch_error)) {
    close_operation_events();
    close_cancel_handles();
    close_pipe_handles();
    *error_message = "Failed to start process";
    if (launch_error.message != nullptr) {
      *error_message += ": ";
      *error_message += launch_error.message;
    }
    if (launch_error.windows_error != ERROR_SUCCESS) {
      *error_message += ": ";
      *error_message += std::error_code(
                            static_cast<int>(launch_error.windows_error),
                            std::system_category())
                            .message();
    }
    return false;
  }
  CloseWindowsHandle(&stdin_read);
  CloseWindowsHandle(&stdout_write);
  CloseWindowsHandle(&stderr_write);

  process->process_id = windows_process.process_id;
  process->windows_process = windows_process;
  process->io_cancel_event = io_cancel_event;
  process->stdin_write = stdin_write;
  process->stdout_read = stdout_read;
  process->stderr_read = stderr_read;
  const auto stdin_worker_pipe = stdin_write;
  const auto stdout_worker_pipe = stdout_read;
  const auto stderr_worker_pipe = stderr_read;
  windows_process = MuonWindowsJobProcess{};
  io_cancel_event = nullptr;
  stdin_write = nullptr;
  stdout_read = nullptr;
  stderr_read = nullptr;

  // Each thread owns its pipe handle and a duplicate of the shared manual-reset
  // cancellation event. The process retains only cancellable references.
  std::array<std::thread, 4> workers;
  auto start_io_worker = [&](size_t index, auto operation) {
    {
      std::lock_guard<std::mutex> lock(process->mutex);
      process->io_threads_running += 1;
    }
    try {
      workers[index] = std::thread(std::move(operation));
    } catch (...) {
      {
        std::lock_guard<std::mutex> lock(process->mutex);
        process->io_threads_running -= 1;
      }
      process->io_threads_cv.notify_all();
      throw;
    }
  };
  try {
    start_io_worker(0, [process, stdout_worker_pipe, stdout_cancel_event,
                        stdout_operation_event]() {
      RunWindowsPipeReader(
          process, stdout_worker_pipe, stdout_cancel_event,
          stdout_operation_event, true);
    });
    stdout_cancel_event = nullptr;
    stdout_operation_event = nullptr;
    start_io_worker(1, [process, stderr_worker_pipe, stderr_cancel_event,
                        stderr_operation_event]() {
      RunWindowsPipeReader(
          process, stderr_worker_pipe, stderr_cancel_event,
          stderr_operation_event, false);
    });
    stderr_cancel_event = nullptr;
    stderr_operation_event = nullptr;
    start_io_worker(2, [process, stdin_worker_pipe, stdin_cancel_event,
                        stdin_operation_event]() {
      RunWindowsCommandThread(
          process, stdin_worker_pipe, stdin_cancel_event,
          stdin_operation_event);
    });
    stdin_cancel_event = nullptr;
    stdin_operation_event = nullptr;
    workers[3] =
        std::thread(RunWindowsWaitThread, process, wait_cancel_event);
    wait_cancel_event = nullptr;
    for (auto& worker : workers) {
      worker.detach();
    }
  } catch (...) {
    auto job_termination_succeeded = false;
    auto kill_on_close_enabled = false;
    {
      std::lock_guard<std::mutex> lock(process->mutex);
      job_termination_succeeded = TerminateMuonWindowsJobProcess(
          &process->windows_process, 1u);
      if (!job_termination_succeeded) {
        kill_on_close_enabled = EnableMuonWindowsJobKillOnClose(
            &process->windows_process);
        if (process->windows_process.process != nullptr) {
          (void)TerminateProcess(
              process->windows_process.process, 1u);
        }
      }
      CancelWindowsProcessIoLocked(
          process, "executor process setup failed");
    }
    process->command_cv.notify_all();
    for (auto& worker : workers) {
      if (worker.joinable()) {
        worker.join();
      }
    }
    WaitForWindowsIoThreads(process);

    auto remaining_process = MuonWindowsJobProcess{};
    auto remaining_cancel_event = HANDLE{nullptr};
    {
      std::lock_guard<std::mutex> lock(process->mutex);
      CloseWindowsHandle(&process->stdin_write);
      CloseWindowsHandle(&process->stdout_read);
      CloseWindowsHandle(&process->stderr_read);
      remaining_cancel_event =
          TakeWindowsHandle(&process->io_cancel_event);
      remaining_process = process->windows_process;
      process->windows_process = MuonWindowsJobProcess{};
      process->process_id = 0;
    }
    CloseWindowsHandle(&remaining_cancel_event);
    CloseMuonWindowsJobProcess(&remaining_process);
    close_operation_events();
    close_cancel_handles();
    close_pipe_handles();
    *error_message = "Failed to start process I/O workers";
    if (!job_termination_succeeded && !kill_on_close_enabled) {
      *error_message +=
          "; process tree cleanup could not be guaranteed";
    }
    return false;
  }
  return true;
}

static void WakePlatformProcess(const std::shared_ptr<ExecutorProcess>& process) {
  process->command_cv.notify_all();
}

static void RequestPlatformTerminate(
    const std::shared_ptr<ExecutorProcess>& process) {
  std::lock_guard<std::mutex> lock(process->mutex);
  if (process->windows_process.job != nullptr) {
    if (!TerminateMuonWindowsJobProcess(
            &process->windows_process, 1u)) {
      (void)EnableMuonWindowsJobKillOnClose(
          &process->windows_process);
      if (process->windows_process.process != nullptr) {
        (void)TerminateProcess(
            process->windows_process.process, 1u);
      }
    }
  }
  process->command_cv.notify_all();
}

static void ReleasePlatformProcessConnection(
    const std::shared_ptr<ExecutorProcess>& process) {
  auto job_process = MuonWindowsJobProcess{};
  {
    std::lock_guard<std::mutex> lock(process->mutex);
    CancelWindowsProcessIoLocked(process, "executor process is disposed");
    job_process.job =
        TakeWindowsHandle(&process->windows_process.job);
  }
  process->command_cv.notify_all();
  WaitForWindowsIoThreads(process);
  auto cancel_event = HANDLE{nullptr};
  {
    std::lock_guard<std::mutex> lock(process->mutex);
    cancel_event = TakeWindowsHandle(&process->io_cancel_event);
  }
  CloseWindowsHandle(&cancel_event);
  CloseMuonWindowsJobProcess(&job_process);
}

#else

static bool SetNonBlocking(int fd, std::string* error_message) {
  const auto flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    *error_message = "Failed to configure process pipe";
    return false;
  }
  return true;
}

static bool CreateCloseOnExecPipe(int fds[2], std::string* error_message) {
  if (pipe(fds) != 0) {
    *error_message = "Failed to create process pipe";
    return false;
  }
  for (auto index = 0; index < 2; ++index) {
    const auto flags = fcntl(fds[index], F_GETFD, 0);
    if (flags < 0 || fcntl(fds[index], F_SETFD, flags | FD_CLOEXEC) < 0) {
      close(fds[0]);
      close(fds[1]);
      *error_message = "Failed to configure process pipe";
      return false;
    }
  }
  return true;
}

static void CloseFd(int* fd) {
  if (*fd >= 0) {
    close(*fd);
    *fd = -1;
  }
}

static int TakeFd(int* fd) {
  const auto taken = *fd;
  *fd = -1;
  return taken;
}

#if !defined(__linux__)
static std::vector<std::string> SplitPathList(const std::string& path) {
  std::vector<std::string> entries;
  auto begin = size_t{0};
  while (begin <= path.size()) {
    const auto end = path.find(':', begin);
    const auto length =
        end == std::string::npos ? path.size() - begin : end - begin;
    entries.push_back(length == 0 ? "." : path.substr(begin, length));
    if (end == std::string::npos) {
      break;
    }
    begin = end + 1;
  }
  return entries;
}

static std::vector<std::string> CreateCommandCandidates(
    const std::string& command,
    const std::map<std::string, std::string>& environment,
    bool has_environment) {
  if (command.find('/') != std::string::npos) {
    return {command};
  }
  std::string path_value;
  if (has_environment) {
    const auto iterator = environment.find("PATH");
    path_value = iterator == environment.end() ? "" : iterator->second;
  } else {
    const auto* path = std::getenv("PATH");
    path_value = path == nullptr ? "" : path;
  }
  if (path_value.empty()) {
    path_value = "/bin:/usr/bin";
  }

  std::vector<std::string> candidates;
  for (const auto& path_entry : SplitPathList(path_value)) {
    candidates.push_back(path_entry + "/" + command);
  }
  return candidates;
}
#endif

static std::map<std::string, std::string> CreateMergedEnvironment(
    const RunOptions& options) {
  std::map<std::string, std::string> merged;
  for (auto entry : GetMuonEnvironmentEntries()) {
    if (entry.first.empty()) {
      continue;
    }
    merged[std::move(entry.first)] = std::move(entry.second);
  }
  for (const auto& entry : options.env) {
    merged[entry.first] = entry.second;
  }
  return merged;
}

static void WakePlatformProcess(const std::shared_ptr<ExecutorProcess>& process) {
  std::lock_guard<std::mutex> lock(process->mutex);
  const auto fd = process->wake_write_fd;
  if (fd < 0) {
    return;
  }
  const uint8_t value = 1;
  while (write(fd, &value, sizeof(value)) < 0) {
    if (errno == EINTR) {
      continue;
    }
    break;
  }
}

#if defined(__linux__)
static void TerminatePosixProcessGroupIfRequired(
    const std::shared_ptr<ExecutorProcess>& process);
#endif

static void RequestPlatformTerminate(
    const std::shared_ptr<ExecutorProcess>& process) {
#if defined(__linux__)
  auto control_fd = -1;
  auto supervisor_available = false;
  {
    std::lock_guard<std::mutex> lock(process->mutex);
    process->termination_requested = true;
    control_fd = process->control_fd;
    supervisor_available = process->supervisor_cleanup_delegated;
    if (control_fd >= 0) {
      std::string ignored_error;
      supervisor_available = SendMuonExecutorSupervisorKill(
          control_fd, &ignored_error);
    }
  }
  if (!supervisor_available) {
    TerminatePosixProcessGroupIfRequired(process);
  }
#else
  pid_t child = -1;
  {
    std::lock_guard<std::mutex> lock(process->mutex);
    child = process->child;
  }
  if (child > 0) {
    kill(child, SIGTERM);
  }
#endif
  WakePlatformProcess(process);
}

static void ReleasePlatformProcessConnection(
    const std::shared_ptr<ExecutorProcess>& process) {
#if defined(__linux__)
  WakePlatformProcess(process);
#else
  (void)process;
#endif
}

static void AppendProcessOutput(const std::shared_ptr<ExecutorProcess>& process,
                                bool is_stdout,
                                const uint8_t* data,
                                size_t size) {
  if (size == 0) {
    return;
  }
  auto chunk = std::vector<uint8_t>(data, data + size);
  auto callback_enabled = false;
  {
    std::lock_guard<std::mutex> lock(process->mutex);
    if (is_stdout) {
      if (process->capture_stdout) {
        process->stdout_data.insert(process->stdout_data.end(), data,
                                    data + size);
      }
      callback_enabled = process->stdout_callback != nullptr;
    } else {
      if (process->capture_stderr) {
        process->stderr_data.insert(process->stderr_data.end(), data,
                                    data + size);
      }
      callback_enabled = process->stderr_callback != nullptr;
    }
  }
  if (callback_enabled) {
    InvokeOutputCallbackOnDispatcher(process, is_stdout, std::move(chunk));
  }
}

static void DrainWakePipe(int fd) {
  uint8_t buffer[64];
  while (read(fd, buffer, sizeof(buffer)) > 0) {
  }
}

static void CompleteCloseRequests(
    const std::shared_ptr<ExecutorProcess>& process,
    std::vector<muon_completion_func>* completions) {
  for (const auto completion : *completions) {
    CompleteEmptyJsonOnDispatcher(process, completion);
  }
  completions->clear();
}

static void RejectCloseRequests(
    const std::shared_ptr<ExecutorProcess>& process,
    std::vector<muon_completion_func>* completions,
    const std::string& message) {
  for (const auto completion : *completions) {
    CompleteErrorOnDispatcher(process, completion, message);
  }
  completions->clear();
}

static void ProcessPosixCommands(
    const std::shared_ptr<ExecutorProcess>& process,
    bool* stdin_open,
    std::deque<StdinWrite>* writes,
    std::vector<muon_completion_func>* close_completions) {
  std::deque<ExecutorCommand> commands;
  {
    std::lock_guard<std::mutex> lock(process->mutex);
    commands.swap(process->commands);
  }
  while (!commands.empty()) {
    auto command = std::move(commands.front());
    commands.pop_front();
    if (command.kind == ExecutorCommand::Kind::kWriteStdin) {
      if (!*stdin_open) {
        CompleteErrorOnDispatcher(process, command.completion,
                                  "stdin is closed");
        continue;
      }
      writes->push_back(
          {std::move(command.data), 0, command.completion});
      continue;
    }
    {
      std::lock_guard<std::mutex> lock(process->mutex);
      process->stdin_close_requested = true;
    }
    close_completions->push_back(command.completion);
  }
}

static void TryClosePosixStdin(
    const std::shared_ptr<ExecutorProcess>& process,
    bool* stdin_open,
    int* stdin_fd,
    const std::deque<StdinWrite>& writes,
    std::vector<muon_completion_func>* close_completions) {
  auto close_requested = false;
  {
    std::lock_guard<std::mutex> lock(process->mutex);
    close_requested = process->stdin_close_requested;
  }
  if (!*stdin_open || !close_requested || !writes.empty()) {
    return;
  }
  CloseFd(stdin_fd);
  *stdin_open = false;
  {
    std::lock_guard<std::mutex> lock(process->mutex);
    process->stdin_closed = true;
  }
  CompleteCloseRequests(process, close_completions);
}

static void RejectPosixWrites(
    const std::shared_ptr<ExecutorProcess>& process,
    std::deque<StdinWrite>* writes,
    const std::string& message) {
  while (!writes->empty()) {
    const auto completion = writes->front().completion;
    writes->pop_front();
    CompleteErrorOnDispatcher(process, completion, message);
  }
}

#if defined(__linux__)
static constexpr size_t kPosixOutputReadBudgetBytes = 64 * 1024;

static void ReadAndClosePosixOutput(
    const std::shared_ptr<ExecutorProcess>& process,
    bool is_stdout,
    int* fd,
    bool* open,
    size_t read_budget) {
  auto remaining = read_budget;
  while (*open && remaining > 0) {
    uint8_t buffer[4096];
    const auto requested = std::min(remaining, sizeof(buffer));
    const auto read_size = read(*fd, buffer, requested);
    if (read_size > 0) {
      AppendProcessOutput(
          process, is_stdout, buffer, static_cast<size_t>(read_size));
      remaining -= static_cast<size_t>(read_size);
      continue;
    }
    if (read_size < 0 && errno == EINTR) {
      continue;
    }
    if (read_size < 0 &&
        (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return;
    }
    CloseFd(fd);
    *open = false;
  }
}

static size_t GetBufferedPosixOutputSize(int fd) {
  auto buffered_bytes = int{0};
  if (fd >= 0 &&
      ioctl(fd, FIONREAD, &buffered_bytes) == 0 &&
      buffered_bytes >= 0) {
    return static_cast<size_t>(buffered_bytes);
  }
  return kPosixOutputReadBudgetBytes;
}

static void ClosePosixControlConnection(
    const std::shared_ptr<ExecutorProcess>& process,
    int* control_fd) {
  {
    std::lock_guard<std::mutex> lock(process->mutex);
    if (process->control_fd == *control_fd) {
      process->control_fd = -1;
    }
  }
  CloseFd(control_fd);
}

static void ForceKillPosixProcessGroup(pid_t process_group_id) {
  if (process_group_id > 0) {
    (void)kill(-process_group_id, SIGKILL);
  }
}

static void TerminatePosixProcessGroupIfRequired(
    const std::shared_ptr<ExecutorProcess>& process) {
  auto should_terminate = false;
  auto target_process_group_id = pid_t{-1};
  {
    std::lock_guard<std::mutex> lock(process->mutex);
    should_terminate =
        (!process->daemon || process->termination_requested) &&
        !process->process_group_force_kill_issued;
    target_process_group_id = process->target_process_group_id;
    if (should_terminate && target_process_group_id > 0) {
      process->process_group_force_kill_issued = true;
    }
  }
  if (should_terminate && target_process_group_id > 0) {
    ForceKillPosixProcessGroup(target_process_group_id);
  }
}

static bool ReadPosixSupervisorMessage(
    const std::shared_ptr<ExecutorProcess>& process,
    int* control_fd,
    bool* root_running,
    int32_t* exit_code,
    std::string* failure_message) {
  MuonExecutorSupervisorMessage message;
  std::string receive_error;
  const auto result = ReceiveMuonExecutorSupervisorMessage(
      *control_fd, &message, &receive_error);
  if (result == MuonExecutorSupervisorReceiveResult::kClosed) {
    auto disposed = false;
    {
      std::lock_guard<std::mutex> lock(process->mutex);
      disposed = process->disposed;
    }
    if (*root_running && !disposed) {
      *failure_message =
          "Executor supervisor closed before the process exited";
      *root_running = false;
    }
    ClosePosixControlConnection(process, control_fd);
    TerminatePosixProcessGroupIfRequired(process);
    return false;
  }
  if (result == MuonExecutorSupervisorReceiveResult::kError) {
    *failure_message = receive_error.empty()
                           ? "Failed to read executor supervisor"
                           : receive_error;
    *root_running = false;
    ClosePosixControlConnection(process, control_fd);
    TerminatePosixProcessGroupIfRequired(process);
    return false;
  }

  switch (message.type) {
    case MuonExecutorSupervisorMessageType::kAck:
      return true;
    case MuonExecutorSupervisorMessageType::kExit:
      *exit_code = message.value;
      *root_running = false;
      return true;
    case MuonExecutorSupervisorMessageType::kError:
      *failure_message =
          message.text.empty() ? "Executor supervisor failed"
                               : message.text;
      *root_running = false;
      ClosePosixControlConnection(process, control_fd);
      TerminatePosixProcessGroupIfRequired(process);
      return false;
    case MuonExecutorSupervisorMessageType::kConfig:
    case MuonExecutorSupervisorMessageType::kReady:
    case MuonExecutorSupervisorMessageType::kKill:
      *failure_message =
          "Executor supervisor returned an unexpected message";
      *root_running = false;
      ClosePosixControlConnection(process, control_fd);
      TerminatePosixProcessGroupIfRequired(process);
      return false;
  }
  return false;
}

static bool ReapPosixSupervisor(pid_t process_id) {
  if (process_id <= 0) {
    return false;
  }
  int status = 0;
  while (true) {
    const auto result = waitpid(process_id, &status, 0);
    if (result == process_id) {
      return !WIFEXITED(status) || WEXITSTATUS(status) != 0;
    }
    if (result < 0 && errno == EINTR) {
      continue;
    }
    return true;
  }
}

static void RunPosixProcessThread(std::shared_ptr<ExecutorProcess> process) {
  int stdin_fd = -1;
  int stdout_fd = -1;
  int stderr_fd = -1;
  int wake_read_fd = -1;
  int control_fd = -1;
  pid_t supervisor_process_id = -1;
  {
    std::lock_guard<std::mutex> lock(process->mutex);
    stdin_fd = TakeFd(&process->stdin_fd);
    stdout_fd = TakeFd(&process->stdout_fd);
    stderr_fd = TakeFd(&process->stderr_fd);
    wake_read_fd = TakeFd(&process->wake_read_fd);
    control_fd = process->control_fd;
    supervisor_process_id = process->supervisor_process_id;
    process->supervisor_process_id = -1;
  }
  auto stdin_open = stdin_fd >= 0;
  auto stdout_open = stdout_fd >= 0;
  auto stderr_open = stderr_fd >= 0;
  auto root_running = true;
  auto exit_reported = false;
  auto exit_code = int32_t{-1};
  std::deque<StdinWrite> writes;
  std::vector<muon_completion_func> close_completions;
  std::string failure_message;

  while (true) {
    auto disposed = false;
    {
      std::lock_guard<std::mutex> lock(process->mutex);
      disposed = process->disposed;
    }
    if (disposed) {
      auto terminate_daemon = false;
      {
        std::lock_guard<std::mutex> lock(process->mutex);
        if (process->daemon) {
          terminate_daemon = process->termination_requested;
        } else {
          process->supervisor_cleanup_delegated = true;
        }
      }
      if (terminate_daemon) {
        TerminatePosixProcessGroupIfRequired(process);
      }
      ClosePosixControlConnection(process, &control_fd);
      break;
    }

    if (root_running) {
      ProcessPosixCommands(
          process, &stdin_open, &writes, &close_completions);
      TryClosePosixStdin(
          process, &stdin_open, &stdin_fd, writes,
          &close_completions);
    } else {
      if (stdin_open) {
        CloseFd(&stdin_fd);
        stdin_open = false;
        {
          std::lock_guard<std::mutex> lock(process->mutex);
          process->stdin_closed = true;
        }
      }
      RejectPosixWrites(
          process, &writes, "executor process has exited");
      RejectCloseRequests(
          process, &close_completions,
          "executor process has exited");
      const auto buffered_stdout =
          GetBufferedPosixOutputSize(stdout_fd);
      const auto buffered_stderr =
          GetBufferedPosixOutputSize(stderr_fd);
      ReadAndClosePosixOutput(
          process, true, &stdout_fd, &stdout_open,
          buffered_stdout);
      ReadAndClosePosixOutput(
          process, false, &stderr_fd, &stderr_open,
          buffered_stderr);
      CloseFd(&stdout_fd);
      CloseFd(&stderr_fd);
      stdout_open = false;
      stderr_open = false;
      if (!exit_reported) {
        exit_reported = true;
        MarkProcessExited(process, exit_code, failure_message);
      }
      if (!process->daemon || control_fd < 0) {
        ClosePosixControlConnection(process, &control_fd);
        break;
      }
    }

    std::vector<pollfd> poll_fds;
    enum class PollSource {
      kWake,
      kStdin,
      kStdout,
      kStderr,
      kControl,
    };
    std::vector<PollSource> poll_sources;
    if (wake_read_fd >= 0) {
      poll_fds.push_back({wake_read_fd, POLLIN, 0});
      poll_sources.push_back(PollSource::kWake);
    }
    if (stdin_open && !writes.empty()) {
      poll_fds.push_back({stdin_fd, POLLOUT, 0});
      poll_sources.push_back(PollSource::kStdin);
    }
    if (stdout_open) {
      poll_fds.push_back({stdout_fd, POLLIN, 0});
      poll_sources.push_back(PollSource::kStdout);
    }
    if (stderr_open) {
      poll_fds.push_back({stderr_fd, POLLIN, 0});
      poll_sources.push_back(PollSource::kStderr);
    }
    if (control_fd >= 0) {
      poll_fds.push_back({control_fd, POLLIN, 0});
      poll_sources.push_back(PollSource::kControl);
    }
    if (poll_fds.empty()) {
      failure_message =
          "Executor process connection closed unexpectedly";
      root_running = false;
      continue;
    }

    if (poll(poll_fds.data(), poll_fds.size(), -1) < 0) {
      if (errno == EINTR) {
        continue;
      }
      failure_message = "Failed to poll process pipes";
      RequestPlatformTerminate(process);
      root_running = false;
      continue;
    }

    for (auto index = size_t{0}; index < poll_fds.size(); ++index) {
      const auto events = poll_fds[index].revents;
      if (events == 0) {
        continue;
      }
      switch (poll_sources[index]) {
        case PollSource::kWake:
          DrainWakePipe(wake_read_fd);
          break;
        case PollSource::kStdin:
          while (stdin_open && !writes.empty()) {
            auto& write_request = writes.front();
            if (write_request.offset >= write_request.data.size()) {
              CompleteEmptyJsonOnDispatcher(
                  process, write_request.completion);
              writes.pop_front();
              continue;
            }
            const auto remaining =
                write_request.data.size() - write_request.offset;
            const auto written =
                write(
                    stdin_fd,
                    write_request.data.data() + write_request.offset,
                    remaining);
            if (written > 0) {
              write_request.offset += static_cast<size_t>(written);
              continue;
            }
            if (written < 0 &&
                (errno == EAGAIN || errno == EWOULDBLOCK ||
                 errno == EINTR)) {
              break;
            }
            CloseFd(&stdin_fd);
            stdin_open = false;
            {
              std::lock_guard<std::mutex> lock(process->mutex);
              process->stdin_closed = true;
            }
            RejectPosixWrites(process, &writes, "stdin is closed");
            RejectCloseRequests(
                process, &close_completions, "stdin is closed");
            break;
          }
          break;
        case PollSource::kStdout:
          ReadAndClosePosixOutput(
              process, true, &stdout_fd, &stdout_open,
              kPosixOutputReadBudgetBytes);
          break;
        case PollSource::kStderr:
          ReadAndClosePosixOutput(
              process, false, &stderr_fd, &stderr_open,
              kPosixOutputReadBudgetBytes);
          break;
        case PollSource::kControl:
          (void)ReadPosixSupervisorMessage(
              process, &control_fd, &root_running, &exit_code,
              &failure_message);
          break;
      }
    }
  }

  CloseFd(&stdin_fd);
  CloseFd(&stdout_fd);
  CloseFd(&stderr_fd);
  ClosePosixControlConnection(process, &control_fd);
  CloseFd(&wake_read_fd);
  {
    std::lock_guard<std::mutex> lock(process->mutex);
    CloseFd(&process->wake_write_fd);
    process->stdin_closed = true;
  }
  RejectPosixWrites(
      process, &writes, "executor process has exited");
  RejectCloseRequests(
      process, &close_completions, "executor process has exited");
  if (supervisor_process_id > 0) {
    if (ReapPosixSupervisor(supervisor_process_id)) {
      TerminatePosixProcessGroupIfRequired(process);
    } else {
      std::lock_guard<std::mutex> lock(process->mutex);
      process->target_process_group_id = -1;
    }
  }
}

static bool StartPlatformProcess(const RunOptions& options,
                                 const std::shared_ptr<ExecutorProcess>& process,
                                 std::string* error_message) {
  int stdin_pipe[2] = {-1, -1};
  int stdout_pipe[2] = {-1, -1};
  int stderr_pipe[2] = {-1, -1};
  int wake_pipe[2] = {-1, -1};
  if (!CreateCloseOnExecPipe(stdin_pipe, error_message) ||
      !CreateCloseOnExecPipe(stdout_pipe, error_message) ||
      !CreateCloseOnExecPipe(stderr_pipe, error_message) ||
      !CreateCloseOnExecPipe(wake_pipe, error_message)) {
    CloseFd(&stdin_pipe[0]);
    CloseFd(&stdin_pipe[1]);
    CloseFd(&stdout_pipe[0]);
    CloseFd(&stdout_pipe[1]);
    CloseFd(&stderr_pipe[0]);
    CloseFd(&stderr_pipe[1]);
    CloseFd(&wake_pipe[0]);
    CloseFd(&wake_pipe[1]);
    return false;
  }

  MuonLinuxExecutorSupervisorLaunchOptions launch_options;
  launch_options.supervisor_path =
      (GetMuonExecutableDirectory() / "muon-executor-supervisor").string();
  launch_options.config.command = options.command;
  launch_options.config.arguments = options.args;
  launch_options.config.cwd = options.cwd;
  launch_options.config.has_cwd = options.has_cwd;
  launch_options.config.has_environment = options.has_env;
  launch_options.config.daemon = options.daemon;
  if (options.has_env) {
    const auto merged_environment = CreateMergedEnvironment(options);
    launch_options.config.environment.reserve(
        merged_environment.size());
    for (const auto& entry : merged_environment) {
      launch_options.config.environment.push_back(
          entry.first + "=" + entry.second);
    }
  }
  launch_options.target_stdin_fd = stdin_pipe[0];
  launch_options.target_stdout_fd = stdout_pipe[1];
  launch_options.target_stderr_fd = stderr_pipe[1];

  MuonLinuxExecutorSupervisorConnection connection;
  if (!LaunchMuonLinuxExecutorSupervisor(
          launch_options, &connection, error_message)) {
    CloseFd(&stdin_pipe[0]);
    CloseFd(&stdin_pipe[1]);
    CloseFd(&stdout_pipe[0]);
    CloseFd(&stdout_pipe[1]);
    CloseFd(&stderr_pipe[0]);
    CloseFd(&stderr_pipe[1]);
    CloseFd(&wake_pipe[0]);
    CloseFd(&wake_pipe[1]);
    return false;
  }
  CloseFd(&stdin_pipe[0]);
  CloseFd(&stdout_pipe[1]);
  CloseFd(&stderr_pipe[1]);
  if (!SetNonBlocking(stdin_pipe[1], error_message) ||
      !SetNonBlocking(stdout_pipe[0], error_message) ||
      !SetNonBlocking(stderr_pipe[0], error_message) ||
      !SetNonBlocking(wake_pipe[0], error_message) ||
      !SetNonBlocking(wake_pipe[1], error_message)) {
    std::string ignored_error;
    (void)SendMuonExecutorSupervisorKill(
        connection.control_fd, &ignored_error);
    CloseFd(&connection.control_fd);
    CloseFd(&stdin_pipe[1]);
    CloseFd(&stdout_pipe[0]);
    CloseFd(&stderr_pipe[0]);
    CloseFd(&wake_pipe[0]);
    CloseFd(&wake_pipe[1]);
    ForceKillPosixProcessGroup(connection.target_process_group_id);
    (void)ReapPosixSupervisor(connection.supervisor_process_id);
    return false;
  }

  process->supervisor_process_id =
      connection.supervisor_process_id;
  process->target_process_group_id =
      connection.target_process_group_id;
  process->control_fd = connection.control_fd;
  process->process_id =
      static_cast<uint32_t>(connection.target_process_id);
  process->stdin_fd = stdin_pipe[1];
  process->stdout_fd = stdout_pipe[0];
  process->stderr_fd = stderr_pipe[0];
  process->wake_read_fd = wake_pipe[0];
  process->wake_write_fd = wake_pipe[1];

  auto worker = std::thread{};
  try {
    worker = std::thread(RunPosixProcessThread, process);
  } catch (...) {
    std::string ignored_error;
    (void)SendMuonExecutorSupervisorKill(
        process->control_fd, &ignored_error);
    CloseFd(&process->control_fd);
    CloseFd(&process->stdin_fd);
    CloseFd(&process->stdout_fd);
    CloseFd(&process->stderr_fd);
    CloseFd(&process->wake_read_fd);
    CloseFd(&process->wake_write_fd);
    ForceKillPosixProcessGroup(process->target_process_group_id);
    (void)ReapPosixSupervisor(process->supervisor_process_id);
    process->supervisor_process_id = -1;
    process->target_process_group_id = -1;
    process->process_id = 0;
    *error_message = "Failed to start process I/O worker";
    return false;
  }
  try {
    worker.detach();
  } catch (...) {
    {
      std::lock_guard<std::mutex> lock(process->mutex);
      process->disposed = true;
      process->termination_requested = true;
    }
    WakePlatformProcess(process);
    if (worker.joinable()) {
      worker.join();
    }
    process->process_id = 0;
    *error_message = "Failed to detach process I/O worker";
    return false;
  }
  return true;
}

#else

static void RunPosixWaitThread(std::shared_ptr<ExecutorProcess> process) {
  int status = 0;
  auto failed = false;
  while (waitpid(process->child, &status, 0) < 0) {
    if (errno == EINTR) {
      continue;
    }
    failed = true;
    break;
  }
  {
    std::lock_guard<std::mutex> lock(process->mutex);
    process->wait_failed = failed;
    process->wait_status = status;
    process->wait_status_ready = true;
  }
  WakePlatformProcess(process);
}

static void RunPosixProcessThread(std::shared_ptr<ExecutorProcess> process) {
  int stdin_fd = -1;
  int stdout_fd = -1;
  int stderr_fd = -1;
  int wake_read_fd = -1;
  {
    std::lock_guard<std::mutex> lock(process->mutex);
    stdin_fd = TakeFd(&process->stdin_fd);
    stdout_fd = TakeFd(&process->stdout_fd);
    stderr_fd = TakeFd(&process->stderr_fd);
    wake_read_fd = TakeFd(&process->wake_read_fd);
  }
  auto stdin_open = stdin_fd >= 0;
  auto stdout_open = stdout_fd >= 0;
  auto stderr_open = stderr_fd >= 0;
  auto child_running = true;
  std::deque<StdinWrite> writes;
  std::vector<muon_completion_func> close_completions;
  std::string failure_message;

  while (child_running || stdout_open || stderr_open || stdin_open) {
    ProcessPosixCommands(process, &stdin_open, &writes, &close_completions);
    TryClosePosixStdin(process, &stdin_open, &stdin_fd, writes,
                       &close_completions);

    {
      std::lock_guard<std::mutex> lock(process->mutex);
      if (process->wait_status_ready && child_running) {
        child_running = false;
        if (process->wait_failed) {
          failure_message = "Failed to wait for process";
        }
      }
    }
    if (!child_running && stdin_open) {
      CloseFd(&stdin_fd);
      stdin_open = false;
      {
        std::lock_guard<std::mutex> lock(process->mutex);
        process->stdin_closed = true;
      }
      RejectPosixWrites(process, &writes, "executor process has exited");
      RejectCloseRequests(process, &close_completions,
                          "executor process has exited");
    }
    if (!child_running && !stdin_open && !stdout_open && !stderr_open) {
      break;
    }

    std::vector<pollfd> poll_fds;
    enum class PollSource {
      kWake,
      kStdin,
      kStdout,
      kStderr,
    };
    std::vector<PollSource> poll_sources;
    if (wake_read_fd >= 0) {
      poll_fds.push_back({wake_read_fd, POLLIN, 0});
      poll_sources.push_back(PollSource::kWake);
    }
    if (stdin_open && !writes.empty()) {
      poll_fds.push_back({stdin_fd, POLLOUT, 0});
      poll_sources.push_back(PollSource::kStdin);
    }
    if (stdout_open) {
      poll_fds.push_back({stdout_fd, POLLIN, 0});
      poll_sources.push_back(PollSource::kStdout);
    }
    if (stderr_open) {
      poll_fds.push_back({stderr_fd, POLLIN, 0});
      poll_sources.push_back(PollSource::kStderr);
    }
    if (poll_fds.empty()) {
      break;
    }

    if (poll(poll_fds.data(), poll_fds.size(), -1) < 0) {
      if (errno == EINTR) {
        continue;
      }
      failure_message = "Failed to poll process pipes";
      RequestPlatformTerminate(process);
      break;
    }

    for (auto index = size_t{0}; index < poll_fds.size(); ++index) {
      const auto events = poll_fds[index].revents;
      if (events == 0) {
        continue;
      }
      switch (poll_sources[index]) {
        case PollSource::kWake:
          DrainWakePipe(wake_read_fd);
          break;
        case PollSource::kStdin:
          while (stdin_open && !writes.empty()) {
            auto& write_request = writes.front();
            if (write_request.offset >= write_request.data.size()) {
              CompleteEmptyJsonOnDispatcher(process,
                                            write_request.completion);
              writes.pop_front();
              continue;
            }
            const auto remaining =
                write_request.data.size() - write_request.offset;
            const auto written =
                write(stdin_fd,
                      write_request.data.data() + write_request.offset,
                      remaining);
            if (written > 0) {
              write_request.offset += static_cast<size_t>(written);
              continue;
            }
            if (written < 0 &&
                (errno == EAGAIN || errno == EWOULDBLOCK ||
                 errno == EINTR)) {
              break;
            }
            CloseFd(&stdin_fd);
            stdin_open = false;
            {
              std::lock_guard<std::mutex> lock(process->mutex);
              process->stdin_closed = true;
            }
            RejectPosixWrites(process, &writes, "stdin is closed");
            RejectCloseRequests(process, &close_completions,
                                "stdin is closed");
            break;
          }
          break;
        case PollSource::kStdout:
        case PollSource::kStderr: {
          const auto is_stdout = poll_sources[index] == PollSource::kStdout;
          auto* fd = is_stdout ? &stdout_fd : &stderr_fd;
          auto* open = is_stdout ? &stdout_open : &stderr_open;
          while (*open) {
            uint8_t buffer[4096];
            const auto read_size = read(*fd, buffer, sizeof(buffer));
            if (read_size > 0) {
              AppendProcessOutput(process, is_stdout, buffer,
                                  static_cast<size_t>(read_size));
              continue;
            }
            if (read_size < 0 &&
                (errno == EAGAIN || errno == EWOULDBLOCK ||
                 errno == EINTR)) {
              break;
            }
            CloseFd(fd);
            *open = false;
            break;
          }
          break;
        }
      }
    }
  }

  CloseFd(&stdin_fd);
  CloseFd(&stdout_fd);
  CloseFd(&stderr_fd);
  CloseFd(&wake_read_fd);
  {
    std::lock_guard<std::mutex> lock(process->mutex);
    CloseFd(&process->wake_write_fd);
    process->stdin_closed = true;
  }
  RejectPosixWrites(process, &writes, "executor process has exited");
  RejectCloseRequests(process, &close_completions,
                      "executor process has exited");

  int status = 0;
  auto wait_failed = false;
  {
    std::lock_guard<std::mutex> lock(process->mutex);
    status = process->wait_status;
    wait_failed = process->wait_failed || !process->wait_status_ready;
  }
  auto exit_code = int32_t{-1};
  if (wait_failed) {
    failure_message = failure_message.empty() ? "Failed to wait for process"
                                              : failure_message;
  } else if (WIFEXITED(status)) {
    exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    exit_code = 128 + WTERMSIG(status);
  }
  MarkProcessExited(process, exit_code, failure_message);
}

static bool StartPlatformProcess(const RunOptions& options,
                                 const std::shared_ptr<ExecutorProcess>& process,
                                 std::string* error_message) {
  int stdin_pipe[2] = {-1, -1};
  int stdout_pipe[2] = {-1, -1};
  int stderr_pipe[2] = {-1, -1};
  int wake_pipe[2] = {-1, -1};
  if (!CreateCloseOnExecPipe(stdin_pipe, error_message) ||
      !CreateCloseOnExecPipe(stdout_pipe, error_message) ||
      !CreateCloseOnExecPipe(stderr_pipe, error_message) ||
      !CreateCloseOnExecPipe(wake_pipe, error_message)) {
    CloseFd(&stdin_pipe[0]);
    CloseFd(&stdin_pipe[1]);
    CloseFd(&stdout_pipe[0]);
    CloseFd(&stdout_pipe[1]);
    CloseFd(&stderr_pipe[0]);
    CloseFd(&stderr_pipe[1]);
    CloseFd(&wake_pipe[0]);
    CloseFd(&wake_pipe[1]);
    return false;
  }

  std::vector<std::string> argv_storage;
  argv_storage.reserve(options.args.size() + 1);
  argv_storage.push_back(options.command);
  for (const auto& arg : options.args) {
    argv_storage.push_back(arg);
  }
  std::vector<char*> argv;
  argv.reserve(argv_storage.size() + 1);
  for (auto& arg : argv_storage) {
    argv.push_back(arg.data());
  }
  argv.push_back(nullptr);

  const auto merged_environment = CreateMergedEnvironment(options);
  std::vector<std::string> env_storage;
  std::vector<char*> envp;
  char** environment_pointer = environ;
  if (options.has_env) {
    env_storage.reserve(merged_environment.size());
    for (const auto& entry : merged_environment) {
      env_storage.push_back(entry.first + "=" + entry.second);
    }
    envp.reserve(env_storage.size() + 1);
    for (auto& entry : env_storage) {
      envp.push_back(entry.data());
    }
    envp.push_back(nullptr);
    environment_pointer = envp.data();
  }
  const auto command_candidates = CreateCommandCandidates(
      options.command, merged_environment, options.has_env);

  const auto child = fork();
  if (child < 0) {
    CloseFd(&stdin_pipe[0]);
    CloseFd(&stdin_pipe[1]);
    CloseFd(&stdout_pipe[0]);
    CloseFd(&stdout_pipe[1]);
    CloseFd(&stderr_pipe[0]);
    CloseFd(&stderr_pipe[1]);
    CloseFd(&wake_pipe[0]);
    CloseFd(&wake_pipe[1]);
    *error_message = "Failed to start process";
    return false;
  }

  if (child == 0) {
    dup2(stdin_pipe[0], STDIN_FILENO);
    dup2(stdout_pipe[1], STDOUT_FILENO);
    dup2(stderr_pipe[1], STDERR_FILENO);
    close(stdin_pipe[0]);
    close(stdin_pipe[1]);
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);
    close(stderr_pipe[0]);
    close(stderr_pipe[1]);
    close(wake_pipe[0]);
    close(wake_pipe[1]);
    if (options.has_cwd && chdir(options.cwd.c_str()) != 0) {
      _exit(126);
    }
    for (const auto& candidate : command_candidates) {
      execve(candidate.c_str(), argv.data(), environment_pointer);
    }
    _exit(errno == ENOENT ? 127 : 126);
  }

  CloseFd(&stdin_pipe[0]);
  CloseFd(&stdout_pipe[1]);
  CloseFd(&stderr_pipe[1]);
  if (!SetNonBlocking(stdin_pipe[1], error_message) ||
      !SetNonBlocking(stdout_pipe[0], error_message) ||
      !SetNonBlocking(stderr_pipe[0], error_message) ||
      !SetNonBlocking(wake_pipe[0], error_message) ||
      !SetNonBlocking(wake_pipe[1], error_message)) {
    kill(child, SIGTERM);
    CloseFd(&stdin_pipe[1]);
    CloseFd(&stdout_pipe[0]);
    CloseFd(&stderr_pipe[0]);
    CloseFd(&wake_pipe[0]);
    CloseFd(&wake_pipe[1]);
    return false;
  }

  process->child = child;
  process->process_id = static_cast<uint32_t>(child);
  process->stdin_fd = stdin_pipe[1];
  process->stdout_fd = stdout_pipe[0];
  process->stderr_fd = stderr_pipe[0];
  process->wake_read_fd = wake_pipe[0];
  process->wake_write_fd = wake_pipe[1];

  std::thread(RunPosixWaitThread, process).detach();
  std::thread(RunPosixProcessThread, process).detach();
  return true;
}

#endif

#endif

static bool StartExecutorProcess(const SpawnRpcRequest& request,
                                 int renderer_context_id,
                                 muon_native_function owner_callback,
                                 muon_native_function stdout_callback,
                                 muon_native_function stderr_callback,
                                 std::shared_ptr<ExecutorProcess>* process_out,
                                 std::string* result_json,
                                 std::string* error_message) {
  auto* runtime = GetExecutorRuntime();
  if (runtime == nullptr || runtime->shutting_down) {
    *error_message = "executor runtime is unavailable";
    return false;
  }
  if (renderer_context_id <= 0) {
    *error_message = "renderer context id is unavailable";
    return false;
  }
  if (!RetainProcessCallbacks(runtime->helpers, owner_callback,
                              stdout_callback, stderr_callback,
                              error_message)) {
    return false;
  }

  auto process = std::make_shared<ExecutorProcess>();
  process->renderer_context_id = renderer_context_id;
  process->helpers = runtime->helpers;
  process->dispatcher = runtime->dispatcher;
  process->capture_stdout = request.capture_stdout;
  process->capture_stderr = request.capture_stderr;
  process->daemon = request.options.daemon;
  process->owner_callback = owner_callback;
  process->stdout_callback = stdout_callback;
  process->stderr_callback = stderr_callback;

  {
    std::lock_guard<std::mutex> lock(runtime->mutex);
    process->handle_id = runtime->next_handle_id;
    runtime->next_handle_id += 1;
  }

  if (!StartPlatformProcess(request.options, process, error_message)) {
    ReleaseProcessCallbacks(process);
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(runtime->mutex);
    runtime->processes[process->handle_id] = process;
  }
  *result_json = CreateStartResultJson(process->handle_id,
                                       process->process_id);
  if (process_out != nullptr) {
    *process_out = process;
  }
  return true;
}

static bool QueueExecutorCommand(
    const std::shared_ptr<ExecutorProcess>& process,
    ExecutorCommand command,
    std::string* error_message) {
  {
    std::lock_guard<std::mutex> lock(process->mutex);
    if (process->disposed) {
      *error_message = "executor process is disposed";
      return false;
    }
    if (process->exited) {
      *error_message = "executor process has exited";
      return false;
    }
    if (command.kind == ExecutorCommand::Kind::kWriteStdin &&
        (process->stdin_close_requested || process->stdin_closed)) {
      *error_message = "stdin is closed";
      return false;
    }
    if (command.kind == ExecutorCommand::Kind::kCloseStdin) {
      if (process->stdin_closed) {
        return true;
      }
      process->stdin_close_requested = true;
    }
    process->commands.push_back(std::move(command));
  }
  WakePlatformProcess(process);
  return true;
}

static bool QueueExecutorCloseStdin(
    const std::shared_ptr<ExecutorProcess>& process,
    muon_completion_func completion,
    bool* completed_immediately,
    std::string* error_message) {
  *completed_immediately = false;
  {
    std::lock_guard<std::mutex> lock(process->mutex);
    if (process->disposed) {
      *error_message = "executor process is disposed";
      return false;
    }
    if (process->exited) {
      *error_message = "executor process has exited";
      return false;
    }
    if (process->stdin_closed) {
      *completed_immediately = true;
      return true;
    }
    process->stdin_close_requested = true;
    auto command = ExecutorCommand{};
    command.kind = ExecutorCommand::Kind::kCloseStdin;
    command.completion = completion;
    process->commands.push_back(std::move(command));
  }
  WakePlatformProcess(process);
  return true;
}

static void WaitExecutorProcess(const std::shared_ptr<ExecutorProcess>& process,
                                muon_completion_func completion) {
  std::string result_json;
  std::string failure_message;
  auto complete_now = false;
  {
    std::lock_guard<std::mutex> lock(process->mutex);
    if (process->disposed) {
      failure_message = "executor process is disposed";
      complete_now = true;
    } else if (process->exited) {
      failure_message = process->failure_message;
      if (failure_message.empty()) {
        result_json = CreateWaitResultJson(*process);
      }
      complete_now = true;
    } else {
      process->wait_completions.push_back(completion);
    }
  }
  if (!complete_now) {
    return;
  }
  if (failure_message.empty()) {
    CompleteMuonString(completion, result_json);
  } else {
    CompleteMuonError(completion, failure_message);
  }
}

static void DisposeExecutorProcess(
    const std::shared_ptr<ExecutorProcess>& process) {
  std::deque<ExecutorCommand> pending_commands;
  std::vector<muon_completion_func> wait_completions;
  auto should_terminate = false;
  {
    std::lock_guard<std::mutex> lock(process->mutex);
    if (!process->exited && !process->daemon) {
      should_terminate = true;
    }
    process->disposed = true;
    pending_commands.swap(process->commands);
    wait_completions.swap(process->wait_completions);
  }
  for (const auto& command : pending_commands) {
    CompleteErrorOnDispatcher(process, command.completion,
                              "executor process is disposed");
  }
  for (const auto completion : wait_completions) {
    CompleteErrorOnDispatcher(process, completion,
                              "executor process is disposed");
  }
  ReleaseProcessCallbacks(process);
  if (should_terminate) {
    RequestPlatformTerminate(process);
  }
  ReleasePlatformProcessConnection(process);
}

static bool LoadAdhocLibrary(const LibraryRpcRequest& request,
                             int renderer_context_id,
                             std::string* result_json,
                             std::string* error_message) {
  auto* runtime = GetExecutorRuntime();
  if (runtime == nullptr || runtime->shutting_down) {
    *error_message = "executor runtime is unavailable";
    return false;
  }
  if (renderer_context_id <= 0) {
    *error_message = "renderer context id is unavailable";
    return false;
  }
  auto* handle = OpenAdhocDynamicLibrary(request.path, error_message);
  if (handle == nullptr) {
    return false;
  }

  auto library = std::make_shared<AdhocLibrary>();
  library->renderer_context_id = renderer_context_id;
  library->helpers = runtime->helpers;
  library->dispatcher = runtime->dispatcher;
  library->handle = handle;
  tra_ffic_error traffic_error;
  if (!tra_ffic_task_queue_init(&library->traffic_queue, nullptr, nullptr)) {
    CloseAdhocDynamicLibrary(handle);
    *error_message = "Failed to initialize adhoc tra-ffic queue";
    return false;
  }
  if (!tra_ffic_side_init_pair(&library->caller_side, &library->callee_side,
                               tra_ffic_task_queue_schedule_callback,
                               &library->traffic_queue, &traffic_error)) {
    tra_ffic_task_queue_destroy(&library->traffic_queue);
    CloseAdhocDynamicLibrary(handle);
    *error_message = GetExecutorTrafficError(traffic_error);
    return false;
  }
  library->traffic_initialized = true;
  {
    std::lock_guard<std::mutex> lock(runtime->mutex);
    library->library_id = runtime->next_library_id;
    runtime->next_library_id += 1;
    runtime->libraries[library->library_id] = library;
  }

  *result_json = "{\"libraryId\":";
  *result_json += std::to_string(library->library_id);
  *result_json += "}";
  return true;
}

static bool GetAdhocFunction(const LibraryRpcRequest& request,
                             std::string* result_json,
                             std::string* error_message) {
  const auto library = FindAdhocLibrary(request.library_id);
  if (!library) {
    *error_message = "adhoc library handle is unavailable";
    return false;
  }

  auto function = std::make_shared<AdhocFunction>();
  function->name = request.name;
  function->arg_types = request.arg_types;
  function->arg_type_names = request.arg_type_names;
  function->return_type = request.return_type;
  function->return_type_name = request.return_type_name;
  function->signature_storage = std::shared_ptr<MuonFunctionSignatureStorage>(
      CreateMuonFunctionSignatureStorageForAbi(
          function->arg_types, function->return_type,
          TRA_FFIC_SIGNATURE_ABI_RETVAL)
          .release());

  tra_ffic_error traffic_error;
  {
    std::lock_guard<std::mutex> lock(library->mutex);
    if (library->release_requested || library->closed) {
      *error_message = "adhoc library is released";
      return false;
    }
    auto* symbol = GetAdhocDynamicLibrarySymbol(library->handle, request.name,
                                                error_message);
    if (symbol == nullptr) {
      return false;
    }
    if (!tra_ffic_side_create_pure_function_impl(
            &library->callee_side,
            GetMuonFunctionSignature(function->signature_storage.get()),
            reinterpret_cast<tra_ffic_user_function>(symbol),
            &function->registered_function, &traffic_error)) {
      *error_message = GetExecutorTrafficError(traffic_error);
      return false;
    }
    function->function_ref.raw = reinterpret_cast<tra_ffic_native_function>(
        function->registered_function);
    function->function_ref.owner_side = &library->callee_side;
    function->function_ref.signature =
        GetMuonFunctionSignature(function->signature_storage.get());
    function->function_id = library->next_function_id;
    library->next_function_id += 1;
    library->functions[function->function_id] = function;
  }

  *result_json = "{\"functionId\":";
  *result_json += std::to_string(function->function_id);
  *result_json += "}";
  return true;
}

static bool BeginAdhocCall(const LibraryRpcRequest& request,
                           const muon_buffer_view& data,
                           std::shared_ptr<AdhocLibrary>* library_out,
                           std::shared_ptr<AdhocFunction>* function_out,
                           AdhocDecodedArgs* decoded_args,
                           std::string* error_message) {
  auto library = FindAdhocLibrary(request.library_id);
  if (!library) {
    *error_message = "adhoc library handle is unavailable";
    return false;
  }
  std::shared_ptr<AdhocFunction> function;
  {
    std::lock_guard<std::mutex> lock(library->mutex);
    if (library->release_requested || library->closed) {
      *error_message = "adhoc library is released";
      return false;
    }
    const auto iterator = library->functions.find(request.function_id);
    if (iterator == library->functions.end()) {
      *error_message = "adhoc function handle is unavailable";
      return false;
    }
    function = iterator->second;
    library->active_calls += 1;
  }

  if (!DecodeAdhocCallArguments(*function, request.args,
                                request.buffer_views, data, decoded_args,
                                error_message)) {
    FinishAdhocCall(library);
    return false;
  }
  *library_out = std::move(library);
  *function_out = std::move(function);
  return true;
}

static void CompleteAdhocCallOnDispatcher(
    const std::shared_ptr<AdhocCallState>& state) {
  const auto library = state->library;
  if (!library || !library->runtime_available ||
      library->dispatcher == nullptr || state->completion == nullptr) {
    return;
  }
  if (state->error_message.empty()) {
    FireAndForgetOnDispatcher(
        library->dispatcher,
        [completion = state->completion, result = state->result_json]() {
          CompleteMuonString(completion, result);
        });
  } else {
    FireAndForgetOnDispatcher(
        library->dispatcher,
        [completion = state->completion, message = state->error_message]() {
          CompleteMuonError(completion, message);
        });
  }
}

static void RunAdhocCallThread(std::shared_ptr<AdhocCallState> state) {
  tra_ffic_error traffic_error;
  if (!tra_ffic_call_with_result(
          &state->library->caller_side, &state->function->function_ref,
          state->args.values.empty() ? nullptr : state->args.values.data(),
          static_cast<uint32_t>(state->args.values.size()),
          CaptureAdhocCallResult, state.get(), &traffic_error)) {
    state->error_message = GetExecutorTrafficError(traffic_error);
  }
  CompleteAdhocCallOnDispatcher(state);
  FinishAdhocCall(state->library);
}

static bool StartAdhocCall(const LibraryRpcRequest& request,
                           const muon_buffer_view& data,
                           muon_completion_func completion,
                           std::string* error_message) {
  auto state = std::make_shared<AdhocCallState>();
  state->completion = completion;
  if (!BeginAdhocCall(request, data, &state->library, &state->function,
                      &state->args, error_message)) {
    return false;
  }
  try {
    std::thread(RunAdhocCallThread, state).detach();
  } catch (...) {
    FinishAdhocCall(state->library);
    *error_message = "Failed to start adhoc call worker";
    return false;
  }
  return true;
}

extern "C" void muon_builtin_executor_spawn_rpc(
    muon_completion_func completion,
    const char* request_json,
    muon_buffer_view data,
    uint32_t renderer_context_id,
    muon_native_function owner_callback,
    muon_native_function stdout_callback,
    muon_native_function stderr_callback) {
  SpawnRpcRequest request;
  std::string error_message;
  if (!ParseSpawnRpcRequest(request_json, &request, &error_message)) {
    CompleteMuonError(completion, error_message);
    return;
  }

  if (request.operation == SpawnRpcOperation::kStart) {
    std::string result_json;
    if (!StartExecutorProcess(request, static_cast<int>(renderer_context_id),
                              owner_callback, stdout_callback,
                              stderr_callback, nullptr, &result_json,
                              &error_message)) {
      CompleteMuonError(completion, error_message);
      return;
    }
    CompleteMuonString(completion, result_json);
    return;
  }

  auto process = FindExecutorProcess(request.handle_id);
  if (!process) {
    CompleteMuonError(completion, "executor process handle is unavailable");
    return;
  }

  switch (request.operation) {
    case SpawnRpcOperation::kWriteStdin: {
      std::vector<uint8_t> bytes;
      if (!CopyBufferView(data, &bytes, &error_message)) {
        CompleteMuonError(completion, error_message);
        return;
      }
      auto command = ExecutorCommand{};
      command.kind = ExecutorCommand::Kind::kWriteStdin;
      command.data = std::move(bytes);
      command.completion = completion;
      if (!QueueExecutorCommand(process, std::move(command),
                                &error_message)) {
        CompleteMuonError(completion, error_message);
      }
      return;
    }
    case SpawnRpcOperation::kCloseStdin: {
      auto completed_immediately = false;
      if (!QueueExecutorCloseStdin(process, completion,
                                   &completed_immediately, &error_message)) {
        CompleteMuonError(completion, error_message);
        return;
      }
      if (completed_immediately) {
        CompleteMuonString(completion, "{}");
      }
      return;
    }
    case SpawnRpcOperation::kWait:
      WaitExecutorProcess(process, completion);
      return;
    case SpawnRpcOperation::kKill:
      RequestPlatformTerminate(process);
      CompleteMuonString(completion, "{}");
      return;
    case SpawnRpcOperation::kDispose:
      (void)RemoveExecutorProcess(request.handle_id);
      DisposeExecutorProcess(process);
      CompleteMuonString(completion, "{}");
      return;
    case SpawnRpcOperation::kStart:
      break;
  }
  CompleteMuonError(completion, "unsupported executor operation");
}

extern "C" void muon_builtin_executor_library_rpc(
    muon_completion_func completion,
    const char* request_json,
    muon_buffer_view data,
    uint32_t renderer_context_id) {
  if (request_json == nullptr) {
    CompleteMuonError(completion, "Request JSON is required");
    return;
  }
  yyjson_read_err read_error = {};
  auto* document = yyjson_read_opts(
      const_cast<char*>(request_json), std::strlen(request_json),
      YYJSON_READ_NOFLAG, nullptr, &read_error);
  if (document == nullptr) {
    CompleteMuonError(completion, "Request JSON is invalid");
    return;
  }

  LibraryRpcRequest request;
  std::string error_message;
  std::string result_json;
  auto started_async_call = false;
  const auto parsed = ParseLibraryRpcRequestRoot(
      yyjson_doc_get_root(document), &request, &error_message);
  if (parsed) {
    switch (request.operation) {
      case LibraryRpcOperation::kLoad:
        if (LoadAdhocLibrary(request, static_cast<int>(renderer_context_id),
                             &result_json, &error_message)) {
          CompleteMuonString(completion, result_json);
        } else {
          CompleteMuonError(completion, error_message);
        }
        break;
      case LibraryRpcOperation::kGetFunction:
        if (GetAdhocFunction(request, &result_json, &error_message)) {
          CompleteMuonString(completion, result_json);
        } else {
          CompleteMuonError(completion, error_message);
        }
        break;
      case LibraryRpcOperation::kCall:
        started_async_call =
            StartAdhocCall(request, data, completion, &error_message);
        if (!started_async_call) {
          CompleteMuonError(completion, error_message);
        }
        break;
      case LibraryRpcOperation::kRelease: {
        const auto library = RemoveAdhocLibrary(request.library_id);
        if (!library) {
          CompleteMuonError(completion, "adhoc library handle is unavailable");
        } else {
          ReleaseAdhocLibrary(library, completion);
        }
        break;
      }
    }
  } else {
    CompleteMuonError(completion, error_message);
  }
  yyjson_doc_free(document);
}

bool InitializeMuonBuiltinExecutor(const muon_plugin_init_context* context,
                                   cardio::dispatcher* dispatcher,
                                   std::string* error_message) {
  const auto* helpers = context == nullptr ? nullptr : context->helpers;
  if (helpers == nullptr) {
    if (error_message != nullptr) {
      *error_message = "Plugin helpers are unavailable";
    }
    return false;
  }
  if (dispatcher == nullptr) {
    if (error_message != nullptr) {
      *error_message = "muon main dispatcher is unavailable";
    }
    return false;
  }
  std::lock_guard<std::mutex> lock(g_executor_runtime_mutex);
  auto runtime = std::make_unique<ExecutorRuntime>();
  runtime->helpers = helpers;
  runtime->dispatcher = dispatcher;
  g_executor_runtime = std::move(runtime);
#if !defined(_WIN32)
  signal(SIGPIPE, SIG_IGN);
#endif
  return true;
}

void ShutdownMuonBuiltinExecutor() {
  std::vector<std::shared_ptr<ExecutorProcess>> processes;
  std::vector<std::shared_ptr<AdhocLibrary>> libraries;
  {
    std::lock_guard<std::mutex> runtime_lock(g_executor_runtime_mutex);
    if (!g_executor_runtime) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(g_executor_runtime->mutex);
      g_executor_runtime->shutting_down = true;
      for (const auto& entry : g_executor_runtime->processes) {
        processes.push_back(entry.second);
      }
      for (const auto& entry : g_executor_runtime->libraries) {
        libraries.push_back(entry.second);
      }
      g_executor_runtime->processes.clear();
      g_executor_runtime->libraries.clear();
    }
    g_executor_runtime.reset();
  }
  for (const auto& process : processes) {
    {
      std::lock_guard<std::mutex> lock(process->mutex);
      process->runtime_available = false;
    }
    DisposeExecutorProcess(process);
  }
  for (const auto& library : libraries) {
    {
      std::lock_guard<std::mutex> lock(library->mutex);
      library->runtime_available = false;
    }
    ReleaseAdhocLibrary(library, nullptr);
  }
}

void ReleaseMuonBuiltinExecutorContext(int renderer_context_id) {
  std::vector<std::shared_ptr<ExecutorProcess>> processes;
  std::vector<std::shared_ptr<AdhocLibrary>> libraries;
  {
    std::lock_guard<std::mutex> runtime_lock(g_executor_runtime_mutex);
    auto* runtime = g_executor_runtime.get();
    if (runtime == nullptr) {
      return;
    }
    std::lock_guard<std::mutex> lock(runtime->mutex);
    for (auto iterator = runtime->processes.begin();
         iterator != runtime->processes.end();) {
      const auto process = iterator->second;
      if (process->renderer_context_id == renderer_context_id) {
        processes.push_back(process);
        iterator = runtime->processes.erase(iterator);
      } else {
        ++iterator;
      }
    }
    for (auto iterator = runtime->libraries.begin();
         iterator != runtime->libraries.end();) {
      const auto library = iterator->second;
      if (library->renderer_context_id == renderer_context_id) {
        libraries.push_back(library);
        iterator = runtime->libraries.erase(iterator);
      } else {
        ++iterator;
      }
    }
  }
  for (const auto& process : processes) {
    DisposeExecutorProcess(process);
  }
  for (const auto& library : libraries) {
    {
      std::lock_guard<std::mutex> lock(library->mutex);
      library->runtime_available = false;
    }
    ReleaseAdhocLibrary(library, nullptr);
  }
}

static const muon_plugin_function_metadata spawn_function = {
    "__spawnRpc",
    reinterpret_cast<muon_native_function>(&muon_builtin_executor_spawn_rpc),
    {6, spawn_rpc_args, &type_string},
    "spawn",
};

static const muon_plugin_function_metadata library_function = {
    "__libraryRpc",
    reinterpret_cast<muon_native_function>(&muon_builtin_executor_library_rpc),
    {3, library_rpc_args, &type_string},
    "loadLibrary",
};

static const muon_plugin_function_metadata* const executor_functions[] = {
    &spawn_function,
    &library_function,
    nullptr,
};

static constexpr char executor_setup_script[] = R"JS(
const __muonExecutorActiveProcesses = new Set();
const __muonExecutorActiveLibraries = new Set();
const __muonExecutorEmptyBytes = new Uint8Array(0);
const __muonExecutorOwnerCallback = () => {};
const __muonExecutorAsyncDispose =
  typeof Symbol === "function" && typeof Symbol.asyncDispose === "symbol"
    ? Symbol.asyncDispose
    : null;
const __muonExecutorToBytes = (data) => {
  if (typeof data === "string") {
    return new TextEncoder().encode(data);
  }
  if (data instanceof ArrayBuffer) {
    return new Uint8Array(data);
  }
  if (ArrayBuffer.isView(data)) {
    return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
  }
  throw new TypeError("stdin data must be a string or BufferSource");
};
const __muonExecutorFromBase64 = (source) => {
  const text = atob(source ?? "");
  const bytes = new Uint8Array(text.length);
  for (let index = 0; index < text.length; index += 1) {
    bytes[index] = text.charCodeAt(index);
  }
  return bytes;
};
const __muonExecutorPointerBrand =
  typeof Symbol === "function" ? Symbol.for("muon.nativePointer") : "__muonNativePointer";
const __muonExecutorType = (name) => Object.freeze({ name });
const __muonExecutorTypes = Object.freeze({
  voidType: __muonExecutorType("void"),
  boolType: __muonExecutorType("bool"),
  int8Type: __muonExecutorType("int8"),
  uint8Type: __muonExecutorType("uint8"),
  int16Type: __muonExecutorType("int16"),
  uint16Type: __muonExecutorType("uint16"),
  int32Type: __muonExecutorType("int32"),
  uint32Type: __muonExecutorType("uint32"),
  int64Type: __muonExecutorType("int64"),
  uint64Type: __muonExecutorType("uint64"),
  float32Type: __muonExecutorType("float32"),
  float64Type: __muonExecutorType("float64"),
  stringType: __muonExecutorType("string"),
  pointerType: __muonExecutorType("pointer"),
  bufferViewType: __muonExecutorType("bufferView"),
  usizeType: __muonExecutorType("usize"),
});
const __muonExecutorCreatePointer = (value) => {
  const text = String(value ?? "0");
  const pointer = {
    [__muonExecutorPointerBrand]: true,
    value: text,
    toString: () => text,
    toJSON: () => text,
  };
  return Object.freeze(pointer);
};
const __muonExecutorNormalizeType = (type) => {
  if (typeof type === "string") {
    return { name: type };
  }
  if (type && typeof type.name === "string") {
    return { name: type.name };
  }
  throw new TypeError("adhoc type descriptor is invalid");
};
const __muonExecutorNormalizeSignature = (signature) => {
  if (!signature || !Array.isArray(signature.argTypes)) {
    throw new TypeError("adhoc signature is invalid");
  }
  return {
    argTypes: signature.argTypes.map(__muonExecutorNormalizeType),
    returnType: __muonExecutorNormalizeType(signature.returnType),
  };
};
const __muonExecutorEncodeInteger = (value) => {
  if (typeof value === "bigint") {
    return value.toString();
  }
  if (typeof value === "number") {
    if (!Number.isFinite(value) || Math.trunc(value) !== value) {
      throw new TypeError("integer argument must be finite");
    }
    return String(value);
  }
  if (typeof value === "string") {
    return value;
  }
  throw new TypeError("integer argument must be a number, bigint, or string");
};
const __muonExecutorEncodePointer = (value) => {
  if (value === null || value === undefined) {
    return "0";
  }
  if (typeof value === "object" && value[__muonExecutorPointerBrand]) {
    return value.value;
  }
  return __muonExecutorEncodeInteger(value);
};
const __muonExecutorEncodeCall = (signature, args) => {
  if (args.length !== signature.argTypes.length) {
    throw new TypeError("Invalid adhoc argument count");
  }
  const encoded = [];
  const chunks = [];
  const bufferViews = [];
  let offset = 0;
  for (let index = 0; index < args.length; index += 1) {
    const name = signature.argTypes[index].name;
    const value = args[index];
    if (name === "int64" || name === "uint64" || name === "usize") {
      encoded.push(__muonExecutorEncodeInteger(value));
    } else if (name === "pointer") {
      encoded.push(__muonExecutorEncodePointer(value));
    } else if (name === "bufferView") {
      const bytes = __muonExecutorToBytes(value);
      chunks.push(bytes);
      bufferViews.push({ argIndex: index, offset, size: bytes.byteLength });
      offset += bytes.byteLength;
      encoded.push(null);
    } else {
      encoded.push(value);
    }
  }
  const data = new Uint8Array(offset);
  let writeOffset = 0;
  for (const chunk of chunks) {
    data.set(chunk, writeOffset);
    writeOffset += chunk.byteLength;
  }
  return { args: encoded, bufferViews, data };
};
const __muonExecutorDecodeAdhocResult = (signature, raw) => {
  const name = signature.returnType.name;
  if (name === "void") {
    return undefined;
  }
  if (name === "pointer") {
    return __muonExecutorCreatePointer(raw.pointer);
  }
  if (name === "int64" || name === "uint64" || name === "usize") {
    return BigInt(raw.value);
  }
  if (name === "bufferView") {
    return __muonExecutorFromBase64(raw.base64);
  }
  return raw.value;
};
const __muonExecutorRpc = async (
  request,
  data = __muonExecutorEmptyBytes,
  onStdout = null,
  onStderr = null,
) =>
  JSON.parse(
    await namespace.__spawnRpc(
      JSON.stringify(request),
      data,
      0,
      __muonExecutorOwnerCallback,
      onStdout,
      onStderr,
    ),
  );
const __muonExecutorLibraryRpc = async (
  request,
  data = __muonExecutorEmptyBytes,
) =>
  JSON.parse(
    await namespace.__libraryRpc(
      JSON.stringify(request),
      data,
      0,
    ),
  );
const __muonExecutorDecodeWaitResult = (raw) => {
  const result = {
    processId: raw.processId,
    exitCode: raw.exitCode,
  };
  if (Object.prototype.hasOwnProperty.call(raw, "stdoutBase64")) {
    result.stdout = __muonExecutorFromBase64(raw.stdoutBase64);
  }
  if (Object.prototype.hasOwnProperty.call(raw, "stderrBase64")) {
    result.stderr = __muonExecutorFromBase64(raw.stderrBase64);
  }
  return result;
};
const properties = {};
if (isAllowed("spawn")) {
  properties.spawn = {
    enumerable: true,
    configurable: false,
    writable: false,
    value: async (options = {}) => {
      const stdoutCallback =
        typeof options.onStdout === "function"
          ? (chunk) => options.onStdout(new Uint8Array(chunk))
          : null;
      const stderrCallback =
        typeof options.onStderr === "function"
          ? (chunk) => options.onStderr(new Uint8Array(chunk))
          : null;
      const start = await __muonExecutorRpc(
        {
          op: "start",
          options: {
            command: options.command,
            args: options.args,
            cwd: options.cwd,
            env: options.env,
            daemon: options.daemon,
          },
          captureStdout: stdoutCallback === null,
          captureStderr: stderrCallback === null,
        },
        __muonExecutorEmptyBytes,
        stdoutCallback,
        stderrCallback,
      );
      const handleId = start.handleId;
      let released = false;
      let stdinClosing = false;
      let waitPromise = null;
      const release = async () => {
        if (released) {
          return;
        }
        released = true;
        __muonExecutorActiveProcesses.delete(handle);
        await __muonExecutorRpc({ op: "dispose", handleId });
      };
      const handle = {
        processId: start.processId,
        writeStdin: async (data) => {
          if (released) {
            throw new Error("executor process is released");
          }
          if (stdinClosing) {
            throw new Error("stdin is closed");
          }
          await __muonExecutorRpc(
            { op: "writeStdin", handleId },
            __muonExecutorToBytes(data),
          );
        },
        closeStdin: async () => {
          if (released) {
            throw new Error("executor process is released");
          }
          stdinClosing = true;
          await __muonExecutorRpc({ op: "closeStdin", handleId });
        },
        wait: () => {
          if (waitPromise !== null) {
            return waitPromise;
          }
          waitPromise = (async () => {
            try {
              return __muonExecutorDecodeWaitResult(
                await __muonExecutorRpc({ op: "wait", handleId }),
              );
            } finally {
              if (!released) {
                released = true;
                __muonExecutorActiveProcesses.delete(handle);
                try {
                  await __muonExecutorRpc({ op: "dispose", handleId });
                } catch {}
              }
            }
          })();
          return waitPromise;
        },
        kill: async () => {
          if (released) {
            throw new Error("executor process is released");
          }
          await __muonExecutorRpc({ op: "kill", handleId });
        },
        release,
      };
      if (__muonExecutorAsyncDispose !== null) {
        Object.defineProperty(handle, __muonExecutorAsyncDispose, {
          configurable: false,
          enumerable: false,
          value: release,
          writable: false,
        });
      }
      Object.freeze(handle);
      __muonExecutorActiveProcesses.add(handle);
      return handle;
    },
  };
}
if (isAllowed("loadLibrary")) {
  for (const [name, value] of Object.entries(__muonExecutorTypes)) {
    properties[name] = {
      enumerable: true,
      configurable: false,
      writable: false,
      value,
    };
  }
  properties.loadLibrary = {
    enumerable: true,
    configurable: false,
    writable: false,
    value: async (path) => {
      const start = await __muonExecutorLibraryRpc({ op: "load", path });
      const libraryId = start.libraryId;
      let released = false;
      const release = async () => {
        if (released) {
          return;
        }
        released = true;
        __muonExecutorActiveLibraries.delete(handle);
        await __muonExecutorLibraryRpc({ op: "release", libraryId });
      };
      const handle = {
        getFunction: async (name, signature) => {
          if (released) {
            throw new Error("adhoc library is released");
          }
          const normalizedSignature =
            __muonExecutorNormalizeSignature(signature);
          const loaded = await __muonExecutorLibraryRpc({
            op: "getFunction",
            libraryId,
            name,
            signature: normalizedSignature,
          });
          const functionId = loaded.functionId;
          return async (...args) => {
            if (released) {
              throw new Error("adhoc library is released");
            }
            const encoded = __muonExecutorEncodeCall(
              normalizedSignature,
              args,
            );
            const raw = await __muonExecutorLibraryRpc(
              {
                op: "call",
                libraryId,
                functionId,
                args: encoded.args,
                bufferViews: encoded.bufferViews,
              },
              encoded.data,
            );
            return __muonExecutorDecodeAdhocResult(
              normalizedSignature,
              raw,
            );
          };
        },
        release,
      };
      if (__muonExecutorAsyncDispose !== null) {
        Object.defineProperty(handle, __muonExecutorAsyncDispose, {
          configurable: false,
          enumerable: false,
          value: release,
          writable: false,
        });
      }
      Object.freeze(handle);
      __muonExecutorActiveLibraries.add(handle);
      return handle;
    },
  };
}
const __muonExecutorReleaseActiveProcesses = async () => {
  if (__muonExecutorActiveProcesses.size === 0) {
    return;
  }
  for (const handle of Array.from(__muonExecutorActiveProcesses)) {
    try {
      await handle.release();
    } catch {}
  }
};
const __muonExecutorReleaseActiveLibraries = async () => {
  if (__muonExecutorActiveLibraries.size === 0) {
    return;
  }
  for (const handle of Array.from(__muonExecutorActiveLibraries)) {
    try {
      await handle.release();
    } catch {}
  }
};
if (typeof globalThis.addEventListener === "function") {
  for (const eventName of ["beforeunload", "pagehide", "unload"]) {
    globalThis.addEventListener(eventName, __muonExecutorReleaseActiveProcesses);
    globalThis.addEventListener(eventName, __muonExecutorReleaseActiveLibraries);
  }
}
Object.defineProperties(namespace, properties);
)JS";

}  // namespace muon_internal

bool InitializeMuonBuiltinExecutor(const muon_plugin_init_context* context,
                                   cardio::dispatcher* dispatcher,
                                   std::string* error_message) {
  return muon_internal::InitializeMuonBuiltinExecutor(
      context, dispatcher, error_message);
}

void ShutdownMuonBuiltinExecutor() {
  muon_internal::ShutdownMuonBuiltinExecutor();
}

void ReleaseMuonBuiltinExecutorContext(int renderer_context_id) {
  muon_internal::ReleaseMuonBuiltinExecutorContext(renderer_context_id);
}

const muon_plugin_namespace kMuonBuiltinExecutorNamespace = {
    "muon.executor",
    muon_internal::executor_setup_script,
    muon_internal::executor_functions,
};

static const muon_plugin_namespace* const executor_namespaces[] = {
    &kMuonBuiltinExecutorNamespace,
    nullptr,
};

static const muon_plugin_metadata executor_metadata = {
    executor_namespaces,
    nullptr,
    nullptr,
};

const muon_plugin_metadata* GetMuonBuiltinExecutorPluginMetadata() {
  return &executor_metadata;
}
