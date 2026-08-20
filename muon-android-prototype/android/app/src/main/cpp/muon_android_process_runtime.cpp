/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "muon_android_process_runtime.h"

#include "muon_android_plugin_registry.h"
#include "muon_cardio_post.h"

#include <android/log.h>
#include <cardio.h>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <thread>
#include <utility>
#include <vector>

enum class MuonAndroidProcessRuntimeState {
  Idle,
  Running,
  Stopping,
};

struct MuonAndroidProcessSession {
  MuonRpcOwner owner;
  MuonAndroidProcessSessionCallbacks callbacks;
  bool available = true;
};

struct MuonAndroidProcessRuntimeControllerImpl {
  explicit MuonAndroidProcessRuntimeControllerImpl(
      std::function<bool()> schedule_stop_completion)
      : owner_thread(std::this_thread::get_id()),
        schedule_stop_completion(std::move(schedule_stop_completion)) {}

  bool CreateRuntime(std::string* error_message);
  void StartProbe(const MuonRpcOwner& owner);
  void DrainPendingProbes();
  void MaybeBeginStop();
  void CompleteStop();

  std::thread::id owner_thread;
  std::function<bool()> schedule_stop_completion;
  MuonAndroidProcessRuntimeState state =
      MuonAndroidProcessRuntimeState::Idle;
  int next_owner_id = 1;
  uint32_t next_probe_call_id = 1;
  uint32_t probe_function_id = 0;
  uint64_t generation = 0;
  uint64_t created_dispatcher_hosts = 0;
  uint64_t destroyed_dispatcher_hosts = 0;
  uint64_t leaked_dispatcher_file_descriptors = 0;
  uint64_t suppressed_probe_results = 0;
  size_t outstanding_probes = 0;
  bool stop_requested = false;
  bool stop_completion_posted = false;
  std::set<int> dispatcher_file_descriptors;
  std::vector<void*> deferred_library_handles;
  std::map<int, MuonAndroidProcessSession> sessions;
  std::vector<MuonRpcOwner> pending_probes;
  MuonAndroidPluginCatalog plugin_catalog;
  std::unique_ptr<cardio::dispatcher_host_android_auto> dispatcher_host;
  std::shared_ptr<MuonPluginRuntime> plugin_runtime;
};

static std::set<int> GetOpenFileDescriptors() {
  auto result = std::set<int>{};
  auto* directory = ::opendir("/proc/self/fd");
  if (directory == nullptr) {
    return result;
  }
  const auto directory_fd = ::dirfd(directory);
  while (const auto* entry = ::readdir(directory)) {
    char* end = nullptr;
    const auto value = std::strtol(entry->d_name, &end, 10);
    if (end == entry->d_name || *end != '\0' || value < 0 ||
        value > std::numeric_limits<int>::max() ||
        value == directory_fd) {
      continue;
    }
    result.insert(static_cast<int>(value));
  }
  (void)::closedir(directory);
  return result;
}

static int GetAndroidLogPriority(muon_log_level level) {
  switch (level) {
    case MUON_LOG_LEVEL_DEBUG:
      return ANDROID_LOG_DEBUG;
    case MUON_LOG_LEVEL_INFO:
      return ANDROID_LOG_INFO;
    case MUON_LOG_LEVEL_WARNING:
      return ANDROID_LOG_WARN;
    case MUON_LOG_LEVEL_ERROR:
      return ANDROID_LOG_ERROR;
    case MUON_LOG_LEVEL_FATAL:
      return ANDROID_LOG_FATAL;
    default:
      return ANDROID_LOG_DEFAULT;
  }
}

static const MuonRpcOwner& GetMessageOwner(const MuonRpcMessage& message) {
  return std::visit(
      [](const auto& value) -> const MuonRpcOwner& { return value.owner; },
      message);
}

static bool IsSameSessionOwner(const MuonRpcOwner& first,
                               const MuonRpcOwner& second) {
  return AreEqualMuonRpcOwners(first, second);
}

static MuonAndroidProcessSession* FindSession(
    MuonAndroidProcessRuntimeControllerImpl* impl,
    const MuonRpcOwner& owner) {
  if (impl == nullptr || !IsValidMuonRpcOwner(owner)) {
    return nullptr;
  }
  const auto iterator = impl->sessions.find(owner.browser_id);
  if (iterator == impl->sessions.end() ||
      !IsSameSessionOwner(iterator->second.owner, owner)) {
    return nullptr;
  }
  return &iterator->second;
}

bool MuonAndroidProcessRuntimeControllerImpl::CreateRuntime(
    std::string* error_message) {
  if (error_message == nullptr ||
      std::this_thread::get_id() != owner_thread ||
      state != MuonAndroidProcessRuntimeState::Idle) {
    return false;
  }
  error_message->clear();
  const auto descriptors_before = GetOpenFileDescriptors();
  try {
    dispatcher_host =
        std::make_unique<cardio::dispatcher_host_android_auto>();
  } catch (const std::exception& exception) {
    *error_message = exception.what();
    return false;
  }
  const auto descriptors_after = GetOpenFileDescriptors();
  dispatcher_file_descriptors.clear();
  std::set_difference(
      descriptors_after.begin(), descriptors_after.end(),
      descriptors_before.begin(), descriptors_before.end(),
      std::inserter(dispatcher_file_descriptors,
                    dispatcher_file_descriptors.end()));
  created_dispatcher_hosts += 1;
  generation += 1;
  state = MuonAndroidProcessRuntimeState::Running;

  auto plugins = std::vector<MuonPluginRuntimeLoadEntry>{};
  if (!CreateMuonAndroidPluginLoadEntries(&plugins, error_message)) {
    return false;
  }
  plugin_catalog = MuonAndroidPluginCatalog{};
  auto ordered_capability_policies =
      std::vector<std::pair<std::string, std::shared_ptr<MuonPluginPolicy>>>{};
  ordered_capability_policies.reserve(plugins.size());
  for (const auto& plugin : plugins) {
    if (plugin.plugin.empty() || !plugin.plugin_policy ||
        !plugin_catalog.capability_policies
             .emplace(plugin.plugin, plugin.plugin_policy)
             .second) {
      *error_message = "Invalid Android plugin capability catalog";
      return false;
    }
    ordered_capability_policies.push_back(
        {plugin.plugin, plugin.plugin_policy});
  }

  MuonPluginRuntimeServices services;
  services.dispatcher = dispatcher_host.get();
  services.is_owner_thread = [this]() {
    return std::this_thread::get_id() == owner_thread;
  };
  services.post_owner_task = [this](std::function<void()> task) {
    if (!task || dispatcher_host == nullptr ||
        state == MuonAndroidProcessRuntimeState::Idle) {
      return false;
    }
    muon_internal::FireAndForgetOnDispatcher(
        dispatcher_host.get(),
        [task = std::move(task)]() mutable { task(); });
    return true;
  };
  services.allocate_buffer = [](size_t size, std::string*) {
    return CreateMuonRpcOwnedBuffer(size);
  };
  services.is_owner_available = [this](const MuonRpcOwner& owner) {
    const auto* session = FindSession(this, owner);
    return session != nullptr && session->available;
  };
  services.send_message = [this](const MuonRpcMessage& message,
                                 std::string* send_error) {
    auto* session = FindSession(this, GetMessageOwner(message));
    if (session == nullptr || !session->available ||
        !session->callbacks.send_message) {
      if (send_error != nullptr) {
        *send_error = "Android WebView session is unavailable";
      }
      return false;
    }
    return session->callbacks.send_message(message, send_error);
  };
  services.emit_log = [](MuonPluginRuntimeLogSource source,
                         muon_log_level level,
                         const std::string& message) {
    const auto* tag = source == MuonPluginRuntimeLogSource::Plugin
                          ? "muon-plugin"
                          : "muon-runtime";
    (void)__android_log_write(GetAndroidLogPriority(level), tag,
                              message.c_str());
  };
  services.open_library = [](const std::string& locator,
                             std::string* loader_error) -> void* {
    (void)::dlerror();
    auto* handle = ::dlopen(locator.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr && loader_error != nullptr) {
      const auto* diagnostic = ::dlerror();
      *loader_error = diagnostic == nullptr ? "dlopen failed" : diagnostic;
    }
    return handle;
  };
  services.find_symbol = [](void* handle, const char* symbol) -> void* {
    return handle == nullptr || symbol == nullptr
               ? nullptr
               : ::dlsym(handle, symbol);
  };
  // A plugin Stop coroutine may leave its cardio fire_and_forget cleanup in
  // the dispatcher queue. Keep its DSO mapped until that queue is destroyed.
  services.close_library = [this](void* handle) {
    if (handle != nullptr) {
      deferred_library_handles.push_back(handle);
    }
  };

  plugin_runtime = std::make_shared<MuonPluginRuntime>(
      std::filesystem::path{}, std::move(plugins), std::move(services));
  if (!plugin_runtime->IsReady()) {
    *error_message = plugin_runtime->GetStartupError();
    return false;
  }
  plugin_catalog.namespaces = plugin_runtime->GetNamespaces();
  plugin_catalog.functions = plugin_runtime->GetFunctions();
  for (const auto& function : plugin_catalog.functions) {
    const auto path = CreateMuonFunctionPublicPath(function);
    for (const auto& policy : ordered_capability_policies) {
      if (policy.second->IsAllowedFunctionPath(path)) {
        plugin_catalog.capability_ids_by_function_path.emplace(
            path, policy.first);
        break;
      }
    }
    if (plugin_catalog.capability_ids_by_function_path.find(path) ==
        plugin_catalog.capability_ids_by_function_path.end()) {
      *error_message = "Android plugin function has no capability: " + path;
      return false;
    }
  }
  probe_function_id = 0;
  for (const auto& function : plugin_runtime->GetFunctions()) {
    if (CreateMuonFunctionPublicPath(function) ==
        "muon.test.cardio.dispatcherProbe") {
      probe_function_id = function.id;
      break;
    }
  }
  if (probe_function_id == 0) {
    *error_message = "Android cardio probe plugin function is unavailable";
    return false;
  }
  DrainPendingProbes();
  return true;
}

void MuonAndroidProcessRuntimeControllerImpl::StartProbe(
    const MuonRpcOwner& owner) {
  auto* session = FindSession(this, owner);
  if (session == nullptr || !session->available || !plugin_runtime ||
      state != MuonAndroidProcessRuntimeState::Running ||
      probe_function_id == 0) {
    return;
  }
  if (next_probe_call_id == 0 ||
      next_probe_call_id == std::numeric_limits<uint32_t>::max()) {
    if (session->callbacks.deliver_runtime_probe) {
      session->callbacks.deliver_runtime_probe(0);
    }
    if (session->callbacks.settle_runtime_probe) {
      session->callbacks.settle_runtime_probe(true);
    }
    return;
  }
  MuonRpcCallRequest request;
  request.owner = owner;
  request.call_id = next_probe_call_id;
  next_probe_call_id += 1;
  request.kind = MuonRpcCallKind::Plugin;
  request.function_id = probe_function_id;
  outstanding_probes += 1;
  plugin_runtime->Invoke(
      request, [this, owner](const MuonRpcCallResult& result) {
        if (outstanding_probes > 0) {
          outstanding_probes -= 1;
        }
        auto* current_session = FindSession(this, owner);
        const auto deliver = current_session != nullptr &&
                             current_session->available &&
                             current_session->callbacks.deliver_runtime_probe;
        if (deliver) {
          const auto mask =
              result.success && result.value.type.type == MUON_TYPE_U32
                  ? result.value.u32_value
                  : 0;
          current_session->callbacks.deliver_runtime_probe(mask);
        } else {
          suppressed_probe_results += 1;
        }
        current_session = FindSession(this, owner);
        if (current_session != nullptr &&
            current_session->callbacks.settle_runtime_probe) {
          current_session->callbacks.settle_runtime_probe(deliver);
        }
        MaybeBeginStop();
      });
}

void MuonAndroidProcessRuntimeControllerImpl::DrainPendingProbes() {
  if (state != MuonAndroidProcessRuntimeState::Running ||
      !plugin_runtime) {
    return;
  }
  auto probes = std::move(pending_probes);
  pending_probes.clear();
  for (const auto& owner : probes) {
    StartProbe(owner);
  }
}

void MuonAndroidProcessRuntimeControllerImpl::MaybeBeginStop() {
  if (!stop_requested || !sessions.empty() || outstanding_probes != 0 ||
      state != MuonAndroidProcessRuntimeState::Running ||
      !plugin_runtime) {
    return;
  }
  stop_requested = false;
  state = MuonAndroidProcessRuntimeState::Stopping;
  plugin_runtime->Stop([this]() {
    if (stop_completion_posted || dispatcher_host == nullptr ||
        !schedule_stop_completion) {
      return;
    }
    stop_completion_posted = true;
    if (!schedule_stop_completion()) {
      stop_completion_posted = false;
      (void)__android_log_write(
          ANDROID_LOG_ERROR, "muon-runtime",
          "Could not schedule Android runtime stop completion");
    }
  });
}

void MuonAndroidProcessRuntimeControllerImpl::CompleteStop() {
  if (state != MuonAndroidProcessRuntimeState::Stopping) {
    return;
  }
  stop_completion_posted = false;
  plugin_runtime.reset();
  dispatcher_host.reset();
  // Dispatcher work-item destructors can execute std::function managers that
  // were instantiated in a plugin DSO, so dlclose must be the final step.
  for (auto* handle : deferred_library_handles) {
    if (handle != nullptr) {
      (void)::dlclose(handle);
    }
  }
  deferred_library_handles.clear();
  for (const auto descriptor : dispatcher_file_descriptors) {
    errno = 0;
    if (::fcntl(descriptor, F_GETFD) != -1 || errno != EBADF) {
      leaked_dispatcher_file_descriptors += 1;
    }
  }
  dispatcher_file_descriptors.clear();
  destroyed_dispatcher_hosts += 1;
  probe_function_id = 0;
  plugin_catalog = MuonAndroidPluginCatalog{};
  state = MuonAndroidProcessRuntimeState::Idle;
  if (sessions.empty()) {
    pending_probes.clear();
    return;
  }
  auto error_message = std::string{};
  if (!CreateRuntime(&error_message)) {
    (void)__android_log_write(ANDROID_LOG_ERROR, "muon-runtime",
                              error_message.c_str());
    auto probes = std::move(pending_probes);
    pending_probes.clear();
    for (const auto& owner : probes) {
      auto* session = FindSession(this, owner);
      if (session != nullptr && session->callbacks.deliver_runtime_probe) {
        session->callbacks.deliver_runtime_probe(0);
      }
      session = FindSession(this, owner);
      if (session != nullptr && session->callbacks.settle_runtime_probe) {
        session->callbacks.settle_runtime_probe(session->available);
      }
    }
  }
}

MuonAndroidProcessRuntimeController::MuonAndroidProcessRuntimeController(
    std::function<bool()> schedule_stop_completion)
    : impl_(std::make_unique<MuonAndroidProcessRuntimeControllerImpl>(
          std::move(schedule_stop_completion))) {}

MuonAndroidProcessRuntimeController::~MuonAndroidProcessRuntimeController() =
    default;

bool MuonAndroidProcessRuntimeController::RegisterSession(
    MuonAndroidProcessSessionCallbacks callbacks,
    MuonRpcOwner* owner,
    std::string* error_message) {
  if (owner == nullptr || error_message == nullptr ||
      std::this_thread::get_id() != impl_->owner_thread ||
      !callbacks.send_message || !callbacks.deliver_runtime_probe ||
      !callbacks.settle_runtime_probe || impl_->next_owner_id <= 0 ||
      impl_->next_owner_id == std::numeric_limits<int>::max()) {
    return false;
  }
  error_message->clear();
  MuonRpcOwner new_owner;
  new_owner.browser_id = impl_->next_owner_id;
  new_owner.frame_id = "main";
  new_owner.context_id = impl_->next_owner_id;
  impl_->next_owner_id += 1;
  MuonAndroidProcessSession session;
  session.owner = new_owner;
  session.callbacks = std::move(callbacks);
  impl_->sessions.emplace(new_owner.browser_id, std::move(session));
  impl_->stop_requested = false;
  if (impl_->state == MuonAndroidProcessRuntimeState::Idle &&
      !impl_->CreateRuntime(error_message)) {
    impl_->sessions.erase(new_owner.browser_id);
    impl_->stop_requested = true;
    impl_->MaybeBeginStop();
    return false;
  }
  *owner = std::move(new_owner);
  return true;
}

void MuonAndroidProcessRuntimeController::ActivateSession(
    const MuonRpcOwner& owner) {
  auto* session = FindSession(impl_.get(), owner);
  if (session != nullptr) {
    session->available = true;
  }
}

bool MuonAndroidProcessRuntimeController::GetPluginCatalog(
    MuonAndroidPluginCatalog* catalog,
    std::string* error_message) const {
  if (catalog == nullptr || error_message == nullptr) {
    return false;
  }
  *catalog = MuonAndroidPluginCatalog{};
  error_message->clear();
  if (std::this_thread::get_id() != impl_->owner_thread ||
      impl_->state != MuonAndroidProcessRuntimeState::Running ||
      !impl_->plugin_runtime) {
    *error_message = "Android native plugin runtime is unavailable";
    return false;
  }
  *catalog = impl_->plugin_catalog;
  return true;
}

bool MuonAndroidProcessRuntimeController::GetCallArgumentTypes(
    const MuonRpcCallRequest& request,
    std::vector<MuonTypeMetadata>* argument_types,
    std::string* error_message) const {
  if (argument_types == nullptr || error_message == nullptr) {
    return false;
  }
  argument_types->clear();
  error_message->clear();
  if (std::this_thread::get_id() != impl_->owner_thread ||
      impl_->state != MuonAndroidProcessRuntimeState::Running ||
      !impl_->plugin_runtime) {
    *error_message = "Android native plugin runtime is unavailable";
    return false;
  }
  return impl_->plugin_runtime->GetCallArgumentTypes(
      request, argument_types, error_message);
}

void MuonAndroidProcessRuntimeController::Invoke(
    const MuonRpcCallRequest& request,
    MuonPluginRuntime::Completion completion) {
  if (!completion) {
    return;
  }
  if (std::this_thread::get_id() != impl_->owner_thread ||
      impl_->state != MuonAndroidProcessRuntimeState::Running ||
      !impl_->plugin_runtime) {
    MuonRpcCallResult result;
    result.owner = request.owner;
    result.call_id = request.call_id;
    result.success = false;
    result.error_message = "Android native plugin runtime is unavailable";
    completion(result);
    return;
  }
  impl_->plugin_runtime->Invoke(request, std::move(completion));
}

bool MuonAndroidProcessRuntimeController::GetRendererFunctionReturnType(
    const MuonRpcOwner& owner,
    uint32_t call_id,
    MuonTypeMetadata* return_type) const {
  if (return_type == nullptr ||
      std::this_thread::get_id() != impl_->owner_thread ||
      impl_->state != MuonAndroidProcessRuntimeState::Running ||
      !impl_->plugin_runtime) {
    return false;
  }
  return impl_->plugin_runtime->GetRendererFunctionReturnType(
      owner, call_id, return_type);
}

void MuonAndroidProcessRuntimeController::CompleteRendererFunctionCall(
    const MuonRpcRendererFunctionResult& result) {
  if (std::this_thread::get_id() == impl_->owner_thread &&
      impl_->state == MuonAndroidProcessRuntimeState::Running &&
      impl_->plugin_runtime) {
    impl_->plugin_runtime->CompleteRendererFunctionCall(result);
  }
}

void MuonAndroidProcessRuntimeController::ReleasePluginFunctionProxy(
    const MuonRpcPluginProxyRelease& release) {
  if (std::this_thread::get_id() == impl_->owner_thread &&
      impl_->state == MuonAndroidProcessRuntimeState::Running &&
      impl_->plugin_runtime) {
    impl_->plugin_runtime->ReleasePluginFunctionProxy(release);
  }
}

void MuonAndroidProcessRuntimeController::ReleaseSessionContext(
    const MuonRpcOwner& owner) {
  auto* session = FindSession(impl_.get(), owner);
  if (session == nullptr || !session->available) {
    return;
  }
  session->available = false;
  if (impl_->plugin_runtime) {
    impl_->plugin_runtime->CancelPlatformOperationsForOwner(owner.context_id);
    MuonRpcContextReleased release;
    release.owner = owner;
    impl_->plugin_runtime->ReleaseFunctionContext(release);
  }
}

void MuonAndroidProcessRuntimeController::UnregisterSession(
    const MuonRpcOwner& owner,
    bool preserve_runtime) {
  ReleaseSessionContext(owner);
  impl_->sessions.erase(owner.browser_id);
  if (preserve_runtime) {
    return;
  }
  if (impl_->sessions.empty()) {
    impl_->stop_requested = true;
    impl_->MaybeBeginStop();
  }
}

void MuonAndroidProcessRuntimeController::StartRuntimeProbe(
    const MuonRpcOwner& owner) {
  auto* session = FindSession(impl_.get(), owner);
  if (session == nullptr || !session->available) {
    return;
  }
  if (impl_->state == MuonAndroidProcessRuntimeState::Stopping) {
    impl_->pending_probes.push_back(owner);
    return;
  }
  if (impl_->state == MuonAndroidProcessRuntimeState::Idle) {
    auto error_message = std::string{};
    if (!impl_->CreateRuntime(&error_message)) {
      session->callbacks.deliver_runtime_probe(0);
      session = FindSession(impl_.get(), owner);
      if (session != nullptr) {
        session->callbacks.settle_runtime_probe(true);
      }
      return;
    }
  }
  impl_->StartProbe(owner);
}

void MuonAndroidProcessRuntimeController::CompletePendingStop() {
  if (std::this_thread::get_id() == impl_->owner_thread &&
      impl_->stop_completion_posted) {
    impl_->CompleteStop();
  }
}

MuonAndroidProcessRuntimeDiagnostics
MuonAndroidProcessRuntimeController::GetDiagnostics() const {
  MuonAndroidProcessRuntimeDiagnostics diagnostics;
  diagnostics.generation = impl_->generation;
  diagnostics.created_dispatcher_hosts = impl_->created_dispatcher_hosts;
  diagnostics.destroyed_dispatcher_hosts = impl_->destroyed_dispatcher_hosts;
  diagnostics.live_dispatcher_hosts = impl_->dispatcher_host ? 1 : 0;
  diagnostics.active_sessions = impl_->sessions.size();
  diagnostics.runtime_file_descriptors =
      impl_->dispatcher_file_descriptors.size();
  diagnostics.leaked_dispatcher_file_descriptors =
      impl_->leaked_dispatcher_file_descriptors;
  diagnostics.outstanding_probes = impl_->outstanding_probes;
  diagnostics.suppressed_probe_results = impl_->suppressed_probe_results;
  diagnostics.owner_thread =
      std::this_thread::get_id() == impl_->owner_thread;
  return diagnostics;
}
