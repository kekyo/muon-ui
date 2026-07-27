/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "muon_plugin_api.h"

#if defined(_WIN32)
#include "muon_windows_job_process.h"
#endif

#include <cardio.h>
#include <yyjson.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <algorithm>
#include <bcrypt.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

static constexpr char kMuonNodeProtocol[] = "muon-node/1";
static constexpr size_t kMuonNodeMaximumFrameSize = 16u * 1024u * 1024u;
static constexpr size_t kMuonNodeMaximumPendingRequests = 1024u;
static constexpr size_t kMuonNodeMaximumCallbackRequestIdSize = 128u;
static constexpr uint64_t kMuonNodeShutdownTimeoutMilliseconds = 2000u;
static constexpr uint64_t kMuonNodeKillTimeoutMilliseconds = 1000u;

static const muon_plugin_helpers* g_muon_node_helpers = nullptr;

enum class MuonNodeRuntimeState {
  Disabled,
  Starting,
  Ready,
  Failed,
  Stopping,
  Stopped,
};

enum class MuonNodeHostResultKind {
  String,
  Void,
};

static const muon_type_descriptor kMuonNodeStringType = {
    MUON_TYPE_STRING,
    nullptr,
};

struct MuonNodeHostInvocation {
  muon_completion_func completion = nullptr;
  MuonNodeHostResultKind result_kind = MuonNodeHostResultKind::String;
  muon_native_function callback_dispatcher = nullptr;
  std::vector<std::string> callback_handles;
  bool callback_dispatcher_retained = false;
};

struct MuonNodeQueuedCommand {
  std::string command;
  std::string first;
  std::string second;
  std::string arguments_json;
  std::shared_ptr<MuonNodeHostInvocation> invocation;
};

struct MuonNodePendingRequest {
  std::function<void(bool, const std::string&, const std::string&)> complete;
};

struct MuonNodeCallbackLease {
  muon_native_function dispatcher = nullptr;
  size_t references = 0;
};

struct MuonNodeRuntime;
struct MuonNodeManager;

struct MuonNodeRendererCallbackState {
  MuonNodeRuntime* runtime = nullptr;
  std::string request_id;
  muon_completion_func completion_function = nullptr;
};

struct MuonNodeRuntime {
  MuonNodeRuntimeState state = MuonNodeRuntimeState::Disabled;
  MuonNodeManager* manager = nullptr;
  std::string instance_id;
  std::string renderer_owner_token;
  std::string project_root;
  std::string bridge_path;
  std::string executable_path;
  std::string session_token;
  std::string failure_message;
  uint64_t next_request_id = 1;
  std::map<std::string, MuonNodePendingRequest> pending_requests;
  std::deque<MuonNodeQueuedCommand> queued_commands;
  std::map<std::string, MuonNodeCallbackLease> callback_leases;
  std::set<std::string> pending_callback_requests;
  std::deque<std::vector<std::byte>> write_queue;
  cardio::cancellation_source io_cancellation;
  cardio::cancellation_source shutdown_timer_cancellation;
  bool startup_active = false;
  bool reader_active = false;
  bool writer_active = false;
  bool process_monitor_active = false;
  bool shutdown_timer_active = false;
  bool process_started = false;
  bool process_exited = false;
  // Shutdown RPCs are valid only after the child completed its IPC handshake.
  bool transport_connected = false;
  bool shutdown_sent = false;
  // Plugin unload must wait for host completion closures still entering this DLL.
  size_t active_renderer_callbacks = 0;
  std::shared_ptr<MuonNodeHostInvocation> create_invocation;
  std::vector<std::shared_ptr<MuonNodeHostInvocation>> release_invocations;
#if defined(_WIN32)
  HANDLE pipe = INVALID_HANDLE_VALUE;
  MuonWindowsJobProcess process{};
#else
  int socket_fd = -1;
  int process_fd = -1;
  pid_t process_id = -1;
#endif
};

struct MuonNodeManager {
  std::string project_root;
  std::string bridge_path;
  std::string executable_path;
  uint64_t next_instance_id = 1;
  std::map<std::string, std::shared_ptr<MuonNodeRuntime>> runtimes;
  bool stopping = false;
  muon_plugin_stop_completion stop_completion = nullptr;
  void* stop_user_data = nullptr;
};

static std::unique_ptr<MuonNodeManager> g_muon_node_manager;

static void LogMuonNodeMessage(muon_log_level level,
                               const std::string& message) {
  if (g_muon_node_helpers != nullptr &&
      g_muon_node_helpers->__log_message_impl != nullptr) {
    g_muon_node_helpers->log_message(level, message.c_str());
  }
}

static std::string GetMuonNodeExceptionMessage(
    const std::string& fallback,
    std::exception_ptr exception = std::current_exception()) {
  try {
    if (exception) {
      std::rethrow_exception(exception);
    }
  } catch (const std::exception& error) {
    const auto* message = error.what();
    if (message != nullptr && message[0] != '\0') {
      return message;
    }
  } catch (...) {
  }
  return fallback;
}

static std::string WriteMuonNodeJsonValue(const yyjson_val* value) {
  if (value == nullptr) {
    return "null";
  }
  size_t size = 0;
  auto* raw = yyjson_val_write(value, YYJSON_WRITE_NOFLAG, &size);
  if (raw == nullptr) {
    throw std::runtime_error("Failed to encode a Node IPC JSON value");
  }
  auto result = std::string(raw, size);
  std::free(raw);
  return result;
}

static std::string WriteMuonNodeMutableJson(yyjson_mut_doc* document) {
  if (document == nullptr) {
    throw std::runtime_error("Node IPC JSON document is unavailable");
  }
  size_t size = 0;
  auto* raw =
      yyjson_mut_write(document, YYJSON_WRITE_NOFLAG, &size);
  if (raw == nullptr) {
    throw std::runtime_error("Failed to encode a Node IPC JSON message");
  }
  auto result = std::string(raw, size);
  std::free(raw);
  return result;
}

static std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>
ReadMuonNodeJson(const std::string& json) {
  yyjson_read_err error{};
  auto* document = yyjson_read_opts(
      const_cast<char*>(json.data()), json.size(), YYJSON_READ_NOFLAG,
      nullptr, &error);
  return {document, &yyjson_doc_free};
}

static std::string CreateMuonNodeSessionToken() {
  std::array<uint8_t, 32> bytes{};
#if defined(_WIN32)
  const auto status = BCryptGenRandom(
      nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
      BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  if (status != 0) {
    throw std::runtime_error("BCryptGenRandom failed for Node IPC token");
  }
#else
  auto offset = size_t{0};
  while (offset < bytes.size()) {
    const auto count = ::getrandom(bytes.data() + offset,
                                   bytes.size() - offset, 0);
    if (count > 0) {
      offset += static_cast<size_t>(count);
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    throw std::runtime_error("getrandom failed for Node IPC token");
  }
#endif
  static constexpr char digits[] = "0123456789abcdef";
  std::string token;
  token.reserve(bytes.size() * 2);
  for (const auto byte : bytes) {
    token.push_back(digits[(byte >> 4) & 0x0f]);
    token.push_back(digits[byte & 0x0f]);
  }
  return token;
}

static void ReleaseMuonNodeCallbackDispatcher(
    const std::shared_ptr<MuonNodeHostInvocation>& invocation) {
  if (!invocation || !invocation->callback_dispatcher_retained ||
      invocation->callback_dispatcher == nullptr ||
      g_muon_node_helpers == nullptr ||
      g_muon_node_helpers->__release_plugin_function_pointer_impl == nullptr) {
    return;
  }
  invocation->callback_dispatcher_retained = false;
  g_muon_node_helpers->release_plugin_function_pointer(
      invocation->callback_dispatcher);
}

static void RemoveMuonNodeCallbackHandles(
    MuonNodeRuntime* runtime,
    const std::shared_ptr<MuonNodeHostInvocation>& invocation) {
  if (runtime == nullptr || !invocation) {
    return;
  }
  for (const auto& handle : invocation->callback_handles) {
    const auto entry = runtime->callback_leases.find(handle);
    if (entry == runtime->callback_leases.end()) {
      continue;
    }
    if (entry->second.references > 1) {
      entry->second.references -= 1;
    } else {
      runtime->callback_leases.erase(entry);
    }
  }
  ReleaseMuonNodeCallbackDispatcher(invocation);
}

static void CompleteMuonNodeHostInvocation(
    MuonNodeRuntime* runtime,
    const std::shared_ptr<MuonNodeHostInvocation>& invocation,
    bool success,
    const std::string& value_json,
    const std::string& error_message) {
  if (!invocation || invocation->completion == nullptr) {
    RemoveMuonNodeCallbackHandles(runtime, invocation);
    return;
  }
  const auto completion = std::exchange(invocation->completion, nullptr);
  RemoveMuonNodeCallbackHandles(runtime, invocation);
  if (!success) {
    completion(nullptr, error_message.empty()
                            ? "Node IPC request failed"
                            : error_message.c_str());
    return;
  }
  if (invocation->result_kind == MuonNodeHostResultKind::Void) {
    completion(nullptr, nullptr);
    return;
  }
  const auto* value = value_json.c_str();
  completion(&value, nullptr);
}

static void CompleteAllMuonNodeHostInvocations(
    MuonNodeRuntime* runtime,
    const std::string& error_message) {
  if (runtime == nullptr) {
    return;
  }
  auto queued = std::move(runtime->queued_commands);
  runtime->queued_commands.clear();
  for (auto& command : queued) {
    CompleteMuonNodeHostInvocation(runtime, command.invocation, false, "",
                                   error_message);
  }
}

static void MaybeCompleteMuonNodeManagerStop(MuonNodeManager* manager);
static void MaybeCompleteMuonNodeStop(MuonNodeRuntime* runtime);
static void FailMuonNodeRuntime(MuonNodeRuntime* runtime,
                                const std::string& error_message);
static void EnqueueMuonNodeMessage(MuonNodeRuntime* runtime,
                                   const std::string& json);
static void ExecuteMuonNodeQueuedCommand(MuonNodeRuntime* runtime,
                                         MuonNodeQueuedCommand command);
static void SendMuonNodeShutdownRequest(MuonNodeRuntime* runtime);
static cardio::promise<void> MonitorMuonNodeProcess(
    MuonNodeRuntime* runtime);

static bool CollectMuonNodeCallbackHandles(
    const std::string& arguments_json,
    std::vector<std::string>* handles,
    std::string* error_message) {
  if (handles == nullptr || error_message == nullptr) {
    return false;
  }
  handles->clear();
  if (arguments_json.size() > kMuonNodeMaximumFrameSize) {
    *error_message = "Node call arguments exceed the IPC size limit";
    return false;
  }
  const auto document = ReadMuonNodeJson(arguments_json);
  if (!document) {
    *error_message = "Node call arguments are not valid JSON";
    return false;
  }
  auto* arguments = yyjson_doc_get_root(document.get());
  if (!yyjson_is_arr(arguments)) {
    *error_message = "Node call arguments must be a JSON array";
    return false;
  }
  const auto count = yyjson_arr_size(arguments);
  for (auto index = size_t{0}; index < count; ++index) {
    auto* value = yyjson_arr_get(arguments, index);
    if (!yyjson_is_obj(value)) {
      continue;
    }
    auto* kind = yyjson_obj_get(value, "kind");
    auto* handle = yyjson_obj_get(value, "handle");
    if (!yyjson_is_str(kind) || !yyjson_is_str(handle) ||
        std::strcmp(yyjson_get_str(kind), "function") != 0) {
      continue;
    }
    const auto handle_value =
        std::string(yyjson_get_str(handle), yyjson_get_len(handle));
    if (handle_value.empty()) {
      *error_message = "Node callback handle must not be empty";
      return false;
    }
    handles->push_back(handle_value);
  }
  return true;
}

static std::string CreateMuonNodeRequestJson(
    const std::string& id,
    const std::string& command,
    const std::function<bool(yyjson_mut_doc*, yyjson_mut_val*)>&
        add_parameters) {
  auto* document = yyjson_mut_doc_new(nullptr);
  if (document == nullptr) {
    throw std::runtime_error("Failed to allocate a Node IPC request");
  }
  const auto cleanup =
      std::unique_ptr<yyjson_mut_doc, decltype(&yyjson_mut_doc_free)>(
          document, &yyjson_mut_doc_free);
  auto* root = yyjson_mut_obj(document);
  auto* parameters = yyjson_mut_obj(document);
  if (root == nullptr || parameters == nullptr ||
      !yyjson_mut_obj_add_strcpy(document, root, "kind", "request") ||
      !yyjson_mut_obj_add_strcpy(document, root, "id", id.c_str()) ||
      !yyjson_mut_obj_add_strcpy(document, root, "command",
                                 command.c_str()) ||
      !add_parameters(document, parameters) ||
      !yyjson_mut_obj_add_val(document, root, "params", parameters)) {
    throw std::runtime_error("Failed to construct a Node IPC request");
  }
  yyjson_mut_doc_set_root(document, root);
  return WriteMuonNodeMutableJson(document);
}

static std::string CreateMuonNodeInitializeRequestJson(
    const std::string& id,
    const MuonNodeRuntime& runtime) {
  return CreateMuonNodeRequestJson(
      id, "initialize",
      [&runtime](yyjson_mut_doc* document, yyjson_mut_val* parameters) {
        return yyjson_mut_obj_add_strcpy(
                   document, parameters, "protocol", kMuonNodeProtocol) &&
               yyjson_mut_obj_add_strcpy(
                   document, parameters, "projectRoot",
                   runtime.project_root.c_str()) &&
               yyjson_mut_obj_add_strcpy(
                   document, parameters, "token",
                   runtime.session_token.c_str());
      });
}

static std::string CreateMuonNodeCommandRequestJson(
    const std::string& id,
    const MuonNodeQueuedCommand& command) {
  if (command.command == "importModule") {
    return CreateMuonNodeRequestJson(
        id, command.command,
        [&command](yyjson_mut_doc* document, yyjson_mut_val* parameters) {
          return yyjson_mut_obj_add_strcpy(
              document, parameters, "specifier", command.first.c_str());
        });
  }
  if (command.command == "release") {
    return CreateMuonNodeRequestJson(
        id, command.command,
        [&command](yyjson_mut_doc* document, yyjson_mut_val* parameters) {
          return yyjson_mut_obj_add_strcpy(
                     document, parameters, "kind", "module") &&
                 yyjson_mut_obj_add_strcpy(
                     document, parameters, "handle", command.first.c_str());
        });
  }
  if (command.command == "call") {
    const auto arguments_document =
        ReadMuonNodeJson(command.arguments_json);
    if (!arguments_document) {
      throw std::runtime_error("Node call arguments are not valid JSON");
    }
    auto* arguments =
        yyjson_doc_get_root(arguments_document.get());
    if (!yyjson_is_arr(arguments)) {
      throw std::runtime_error("Node call arguments must be a JSON array");
    }
    return CreateMuonNodeRequestJson(
        id, command.command,
        [&command, arguments](yyjson_mut_doc* document,
                              yyjson_mut_val* parameters) {
          auto* copied_arguments = yyjson_val_mut_copy(document, arguments);
          return copied_arguments != nullptr &&
                 yyjson_mut_obj_add_strcpy(
                     document, parameters, "moduleId",
                     command.first.c_str()) &&
                 yyjson_mut_obj_add_strcpy(
                     document, parameters, "exportName",
                     command.second.c_str()) &&
                 yyjson_mut_obj_add_val(
                     document, parameters, "arguments", copied_arguments);
        });
  }
  if (command.command == "shutdown") {
    return CreateMuonNodeRequestJson(
        id, command.command,
        [](yyjson_mut_doc*, yyjson_mut_val*) { return true; });
  }
  throw std::runtime_error("Unknown Node IPC command: " + command.command);
}

static std::string CreateMuonNodeCallbackResultJson(
    const std::string& id,
    bool success,
    const std::string& value_json,
    const std::string& error_message) {
  auto* document = yyjson_mut_doc_new(nullptr);
  if (document == nullptr) {
    throw std::runtime_error("Failed to allocate a Node callback result");
  }
  const auto cleanup =
      std::unique_ptr<yyjson_mut_doc, decltype(&yyjson_mut_doc_free)>(
          document, &yyjson_mut_doc_free);
  auto* root = yyjson_mut_obj(document);
  if (root == nullptr ||
      !yyjson_mut_obj_add_strcpy(document, root, "kind",
                                 "callbackResult") ||
      !yyjson_mut_obj_add_strcpy(document, root, "id", id.c_str()) ||
      !yyjson_mut_obj_add_bool(document, root, "ok", success)) {
    throw std::runtime_error("Failed to construct a Node callback result");
  }
  if (success) {
    const auto value_document = ReadMuonNodeJson(value_json);
    if (!value_document) {
      throw std::runtime_error(
          "Renderer callback returned invalid JSON");
    }
    auto* value = yyjson_val_mut_copy(
        document, yyjson_doc_get_root(value_document.get()));
    if (value == nullptr ||
        !yyjson_mut_obj_add_val(document, root, "value", value)) {
      throw std::runtime_error(
          "Failed to copy a Node callback result value");
    }
  } else {
    auto* error = yyjson_mut_obj(document);
    if (error == nullptr ||
        !yyjson_mut_obj_add_strcpy(
            document, error, "code", "ERR_MUON_RENDERER_CALLBACK") ||
        !yyjson_mut_obj_add_strcpy(
            document, error, "message", error_message.c_str()) ||
        !yyjson_mut_obj_add_val(document, root, "error", error)) {
      throw std::runtime_error(
          "Failed to construct a Node callback error");
    }
  }
  yyjson_mut_doc_set_root(document, root);
  return WriteMuonNodeMutableJson(document);
}

static std::vector<std::byte> CreateMuonNodeFrame(
    const std::string& json) {
  if (json.size() > kMuonNodeMaximumFrameSize ||
      json.size() > std::numeric_limits<uint32_t>::max()) {
    throw std::runtime_error("Node IPC frame exceeds the size limit");
  }
  const auto size = static_cast<uint32_t>(json.size());
  std::vector<std::byte> frame(4 + json.size());
  frame[0] = static_cast<std::byte>((size >> 24) & 0xff);
  frame[1] = static_cast<std::byte>((size >> 16) & 0xff);
  frame[2] = static_cast<std::byte>((size >> 8) & 0xff);
  frame[3] = static_cast<std::byte>(size & 0xff);
  if (!json.empty()) {
    std::memcpy(frame.data() + 4, json.data(), json.size());
  }
  return frame;
}

#if defined(_WIN32)
static cardio::promise<size_t> ReadMuonNodeTransport(
    MuonNodeRuntime* runtime,
    std::span<std::byte> buffer) {
  if (runtime == nullptr || runtime->pipe == INVALID_HANDLE_VALUE) {
    throw std::runtime_error("Node IPC pipe is unavailable");
  }
  co_return co_await cardio::win32::read(
      runtime->pipe, buffer, runtime->io_cancellation.get_cancellation());
}

static cardio::promise<size_t> WriteMuonNodeTransport(
    MuonNodeRuntime* runtime,
    std::span<const std::byte> buffer) {
  if (runtime == nullptr || runtime->pipe == INVALID_HANDLE_VALUE) {
    throw std::runtime_error("Node IPC pipe is unavailable");
  }
  co_return co_await cardio::win32::write(
      runtime->pipe, buffer, runtime->io_cancellation.get_cancellation());
}
#else
static cardio::promise<size_t> ReadMuonNodeTransport(
    MuonNodeRuntime* runtime,
    std::span<std::byte> buffer) {
  if (runtime == nullptr || runtime->socket_fd < 0) {
    throw std::runtime_error("Node IPC socket is unavailable");
  }
  for (;;) {
    const auto count =
        ::read(runtime->socket_fd, buffer.data(), buffer.size());
    if (count >= 0) {
      co_return static_cast<size_t>(count);
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      throw std::system_error(
          errno, std::generic_category(), "Node IPC read failed");
    }
    const auto events = co_await cardio::from_fd(
        runtime->socket_fd,
        cardio::fd_event::read | cardio::fd_event::error |
            cardio::fd_event::hangup,
        runtime->io_cancellation.get_cancellation());
    if ((events & cardio::fd_event::error) != cardio::fd_event::none) {
      throw std::runtime_error("Node IPC socket reported a read error");
    }
  }
}

static cardio::promise<size_t> WriteMuonNodeTransport(
    MuonNodeRuntime* runtime,
    std::span<const std::byte> buffer) {
  if (runtime == nullptr || runtime->socket_fd < 0) {
    throw std::runtime_error("Node IPC socket is unavailable");
  }
  for (;;) {
    // A sidecar exit must become an IPC error, never a process-wide SIGPIPE.
    const auto count =
        ::send(runtime->socket_fd, buffer.data(), buffer.size(), MSG_NOSIGNAL);
    if (count >= 0) {
      co_return static_cast<size_t>(count);
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      throw std::system_error(
          errno, std::generic_category(), "Node IPC write failed");
    }
    const auto events = co_await cardio::from_fd(
        runtime->socket_fd,
        cardio::fd_event::write | cardio::fd_event::error |
            cardio::fd_event::hangup,
        runtime->io_cancellation.get_cancellation());
    if ((events & cardio::fd_event::error) != cardio::fd_event::none) {
      throw std::runtime_error("Node IPC socket reported a write error");
    }
  }
}
#endif

static cardio::promise<void> ReadMuonNodeExact(
    MuonNodeRuntime* runtime,
    std::span<std::byte> buffer) {
  auto offset = size_t{0};
  while (offset < buffer.size()) {
    const auto count =
        co_await ReadMuonNodeTransport(runtime, buffer.subspan(offset));
    if (count == 0) {
      throw std::runtime_error("Node IPC transport closed");
    }
    offset += count;
  }
}

static cardio::promise<void> WriteMuonNodeExact(
    MuonNodeRuntime* runtime,
    std::span<const std::byte> buffer) {
  auto offset = size_t{0};
  while (offset < buffer.size()) {
    const auto count =
        co_await WriteMuonNodeTransport(runtime, buffer.subspan(offset));
    if (count == 0) {
      throw std::runtime_error("Node IPC transport closed while writing");
    }
    offset += count;
  }
}

static void CloseMuonNodeTransport(MuonNodeRuntime* runtime) {
  if (runtime == nullptr) {
    return;
  }
#if defined(_WIN32)
  if (runtime->pipe != INVALID_HANDLE_VALUE) {
    (void)DisconnectNamedPipe(runtime->pipe);
    (void)CloseHandle(runtime->pipe);
    runtime->pipe = INVALID_HANDLE_VALUE;
  }
#else
  if (runtime->socket_fd >= 0) {
    (void)::close(runtime->socket_fd);
    runtime->socket_fd = -1;
  }
#endif
  runtime->transport_connected = false;
}

static void CloseMuonNodeProcessHandles(MuonNodeRuntime* runtime) {
  if (runtime == nullptr) {
    return;
  }
#if defined(_WIN32)
  CloseMuonWindowsJobProcess(&runtime->process);
#else
  if (runtime->process_fd >= 0) {
    (void)::close(runtime->process_fd);
    runtime->process_fd = -1;
  }
  runtime->process_id = -1;
#endif
}

static void TerminateMuonNodeProcess(MuonNodeRuntime* runtime,
                                     bool force) {
  if (runtime == nullptr || !runtime->process_started ||
      runtime->process_exited) {
    return;
  }
#if defined(_WIN32)
  (void)TerminateMuonWindowsJobProcess(
      &runtime->process, force ? 137u : 143u);
#else
  if (runtime->process_id > 0) {
    (void)::kill(runtime->process_id, force ? SIGKILL : SIGTERM);
  }
#endif
}

static void CompleteMuonNodePendingRequests(
    MuonNodeRuntime* runtime,
    const std::string& error_message) {
  if (runtime == nullptr) {
    return;
  }
  auto pending = std::move(runtime->pending_requests);
  runtime->pending_requests.clear();
  for (auto& entry : pending) {
    if (entry.second.complete) {
      entry.second.complete(false, "", error_message);
    }
  }
}

static void MaybeCompleteMuonNodeManagerStop(MuonNodeManager* manager) {
  if (manager == nullptr || !manager->stopping) {
    return;
  }
  for (const auto& entry : manager->runtimes) {
    if (entry.second &&
        (entry.second->state != MuonNodeRuntimeState::Stopped ||
         entry.second->active_renderer_callbacks != 0)) {
      return;
    }
  }
  const auto completion =
      std::exchange(manager->stop_completion, nullptr);
  auto* user_data = std::exchange(manager->stop_user_data, nullptr);
  if (completion != nullptr) {
    completion(user_data);
  }
}

static void MaybeCompleteMuonNodeStop(MuonNodeRuntime* runtime) {
  if (runtime == nullptr ||
      runtime->state != MuonNodeRuntimeState::Stopping ||
      runtime->startup_active || runtime->reader_active ||
      runtime->writer_active || runtime->process_monitor_active ||
      runtime->shutdown_timer_active ||
      (runtime->process_started && !runtime->process_exited)) {
    return;
  }
  CloseMuonNodeTransport(runtime);
  CloseMuonNodeProcessHandles(runtime);
  runtime->write_queue.clear();
  runtime->callback_leases.clear();
  runtime->pending_callback_requests.clear();
  runtime->state = MuonNodeRuntimeState::Stopped;
  auto release_invocations = std::move(runtime->release_invocations);
  runtime->release_invocations.clear();
  for (const auto& invocation : release_invocations) {
    CompleteMuonNodeHostInvocation(
        runtime, invocation, true, "", "");
  }
  MaybeCompleteMuonNodeManagerStop(runtime->manager);
}

static void FailMuonNodeRuntime(MuonNodeRuntime* runtime,
                                const std::string& error_message) {
  if (runtime == nullptr ||
      runtime->state == MuonNodeRuntimeState::Stopped) {
    return;
  }
  if (runtime->failure_message.empty()) {
    runtime->failure_message =
        error_message.empty() ? "Node runtime failed" : error_message;
    LogMuonNodeMessage(MUON_LOG_LEVEL_ERROR, runtime->failure_message);
  }
  if (runtime->state != MuonNodeRuntimeState::Stopping) {
    runtime->state = MuonNodeRuntimeState::Failed;
  }
  if (runtime->create_invocation) {
    const auto invocation =
        std::exchange(runtime->create_invocation, nullptr);
    CompleteMuonNodeHostInvocation(
        runtime, invocation, false, "", runtime->failure_message);
  }
  CompleteAllMuonNodeHostInvocations(runtime, runtime->failure_message);
  CompleteMuonNodePendingRequests(runtime, runtime->failure_message);
  runtime->pending_callback_requests.clear();
  (void)runtime->io_cancellation.cancel();
  TerminateMuonNodeProcess(runtime, true);
  MaybeCompleteMuonNodeStop(runtime);
}

static void CompleteMuonNodeRendererCallback(
    void* raw_state,
    const char* value_json,
    const muon_completion_error* error) {
  auto state = std::unique_ptr<MuonNodeRendererCallbackState>(
      static_cast<MuonNodeRendererCallbackState*>(raw_state));
  if (!state) {
    return;
  }
  auto* runtime = state->runtime;
  const auto was_pending =
      runtime != nullptr &&
      runtime->pending_callback_requests.erase(state->request_id) != 0;
  if (was_pending && runtime->state == MuonNodeRuntimeState::Ready) {
    try {
      if (error == nullptr && value_json != nullptr) {
        EnqueueMuonNodeMessage(
            runtime,
            CreateMuonNodeCallbackResultJson(
                state->request_id, true, value_json, ""));
      } else {
        const auto* error_message =
            error == nullptr
                ? "Renderer callback returned no value"
                : error->message[0] == '\0'
                      ? "Renderer callback dispatch failed"
                      : error->message;
        EnqueueMuonNodeMessage(
            runtime,
            CreateMuonNodeCallbackResultJson(
                state->request_id, false, "null", error_message));
      }
    } catch (...) {
      try {
        EnqueueMuonNodeMessage(
            runtime,
            CreateMuonNodeCallbackResultJson(
                state->request_id, false, "null",
                "Renderer callback result could not cross Node IPC"));
      } catch (...) {
        try {
          FailMuonNodeRuntime(
              runtime, "Failed to complete a renderer callback");
        } catch (...) {
        }
      }
    }
  }
  if (state->completion_function != nullptr &&
      g_muon_node_helpers != nullptr &&
      g_muon_node_helpers->__release_plugin_function_pointer_impl != nullptr) {
    g_muon_node_helpers->release_plugin_function_pointer(
        state->completion_function);
  }
  state.reset();
  if (runtime != nullptr && runtime->active_renderer_callbacks > 0) {
    runtime->active_renderer_callbacks -= 1;
    MaybeCompleteMuonNodeStop(runtime);
    MaybeCompleteMuonNodeManagerStop(runtime->manager);
  }
}

static void HandleMuonNodeCallbackMessage(MuonNodeRuntime* runtime,
                                          yyjson_val* root) {
  if (runtime == nullptr ||
      runtime->state != MuonNodeRuntimeState::Ready) {
    return;
  }
  auto* id = yyjson_obj_get(root, "id");
  auto* handle = yyjson_obj_get(root, "handle");
  auto* arguments = yyjson_obj_get(root, "arguments");
  if (!yyjson_is_str(id) || !yyjson_is_str(handle) ||
      !yyjson_is_arr(arguments)) {
    throw std::runtime_error("Node IPC callback message is invalid");
  }
  const auto id_value = std::string(yyjson_get_str(id), yyjson_get_len(id));
  if (id_value.empty() ||
      id_value.size() > kMuonNodeMaximumCallbackRequestIdSize) {
    throw std::runtime_error(
        "Node IPC callback request id is invalid");
  }
  const auto handle_value =
      std::string(yyjson_get_str(handle), yyjson_get_len(handle));
  const auto lease = runtime->callback_leases.find(handle_value);
  if (lease == runtime->callback_leases.end() ||
      lease->second.dispatcher == nullptr) {
    EnqueueMuonNodeMessage(
        runtime,
        CreateMuonNodeCallbackResultJson(
            id_value, false, "null",
            "Renderer callback handle is no longer available"));
    return;
  }
  if (g_muon_node_helpers == nullptr ||
      g_muon_node_helpers->__create_completion_function_impl == nullptr ||
      g_muon_node_helpers->__release_plugin_function_pointer_impl == nullptr) {
    throw std::runtime_error(
        "Muon completion helpers are unavailable for Node callbacks");
  }

  auto state = std::make_unique<MuonNodeRendererCallbackState>();
  state->runtime = runtime;
  state->request_id = id_value;
  char helper_error_storage[MUON_COMPLETION_ERROR_MESSAGE_CAPACITY] = "";
  auto helper_error = muon_error_buffer{
      helper_error_storage,
      static_cast<uint32_t>(sizeof(helper_error_storage)),
  };
  if (!g_muon_node_helpers->create_completion_function(
          &kMuonNodeStringType, &CompleteMuonNodeRendererCallback, state.get(),
          &state->completion_function, &helper_error)) {
    throw std::runtime_error(
        helper_error_storage[0] == '\0'
            ? "Failed to create a renderer callback completion"
            : helper_error_storage);
  }

  auto* payload_document = yyjson_mut_doc_new(nullptr);
  if (payload_document == nullptr) {
    g_muon_node_helpers->release_plugin_function_pointer(
        state->completion_function);
    throw std::runtime_error(
        "Failed to allocate a renderer callback payload");
  }
  const auto payload_cleanup =
      std::unique_ptr<yyjson_mut_doc, decltype(&yyjson_mut_doc_free)>(
          payload_document, &yyjson_mut_doc_free);
  auto* payload = yyjson_mut_obj(payload_document);
  auto* copied_arguments =
      yyjson_val_mut_copy(payload_document, arguments);
  if (payload == nullptr || copied_arguments == nullptr ||
      !yyjson_mut_obj_add_strcpy(
          payload_document, payload, "id", id_value.c_str()) ||
      !yyjson_mut_obj_add_strcpy(
          payload_document, payload, "handle", handle_value.c_str()) ||
      !yyjson_mut_obj_add_val(
          payload_document, payload, "arguments", copied_arguments)) {
    g_muon_node_helpers->release_plugin_function_pointer(
        state->completion_function);
    throw std::runtime_error(
        "Failed to construct a renderer callback payload");
  }
  yyjson_mut_doc_set_root(payload_document, payload);
  const auto payload_json = WriteMuonNodeMutableJson(payload_document);
  if (!runtime->pending_callback_requests.insert(id_value).second) {
    g_muon_node_helpers->release_plugin_function_pointer(
        state->completion_function);
    throw std::runtime_error(
        "Node IPC returned a duplicate callback request id");
  }
  using RendererCallbackFunction = void (*)(muon_completion_func,
                                            const char*);
  const auto completion_function = state->completion_function;
  runtime->active_renderer_callbacks += 1;
  (void)state.release();
  reinterpret_cast<RendererCallbackFunction>(
      lease->second.dispatcher)(completion_function, payload_json.c_str());
}

static void HandleMuonNodeResponseMessage(MuonNodeRuntime* runtime,
                                          yyjson_val* root) {
  auto* id = yyjson_obj_get(root, "id");
  auto* ok = yyjson_obj_get(root, "ok");
  if (!yyjson_is_str(id) || !yyjson_is_bool(ok)) {
    throw std::runtime_error("Node IPC response message is invalid");
  }
  const auto id_value = std::string(yyjson_get_str(id), yyjson_get_len(id));
  const auto pending = runtime->pending_requests.find(id_value);
  if (pending == runtime->pending_requests.end()) {
    if (runtime->state == MuonNodeRuntimeState::Stopping ||
        runtime->state == MuonNodeRuntimeState::Stopped) {
      return;
    }
    throw std::runtime_error(
        "Node IPC returned an unknown request id: " + id_value);
  }
  auto request = std::move(pending->second);
  runtime->pending_requests.erase(pending);
  if (yyjson_get_bool(ok)) {
    auto* value = yyjson_obj_get(root, "value");
    request.complete(true, WriteMuonNodeJsonValue(value), "");
    return;
  }
  auto* error = yyjson_obj_get(root, "error");
  auto* message =
      yyjson_is_obj(error) ? yyjson_obj_get(error, "message") : nullptr;
  const auto error_message =
      yyjson_is_str(message)
          ? std::string(yyjson_get_str(message), yyjson_get_len(message))
          : std::string("Node IPC request failed");
  request.complete(false, "", error_message);
}

static void HandleMuonNodeMessage(MuonNodeRuntime* runtime,
                                  const std::string& json) {
  const auto document = ReadMuonNodeJson(json);
  if (!document) {
    throw std::runtime_error("Node IPC returned invalid JSON");
  }
  auto* root = yyjson_doc_get_root(document.get());
  auto* kind =
      yyjson_is_obj(root) ? yyjson_obj_get(root, "kind") : nullptr;
  if (!yyjson_is_str(kind)) {
    throw std::runtime_error("Node IPC message kind is unavailable");
  }
  const auto kind_value =
      std::string(yyjson_get_str(kind), yyjson_get_len(kind));
  if (kind_value == "response") {
    HandleMuonNodeResponseMessage(runtime, root);
    return;
  }
  if (kind_value == "callback") {
    HandleMuonNodeCallbackMessage(runtime, root);
    return;
  }
  throw std::runtime_error(
      "Node IPC returned an unsupported message kind: " + kind_value);
}

static cardio::promise<void> RunMuonNodeReader(MuonNodeRuntime* runtime) {
  runtime->reader_active = true;
  try {
    for (;;) {
      std::array<std::byte, 4> header{};
      co_await ReadMuonNodeExact(runtime, header);
      const auto size =
          (std::to_integer<uint32_t>(header[0]) << 24) |
          (std::to_integer<uint32_t>(header[1]) << 16) |
          (std::to_integer<uint32_t>(header[2]) << 8) |
          std::to_integer<uint32_t>(header[3]);
      if (size == 0 || size > kMuonNodeMaximumFrameSize) {
        throw std::runtime_error(
            "Node IPC returned an invalid frame size");
      }
      std::string json(size, '\0');
      co_await ReadMuonNodeExact(
          runtime,
          std::as_writable_bytes(std::span<char>(json.data(), json.size())));
      HandleMuonNodeMessage(runtime, json);
    }
  } catch (const cardio::canceled_exception&) {
  } catch (...) {
    if (runtime->state != MuonNodeRuntimeState::Stopping &&
        runtime->state != MuonNodeRuntimeState::Stopped) {
      FailMuonNodeRuntime(
          runtime,
          GetMuonNodeExceptionMessage("Node IPC reader failed"));
    }
  }
  runtime->reader_active = false;
  MaybeCompleteMuonNodeStop(runtime);
}

static cardio::promise<void> RunMuonNodeWriter(MuonNodeRuntime* runtime) {
  runtime->writer_active = true;
  try {
    while (!runtime->write_queue.empty()) {
      auto frame = std::move(runtime->write_queue.front());
      runtime->write_queue.pop_front();
      co_await WriteMuonNodeExact(runtime, frame);
    }
  } catch (const cardio::canceled_exception&) {
  } catch (...) {
    if (runtime->state != MuonNodeRuntimeState::Stopping &&
        runtime->state != MuonNodeRuntimeState::Stopped) {
      FailMuonNodeRuntime(
          runtime,
          GetMuonNodeExceptionMessage("Node IPC writer failed"));
    }
  }
  runtime->writer_active = false;
  if (!runtime->write_queue.empty() &&
      runtime->state != MuonNodeRuntimeState::Stopped) {
    cardio::fire_and_forget(RunMuonNodeWriter(runtime));
    co_return;
  }
  MaybeCompleteMuonNodeStop(runtime);
}

static void EnqueueMuonNodeMessage(MuonNodeRuntime* runtime,
                                   const std::string& json) {
  if (runtime == nullptr ||
      runtime->state == MuonNodeRuntimeState::Stopped) {
    return;
  }
  runtime->write_queue.push_back(CreateMuonNodeFrame(json));
  if (!runtime->writer_active) {
    cardio::fire_and_forget(RunMuonNodeWriter(runtime));
  }
}

static std::string AllocateMuonNodeRequestId(MuonNodeRuntime* runtime) {
  if (runtime == nullptr ||
      runtime->next_request_id == std::numeric_limits<uint64_t>::max()) {
    throw std::runtime_error("Node IPC request ids are exhausted");
  }
  const auto id = std::to_string(runtime->next_request_id);
  runtime->next_request_id += 1;
  return id;
}

static void RegisterAndSendMuonNodeRequest(
    MuonNodeRuntime* runtime,
    const std::string& id,
    const std::string& json,
    MuonNodePendingRequest request) {
  if (runtime == nullptr) {
    throw std::runtime_error("Node runtime is unavailable");
  }
  if (runtime->pending_requests.size() >=
      kMuonNodeMaximumPendingRequests) {
    throw std::runtime_error(
        "Node IPC pending request limit was exceeded");
  }
  const auto inserted =
      runtime->pending_requests.emplace(id, std::move(request));
  if (!inserted.second) {
    throw std::runtime_error("Duplicate Node IPC request id");
  }
  try {
    EnqueueMuonNodeMessage(runtime, json);
  } catch (...) {
    runtime->pending_requests.erase(id);
    throw;
  }
}

static void ExecuteMuonNodeQueuedCommand(MuonNodeRuntime* runtime,
                                         MuonNodeQueuedCommand command) {
  if (runtime == nullptr ||
      runtime->state != MuonNodeRuntimeState::Ready) {
    CompleteMuonNodeHostInvocation(
        runtime, command.invocation, false, "",
        runtime == nullptr || runtime->failure_message.empty()
            ? "Node runtime is unavailable"
            : runtime->failure_message);
    return;
  }
  try {
    const auto id = AllocateMuonNodeRequestId(runtime);
    const auto json = CreateMuonNodeCommandRequestJson(id, command);
    auto invocation = command.invocation;
    RegisterAndSendMuonNodeRequest(
        runtime, id, json,
        {[runtime, invocation](
             bool success, const std::string& value_json,
             const std::string& error_message) {
          CompleteMuonNodeHostInvocation(
              runtime, invocation, success, value_json, error_message);
        }});
  } catch (...) {
    CompleteMuonNodeHostInvocation(
        runtime, command.invocation, false, "",
        GetMuonNodeExceptionMessage("Failed to submit a Node IPC request"));
  }
}

static void DrainMuonNodeQueuedCommands(MuonNodeRuntime* runtime) {
  if (runtime == nullptr ||
      runtime->state != MuonNodeRuntimeState::Ready) {
    return;
  }
  auto commands = std::move(runtime->queued_commands);
  runtime->queued_commands.clear();
  for (auto& command : commands) {
    ExecuteMuonNodeQueuedCommand(runtime, std::move(command));
  }
}

static void CompleteMuonNodeInitialization(
    MuonNodeRuntime* runtime,
    bool success,
    const std::string&,
    const std::string& error_message) {
  if (runtime == nullptr ||
      runtime->state == MuonNodeRuntimeState::Stopped) {
    return;
  }
  if (!success) {
    FailMuonNodeRuntime(
        runtime,
        error_message.empty()
            ? "Node bridge initialization failed"
            : "Node bridge initialization failed: " + error_message);
    return;
  }
  if (runtime->state == MuonNodeRuntimeState::Stopping) {
    SendMuonNodeShutdownRequest(runtime);
    return;
  }
  runtime->state = MuonNodeRuntimeState::Ready;
  LogMuonNodeMessage(
      MUON_LOG_LEVEL_INFO,
      "Node sidecar is ready for project " + runtime->project_root);
  if (runtime->create_invocation) {
    const auto invocation =
        std::exchange(runtime->create_invocation, nullptr);
    CompleteMuonNodeHostInvocation(
        runtime, invocation, true, runtime->instance_id, "");
  }
  DrainMuonNodeQueuedCommands(runtime);
}

static void SendMuonNodeInitializeRequest(MuonNodeRuntime* runtime) {
  const auto id = AllocateMuonNodeRequestId(runtime);
  const auto json = CreateMuonNodeInitializeRequestJson(id, *runtime);
  RegisterAndSendMuonNodeRequest(
      runtime, id, json,
      {[runtime](bool success, const std::string& value_json,
                 const std::string& error_message) {
        CompleteMuonNodeInitialization(
            runtime, success, value_json, error_message);
      }});
}

#if defined(_WIN32)
static std::wstring ConvertMuonNodeUtf8ToWide(const std::string& value) {
  if (value.empty()) {
    return {};
  }
  const auto size = MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
      static_cast<int>(value.size()), nullptr, 0);
  if (size <= 0) {
    throw std::runtime_error("Failed to decode a Node UTF-8 path");
  }
  std::wstring result(static_cast<size_t>(size), L'\0');
  if (MultiByteToWideChar(
          CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
          static_cast<int>(value.size()), result.data(), size) != size) {
    throw std::runtime_error("Failed to decode a Node UTF-8 path");
  }
  return result;
}

static std::wstring QuoteMuonNodeWindowsArgument(
    const std::wstring& argument) {
  if (argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
    return argument;
  }
  std::wstring result = L"\"";
  auto slash_count = size_t{0};
  for (const auto character : argument) {
    if (character == L'\\') {
      slash_count += 1;
      continue;
    }
    if (character == L'"') {
      result.append(slash_count * 2 + 1, L'\\');
      result.push_back(L'"');
      slash_count = 0;
      continue;
    }
    result.append(slash_count, L'\\');
    slash_count = 0;
    result.push_back(character);
  }
  result.append(slash_count * 2, L'\\');
  result.push_back(L'"');
  return result;
}

static int CompareMuonNodeWindowsStrings(
    const std::wstring& first,
    const std::wstring& second) {
  if (first.size() >
          static_cast<size_t>((std::numeric_limits<int>::max)()) ||
      second.size() >
          static_cast<size_t>((std::numeric_limits<int>::max)())) {
    throw std::runtime_error(
        "Node environment entry exceeds the Win32 string size limit");
  }
  const auto result = CompareStringOrdinal(
      first.data(), static_cast<int>(first.size()),
      second.data(), static_cast<int>(second.size()), TRUE);
  if (result == 0) {
    throw std::system_error(
        static_cast<int>(GetLastError()), std::system_category(),
        "CompareStringOrdinal failed for Node environment");
  }
  return result;
}

static bool IsMuonNodeWindowsEnvironmentEntry(
    const std::wstring& entry,
    const std::wstring& name) {
  if (entry.size() <= name.size() ||
      entry[name.size()] != L'=') {
    return false;
  }
  return CompareMuonNodeWindowsStrings(
             entry.substr(0, name.size()), name) == CSTR_EQUAL;
}

static std::vector<wchar_t> CreateMuonNodeWindowsEnvironment(
    const std::wstring& pipe_name,
    const std::wstring& token) {
  auto* environment = GetEnvironmentStringsW();
  if (environment == nullptr) {
    throw std::runtime_error(
        "GetEnvironmentStringsW failed for Node sidecar");
  }
  auto environment_guard =
      std::unique_ptr<wchar_t, decltype(&FreeEnvironmentStringsW)>(
          environment, &FreeEnvironmentStringsW);
  std::vector<std::wstring> entries;
  for (auto* cursor = environment; *cursor != L'\0';) {
    const auto entry = std::wstring(cursor);
    cursor += entry.size() + 1;
    if (IsMuonNodeWindowsEnvironmentEntry(
            entry, L"MUON_NODE_PIPE") ||
        IsMuonNodeWindowsEnvironmentEntry(
            entry, L"MUON_NODE_TOKEN")) {
      continue;
    }
    entries.push_back(entry);
  }
  entries.push_back(L"MUON_NODE_PIPE=" + pipe_name);
  entries.push_back(L"MUON_NODE_TOKEN=" + token);
  // CreateProcessW requires a locale-independent, case-insensitive Unicode
  // ordering for caller-supplied environment blocks.
  std::sort(
      entries.begin(), entries.end(),
      [](const std::wstring& first, const std::wstring& second) {
        return CompareMuonNodeWindowsStrings(first, second) ==
               CSTR_LESS_THAN;
      });

  std::vector<wchar_t> block;
  for (const auto& entry : entries) {
    block.insert(block.end(), entry.begin(), entry.end());
    block.push_back(L'\0');
  }
  block.push_back(L'\0');
  return block;
}

static void CloseMuonNodeWindowsHandle(HANDLE* handle) noexcept {
  if (handle == nullptr || *handle == nullptr ||
      *handle == INVALID_HANDLE_VALUE) {
    return;
  }
  (void)CloseHandle(*handle);
  *handle = nullptr;
}

static HANDLE CreateMuonNodeInheritedStandardHandle(
    DWORD standard_handle,
    DWORD null_access) {
  const auto source = GetStdHandle(standard_handle);
  if (source != nullptr && source != INVALID_HANDLE_VALUE) {
    HANDLE inherited = nullptr;
    if (DuplicateHandle(
            GetCurrentProcess(), source, GetCurrentProcess(), &inherited,
            0, TRUE, DUPLICATE_SAME_ACCESS) != FALSE) {
      return inherited;
    }
    const auto error = GetLastError();
    // GUI subsystem processes normally have no standard handles. Treat stale
    // values as absent, but preserve actionable duplication failures.
    if (error != ERROR_INVALID_HANDLE) {
      throw std::system_error(
          static_cast<int>(error), std::system_category(),
          "Failed to duplicate a Node standard handle");
    }
  }

  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  const auto handle = CreateFileW(
      L"NUL", null_access, FILE_SHARE_READ | FILE_SHARE_WRITE,
      &attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    throw std::system_error(
        static_cast<int>(GetLastError()), std::system_category(),
        "Failed to open NUL for the Node sidecar");
  }
  return handle;
}

static cardio::promise<void> ConnectMuonNodeWindowsPipe(
    MuonNodeRuntime* runtime) {
  const auto event =
      CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (event == nullptr) {
    throw std::system_error(
        static_cast<int>(GetLastError()), std::system_category(),
        "CreateEventW failed for Node IPC connection");
  }
  OVERLAPPED overlapped{};
  overlapped.hEvent = event;
  auto pending = false;
  try {
    if (ConnectNamedPipe(runtime->pipe, &overlapped) == FALSE) {
      const auto error = GetLastError();
      if (error == ERROR_PIPE_CONNECTED) {
        // A client won the documented connect race; no OVERLAPPED operation
        // is pending in this case.
        (void)CloseHandle(event);
        co_return;
      }
      if (error != ERROR_IO_PENDING) {
        throw std::system_error(
            static_cast<int>(error), std::system_category(),
            "ConnectNamedPipe failed for Node sidecar");
      }
      pending = true;
      (void)co_await cardio::from_win32_overlapped(
          runtime->pipe, overlapped,
          runtime->io_cancellation.get_cancellation());
      pending = false;
    }
  } catch (...) {
    if (pending) {
      (void)CancelIoEx(runtime->pipe, &overlapped);
      DWORD ignored = 0;
      (void)GetOverlappedResult(
          runtime->pipe, &overlapped, &ignored, TRUE);
    }
    (void)CloseHandle(event);
    throw;
  }
  (void)CloseHandle(event);
}

static cardio::promise<void> StartMuonNodeProcess(
    MuonNodeRuntime* runtime) {
  const auto pipe_name =
      std::wstring(L"\\\\.\\pipe\\muon-node-") +
      std::to_wstring(GetCurrentProcessId()) + L"-" +
      ConvertMuonNodeUtf8ToWide(CreateMuonNodeSessionToken());
  runtime->pipe = CreateNamedPipeW(
      pipe_name.c_str(),
      PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
          PIPE_REJECT_REMOTE_CLIENTS,
      1, static_cast<DWORD>(kMuonNodeMaximumFrameSize),
      static_cast<DWORD>(kMuonNodeMaximumFrameSize), 0, nullptr);
  if (runtime->pipe == INVALID_HANDLE_VALUE) {
    throw std::system_error(
        static_cast<int>(GetLastError()), std::system_category(),
        "CreateNamedPipeW failed for Node sidecar");
  }

  const auto executable =
      ConvertMuonNodeUtf8ToWide(runtime->executable_path);
  const auto bridge = ConvertMuonNodeUtf8ToWide(runtime->bridge_path);
  auto command_line =
      QuoteMuonNodeWindowsArgument(executable) + L" " +
      QuoteMuonNodeWindowsArgument(bridge);
  std::vector<wchar_t> command_line_buffer(
      command_line.begin(), command_line.end());
  command_line_buffer.push_back(L'\0');
  auto environment = CreateMuonNodeWindowsEnvironment(
      pipe_name, ConvertMuonNodeUtf8ToWide(runtime->session_token));

  std::array<HANDLE, 3> inherited_standard_handles{
      nullptr, nullptr, nullptr};
  try {
    inherited_standard_handles[0] =
        CreateMuonNodeInheritedStandardHandle(
            STD_INPUT_HANDLE, GENERIC_READ);
    inherited_standard_handles[1] =
        CreateMuonNodeInheritedStandardHandle(
            STD_OUTPUT_HANDLE, GENERIC_WRITE);
    inherited_standard_handles[2] =
        CreateMuonNodeInheritedStandardHandle(
            STD_ERROR_HANDLE, GENERIC_WRITE);

    MuonWindowsJobProcessLaunchOptions launch_options{};
    launch_options.application_name = executable.c_str();
    launch_options.command_line = command_line_buffer.data();
    launch_options.environment = environment.data();
    launch_options.creation_flags =
        CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW;
    launch_options.startup_info.dwFlags = STARTF_USESTDHANDLES;
    launch_options.startup_info.hStdInput =
        inherited_standard_handles[0];
    launch_options.startup_info.hStdOutput =
        inherited_standard_handles[1];
    launch_options.startup_info.hStdError =
        inherited_standard_handles[2];
    launch_options.inherited_handles =
        inherited_standard_handles.data();
    launch_options.inherited_handle_count =
        inherited_standard_handles.size();
    launch_options.lifetime =
        MuonWindowsJobProcessLifetime::KillOnOwnerClose;
    runtime->process =
        LaunchMuonWindowsJobProcess(launch_options);
    for (auto& handle : inherited_standard_handles) {
      CloseMuonNodeWindowsHandle(&handle);
    }
    runtime->process_started = true;
  } catch (...) {
    for (auto& handle : inherited_standard_handles) {
      CloseMuonNodeWindowsHandle(&handle);
    }
    throw;
  }
  // Observe pre-connect exits so ConnectNamedPipe cannot strand startup.
  runtime->process_monitor_active = true;
  cardio::fire_and_forget(MonitorMuonNodeProcess(runtime));
  co_await ConnectMuonNodeWindowsPipe(runtime);
  // Authenticate the peer before the caller can send the initialize frame,
  // which is the first frame containing the session token.
  ULONG pipe_client_process_id = 0;
  if (GetNamedPipeClientProcessId(
          runtime->pipe, &pipe_client_process_id) == FALSE) {
    throw std::system_error(
        static_cast<int>(GetLastError()), std::system_category(),
        "GetNamedPipeClientProcessId failed for Node sidecar");
  }
  if (runtime->process.process_id == 0 ||
      pipe_client_process_id != runtime->process.process_id) {
    throw std::runtime_error(
        "Node IPC pipe was connected by an unexpected process");
  }
  runtime->transport_connected = true;
}
#else
static std::vector<std::string> CreateMuonNodePosixEnvironment(
    const std::string& token) {
  std::vector<std::string> entries;
  for (auto** cursor = environ; cursor != nullptr && *cursor != nullptr;
       ++cursor) {
    const auto entry = std::string(*cursor);
    if (entry.rfind("MUON_NODE_FD=", 0) == 0 ||
        entry.rfind("MUON_NODE_TOKEN=", 0) == 0) {
      continue;
    }
    entries.push_back(entry);
  }
  entries.push_back("MUON_NODE_FD=3");
  entries.push_back("MUON_NODE_TOKEN=" + token);
  return entries;
}

static cardio::promise<void> StartMuonNodeProcess(
    MuonNodeRuntime* runtime) {
  int sockets[2] = {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0) {
    throw std::system_error(
        errno, std::generic_category(),
        "socketpair failed for Node sidecar");
  }
  runtime->socket_fd = sockets[0];
  auto child_socket = sockets[1];
  if (child_socket == 3) {
    const auto duplicated = fcntl(child_socket, F_DUPFD_CLOEXEC, 4);
    if (duplicated < 0) {
      (void)::close(runtime->socket_fd);
      (void)::close(child_socket);
      runtime->socket_fd = -1;
      throw std::system_error(
          errno, std::generic_category(),
          "Failed to reserve the Node sidecar descriptor");
    }
    (void)::close(child_socket);
    child_socket = duplicated;
  }
  const auto close_sockets = [&]() {
    if (runtime->socket_fd >= 0) {
      (void)::close(runtime->socket_fd);
      runtime->socket_fd = -1;
    }
    if (child_socket >= 0) {
      (void)::close(child_socket);
    }
  };

  posix_spawn_file_actions_t actions{};
  if (posix_spawn_file_actions_init(&actions) != 0) {
    close_sockets();
    throw std::runtime_error(
        "posix_spawn file actions initialization failed");
  }
  const auto destroy_actions =
      std::unique_ptr<posix_spawn_file_actions_t,
                      std::function<void(posix_spawn_file_actions_t*)>>(
          &actions, [](posix_spawn_file_actions_t* value) {
            (void)posix_spawn_file_actions_destroy(value);
          });
  auto action_error =
      posix_spawn_file_actions_addclose(&actions, runtime->socket_fd);
  if (action_error == 0) {
    action_error =
        posix_spawn_file_actions_adddup2(&actions, child_socket, 3);
  }
  if (action_error == 0) {
    action_error =
        posix_spawn_file_actions_addclose(&actions, child_socket);
  }
  if (action_error != 0) {
    close_sockets();
    throw std::system_error(
        action_error, std::generic_category(),
        "Failed to configure Node sidecar descriptors");
  }
  const auto current_flags = fcntl(runtime->socket_fd, F_GETFL, 0);
  if (current_flags < 0 ||
      fcntl(runtime->socket_fd, F_SETFL, current_flags | O_NONBLOCK) != 0) {
    const auto configure_error = errno;
    close_sockets();
    throw std::system_error(
        configure_error, std::generic_category(),
        "Failed to configure the Node IPC socket");
  }

  auto arguments_storage =
      std::vector<std::string>{runtime->executable_path,
                               runtime->bridge_path};
  std::vector<char*> arguments;
  for (auto& argument : arguments_storage) {
    arguments.push_back(argument.data());
  }
  arguments.push_back(nullptr);
  auto environment_storage =
      CreateMuonNodePosixEnvironment(runtime->session_token);
  std::vector<char*> environment;
  for (auto& entry : environment_storage) {
    environment.push_back(entry.data());
  }
  environment.push_back(nullptr);

  pid_t process_id = -1;
  const auto spawn_error = posix_spawn(
      &process_id, runtime->executable_path.c_str(), &actions, nullptr,
      arguments.data(), environment.data());
  (void)::close(child_socket);
  if (spawn_error != 0) {
    (void)::close(runtime->socket_fd);
    runtime->socket_fd = -1;
    throw std::system_error(
        spawn_error, std::generic_category(),
        "Failed to start Node executable " + runtime->executable_path);
  }
#if defined(SYS_pidfd_open)
  const auto process_fd =
      static_cast<int>(::syscall(SYS_pidfd_open, process_id, 0));
  if (process_fd < 0) {
    const auto pidfd_error = errno;
    (void)::kill(process_id, SIGKILL);
    int ignored_status = 0;
    (void)::waitpid(process_id, &ignored_status, 0);
    CloseMuonNodeTransport(runtime);
    throw std::system_error(
        pidfd_error, std::generic_category(),
        "pidfd_open failed for Node sidecar");
  }
  runtime->process_fd = process_fd;
#else
  (void)::kill(process_id, SIGKILL);
  int ignored_status = 0;
  (void)::waitpid(process_id, &ignored_status, 0);
  CloseMuonNodeTransport(runtime);
  throw std::runtime_error(
      "pidfd_open is required for the Node sidecar");
#endif
  runtime->process_id = process_id;
  runtime->process_started = true;
  runtime->transport_connected = true;
  co_return;
}
#endif

static void HandleMuonNodeProcessExit(MuonNodeRuntime* runtime,
                                      int exit_code,
                                      bool exit_status_available) {
  if (runtime == nullptr || runtime->process_exited) {
    return;
  }
  runtime->process_exited = true;
  (void)runtime->io_cancellation.cancel();
  (void)runtime->shutdown_timer_cancellation.cancel();
  if (runtime->state == MuonNodeRuntimeState::Stopping) {
    CompleteMuonNodePendingRequests(
        runtime, "Node runtime stopped before the request completed");
    runtime->callback_leases.clear();
  } else if (runtime->state != MuonNodeRuntimeState::Stopped) {
    FailMuonNodeRuntime(
        runtime,
        exit_status_available
            ? "Node runtime terminated with exit code " +
                  std::to_string(exit_code)
            : "Node runtime terminated; exit status is unavailable");
  }
}

static cardio::promise<void> MonitorMuonNodeProcess(
    MuonNodeRuntime* runtime) {
  runtime->process_monitor_active = true;
  auto exit_code = -1;
  auto exit_status_available = false;
  try {
#if defined(_WIN32)
    if (runtime->process.process == nullptr) {
      throw std::runtime_error("Node process handle is unavailable");
    }
    (void)co_await cardio::from_win32_handle(
        runtime->process.process);
    DWORD native_exit_code = 0;
    if (GetExitCodeProcess(
            runtime->process.process, &native_exit_code) == FALSE) {
      throw std::system_error(
          static_cast<int>(GetLastError()), std::system_category(),
          "GetExitCodeProcess failed for Node sidecar");
    }
    exit_code = static_cast<int>(native_exit_code);
    exit_status_available = true;
#else
    if (runtime->process_fd < 0 || runtime->process_id <= 0) {
      throw std::runtime_error("Node process descriptor is unavailable");
    }
    (void)co_await cardio::from_fd(
        runtime->process_fd,
        cardio::fd_event::read | cardio::fd_event::error |
            cardio::fd_event::hangup);
    int status = 0;
    for (;;) {
      const auto waited = ::waitpid(runtime->process_id, &status, 0);
      if (waited == runtime->process_id) {
        exit_status_available = true;
        break;
      }
      const auto wait_error = errno;
      if (waited < 0 && wait_error == EINTR) {
        continue;
      }
      if (waited < 0 && wait_error == ECHILD) {
        break;
      }
      throw std::system_error(
          wait_error, std::generic_category(),
          "waitpid failed for Node sidecar");
    }
    if (exit_status_available && WIFEXITED(status)) {
      exit_code = WEXITSTATUS(status);
    } else if (exit_status_available && WIFSIGNALED(status)) {
      exit_code = 128 + WTERMSIG(status);
    } else {
      exit_status_available = false;
    }
#endif
    HandleMuonNodeProcessExit(
        runtime, exit_code, exit_status_available);
  } catch (...) {
    FailMuonNodeRuntime(
        runtime,
        GetMuonNodeExceptionMessage(
            "Failed to observe the Node sidecar process"));
  }
  runtime->process_monitor_active = false;
  MaybeCompleteMuonNodeStop(runtime);
}

static cardio::promise<void> RunMuonNodeShutdownTimer(
    MuonNodeRuntime* runtime) {
  runtime->shutdown_timer_active = true;
  try {
    co_await cardio::promises::delay(
        kMuonNodeShutdownTimeoutMilliseconds,
        runtime->shutdown_timer_cancellation.get_cancellation());
    if (!runtime->process_exited) {
      LogMuonNodeMessage(
          MUON_LOG_LEVEL_WARNING,
          "Node sidecar did not stop gracefully; terminating it");
      TerminateMuonNodeProcess(runtime, false);
      co_await cardio::promises::delay(
          kMuonNodeKillTimeoutMilliseconds,
          runtime->shutdown_timer_cancellation.get_cancellation());
      if (!runtime->process_exited) {
        LogMuonNodeMessage(
            MUON_LOG_LEVEL_WARNING,
            "Node sidecar did not terminate; killing it");
        TerminateMuonNodeProcess(runtime, true);
      }
    }
  } catch (const cardio::canceled_exception&) {
  } catch (...) {
    LogMuonNodeMessage(
        MUON_LOG_LEVEL_WARNING,
        GetMuonNodeExceptionMessage(
            "Node sidecar shutdown timer failed"));
    TerminateMuonNodeProcess(runtime, true);
  }
  runtime->shutdown_timer_active = false;
  MaybeCompleteMuonNodeStop(runtime);
}

static void SendMuonNodeShutdownRequest(MuonNodeRuntime* runtime) {
  if (runtime == nullptr || runtime->shutdown_sent ||
      runtime->state != MuonNodeRuntimeState::Stopping ||
      runtime->process_exited || !runtime->transport_connected) {
    return;
  }
  runtime->shutdown_sent = true;
  try {
    const auto id = AllocateMuonNodeRequestId(runtime);
    MuonNodeQueuedCommand shutdown;
    shutdown.command = "shutdown";
    const auto json = CreateMuonNodeCommandRequestJson(id, shutdown);
    RegisterAndSendMuonNodeRequest(
        runtime, id, json,
        {[runtime](bool success, const std::string&,
                   const std::string& error_message) {
          if (success) {
            LogMuonNodeMessage(
                MUON_LOG_LEVEL_DEBUG,
                "Node sidecar accepted shutdown");
          } else if (!runtime->process_exited) {
            LogMuonNodeMessage(
                MUON_LOG_LEVEL_WARNING,
                "Node sidecar rejected shutdown: " + error_message);
            TerminateMuonNodeProcess(runtime, false);
          }
        }});
  } catch (...) {
    LogMuonNodeMessage(
        MUON_LOG_LEVEL_WARNING,
        GetMuonNodeExceptionMessage(
            "Failed to send Node sidecar shutdown"));
    TerminateMuonNodeProcess(runtime, false);
  }
}

static void StartMuonNodeShutdownOperations(MuonNodeRuntime* runtime) {
  if (runtime == nullptr ||
      runtime->state != MuonNodeRuntimeState::Stopping ||
      !runtime->process_started || runtime->process_exited) {
    return;
  }
  if (!runtime->shutdown_timer_active) {
    runtime->shutdown_timer_cancellation =
        cardio::cancellation_source{};
    cardio::fire_and_forget(RunMuonNodeShutdownTimer(runtime));
  }
  if (!runtime->transport_connected) {
    // This cancels an in-flight ConnectNamedPipe during early shutdown.
    (void)runtime->io_cancellation.cancel();
    return;
  }
  SendMuonNodeShutdownRequest(runtime);
}

static void BeginMuonNodeShutdown(MuonNodeRuntime* runtime) {
  if (runtime == nullptr ||
      runtime->state == MuonNodeRuntimeState::Stopped) {
    return;
  }
  if (runtime->state != MuonNodeRuntimeState::Stopping) {
    runtime->state = MuonNodeRuntimeState::Stopping;
    if (runtime->create_invocation) {
      const auto invocation =
          std::exchange(runtime->create_invocation, nullptr);
      CompleteMuonNodeHostInvocation(
          runtime, invocation, false, "",
          "Node instance was released before the request completed");
    }
    CompleteAllMuonNodeHostInvocations(
        runtime,
        "Node instance was released before the request completed");
    CompleteMuonNodePendingRequests(
        runtime,
        "Node instance was released before the request completed");
    runtime->pending_callback_requests.clear();
  }
  if (!runtime->process_started || runtime->process_exited) {
    (void)runtime->io_cancellation.cancel();
    MaybeCompleteMuonNodeStop(runtime);
    return;
  }
  StartMuonNodeShutdownOperations(runtime);
}

static cardio::promise<void> StartMuonNodeRuntime(
    MuonNodeRuntime* runtime) {
  try {
    if (runtime->state == MuonNodeRuntimeState::Stopping ||
        runtime->state == MuonNodeRuntimeState::Stopped) {
      runtime->startup_active = false;
      MaybeCompleteMuonNodeStop(runtime);
      co_return;
    }
    runtime->session_token = CreateMuonNodeSessionToken();
    co_await StartMuonNodeProcess(runtime);
    if (runtime->process_exited) {
      throw std::runtime_error(
          "Node runtime terminated while connecting the IPC transport");
    }
    if (!runtime->process_monitor_active) {
      runtime->process_monitor_active = true;
      cardio::fire_and_forget(MonitorMuonNodeProcess(runtime));
    }
    runtime->reader_active = true;
    cardio::fire_and_forget(RunMuonNodeReader(runtime));
    SendMuonNodeInitializeRequest(runtime);
    if (runtime->state == MuonNodeRuntimeState::Stopping) {
      StartMuonNodeShutdownOperations(runtime);
    }
  } catch (...) {
    if (!runtime->process_started) {
      CloseMuonNodeTransport(runtime);
    }
    if (runtime->process_started && !runtime->process_exited &&
        !runtime->process_monitor_active) {
      runtime->process_monitor_active = true;
      cardio::fire_and_forget(MonitorMuonNodeProcess(runtime));
    }
    FailMuonNodeRuntime(
        runtime,
        GetMuonNodeExceptionMessage("Failed to start the Node runtime"));
  }
  runtime->startup_active = false;
  MaybeCompleteMuonNodeStop(runtime);
}

static void EnsureMuonNodeRuntimeStarted(MuonNodeRuntime* runtime) {
  if (runtime == nullptr ||
      runtime->state != MuonNodeRuntimeState::Disabled) {
    return;
  }
  runtime->state = MuonNodeRuntimeState::Starting;
  runtime->startup_active = true;
  cardio::fire_and_forget(StartMuonNodeRuntime(runtime));
}

static void SubmitMuonNodeCommand(MuonNodeRuntime* runtime,
                                  MuonNodeQueuedCommand command) {
  if (runtime == nullptr) {
    CompleteMuonNodeHostInvocation(
        nullptr, command.invocation, false, "",
        "Node runtime is unavailable");
    return;
  }
  if (runtime->state == MuonNodeRuntimeState::Failed) {
    CompleteMuonNodeHostInvocation(
        runtime, command.invocation, false, "",
        runtime->failure_message.empty()
            ? "Node runtime failed"
            : runtime->failure_message);
    return;
  }
  if (runtime->state == MuonNodeRuntimeState::Stopping ||
      runtime->state == MuonNodeRuntimeState::Stopped) {
    CompleteMuonNodeHostInvocation(
        runtime, command.invocation, false, "",
        "Node instance is being released or has been released");
    return;
  }
  if (runtime->pending_requests.size() +
          runtime->queued_commands.size() >=
      kMuonNodeMaximumPendingRequests) {
    CompleteMuonNodeHostInvocation(
        runtime, command.invocation, false, "",
        "Node IPC pending request limit was exceeded");
    return;
  }
  if (runtime->state == MuonNodeRuntimeState::Ready) {
    ExecuteMuonNodeQueuedCommand(runtime, std::move(command));
    return;
  }
  runtime->queued_commands.push_back(std::move(command));
  EnsureMuonNodeRuntimeStarted(runtime);
}

static std::shared_ptr<MuonNodeHostInvocation>
CreateMuonNodeInvocation(muon_completion_func completion,
                         MuonNodeHostResultKind result_kind) {
  auto invocation = std::make_shared<MuonNodeHostInvocation>();
  invocation->completion = completion;
  invocation->result_kind = result_kind;
  return invocation;
}

static MuonNodeRuntime* FindMuonNodeRuntime(
    const char* renderer_owner_token,
    const char* instance_id) {
  if (g_muon_node_manager == nullptr ||
      renderer_owner_token == nullptr ||
      renderer_owner_token[0] == '\0' ||
      instance_id == nullptr || instance_id[0] == '\0') {
    return nullptr;
  }
  const auto entry =
      g_muon_node_manager->runtimes.find(instance_id);
  if (entry == g_muon_node_manager->runtimes.end() ||
      entry->second->renderer_owner_token != renderer_owner_token) {
    return nullptr;
  }
  return entry->second.get();
}

/**
 * Creates one hosted Node.js runtime for a renderer context.
 *
 * @param completion Completes with an opaque runtime instance identifier after
 * the sidecar handshake and project initialization finish.
 * @param renderer_owner_token Opaque renderer owner token supplied by muon.
 */
extern "C" void muon_node_create_node(
    muon_completion_func completion,
    const char* renderer_owner_token) {
  auto invocation = CreateMuonNodeInvocation(
      completion, MuonNodeHostResultKind::String);
  auto* manager = g_muon_node_manager.get();
  if (manager == nullptr || manager->stopping) {
    CompleteMuonNodeHostInvocation(
        nullptr, invocation, false, "",
        "Node plugin is shutting down");
    return;
  }
  if (renderer_owner_token == nullptr ||
      renderer_owner_token[0] == '\0') {
    CompleteMuonNodeHostInvocation(
        nullptr, invocation, false, "",
        "Node renderer owner token is unavailable");
    return;
  }
  if (manager->next_instance_id ==
      std::numeric_limits<uint64_t>::max()) {
    CompleteMuonNodeHostInvocation(
        nullptr, invocation, false, "",
        "Node instance ids are exhausted");
    return;
  }

  const auto instance_id =
      std::to_string(manager->next_instance_id);
  manager->next_instance_id += 1;
  auto runtime = std::make_shared<MuonNodeRuntime>();
  runtime->manager = manager;
  runtime->instance_id = instance_id;
  runtime->renderer_owner_token = renderer_owner_token;
  runtime->project_root = manager->project_root;
  runtime->bridge_path = manager->bridge_path;
  runtime->executable_path = manager->executable_path;
  runtime->create_invocation = invocation;
  manager->runtimes.emplace(instance_id, runtime);
  EnsureMuonNodeRuntimeStarted(runtime.get());
}

/**
 * Imports a module into the hosted Node.js runtime.
 *
 * @param completion Completes with a JSON module descriptor.
 * @param renderer_owner_token Opaque renderer owner token supplied by muon.
 * @param instance_id Opaque runtime instance identifier.
 * @param specifier Node.js module specifier.
 */
extern "C" void muon_node_import_module(
    muon_completion_func completion,
    const char* renderer_owner_token,
    const char* instance_id,
    const char* specifier) {
  auto invocation = CreateMuonNodeInvocation(
      completion, MuonNodeHostResultKind::String);
  auto* runtime =
      FindMuonNodeRuntime(renderer_owner_token, instance_id);
  if (runtime == nullptr) {
    CompleteMuonNodeHostInvocation(
        nullptr, invocation, false, "",
        "Node instance is being released or has been released");
    return;
  }
  if (specifier == nullptr || specifier[0] == '\0') {
    CompleteMuonNodeHostInvocation(
        runtime, invocation, false, "",
        "Node module specifier must be a non-empty string");
    return;
  }
  MuonNodeQueuedCommand command;
  command.command = "importModule";
  command.first = specifier;
  command.invocation = invocation;
  SubmitMuonNodeCommand(runtime, std::move(command));
}

/**
 * Calls one function export through a descriptor module handle.
 *
 * @param completion Completes with a JSON encoded bridge value.
 * @param renderer_owner_token Opaque renderer owner token supplied by muon.
 * @param instance_id Opaque runtime instance identifier.
 * @param module_id Sidecar module handle.
 * @param export_name Function export name.
 * @param arguments_json JSON array containing encoded bridge arguments.
 * @param callback_dispatcher Renderer callback dispatcher function.
 */
extern "C" void muon_node_call(
    muon_completion_func completion,
    const char* renderer_owner_token,
    const char* instance_id,
    const char* module_id,
    const char* export_name,
    const char* arguments_json,
    muon_native_function callback_dispatcher) {
  auto invocation = CreateMuonNodeInvocation(
      completion, MuonNodeHostResultKind::String);
  auto* runtime =
      FindMuonNodeRuntime(renderer_owner_token, instance_id);
  if (runtime == nullptr) {
    CompleteMuonNodeHostInvocation(
        nullptr, invocation, false, "",
        "Node instance is being released or has been released");
    return;
  }
  if (module_id == nullptr || module_id[0] == '\0' ||
      export_name == nullptr || export_name[0] == '\0' ||
      arguments_json == nullptr) {
    CompleteMuonNodeHostInvocation(
        runtime, invocation, false, "",
        "Node call requires a module, export, and argument array");
    return;
  }

  auto handles = std::vector<std::string>{};
  auto error_message = std::string{};
  if (!CollectMuonNodeCallbackHandles(
          arguments_json, &handles, &error_message)) {
    CompleteMuonNodeHostInvocation(
        runtime, invocation, false, "", error_message);
    return;
  }
  if (!handles.empty()) {
    if (runtime == nullptr || callback_dispatcher == nullptr ||
        g_muon_node_helpers == nullptr ||
        g_muon_node_helpers->__retain_plugin_function_pointer_impl ==
            nullptr) {
      CompleteMuonNodeHostInvocation(
          runtime, invocation, false, "",
          "Node callback dispatcher is unavailable");
      return;
    }
    for (const auto& handle : handles) {
      const auto existing = runtime->callback_leases.find(handle);
      if (existing != runtime->callback_leases.end() &&
          existing->second.dispatcher != callback_dispatcher) {
        CompleteMuonNodeHostInvocation(
            runtime, invocation, false, "",
            "Node callback handle is already in use");
        return;
      }
    }
    if (!g_muon_node_helpers->retain_plugin_function_pointer(
            callback_dispatcher)) {
      CompleteMuonNodeHostInvocation(
          runtime, invocation, false, "",
          "Failed to retain the Node callback dispatcher");
      return;
    }
    invocation->callback_dispatcher = callback_dispatcher;
    invocation->callback_dispatcher_retained = true;
    invocation->callback_handles = handles;
    for (const auto& handle : handles) {
      auto& lease = runtime->callback_leases[handle];
      lease.dispatcher = callback_dispatcher;
      lease.references += 1;
    }
  }

  MuonNodeQueuedCommand command;
  command.command = "call";
  command.first = module_id;
  command.second = export_name;
  command.arguments_json = arguments_json;
  command.invocation = invocation;
  SubmitMuonNodeCommand(runtime, std::move(command));
}

/**
 * Releases a descriptor module handle in the sidecar.
 *
 * @param completion Completes after the handle is released.
 * @param renderer_owner_token Opaque renderer owner token supplied by muon.
 * @param instance_id Opaque runtime instance identifier.
 * @param module_id Sidecar module handle.
 */
extern "C" void muon_node_release_module(
    muon_completion_func completion,
    const char* renderer_owner_token,
    const char* instance_id,
    const char* module_id) {
  auto invocation = CreateMuonNodeInvocation(
      completion, MuonNodeHostResultKind::Void);
  auto* runtime =
      FindMuonNodeRuntime(renderer_owner_token, instance_id);
  if (runtime == nullptr) {
    CompleteMuonNodeHostInvocation(
        nullptr, invocation, false, "",
        "Node instance is being released or has been released");
    return;
  }
  if (module_id == nullptr || module_id[0] == '\0') {
    CompleteMuonNodeHostInvocation(
        runtime, invocation, false, "",
        "Node module handle must be a non-empty string");
    return;
  }
  MuonNodeQueuedCommand command;
  command.command = "release";
  command.first = module_id;
  command.invocation = invocation;
  SubmitMuonNodeCommand(runtime, std::move(command));
}

/**
 * Releases one hosted Node.js runtime and its sidecar process.
 *
 * @param completion Completes after the process and native handles have been
 * recovered.
 * @param renderer_owner_token Opaque renderer owner token supplied by muon.
 * @param instance_id Opaque runtime instance identifier.
 */
extern "C" void muon_node_release_node(
    muon_completion_func completion,
    const char* renderer_owner_token,
    const char* instance_id) {
  auto invocation = CreateMuonNodeInvocation(
      completion, MuonNodeHostResultKind::Void);
  auto* runtime =
      FindMuonNodeRuntime(renderer_owner_token, instance_id);
  if (runtime == nullptr) {
    CompleteMuonNodeHostInvocation(
        nullptr, invocation, false, "",
        "Node instance is being released or has been released");
    return;
  }
  if (runtime->state == MuonNodeRuntimeState::Stopped) {
    CompleteMuonNodeHostInvocation(
        runtime, invocation, true, "", "");
    return;
  }
  runtime->release_invocations.push_back(invocation);
  BeginMuonNodeShutdown(runtime);
}

static void ReleaseMuonNodeRendererContext(
    const char* renderer_owner_token) {
  auto* manager = g_muon_node_manager.get();
  if (manager == nullptr || renderer_owner_token == nullptr ||
      renderer_owner_token[0] == '\0') {
    return;
  }
  for (const auto& entry : manager->runtimes) {
    auto* runtime = entry.second.get();
    if (runtime != nullptr &&
        runtime->renderer_owner_token == renderer_owner_token) {
      BeginMuonNodeShutdown(runtime);
    }
  }
}

static void StopMuonNodePlugin(muon_plugin_stop_completion completion,
                               void* user_data) {
  if (completion == nullptr) {
    return;
  }
  auto* manager = g_muon_node_manager.get();
  if (manager == nullptr) {
    completion(user_data);
    return;
  }
  manager->stopping = true;
  manager->stop_completion = completion;
  manager->stop_user_data = user_data;
  for (const auto& entry : manager->runtimes) {
    BeginMuonNodeShutdown(entry.second.get());
  }
  MaybeCompleteMuonNodeManagerStop(manager);
}

static const muon_type_descriptor kMuonNodeVoidType = {
    MUON_TYPE_VOID,
    nullptr,
};
static const muon_type_descriptor kMuonNodeCallbackArguments[] = {
    kMuonNodeStringType,
};
static const muon_function_signature kMuonNodeCallbackSignature = {
    1,
    kMuonNodeCallbackArguments,
    &kMuonNodeStringType,
};
static const muon_type_descriptor kMuonNodeCallbackType = {
    MUON_TYPE_FUNCTION,
    &kMuonNodeCallbackSignature,
};
static const muon_type_descriptor kMuonNodeCreateArguments[] = {
    kMuonNodeStringType,
};
static const muon_type_descriptor kMuonNodeImportArguments[] = {
    kMuonNodeStringType,
    kMuonNodeStringType,
    kMuonNodeStringType,
};
static const muon_type_descriptor kMuonNodeCallArguments[] = {
    kMuonNodeStringType,
    kMuonNodeStringType,
    kMuonNodeStringType,
    kMuonNodeStringType,
    kMuonNodeStringType,
    kMuonNodeCallbackType,
};
static const muon_type_descriptor kMuonNodeReleaseModuleArguments[] = {
    kMuonNodeStringType,
    kMuonNodeStringType,
    kMuonNodeStringType,
};
static const muon_type_descriptor kMuonNodeReleaseNodeArguments[] = {
    kMuonNodeStringType,
    kMuonNodeStringType,
};
static const muon_plugin_function_metadata kMuonNodeCreateFunction = {
    "__createNode",
    reinterpret_cast<muon_native_function>(&muon_node_create_node),
    {1, kMuonNodeCreateArguments, &kMuonNodeStringType},
    "createNode",
};
static const muon_plugin_function_metadata kMuonNodeImportFunction = {
    "__importModule",
    reinterpret_cast<muon_native_function>(&muon_node_import_module),
    {3, kMuonNodeImportArguments, &kMuonNodeStringType},
    "importModule",
};
static const muon_plugin_function_metadata kMuonNodeCallFunction = {
    "__call",
    reinterpret_cast<muon_native_function>(&muon_node_call),
    {6, kMuonNodeCallArguments, &kMuonNodeStringType},
    "call",
};
static const muon_plugin_function_metadata
    kMuonNodeReleaseModuleFunction = {
        "__releaseModule",
        reinterpret_cast<muon_native_function>(
            &muon_node_release_module),
        {3, kMuonNodeReleaseModuleArguments, &kMuonNodeVoidType},
        "releaseModule",
};
static const muon_plugin_function_metadata
    kMuonNodeReleaseNodeFunction = {
        "__releaseNode",
        reinterpret_cast<muon_native_function>(
            &muon_node_release_node),
        {2, kMuonNodeReleaseNodeArguments, &kMuonNodeVoidType},
        "releaseNode",
};
static const muon_plugin_function_metadata* const kMuonNodeFunctions[] = {
    &kMuonNodeCreateFunction,
    &kMuonNodeImportFunction,
    &kMuonNodeCallFunction,
    &kMuonNodeReleaseModuleFunction,
    &kMuonNodeReleaseNodeFunction,
    nullptr,
};

static constexpr char kMuonNodeSetupScript[] = R"JS(
const createNodeBridge = namespace.__createNode;
const importModuleBridge = namespace.__importModule;
const callBridge = namespace.__call;
const releaseModuleBridge = namespace.__releaseModule;
const releaseNodeBridge = namespace.__releaseNode;
delete namespace.__createNode;
delete namespace.__importModule;
delete namespace.__call;
delete namespace.__releaseModule;
delete namespace.__releaseNode;

const signed64Minimum = -(2n ** 63n);
const signed64Maximum = 2n ** 63n - 1n;
const unsigned64Maximum = 2n ** 64n - 1n;
let nextCallbackHandle = 1;
const callbacks = new Map();

const arrayBufferByteLengthGetter = Object.getOwnPropertyDescriptor(
  ArrayBuffer.prototype,
  "byteLength"
).get;

// ArrayBuffer.prototype can be imitated, so verify the internal slot with the
// intrinsic getter instead of relying on instanceof.
const hasArrayBufferInternalSlot = (value) => {
  try {
    Reflect.apply(arrayBufferByteLengthGetter, value, []);
    return true;
  } catch {
    return false;
  }
};

const encodeBase64 = (value, isArrayBufferValue) => {
  try {
    const bytes = isArrayBufferValue
      ? new Uint8Array(value)
      : new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
    let binary = "";
    const chunkSize = 0x8000;
    for (let offset = 0; offset < bytes.length; offset += chunkSize) {
      binary += String.fromCharCode(
        ...bytes.subarray(offset, offset + chunkSize)
      );
    }
    return btoa(binary);
  } catch {
    throw new TypeError("The Node bridge value could not be inspected safely");
  }
};

const decodeBase64 = (source) => {
  const binary = atob(source);
  const bytes = new Uint8Array(binary.length);
  for (let index = 0; index < binary.length; index += 1) {
    bytes[index] = binary.charCodeAt(index);
  }
  return bytes;
};

const cloneJsonValue = (value, activeValues) => {
  if (
    value === null ||
    typeof value === "boolean" ||
    typeof value === "string"
  ) {
    return value;
  }
  if (typeof value === "number") {
    if (!Number.isFinite(value) || Object.is(value, -0)) {
      throw new TypeError("Unsupported strict JSON number");
    }
    return value;
  }
  if (typeof value !== "object") {
    throw new TypeError("Unsupported strict JSON value");
  }
  if (activeValues.has(value)) {
    throw new TypeError("Circular strict JSON value");
  }

  const isArray = Array.isArray(value);
  const prototype = Object.getPrototypeOf(value);
  if (
    (isArray && prototype !== Array.prototype) ||
    (!isArray && prototype !== Object.prototype && prototype !== null)
  ) {
    throw new TypeError("Unsupported strict JSON object");
  }

  activeValues.add(value);
  try {
    const ownKeys = Reflect.ownKeys(value);
    if (isArray) {
      const lengthDescriptor = Object.getOwnPropertyDescriptor(value, "length");
      if (
        lengthDescriptor === undefined ||
        !Object.hasOwn(lengthDescriptor, "value") ||
        lengthDescriptor.enumerable ||
        typeof lengthDescriptor.value !== "number" ||
        !Number.isInteger(lengthDescriptor.value) ||
        lengthDescriptor.value < 0 ||
        lengthDescriptor.value > 0xffffffff ||
        ownKeys.length !== lengthDescriptor.value + 1 ||
        ownKeys.some((key) => typeof key !== "string")
      ) {
        throw new TypeError("Unsupported strict JSON array");
      }

      const stringKeys = new Set(ownKeys);
      if (!stringKeys.has("length")) {
        throw new TypeError("Unsupported strict JSON array");
      }
      const result = [];
      for (let index = 0; index < lengthDescriptor.value; index += 1) {
        const key = String(index);
        if (!stringKeys.has(key)) {
          throw new TypeError("Unsupported strict JSON array");
        }
        const descriptor = Object.getOwnPropertyDescriptor(value, key);
        if (
          descriptor === undefined ||
          !descriptor.enumerable ||
          !Object.hasOwn(descriptor, "value")
        ) {
          throw new TypeError("Unsupported strict JSON array");
        }
        Object.defineProperty(result, key, {
          configurable: true,
          enumerable: true,
          value: cloneJsonValue(descriptor.value, activeValues),
          writable: true,
        });
      }
      return result;
    }

    const result = {};
    for (const key of ownKeys) {
      if (typeof key !== "string") {
        throw new TypeError("Unsupported strict JSON object");
      }
      const descriptor = Object.getOwnPropertyDescriptor(value, key);
      if (
        descriptor === undefined ||
        !descriptor.enumerable ||
        !Object.hasOwn(descriptor, "value")
      ) {
        throw new TypeError("Unsupported strict JSON object");
      }
      Object.defineProperty(result, key, {
        configurable: true,
        enumerable: true,
        value: cloneJsonValue(descriptor.value, activeValues),
        writable: true,
      });
    }
    return result;
  } finally {
    activeValues.delete(value);
  }
};

const cloneJsonAggregate = (value, errorMessage) => {
  try {
    if (value === null || typeof value !== "object") {
      throw new TypeError("Strict JSON aggregate expected");
    }
    return cloneJsonValue(value, new WeakSet());
  } catch {
    throw new TypeError(errorMessage);
  }
};

const decodeValue = (value) => {
  if (
    value === null ||
    typeof value === "boolean" ||
    typeof value === "string"
  ) {
    return value;
  }
  if (typeof value === "number") {
    if (!Number.isFinite(value) || Object.is(value, -0)) {
      throw new TypeError("Unsupported value returned by the Node runtime");
    }
    return value;
  }
  if (typeof value !== "object" || Array.isArray(value)) {
    throw new TypeError("Unsupported value returned by the Node runtime");
  }
  if (value.kind === "undefined") {
    return undefined;
  }
  if (value.kind === "i64" || value.kind === "u64") {
    return BigInt(value.value);
  }
  if (value.kind === "buffer" && typeof value.data === "string") {
    return decodeBase64(value.data);
  }
  if (value.kind === "json" && Object.hasOwn(value, "value")) {
    return cloneJsonAggregate(
      value.value,
      "Unsupported strict JSON value returned by the Node runtime"
    );
  }
  throw new TypeError("Unsupported value returned by the Node runtime");
};

const encodeValue = (
  value,
  callbackHandles,
  allowFunction,
  callbackHandlePrefix
) => {
  if (value === undefined) {
    return { kind: "undefined" };
  }
  if (
    value === null ||
    typeof value === "boolean" ||
    typeof value === "string"
  ) {
    return value;
  }
  if (typeof value === "number") {
    if (!Number.isFinite(value) || Object.is(value, -0)) {
      throw new TypeError(
        "Only finite numbers other than negative zero can cross the Node bridge"
      );
    }
    return value;
  }
  if (typeof value === "bigint") {
    if (value >= signed64Minimum && value <= signed64Maximum) {
      return { kind: "i64", value: value.toString() };
    }
    if (value >= 0n && value <= unsigned64Maximum) {
      return { kind: "u64", value: value.toString() };
    }
    throw new RangeError("BigInt is outside the supported i64/u64 range");
  }
  const isArrayBufferValue = hasArrayBufferInternalSlot(value);
  let isArrayBufferView = false;
  try {
    isArrayBufferView = ArrayBuffer.isView(value);
  } catch {
    throw new TypeError("The Node bridge value could not be inspected safely");
  }
  if (isArrayBufferValue || isArrayBufferView) {
    return {
      kind: "buffer",
      data: encodeBase64(value, isArrayBufferValue),
    };
  }
  if (typeof value === "function" && allowFunction) {
    const handle = `${callbackHandlePrefix}:renderer-${nextCallbackHandle}`;
    nextCallbackHandle += 1;
    callbacks.set(handle, value);
    callbackHandles.push(handle);
    return { kind: "function", handle };
  }
  if (value !== null && typeof value === "object") {
    return {
      kind: "json",
      value: cloneJsonAggregate(
        value,
        "Only strict JSON objects and arrays can cross the Node bridge"
      ),
    };
  }
  throw new TypeError(
    "Only strict JSON values, BigInt, buffers, and callback functions can cross the Node bridge"
  );
};

const dispatchCallback = async (payloadSource) => {
  const payload = JSON.parse(payloadSource);
  const callback = callbacks.get(payload.handle);
  if (callback === undefined) {
    throw new Error("Node callback handle has been released");
  }
  const callbackArguments = payload.arguments.map(decodeValue);
  const result = await callback(...callbackArguments);
  return JSON.stringify(encodeValue(result, [], false, ""));
};

const createModuleFacade = (instanceState, imported) => {
  if (
    imported === null ||
    typeof imported !== "object" ||
    typeof imported.moduleId !== "string" ||
    imported.descriptor === null ||
    typeof imported.descriptor !== "object" ||
    !Array.isArray(imported.descriptor.exports)
  ) {
    throw new Error("Node runtime returned an invalid module descriptor");
  }

  let released = false;
  let releaseOperation;
  const facade = Object.create(null);
  for (const exported of imported.descriptor.exports) {
    if (
      exported === null ||
      typeof exported !== "object" ||
      typeof exported.name !== "string"
    ) {
      throw new Error("Node runtime returned an invalid export descriptor");
    }
    if (exported.kind === "primitive") {
      Object.defineProperty(facade, exported.name, {
        enumerable: true,
        configurable: false,
        writable: false,
        value: decodeValue(exported.value),
      });
      continue;
    }
    if (exported.kind !== "function") {
      throw new Error("Node runtime returned an unknown export kind");
    }
    Object.defineProperty(facade, exported.name, {
      enumerable: true,
      configurable: false,
      writable: false,
      value: async (...argumentsValue) => {
        if (
          instanceState.status !== "active" ||
          released ||
          releaseOperation !== undefined
        ) {
          throw new Error(
            instanceState.status === "active"
              ? "Node module handle is being released or has been released"
              : "Node instance is being released or has been released"
          );
        }
        const callbackHandles = [];
        try {
          const encodedArguments = argumentsValue.map((value) =>
            encodeValue(
              value,
              callbackHandles,
              true,
              `${instanceState.id}:${imported.moduleId}`
            )
          );
          const resultSource = await callBridge(
            "",
            instanceState.id,
            imported.moduleId,
            exported.name,
            JSON.stringify(encodedArguments),
            dispatchCallback
          );
          return decodeValue(JSON.parse(resultSource));
        } finally {
          for (const handle of callbackHandles) {
            callbacks.delete(handle);
          }
        }
      },
    });
  }
  Object.defineProperty(facade, "$release", {
    enumerable: false,
    configurable: false,
    writable: false,
    value: async () => {
      if (released) {
        return;
      }
      if (instanceState.status !== "active") {
        await instanceState.release();
        released = true;
        return;
      }
      if (releaseOperation === undefined) {
        releaseOperation = (async () => {
          await releaseModuleBridge(
            "",
            instanceState.id,
            imported.moduleId
          );
        })();
      }
      const operation = releaseOperation;
      try {
        await operation;
        released = true;
      } catch (error) {
        if (instanceState.status !== "active") {
          await instanceState.release();
          released = true;
          return;
        }
        throw error;
      } finally {
        if (releaseOperation === operation) {
          releaseOperation = undefined;
        }
      }
    },
  });
  return Object.freeze(facade);
};

const createNodeFacade = (instanceId) => {
  const instanceState = {
    id: instanceId,
    status: "active",
    releaseOperation: undefined,
    release: undefined,
  };
  const facade = Object.create(null);
  Object.defineProperty(facade, "importModule", {
    enumerable: true,
    configurable: false,
    writable: false,
    value: async (specifier) => {
      if (instanceState.status !== "active") {
        throw new Error(
          "Node instance is being released or has been released"
        );
      }
      if (typeof specifier !== "string" || specifier.length === 0) {
        throw new TypeError("Node module specifier must be a non-empty string");
      }
      return createModuleFacade(
        instanceState,
        JSON.parse(await importModuleBridge("", instanceId, specifier))
      );
    },
  });
  const release = async () => {
    if (instanceState.status === "released") {
      return;
    }
    if (instanceState.releaseOperation === undefined) {
      instanceState.status = "releasing";
      instanceState.releaseOperation = (async () => {
        await releaseNodeBridge("", instanceId);
        instanceState.status = "released";
      })();
    }
    await instanceState.releaseOperation;
  };
  instanceState.release = release;
  Object.defineProperty(facade, "release", {
    enumerable: true,
    configurable: false,
    writable: false,
    value: release,
  });
  if (typeof Symbol.asyncDispose === "symbol") {
    Object.defineProperty(facade, Symbol.asyncDispose, {
      enumerable: false,
      configurable: false,
      writable: false,
      value: release,
    });
  }
  return Object.freeze(facade);
};

Object.defineProperty(namespace, "createNode", {
  enumerable: true,
  configurable: false,
  writable: false,
  value: async () =>
    createNodeFacade(await createNodeBridge("")),
});
)JS";

static const muon_plugin_namespace kMuonNodeNamespace = {
    "muon.node",
    kMuonNodeSetupScript,
    kMuonNodeFunctions,
};
static const muon_plugin_namespace* const kMuonNodeNamespaces[] = {
    &kMuonNodeNamespace,
    nullptr,
};
static const muon_plugin_metadata kMuonNodeMetadata = {
    kMuonNodeNamespaces,
    &StopMuonNodePlugin,
    &ReleaseMuonNodeRendererContext,
};

static bool IsMuonNodeExecutablePathAbsolute(
    const char* executable) noexcept {
  if (executable == nullptr || executable[0] == '\0') {
    return false;
  }
  try {
#if defined(_WIN32)
    return std::filesystem::path(
               ConvertMuonNodeUtf8ToWide(executable))
        .is_absolute();
#else
    return std::filesystem::path(executable).is_absolute();
#endif
  } catch (...) {
    return false;
  }
}

/**
 * Initializes the optional out-of-process Node.js plugin.
 *
 * @param context Host helpers and resolved project, bridge, and executable
 * configuration.
 * @return Static Node plugin metadata, or null when configuration is invalid.
 *
 * @remarks Initialization only records metadata and paths. Each Node.js
 * process starts when the renderer creates its corresponding Node instance.
 */
extern "C" const muon_plugin_metadata* muon_init_plugin(
    const muon_plugin_init_context* context) {
  if (context == nullptr || context->helpers == nullptr) {
    return nullptr;
  }
  const auto* project =
      muon_plugin_get_config_value(context, "project");
  const auto* bridge =
      muon_plugin_get_config_value(context, "bridge");
  const auto* executable =
      muon_plugin_get_config_value(context, "executable");
  if (project == nullptr || project[0] == '\0' ||
      bridge == nullptr || bridge[0] == '\0' ||
      !IsMuonNodeExecutablePathAbsolute(executable)) {
    return nullptr;
  }
  g_muon_node_helpers = context->helpers;
  g_muon_node_manager = std::make_unique<MuonNodeManager>();
  g_muon_node_manager->project_root = project;
  g_muon_node_manager->bridge_path = bridge;
  g_muon_node_manager->executable_path = executable;
  return &kMuonNodeMetadata;
}
