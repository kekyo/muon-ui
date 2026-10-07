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
#include <fstream>
#include <limits>
#include <map>
#include <thread>
#include <utility>
#include <vector>

enum class MuonAndroidProcessRuntimeState {
  Idle,
  Running,
  Stopping,
};

#if defined(MUON_TEST_BUILD)
enum class MuonAndroidRuntimeStartupFault {
  None,
  MissingLibrary,
  MissingEntry,
  InitFailure,
  InvalidMetadata,
  DuplicatePath,
  AllowMismatch,
};

static constexpr char kMuonAndroidFaultPluginSoname[] =
    "libmuon_test_plugin_function_lifetime.so";

static const muon_plugin_metadata* DeclineMuonAndroidTestPlugin(
    const muon_plugin_init_context*) {
  return nullptr;
}

static const muon_plugin_namespace kMuonAndroidInvalidNamespace = {
    "invalid namespace",
    nullptr,
    nullptr,
};

static const muon_plugin_namespace* const
    kMuonAndroidInvalidNamespaces[] = {
        &kMuonAndroidInvalidNamespace,
        nullptr,
};

static const muon_plugin_metadata kMuonAndroidInvalidMetadata = {
    kMuonAndroidInvalidNamespaces,
    nullptr,
    nullptr,
};

static const muon_plugin_metadata* LoadMuonAndroidInvalidTestPlugin(
    const muon_plugin_init_context*) {
  return &kMuonAndroidInvalidMetadata;
}

static void CompleteMuonAndroidDuplicateFunction(
    muon_completion_func completion) {
  completion(nullptr, nullptr);
}

static const muon_type_descriptor kMuonAndroidVoidType = {
    MUON_TYPE_VOID,
    nullptr,
};

static const muon_plugin_function_metadata
    kMuonAndroidDuplicateFunction = {
        "alphaName",
        reinterpret_cast<muon_native_function>(
            &CompleteMuonAndroidDuplicateFunction),
        {0, nullptr, &kMuonAndroidVoidType},
        nullptr,
};

static const muon_plugin_function_metadata* const
    kMuonAndroidDuplicateFunctions[] = {
        &kMuonAndroidDuplicateFunction,
        nullptr,
};

static const muon_plugin_namespace kMuonAndroidDuplicateNamespace = {
    "muon.test.alpha",
    nullptr,
    kMuonAndroidDuplicateFunctions,
};

static const muon_plugin_namespace* const
    kMuonAndroidDuplicateNamespaces[] = {
        &kMuonAndroidDuplicateNamespace,
        nullptr,
};

static const muon_plugin_metadata kMuonAndroidDuplicateMetadata = {
    kMuonAndroidDuplicateNamespaces,
    nullptr,
    nullptr,
};

static const muon_plugin_metadata* LoadMuonAndroidDuplicateTestPlugin(
    const muon_plugin_init_context*) {
  return &kMuonAndroidDuplicateMetadata;
}
#endif

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
  std::map<int, std::string> dispatcher_file_descriptors;
  uint64_t opened_library_handles = 0;
  uint64_t closed_library_handles = 0;
  std::map<void*, std::string> live_library_locators;
  std::vector<std::pair<void*, std::string>> deferred_library_handles;
  std::vector<std::string> last_closed_libraries;
#if defined(MUON_TEST_BUILD)
  MuonAndroidRuntimeStartupFault next_startup_fault =
      MuonAndroidRuntimeStartupFault::None;
  MuonAndroidRuntimeStartupFault active_startup_fault =
      MuonAndroidRuntimeStartupFault::None;
#endif
  std::map<int, MuonAndroidProcessSession> sessions;
  std::vector<MuonRpcOwner> pending_probes;
  MuonAndroidPluginCatalog plugin_catalog;
  std::unique_ptr<cardio::dispatcher_host_android_auto> dispatcher_host;
  std::shared_ptr<MuonPluginRuntime> plugin_runtime;
};

static std::string GetFileDescriptorTarget(int descriptor) {
  auto error = std::error_code{};
  const auto target = std::filesystem::read_symlink(
      std::filesystem::path{"/proc/self/fd"} /
          std::to_string(descriptor),
      error);
  return error ? std::string{} : target.string();
}

static bool IsDispatcherFileDescriptorTarget(const std::string& target) {
  return target.rfind("pipe:[", 0) == 0 ||
         target == "anon_inode:[eventfd]" ||
         target.rfind("anon_inode:[timerfd", 0) == 0;
}

static std::string GetEventFileDescriptorIdentity(int descriptor) {
  auto descriptor_info = std::ifstream{
      std::filesystem::path{"/proc/self/fdinfo"} /
      std::to_string(descriptor)};
  auto line = std::string{};
  while (std::getline(descriptor_info, line)) {
    if (line.rfind("eventfd-id:", 0) == 0) {
      return line;
    }
  }
  return {};
}

static std::string GetDispatcherFileDescriptorFingerprint(int descriptor) {
  const auto target = GetFileDescriptorTarget(descriptor);
  if (!IsDispatcherFileDescriptorTarget(target)) {
    return {};
  }
  if (target != "anon_inode:[eventfd]") {
    return target;
  }

  // An eventfd target does not contain an inode. Include its kernel identity
  // so a different eventfd that reuses the same descriptor is not a leak.
  const auto identity = GetEventFileDescriptorIdentity(descriptor);
  return identity.empty() ? std::string{} : target + '|' + identity;
}

static std::map<int, std::string> GetOpenDispatcherFileDescriptors() {
  auto result = std::map<int, std::string>{};
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
    const auto descriptor = static_cast<int>(value);
    const auto fingerprint =
        GetDispatcherFileDescriptorFingerprint(descriptor);
    if (!fingerprint.empty()) {
      result.emplace(descriptor, fingerprint);
    }
  }
  (void)::closedir(directory);
  return result;
}

#if defined(MUON_TEST_BUILD)
static std::string GetAddressMappingPermissions(uintptr_t address) {
  if (address == 0) {
    return {};
  }
  auto mappings = std::ifstream{"/proc/self/maps"};
  auto line = std::string{};
  while (std::getline(mappings, line)) {
    const auto range_end = line.find(' ');
    const auto separator = line.find('-');
    if (separator == std::string::npos || range_end == std::string::npos ||
        separator >= range_end) {
      continue;
    }
    const auto begin_text = line.substr(0, separator);
    const auto end_text = line.substr(separator + 1,
                                      range_end - separator - 1);
    char* begin_parse_end = nullptr;
    char* end_parse_end = nullptr;
    const auto begin = std::strtoull(
        begin_text.c_str(), &begin_parse_end, 16);
    const auto end = std::strtoull(
        end_text.c_str(), &end_parse_end, 16);
    if (begin_parse_end == begin_text.c_str() ||
        *begin_parse_end != '\0' || end_parse_end == end_text.c_str() ||
        *end_parse_end != '\0' || address < begin || address >= end) {
      continue;
    }
    const auto permissions_begin = line.find_first_not_of(' ', range_end);
    if (permissions_begin == std::string::npos) {
      return {};
    }
    const auto permissions_end = line.find(' ', permissions_begin);
    return line.substr(permissions_begin,
                       permissions_end - permissions_begin);
  }
  return {};
}
#endif

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
#if defined(MUON_TEST_BUILD)
  active_startup_fault = next_startup_fault;
  next_startup_fault = MuonAndroidRuntimeStartupFault::None;
#endif
  last_closed_libraries.clear();
  // Android and WebView worker threads may open unrelated package, graphics,
  // and database descriptors while this main-thread constructor runs. Limit
  // the process snapshot to the native wait descriptor kinds used by the
  // dispatcher so those concurrent opens are not attributed to cardio.
  const auto descriptors_before = GetOpenDispatcherFileDescriptors();
  try {
    dispatcher_host =
        std::make_unique<cardio::dispatcher_host_android_auto>();
  } catch (const std::exception& exception) {
    *error_message = exception.what();
    return false;
  }
  const auto descriptors_after = GetOpenDispatcherFileDescriptors();
  dispatcher_file_descriptors.clear();
  for (const auto& [descriptor, fingerprint] : descriptors_after) {
    const auto previous = descriptors_before.find(descriptor);
    if (previous == descriptors_before.end() ||
        previous->second != fingerprint) {
      dispatcher_file_descriptors.emplace(descriptor, fingerprint);
    }
  }
  created_dispatcher_hosts += 1;
  generation += 1;
  state = MuonAndroidProcessRuntimeState::Running;

  auto plugins = std::vector<MuonPluginRuntimeLoadEntry>{};
  if (!CreateMuonAndroidPluginLoadEntries(&plugins, error_message)) {
    return false;
  }
#if defined(MUON_TEST_BUILD)
  for (auto& plugin : plugins) {
    if (plugin.library_locator != kMuonAndroidFaultPluginSoname) {
      continue;
    }
    auto fault_allow = std::vector<std::string>{};
    if (active_startup_fault ==
        MuonAndroidRuntimeStartupFault::DuplicatePath) {
      fault_allow.push_back("muon.test.alpha.*");
    } else if (active_startup_fault ==
               MuonAndroidRuntimeStartupFault::AllowMismatch) {
      fault_allow.push_back("muon.test.unavailable.*");
    }
    if (!fault_allow.empty() &&
        !CreateMuonPluginPolicy(
            fault_allow, &plugin.plugin_policy, error_message)) {
      return false;
    }
    break;
  }
#endif
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
  services.open_library = [this](const std::string& locator,
                                 std::string* loader_error) -> void* {
#if defined(MUON_TEST_BUILD)
    if (active_startup_fault ==
            MuonAndroidRuntimeStartupFault::MissingLibrary &&
        locator == kMuonAndroidFaultPluginSoname) {
      if (loader_error != nullptr) {
        *loader_error = "test fault: packaged library is missing";
      }
      return nullptr;
    }
#endif
    (void)::dlerror();
    auto* handle = ::dlopen(locator.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr && loader_error != nullptr) {
      const auto* diagnostic = ::dlerror();
      *loader_error = diagnostic == nullptr ? "dlopen failed" : diagnostic;
    }
    if (handle != nullptr) {
      opened_library_handles += 1;
      live_library_locators[handle] = locator;
    }
    return handle;
  };
  services.find_symbol = [this](void* handle, const char* symbol) -> void* {
    // Release builds do not inject startup faults, but keep one loader shape
    // across variants so the production lookup path stays identical.
    (void)this;
    if (handle == nullptr || symbol == nullptr) {
      return nullptr;
    }
#if defined(MUON_TEST_BUILD)
    const auto locator = live_library_locators.find(handle);
    if (locator != live_library_locators.end() &&
        locator->second == kMuonAndroidFaultPluginSoname &&
        std::string{symbol} == "muon_init_plugin") {
      switch (active_startup_fault) {
        case MuonAndroidRuntimeStartupFault::MissingEntry:
          return nullptr;
        case MuonAndroidRuntimeStartupFault::InitFailure:
          return reinterpret_cast<void*>(&DeclineMuonAndroidTestPlugin);
        case MuonAndroidRuntimeStartupFault::InvalidMetadata:
          return reinterpret_cast<void*>(&LoadMuonAndroidInvalidTestPlugin);
        case MuonAndroidRuntimeStartupFault::DuplicatePath:
          return reinterpret_cast<void*>(&LoadMuonAndroidDuplicateTestPlugin);
        default:
          break;
      }
    }
#endif
    return ::dlsym(handle, symbol);
  };
  // A plugin Stop coroutine may leave its cardio fire_and_forget cleanup in
  // the dispatcher queue. Keep its DSO mapped until that queue is destroyed.
  services.close_library = [this](void* handle) {
    if (handle != nullptr) {
      const auto locator = live_library_locators.find(handle);
      deferred_library_handles.push_back(
          {handle,
           locator == live_library_locators.end()
               ? std::string{}
               : locator->second});
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
      state != MuonAndroidProcessRuntimeState::Running) {
    return;
  }
  stop_requested = false;
  state = MuonAndroidProcessRuntimeState::Stopping;
  const auto post_completion = [this]() {
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
  };
  if (plugin_runtime) {
    plugin_runtime->Stop(post_completion);
  } else {
    post_completion();
  }
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
  for (const auto& deferred : deferred_library_handles) {
    if (deferred.first != nullptr) {
      (void)::dlclose(deferred.first);
      closed_library_handles += 1;
      last_closed_libraries.push_back(deferred.second);
      live_library_locators.erase(deferred.first);
    }
  }
  deferred_library_handles.clear();
#if defined(MUON_TEST_BUILD)
  active_startup_fault = MuonAndroidRuntimeStartupFault::None;
#endif
  for (const auto& [descriptor, fingerprint] :
       dispatcher_file_descriptors) {
    errno = 0;
    if ((::fcntl(descriptor, F_GETFD) != -1 || errno != EBADF) &&
        GetDispatcherFileDescriptorFingerprint(descriptor) == fingerprint) {
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
    auto session_ids = std::vector<int>{};
    session_ids.reserve(sessions.size());
    for (const auto& [session_id, session] : sessions) {
      (void)session;
      session_ids.push_back(session_id);
    }
    for (const auto session_id : session_ids) {
      const auto iterator = sessions.find(session_id);
      if (iterator != sessions.end() &&
          iterator->second.callbacks.runtime_failed) {
        auto callback = iterator->second.callbacks.runtime_failed;
        callback(error_message.empty()
                     ? "Could not restart the Android native runtime"
                     : error_message);
      }
    }
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
    return;
  }

  // A new Activity can be created while asynchronous plugin Stop() callbacks
  // are still running. Its session remains registered without blocking the
  // Java main Looper and is attached only after the replacement runtime is
  // fully constructed.
  auto session_ids = std::vector<int>{};
  session_ids.reserve(sessions.size());
  for (const auto& [session_id, session] : sessions) {
    (void)session;
    session_ids.push_back(session_id);
  }
  for (const auto session_id : session_ids) {
    const auto iterator = sessions.find(session_id);
    if (iterator != sessions.end() &&
        iterator->second.callbacks.runtime_ready) {
      auto callback = iterator->second.callbacks.runtime_ready;
      callback();
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
    bool* runtime_ready,
    std::string* error_message) {
  if (owner == nullptr || runtime_ready == nullptr ||
      error_message == nullptr ||
      std::this_thread::get_id() != impl_->owner_thread ||
      !callbacks.runtime_ready || !callbacks.runtime_failed ||
      !callbacks.send_message || !callbacks.deliver_runtime_probe ||
      !callbacks.settle_runtime_probe || impl_->next_owner_id <= 0 ||
      impl_->next_owner_id == std::numeric_limits<int>::max()) {
    return false;
  }
  *runtime_ready = false;
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
  *runtime_ready =
      impl_->state == MuonAndroidProcessRuntimeState::Running &&
      impl_->plugin_runtime && impl_->plugin_runtime->IsReady();
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
  switch (impl_->state) {
    case MuonAndroidProcessRuntimeState::Idle:
      diagnostics.runtime_state = "idle";
      break;
    case MuonAndroidProcessRuntimeState::Running:
      diagnostics.runtime_state = "running";
      break;
    case MuonAndroidProcessRuntimeState::Stopping:
      diagnostics.runtime_state = "stopping";
      break;
  }
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
  diagnostics.opened_library_handles = impl_->opened_library_handles;
  diagnostics.closed_library_handles = impl_->closed_library_handles;
  diagnostics.live_library_handles = impl_->live_library_locators.size();
  diagnostics.deferred_library_handles =
      impl_->deferred_library_handles.size();
  diagnostics.last_closed_libraries = impl_->last_closed_libraries;
#if defined(MUON_TEST_BUILD)
  if (impl_->plugin_runtime) {
    const auto owner = impl_->sessions.empty()
                           ? MuonRpcOwner{}
                           : impl_->sessions.begin()->second.owner;
    const auto function_diagnostics =
        impl_->plugin_runtime->GetFunctionWrapperDiagnostics(owner);
    diagnostics.function_owner_sources =
        function_diagnostics.owner.sources;
    diagnostics.function_global_sources =
        function_diagnostics.global.sources;
    diagnostics.function_global_borrows =
        function_diagnostics.global.borrows;
    diagnostics.function_global_proxies =
        function_diagnostics.global.proxies;
    diagnostics.function_global_proxy_leases =
        function_diagnostics.global.proxy_leases;
    diagnostics.pending_renderer_function_calls =
        function_diagnostics.pending_renderer_function_calls;
    diagnostics.traffic_tasks_pending =
        function_diagnostics.traffic_tasks_pending;
    diagnostics.ffi_closures_enabled =
        function_diagnostics.ffi_closures_enabled;
    diagnostics.ffi_closure_alloc =
        function_diagnostics.ffi_closure_alloc;
    diagnostics.ffi_closure_free =
        function_diagnostics.ffi_closure_free;
    diagnostics.ffi_closure_live =
        function_diagnostics.ffi_closure_live;
    diagnostics.ffi_closure_high_water =
        function_diagnostics.ffi_closure_high_water;
    diagnostics.closure_mapping_permissions = GetAddressMappingPermissions(
        function_diagnostics.ffi_closure_executable_address);
    if (diagnostics.closure_mapping_permissions.size() >= 3) {
      diagnostics.closure_writable =
          diagnostics.closure_mapping_permissions[1] == 'w';
      diagnostics.closure_executable =
          diagnostics.closure_mapping_permissions[2] == 'x';
    }
  }
#endif
  diagnostics.owner_thread =
      std::this_thread::get_id() == impl_->owner_thread;
  return diagnostics;
}

#if defined(MUON_TEST_BUILD)
bool MuonAndroidProcessRuntimeController::SetStartupFaultForTest(
    const std::string& fault,
    std::string* error_message) {
  if (error_message == nullptr ||
      std::this_thread::get_id() != impl_->owner_thread) {
    return false;
  }
  error_message->clear();
  if (impl_->state != MuonAndroidProcessRuntimeState::Idle ||
      !impl_->sessions.empty() || impl_->dispatcher_host ||
      impl_->plugin_runtime || !impl_->live_library_locators.empty() ||
      !impl_->deferred_library_handles.empty()) {
    *error_message = "Android native runtime is not idle";
    return false;
  }
  if (fault == "missing-library") {
    impl_->next_startup_fault =
        MuonAndroidRuntimeStartupFault::MissingLibrary;
  } else if (fault == "missing-entry") {
    impl_->next_startup_fault =
        MuonAndroidRuntimeStartupFault::MissingEntry;
  } else if (fault == "init-failure") {
    impl_->next_startup_fault =
        MuonAndroidRuntimeStartupFault::InitFailure;
  } else if (fault == "invalid-metadata") {
    impl_->next_startup_fault =
        MuonAndroidRuntimeStartupFault::InvalidMetadata;
  } else if (fault == "duplicate-path") {
    impl_->next_startup_fault =
        MuonAndroidRuntimeStartupFault::DuplicatePath;
  } else if (fault == "allow-mismatch") {
    impl_->next_startup_fault =
        MuonAndroidRuntimeStartupFault::AllowMismatch;
  } else {
    *error_message = "Unknown Android runtime startup fault: " + fault;
    return false;
  }
  return true;
}
#endif
