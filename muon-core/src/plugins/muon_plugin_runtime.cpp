/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "plugins/muon_plugin_runtime.h"

#include "plugins/muon_traffic_type_metadata.h"

#include "muon_cardio_post.h"
#include "muon_sha256.h"

#include "plugins/muon_function_wrapper_lifecycle.h"

#include <cardio.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

static constexpr char kMuonPluginEntryPoint[] = "muon_init_plugin";
static constexpr char kMuonInternalPluginName[] = "internal";
static constexpr uint32_t kMaxMuonPluginFunctionArgs = 32;
static std::string GetMuonTrafficError(const tra_ffic_error& error);

struct MuonDynamicLibrary {
  std::string locator;
  void* handle = nullptr;
  muon_plugin_stop_func stop = nullptr;
  muon_plugin_renderer_context_released_func renderer_context_released =
      nullptr;
};

enum class MuonPluginRuntimeStopState {
  Running,
  Stopping,
  Stopped,
};

struct MuonRegisteredFunction {
  MuonFunctionMetadata metadata;
  muon_native_function function = nullptr;
  std::shared_ptr<MuonFunctionSignatureStorage> signature_storage;
  tra_ffic_function_ref function_ref = {};
};

struct MuonFunctionProxy {
  uint32_t id = 0;
  muon_native_function function = nullptr;
  MuonTypeMetadata function_type;
  std::shared_ptr<MuonFunctionSignatureStorage> signature_storage;
  tra_ffic_function_ref function_ref = {};
  struct Lease {
    std::string owner_id;
    MuonFunctionWrapperLease wrapper_lease;
  };
  std::map<std::string, Lease> leases_by_token;
};

struct MuonPluginFunctionProxyRegistration {
  uint32_t proxy_id = 0;
  std::string lease_token;
};

struct MuonFunctionOwner {
  int browser_id = 0;
  std::string frame_id;
  int renderer_context_id = 0;
};

struct MuonRendererFunctionSource {
  struct MuonPluginRuntimeImpl* impl = nullptr;
  MuonRpcOwner owner;
  std::string owner_id;
  std::string source_id;
  std::string lease_token;
  int renderer_context_id = 0;
  int function_id = 0;
  MuonTypeMetadata function_type;
  MuonFunctionWrapperLease wrapper_lease;
  muon_native_function function = nullptr;
  // tra-ffic can run the closure finalizer synchronously when creation fails
  // after it has accepted the closure state.
  bool* finalized_during_acquire = nullptr;
  size_t active_bridge_borrows = 0;
  bool bridge_retain_active = false;
  bool context_valid = true;
  bool renderer_lease_active = false;
};

static void ReleaseMuonRendererFunctionBorrow(
    MuonRendererFunctionSource* source);
static bool ReleaseMuonFunctionProxyLease(
    struct MuonPluginRuntimeImpl* impl,
    const std::string& owner_id,
    uint32_t proxy_id,
    const std::string& lease_token);

struct MuonRendererFunctionBorrow {
  MuonRendererFunctionBorrow() = default;

  explicit MuonRendererFunctionBorrow(MuonRendererFunctionSource* source)
      : source(source) {}

  ~MuonRendererFunctionBorrow() { Reset(); }

  MuonRendererFunctionBorrow(const MuonRendererFunctionBorrow&) = delete;
  MuonRendererFunctionBorrow& operator=(
      const MuonRendererFunctionBorrow&) = delete;

  MuonRendererFunctionBorrow(MuonRendererFunctionBorrow&& other) noexcept
      : source(std::exchange(other.source, nullptr)) {}

  MuonRendererFunctionBorrow& operator=(
      MuonRendererFunctionBorrow&& other) noexcept {
    if (this != &other) {
      Reset();
      source = std::exchange(other.source, nullptr);
    }
    return *this;
  }

  void Reset() {
    auto* borrowed_source = std::exchange(source, nullptr);
    if (borrowed_source != nullptr) {
      ReleaseMuonRendererFunctionBorrow(borrowed_source);
    }
  }

  MuonRendererFunctionSource* source = nullptr;
};

struct MuonFunctionRetain {
  MuonFunctionRetain() = default;

  ~MuonFunctionRetain() { Reset(); }

  MuonFunctionRetain(const MuonFunctionRetain&) = delete;
  MuonFunctionRetain& operator=(const MuonFunctionRetain&) = delete;

  MuonFunctionRetain(MuonFunctionRetain&& other) noexcept
      : function(std::exchange(other.function, nullptr)) {}

  MuonFunctionRetain& operator=(MuonFunctionRetain&& other) noexcept {
    if (this != &other) {
      Reset();
      function = std::exchange(other.function, nullptr);
    }
    return *this;
  }

  bool Acquire(muon_native_function retained_function,
               std::string* error_message) {
    Reset();
    tra_ffic_error error;
    if (!tra_ffic_function_retain(retained_function, &error)) {
      if (error_message != nullptr) {
        *error_message = GetMuonTrafficError(error);
      }
      return false;
    }
    function = retained_function;
    return true;
  }

  void Reset() {
    const auto retained_function = std::exchange(function, nullptr);
    if (retained_function != nullptr) {
      tra_ffic_error error;
      (void)tra_ffic_function_release(retained_function, &error);
    }
  }

  muon_native_function Detach() {
    return std::exchange(function, nullptr);
  }

  muon_native_function function = nullptr;
};

struct MuonFunctionWrapperLeaseGuard {
  MuonFunctionWrapperLeaseGuard(
      MuonFunctionWrapperLifecycle* lifecycle,
      const MuonFunctionWrapperLease* lease)
      : lifecycle(lifecycle), lease(lease) {}

  ~MuonFunctionWrapperLeaseGuard() {
    if (lifecycle != nullptr && lease != nullptr) {
      (void)lifecycle->Release(*lease);
    }
  }

  MuonFunctionWrapperLeaseGuard(
      const MuonFunctionWrapperLeaseGuard&) = delete;
  MuonFunctionWrapperLeaseGuard& operator=(
      const MuonFunctionWrapperLeaseGuard&) = delete;

  void Commit() {
    lifecycle = nullptr;
    lease = nullptr;
  }

  MuonFunctionWrapperLifecycle* lifecycle = nullptr;
  const MuonFunctionWrapperLease* lease = nullptr;
};

struct MuonPendingProxyTransfers {
  MuonPendingProxyTransfers() = default;

  MuonPendingProxyTransfers(struct MuonPluginRuntimeImpl* impl,
                            std::string owner_id)
      : impl(impl), owner_id(std::move(owner_id)) {}

  ~MuonPendingProxyTransfers() { Reset(); }

  MuonPendingProxyTransfers(const MuonPendingProxyTransfers&) = delete;
  MuonPendingProxyTransfers& operator=(
      const MuonPendingProxyTransfers&) = delete;

  MuonPendingProxyTransfers(MuonPendingProxyTransfers&& other) noexcept
      : impl(std::exchange(other.impl, nullptr)),
        owner_id(std::move(other.owner_id)),
        registrations(std::move(other.registrations)) {}

  MuonPendingProxyTransfers& operator=(
      MuonPendingProxyTransfers&& other) noexcept {
    if (this != &other) {
      Reset();
      impl = std::exchange(other.impl, nullptr);
      owner_id = std::move(other.owner_id);
      registrations = std::move(other.registrations);
    }
    return *this;
  }

  void Add(MuonPluginFunctionProxyRegistration registration) {
    registrations.push_back(std::move(registration));
  }

  void Commit() {
    impl = nullptr;
    owner_id.clear();
    registrations.clear();
  }

  void Reset() {
    auto* release_impl = std::exchange(impl, nullptr);
    if (release_impl != nullptr) {
      for (const auto& registration : registrations) {
        (void)ReleaseMuonFunctionProxyLease(
            release_impl, owner_id, registration.proxy_id,
            registration.lease_token);
      }
    }
    owner_id.clear();
    registrations.clear();
  }

  struct MuonPluginRuntimeImpl* impl = nullptr;
  std::string owner_id;
  std::vector<MuonPluginFunctionProxyRegistration> registrations;
};

struct MuonPendingRendererFunctionCall {
  tra_ffic_completion completion = nullptr;
  MuonRendererFunctionSource* source = nullptr;
  MuonRendererFunctionBorrow source_borrow;
  MuonPendingProxyTransfers proxy_transfers;
};

struct muon_shared_buffer {
  std::shared_ptr<MuonRpcBufferStorage> storage;
  void* data = nullptr;
  size_t size = 0;
};

struct MuonReleasedSharedBufferRange {
  uintptr_t begin = 0;
  uintptr_t end = 0;
  size_t size = 0;
};

struct MuonDecodedArguments {
  std::vector<tra_ffic_value> values;
  std::vector<std::string> string_storage;
  std::vector<std::shared_ptr<MuonRpcBufferStorage>> buffer_storage;
  std::vector<MuonRendererFunctionBorrow> renderer_function_borrows;
  std::vector<MuonFunctionRetain> function_retains;

  void ResetFunctionBorrows() {
    renderer_function_borrows.clear();
    function_retains.clear();
  }
};

struct MuonTrafficCallState {
  MuonPluginRuntime::Completion completion;
  MuonPluginRuntimeImpl* impl = nullptr;
  MuonDecodedArguments decoded_args;
  MuonTypeMetadata return_type = CreateMuonPrimitiveType(MUON_TYPE_VOID);
  MuonRpcOwner owner;
  uint32_t call_id = 0;
};

struct MuonPreparedPluginNamespace {
  const muon_plugin_namespace* source = nullptr;
  std::string plugin_namespace;
  std::vector<std::string> namespace_paths;
  std::vector<const muon_plugin_function_metadata*> allowed_functions;
  std::vector<std::string> allowed_function_names;
};

struct MuonTrafficDrainState {
  cardio::dispatcher* dispatcher = nullptr;
  struct MuonPluginRuntimeImpl* impl = nullptr;
  bool drain_posted = false;
};

struct MuonPluginRuntimeImpl {
  MuonPluginRuntimeImpl(std::filesystem::path plugin_directory,
                         std::vector<MuonPluginRuntimeLoadEntry> plugins,
                         MuonPluginRuntimeServices services);
  ~MuonPluginRuntimeImpl();

  void RequestTrafficDrain(tra_ffic_task_queue* queue);
  void DrainTrafficTasks();
  bool HasTrafficTasks();

  std::filesystem::path plugin_directory;
  std::vector<MuonPluginRuntimeLoadEntry> plugins;
  MuonPluginRuntimeServices services;
  std::vector<MuonDynamicLibrary> libraries;
  MuonPluginRuntimeStopState stop_state =
      MuonPluginRuntimeStopState::Running;
  size_t next_stop_library_index = 0;
  std::vector<MuonPluginRuntime::StopCompletion> stop_completions;
  std::vector<std::function<void(int)>> cancel_owner_operations;
  std::vector<std::unique_ptr<MuonRegisteredFunction>> registered_functions;
  std::vector<MuonNamespaceMetadata> renderer_namespaces;
  std::vector<MuonFunctionMetadata> renderer_functions;
  uint32_t next_function_id = 0;
  std::map<uint32_t, MuonRegisteredFunction*> functions_by_id;
  std::map<uint32_t, uint32_t> platform_route_ids_by_function_id;
  std::set<std::string> plugin_namespaces;
  std::set<std::string> namespace_paths;
  std::map<std::string, uint32_t> function_paths;
  std::string startup_error;
  bool platform_initialized = false;

  cardio::dispatcher* main_dispatcher = nullptr;
  std::shared_ptr<MuonTrafficDrainState> traffic_drain_state;
  std::unique_ptr<std::shared_ptr<MuonTrafficDrainState>>
      traffic_drain_state_handle;
  tra_ffic_task_queue traffic_queue = {};
  tra_ffic_side renderer_side = {};
  tra_ffic_side plugin_side = {};
  bool traffic_initialized = false;

  MuonFunctionWrapperLifecycle function_wrapper_lifecycle{
      {kMuonFunctionWrapperOwnerLimit,
       kMuonFunctionWrapperGlobalLimit}};
  std::set<MuonRendererFunctionSource*> live_renderer_function_sources;

  uint64_t next_renderer_source_lease_token = 1;
  std::map<std::string, MuonRendererFunctionSource*>
      renderer_functions_by_source;
  std::map<std::string, std::set<MuonRendererFunctionSource*>>
      renderer_sources_by_owner;

  uint32_t next_renderer_call_id = 1;
  std::map<uint32_t, MuonPendingRendererFunctionCall>
      pending_renderer_function_calls;

  uint32_t next_proxy_id = 1;
  uint64_t next_proxy_lease_token = 1;
  std::map<uint32_t, MuonFunctionProxy> proxies_by_id;
  std::map<std::string, uint32_t> proxy_ids_by_key;
  std::map<std::string, std::map<std::string, uint32_t>>
      proxy_leases_by_owner;
  std::map<std::string, MuonFunctionOwner> active_function_owners;

  std::map<muon_shared_buffer_handle,
           std::unique_ptr<muon_shared_buffer>>
      shared_buffer_allocations;
  std::vector<MuonReleasedSharedBufferRange> released_shared_buffer_ranges;
};

static MuonPluginRuntimeImpl* g_muon_runtime_helpers = nullptr;

static void LogMuonPluginRuntimeMessage(
    MuonPluginRuntimeImpl* impl,
    MuonPluginRuntimeLogSource source,
    muon_log_level level,
    const std::string& message) {
  if (impl != nullptr && impl->services.emit_log) {
    impl->services.emit_log(source, level, message);
  }
}

static std::shared_ptr<MuonFunctionSignatureStorage> CreateSharedSignature(
    const std::vector<MuonTypeMetadata>& arg_types,
    const MuonTypeMetadata& return_type) {
  return std::shared_ptr<MuonFunctionSignatureStorage>(
      CreateMuonFunctionSignatureStorage(arg_types, return_type).release());
}

static bool CreateSharedSignatureFromPluginAbi(
    const muon_function_signature* signature,
    std::shared_ptr<MuonFunctionSignatureStorage>* signature_storage,
    std::string* error_message) {
  if (signature == nullptr) {
    *error_message = "Function signature is null";
    return false;
  }
  std::vector<MuonTypeMetadata> arg_types;
  MuonTypeMetadata return_type;
  if (!ConvertMuonFunctionSignature(*signature, &arg_types, &return_type,
                                     error_message)) {
    return false;
  }
  *signature_storage = CreateSharedSignature(arg_types, return_type);
  return true;
}

static std::string GetMuonTrafficError(const tra_ffic_error& error) {
  return error.message[0] == '\0' ? "tra-ffic operation failed"
                                  : error.message;
}

static_assert(MUON_COMPLETION_ERROR_MESSAGE_CAPACITY ==
                  TRA_FFIC_ERROR_MESSAGE_CAPACITY,
              "muon completion errors must match tra-ffic errors");
static_assert(sizeof(muon_completion_error) == sizeof(tra_ffic_error),
              "muon completion error ABI must match tra-ffic error ABI");
static_assert(offsetof(muon_completion_error, message) ==
                  offsetof(tra_ffic_error, message),
              "muon completion error message offset must match tra-ffic");

static void NotifyMuonTrafficFinalization(tra_ffic_task_queue* queue,
                                           void* state) {
  auto* drain_state_handle =
      static_cast<std::shared_ptr<MuonTrafficDrainState>*>(state);
  if (drain_state_handle == nullptr || !*drain_state_handle) {
    return;
  }
  auto drain_state = *drain_state_handle;
  auto* dispatcher = drain_state->dispatcher;
  if (dispatcher == nullptr) {
    return;
  }
  muon_internal::FireAndForgetOnDispatcher(
      dispatcher, [drain_state, queue]() {
        auto* impl = drain_state->impl;
        if (impl != nullptr) {
          impl->RequestTrafficDrain(queue);
        }
      });
}

MuonPluginRuntimeImpl::MuonPluginRuntimeImpl(
    std::filesystem::path plugin_directory,
    std::vector<MuonPluginRuntimeLoadEntry> plugins,
    MuonPluginRuntimeServices services)
    : plugin_directory(std::move(plugin_directory)),
      plugins(std::move(plugins)),
      services(std::move(services)),
      main_dispatcher(cardio::unsafe_get_current_dispatcher()),
      traffic_drain_state(std::make_shared<MuonTrafficDrainState>()) {
  traffic_drain_state->dispatcher = main_dispatcher;
  traffic_drain_state->impl = this;
  traffic_drain_state_handle =
      std::make_unique<std::shared_ptr<MuonTrafficDrainState>>(
          traffic_drain_state);
  if (!this->services.is_owner_thread ||
      !this->services.post_owner_task ||
      !this->services.allocate_buffer ||
      !this->services.is_owner_available ||
      !this->services.send_message ||
      !this->services.emit_log ||
      !this->services.open_library ||
      !this->services.find_symbol ||
      !this->services.close_library) {
    startup_error = "muon plugin runtime services are incomplete";
    LogMuonPluginRuntimeMessage(
        this, MuonPluginRuntimeLogSource::Runtime,
        MUON_LOG_LEVEL_ERROR, startup_error);
    return;
  }
  if (!this->services.is_owner_thread()) {
    startup_error = "muon plugin runtime must be created on its owner thread";
    LogMuonPluginRuntimeMessage(
        this, MuonPluginRuntimeLogSource::Runtime,
        MUON_LOG_LEVEL_ERROR, startup_error);
    return;
  }
  if (main_dispatcher == nullptr) {
    startup_error = "muon main dispatcher is unavailable";
    LogMuonPluginRuntimeMessage(
        this, MuonPluginRuntimeLogSource::Runtime,
        MUON_LOG_LEVEL_ERROR, startup_error);
    return;
  }
  tra_ffic_error error;
  if (!tra_ffic_task_queue_init(
          &traffic_queue,
          NotifyMuonTrafficFinalization,
          traffic_drain_state_handle.get())) {
    LogMuonPluginRuntimeMessage(
        this, MuonPluginRuntimeLogSource::Runtime,
        MUON_LOG_LEVEL_ERROR, "Failed to initialize tra-ffic task queue");
    return;
  }
  if (!tra_ffic_side_init_pair(&renderer_side, &plugin_side,
                               tra_ffic_task_queue_schedule_callback,
                               &traffic_queue,
                               &error)) {
    LogMuonPluginRuntimeMessage(
        this, MuonPluginRuntimeLogSource::Runtime,
        MUON_LOG_LEVEL_ERROR,
        "Failed to initialize tra-ffic sides: " + GetMuonTrafficError(error));
    tra_ffic_task_queue_destroy(&traffic_queue);
    return;
  }
  traffic_initialized = true;
}

MuonPluginRuntimeImpl::~MuonPluginRuntimeImpl() {
  if (!traffic_initialized) {
    if (traffic_drain_state->impl == this) {
      traffic_drain_state->impl = nullptr;
      traffic_drain_state->drain_posted = false;
    }
    return;
  }
  tra_ffic_side_destroy(&plugin_side);
  tra_ffic_side_destroy(&renderer_side);
  DrainTrafficTasks();
  traffic_initialized = false;
  tra_ffic_task_queue_destroy(&traffic_queue);
  if (traffic_drain_state->impl == this) {
    traffic_drain_state->impl = nullptr;
    traffic_drain_state->drain_posted = false;
  }
}

void MuonPluginRuntimeImpl::RequestTrafficDrain(tra_ffic_task_queue* queue) {
  if (!services.is_owner_thread()) {
    auto drain_state = traffic_drain_state;
    (void)services.post_owner_task([drain_state, queue]() {
      auto* impl = drain_state->impl;
      if (impl != nullptr) {
        impl->RequestTrafficDrain(queue);
      }
    });
    return;
  }
  if (queue != &traffic_queue ||
      !traffic_initialized ||
      main_dispatcher == nullptr) {
    return;
  }
  if (traffic_drain_state->impl != this ||
      traffic_drain_state->drain_posted) {
    return;
  }
  traffic_drain_state->drain_posted = true;
  auto drain_state = traffic_drain_state;
  muon_internal::FireAndForgetOnDispatcher(main_dispatcher, [drain_state]() {
    auto* impl = static_cast<MuonPluginRuntimeImpl*>(nullptr);
    impl = drain_state->impl;
    if (impl != nullptr) {
      impl->DrainTrafficTasks();
      return;
    }
    drain_state->drain_posted = false;
  });
}

void MuonPluginRuntimeImpl::DrainTrafficTasks() {
  if (!services.is_owner_thread()) {
    auto drain_state = traffic_drain_state;
    (void)services.post_owner_task([drain_state]() {
      auto* impl = drain_state->impl;
      if (impl != nullptr) {
        impl->DrainTrafficTasks();
        return;
      }
      drain_state->drain_posted = false;
    });
    return;
  }
  if (!traffic_initialized) {
    return;
  }
  tra_ffic_task_drain_finalization(&traffic_queue);
  if (traffic_drain_state->impl == this) {
    traffic_drain_state->drain_posted = false;
  }
  if (HasTrafficTasks()) {
    RequestTrafficDrain(&traffic_queue);
  }
}

bool MuonPluginRuntimeImpl::HasTrafficTasks() {
  if (!traffic_initialized) {
    return false;
  }
  tra_ffic_mutex_lock(&traffic_queue.mutex);
  const auto has_tasks = traffic_queue.head != nullptr;
  tra_ffic_mutex_unlock(&traffic_queue.mutex);
  return has_tasks;
}

static void CloseMuonDynamicLibrary(MuonPluginRuntimeImpl* impl,
                                    void* handle) {
  if (impl != nullptr && handle != nullptr &&
      impl->services.close_library) {
    impl->services.close_library(handle);
  }
}

static void* OpenMuonDynamicLibrary(MuonPluginRuntimeImpl* impl,
                                    const std::string& locator,
                                    std::string* error_message) {
  if (impl == nullptr || !impl->services.open_library) {
    return nullptr;
  }
  return impl->services.open_library(locator, error_message);
}

static void* GetMuonDynamicLibrarySymbol(MuonPluginRuntimeImpl* impl,
                                         void* handle,
                                         const char* name) {
  if (impl == nullptr || handle == nullptr || name == nullptr ||
      !impl->services.find_symbol) {
    return nullptr;
  }
  return impl->services.find_symbol(handle, name);
}

static const char* GetMuonPluginLibraryExtension() {
#if defined(_WIN32)
  return ".dll";
#else
  return ".so";
#endif
}

static muon_init_plugin_func GetMuonPluginInitFunction(
    MuonPluginRuntimeImpl* impl,
    void* handle) {
  const auto address = GetMuonDynamicLibrarySymbol(
      impl, handle, kMuonPluginEntryPoint);
  return reinterpret_cast<muon_init_plugin_func>(address);
}

static muon_plugin_init_context CreateMuonPluginInitContext(
    const MuonPluginRuntimeLoadEntry& plugin,
    const muon_plugin_helpers* helpers,
    std::vector<muon_plugin_config_entry>* config_entries) {
  config_entries->clear();
  config_entries->reserve(plugin.config.size());
  for (const auto& entry : plugin.config) {
    config_entries->push_back({entry.key.c_str(), entry.value.c_str()});
  }
  return {
      helpers,
      plugin.plugin.c_str(),
      static_cast<uint32_t>(config_entries->size()),
      config_entries->empty() ? nullptr : config_entries->data(),
  };
}

static std::filesystem::path ResolveMuonPluginLibraryPath(
    const std::filesystem::path& plugin_directory,
    const std::string& plugin) {
  return plugin_directory / (plugin + GetMuonPluginLibraryExtension());
}

static void CopyMuonPluginHelperError(const std::string& source,
                                       muon_error_buffer* error) {
  if (error == nullptr || error->message == nullptr ||
      error->message_capacity == 0) {
    return;
  }

  const auto writable_length = static_cast<size_t>(error->message_capacity - 1);
  const auto copy_length = std::min(source.size(), writable_length);
  if (copy_length > 0) {
    std::memcpy(error->message, source.data(), copy_length);
  }
  error->message[copy_length] = '\0';
}

static MuonPluginRuntimeImpl* GetMuonRuntimeForHelpers() {
  return g_muon_runtime_helpers;
}

static uint8_t RegisterMuonPluginPureFunction(
    const muon_function_signature* signature,
    muon_user_function function,
    muon_native_function* out_function,
    muon_error_buffer* error) {
  CopyMuonPluginHelperError("", error);
  auto* impl = GetMuonRuntimeForHelpers();
  if (impl == nullptr ||
      !impl->traffic_initialized) {
    CopyMuonPluginHelperError("muon plugin runtime is unavailable", error);
    return 0;
  }
  std::shared_ptr<MuonFunctionSignatureStorage> signature_storage;
  std::string error_message;
  if (!CreateSharedSignatureFromPluginAbi(signature, &signature_storage,
                                         &error_message)) {
    CopyMuonPluginHelperError(error_message, error);
    return 0;
  }
  tra_ffic_error traffic_error;
  if (!tra_ffic_side_create_pure_function_impl(
          &impl->plugin_side, GetMuonFunctionSignature(signature_storage.get()),
          ConvertMuonUserFunctionToTraffic(function), out_function,
          &traffic_error)) {
    CopyMuonPluginHelperError(GetMuonTrafficError(traffic_error), error);
    return 0;
  }
  return 1;
}

static uint8_t RegisterMuonPluginClosure(
    const muon_function_signature* signature,
    muon_user_function function,
    void* state,
    muon_finalize_user_data finalize_state,
    muon_native_function* out_function,
    muon_error_buffer* error) {
  CopyMuonPluginHelperError("", error);
  auto* impl = GetMuonRuntimeForHelpers();
  if (impl == nullptr ||
      !impl->traffic_initialized) {
    CopyMuonPluginHelperError("muon plugin runtime is unavailable", error);
    return 0;
  }
  std::shared_ptr<MuonFunctionSignatureStorage> signature_storage;
  std::string error_message;
  if (!CreateSharedSignatureFromPluginAbi(signature, &signature_storage,
                                         &error_message)) {
    CopyMuonPluginHelperError(error_message, error);
    return 0;
  }
  tra_ffic_error traffic_error;
  if (!tra_ffic_side_create_closure_impl(
          &impl->plugin_side, GetMuonFunctionSignature(signature_storage.get()),
          ConvertMuonUserFunctionToTraffic(function), state,
          ConvertMuonFinalizerToTraffic(finalize_state), out_function,
          &traffic_error)) {
    CopyMuonPluginHelperError(GetMuonTrafficError(traffic_error), error);
    return 0;
  }
  return 1;
}

static uint8_t CreateMuonCompletionFunction(
    const muon_type_descriptor* return_type,
    muon_completion_callback callback,
    void* user_data,
    muon_completion_func* out_completion,
    muon_error_buffer* error) {
  CopyMuonPluginHelperError("", error);
  if (out_completion != nullptr) {
    *out_completion = nullptr;
  }
  if (out_completion == nullptr) {
    CopyMuonPluginHelperError("Completion output argument is required", error);
    return 0;
  }
  auto* impl = GetMuonRuntimeForHelpers();
  if (impl == nullptr ||
      !impl->traffic_initialized) {
    CopyMuonPluginHelperError("muon plugin runtime is unavailable", error);
    return 0;
  }
  MuonTypeMetadata return_metadata;
  std::string error_message;
  if (!ConvertMuonTypeDescriptor(return_type, true, &return_metadata,
                                  &error_message)) {
    CopyMuonPluginHelperError(error_message, error);
    return 0;
  }
  const auto return_storage = CreateMuonTypeDescriptorStorage(return_metadata);
  tra_ffic_error traffic_error;
  tra_ffic_native_function created_completion = nullptr;
  if (!tra_ffic_side_create_completion_function_impl(
          &impl->plugin_side, &return_storage.descriptor,
          ConvertMuonCompletionCallbackToTraffic(callback),
          &created_completion, user_data, &traffic_error)) {
    CopyMuonPluginHelperError(GetMuonTrafficError(traffic_error), error);
    return 0;
  }
  *out_completion =
      reinterpret_cast<muon_completion_func>(created_completion);
  return 1;
}

static uint8_t RetainMuonPluginFunction(muon_native_function function) {
  tra_ffic_error error;
  return tra_ffic_function_retain(function, &error) ? 1 : 0;
}

static void ReleaseMuonPluginFunction(muon_native_function function) {
  if (function == nullptr) {
    return;
  }
  tra_ffic_error error;
  (void)tra_ffic_function_release(function, &error);
}

static bool GetMuonBufferViewRange(const muon_buffer_view& view,
                                    uintptr_t* begin,
                                    uintptr_t* end,
                                    std::string* error_message) {
  if (begin == nullptr || end == nullptr) {
    return false;
  }
  if (view.data == nullptr) {
    if (view.size == 0) {
      *begin = 0;
      *end = 0;
      return true;
    }
    if (error_message != nullptr) {
      *error_message = "Buffer view data is null";
    }
    return false;
  }
  const auto range_begin = reinterpret_cast<uintptr_t>(view.data);
  if (view.size > std::numeric_limits<uintptr_t>::max() - range_begin) {
    if (error_message != nullptr) {
      *error_message = "Buffer view range is out of address space";
    }
    return false;
  }
  *begin = range_begin;
  *end = range_begin + view.size;
  return true;
}

static void AddReleasedMuonSharedBufferRange(MuonPluginRuntimeImpl* impl,
                                              void* data,
                                              size_t size) {
  if (impl == nullptr || data == nullptr) {
    return;
  }
  const auto begin = reinterpret_cast<uintptr_t>(data);
  if (size > std::numeric_limits<uintptr_t>::max() - begin) {
    return;
  }
  MuonReleasedSharedBufferRange range;
  range.begin = begin;
  range.end = begin + size;
  range.size = size;
  impl->released_shared_buffer_ranges.push_back(range);
}

static bool IsMuonRangeInside(uintptr_t inner_begin,
                               uintptr_t inner_end,
                               uintptr_t outer_begin,
                               uintptr_t outer_end) {
  return inner_begin >= outer_begin && inner_end >= inner_begin &&
         inner_end <= outer_end;
}

static bool MatchesReleasedMuonSharedBufferAllocation(
    MuonPluginRuntimeImpl* impl,
    const muon_buffer_view& view) {
  if (impl == nullptr || view.data == nullptr) {
    return false;
  }
  auto view_begin = uintptr_t{0};
  auto view_end = uintptr_t{0};
  if (!GetMuonBufferViewRange(view, &view_begin, &view_end, nullptr)) {
    return false;
  }
  for (const auto& range : impl->released_shared_buffer_ranges) {
    if (IsMuonRangeInside(view_begin, view_end, range.begin, range.end)) {
      return true;
    }
  }
  return false;
}

static bool TryConsumeMuonSharedBufferAllocation(
    MuonPluginRuntimeImpl* impl,
    const muon_buffer_view& view,
    MuonRpcBinary* binary,
    bool* consumed,
    std::string* error_message) {
  if (binary == nullptr || consumed == nullptr ||
      error_message == nullptr) {
    return false;
  }
  *consumed = false;
  *binary = MuonRpcBinary{};
  if (impl == nullptr || view.data == nullptr) {
    return true;
  }

  auto view_begin = uintptr_t{0};
  auto view_end = uintptr_t{0};
  if (!GetMuonBufferViewRange(view, &view_begin, &view_end, error_message)) {
    return false;
  }

  std::unique_ptr<muon_shared_buffer> allocation;
  auto binary_offset = size_t{0};
  for (auto iterator = impl->shared_buffer_allocations.begin();
       iterator != impl->shared_buffer_allocations.end(); ++iterator) {
    const auto* candidate = iterator->second.get();
    const auto candidate_begin = reinterpret_cast<uintptr_t>(candidate->data);
    const auto candidate_end = candidate_begin + candidate->size;
    if (IsMuonRangeInside(view_begin, view_end, candidate_begin,
                          candidate_end)) {
      allocation = std::move(iterator->second);
      impl->shared_buffer_allocations.erase(iterator);
      AddReleasedMuonSharedBufferRange(impl, allocation->data,
                                        allocation->size);
      binary_offset = static_cast<size_t>(view_begin - candidate_begin);
      break;
    }
  }
  if (!allocation) {
    for (const auto& range : impl->released_shared_buffer_ranges) {
      if (IsMuonRangeInside(view_begin, view_end, range.begin, range.end)) {
        *error_message = "Buffer view references a released shared buffer";
        return false;
      }
    }
  }

  if (!allocation) {
    return true;
  }
  if (!allocation->storage ||
      allocation->storage->GetSize() < allocation->size ||
      (allocation->size > 0 && allocation->storage->GetData() == nullptr)) {
    *error_message = "Shared buffer allocation is no longer valid";
    return false;
  }
  binary->storage = std::move(allocation->storage);
  binary->offset = binary_offset;
  binary->size = static_cast<size_t>(view.size);
  *consumed = true;
  return true;
}

static bool CreateMuonRuntimeBinary(
    MuonPluginRuntimeImpl* impl,
    const muon_buffer_view& view,
    MuonRpcBinary* binary,
    std::string* error_message) {
  if (impl == nullptr || binary == nullptr || error_message == nullptr) {
    return false;
  }
  *binary = MuonRpcBinary{};
  if (view.data == nullptr && view.size != 0) {
    *error_message = "Buffer view data is null";
    return false;
  }
  if constexpr (sizeof(uintptr_t) > sizeof(size_t)) {
    if (view.size > static_cast<uintptr_t>(
                        std::numeric_limits<size_t>::max())) {
      *error_message = "Buffer view is too large";
      return false;
    }
  }
  auto consumed = false;
  if (!TryConsumeMuonSharedBufferAllocation(
          impl, view, binary, &consumed, error_message)) {
    return false;
  }
  if (consumed) {
    return true;
  }
  if (MatchesReleasedMuonSharedBufferAllocation(impl, view)) {
    *error_message = "Buffer view references a released shared buffer";
    return false;
  }
  const auto size = static_cast<size_t>(view.size);
  auto storage = impl->services.allocate_buffer(size, error_message);
  if (!storage || storage->GetSize() < size ||
      (size > 0 && storage->GetData() == nullptr)) {
    if (error_message->empty()) {
      *error_message = "Failed to allocate shared buffer";
    }
    return false;
  }
  if (size > 0) {
    std::memcpy(storage->GetData(), view.data, size);
  }
  binary->storage = std::move(storage);
  binary->size = size;
  return true;
}

static bool CreateMuonRuntimeBinary(
    MuonPluginRuntimeImpl* impl,
    const tra_ffic_buffer_view& view,
    MuonRpcBinary* binary,
    std::string* error_message) {
  const auto plugin_view = muon_buffer_view{view.data, view.size};
  return CreateMuonRuntimeBinary(impl, plugin_view, binary, error_message);
}

static uint8_t AllocateMuonSharedBuffer(
    uintptr_t size,
    muon_buffer_view* out_view,
    muon_shared_buffer_handle* out_handle,
    muon_error_buffer* error) {
  CopyMuonPluginHelperError("", error);
  if (out_view != nullptr) {
    out_view->data = nullptr;
    out_view->size = 0;
  }
  if (out_handle != nullptr) {
    *out_handle = nullptr;
  }
  if (out_view == nullptr || out_handle == nullptr) {
    CopyMuonPluginHelperError("Shared buffer output arguments are required",
                               error);
    return 0;
  }
  auto* impl = GetMuonRuntimeForHelpers();
  if (impl == nullptr) {
    CopyMuonPluginHelperError("muon plugin runtime is unavailable", error);
    return 0;
  }
  if constexpr (sizeof(uintptr_t) > sizeof(size_t)) {
    if (size > static_cast<uintptr_t>(std::numeric_limits<size_t>::max())) {
      CopyMuonPluginHelperError("Shared buffer size is too large", error);
      return 0;
    }
  }
  auto allocation_error = std::string{};
  auto storage = impl->services.allocate_buffer(
      static_cast<size_t>(size), &allocation_error);
  if (!storage || storage->GetSize() < static_cast<size_t>(size) ||
      (size > 0 && storage->GetData() == nullptr)) {
    CopyMuonPluginHelperError(
        allocation_error.empty() ? "Failed to allocate shared buffer"
                                 : allocation_error,
        error);
    return 0;
  }

  auto allocation = std::make_unique<muon_shared_buffer>();
  allocation->storage = std::move(storage);
  allocation->data = allocation->storage->GetData();
  allocation->size = static_cast<size_t>(size);
  auto* handle = allocation.get();
  impl->shared_buffer_allocations[handle] = std::move(allocation);

  out_view->data = handle->data;
  out_view->size = size;
  *out_handle = handle;
  return 1;
}

static void ReleaseMuonSharedBuffer(muon_shared_buffer_handle handle) {
  if (handle == nullptr) {
    return;
  }
  auto* impl = GetMuonRuntimeForHelpers();
  if (impl == nullptr) {
    return;
  }
  const auto iterator = impl->shared_buffer_allocations.find(handle);
  if (iterator == impl->shared_buffer_allocations.end()) {
    return;
  }
  AddReleasedMuonSharedBufferRange(impl, iterator->second->data,
                                    iterator->second->size);
  impl->shared_buffer_allocations.erase(iterator);
}

static void LogMuonPluginMessage(muon_log_level level, const char* message) {
  LogMuonPluginRuntimeMessage(
      GetMuonRuntimeForHelpers(), MuonPluginRuntimeLogSource::Plugin,
      level, message == nullptr ? std::string() : std::string(message));
}

static const muon_plugin_helpers kMuonPluginHelpers = {
    RegisterMuonPluginPureFunction,
    RegisterMuonPluginClosure,
    RetainMuonPluginFunction,
    ReleaseMuonPluginFunction,
    AllocateMuonSharedBuffer,
    ReleaseMuonSharedBuffer,
    CreateMuonCompletionFunction,
    LogMuonPluginMessage,
};

static bool ValidateMuonPluginFunctionMetadata(
    const muon_plugin_function_metadata& source,
    std::string* error_message) {
  if (source.js_name == nullptr || source.native_func == nullptr) {
    *error_message = "Function name or native function is null";
    return false;
  }

  const std::string js_name(source.js_name);
  if (!IsValidMuonJsIdentifier(js_name)) {
    *error_message = "Function name is not a valid JavaScript identifier";
    return false;
  }
  if (source.filter_name != nullptr &&
      !IsValidMuonJsIdentifier(source.filter_name)) {
    *error_message = "Function filter name is not a valid JavaScript identifier";
    return false;
  }

  if (source.signature.arg_count > kMaxMuonPluginFunctionArgs) {
    *error_message = "Function has too many arguments";
    return false;
  }
  std::vector<MuonTypeMetadata> arg_types;
  MuonTypeMetadata return_type;
  return ConvertMuonFunctionSignature(source.signature, &arg_types,
                                       &return_type, error_message);
}

static std::string GetMuonPluginFunctionFilterName(
    const muon_plugin_function_metadata& source) {
  return source.filter_name == nullptr ? std::string(source.js_name)
                                       : std::string(source.filter_name);
}

static bool IsMuonPluginFunctionAllowed(
    const MuonPluginPolicy& plugin_policy,
    const std::string& function_path) {
  return plugin_policy.IsAllowedFunctionPath(function_path);
}

static bool FailMuonPluginStartup(MuonPluginRuntimeImpl* impl,
                                   const std::string& error_message) {
  if (impl != nullptr && impl->startup_error.empty()) {
    impl->startup_error = error_message;
  }
  LogMuonPluginRuntimeMessage(
      impl, MuonPluginRuntimeLogSource::Runtime,
      MUON_LOG_LEVEL_ERROR, error_message);
  return false;
}

static uint32_t AllocateMuonFunctionId(MuonPluginRuntimeImpl* impl) {
  const auto id = impl->next_function_id;
  impl->next_function_id += 1;
  return id;
}

static std::vector<std::string> CreateMuonNamespacePaths(
    const std::vector<std::string>& segments) {
  std::vector<std::string> paths;
  std::string current;
  for (const auto& segment : segments) {
    if (!current.empty()) {
      current += ".";
    }
    current += segment;
    paths.push_back(current);
  }
  return paths;
}

static bool IsMuonReservedPluginNamespacePath(
    MuonPluginRuntimeImpl* impl,
    const std::string& namespace_path) {
  if (impl == nullptr || !impl->services.platform_adapter) {
    return false;
  }
  for (const auto& reserved_namespace :
       impl->services.platform_adapter->reserved_namespaces) {
    if (namespace_path == reserved_namespace) {
      return true;
    }
  }
  return false;
}

static bool ValidateMuonPluginNamespaceRegistration(
    MuonPluginRuntimeImpl* impl,
    const std::string& plugin_namespace,
    const std::vector<std::string>& namespace_paths,
    bool allow_reserved_namespaces) {
  if (impl == nullptr) {
    return false;
  }
  if (!allow_reserved_namespaces) {
    if (IsMuonReservedPluginNamespacePath(impl, plugin_namespace)) {
      return FailMuonPluginStartup(
          impl, "Reserved plugin namespace: " + plugin_namespace);
    }
  }
  if (impl->plugin_namespaces.find(plugin_namespace) !=
      impl->plugin_namespaces.end()) {
    return FailMuonPluginStartup(
        impl, "Duplicate plugin namespace: " + plugin_namespace);
  }
  for (const auto& namespace_path : namespace_paths) {
    if (impl->function_paths.find(namespace_path) !=
        impl->function_paths.end()) {
      return FailMuonPluginStartup(
          impl, "Plugin namespace path conflicts with a function path: " +
                    namespace_path);
    }
  }
  return true;
}

static bool ValidateMuonPluginFunctionPath(
    MuonPluginRuntimeImpl* impl,
    const std::string& public_path,
    bool allow_reserved_namespaces) {
  if (impl == nullptr) {
    return false;
  }
  if (!allow_reserved_namespaces &&
      IsMuonReservedPluginNamespacePath(impl, public_path)) {
    return FailMuonPluginStartup(
        impl, "Plugin function path conflicts with a reserved namespace: " +
                  public_path);
  }
  if (impl->function_paths.find(public_path) != impl->function_paths.end()) {
    return FailMuonPluginStartup(
        impl, "Duplicate plugin function path: " + public_path);
  }
  if (impl->namespace_paths.find(public_path) != impl->namespace_paths.end()) {
    return FailMuonPluginStartup(
        impl, "Plugin function path conflicts with a namespace path: " +
                  public_path);
  }
  return true;
}

static bool PrepareMuonPluginNamespaces(
    MuonPluginRuntimeImpl* impl,
    const muon_plugin_metadata& metadata,
    const std::filesystem::path& path,
    const MuonPluginPolicy& plugin_policy,
    bool allow_reserved_namespaces,
    std::vector<MuonPreparedPluginNamespace>* prepared_namespaces) {
  if (impl == nullptr || prepared_namespaces == nullptr) {
    return false;
  }
  prepared_namespaces->clear();
  if (metadata.namespaces == nullptr) {
    return true;
  }

  std::set<std::string> local_plugin_namespaces;
  std::set<std::string> local_namespace_paths;
  std::set<std::string> local_function_paths;
  for (auto namespace_entry = metadata.namespaces;
       *namespace_entry != nullptr; ++namespace_entry) {
    const auto* source_namespace = *namespace_entry;
    if (source_namespace->plugin_namespace == nullptr) {
      return FailMuonPluginStartup(
          impl, "Plugin namespace metadata is invalid: " + path.string());
    }

    const std::string plugin_namespace(source_namespace->plugin_namespace);
    std::vector<std::string> namespace_segments;
    if (!SplitMuonPluginNamespace(plugin_namespace, &namespace_segments)) {
      return FailMuonPluginStartup(
          impl, "Plugin namespace is invalid: " + plugin_namespace);
    }
    if (namespace_segments.size() < 2) {
      return FailMuonPluginStartup(
          impl, "Plugin namespace must contain at least two segments: " +
                    plugin_namespace);
    }
    const auto namespace_paths =
        CreateMuonNamespacePaths(namespace_segments);

    std::vector<const muon_plugin_function_metadata*> allowed_functions;
    std::vector<std::string> allowed_function_names;
    if (source_namespace->functions != nullptr) {
      for (auto function_entry = source_namespace->functions;
           *function_entry != nullptr; ++function_entry) {
        const auto* source_function = *function_entry;
        std::string error_message;
        if (!ValidateMuonPluginFunctionMetadata(*source_function,
                                                &error_message)) {
          LogMuonPluginRuntimeMessage(
              impl, MuonPluginRuntimeLogSource::Runtime,
              MUON_LOG_LEVEL_WARNING,
              "Skipping plugin function from " + path.string() + ": " +
                  error_message);
          continue;
        }

        const auto filter_name =
            GetMuonPluginFunctionFilterName(*source_function);
        const auto public_path = CreateMuonFunctionPublicPath(
            plugin_namespace, filter_name);
        if (!IsMuonPluginFunctionAllowed(plugin_policy, public_path)) {
          continue;
        }
        const auto native_path = CreateMuonFunctionPublicPath(
            plugin_namespace, source_function->js_name);
        if (local_function_paths.find(public_path) !=
            local_function_paths.end()) {
          return FailMuonPluginStartup(
              impl, "Duplicate plugin function path: " + public_path);
        }
        if (native_path != public_path &&
            local_function_paths.find(native_path) !=
                local_function_paths.end()) {
          return FailMuonPluginStartup(
              impl, "Duplicate plugin function path: " + native_path);
        }
        if (!ValidateMuonPluginFunctionPath(
                impl, public_path, allow_reserved_namespaces)) {
          return false;
        }
        if (native_path != public_path &&
            !ValidateMuonPluginFunctionPath(
                impl, native_path, allow_reserved_namespaces)) {
          return false;
        }
        if (local_namespace_paths.find(public_path) !=
            local_namespace_paths.end()) {
          return FailMuonPluginStartup(
              impl, "Plugin function path conflicts with a namespace path: " +
                        public_path);
        }
        if (native_path != public_path &&
            local_namespace_paths.find(native_path) !=
                local_namespace_paths.end()) {
          return FailMuonPluginStartup(
              impl, "Plugin function path conflicts with a namespace path: " +
                        native_path);
        }
        local_function_paths.insert(public_path);
        if (native_path != public_path) {
          local_function_paths.insert(native_path);
        }
        allowed_functions.push_back(source_function);
        allowed_function_names.push_back(filter_name);
      }
    }

    if (allowed_functions.empty()) {
      continue;
    }
    if (local_plugin_namespaces.find(plugin_namespace) !=
        local_plugin_namespaces.end()) {
      return FailMuonPluginStartup(
          impl, "Duplicate plugin namespace: " + plugin_namespace);
    }
    if (!ValidateMuonPluginNamespaceRegistration(
            impl, plugin_namespace, namespace_paths,
            allow_reserved_namespaces)) {
      return false;
    }
    for (const auto& namespace_path : namespace_paths) {
      if (local_function_paths.find(namespace_path) !=
          local_function_paths.end()) {
        return FailMuonPluginStartup(
            impl, "Plugin namespace path conflicts with a function path: " +
                      namespace_path);
      }
    }

    local_plugin_namespaces.insert(plugin_namespace);
    local_namespace_paths.insert(namespace_paths.begin(),
                                 namespace_paths.end());

    MuonPreparedPluginNamespace prepared_namespace;
    prepared_namespace.source = source_namespace;
    prepared_namespace.plugin_namespace = plugin_namespace;
    prepared_namespace.namespace_paths = namespace_paths;
    prepared_namespace.allowed_functions = std::move(allowed_functions);
    prepared_namespace.allowed_function_names =
        std::move(allowed_function_names);
    prepared_namespaces->push_back(prepared_namespace);
  }
  return true;
}

static std::string CreateMuonFunctionOwnerId(
    const MuonRpcOwner& owner) {
  return CreateMuonRpcOwnerKey(owner);
}

static std::string CreateMuonFunctionSourceId(
    const std::string& owner_id,
    int function_id) {
  return owner_id + ":" + std::to_string(function_id);
}

static std::string CreateMuonFunctionProxyKey(
    muon_native_function function,
    const MuonTypeMetadata& function_type) {
  return std::to_string(reinterpret_cast<uintptr_t>(function)) + ":" +
         CreateMuonTypeCanonicalKey(function_type);
}

static bool CreateMuonTrafficFunctionRef(
    muon_native_function function,
    const std::shared_ptr<MuonFunctionSignatureStorage>& signature_storage,
    tra_ffic_function_ref* function_ref,
    std::string* error_message) {
  tra_ffic_error error;
  if (!tra_ffic_function_ref_from_raw(
          function, GetMuonFunctionSignature(signature_storage.get()),
          function_ref, &error)) {
    *error_message = GetMuonTrafficError(error);
    return false;
  }
  return true;
}

static bool IsMuonRendererInvocationOwnerAvailable(
    const MuonPluginRuntimeImpl* impl,
    const MuonRpcOwner& owner) {
  return impl != nullptr && impl->services.is_owner_available(owner);
}

static void SendMuonRendererFunctionSourceLeaseMessage(
    const MuonRendererFunctionSource& source,
    bool acquire) {
  if (!source.context_valid || !source.renderer_lease_active ||
      !IsMuonRendererInvocationOwnerAvailable(source.impl, source.owner)) {
    return;
  }
  MuonRpcRendererFunctionLease lease;
  lease.owner = source.owner;
  lease.function_id = source.function_id;
  lease.lease_token = source.lease_token;
  lease.acquire = acquire;
  auto error_message = std::string{};
  (void)source.impl->services.send_message(lease, &error_message);
}

static void ReleaseMuonRendererFunctionBridgeRetainIfIdle(
    MuonRendererFunctionSource* source) {
  if (source == nullptr || source->active_bridge_borrows != 0 ||
      !source->bridge_retain_active) {
    return;
  }

  const auto function = source->function;
  source->bridge_retain_active = false;
  tra_ffic_error error;
  (void)tra_ffic_function_release(function, &error);
}

static void ReleaseMuonRendererFunctionBorrow(
    MuonRendererFunctionSource* source) {
  if (source == nullptr || source->active_bridge_borrows == 0) {
    return;
  }
  source->active_bridge_borrows -= 1;
  ReleaseMuonRendererFunctionBridgeRetainIfIdle(source);
}

static bool AcquireMuonRendererFunctionBorrow(
    MuonRendererFunctionSource* source,
    MuonRendererFunctionBorrow* borrow,
    std::string* error_message) {
  if (source == nullptr || borrow == nullptr || error_message == nullptr) {
    return false;
  }
  if (!source->context_valid) {
    *error_message = "Renderer function context is unavailable";
    return false;
  }
  if (source->active_bridge_borrows ==
      std::numeric_limits<size_t>::max()) {
    *error_message = "Renderer function borrow limit is exhausted";
    return false;
  }
  if (!source->bridge_retain_active) {
    tra_ffic_error retain_error;
    if (!tra_ffic_function_retain(source->function, &retain_error)) {
      *error_message = GetMuonTrafficError(retain_error);
      return false;
    }
    source->bridge_retain_active = true;
  }
  source->active_bridge_borrows += 1;
  *borrow = MuonRendererFunctionBorrow(source);
  return true;
}

static void DestroyMuonRendererFunctionSource(void* state) {
  auto* source = static_cast<MuonRendererFunctionSource*>(state);
  if (source == nullptr) {
    return;
  }
  if (source->finalized_during_acquire != nullptr) {
    *source->finalized_during_acquire = true;
  }
  auto* impl = source->impl;
  if (impl != nullptr) {
    const auto source_iterator =
        impl->renderer_functions_by_source.find(source->source_id);
    if (source_iterator != impl->renderer_functions_by_source.end() &&
        source_iterator->second == source) {
      impl->renderer_functions_by_source.erase(source_iterator);
    }
    const auto owner_iterator =
        impl->renderer_sources_by_owner.find(source->owner_id);
    if (owner_iterator != impl->renderer_sources_by_owner.end()) {
      owner_iterator->second.erase(source);
      if (owner_iterator->second.empty()) {
        impl->renderer_sources_by_owner.erase(owner_iterator);
      }
    }
    impl->live_renderer_function_sources.erase(source);
    (void)impl->function_wrapper_lifecycle.Release(
        source->wrapper_lease);
  }
  SendMuonRendererFunctionSourceLeaseMessage(*source, false);
  source->renderer_lease_active = false;
  source->impl = nullptr;
  delete source;
}

static bool RegisterMuonFunctionProxyForOwner(
    MuonPluginRuntimeImpl* impl,
    const std::string& owner_id,
    muon_native_function function,
    const MuonTypeMetadata& function_type,
    MuonPluginFunctionProxyRegistration* registration,
    std::string* error_message) {
  if (impl == nullptr || function == nullptr ||
      function_type.type != MUON_TYPE_FUNCTION ||
      function_type.function_return_type.empty() || registration == nullptr ||
      error_message == nullptr) {
    return false;
  }
  registration->proxy_id = 0;
  registration->lease_token.clear();
  if (impl->active_function_owners.find(owner_id) ==
      impl->active_function_owners.end()) {
    *error_message = "Renderer function context is unavailable";
    return false;
  }

  const auto key = CreateMuonFunctionProxyKey(function, function_type);
  auto* proxy = static_cast<MuonFunctionProxy*>(nullptr);
  const auto existing = impl->proxy_ids_by_key.find(key);
  if (existing != impl->proxy_ids_by_key.end()) {
    const auto proxy_iterator = impl->proxies_by_id.find(existing->second);
    if (proxy_iterator != impl->proxies_by_id.end()) {
      proxy = &proxy_iterator->second;
    } else {
      impl->proxy_ids_by_key.erase(existing);
    }
  }

  if (impl->next_proxy_lease_token == 0 ||
      impl->next_proxy_lease_token ==
          std::numeric_limits<uint64_t>::max()) {
    *error_message = "Plugin function proxy lease ids are exhausted";
    return false;
  }
  if (proxy == nullptr &&
      (impl->next_proxy_id == 0 ||
       impl->next_proxy_id >
           static_cast<uint32_t>(std::numeric_limits<int>::max()))) {
    *error_message = "Plugin function proxy ids are exhausted";
    return false;
  }

  auto signature_storage = std::shared_ptr<MuonFunctionSignatureStorage>{};
  auto function_ref = tra_ffic_function_ref{};
  auto function_retain = MuonFunctionRetain{};
  const auto wrapper_lease = impl->function_wrapper_lifecycle.TryAcquire(
      owner_id, MuonFunctionWrapperKind::kPluginProxyLease,
      [&]() {
        if (proxy == nullptr) {
          signature_storage = CreateSharedSignature(
              function_type.function_arg_types,
              function_type.function_return_type[0]);
        }
        if (!function_retain.Acquire(function, error_message)) {
          return false;
        }
        if (proxy != nullptr) {
          return true;
        }
        if (!CreateMuonTrafficFunctionRef(
                function, signature_storage, &function_ref,
                error_message)) {
          return false;
        }
        return true;
      });
  if (!wrapper_lease.has_value()) {
    if (error_message->empty()) {
      *error_message = "Function wrapper quota exceeded";
    }
    return false;
  }
  auto wrapper_lease_guard = MuonFunctionWrapperLeaseGuard(
      &impl->function_wrapper_lifecycle, &*wrapper_lease);

  if (proxy == nullptr) {
    MuonFunctionProxy created_proxy;
    created_proxy.id = impl->next_proxy_id;
    impl->next_proxy_id += 1;
    created_proxy.function = function;
    created_proxy.function_type = function_type;
    created_proxy.signature_storage = std::move(signature_storage);
    created_proxy.function_ref = function_ref;
    const auto proxy_id = created_proxy.id;
    impl->proxies_by_id.emplace(proxy_id, std::move(created_proxy));
    impl->proxy_ids_by_key[key] = proxy_id;
    proxy = &impl->proxies_by_id.find(proxy_id)->second;
  }

  registration->proxy_id = proxy->id;
  registration->lease_token =
      std::to_string(impl->next_proxy_lease_token);
  impl->next_proxy_lease_token += 1;
  proxy->leases_by_token[registration->lease_token] = {
      owner_id,
      *wrapper_lease,
  };
  impl->proxy_leases_by_owner[owner_id][registration->lease_token] =
      proxy->id;
  (void)function_retain.Detach();
  wrapper_lease_guard.Commit();
  return true;
}

static bool ReleaseMuonFunctionProxyLease(
    MuonPluginRuntimeImpl* impl,
    const std::string& owner_id,
    uint32_t proxy_id,
    const std::string& lease_token) {
  if (impl == nullptr || lease_token.empty()) {
    return false;
  }
  const auto proxy_iterator = impl->proxies_by_id.find(proxy_id);
  if (proxy_iterator == impl->proxies_by_id.end()) {
    return false;
  }
  auto& proxy = proxy_iterator->second;
  const auto lease_iterator =
      proxy.leases_by_token.find(lease_token);
  if (lease_iterator == proxy.leases_by_token.end() ||
      lease_iterator->second.owner_id != owner_id) {
    return false;
  }

  const auto function = proxy.function;
  const auto wrapper_lease = lease_iterator->second.wrapper_lease;
  proxy.leases_by_token.erase(lease_iterator);
  const auto owner_iterator = impl->proxy_leases_by_owner.find(owner_id);
  if (owner_iterator != impl->proxy_leases_by_owner.end()) {
    const auto owner_lease_iterator =
        owner_iterator->second.find(lease_token);
    if (owner_lease_iterator != owner_iterator->second.end() &&
        owner_lease_iterator->second == proxy_id) {
      owner_iterator->second.erase(owner_lease_iterator);
    }
    if (owner_iterator->second.empty()) {
      impl->proxy_leases_by_owner.erase(owner_iterator);
    }
  }
  (void)impl->function_wrapper_lifecycle.Release(wrapper_lease);
  if (proxy.leases_by_token.empty()) {
    impl->proxy_ids_by_key.erase(
        CreateMuonFunctionProxyKey(proxy.function, proxy.function_type));
    impl->proxies_by_id.erase(proxy_iterator);
  }

  tra_ffic_error error;
  (void)tra_ffic_function_release(function, &error);
  return true;
}

static bool TryGetMuonFunctionProxyForLease(
    MuonPluginRuntimeImpl* impl,
    const std::string& owner_id,
    uint32_t proxy_id,
    const std::string& lease_token,
    MuonFunctionProxy* proxy) {
  if (impl == nullptr || lease_token.empty()) {
    return false;
  }
  const auto proxy_iterator = impl->proxies_by_id.find(proxy_id);
  if (proxy_iterator == impl->proxies_by_id.end()) {
    return false;
  }
  const auto lease_iterator =
      proxy_iterator->second.leases_by_token.find(lease_token);
  if (lease_iterator ==
          proxy_iterator->second.leases_by_token.end() ||
      lease_iterator->second.owner_id != owner_id) {
    return false;
  }
  if (proxy != nullptr) {
    *proxy = proxy_iterator->second;
  }
  return true;
}

static bool CopyMuonTrafficValueToRpcValue(
    MuonPluginRuntimeImpl* impl,
    const MuonRpcOwner& owner,
    const tra_ffic_value& source,
    const MuonTypeMetadata& expected_type,
    MuonRpcValue* target,
    std::string* error_message) {
  if (target == nullptr) {
    *error_message = "Plugin result storage is unavailable";
    return false;
  }
  auto source_type = MUON_TYPE_VOID;
  if (!ConvertTrafficValueTypeToMuon(source.kind, &source_type) ||
      source_type != expected_type.type) {
    *error_message = "Plugin returned an unexpected result type";
    return false;
  }
  target->type = expected_type;
  switch (expected_type.type) {
    case MUON_TYPE_VOID:
      return true;
    case MUON_TYPE_BOOL:
      target->bool_value = source.as.bool_value;
      return true;
    case MUON_TYPE_I8:
      target->i8_value = source.as.int8_value;
      return true;
    case MUON_TYPE_U8:
      target->u8_value = source.as.uint8_value;
      return true;
    case MUON_TYPE_I16:
      target->i16_value = source.as.int16_value;
      return true;
    case MUON_TYPE_U16:
      target->u16_value = source.as.uint16_value;
      return true;
    case MUON_TYPE_I32:
      target->i32_value = source.as.int32_value;
      return true;
    case MUON_TYPE_U32:
      target->u32_value = source.as.uint32_value;
      return true;
    case MUON_TYPE_I64:
      target->i64_value = source.as.int64_value;
      return true;
    case MUON_TYPE_U64:
      target->u64_value = source.as.uint64_value;
      return true;
    case MUON_TYPE_F32:
      if (!std::isfinite(source.as.float_value)) {
        *error_message = "Plugin returned a non-finite f32 value";
        return false;
      }
      target->f32_value = source.as.float_value;
      return true;
    case MUON_TYPE_F64:
      if (!std::isfinite(source.as.double_value)) {
        *error_message = "Plugin returned a non-finite f64 value";
        return false;
      }
      target->f64_value = source.as.double_value;
      return true;
    case MUON_TYPE_POINTER:
      target->pointer_value =
          reinterpret_cast<uintptr_t>(source.as.pointer_value);
      return true;
    case MUON_TYPE_STRING:
      if (source.as.string_value == nullptr) {
        target->is_null = true;
        return true;
      }
      target->string_value = source.as.string_value;
      return true;
    case MUON_TYPE_FUNCTION:
      if (source.as.function_value == nullptr) {
        target->is_null = true;
        return true;
      }
      {
        MuonPluginFunctionProxyRegistration registration;
        if (!RegisterMuonFunctionProxyForOwner(
                impl, CreateMuonFunctionOwnerId(owner),
                source.as.function_value, expected_type, &registration,
                error_message)) {
          return false;
        }
        target->function.kind = MuonRpcFunctionKind::PluginProxy;
        target->function.proxy_id = registration.proxy_id;
        target->function.lease_token = registration.lease_token;
        target->function.type = expected_type;
      }
      return true;
    case MUON_TYPE_BUFFER_VIEW: {
      const auto& view = source.as.buffer_view_value;
      if (view.data == nullptr && view.size != 0) {
        *error_message = "Plugin returned an invalid buffer_view";
        return false;
      }
      return CreateMuonRuntimeBinary(impl, view, &target->binary,
                                     error_message);
    }
    default:
      *error_message = "Unsupported result type";
      return false;
  }
}

static void HandleMuonTrafficCallResult(void* user_data,
                                         const tra_ffic_result* result) {
  std::unique_ptr<MuonTrafficCallState> state(
      static_cast<MuonTrafficCallState*>(user_data));
  if (!state || !state->completion) {
    return;
  }

  MuonRpcCallResult call_result;
  call_result.owner = state->owner;
  call_result.call_id = state->call_id;
  if (result == nullptr) {
    call_result.success = false;
    call_result.error_message = "muon plugin call did not produce a result";
  } else if (!result->success) {
    call_result.success = false;
    call_result.error_message = result->error_message;
  } else {
    call_result.success = CopyMuonTrafficValueToRpcValue(
        state->impl, state->owner, result->value, state->return_type,
        &call_result.value,
        &call_result.error_message);
  }

  auto completion = std::move(state->completion);
  state->decoded_args.ResetFunctionBorrows();
  completion(call_result);
}

static void CompleteMuonRendererFunctionWithError(
    tra_ffic_completion completion,
    const std::string& error_message) {
  if (completion != nullptr) {
    completion(nullptr, error_message.c_str());
  }
}

static void CompleteMuonPendingRendererFunctionCall(
    MuonPendingRendererFunctionCall* pending_call,
    uint32_t call_id,
    const void* value,
    const char* error_message) {
  if (pending_call == nullptr) {
    return;
  }
  auto* source = pending_call->source;
  if (source != nullptr && source->context_valid &&
      IsMuonRendererInvocationOwnerAvailable(source->impl, source->owner)) {
    MuonRpcRendererFunctionResultConsumed consumed;
    consumed.owner = source->owner;
    consumed.call_id = call_id;
    auto send_error = std::string{};
    (void)source->impl->services.send_message(consumed, &send_error);
  }

  const auto completion = std::exchange(pending_call->completion, nullptr);
  if (completion != nullptr) {
    completion(value, error_message);
  }
}

static bool CopyMuonTrafficArgumentForRenderer(
    MuonPluginRuntimeImpl* impl,
    const MuonRendererFunctionSource& source,
    const MuonTypeMetadata& expected_type,
    const tra_ffic_value& raw_value,
    MuonRpcValue* value,
    MuonPendingProxyTransfers* proxy_transfers,
    std::string* error_message) {
  auto raw_type = MUON_TYPE_VOID;
  if (!ConvertTrafficValueTypeToMuon(raw_value.kind, &raw_type) ||
      raw_type != expected_type.type) {
    *error_message = "Function argument type mismatch";
    return false;
  }
  value->type = expected_type;
  switch (expected_type.type) {
    case MUON_TYPE_BOOL:
      value->bool_value = raw_value.as.bool_value;
      return true;
    case MUON_TYPE_I8:
      value->i8_value = raw_value.as.int8_value;
      return true;
    case MUON_TYPE_U8:
      value->u8_value = raw_value.as.uint8_value;
      return true;
    case MUON_TYPE_I16:
      value->i16_value = raw_value.as.int16_value;
      return true;
    case MUON_TYPE_U16:
      value->u16_value = raw_value.as.uint16_value;
      return true;
    case MUON_TYPE_I32:
      value->i32_value = raw_value.as.int32_value;
      return true;
    case MUON_TYPE_U32:
      value->u32_value = raw_value.as.uint32_value;
      return true;
    case MUON_TYPE_I64:
      value->i64_value = raw_value.as.int64_value;
      return true;
    case MUON_TYPE_U64:
      value->u64_value = raw_value.as.uint64_value;
      return true;
    case MUON_TYPE_F32:
      if (!std::isfinite(raw_value.as.float_value)) {
        *error_message = "Function argument f32 is not finite";
        return false;
      }
      value->f32_value = raw_value.as.float_value;
      return true;
    case MUON_TYPE_F64:
      if (!std::isfinite(raw_value.as.double_value)) {
        *error_message = "Function argument f64 is not finite";
        return false;
      }
      value->f64_value = raw_value.as.double_value;
      return true;
    case MUON_TYPE_POINTER:
      value->pointer_value =
          reinterpret_cast<uintptr_t>(raw_value.as.pointer_value);
      return true;
    case MUON_TYPE_STRING:
      if (raw_value.as.string_value == nullptr) {
        value->is_null = true;
        return true;
      }
      value->string_value = raw_value.as.string_value;
      return true;
    case MUON_TYPE_FUNCTION:
      if (raw_value.as.function_value == nullptr) {
        value->is_null = true;
        return true;
      }
      if (proxy_transfers == nullptr) {
        *error_message = "Nested function proxy transfer is unavailable";
        return false;
      }
      {
        MuonPluginFunctionProxyRegistration registration;
        if (!RegisterMuonFunctionProxyForOwner(
                impl, source.owner_id, raw_value.as.function_value,
                expected_type, &registration, error_message)) {
          return false;
        }
        value->function.kind = MuonRpcFunctionKind::PluginProxy;
        value->function.proxy_id = registration.proxy_id;
        value->function.lease_token = registration.lease_token;
        value->function.type = expected_type;
        proxy_transfers->Add(std::move(registration));
      }
      return true;
    case MUON_TYPE_BUFFER_VIEW:
      if (raw_value.as.buffer_view_value.data == nullptr &&
          raw_value.as.buffer_view_value.size != 0) {
        *error_message = "Function argument buffer_view is invalid";
        return false;
      }
      return CreateMuonRuntimeBinary(
          impl, raw_value.as.buffer_view_value, &value->binary,
          error_message);
    case MUON_TYPE_VOID:
      *error_message = "Void function arguments are not supported";
      return false;
    default:
      *error_message = "Unsupported function argument type";
      return false;
  }
}

static void InvokeMuonRendererFunctionClosure(
    tra_ffic_completion completion,
    void* closure_state,
    const tra_ffic_value* args,
    uint32_t arg_count) {
  auto* source = static_cast<MuonRendererFunctionSource*>(closure_state);
  if (source == nullptr || source->impl == nullptr) {
    CompleteMuonRendererFunctionWithError(
        completion, "Renderer function source is unavailable");
    return;
  }
  if (!source->context_valid) {
    CompleteMuonRendererFunctionWithError(
        completion, "Renderer function context is unavailable");
    return;
  }
  if (arg_count != source->function_type.function_arg_types.size()) {
    CompleteMuonRendererFunctionWithError(
        completion, "Renderer function argument count is invalid");
    return;
  }

  MuonPendingProxyTransfers proxy_transfers(source->impl,
                                             source->owner_id);
  std::vector<MuonRpcValue> encoded_values(arg_count);
  std::string error_message;
  for (auto index = size_t{0}; index < arg_count; ++index) {
    if (!CopyMuonTrafficArgumentForRenderer(
            source->impl, *source,
            source->function_type.function_arg_types[index], args[index],
            &encoded_values[index], &proxy_transfers, &error_message)) {
      CompleteMuonRendererFunctionWithError(completion, error_message);
      return;
    }
  }

  MuonRendererFunctionBorrow source_borrow;
  if (!AcquireMuonRendererFunctionBorrow(
          source, &source_borrow, &error_message)) {
    CompleteMuonRendererFunctionWithError(completion, error_message);
    return;
  }

  const auto expects_result = completion != nullptr;
  if (source->impl->next_renderer_call_id == 0 ||
      source->impl->next_renderer_call_id >
          static_cast<uint32_t>(std::numeric_limits<int>::max())) {
    CompleteMuonRendererFunctionWithError(
        completion, "Renderer function call ids are exhausted");
    return;
  }
  auto call_id = uint32_t{0};
  call_id = source->impl->next_renderer_call_id;
  source->impl->next_renderer_call_id += 1;
  MuonPendingRendererFunctionCall pending_call;
  pending_call.completion = completion;
  pending_call.source = source;
  pending_call.source_borrow = std::move(source_borrow);
  pending_call.proxy_transfers = std::move(proxy_transfers);
  source->impl->pending_renderer_function_calls.emplace(
      call_id, std::move(pending_call));

  const auto task_posted = source->impl->services.post_owner_task(
      [impl = source->impl, call_id,
       encoded_values = std::move(encoded_values), expects_result]() {
    auto pending_iterator =
        impl->pending_renderer_function_calls.find(call_id);
    if (pending_iterator == impl->pending_renderer_function_calls.end()) {
      return;
    }
    auto* source = pending_iterator->second.source;
    if (source == nullptr || !source->context_valid ||
        !IsMuonRendererInvocationOwnerAvailable(impl, source->owner)) {
      MuonPendingRendererFunctionCall pending_call;
      pending_call = std::move(pending_iterator->second);
      impl->pending_renderer_function_calls.erase(pending_iterator);
      CompleteMuonRendererFunctionWithError(
          pending_call.completion, "Renderer frame is unavailable");
      return;
    }

    MuonRpcRendererFunctionCall call;
    call.owner = source->owner;
    call.call_id = call_id;
    call.function_id = source->function_id;
    call.expects_result = expects_result;
    call.function_type = source->function_type;
    call.arguments = std::move(encoded_values);
    auto send_error = std::string{};
    if (!impl->services.send_message(call, &send_error)) {
      MuonPendingRendererFunctionCall failed_call =
          std::move(pending_iterator->second);
      impl->pending_renderer_function_calls.erase(pending_iterator);
      CompleteMuonRendererFunctionWithError(
          failed_call.completion,
          send_error.empty() ? "Failed to send renderer function call"
                             : send_error);
      return;
    }
    pending_iterator->second.proxy_transfers.Commit();
    if (!expects_result) {
      impl->pending_renderer_function_calls.erase(pending_iterator);
    }
  });
  if (!task_posted) {
    MuonPendingRendererFunctionCall failed_call;
    const auto pending_iterator =
        source->impl->pending_renderer_function_calls.find(call_id);
    if (pending_iterator !=
        source->impl->pending_renderer_function_calls.end()) {
      failed_call = std::move(pending_iterator->second);
      source->impl->pending_renderer_function_calls.erase(pending_iterator);
    }
    CompleteMuonRendererFunctionWithError(
        failed_call.completion, "Failed to dispatch renderer function call");
  }
}

static bool GetOrCreateMuonRendererFunction(
    MuonPluginRuntimeImpl* impl,
    const MuonRpcOwner& owner,
    int function_id,
    const MuonTypeMetadata& function_type,
    muon_native_function* function,
    MuonRendererFunctionBorrow* borrow,
    std::string* error_message) {
  if (impl == nullptr || function == nullptr || borrow == nullptr ||
      error_message == nullptr) {
    return false;
  }
  if (function_type.type != MUON_TYPE_FUNCTION ||
      function_type.function_return_type.empty()) {
    *error_message = "Renderer function type is invalid";
    return false;
  }

  if (!IsValidMuonRpcOwner(owner) || function_id <= 0) {
    *error_message = "Renderer function owner is invalid";
    return false;
  }
  const auto owner_id = CreateMuonFunctionOwnerId(owner);
  impl->active_function_owners[owner_id] = {
      owner.browser_id,
      owner.frame_id,
      owner.context_id,
  };
  const auto source_id =
      CreateMuonFunctionSourceId(owner_id, function_id) + ":" +
      CreateMuonTypeCanonicalKey(function_type);
  auto existing = impl->renderer_functions_by_source.find(source_id);
  if (existing != impl->renderer_functions_by_source.end()) {
    auto* source = existing->second;
    if (source == nullptr) {
      impl->renderer_functions_by_source.erase(existing);
    } else if (!source->context_valid) {
      *error_message = "Renderer function context is unavailable";
      return false;
    } else {
      if (!source->bridge_retain_active) {
        tra_ffic_error retain_error;
        if (!tra_ffic_function_retain(source->function, &retain_error)) {
          impl->renderer_functions_by_source.erase(existing);
          source = nullptr;
        } else {
          source->bridge_retain_active = true;
        }
      }
      if (source != nullptr) {
        if (source->active_bridge_borrows ==
            std::numeric_limits<size_t>::max()) {
          *error_message = "Renderer function borrow limit is exhausted";
          return false;
        }
        source->active_bridge_borrows += 1;
        *function = source->function;
        *borrow = MuonRendererFunctionBorrow(source);
        return true;
      }
    }
  }

  if (!IsMuonRendererInvocationOwnerAvailable(impl, owner)) {
    *error_message = "Renderer function owner is unavailable";
    return false;
  }
  if (impl->next_renderer_source_lease_token == 0 ||
      impl->next_renderer_source_lease_token ==
          std::numeric_limits<uint64_t>::max()) {
    *error_message = "Renderer function source lease ids are exhausted";
    return false;
  }

  auto source = std::make_unique<MuonRendererFunctionSource>();
  source->impl = impl;
  source->owner = owner;
  source->owner_id = owner_id;
  source->source_id = source_id;
  source->renderer_context_id = owner.context_id;
  source->function_id = function_id;
  source->function_type = function_type;
  auto* created_source = source.get();

  const auto signature_storage = CreateSharedSignature(
      function_type.function_arg_types, function_type.function_return_type[0]);
  muon_native_function created_function = nullptr;
  const auto wrapper_lease = impl->function_wrapper_lifecycle.TryAcquire(
      owner_id, MuonFunctionWrapperKind::kRendererSource,
      [&]() {
        created_source->lease_token =
            std::to_string(impl->next_renderer_source_lease_token);
        impl->next_renderer_source_lease_token += 1;
        auto finalized_during_acquire = false;
        created_source->finalized_during_acquire =
            &finalized_during_acquire;
        auto* closure_state = source.release();
        auto error = tra_ffic_error{};
        if (!tra_ffic_side_create_raw_closure(
                &impl->renderer_side,
                GetMuonFunctionSignature(signature_storage.get()),
                InvokeMuonRendererFunctionClosure, closure_state,
                DestroyMuonRendererFunctionSource, &created_function,
                &error)) {
          if (!finalized_during_acquire) {
            closure_state->finalized_during_acquire = nullptr;
            delete closure_state;
          }
          *error_message = GetMuonTrafficError(error);
          return false;
        }
        closure_state->finalized_during_acquire = nullptr;
        return true;
      });
  if (!wrapper_lease.has_value()) {
    if (error_message->empty()) {
      *error_message = "Function wrapper quota exceeded";
    }
    return false;
  }
  created_source->wrapper_lease = *wrapper_lease;
  created_source->function = created_function;
  created_source->active_bridge_borrows = 1;
  created_source->bridge_retain_active = true;
  created_source->renderer_lease_active = true;
  auto* borrowed_source = created_source;
  impl->renderer_functions_by_source[source_id] = borrowed_source;
  impl->renderer_sources_by_owner[owner_id].insert(borrowed_source);
  impl->live_renderer_function_sources.insert(borrowed_source);
  SendMuonRendererFunctionSourceLeaseMessage(*borrowed_source, true);

  *function = created_function;
  *borrow = MuonRendererFunctionBorrow(borrowed_source);
  return true;
}

static bool DecodeMuonPluginArguments(
    MuonPluginRuntimeImpl* impl,
    const MuonRpcOwner& owner,
    const std::vector<MuonTypeMetadata>& arg_types,
    const std::vector<MuonRpcValue>& arguments,
    MuonDecodedArguments* decoded_args,
    std::string* error_message) {
  decoded_args->ResetFunctionBorrows();
  if (arguments.size() != arg_types.size()) {
    *error_message = "Invalid argument count";
    return false;
  }

  decoded_args->values.clear();
  decoded_args->string_storage.clear();
  decoded_args->buffer_storage.clear();
  decoded_args->values.resize(arg_types.size());
  decoded_args->string_storage.reserve(arg_types.size());
  decoded_args->buffer_storage.reserve(arg_types.size());
  decoded_args->renderer_function_borrows.reserve(arg_types.size());
  decoded_args->function_retains.reserve(arg_types.size());
  for (auto index = size_t{0}; index < arg_types.size(); ++index) {
    const auto& expected_type = arg_types[index];
    const auto& source = arguments[index];
    auto& target = decoded_args->values[index];
    if (!AreEqualMuonTypes(source.type, expected_type) ||
        !ConvertMuonValueTypeToTraffic(expected_type.type, &target.kind)) {
      *error_message = "Argument type mismatch";
      return false;
    }
    switch (expected_type.type) {
      case MUON_TYPE_BOOL:
        target = tra_ffic_value_bool(source.bool_value);
        break;
      case MUON_TYPE_I8:
        target = tra_ffic_value_int8(source.i8_value);
        break;
      case MUON_TYPE_U8:
        target = tra_ffic_value_uint8(source.u8_value);
        break;
      case MUON_TYPE_I16:
        target = tra_ffic_value_int16(source.i16_value);
        break;
      case MUON_TYPE_U16:
        target = tra_ffic_value_uint16(source.u16_value);
        break;
      case MUON_TYPE_I32:
        target = tra_ffic_value_int32(source.i32_value);
        break;
      case MUON_TYPE_U32:
        target = tra_ffic_value_uint32(source.u32_value);
        break;
      case MUON_TYPE_I64:
        target = tra_ffic_value_int64(source.i64_value);
        break;
      case MUON_TYPE_U64:
        target = tra_ffic_value_uint64(source.u64_value);
        break;
      case MUON_TYPE_F32:
        if (!std::isfinite(source.f32_value)) {
          *error_message = "Invalid f32 argument";
          return false;
        }
        target = tra_ffic_value_float(source.f32_value);
        break;
      case MUON_TYPE_F64:
        if (!std::isfinite(source.f64_value)) {
          *error_message = "Invalid f64 argument";
          return false;
        }
        target = tra_ffic_value_double(source.f64_value);
        break;
      case MUON_TYPE_POINTER:
        target = tra_ffic_value_pointer(
            reinterpret_cast<void*>(source.pointer_value));
        break;
      case MUON_TYPE_STRING:
        if (source.is_null) {
          target = tra_ffic_value_string(nullptr);
          break;
        }
        decoded_args->string_storage.push_back(source.string_value);
        target = tra_ffic_value_string(
            decoded_args->string_storage.back().c_str());
        break;
      case MUON_TYPE_BUFFER_VIEW: {
        if (!IsValidMuonRpcBinary(source.binary)) {
          *error_message = "Invalid buffer_view argument";
          return false;
        }
        target = tra_ffic_value_buffer_view(
            GetMuonRpcBinaryData(source.binary),
            static_cast<uintptr_t>(source.binary.size));
        decoded_args->buffer_storage.push_back(source.binary.storage);
        break;
      }
      case MUON_TYPE_FUNCTION: {
        if (source.is_null) {
          target = tra_ffic_value_function(nullptr);
          break;
        }
        if (!AreEqualMuonTypes(source.function.type, expected_type)) {
          *error_message = "Invalid function argument";
          return false;
        }
        muon_native_function function = nullptr;
        if (source.function.kind == MuonRpcFunctionKind::PluginProxy) {
          if (source.function.proxy_id == 0 ||
              source.function.lease_token.empty()) {
            *error_message = "Invalid plugin function proxy";
            return false;
          }
          const auto owner_id = CreateMuonFunctionOwnerId(owner);
          MuonFunctionProxy proxy;
          if (!TryGetMuonFunctionProxyForLease(
                  impl, owner_id, source.function.proxy_id,
                  source.function.lease_token, &proxy)) {
            *error_message = "Unknown plugin function proxy";
            return false;
          }
          if (!AreEqualMuonTypes(proxy.function_type, expected_type)) {
            *error_message = "Plugin function proxy type mismatch";
            return false;
          }
          MuonFunctionRetain function_retain;
          if (!function_retain.Acquire(proxy.function, error_message)) {
            return false;
          }
          function = proxy.function;
          decoded_args->function_retains.push_back(
              std::move(function_retain));
        } else {
          if (source.function.renderer_context_id != owner.context_id ||
              source.function.function_id <= 0) {
            *error_message = "Invalid function argument";
            return false;
          }
          MuonRendererFunctionBorrow renderer_function_borrow;
          if (!GetOrCreateMuonRendererFunction(
                  impl, owner, source.function.function_id, expected_type,
                  &function, &renderer_function_borrow, error_message)) {
            return false;
          }
          decoded_args->renderer_function_borrows.push_back(
              std::move(renderer_function_borrow));
        }
        target = tra_ffic_value_function(function);
        break;
      }
      case MUON_TYPE_VOID:
        *error_message = "Void arguments are not supported";
        return false;
      default:
        *error_message = "Unsupported argument type";
        return false;
    }
  }
  return true;
}

static void InvokeMuonTrafficFunction(
    MuonPluginRuntimeImpl* impl,
    tra_ffic_side* caller_side,
    const tra_ffic_function_ref& function_ref,
    const MuonTypeMetadata& return_type,
    const MuonRpcOwner& owner,
    uint32_t call_id,
    MuonDecodedArguments decoded_args,
    MuonPluginRuntime::Completion completion) {
  auto* state = new MuonTrafficCallState;
  state->completion = std::move(completion);
  state->impl = impl;
  state->return_type = return_type;
  state->owner = owner;
  state->call_id = call_id;
  state->decoded_args = std::move(decoded_args);

  tra_ffic_error error;
  if (!tra_ffic_call_with_result(
          caller_side, &function_ref, state->decoded_args.values.data(),
          static_cast<uint32_t>(state->decoded_args.values.size()),
          HandleMuonTrafficCallResult, state, &error)) {
    MuonRpcCallResult result;
    result.owner = owner;
    result.call_id = call_id;
    result.success = false;
    result.error_message = GetMuonTrafficError(error);
    auto call_completion = std::move(state->completion);
    state->decoded_args.ResetFunctionBorrows();
    delete state;
    call_completion(result);
  }
}

static void KeepOrCloseMuonPluginLibrary(MuonPluginRuntimeImpl* impl,
                                          const std::string& locator,
                                          void* handle,
                                          size_t initial_function_count,
                                          muon_plugin_stop_func stop,
                                          muon_plugin_renderer_context_released_func
                                              renderer_context_released) {
  if (impl != nullptr &&
      impl->registered_functions.size() > initial_function_count) {
    MuonDynamicLibrary library;
    library.locator = locator;
    library.handle = handle;
    library.stop = stop;
    library.renderer_context_released = renderer_context_released;
    impl->libraries.push_back(library);
    if (impl->services.platform_adapter &&
        impl->services.platform_adapter->library_loaded) {
      impl->services.platform_adapter->library_loaded(
          handle, &impl->cancel_owner_operations);
    }
    return;
  }
  CloseMuonDynamicLibrary(impl, handle);
}

struct MuonPluginStopCallbackState {
  MuonPluginRuntimeImpl* impl = nullptr;
};

static void ContinueMuonPluginStop(MuonPluginRuntimeImpl* impl);

static void CompleteMuonPluginStop(void* user_data) {
  auto state = std::unique_ptr<MuonPluginStopCallbackState>(
      static_cast<MuonPluginStopCallbackState*>(user_data));
  if (!state || state->impl == nullptr) {
    return;
  }
  auto* impl = state->impl;
  if (impl->services.is_owner_thread()) {
    ContinueMuonPluginStop(impl);
    return;
  }
  if (!impl->services.post_owner_task(
          [impl]() { ContinueMuonPluginStop(impl); })) {
    LogMuonPluginRuntimeMessage(
        impl, MuonPluginRuntimeLogSource::Runtime,
        MUON_LOG_LEVEL_ERROR,
        "Cannot dispatch plugin shutdown completion to the runtime owner "
        "thread");
  }
}

static void ContinueMuonPluginStop(MuonPluginRuntimeImpl* impl) {
  if (impl == nullptr ||
      impl->stop_state != MuonPluginRuntimeStopState::Stopping) {
    return;
  }
  while (impl->next_stop_library_index > 0) {
    impl->next_stop_library_index -= 1;
    const auto stop =
        impl->libraries[impl->next_stop_library_index].stop;
    if (stop == nullptr) {
      continue;
    }
    auto state = std::make_unique<MuonPluginStopCallbackState>();
    state->impl = impl;
    stop(&CompleteMuonPluginStop, state.release());
    return;
  }

  impl->stop_state = MuonPluginRuntimeStopState::Stopped;
  auto completions = std::move(impl->stop_completions);
  impl->stop_completions.clear();
  for (auto& completion : completions) {
    if (completion) {
      completion();
    }
  }
}

static bool RegisterMuonPluginMetadata(MuonPluginRuntimeImpl* impl,
                                        const muon_plugin_metadata& metadata,
                                        const std::string& source_name,
                                        const MuonPluginPolicy& plugin_policy,
                                        bool allow_reserved_namespaces) {
  std::vector<MuonPreparedPluginNamespace> prepared_namespaces;
  if (!PrepareMuonPluginNamespaces(impl, metadata,
                                    std::filesystem::path(source_name),
                                    plugin_policy,
                                    allow_reserved_namespaces,
                                    &prepared_namespaces)) {
    return false;
  }

  for (const auto& prepared_namespace : prepared_namespaces) {
    impl->plugin_namespaces.insert(prepared_namespace.plugin_namespace);
    impl->namespace_paths.insert(prepared_namespace.namespace_paths.begin(),
                                 prepared_namespace.namespace_paths.end());
    impl->renderer_namespaces.push_back(
        {prepared_namespace.plugin_namespace,
         prepared_namespace.source->setup_script == nullptr
             ? ""
             : prepared_namespace.source->setup_script,
         prepared_namespace.allowed_function_names});
  }

  for (const auto& prepared_namespace : prepared_namespaces) {
    for (const auto* source : prepared_namespace.allowed_functions) {
      std::string error_message;
      if (!ValidateMuonPluginFunctionMetadata(*source, &error_message)) {
        continue;
      }

      const std::string js_name(source->js_name);
      const auto filter_name = GetMuonPluginFunctionFilterName(*source);
      const auto public_path = CreateMuonFunctionPublicPath(
          prepared_namespace.plugin_namespace, filter_name);
      auto registered_function = std::make_unique<MuonRegisteredFunction>();
      registered_function->metadata.id = AllocateMuonFunctionId(impl);
      registered_function->metadata.plugin_namespace =
          prepared_namespace.plugin_namespace;
      registered_function->metadata.js_name = js_name;
      registered_function->metadata.public_name = filter_name;
      if (!ConvertMuonFunctionSignature(
              source->signature, &registered_function->metadata.arg_types,
              &registered_function->metadata.return_type, &error_message)) {
        LogMuonPluginRuntimeMessage(
            impl, MuonPluginRuntimeLogSource::Runtime,
            MUON_LOG_LEVEL_WARNING,
            "Skipping plugin function " + js_name + ": " + error_message);
        continue;
      }
      registered_function->signature_storage = CreateSharedSignature(
          registered_function->metadata.arg_types,
          registered_function->metadata.return_type);

      tra_ffic_error error;
      if (!tra_ffic_side_create_pure_function_impl(
              &impl->plugin_side,
              GetMuonFunctionSignature(
                  registered_function->signature_storage.get()),
              ConvertMuonUserFunctionToTraffic(
                  reinterpret_cast<muon_user_function>(source->native_func)),
              &registered_function->function, &error)) {
        LogMuonPluginRuntimeMessage(
            impl, MuonPluginRuntimeLogSource::Runtime,
            MUON_LOG_LEVEL_WARNING,
            "Skipping plugin function " + js_name + ": " +
                GetMuonTrafficError(error));
        continue;
      }
      if (!CreateMuonTrafficFunctionRef(
              registered_function->function,
              registered_function->signature_storage,
              &registered_function->function_ref, &error_message)) {
        LogMuonPluginRuntimeMessage(
            impl, MuonPluginRuntimeLogSource::Runtime,
            MUON_LOG_LEVEL_WARNING,
            "Skipping plugin function " + js_name + ": " + error_message);
        (void)tra_ffic_function_release(registered_function->function, &error);
        continue;
      }

      const auto id = registered_function->metadata.id;
      impl->renderer_functions.push_back(registered_function->metadata);
      impl->function_paths[public_path] = id;
      const auto native_path = CreateMuonFunctionPublicPath(
          prepared_namespace.plugin_namespace, js_name);
      if (native_path != public_path) {
        impl->function_paths[native_path] = id;
      }
      impl->functions_by_id[id] = registered_function.get();
      impl->registered_functions.push_back(std::move(registered_function));
    }
  }

  return true;
}

static bool RegisterMuonPlatformFunctions(
    MuonPluginRuntimeImpl* impl,
    const MuonPluginPolicy& plugin_policy,
    const std::vector<MuonPluginRuntimePlatformNamespace>& namespaces) {
  if (impl == nullptr) {
    return false;
  }
  for (const auto& source_namespace : namespaces) {
    std::vector<std::string> namespace_segments;
    if (!SplitMuonPluginNamespace(
            source_namespace.plugin_namespace, &namespace_segments)) {
      return FailMuonPluginStartup(
          impl, "Platform namespace is invalid: " +
                    source_namespace.plugin_namespace);
    }
    const auto namespace_paths =
        CreateMuonNamespacePaths(namespace_segments);
    auto local_function_paths = std::set<std::string>{};
    auto allowed_functions =
        std::vector<const MuonPluginRuntimePlatformFunction*>{};
    auto allowed_function_names = std::vector<std::string>{};
    for (const auto& definition : source_namespace.functions) {
      if (!IsValidMuonJsIdentifier(definition.js_name) ||
          !IsValidMuonJsIdentifier(definition.public_name) ||
          definition.route_id == 0) {
        return FailMuonPluginStartup(
            impl, "Platform function metadata is invalid");
      }
      const auto public_path = CreateMuonFunctionPublicPath(
          source_namespace.plugin_namespace, definition.public_name);
      if (!IsMuonPluginFunctionAllowed(plugin_policy, public_path)) {
        continue;
      }
      const auto native_path = CreateMuonFunctionPublicPath(
          source_namespace.plugin_namespace, definition.js_name);
      if (local_function_paths.contains(public_path) ||
          (native_path != public_path &&
           local_function_paths.contains(native_path))) {
        return FailMuonPluginStartup(
            impl, "Duplicate plugin function path: " +
                      (local_function_paths.contains(public_path)
                           ? public_path
                           : native_path));
      }
      if (!ValidateMuonPluginFunctionPath(impl, public_path, true) ||
          (native_path != public_path &&
           !ValidateMuonPluginFunctionPath(impl, native_path, true))) {
        return false;
      }
      local_function_paths.insert(public_path);
      if (native_path != public_path) {
        local_function_paths.insert(native_path);
      }
      allowed_functions.push_back(&definition);
      allowed_function_names.push_back(definition.public_name);
    }
    if (allowed_functions.empty()) {
      continue;
    }
    if (!ValidateMuonPluginNamespaceRegistration(
            impl, source_namespace.plugin_namespace, namespace_paths,
            true)) {
      return false;
    }
    impl->plugin_namespaces.insert(source_namespace.plugin_namespace);
    impl->namespace_paths.insert(
        namespace_paths.begin(), namespace_paths.end());
    impl->renderer_namespaces.push_back(
        {source_namespace.plugin_namespace, source_namespace.setup_script,
         allowed_function_names});

    for (const auto* definition : allowed_functions) {
      const auto id = AllocateMuonFunctionId(impl);
      MuonFunctionMetadata function;
      function.id = id;
      function.plugin_namespace = source_namespace.plugin_namespace;
      function.js_name = definition->js_name;
      function.public_name = definition->public_name;
      function.arg_types = definition->arg_types;
      function.return_type = definition->return_type;
      impl->renderer_functions.push_back(std::move(function));
      impl->function_paths[CreateMuonFunctionPublicPath(
          source_namespace.plugin_namespace, definition->public_name)] = id;
      const auto native_path = CreateMuonFunctionPublicPath(
          source_namespace.plugin_namespace, definition->js_name);
      if (native_path != CreateMuonFunctionPublicPath(
                             source_namespace.plugin_namespace,
                             definition->public_name)) {
        impl->function_paths[native_path] = id;
      }
      impl->platform_route_ids_by_function_id[id] = definition->route_id;
    }
  }
  return true;
}

static bool LoadMuonPluginLibrary(
    MuonPluginRuntimeImpl* impl,
    const std::string& locator,
    const std::filesystem::path* file_path,
    const MuonPluginRuntimeLoadEntry& plugin,
    const MuonPluginPolicy& plugin_policy) {
  if (file_path != nullptr) {
    std::error_code filesystem_error;
    if (!std::filesystem::exists(*file_path, filesystem_error) ||
        filesystem_error ||
        !std::filesystem::is_regular_file(*file_path, filesystem_error) ||
        filesystem_error) {
      return FailMuonPluginStartup(
          impl, "Plugin file not found: " + file_path->string());
    }
    if (plugin.has_expected_signature) {
      if (!plugin.has_signature_salt) {
        return FailMuonPluginStartup(
            impl, "Plugin signature requires plugin salt: " + plugin.plugin);
      }
      std::string actual_signature;
      if (!muon_internal::CalculateFileSha256Hex(
              *file_path, plugin.signature_salt, &actual_signature)) {
        return FailMuonPluginStartup(
            impl, "Failed to calculate plugin signature: " +
                      file_path->string());
      }
      if (actual_signature != plugin.expected_signature) {
        return FailMuonPluginStartup(
            impl, "Plugin signature mismatch: " + file_path->string() +
                      " expected " + plugin.expected_signature + " actual " +
                      actual_signature);
      }
    }
  } else if (plugin.has_expected_signature || plugin.has_signature_salt) {
    return FailMuonPluginStartup(
        impl, "Packaged plugin signatures are not supported: " +
                  plugin.plugin);
  }

  auto loader_error = std::string{};
  auto* handle = OpenMuonDynamicLibrary(impl, locator, &loader_error);
  if (handle == nullptr) {
    auto message = "Failed to load plugin: " + locator;
    if (!loader_error.empty()) {
      message += ": " + loader_error;
    }
    return FailMuonPluginStartup(impl, message);
  }
  const auto initial_function_count = impl->registered_functions.size();
  const auto init_plugin = GetMuonPluginInitFunction(impl, handle);
  if (init_plugin == nullptr) {
    const auto error_message =
        "Plugin is missing " + std::string(kMuonPluginEntryPoint) + ": " +
        locator;
    CloseMuonDynamicLibrary(impl, handle);
    return FailMuonPluginStartup(impl, error_message);
  }

  std::vector<muon_plugin_config_entry> config_entries;
  const auto init_context =
      CreateMuonPluginInitContext(plugin, &kMuonPluginHelpers, &config_entries);
  const auto* metadata = init_plugin(&init_context);
  if (metadata == nullptr) {
    const auto error_message = "Plugin declined loading: " + locator;
    CloseMuonDynamicLibrary(impl, handle);
    return FailMuonPluginStartup(impl, error_message);
  }
  if (!RegisterMuonPluginMetadata(
          impl, *metadata, locator, plugin_policy, false)) {
    CloseMuonDynamicLibrary(impl, handle);
    return false;
  }
  if (impl->registered_functions.size() == initial_function_count) {
    const auto error_message =
        "Plugin registered no allowed functions: " + locator;
    CloseMuonDynamicLibrary(impl, handle);
    return FailMuonPluginStartup(impl, error_message);
  }

  KeepOrCloseMuonPluginLibrary(
      impl, locator, handle, initial_function_count, metadata->stop,
      metadata->renderer_context_released);
  return true;
}

static void ShutdownMuonPlatformAdapter(MuonPluginRuntimeImpl* impl) {
  if (impl == nullptr || !impl->platform_initialized) {
    return;
  }
  impl->platform_initialized = false;
  if (impl->services.platform_adapter &&
      impl->services.platform_adapter->shutdown) {
    impl->services.platform_adapter->shutdown();
  }
}

static const MuonPluginRuntimeLoadEntry* FindMuonInternalPluginEntry(
    const std::vector<MuonPluginRuntimeLoadEntry>& plugins) {
  for (const auto& plugin : plugins) {
    if (plugin.plugin == kMuonInternalPluginName) {
      return &plugin;
    }
  }
  return nullptr;
}

static bool RegisterMuonInternalPlugins(
    MuonPluginRuntimeImpl* impl,
    const MuonPluginRuntimeLoadEntry& plugin,
    const MuonPluginPolicy& plugin_policy) {
  if (!plugin_policy.HasAllowPatterns()) {
    return true;
  }

  const auto& adapter = impl->services.platform_adapter;
  if (!adapter || !adapter->initialize || !adapter->shutdown) {
    return FailMuonPluginStartup(
        impl, "Platform plugin adapter is unavailable");
  }

  std::vector<muon_plugin_config_entry> config_entries;
  const auto init_context =
      CreateMuonPluginInitContext(plugin, &kMuonPluginHelpers, &config_entries);
  MuonPluginRuntimePlatformInitialization initialization;
  auto error_message = std::string{};
  if (!adapter->initialize(
          &init_context, &initialization, &error_message)) {
    return FailMuonPluginStartup(
        impl, error_message.empty()
                  ? "Platform plugin initialization failed"
                  : error_message);
  }
  impl->platform_initialized = true;
  for (auto& cancel_owner : initialization.cancel_owner_operations) {
    impl->cancel_owner_operations.push_back(std::move(cancel_owner));
  }
  for (const auto& platform_plugin : initialization.plugins) {
    if (platform_plugin.metadata == nullptr ||
        !RegisterMuonPluginMetadata(
            impl, *platform_plugin.metadata, platform_plugin.source,
            plugin_policy, true)) {
      ShutdownMuonPlatformAdapter(impl);
      return false;
    }
  }
  if (!RegisterMuonPlatformFunctions(
          impl, plugin_policy, adapter->namespaces)) {
    ShutdownMuonPlatformAdapter(impl);
    return false;
  }
  return true;
}

static bool LoadConfiguredMuonPluginLibraries(MuonPluginRuntimeImpl* impl) {
  if (impl == nullptr) {
    return false;
  }
  for (const auto& plugin : impl->plugins) {
    if (plugin.plugin == kMuonInternalPluginName) {
      continue;
    }
    if (!plugin.plugin_policy) {
      return FailMuonPluginStartup(
          impl, "Plugin policy is unavailable: " + plugin.plugin);
    }
    if (!plugin.plugin_policy->HasAllowPatterns()) {
      continue;
    }
    if (plugin.has_library_locator) {
      if (plugin.library_locator.empty()) {
        return FailMuonPluginStartup(
            impl, "Plugin library locator is empty: " + plugin.plugin);
      }
      if (plugin.has_library_directory) {
        return FailMuonPluginStartup(
            impl, "Packaged plugin cannot set a library directory: " +
                      plugin.plugin);
      }
      if (!LoadMuonPluginLibrary(
              impl, plugin.library_locator, nullptr, plugin,
              *plugin.plugin_policy)) {
        return false;
      }
      continue;
    }
    const auto path = ResolveMuonPluginLibraryPath(
        plugin.has_library_directory ? plugin.library_directory
                                     : impl->plugin_directory,
        plugin.plugin);
    if (!LoadMuonPluginLibrary(
            impl, path.string(), &path, plugin, *plugin.plugin_policy)) {
      return false;
    }
  }
  return true;
}

MuonPluginRuntime::MuonPluginRuntime(
    std::filesystem::path plugin_directory,
    std::vector<MuonPluginRuntimeLoadEntry> plugins,
    MuonPluginRuntimeServices services)
    : impl_(std::make_unique<MuonPluginRuntimeImpl>(
          std::move(plugin_directory), std::move(plugins),
          std::move(services))) {
  g_muon_runtime_helpers = impl_.get();
  const auto* internal_plugin = FindMuonInternalPluginEntry(impl_->plugins);
  if (internal_plugin != nullptr) {
    if (!internal_plugin->plugin_policy) {
      FailMuonPluginStartup(impl_.get(),
                            "Plugin policy is unavailable: internal");
    } else {
      (void)RegisterMuonInternalPlugins(
          impl_.get(), *internal_plugin, *internal_plugin->plugin_policy);
    }
  }
  if (impl_->startup_error.empty() &&
      !LoadConfiguredMuonPluginLibraries(impl_.get())) {
    ShutdownMuonPlatformAdapter(impl_.get());
  }
  LogMuonPluginRuntimeMessage(
      impl_.get(), MuonPluginRuntimeLogSource::Runtime,
      MUON_LOG_LEVEL_INFO,
      "Loaded " + std::to_string(impl_->registered_functions.size()) +
          " plugin functions from " + impl_->plugin_directory.string());
}

MuonPluginRuntime::~MuonPluginRuntime() {
  ShutdownMuonPlatformAdapter(impl_.get());
  impl_->DrainTrafficTasks();
  if (g_muon_runtime_helpers == impl_.get()) {
    g_muon_runtime_helpers = nullptr;
  }
  for (const auto& owner_entry : impl_->renderer_sources_by_owner) {
    for (auto* source : owner_entry.second) {
      if (source == nullptr) {
        continue;
      }
      source->context_valid = false;
      source->renderer_lease_active = false;
    }
  }
  impl_->renderer_functions_by_source.clear();
  impl_->renderer_sources_by_owner.clear();
  impl_->active_function_owners.clear();
  std::vector<MuonPendingRendererFunctionCall> pending_renderer_calls;
  pending_renderer_calls.reserve(
      impl_->pending_renderer_function_calls.size());
  for (auto& pending_entry : impl_->pending_renderer_function_calls) {
    pending_renderer_calls.push_back(std::move(pending_entry.second));
  }
  impl_->pending_renderer_function_calls.clear();
  for (auto& pending_call : pending_renderer_calls) {
    pending_call.proxy_transfers.Reset();
    CompleteMuonRendererFunctionWithError(
        pending_call.completion, "Plugin runtime is shutting down");
    pending_call.completion = nullptr;
    pending_call.source_borrow.Reset();
    pending_call.source = nullptr;
  }
  impl_->DrainTrafficTasks();
  const auto proxy_leases_by_owner = impl_->proxy_leases_by_owner;
  for (const auto& owner_entry : proxy_leases_by_owner) {
    for (const auto& lease_entry : owner_entry.second) {
      (void)ReleaseMuonFunctionProxyLease(
          impl_.get(), owner_entry.first, lease_entry.second,
          lease_entry.first);
    }
  }
  impl_->DrainTrafficTasks();
  const auto close_library = impl_->services.close_library;
  auto libraries = std::move(impl_->libraries);
  impl_.reset();
  for (auto& library : libraries) {
    if (library.handle != nullptr && close_library) {
      close_library(library.handle);
    }
    library.handle = nullptr;
  }
#if defined(MUON_TRACK_FFI_CLOSURES)
  const auto snapshot = tra_ffic_get_closure_tracker_snapshot();
  std::fprintf(
      stderr,
      "MUON_FFI_CLOSURE_TRACKER alloc=%llu free=%llu live=%llu high_water=%llu\n",
      static_cast<unsigned long long>(snapshot.alloc_count),
      static_cast<unsigned long long>(snapshot.free_count),
      static_cast<unsigned long long>(snapshot.live_count),
      static_cast<unsigned long long>(snapshot.high_water));
  std::fflush(stderr);
#endif
}

const std::vector<MuonFunctionMetadata>& MuonPluginRuntime::GetFunctions()
    const {
  return impl_->renderer_functions;
}

const std::vector<MuonNamespaceMetadata>& MuonPluginRuntime::GetNamespaces()
    const {
  return impl_->renderer_namespaces;
}

bool MuonPluginRuntime::IsReady() const {
  return impl_->startup_error.empty();
}

std::string MuonPluginRuntime::GetStartupError() const {
  return impl_->startup_error;
}

void MuonPluginRuntime::Stop(StopCompletion completion) {
  if (completion) {
    impl_->stop_completions.push_back(std::move(completion));
  }
  if (impl_->stop_state == MuonPluginRuntimeStopState::Stopped) {
    auto completions = std::move(impl_->stop_completions);
    impl_->stop_completions.clear();
    for (auto& stopped_completion : completions) {
      if (stopped_completion) {
        stopped_completion();
      }
    }
    return;
  }
  if (impl_->stop_state == MuonPluginRuntimeStopState::Stopping) {
    return;
  }
  impl_->stop_state = MuonPluginRuntimeStopState::Stopping;
  impl_->next_stop_library_index = impl_->libraries.size();
  ContinueMuonPluginStop(impl_.get());
}

uint32_t MuonPluginRuntime::GetPlatformFunctionRouteId(
    uint32_t function_id) const {
  const auto iterator =
      impl_->platform_route_ids_by_function_id.find(function_id);
  if (iterator == impl_->platform_route_ids_by_function_id.end()) {
    return 0;
  }
  return iterator->second;
}

void MuonPluginRuntime::CancelPlatformOperationsForOwner(int owner_id) {
  if (!impl_ || owner_id <= 0) {
    return;
  }
  for (const auto& cancel_owner : impl_->cancel_owner_operations) {
    if (cancel_owner) {
      cancel_owner(owner_id);
    }
  }
}

bool MuonPluginRuntime::GetCallArgumentTypes(
    const MuonRpcCallRequest& request,
    std::vector<MuonTypeMetadata>* argument_types,
    std::string* error_message) const {
  if (argument_types == nullptr || error_message == nullptr) {
    return false;
  }
  argument_types->clear();
  error_message->clear();
  if (!IsValidMuonRpcOwner(request.owner) || request.call_id == 0 ||
      (request.kind == MuonRpcCallKind::PluginProxy &&
       request.function_id == 0)) {
    *error_message = "Invalid muon plugin call";
    return false;
  }
  if (request.kind == MuonRpcCallKind::Plugin) {
    for (const auto& function : impl_->renderer_functions) {
      if (function.id == request.function_id) {
        *argument_types = function.arg_types;
        return true;
      }
    }
    *error_message = "Unknown muon plugin function";
    return false;
  }

  MuonFunctionProxy proxy;
  if (!TryGetMuonFunctionProxyForLease(
          impl_.get(), CreateMuonFunctionOwnerId(request.owner),
          request.function_id, request.proxy_lease_token, &proxy) ||
      proxy.function_type.type != MUON_TYPE_FUNCTION ||
      proxy.function_type.function_return_type.empty()) {
    *error_message = "Unknown muon function proxy";
    return false;
  }
  *argument_types = proxy.function_type.function_arg_types;
  return true;
}

static void CompleteMuonPluginCallWithError(
    const MuonRpcCallRequest& request,
    const MuonPluginRuntime::Completion& completion,
    const std::string& error_message) {
  if (!completion) {
    return;
  }
  MuonRpcCallResult result;
  result.owner = request.owner;
  result.call_id = request.call_id;
  result.success = false;
  result.error_message = error_message;
  completion(result);
}

void MuonPluginRuntime::Invoke(const MuonRpcCallRequest& request,
                                Completion completion) {
  if (impl_->stop_state != MuonPluginRuntimeStopState::Running) {
    CompleteMuonPluginCallWithError(
        request, completion, "Plugin runtime is shutting down");
    return;
  }
  if (!IsValidMuonRpcOwner(request.owner) || request.call_id == 0 ||
      (request.kind == MuonRpcCallKind::PluginProxy &&
       request.function_id == 0)) {
    CompleteMuonPluginCallWithError(
        request, completion, "Invalid muon plugin call");
    return;
  }

  impl_->active_function_owners[CreateMuonFunctionOwnerId(request.owner)] = {
      request.owner.browser_id,
      request.owner.frame_id,
      request.owner.context_id,
  };
  auto function_ref = tra_ffic_function_ref{};
  auto argument_types = std::vector<MuonTypeMetadata>{};
  auto return_type = CreateMuonPrimitiveType(MUON_TYPE_VOID);
  MuonFunctionRetain proxy_retain;
  if (request.kind == MuonRpcCallKind::Plugin) {
    const auto function_iterator =
        impl_->functions_by_id.find(request.function_id);
    if (function_iterator == impl_->functions_by_id.end()) {
      CompleteMuonPluginCallWithError(
          request, completion, "Unknown muon plugin function");
      return;
    }
    const auto* function = function_iterator->second;
    function_ref = function->function_ref;
    argument_types = function->metadata.arg_types;
    return_type = function->metadata.return_type;
  } else {
    MuonFunctionProxy proxy;
    if (!TryGetMuonFunctionProxyForLease(
            impl_.get(), CreateMuonFunctionOwnerId(request.owner),
            request.function_id, request.proxy_lease_token, &proxy) ||
        proxy.function_type.type != MUON_TYPE_FUNCTION ||
        proxy.function_type.function_return_type.empty()) {
      CompleteMuonPluginCallWithError(
          request, completion, "Unknown muon function proxy");
      return;
    }
    auto retain_error = std::string{};
    if (!proxy_retain.Acquire(proxy.function, &retain_error)) {
      CompleteMuonPluginCallWithError(request, completion, retain_error);
      return;
    }
    function_ref = proxy.function_ref;
    argument_types = proxy.function_type.function_arg_types;
    return_type = proxy.function_type.function_return_type[0];
  }

  MuonDecodedArguments decoded_args;
  std::string error_message;
  if (!DecodeMuonPluginArguments(
          impl_.get(), request.owner, argument_types, request.arguments,
          &decoded_args, &error_message)) {
    decoded_args.ResetFunctionBorrows();
    CompleteMuonPluginCallWithError(request, completion, error_message);
    return;
  }
  if (proxy_retain.function != nullptr) {
    decoded_args.function_retains.push_back(std::move(proxy_retain));
  }

  auto* dispatcher = cardio::unsafe_get_current_dispatcher();
  if (dispatcher == nullptr) {
    decoded_args.ResetFunctionBorrows();
    CompleteMuonPluginCallWithError(
        request, completion, "muon main dispatcher is unavailable");
    return;
  }

  muon_internal::FireAndForgetOnDispatcher(
      dispatcher,
      [impl = impl_.get(),
       function_ref,
       return_type,
       owner = request.owner,
       call_id = request.call_id,
       decoded_args = std::move(decoded_args),
       completion]() mutable {
    InvokeMuonTrafficFunction(
        impl, &impl->renderer_side, function_ref, return_type, owner, call_id,
        std::move(decoded_args), std::move(completion));
  });
}

void MuonPluginRuntime::ReleasePluginFunctionProxy(
    const MuonRpcPluginProxyRelease& release) {
  const auto owner_id = CreateMuonFunctionOwnerId(release.owner);
  (void)ReleaseMuonFunctionProxyLease(
      impl_.get(), owner_id, release.proxy_id, release.lease_token);
}

bool MuonPluginRuntime::GetRendererFunctionReturnType(
    const MuonRpcOwner& owner,
    uint32_t call_id,
    MuonTypeMetadata* return_type) const {
  if (return_type == nullptr || !IsValidMuonRpcOwner(owner) || call_id == 0) {
    return false;
  }
  const auto pending_iterator =
      impl_->pending_renderer_function_calls.find(call_id);
  if (pending_iterator == impl_->pending_renderer_function_calls.end()) {
    return false;
  }
  const auto* source = pending_iterator->second.source;
  if (source == nullptr || !AreEqualMuonRpcOwners(owner, source->owner) ||
      source->function_type.function_return_type.empty()) {
    return false;
  }
  *return_type = source->function_type.function_return_type[0];
  return true;
}

void MuonPluginRuntime::CompleteRendererFunctionCall(
    const MuonRpcRendererFunctionResult& result) {
  const auto call_id = result.call_id;
  MuonPendingRendererFunctionCall pending_call;
  const auto pending_iterator =
      impl_->pending_renderer_function_calls.find(call_id);
  if (pending_iterator == impl_->pending_renderer_function_calls.end()) {
    return;
  }
  const auto* pending_source = pending_iterator->second.source;
  if (pending_source == nullptr ||
      !AreEqualMuonRpcOwners(result.owner, pending_source->owner)) {
    return;
  }
  pending_call = std::move(pending_iterator->second);
  impl_->pending_renderer_function_calls.erase(pending_iterator);

  const auto complete = [&pending_call, call_id](
                            const void* value,
                            const char* error_message) {
    CompleteMuonPendingRendererFunctionCall(
        &pending_call, call_id, value, error_message);
  };
  if (pending_call.completion == nullptr) {
    complete(nullptr, nullptr);
    return;
  }
  if (!result.success) {
    complete(nullptr, result.error_message.c_str());
    return;
  }
  if (pending_call.source == nullptr ||
      pending_call.source->function_type.function_return_type.empty()) {
    complete(nullptr, "Renderer function return type is invalid");
    return;
  }

  const auto& expected_type =
      pending_call.source->function_type.function_return_type[0];
  if (!AreEqualMuonTypes(result.value.type, expected_type)) {
    complete(nullptr, "Renderer function returned an unexpected type");
    return;
  }

  auto bool_storage = false;
  auto i8_storage = int8_t{0};
  auto u8_storage = uint8_t{0};
  auto i16_storage = int16_t{0};
  auto u16_storage = uint16_t{0};
  auto i32_storage = int32_t{0};
  auto u32_storage = uint32_t{0};
  auto i64_storage = int64_t{0};
  auto u64_storage = uint64_t{0};
  auto f32_storage = 0.0f;
  auto f64_storage = 0.0;
  void* pointer_storage = nullptr;
  std::string string_storage;
  const char* string_pointer = nullptr;
  muon_native_function function_storage = nullptr;
  muon_buffer_view buffer_storage = {nullptr, 0};
  switch (expected_type.type) {
    case MUON_TYPE_VOID:
      complete(nullptr, nullptr);
      return;
    case MUON_TYPE_BOOL:
      bool_storage = result.value.bool_value;
      complete(&bool_storage, nullptr);
      return;
    case MUON_TYPE_I8:
      i8_storage = result.value.i8_value;
      complete(&i8_storage, nullptr);
      return;
    case MUON_TYPE_U8:
      u8_storage = result.value.u8_value;
      complete(&u8_storage, nullptr);
      return;
    case MUON_TYPE_I16:
      i16_storage = result.value.i16_value;
      complete(&i16_storage, nullptr);
      return;
    case MUON_TYPE_U16:
      u16_storage = result.value.u16_value;
      complete(&u16_storage, nullptr);
      return;
    case MUON_TYPE_I32:
      i32_storage = result.value.i32_value;
      complete(&i32_storage, nullptr);
      return;
    case MUON_TYPE_U32:
      u32_storage = result.value.u32_value;
      complete(&u32_storage, nullptr);
      return;
    case MUON_TYPE_I64:
      i64_storage = result.value.i64_value;
      complete(&i64_storage, nullptr);
      return;
    case MUON_TYPE_U64:
      u64_storage = result.value.u64_value;
      complete(&u64_storage, nullptr);
      return;
    case MUON_TYPE_F32:
      f32_storage = result.value.f32_value;
      if (!std::isfinite(f32_storage)) {
        complete(nullptr, "Renderer function returned a non-f32 value");
        return;
      }
      complete(&f32_storage, nullptr);
      return;
    case MUON_TYPE_F64:
      f64_storage = result.value.f64_value;
      if (!std::isfinite(f64_storage)) {
        complete(nullptr, "Renderer function returned a non-f64 value");
        return;
      }
      complete(&f64_storage, nullptr);
      return;
    case MUON_TYPE_POINTER:
      pointer_storage = reinterpret_cast<void*>(result.value.pointer_value);
      complete(&pointer_storage, nullptr);
      return;
    case MUON_TYPE_STRING:
      if (result.value.is_null) {
        complete(&string_pointer, nullptr);
        return;
      }
      string_storage = result.value.string_value;
      string_pointer = string_storage.c_str();
      complete(&string_pointer, nullptr);
      return;
    case MUON_TYPE_BUFFER_VIEW: {
      if (!IsValidMuonRpcBinary(result.value.binary) ||
          result.value.binary.size >
              static_cast<size_t>(std::numeric_limits<uintptr_t>::max())) {
        complete(nullptr, "Renderer function buffer_view payload is missing");
        return;
      }
      buffer_storage.data = GetMuonRpcBinaryData(result.value.binary);
      buffer_storage.size =
          static_cast<uintptr_t>(result.value.binary.size);
      complete(&buffer_storage, nullptr);
      return;
    }
    case MUON_TYPE_FUNCTION: {
      if (result.value.is_null) {
        complete(&function_storage, nullptr);
        return;
      }
      if (!AreEqualMuonTypes(result.value.function.type, expected_type)) {
        complete(nullptr, "Renderer function result is invalid");
        return;
      }
      if (result.value.function.kind == MuonRpcFunctionKind::PluginProxy) {
        if (result.value.function.proxy_id == 0 ||
            result.value.function.lease_token.empty()) {
          complete(nullptr, "Renderer returned an invalid function proxy");
          return;
        }
        MuonFunctionProxy proxy;
        if (!TryGetMuonFunctionProxyForLease(
                impl_.get(), pending_call.source->owner_id,
                result.value.function.proxy_id,
                result.value.function.lease_token, &proxy) ||
            !AreEqualMuonTypes(proxy.function_type, expected_type)) {
          complete(nullptr, "Renderer returned an unknown function proxy");
          return;
        }
        MuonFunctionRetain function_retain;
        std::string error_message;
        if (!function_retain.Acquire(proxy.function, &error_message)) {
          complete(nullptr, error_message.c_str());
          return;
        }
        function_storage = proxy.function;
        complete(&function_storage, nullptr);
        return;
      }

      if (result.value.function.renderer_context_id !=
              pending_call.source->renderer_context_id ||
          result.value.function.function_id <= 0) {
        complete(nullptr, "Renderer function result is invalid");
        return;
      }
      std::string error_message;
      MuonRendererFunctionBorrow renderer_function_borrow;
      if (!GetOrCreateMuonRendererFunction(
              impl_.get(), pending_call.source->owner,
              result.value.function.function_id, expected_type,
              &function_storage,
              &renderer_function_borrow, &error_message)) {
        complete(nullptr, error_message.c_str());
        return;
      }
      complete(&function_storage, nullptr);
      renderer_function_borrow.Reset();
      return;
    }
    default:
      complete(nullptr, "Unsupported renderer return type");
      return;
  }
}

static void ReleaseMuonFunctionOwner(MuonPluginRuntimeImpl* impl,
                                     const std::string& owner_id,
                                     int renderer_context_id) {
  if (impl == nullptr) {
    return;
  }
  const auto active_owner =
      impl->active_function_owners.find(owner_id);
  if (active_owner != impl->active_function_owners.end()) {
    for (const auto& library : impl->libraries) {
      if (library.renderer_context_released != nullptr) {
        library.renderer_context_released(owner_id.c_str());
      }
    }
  }
  if (impl->services.platform_adapter &&
      impl->services.platform_adapter->release_context) {
    impl->services.platform_adapter->release_context(renderer_context_id);
  }
  impl->active_function_owners.erase(owner_id);
  std::vector<MuonRendererFunctionSource*> sources;
  const auto owner_iterator =
      impl->renderer_sources_by_owner.find(owner_id);
  if (owner_iterator != impl->renderer_sources_by_owner.end()) {
    sources.assign(owner_iterator->second.begin(),
                   owner_iterator->second.end());
    impl->renderer_sources_by_owner.erase(owner_iterator);
  }

  for (auto* source : sources) {
    if (source == nullptr) {
      continue;
    }
    source->context_valid = false;
    source->renderer_lease_active = false;
    const auto source_iterator =
        impl->renderer_functions_by_source.find(source->source_id);
    if (source_iterator != impl->renderer_functions_by_source.end() &&
        source_iterator->second == source) {
      impl->renderer_functions_by_source.erase(source_iterator);
    }
  }

  std::vector<MuonPendingRendererFunctionCall> pending_calls;
  auto pending_iterator = impl->pending_renderer_function_calls.begin();
  while (pending_iterator != impl->pending_renderer_function_calls.end()) {
    auto* source = pending_iterator->second.source;
    if (source == nullptr || source->owner_id != owner_id) {
      ++pending_iterator;
      continue;
    }
    pending_calls.push_back(std::move(pending_iterator->second));
    pending_iterator =
        impl->pending_renderer_function_calls.erase(pending_iterator);
  }

  for (auto* source : sources) {
    ReleaseMuonRendererFunctionBridgeRetainIfIdle(source);
  }
  for (auto& pending_call : pending_calls) {
    pending_call.proxy_transfers.Reset();
    CompleteMuonRendererFunctionWithError(
        pending_call.completion, "Renderer function context was released");
    pending_call.completion = nullptr;
  }

  auto proxy_leases = std::map<std::string, uint32_t>{};
  const auto proxy_iterator =
      impl->proxy_leases_by_owner.find(owner_id);
  if (proxy_iterator != impl->proxy_leases_by_owner.end()) {
    proxy_leases = proxy_iterator->second;
  }
  for (const auto& lease_entry : proxy_leases) {
    (void)ReleaseMuonFunctionProxyLease(
        impl, owner_id, lease_entry.second, lease_entry.first);
  }
}

void MuonPluginRuntime::ReleaseFunctionContext(
    const MuonRpcContextReleased& release) {
  if (!IsValidMuonRpcOwner(release.owner)) {
    return;
  }
  ReleaseMuonFunctionOwner(
      impl_.get(), CreateMuonFunctionOwnerId(release.owner),
      release.owner.context_id);
}

void MuonPluginRuntime::ReleaseFunctionFrame(int browser_id,
                                             const std::string& frame_id) {
  if (browser_id <= 0 || frame_id.empty()) {
    return;
  }
  auto owners = std::vector<std::pair<std::string, int>>{};
  for (const auto& owner_entry : impl_->active_function_owners) {
    if (owner_entry.second.browser_id == browser_id &&
        owner_entry.second.frame_id == frame_id) {
      owners.emplace_back(owner_entry.first,
                          owner_entry.second.renderer_context_id);
    }
  }
  for (const auto& owner : owners) {
    ReleaseMuonFunctionOwner(impl_.get(), owner.first, owner.second);
  }
}

void MuonPluginRuntime::ReleaseFunctionBrowser(int browser_id) {
  if (browser_id <= 0) {
    return;
  }
  auto owners = std::vector<std::pair<std::string, int>>{};
  for (const auto& owner_entry : impl_->active_function_owners) {
    if (owner_entry.second.browser_id == browser_id) {
      owners.emplace_back(owner_entry.first,
                          owner_entry.second.renderer_context_id);
    }
  }
  for (const auto& owner : owners) {
    ReleaseMuonFunctionOwner(impl_.get(), owner.first, owner.second);
  }
}

#if defined(MUON_TEST_BUILD)
MuonFunctionWrapperDiagnostics
MuonPluginRuntime::GetFunctionWrapperDiagnostics(
    const MuonRpcOwner& owner) const {
  auto diagnostics = MuonFunctionWrapperDiagnostics{};
  const auto owner_id = CreateMuonFunctionOwnerId(owner);
  const auto owner_counts =
      impl_->function_wrapper_lifecycle.GetOwnerCounts(owner_id);
  const auto global_counts =
      impl_->function_wrapper_lifecycle.GetGlobalCounts();
  diagnostics.owner.sources = owner_counts.renderer_source_count;
  diagnostics.owner.proxy_leases =
      owner_counts.plugin_proxy_lease_count;
  diagnostics.global.sources = global_counts.renderer_source_count;
  diagnostics.global.proxy_leases =
      global_counts.plugin_proxy_lease_count;

  for (const auto* source : impl_->live_renderer_function_sources) {
    if (source == nullptr) {
      continue;
    }
    diagnostics.global.borrows += source->active_bridge_borrows;
    if (source->owner_id == owner_id) {
      diagnostics.owner.borrows += source->active_bridge_borrows;
    }
  }
  diagnostics.global.proxies = impl_->proxies_by_id.size();
  const auto owner_proxy_iterator =
      impl_->proxy_leases_by_owner.find(owner_id);
  if (owner_proxy_iterator != impl_->proxy_leases_by_owner.end()) {
    auto owner_proxy_ids = std::set<uint32_t>{};
    for (const auto& lease_entry : owner_proxy_iterator->second) {
      owner_proxy_ids.insert(lease_entry.second);
    }
    diagnostics.owner.proxies = owner_proxy_ids.size();
  }

#if defined(MUON_TRACK_FFI_CLOSURES)
  const auto closure_snapshot = tra_ffic_get_closure_tracker_snapshot();
  diagnostics.ffi_closures_enabled = true;
  diagnostics.ffi_closure_alloc = closure_snapshot.alloc_count;
  diagnostics.ffi_closure_free = closure_snapshot.free_count;
  diagnostics.ffi_closure_live = closure_snapshot.live_count;
  diagnostics.ffi_closure_high_water = closure_snapshot.high_water;
#endif
  return diagnostics;
}
#endif
