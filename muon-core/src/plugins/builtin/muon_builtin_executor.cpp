/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "plugins/builtin/muon_builtin_executor.h"

#include "muon_cardio_post.h"
#include "muon_json_helpers.h"
#include "plugins/builtin/muon_builtin_completion.h"
#include "plugins/builtin/muon_builtin_environment_helpers.h"
#include "yyjson.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
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
  HANDLE process_handle = nullptr;
  HANDLE thread_handle = nullptr;
  HANDLE stdin_write = nullptr;
  HANDLE stdout_read = nullptr;
  HANDLE stderr_read = nullptr;
  std::condition_variable command_cv;
  bool command_thread_exited = false;
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

struct ExecutorRuntime {
  const muon_plugin_helpers* helpers = nullptr;
  cardio::dispatcher* dispatcher = nullptr;
  std::mutex mutex;
  uint32_t next_handle_id = 1;
  std::map<uint32_t, std::shared_ptr<ExecutorProcess>> processes;
  bool shutting_down = false;
};

struct OutputCallbackCompletionState {
  const muon_plugin_helpers* helpers = nullptr;
  std::shared_ptr<ExecutorProcess> process;
  muon_completion_func completion = nullptr;
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

static void RunWindowsPipeReader(std::shared_ptr<ExecutorProcess> process,
                                 HANDLE pipe,
                                 bool is_stdout) {
  std::vector<uint8_t> buffer(4096);
  DWORD read_size = 0;
  while (ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()),
                  &read_size, nullptr) &&
         read_size > 0) {
    AppendProcessOutput(process, is_stdout, buffer.data(),
                        static_cast<size_t>(read_size));
  }
  CloseHandle(pipe);
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

static void RunWindowsCommandThread(std::shared_ptr<ExecutorProcess> process) {
  while (true) {
    ExecutorCommand command;
    {
      std::unique_lock<std::mutex> lock(process->mutex);
      process->command_cv.wait(lock, [&process]() {
        return !process->commands.empty() || process->exited ||
               process->disposed;
      });
      if (process->commands.empty()) {
        if (process->exited || process->disposed) {
          process->command_thread_exited = true;
          return;
        }
        continue;
      }
      command = std::move(process->commands.front());
      process->commands.pop_front();
    }

    if (command.kind == ExecutorCommand::Kind::kCloseStdin) {
      HANDLE handle = nullptr;
      {
        std::lock_guard<std::mutex> lock(process->mutex);
        handle = process->stdin_write;
        process->stdin_write = nullptr;
        process->stdin_closed = true;
      }
      CloseWindowsHandle(&handle);
      CompleteWriteCommand(process, command, "");
      continue;
    }

    auto error_message = std::string{};
    HANDLE handle = nullptr;
    {
      std::lock_guard<std::mutex> lock(process->mutex);
      handle = process->stdin_write;
    }
    if (handle == nullptr) {
      error_message = "stdin is closed";
    } else if (!command.data.empty()) {
      auto offset = size_t{0};
      while (offset < command.data.size()) {
        const auto remaining = std::min<size_t>(
            command.data.size() - offset, std::numeric_limits<DWORD>::max());
        DWORD written = 0;
        if (!WriteFile(handle, command.data.data() + offset,
                       static_cast<DWORD>(remaining), &written, nullptr)) {
          error_message = "Failed to write stdin";
          break;
        }
        offset += static_cast<size_t>(written);
      }
    }
    CompleteWriteCommand(process, command, error_message);
  }
}

static void RunWindowsWaitThread(std::shared_ptr<ExecutorProcess> process) {
  WaitForSingleObject(process->process_handle, INFINITE);
  DWORD exit_code = 0;
  GetExitCodeProcess(process->process_handle, &exit_code);
  CloseWindowsHandle(&process->stdin_write);
  CloseWindowsHandle(&process->thread_handle);
  CloseWindowsHandle(&process->process_handle);
  {
    std::lock_guard<std::mutex> lock(process->mutex);
    process->stdin_closed = true;
  }
  process->command_cv.notify_all();
  MarkProcessExited(process, static_cast<int32_t>(exit_code), "");
}

static bool StartPlatformProcess(const RunOptions& options,
                                 const std::shared_ptr<ExecutorProcess>& process,
                                 std::string* error_message) {
  SECURITY_ATTRIBUTES security_attributes = {};
  security_attributes.nLength = sizeof(security_attributes);
  security_attributes.bInheritHandle = TRUE;

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
  if (!CreatePipe(&stdin_read, &stdin_write, &security_attributes, 0) ||
      !CreatePipe(&stdout_read, &stdout_write, &security_attributes, 0) ||
      !CreatePipe(&stderr_read, &stderr_write, &security_attributes, 0)) {
    close_pipe_handles();
    *error_message = "Failed to create process pipes";
    return false;
  }
  SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);

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

  STARTUPINFOW startup_info = {};
  startup_info.cb = sizeof(startup_info);
  startup_info.dwFlags = STARTF_USESTDHANDLES;
  startup_info.hStdInput = stdin_read;
  startup_info.hStdOutput = stdout_write;
  startup_info.hStdError = stderr_write;
  PROCESS_INFORMATION process_info = {};
  const auto created = CreateProcessW(
      nullptr, mutable_command_line.data(), nullptr, nullptr, TRUE,
      creation_flags, environment_pointer, cwd_pointer, &startup_info,
      &process_info);
  CloseWindowsHandle(&stdin_read);
  CloseWindowsHandle(&stdout_write);
  CloseWindowsHandle(&stderr_write);
  if (!created) {
    close_pipe_handles();
    *error_message = "Failed to start process";
    return false;
  }

  process->process_id = process_info.dwProcessId;
  process->process_handle = process_info.hProcess;
  process->thread_handle = process_info.hThread;
  process->stdin_write = stdin_write;
  process->stdout_read = stdout_read;
  process->stderr_read = stderr_read;

  std::thread(RunWindowsPipeReader, process, stdout_read, true).detach();
  std::thread(RunWindowsPipeReader, process, stderr_read, false).detach();
  std::thread(RunWindowsCommandThread, process).detach();
  std::thread(RunWindowsWaitThread, process).detach();
  return true;
}

static void WakePlatformProcess(const std::shared_ptr<ExecutorProcess>& process) {
  process->command_cv.notify_all();
}

static void RequestPlatformTerminate(
    const std::shared_ptr<ExecutorProcess>& process) {
  HANDLE handle = nullptr;
  {
    std::lock_guard<std::mutex> lock(process->mutex);
    handle = process->process_handle;
  }
  if (handle != nullptr) {
    TerminateProcess(handle, 1);
  }
  WakePlatformProcess(process);
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
  int fd = -1;
  {
    std::lock_guard<std::mutex> lock(process->mutex);
    fd = process->wake_write_fd;
  }
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

static void RequestPlatformTerminate(
    const std::shared_ptr<ExecutorProcess>& process) {
  pid_t child = -1;
  {
    std::lock_guard<std::mutex> lock(process->mutex);
    child = process->child;
  }
  if (child > 0) {
    kill(child, SIGTERM);
  }
  WakePlatformProcess(process);
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
  auto stdin_fd = process->stdin_fd;
  auto stdout_fd = process->stdout_fd;
  auto stderr_fd = process->stderr_fd;
  auto wake_read_fd = process->wake_read_fd;
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
    CloseFd(&process->stdin_fd);
    CloseFd(&process->stdout_fd);
    CloseFd(&process->stderr_fd);
    CloseFd(&process->wake_read_fd);
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
    if (!process->exited) {
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

bool InitializeMuonBuiltinExecutor(const muon_plugin_helpers* helpers,
                                   cardio::dispatcher* dispatcher,
                                   std::string* error_message) {
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
      g_executor_runtime->processes.clear();
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
}

void ReleaseMuonBuiltinExecutorContext(int renderer_context_id) {
  std::vector<std::shared_ptr<ExecutorProcess>> processes;
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
  }
  for (const auto& process : processes) {
    DisposeExecutorProcess(process);
  }
}

static const muon_plugin_function_metadata spawn_function = {
    "__spawnRpc",
    reinterpret_cast<muon_native_function>(&muon_builtin_executor_spawn_rpc),
    {6, spawn_rpc_args, &type_string},
    "spawn",
};

static const muon_plugin_function_metadata* const executor_functions[] = {
    &spawn_function,
    nullptr,
};

static constexpr char executor_setup_script[] = R"JS(
const __muonExecutorActiveProcesses = new Set();
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
            const result = __muonExecutorDecodeWaitResult(
              await __muonExecutorRpc({ op: "wait", handleId }),
            );
            if (!released) {
              released = true;
              __muonExecutorActiveProcesses.delete(handle);
              try {
                await __muonExecutorRpc({ op: "dispose", handleId });
              } catch {}
            }
            return result;
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
if (typeof globalThis.addEventListener === "function") {
  for (const eventName of ["beforeunload", "pagehide", "unload"]) {
    globalThis.addEventListener(eventName, __muonExecutorReleaseActiveProcesses);
  }
}
Object.defineProperties(namespace, properties);
)JS";

}  // namespace muon_internal

bool InitializeMuonBuiltinExecutor(const muon_plugin_helpers* helpers,
                                   cardio::dispatcher* dispatcher,
                                   std::string* error_message) {
  return muon_internal::InitializeMuonBuiltinExecutor(
      helpers, dispatcher, error_message);
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
};

const muon_plugin_metadata* GetMuonBuiltinExecutorPluginMetadata() {
  return &executor_metadata;
}
