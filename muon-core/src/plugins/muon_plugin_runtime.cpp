/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "plugins/muon_plugin_runtime.h"

#include "muon_cardio_post.h"
#include "muon_sha1.h"

#include "plugins/builtin/muon_builtin.h"
#include "browser/muon_builtin_browser.h"
#include "plugins/builtin/muon_builtin_executor.h"
#include "plugins/builtin/muon_builtin_fs.h"
#include "plugins/builtin/muon_builtin_fs_dialogs_plugin.h"
#include "plugins/muon_js_bridge.h"
#include "config/muon_paths.h"
#include "log/muon_log.h"
#include "plugins/muon_shared_buffer.h"

#include "include/cef_command_line.h"
#include "include/cef_shared_process_message_builder.h"
#include "include/cef_task.h"
#include "include/wrapper/cef_helpers.h"

#include <cardio.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <algorithm>
#include <charconv>
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
#include <system_error>
#include <utility>
#include <vector>

static constexpr char kMuonPluginEntryPoint[] = "muon_init_plugin";
static constexpr char kMuonInternalPluginName[] = "internal";
static constexpr uint32_t kMaxMuonPluginFunctionArgs = 32;
static constexpr char kMuonFunctionArgumentContextIdKey[] = "context_id";
static constexpr char kMuonFunctionArgumentFunctionIdKey[] = "function_id";
static constexpr char kMuonFunctionArgumentKindKey[] = "kind";
static constexpr char kMuonFunctionArgumentKindPluginProxy[] = "plugin_proxy";
static constexpr char kMuonFunctionArgumentProxyIdKey[] = "proxy_id";
static constexpr char kMuonFunctionArgumentTypeKey[] = "type_key";

class MuonFunctionTask final : public CefTask {
 public:
  explicit MuonFunctionTask(std::function<void()> task)
      : task_(std::move(task)) {}

  void Execute() override { task_(); }

 private:
  std::function<void()> task_;

  IMPLEMENT_REFCOUNTING(MuonFunctionTask);
  DISALLOW_COPY_AND_ASSIGN(MuonFunctionTask);
};

struct MuonDynamicLibrary {
  std::filesystem::path path;
  void* handle = nullptr;
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
  uint32_t wrapper_lease_count = 0;
};

struct MuonRendererFunctionSource {
  struct MuonPluginRuntimeImpl* impl = nullptr;
  MuonPluginInvocationContext context;
  std::string owner_id;
  std::string source_id;
  std::string lease_token;
  int renderer_context_id = 0;
  int function_id = 0;
  MuonTypeMetadata function_type;
  muon_native_function function = nullptr;
  size_t active_bridge_borrows = 0;
  bool bridge_retain_active = false;
  bool context_valid = true;
  bool renderer_lease_active = false;
};

static void ReleaseMuonRendererFunctionBorrow(
    MuonRendererFunctionSource* source);

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

struct MuonPendingRendererFunctionCall {
  tra_ffic_completion completion = nullptr;
  MuonRendererFunctionSource* source = nullptr;
  MuonRendererFunctionBorrow source_borrow;
};

struct muon_shared_buffer {
  CefRefPtr<CefSharedProcessMessageBuilder> builder;
  void* data = nullptr;
  size_t size = 0;
  std::string message_name;
};

struct MuonReleasedSharedBufferRange {
  uintptr_t begin = 0;
  uintptr_t end = 0;
  size_t size = 0;
};

struct MuonDecodedArguments {
  std::vector<tra_ffic_value> values;
  std::vector<std::string> string_storage;
  std::shared_ptr<MuonSharedBufferPayload> shared_payload;
  std::vector<MuonRendererFunctionBorrow> renderer_function_borrows;

  void ResetRendererFunctionBorrows() {
    renderer_function_borrows.clear();
  }
};

struct MuonTrafficCallState {
  MuonPluginRuntime::Completion completion;
  MuonPluginRuntimeImpl* impl = nullptr;
  MuonDecodedArguments decoded_args;
  MuonTypeMetadata return_type = CreateMuonPrimitiveType(MUON_TYPE_VOID);
  int call_id = 0;
  int renderer_context_id = 0;
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
                         std::vector<MuonPluginRuntimeLoadEntry> plugins);
  ~MuonPluginRuntimeImpl();

  void RequestTrafficDrain(tra_ffic_task_queue* queue);
  void DrainTrafficTasks();
  bool HasTrafficTasks();

  std::filesystem::path plugin_directory;
  std::vector<MuonPluginRuntimeLoadEntry> plugins;
  std::vector<MuonDynamicLibrary> libraries;
  std::vector<MuonFsDialogsCancelOwnerBrowserFunction>
      fs_dialogs_cancel_owner_functions;
  std::vector<std::unique_ptr<MuonRegisteredFunction>> registered_functions;
  std::vector<MuonNamespaceMetadata> renderer_namespaces;
  std::vector<MuonFunctionMetadata> renderer_functions;
  uint32_t next_function_id = 0;
  std::map<uint32_t, MuonRegisteredFunction*> functions_by_id;
  std::map<uint32_t, MuonBuiltinBrowserFunctionKind>
      builtin_browser_functions_by_id;
  std::set<std::string> plugin_namespaces;
  std::set<std::string> namespace_paths;
  std::map<std::string, uint32_t> function_paths;
  std::string startup_error;

  cardio::dispatcher* main_dispatcher = nullptr;
  std::shared_ptr<MuonTrafficDrainState> traffic_drain_state;
  std::unique_ptr<std::shared_ptr<MuonTrafficDrainState>>
      traffic_drain_state_handle;
  tra_ffic_task_queue traffic_queue = {};
  tra_ffic_side renderer_side = {};
  tra_ffic_side plugin_side = {};
  bool traffic_initialized = false;

  uint64_t next_renderer_source_lease_token = 1;
  std::map<std::string, MuonRendererFunctionSource*>
      renderer_functions_by_source;
  std::map<std::string, std::set<MuonRendererFunctionSource*>>
      renderer_sources_by_owner;

  uint32_t next_renderer_call_id = 1;
  std::map<uint32_t, MuonPendingRendererFunctionCall>
      pending_renderer_function_calls;

  uint32_t next_proxy_id = 1;
  std::map<uint32_t, MuonFunctionProxy> proxies_by_id;
  std::map<std::string, uint32_t> proxy_ids_by_key;
  std::map<std::string, std::vector<uint32_t>> proxy_ids_by_owner;

  std::map<muon_shared_buffer_handle,
           std::unique_ptr<muon_shared_buffer>>
      shared_buffer_allocations;
  std::vector<MuonReleasedSharedBufferRange> released_shared_buffer_ranges;
};

static MuonPluginRuntimeImpl* g_muon_runtime_helpers = nullptr;

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
    std::vector<MuonPluginRuntimeLoadEntry> plugins)
    : plugin_directory(std::move(plugin_directory)),
      plugins(std::move(plugins)),
      main_dispatcher(cardio::unsafe_get_current_dispatcher()),
      traffic_drain_state(std::make_shared<MuonTrafficDrainState>()) {
  traffic_drain_state->dispatcher = main_dispatcher;
  traffic_drain_state->impl = this;
  traffic_drain_state_handle =
      std::make_unique<std::shared_ptr<MuonTrafficDrainState>>(
          traffic_drain_state);
  if (main_dispatcher == nullptr) {
    startup_error = "muon main dispatcher is unavailable";
    LogMuonMessage(kMuonLogSourceMuon, kMuonLogLevelError, startup_error);
    return;
  }
  tra_ffic_error error;
  if (!tra_ffic_task_queue_init(
          &traffic_queue,
          NotifyMuonTrafficFinalization,
          traffic_drain_state_handle.get())) {
    LogMuonMessage(kMuonLogSourceMuon, kMuonLogLevelError,
                   "Failed to initialize tra-ffic task queue");
    return;
  }
  if (!tra_ffic_side_init_pair(&renderer_side, &plugin_side,
                               tra_ffic_task_queue_schedule_callback,
                               &traffic_queue,
                               &error)) {
    LogMuonMessage(kMuonLogSourceMuon, kMuonLogLevelError,
                   "Failed to initialize tra-ffic sides: " +
                       GetMuonTrafficError(error));
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
  if (!CefCurrentlyOn(TID_UI)) {
    auto drain_state = traffic_drain_state;
    CefPostTask(TID_UI, new MuonFunctionTask([drain_state, queue]() {
      auto* impl = drain_state->impl;
      if (impl != nullptr) {
        impl->RequestTrafficDrain(queue);
      }
    }));
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
  if (!CefCurrentlyOn(TID_UI)) {
    auto drain_state = traffic_drain_state;
    CefPostTask(TID_UI, new MuonFunctionTask([drain_state]() {
      auto* impl = drain_state->impl;
      if (impl != nullptr) {
        impl->DrainTrafficTasks();
        return;
      }
      drain_state->drain_posted = false;
    }));
    return;
  }
  CEF_REQUIRE_UI_THREAD();
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

static void CloseMuonDynamicLibrary(void* handle) {
  if (handle == nullptr) {
    return;
  }
#if defined(_WIN32)
  FreeLibrary(static_cast<HMODULE>(handle));
#else
  dlclose(handle);
#endif
}

static void* OpenMuonDynamicLibrary(const std::filesystem::path& path) {
#if defined(_WIN32)
  return LoadLibraryW(path.wstring().c_str());
#else
  const auto native_path = path.string();
  return dlopen(native_path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

static void* GetMuonDynamicLibrarySymbol(void* handle, const char* name) {
  if (handle == nullptr || name == nullptr) {
    return nullptr;
  }
#if defined(_WIN32)
  return reinterpret_cast<void*>(
      GetProcAddress(static_cast<HMODULE>(handle), name));
#else
  return dlsym(handle, name);
#endif
}

static const char* GetMuonPluginLibraryExtension() {
#if defined(_WIN32)
  return ".dll";
#else
  return ".so";
#endif
}

static muon_init_plugin_func GetMuonPluginInitFunction(void* handle) {
  const auto address = GetMuonDynamicLibrarySymbol(handle, kMuonPluginEntryPoint);
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

static MuonFsDialogsCancelOwnerBrowserFunction
GetMuonFsDialogsCancelOwnerBrowserFunction(void* handle) {
  const auto address = GetMuonDynamicLibrarySymbol(
      handle, kMuonFsDialogsCancelOwnerBrowserSymbol);
  return reinterpret_cast<MuonFsDialogsCancelOwnerBrowserFunction>(address);
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
    const std::string& message_name,
    int call_id,
    int renderer_context_id,
    size_t value_index,
    const muon_buffer_view& view,
    MuonCreatedSharedBufferMessage* created_message,
    bool* consumed,
    std::string* error_message) {
  if (created_message == nullptr || consumed == nullptr ||
      error_message == nullptr) {
    return false;
  }
  *consumed = false;
  created_message->message = nullptr;
  created_message->entries.clear();
  if (impl == nullptr || view.data == nullptr) {
    return true;
  }

  auto view_begin = uintptr_t{0};
  auto view_end = uintptr_t{0};
  if (!GetMuonBufferViewRange(view, &view_begin, &view_end, error_message)) {
    return false;
  }

  std::unique_ptr<muon_shared_buffer> allocation;
  auto entry_offset = size_t{0};
  for (auto iterator = impl->shared_buffer_allocations.begin();
       iterator != impl->shared_buffer_allocations.end(); ++iterator) {
    const auto* candidate = iterator->second.get();
    const auto candidate_begin = reinterpret_cast<uintptr_t>(candidate->data);
    const auto candidate_end = candidate_begin + candidate->size;
    if (candidate->message_name == message_name &&
        IsMuonRangeInside(view_begin, view_end, candidate_begin,
                           candidate_end)) {
      allocation = std::move(iterator->second);
      impl->shared_buffer_allocations.erase(iterator);
      AddReleasedMuonSharedBufferRange(impl, allocation->data,
                                        allocation->size);
      entry_offset = GetMuonSharedBufferSingleEntryDataOffset() +
                     static_cast<size_t>(view_begin - candidate_begin);
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
  if (!allocation->builder || !allocation->builder->IsValid() ||
      allocation->builder->Memory() == nullptr) {
    *error_message = "Shared buffer allocation is no longer valid";
    return false;
  }

  MuonSharedBufferEntry entry;
  entry.value_index = value_index;
  entry.offset = entry_offset;
  entry.size = static_cast<size_t>(view.size);
  created_message->entries.push_back(entry);
  if (!WriteMuonSharedBufferPayloadHeader(
          allocation->builder->Memory(), allocation->builder->Size(), call_id,
          renderer_context_id, created_message->entries, error_message)) {
    return false;
  }
  created_message->message = allocation->builder->Build();
  if (!created_message->message) {
    *error_message = "Failed to build shared buffer payload";
    return false;
  }
  *consumed = true;
  return true;
}

static bool CreateMuonRuntimeSharedBufferMessage(
    MuonPluginRuntimeImpl* impl,
    const std::string& message_name,
    int call_id,
    int renderer_context_id,
    const std::vector<MuonSharedBufferSource>& sources,
    MuonCreatedSharedBufferMessage* created_message,
    std::string* error_message) {
  if (sources.size() == 1) {
    muon_buffer_view view = {
        const_cast<void*>(sources[0].data),
        static_cast<uintptr_t>(sources[0].size),
    };
    auto consumed = false;
    if (!TryConsumeMuonSharedBufferAllocation(
            impl, message_name, call_id, renderer_context_id,
            sources[0].value_index, view, created_message, &consumed,
            error_message)) {
      return false;
    }
    if (consumed) {
      return true;
    }
  }

  for (const auto& source : sources) {
    const muon_buffer_view view = {
        const_cast<void*>(source.data),
        static_cast<uintptr_t>(source.size),
    };
    if (MatchesReleasedMuonSharedBufferAllocation(impl, view)) {
      *error_message = "Buffer view references a released shared buffer";
      return false;
    }
  }
  return CreateMuonSharedBufferMessage(message_name, call_id,
                                        renderer_context_id, sources,
                                        created_message, error_message);
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
  auto payload_size = size_t{0};
  if (!GetMuonSharedBufferSingleEntryPayloadSize(
          static_cast<size_t>(size), &payload_size)) {
    CopyMuonPluginHelperError("Shared buffer size is too large", error);
    return 0;
  }

  const auto builder = CefSharedProcessMessageBuilder::Create(
      kMuonPluginResultSharedMessageName, payload_size);
  if (!builder || !builder->IsValid() || builder->Memory() == nullptr ||
      builder->Size() != payload_size) {
    CopyMuonPluginHelperError("Failed to allocate shared buffer", error);
    return 0;
  }

  auto allocation = std::make_unique<muon_shared_buffer>();
  allocation->builder = builder;
  allocation->data = static_cast<uint8_t*>(builder->Memory()) +
                     GetMuonSharedBufferSingleEntryDataOffset();
  allocation->size = static_cast<size_t>(size);
  allocation->message_name = kMuonPluginResultSharedMessageName;
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

static MuonLogLevel ConvertMuonPluginLogLevel(muon_log_level level) {
  switch (level) {
    case MUON_LOG_LEVEL_DEBUG:
      return kMuonLogLevelDebug;
    case MUON_LOG_LEVEL_INFO:
      return kMuonLogLevelInfo;
    case MUON_LOG_LEVEL_WARNING:
      return kMuonLogLevelWarning;
    case MUON_LOG_LEVEL_ERROR:
      return kMuonLogLevelError;
    case MUON_LOG_LEVEL_FATAL:
      return kMuonLogLevelFatal;
  }
  return kMuonLogLevelInfo;
}

static void LogMuonPluginMessage(muon_log_level level, const char* message) {
  LogMuonMessage(kMuonLogSourcePlugin, ConvertMuonPluginLogLevel(level),
                 message == nullptr ? std::string() : std::string(message));
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
  LogMuonMessage(kMuonLogSourceMuon, kMuonLogLevelError, error_message);
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

static void AddMuonPluginMetadataNamespaces(
    const muon_plugin_metadata* metadata,
    std::vector<std::string>* namespaces) {
  if (metadata == nullptr || namespaces == nullptr ||
      metadata->namespaces == nullptr) {
    return;
  }
  for (auto namespace_entry = metadata->namespaces;
       *namespace_entry != nullptr; ++namespace_entry) {
    const auto* plugin_namespace = (*namespace_entry)->plugin_namespace;
    if (plugin_namespace != nullptr) {
      namespaces->push_back(plugin_namespace);
    }
  }
}

static std::vector<std::string> GetMuonReservedPluginNamespaces() {
  std::vector<std::string> namespaces;
  AddMuonPluginMetadataNamespaces(GetMuonBuiltinPluginMetadata(), &namespaces);
  AddMuonPluginMetadataNamespaces(
      GetMuonBuiltinFsDialogsPluginMetadata(), &namespaces);
  namespaces.push_back(GetMuonBuiltinBrowserPluginNamespace());
  return namespaces;
}

static bool IsMuonReservedPluginNamespacePath(
    const std::string& namespace_path) {
  for (const auto& reserved_namespace : GetMuonReservedPluginNamespaces()) {
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
    if (IsMuonReservedPluginNamespacePath(plugin_namespace)) {
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
      IsMuonReservedPluginNamespacePath(public_path)) {
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
          LogMuonMessage(kMuonLogSourceMuon, kMuonLogLevelWarning,
                         "Skipping plugin function from " + path.string() +
                             ": " + error_message);
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
    const MuonPluginInvocationContext& context,
    int renderer_context_id) {
  return std::to_string(context.browser_id) + ":" + context.frame_id + ":" +
         std::to_string(renderer_context_id);
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

static CefRefPtr<CefDictionaryValue> CreateMuonEncodedPluginProxy(
    uint32_t proxy_id,
    const MuonTypeMetadata& function_type) {
  const auto encoded_function = CefDictionaryValue::Create();
  encoded_function->SetString(kMuonFunctionArgumentKindKey,
                              kMuonFunctionArgumentKindPluginProxy);
  encoded_function->SetInt(kMuonFunctionArgumentProxyIdKey,
                           static_cast<int>(proxy_id));
  encoded_function->SetString(kMuonFunctionArgumentTypeKey,
                              CreateMuonTypeCanonicalKey(function_type));
  encoded_function->SetDictionary("type",
                                  CreateMuonTypeMetadataDictionary(
                                      function_type));
  return encoded_function;
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

static void SendMuonRendererFunctionSourceLeaseMessage(
    const MuonRendererFunctionSource& source,
    const char* message_name) {
  if (!source.context_valid || !source.renderer_lease_active ||
      !source.context.frame || !source.context.frame->IsValid()) {
    return;
  }
  const auto message = CefProcessMessage::Create(message_name);
  const auto args = message->GetArgumentList();
  args->SetSize(3);
  args->SetInt(0, source.renderer_context_id);
  args->SetInt(1, source.function_id);
  args->SetString(2, source.lease_token);
  source.context.frame->SendProcessMessage(PID_RENDERER, message);
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
  }
  SendMuonRendererFunctionSourceLeaseMessage(
      *source, kMuonRendererFunctionSourceReleaseMessageName);
  source->renderer_lease_active = false;
  source->impl = nullptr;
  delete source;
}

static uint32_t RegisterMuonFunctionProxyForOwner(
    MuonPluginRuntimeImpl* impl,
    const std::string& owner_id,
    muon_native_function function,
    const MuonTypeMetadata& function_type) {
  if (impl == nullptr || function == nullptr ||
      function_type.type != MUON_TYPE_FUNCTION ||
      function_type.function_return_type.empty()) {
    return 0;
  }

  tra_ffic_error retain_error;
  if (!tra_ffic_function_retain(function, &retain_error)) {
    return 0;
  }

  const auto key = CreateMuonFunctionProxyKey(function, function_type);
  const auto existing = impl->proxy_ids_by_key.find(key);
  if (existing != impl->proxy_ids_by_key.end()) {
    auto& proxy = impl->proxies_by_id[existing->second];
    proxy.wrapper_lease_count += 1;
    impl->proxy_ids_by_owner[owner_id].push_back(proxy.id);
    return proxy.id;
  }

  auto signature_storage = CreateSharedSignature(
      function_type.function_arg_types, function_type.function_return_type[0]);
  tra_ffic_function_ref function_ref = {};
  std::string error_message;
  if (!CreateMuonTrafficFunctionRef(function, signature_storage,
                                     &function_ref, &error_message)) {
    (void)tra_ffic_function_release(function, &retain_error);
    return 0;
  }

  MuonFunctionProxy proxy;
  proxy.id = impl->next_proxy_id;
  impl->next_proxy_id += 1;
  proxy.function = function;
  proxy.function_type = function_type;
  proxy.signature_storage = std::move(signature_storage);
  proxy.function_ref = function_ref;
  proxy.wrapper_lease_count = 1;
  const auto proxy_id = proxy.id;
  impl->proxies_by_id[proxy_id] = proxy;
  impl->proxy_ids_by_key[key] = proxy_id;
  impl->proxy_ids_by_owner[owner_id].push_back(proxy_id);
  return proxy_id;
}

static void ReleaseMuonFunctionProxy(MuonPluginRuntimeImpl* impl,
                                      uint32_t proxy_id) {
  muon_native_function function = nullptr;
  const auto proxy_iterator = impl->proxies_by_id.find(proxy_id);
  if (proxy_iterator == impl->proxies_by_id.end()) {
    return;
  }
  auto& proxy = proxy_iterator->second;
  function = proxy.function;
  if (proxy.wrapper_lease_count > 1) {
    proxy.wrapper_lease_count -= 1;
  } else {
    impl->proxy_ids_by_key.erase(
        CreateMuonFunctionProxyKey(proxy.function, proxy.function_type));
    impl->proxies_by_id.erase(proxy_iterator);
  }

  tra_ffic_error error;
  (void)tra_ffic_function_release(function, &error);
}

static bool TryGetMuonFunctionProxy(MuonPluginRuntimeImpl* impl,
                                     uint32_t proxy_id,
                                     MuonFunctionProxy* proxy) {
  const auto proxy_iterator = impl->proxies_by_id.find(proxy_id);
  if (proxy_iterator == impl->proxies_by_id.end()) {
    return false;
  }
  if (proxy != nullptr) {
    *proxy = proxy_iterator->second;
  }
  return true;
}

static bool CopyMuonTrafficValueToPluginValue(
    MuonPluginRuntimeImpl* impl,
    int call_id,
    int renderer_context_id,
    const tra_ffic_value& source,
    const MuonTypeMetadata& expected_type,
    MuonPluginCallResult* call_result,
    std::string* error_message) {
  if (call_result == nullptr) {
    *error_message = "Plugin result storage is unavailable";
    return false;
  }
  auto* target = &call_result->value;
  auto source_type = MUON_TYPE_VOID;
  if (!ConvertTrafficValueTypeToMuon(source.kind, &source_type) ||
      source_type != expected_type.type) {
    *error_message = "Plugin returned an unexpected result type";
    return false;
  }
  target->type = expected_type.type;
  target->function_type = expected_type;
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
      target->pointer_value = source.as.pointer_value;
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
      target->function_value = source.as.function_value;
      return true;
    case MUON_TYPE_BUFFER_VIEW: {
      const auto& view = source.as.buffer_view_value;
      if (view.data == nullptr && view.size != 0) {
        *error_message = "Plugin returned an invalid buffer_view";
        return false;
      }
      target->buffer_view.data = view.data;
      target->buffer_view.size = view.size;
      const auto sources = std::vector<MuonSharedBufferSource>{
          {3, view.data, static_cast<size_t>(view.size)},
      };
      if (!CreateMuonRuntimeSharedBufferMessage(
              impl, kMuonPluginResultSharedMessageName, call_id,
              renderer_context_id, sources,
              &call_result->shared_buffer_message, error_message)) {
        return false;
      }
      call_result->has_shared_buffer_message = true;
      return true;
    }
    default:
      *error_message = "Unsupported result type";
      return false;
  }
}

static void HandleMuonTrafficCallResult(void* user_data,
                                         const tra_ffic_result* result) {
  CEF_REQUIRE_UI_THREAD();
  std::unique_ptr<MuonTrafficCallState> state(
      static_cast<MuonTrafficCallState*>(user_data));
  if (!state || !state->completion) {
    return;
  }

  MuonPluginCallResult call_result;
  if (result == nullptr) {
    call_result.success = false;
    call_result.error_message = "muon plugin call did not produce a result";
  } else if (!result->success) {
    call_result.success = false;
    call_result.error_message = result->error_message;
  } else {
    call_result.success = CopyMuonTrafficValueToPluginValue(
        state->impl, state->call_id, state->renderer_context_id,
        result->value, state->return_type, &call_result,
        &call_result.error_message);
  }

  auto completion = std::move(state->completion);
  state->decoded_args.ResetRendererFunctionBorrows();
  completion(call_result);
}

static bool SetMuonEncodedValue(
    CefRefPtr<CefListValue> list,
    size_t index,
    const MuonPluginValue& value,
    const std::vector<MuonSharedBufferEntry>& shared_entries,
    std::string* error_message) {
  switch (value.type) {
    case MUON_TYPE_VOID:
      list->SetNull(index);
      return true;
    case MUON_TYPE_BOOL:
      list->SetBool(index, value.bool_value);
      return true;
    case MUON_TYPE_I8:
      list->SetInt(index, value.i8_value);
      return true;
    case MUON_TYPE_U8:
      list->SetInt(index, value.u8_value);
      return true;
    case MUON_TYPE_I16:
      list->SetInt(index, value.i16_value);
      return true;
    case MUON_TYPE_U16:
      list->SetInt(index, value.u16_value);
      return true;
    case MUON_TYPE_I32:
      list->SetInt(index, value.i32_value);
      return true;
    case MUON_TYPE_U32:
      list->SetDouble(index, static_cast<double>(value.u32_value));
      return true;
    case MUON_TYPE_I64:
      list->SetString(index, std::to_string(value.i64_value));
      return true;
    case MUON_TYPE_U64:
      list->SetString(index, std::to_string(value.u64_value));
      return true;
    case MUON_TYPE_F32:
      list->SetDouble(index, static_cast<double>(value.f32_value));
      return true;
    case MUON_TYPE_F64:
      list->SetDouble(index, value.f64_value);
      return true;
    case MUON_TYPE_POINTER:
      list->SetDouble(
          index,
          static_cast<double>(reinterpret_cast<uintptr_t>(
              value.pointer_value)));
      return true;
    case MUON_TYPE_STRING:
      if (value.is_null) {
        list->SetNull(index);
        return true;
      }
      list->SetString(index, value.string_value);
      return true;
    case MUON_TYPE_FUNCTION:
      if (value.is_null) {
        list->SetNull(index);
        return true;
      }
      list->SetDictionary(
          index, CreateMuonEncodedPluginProxy(value.function_proxy_id,
                                               value.function_type));
      return true;
    case MUON_TYPE_BUFFER_VIEW: {
      MuonSharedBufferEntry entry;
      if (!FindMuonSharedBufferEntry(shared_entries, index, &entry)) {
        *error_message = "Missing shared buffer payload entry";
        return false;
      }
      list->SetDictionary(index, CreateMuonSharedBufferPlaceholder(entry));
      return true;
    }
    default:
      list->SetNull(index);
      *error_message = "Unsupported encoded value type";
      return false;
  }
  return true;
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
  if (source != nullptr && source->context_valid && source->context.frame &&
      source->context.frame->IsValid()) {
    const auto message =
        CefProcessMessage::Create(kMuonRendererFunctionResultConsumedMessageName);
    const auto args = message->GetArgumentList();
    args->SetSize(2);
    args->SetInt(0, source->renderer_context_id);
    args->SetInt(1, static_cast<int>(call_id));
    source->context.frame->SendProcessMessage(PID_RENDERER, message);
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
    MuonPluginValue* value,
    std::string* error_message) {
  auto raw_type = MUON_TYPE_VOID;
  if (!ConvertTrafficValueTypeToMuon(raw_value.kind, &raw_type) ||
      raw_type != expected_type.type) {
    *error_message = "Function argument type mismatch";
    return false;
  }
  value->type = expected_type.type;
  value->function_type = expected_type;
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
      value->pointer_value = raw_value.as.pointer_value;
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
      value->function_proxy_id = RegisterMuonFunctionProxyForOwner(
          impl, source.owner_id, raw_value.as.function_value, expected_type);
      if (value->function_proxy_id == 0) {
        *error_message = "Failed to register nested function proxy";
        return false;
      }
      return true;
    case MUON_TYPE_BUFFER_VIEW:
      if (raw_value.as.buffer_view_value.data == nullptr &&
          raw_value.as.buffer_view_value.size != 0) {
        *error_message = "Function argument buffer_view is invalid";
        return false;
      }
      value->buffer_view.data = raw_value.as.buffer_view_value.data;
      value->buffer_view.size = raw_value.as.buffer_view_value.size;
      return true;
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

  std::vector<MuonPluginValue> encoded_values(arg_count);
  std::string error_message;
  for (auto index = size_t{0}; index < arg_count; ++index) {
    if (!CopyMuonTrafficArgumentForRenderer(
            source->impl, *source, source->function_type.function_arg_types[index],
            args[index], &encoded_values[index], &error_message)) {
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

  auto call_id = uint32_t{0};
  call_id = source->impl->next_renderer_call_id;
  source->impl->next_renderer_call_id += 1;
  MuonPendingRendererFunctionCall pending_call;
  pending_call.completion = completion;
  pending_call.source = source;
  pending_call.source_borrow = std::move(source_borrow);
  source->impl->pending_renderer_function_calls.emplace(
      call_id, std::move(pending_call));

  std::vector<MuonSharedBufferSource> shared_sources;
  for (auto index = size_t{0}; index < encoded_values.size(); ++index) {
    if (encoded_values[index].type == MUON_TYPE_BUFFER_VIEW) {
      shared_sources.push_back(
          {index, encoded_values[index].buffer_view.data,
           static_cast<size_t>(encoded_values[index].buffer_view.size)});
    }
  }
  MuonCreatedSharedBufferMessage shared_message;
  if (!shared_sources.empty() &&
      !CreateMuonRuntimeSharedBufferMessage(
          source->impl, kMuonRendererFunctionCallSharedMessageName,
          static_cast<int>(call_id), source->renderer_context_id,
          shared_sources, &shared_message, &error_message)) {
    MuonPendingRendererFunctionCall pending_call;
    const auto pending_iterator =
        source->impl->pending_renderer_function_calls.find(call_id);
    if (pending_iterator !=
        source->impl->pending_renderer_function_calls.end()) {
      pending_call = std::move(pending_iterator->second);
      source->impl->pending_renderer_function_calls.erase(pending_iterator);
    }
    CompleteMuonRendererFunctionWithError(pending_call.completion,
                                           error_message);
    return;
  }

  const auto task_posted = CefPostTask(
      TID_UI, new MuonFunctionTask([impl = source->impl, call_id,
                                     encoded_values, shared_message]() {
    auto pending_iterator =
        impl->pending_renderer_function_calls.find(call_id);
    if (pending_iterator == impl->pending_renderer_function_calls.end()) {
      return;
    }
    auto* source = pending_iterator->second.source;
    if (source == nullptr || !source->context_valid ||
        !source->context.frame || !source->context.frame->IsValid()) {
      MuonPendingRendererFunctionCall pending_call;
      pending_call = std::move(pending_iterator->second);
      impl->pending_renderer_function_calls.erase(pending_iterator);
      CompleteMuonRendererFunctionWithError(
          pending_call.completion, "Renderer frame is unavailable");
      return;
    }

    const auto message =
        CefProcessMessage::Create(kMuonRendererFunctionCallMessageName);
    const auto message_args = message->GetArgumentList();
    const auto encoded_args = CefListValue::Create();
    encoded_args->SetSize(encoded_values.size());
    std::string encode_error_message;
    for (auto index = size_t{0}; index < encoded_values.size(); ++index) {
      if (!SetMuonEncodedValue(encoded_args, index, encoded_values[index],
                                shared_message.entries,
                                &encode_error_message)) {
        MuonPendingRendererFunctionCall pending_call;
        const auto pending_iterator =
            impl->pending_renderer_function_calls.find(call_id);
        if (pending_iterator ==
            impl->pending_renderer_function_calls.end()) {
          return;
        }
        pending_call = std::move(pending_iterator->second);
        impl->pending_renderer_function_calls.erase(pending_iterator);
        CompleteMuonRendererFunctionWithError(pending_call.completion,
                                               encode_error_message);
        return;
      }
    }

    message_args->SetSize(5);
    message_args->SetInt(0, static_cast<int>(call_id));
    message_args->SetInt(1, source->renderer_context_id);
    message_args->SetInt(2, source->function_id);
    message_args->SetList(3, encoded_args);
    message_args->SetDictionary(4, CreateMuonTypeMetadataDictionary(
                                       source->function_type));
    if (shared_message.message) {
      source->context.frame->SendProcessMessage(PID_RENDERER,
                                                shared_message.message);
    }
    source->context.frame->SendProcessMessage(PID_RENDERER, message);
  }));
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

static bool GetMuonNumericListValue(CefRefPtr<CefListValue> list,
                                     size_t index,
                                     double* value) {
  const auto type = list->GetType(index);
  if (type == VTYPE_INT) {
    *value = static_cast<double>(list->GetInt(index));
    return true;
  }
  if (type == VTYPE_DOUBLE) {
    *value = list->GetDouble(index);
    return true;
  }
  return false;
}

static bool ConvertMuonNumberToPointer(double source, void** value) {
  if (value == nullptr || !std::isfinite(source) ||
      std::trunc(source) != source || source < 0.0 ||
      source >=
          std::ldexp(1.0, std::numeric_limits<uintptr_t>::digits)) {
    return false;
  }
  *value = reinterpret_cast<void*>(static_cast<uintptr_t>(source));
  return true;
}

static bool GetMuonPointerListValue(CefRefPtr<CefListValue> list,
                                     size_t index,
                                     void** value) {
  if (list->GetType(index) == VTYPE_NULL) {
    *value = nullptr;
    return true;
  }
  auto number = 0.0;
  return GetMuonNumericListValue(list, index, &number) &&
         ConvertMuonNumberToPointer(number, value);
}

static bool ParseMuonInt64(const std::string& source, int64_t* value) {
  if (value == nullptr || source.empty()) {
    return false;
  }
  auto parsed = int64_t{0};
  const auto begin = source.data();
  const auto end = begin + source.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc() || result.ptr != end) {
    return false;
  }
  *value = parsed;
  return true;
}

static bool ParseMuonUInt64(const std::string& source, uint64_t* value) {
  if (value == nullptr || source.empty()) {
    return false;
  }
  auto parsed = uint64_t{0};
  const auto begin = source.data();
  const auto end = begin + source.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc() || result.ptr != end) {
    return false;
  }
  *value = parsed;
  return true;
}

static bool GetOrCreateMuonRendererFunction(
    MuonPluginRuntimeImpl* impl,
    const MuonPluginInvocationContext& context,
    int renderer_context_id,
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

  const auto owner_id = CreateMuonFunctionOwnerId(context, renderer_context_id);
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
        source->active_bridge_borrows += 1;
        *function = source->function;
        *borrow = MuonRendererFunctionBorrow(source);
        return true;
      }
    }
  }

  if (!context.frame || !context.frame->IsValid()) {
    *error_message = "Renderer function frame is unavailable";
    return false;
  }

  auto source = std::make_unique<MuonRendererFunctionSource>();
  source->impl = impl;
  source->context = context;
  source->context.renderer_context_id = renderer_context_id;
  source->owner_id = owner_id;
  source->source_id = source_id;
  source->lease_token =
      std::to_string(impl->next_renderer_source_lease_token);
  impl->next_renderer_source_lease_token += 1;
  source->renderer_context_id = renderer_context_id;
  source->function_id = function_id;
  source->function_type = function_type;

  const auto signature_storage = CreateSharedSignature(
      function_type.function_arg_types, function_type.function_return_type[0]);
  tra_ffic_error error;
  muon_native_function created_function = nullptr;
  if (!tra_ffic_side_create_raw_closure(
          &impl->renderer_side,
          GetMuonFunctionSignature(signature_storage.get()),
          InvokeMuonRendererFunctionClosure, source.get(),
          DestroyMuonRendererFunctionSource, &created_function, &error)) {
    *error_message = GetMuonTrafficError(error);
    return false;
  }
  source->function = created_function;
  source->active_bridge_borrows = 1;
  source->bridge_retain_active = true;
  source->renderer_lease_active = true;
  auto* borrowed_source = source.get();
  impl->renderer_functions_by_source[source_id] = borrowed_source;
  impl->renderer_sources_by_owner[owner_id].insert(borrowed_source);
  SendMuonRendererFunctionSourceLeaseMessage(
      *borrowed_source, kMuonRendererFunctionSourceAcquireMessageName);

  (void)source.release();
  *function = created_function;
  *borrow = MuonRendererFunctionBorrow(borrowed_source);
  return true;
}

static bool DecodeMuonPluginArguments(
    MuonPluginRuntimeImpl* impl,
    const MuonPluginInvocationContext& context,
    const std::vector<MuonTypeMetadata>& arg_types,
    CefRefPtr<CefListValue> encoded_args,
    std::shared_ptr<MuonSharedBufferPayload> shared_payload,
    MuonDecodedArguments* decoded_args,
    std::string* error_message) {
  decoded_args->ResetRendererFunctionBorrows();
  if (!encoded_args) {
    *error_message = "Missing argument list";
    return false;
  }
  if (encoded_args->GetSize() != arg_types.size()) {
    *error_message = "Invalid argument count";
    return false;
  }

  decoded_args->values.clear();
  decoded_args->string_storage.clear();
  decoded_args->shared_payload = std::move(shared_payload);
  decoded_args->values.resize(arg_types.size());
  decoded_args->string_storage.reserve(arg_types.size());
  decoded_args->renderer_function_borrows.reserve(arg_types.size());
  for (auto index = size_t{0}; index < arg_types.size(); ++index) {
    const auto& expected_type = arg_types[index];
    auto& target = decoded_args->values[index];
    if (!ConvertMuonValueTypeToTraffic(expected_type.type, &target.kind)) {
      *error_message = "Unsupported argument type";
      return false;
    }
    switch (expected_type.type) {
      case MUON_TYPE_BOOL:
        if (encoded_args->GetType(index) != VTYPE_BOOL) {
          *error_message = "Invalid bool argument";
          return false;
        }
        target = tra_ffic_value_bool(encoded_args->GetBool(index));
        break;
      case MUON_TYPE_I8: {
        auto value = 0.0;
        if (!GetMuonNumericListValue(encoded_args, index, &value) ||
            !std::isfinite(value) || std::trunc(value) != value ||
            value < static_cast<double>(std::numeric_limits<int8_t>::min()) ||
            value > static_cast<double>(std::numeric_limits<int8_t>::max())) {
          *error_message = "Invalid i8 argument";
          return false;
        }
        target = tra_ffic_value_int8(static_cast<int8_t>(value));
        break;
      }
      case MUON_TYPE_U8: {
        auto value = 0.0;
        if (!GetMuonNumericListValue(encoded_args, index, &value) ||
            !std::isfinite(value) || std::trunc(value) != value ||
            value < 0.0 ||
            value > static_cast<double>(std::numeric_limits<uint8_t>::max())) {
          *error_message = "Invalid u8 argument";
          return false;
        }
        target = tra_ffic_value_uint8(static_cast<uint8_t>(value));
        break;
      }
      case MUON_TYPE_I16: {
        auto value = 0.0;
        if (!GetMuonNumericListValue(encoded_args, index, &value) ||
            !std::isfinite(value) || std::trunc(value) != value ||
            value < static_cast<double>(std::numeric_limits<int16_t>::min()) ||
            value > static_cast<double>(std::numeric_limits<int16_t>::max())) {
          *error_message = "Invalid i16 argument";
          return false;
        }
        target = tra_ffic_value_int16(static_cast<int16_t>(value));
        break;
      }
      case MUON_TYPE_U16: {
        auto value = 0.0;
        if (!GetMuonNumericListValue(encoded_args, index, &value) ||
            !std::isfinite(value) || std::trunc(value) != value ||
            value < 0.0 ||
            value > static_cast<double>(std::numeric_limits<uint16_t>::max())) {
          *error_message = "Invalid u16 argument";
          return false;
        }
        target = tra_ffic_value_uint16(static_cast<uint16_t>(value));
        break;
      }
      case MUON_TYPE_I32: {
        auto value = 0.0;
        if (!GetMuonNumericListValue(encoded_args, index, &value) ||
            !std::isfinite(value) || std::trunc(value) != value ||
            value < static_cast<double>(INT32_MIN) ||
            value > static_cast<double>(INT32_MAX)) {
          *error_message = "Invalid i32 argument";
          return false;
        }
        target = tra_ffic_value_int32(static_cast<int32_t>(value));
        break;
      }
      case MUON_TYPE_U32: {
        auto value = 0.0;
        if (!GetMuonNumericListValue(encoded_args, index, &value) ||
            !std::isfinite(value) || std::trunc(value) != value ||
            value < 0.0 ||
            value > static_cast<double>(UINT32_MAX)) {
          *error_message = "Invalid u32 argument";
          return false;
        }
        target = tra_ffic_value_uint32(static_cast<uint32_t>(value));
        break;
      }
      case MUON_TYPE_I64: {
        if (encoded_args->GetType(index) != VTYPE_STRING) {
          *error_message = "Invalid i64 argument";
          return false;
        }
        auto value = int64_t{0};
        if (!ParseMuonInt64(encoded_args->GetString(index).ToString(),
                             &value)) {
          *error_message = "Invalid i64 argument";
          return false;
        }
        target = tra_ffic_value_int64(value);
        break;
      }
      case MUON_TYPE_U64: {
        if (encoded_args->GetType(index) != VTYPE_STRING) {
          *error_message = "Invalid u64 argument";
          return false;
        }
        auto value = uint64_t{0};
        if (!ParseMuonUInt64(encoded_args->GetString(index).ToString(),
                              &value)) {
          *error_message = "Invalid u64 argument";
          return false;
        }
        target = tra_ffic_value_uint64(value);
        break;
      }
      case MUON_TYPE_F32: {
        auto value = 0.0;
        if (!GetMuonNumericListValue(encoded_args, index, &value) ||
            !std::isfinite(value) ||
            value < -static_cast<double>(std::numeric_limits<float>::max()) ||
            value > static_cast<double>(std::numeric_limits<float>::max())) {
          *error_message = "Invalid f32 argument";
          return false;
        }
        target = tra_ffic_value_float(static_cast<float>(value));
        break;
      }
      case MUON_TYPE_F64: {
        auto value = 0.0;
        if (!GetMuonNumericListValue(encoded_args, index, &value) ||
            !std::isfinite(value)) {
          *error_message = "Invalid f64 argument";
          return false;
        }
        target = tra_ffic_value_double(value);
        break;
      }
      case MUON_TYPE_POINTER: {
        void* value = nullptr;
        if (!GetMuonPointerListValue(encoded_args, index, &value)) {
          *error_message = "Invalid pointer argument";
          return false;
        }
        target = tra_ffic_value_pointer(value);
        break;
      }
      case MUON_TYPE_STRING:
        if (encoded_args->GetType(index) == VTYPE_NULL) {
          target = tra_ffic_value_string(nullptr);
          break;
        }
        if (encoded_args->GetType(index) != VTYPE_STRING) {
          *error_message = "Invalid string argument";
          return false;
        }
        decoded_args->string_storage.push_back(
            encoded_args->GetString(index).ToString());
        target = tra_ffic_value_string(
            decoded_args->string_storage.back().c_str());
        break;
      case MUON_TYPE_BUFFER_VIEW: {
        if (encoded_args->GetType(index) != VTYPE_DICTIONARY ||
            !decoded_args->shared_payload) {
          *error_message = "Invalid buffer_view argument";
          return false;
        }
        MuonSharedBufferEntry placeholder;
        if (!ReadMuonSharedBufferPlaceholder(
                encoded_args->GetDictionary(index), &placeholder) ||
            placeholder.value_index != index) {
          *error_message = "Invalid buffer_view argument";
          return false;
        }
        MuonSharedBufferEntry entry;
        if (!FindMuonSharedBufferEntry(*decoded_args->shared_payload, index,
                                        &entry) ||
            entry.offset != placeholder.offset ||
            entry.size != placeholder.size) {
          *error_message = "Buffer_view shared payload is missing";
          return false;
        }
        target = tra_ffic_value_buffer_view(
            GetMuonSharedBufferEntryData(*decoded_args->shared_payload, entry),
            static_cast<uintptr_t>(entry.size));
        if (entry.size > 0 && target.as.buffer_view_value.data == nullptr) {
          *error_message = "Buffer_view shared payload is invalid";
          return false;
        }
        break;
      }
      case MUON_TYPE_FUNCTION: {
        if (encoded_args->GetType(index) == VTYPE_NULL) {
          target = tra_ffic_value_function(nullptr);
          break;
        }
        if (encoded_args->GetType(index) != VTYPE_DICTIONARY) {
          *error_message = "Invalid function argument";
          return false;
        }
        const auto encoded_function = encoded_args->GetDictionary(index);
        if (!encoded_function) {
          *error_message = "Invalid function argument";
          return false;
        }

        muon_native_function function = nullptr;
        if (encoded_function->HasKey(kMuonFunctionArgumentProxyIdKey) ||
            (encoded_function->HasKey(kMuonFunctionArgumentKindKey) &&
             encoded_function->GetString(kMuonFunctionArgumentKindKey)
                     .ToString() ==
                 kMuonFunctionArgumentKindPluginProxy)) {
          const auto proxy_id = static_cast<uint32_t>(
              encoded_function->GetInt(kMuonFunctionArgumentProxyIdKey));
          MuonFunctionProxy proxy;
          if (!TryGetMuonFunctionProxy(impl, proxy_id, &proxy)) {
            *error_message = "Unknown plugin function proxy";
            return false;
          }
          if (!AreEqualMuonTypes(proxy.function_type, expected_type)) {
            *error_message = "Plugin function proxy type mismatch";
            return false;
          }
          function = proxy.function;
        } else {
          if (!encoded_function->HasKey(kMuonFunctionArgumentContextIdKey) ||
              !encoded_function->HasKey(kMuonFunctionArgumentFunctionIdKey)) {
            *error_message = "Invalid function argument";
            return false;
          }
          const auto renderer_context_id =
              encoded_function->GetInt(kMuonFunctionArgumentContextIdKey);
          const auto function_id =
              encoded_function->GetInt(kMuonFunctionArgumentFunctionIdKey);
          MuonRendererFunctionBorrow renderer_function_borrow;
          if (!GetOrCreateMuonRendererFunction(
                  impl, context, renderer_context_id, function_id,
                  expected_type, &function, &renderer_function_borrow,
                  error_message)) {
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
    int call_id,
    int renderer_context_id,
    MuonDecodedArguments decoded_args,
    MuonPluginRuntime::Completion completion) {
  auto* state = new MuonTrafficCallState;
  state->completion = std::move(completion);
  state->impl = impl;
  state->return_type = return_type;
  state->call_id = call_id;
  state->renderer_context_id = renderer_context_id;
  state->decoded_args = std::move(decoded_args);

  tra_ffic_error error;
  if (!tra_ffic_call_with_result(
          caller_side, &function_ref, state->decoded_args.values.data(),
          static_cast<uint32_t>(state->decoded_args.values.size()),
          HandleMuonTrafficCallResult, state, &error)) {
    MuonPluginCallResult result;
    result.success = false;
    result.error_message = GetMuonTrafficError(error);
    auto call_completion = std::move(state->completion);
    state->decoded_args.ResetRendererFunctionBorrows();
    delete state;
    call_completion(result);
  }
}

static void KeepOrCloseMuonPluginLibrary(MuonPluginRuntimeImpl* impl,
                                          const std::filesystem::path& path,
                                          void* handle,
                                          size_t initial_function_count) {
  if (impl != nullptr &&
      impl->registered_functions.size() > initial_function_count) {
    MuonDynamicLibrary library;
    library.path = path;
    library.handle = handle;
    impl->libraries.push_back(library);
    const auto cancel_owner =
        GetMuonFsDialogsCancelOwnerBrowserFunction(handle);
    if (cancel_owner != nullptr) {
      impl->fs_dialogs_cancel_owner_functions.push_back(cancel_owner);
    }
    return;
  }
  CloseMuonDynamicLibrary(handle);
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
        LogMuonMessage(kMuonLogSourceMuon, kMuonLogLevelWarning,
                       "Skipping plugin function " + js_name + ": " +
                           error_message);
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
        LogMuonMessage(kMuonLogSourceMuon, kMuonLogLevelWarning,
                       "Skipping plugin function " + js_name + ": " +
                           GetMuonTrafficError(error));
        continue;
      }
      if (!CreateMuonTrafficFunctionRef(
              registered_function->function,
              registered_function->signature_storage,
              &registered_function->function_ref, &error_message)) {
        LogMuonMessage(kMuonLogSourceMuon, kMuonLogLevelWarning,
                       "Skipping plugin function " + js_name + ": " +
                           error_message);
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

static bool RegisterMuonBuiltinBrowserFunctions(
    MuonPluginRuntimeImpl* impl,
    const MuonPluginPolicy& plugin_policy) {
  if (impl == nullptr) {
    return false;
  }

  const std::string plugin_namespace(GetMuonBuiltinBrowserPluginNamespace());
  std::vector<std::string> namespace_segments;
  if (!SplitMuonPluginNamespace(plugin_namespace, &namespace_segments)) {
    return FailMuonPluginStartup(
        impl, "Built-in browser namespace is invalid: " + plugin_namespace);
  }
  const auto namespace_paths = CreateMuonNamespacePaths(namespace_segments);

  std::set<std::string> local_function_paths;
  std::vector<const MuonBuiltinBrowserFunctionDefinition*> allowed_functions;
  std::vector<std::string> allowed_function_names;
  for (const auto& definition : GetMuonBuiltinBrowserFunctionDefinitions()) {
    if (definition.js_name == nullptr ||
        !IsValidMuonJsIdentifier(definition.js_name) ||
        (definition.filter_name != nullptr &&
         !IsValidMuonJsIdentifier(definition.filter_name)) ||
        (definition.arg_count > 0 && definition.arg_types == nullptr) ||
        definition.kind == MuonBuiltinBrowserFunctionKind::None) {
      return FailMuonPluginStartup(
          impl, "Built-in browser function metadata is invalid");
    }

    const std::string js_name(definition.js_name);
    const auto filter_name = definition.filter_name == nullptr
                                 ? js_name
                                 : std::string(definition.filter_name);
    const auto public_path =
        CreateMuonFunctionPublicPath(plugin_namespace, filter_name);
    if (!IsMuonPluginFunctionAllowed(plugin_policy, public_path)) {
      continue;
    }
    const auto native_path =
        CreateMuonFunctionPublicPath(plugin_namespace, js_name);
    if (local_function_paths.find(public_path) !=
        local_function_paths.end()) {
      return FailMuonPluginStartup(
          impl, "Duplicate plugin function path: " + public_path);
    }
    if (native_path != public_path &&
        local_function_paths.find(native_path) != local_function_paths.end()) {
      return FailMuonPluginStartup(
          impl, "Duplicate plugin function path: " + native_path);
    }
    if (!ValidateMuonPluginFunctionPath(impl, public_path, true)) {
      return false;
    }
    if (native_path != public_path &&
        !ValidateMuonPluginFunctionPath(impl, native_path, true)) {
      return false;
    }
    local_function_paths.insert(public_path);
    if (native_path != public_path) {
      local_function_paths.insert(native_path);
    }
    allowed_functions.push_back(&definition);
    allowed_function_names.push_back(filter_name);
  }

  if (allowed_functions.empty()) {
    return true;
  }
  if (!ValidateMuonPluginNamespaceRegistration(
          impl, plugin_namespace, namespace_paths, true)) {
    return false;
  }

  impl->plugin_namespaces.insert(plugin_namespace);
  impl->namespace_paths.insert(namespace_paths.begin(), namespace_paths.end());
  impl->renderer_namespaces.push_back(
      {plugin_namespace, GetMuonBuiltinBrowserSetupScript(),
       allowed_function_names});

  for (const auto* definition : allowed_functions) {
    const std::string js_name(definition->js_name);
    const auto filter_name = definition->filter_name == nullptr
                                 ? js_name
                                 : std::string(definition->filter_name);
    const auto id = AllocateMuonFunctionId(impl);
    MuonFunctionMetadata function;
    function.id = id;
    function.plugin_namespace = plugin_namespace;
    function.js_name = js_name;
    function.public_name = filter_name;
    if (definition->arg_types != nullptr && definition->arg_count > 0) {
      function.arg_types.assign(definition->arg_types,
                                definition->arg_types + definition->arg_count);
    }
    function.return_type = definition->return_type;
    impl->renderer_functions.push_back(std::move(function));
    impl->function_paths[CreateMuonFunctionPublicPath(plugin_namespace,
                                                       filter_name)] = id;
    const auto native_path =
        CreateMuonFunctionPublicPath(plugin_namespace, js_name);
    if (native_path !=
        CreateMuonFunctionPublicPath(plugin_namespace, filter_name)) {
      impl->function_paths[native_path] = id;
    }
    impl->builtin_browser_functions_by_id[id] = definition->kind;
  }
  return true;
}

static bool LoadMuonPluginLibrary(MuonPluginRuntimeImpl* impl,
                                   const std::filesystem::path& path,
                                   const MuonPluginRuntimeLoadEntry& plugin,
                                   const MuonPluginPolicy& plugin_policy) {
  std::error_code filesystem_error;
  if (!std::filesystem::exists(path, filesystem_error) || filesystem_error ||
      !std::filesystem::is_regular_file(path, filesystem_error) ||
      filesystem_error) {
    return FailMuonPluginStartup(
        impl, "Plugin file not found: " + path.string());
  }

  if (plugin.has_expected_signature) {
    if (!plugin.has_signature_salt) {
      return FailMuonPluginStartup(
          impl, "Plugin signature requires plugin salt: " + plugin.plugin);
    }
    std::string actual_signature;
    if (!muon_internal::CalculateFileSha1Hex(
            path, plugin.signature_salt, &actual_signature)) {
      return FailMuonPluginStartup(
          impl, "Failed to calculate plugin signature: " + path.string());
    }
    if (actual_signature != plugin.expected_signature) {
      return FailMuonPluginStartup(
          impl, "Plugin signature mismatch: " + path.string() + " expected " +
                    plugin.expected_signature + " actual " +
                    actual_signature);
    }
  }

  auto* handle = OpenMuonDynamicLibrary(path);
  if (handle == nullptr) {
    return FailMuonPluginStartup(impl, "Failed to load plugin: " + path.string());
  }
  const auto initial_function_count = impl->registered_functions.size();

  const auto init_plugin = GetMuonPluginInitFunction(handle);
  if (init_plugin == nullptr) {
    const auto error_message =
        "Plugin is missing " + std::string(kMuonPluginEntryPoint) +
        ": " + path.string();
    CloseMuonDynamicLibrary(handle);
    return FailMuonPluginStartup(impl, error_message);
  }

  std::vector<muon_plugin_config_entry> config_entries;
  const auto init_context =
      CreateMuonPluginInitContext(plugin, &kMuonPluginHelpers, &config_entries);
  const auto* metadata = init_plugin(&init_context);
  if (metadata == nullptr) {
    const auto error_message = "Plugin declined loading: " + path.string();
    CloseMuonDynamicLibrary(handle);
    return FailMuonPluginStartup(impl, error_message);
  }
  if (!RegisterMuonPluginMetadata(impl, *metadata, path.string(),
                                  plugin_policy, false)) {
    CloseMuonDynamicLibrary(handle);
    return false;
  }
  if (impl->registered_functions.size() == initial_function_count) {
    const auto error_message =
        "Plugin registered no allowed functions: " + path.string();
    CloseMuonDynamicLibrary(handle);
    return FailMuonPluginStartup(impl, error_message);
  }

  KeepOrCloseMuonPluginLibrary(impl, path, handle, initial_function_count);
  return true;
}

static void ShutdownMuonBuiltinPlugins() {
  ShutdownMuonBuiltinExecutor();
  ShutdownMuonBuiltinFsDialogs();
  ShutdownMuonBuiltinFs();
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

  std::vector<muon_plugin_config_entry> config_entries;
  const auto init_context =
      CreateMuonPluginInitContext(plugin, &kMuonPluginHelpers, &config_entries);
  std::string error_message;
  if (!InitializeMuonBuiltinFs(&init_context, &error_message)) {
    return FailMuonPluginStartup(
        impl, "Built-in filesystem plugin failed: " + error_message);
  }
  if (!InitializeMuonBuiltinFsDialogs(&init_context, &error_message)) {
    ShutdownMuonBuiltinPlugins();
    return FailMuonPluginStartup(
        impl,
        "Built-in filesystem dialogs plugin failed: " + error_message);
  }
  if (!InitializeMuonBuiltinExecutor(&init_context,
                                     impl->main_dispatcher,
                                     &error_message)) {
    ShutdownMuonBuiltinPlugins();
    return FailMuonPluginStartup(
        impl, "Built-in executor plugin failed: " + error_message);
  }
  impl->fs_dialogs_cancel_owner_functions.push_back(
      &muon_builtin_fs_dialogs_cancel_owner_browser);
  if (!RegisterMuonPluginMetadata(
          impl, *GetMuonBuiltinPluginMetadata(), "<builtin muon>",
          plugin_policy, true)) {
    ShutdownMuonBuiltinPlugins();
    return false;
  }
  if (!RegisterMuonPluginMetadata(
          impl,
          *GetMuonBuiltinFsDialogsPluginMetadata(),
          "<builtin muon fs dialogs>",
          plugin_policy,
          true)) {
    ShutdownMuonBuiltinPlugins();
    return false;
  }
  if (!RegisterMuonBuiltinBrowserFunctions(impl, plugin_policy)) {
    ShutdownMuonBuiltinPlugins();
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
    const auto path =
        ResolveMuonPluginLibraryPath(impl->plugin_directory, plugin.plugin);
    if (!LoadMuonPluginLibrary(impl, path, plugin, *plugin.plugin_policy)) {
      return false;
    }
  }
  return true;
}

std::filesystem::path ResolveMuonPluginDirectory(
    const std::filesystem::path& plugin_path) {
  if (plugin_path.is_absolute()) {
    return plugin_path.lexically_normal();
  }
  return (GetMuonExecutableDirectory() / plugin_path).lexically_normal();
}

std::shared_ptr<MuonPluginRuntime> CreateMuonPluginRuntime(
    std::filesystem::path plugin_path,
    std::vector<MuonPluginRuntimeLoadEntry> plugins) {
  return std::make_shared<MuonPluginRuntime>(ResolveMuonPluginDirectory(
                                                 plugin_path),
                                             std::move(plugins));
}

MuonPluginRuntime::MuonPluginRuntime(
    std::filesystem::path plugin_directory,
    std::vector<MuonPluginRuntimeLoadEntry> plugins)
    : impl_(std::make_unique<MuonPluginRuntimeImpl>(
          std::move(plugin_directory), std::move(plugins))) {
  CEF_REQUIRE_UI_THREAD();
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
    ShutdownMuonBuiltinPlugins();
  }
  LogMuonMessage(kMuonLogSourceMuon, kMuonLogLevelInfo,
                 "Loaded " +
                     std::to_string(impl_->registered_functions.size()) +
                     " plugin functions from " +
                     impl_->plugin_directory.string());
}

MuonPluginRuntime::~MuonPluginRuntime() {
  ShutdownMuonBuiltinPlugins();
  impl_->DrainTrafficTasks();
  if (g_muon_runtime_helpers == impl_.get()) {
    g_muon_runtime_helpers = nullptr;
  }
  std::vector<uint32_t> proxy_ids;
  for (const auto& entry : impl_->proxy_ids_by_owner) {
    proxy_ids.insert(proxy_ids.end(), entry.second.begin(),
                     entry.second.end());
  }
  impl_->proxy_ids_by_owner.clear();
  for (const auto proxy_id : proxy_ids) {
    ReleaseMuonFunctionProxy(impl_.get(), proxy_id);
  }
  auto libraries = std::move(impl_->libraries);
  impl_.reset();
  for (auto& library : libraries) {
    CloseMuonDynamicLibrary(library.handle);
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

bool MuonPluginRuntime::IsReady() const {
  return impl_->startup_error.empty();
}

std::string MuonPluginRuntime::GetStartupError() const {
  return impl_->startup_error;
}

CefRefPtr<CefDictionaryValue> MuonPluginRuntime::CreateRendererMetadata()
    const {
  return CreateMuonRendererMetadata(impl_->renderer_namespaces,
                                    impl_->renderer_functions);
}

MuonBuiltinBrowserFunctionKind MuonPluginRuntime::GetBuiltinBrowserFunctionKind(
    uint32_t function_id) const {
  const auto iterator =
      impl_->builtin_browser_functions_by_id.find(function_id);
  if (iterator == impl_->builtin_browser_functions_by_id.end()) {
    return MuonBuiltinBrowserFunctionKind::None;
  }
  return iterator->second;
}

void MuonPluginRuntime::CancelFsDialogsForOwner(int owner_browser_id) {
  if (!impl_ || owner_browser_id <= 0) {
    return;
  }
  for (const auto cancel_owner : impl_->fs_dialogs_cancel_owner_functions) {
    if (cancel_owner != nullptr) {
      cancel_owner(owner_browser_id);
    }
  }
}

void MuonPluginRuntime::Invoke(const MuonPluginInvocationContext& context,
                                uint32_t function_id,
                                int call_id,
                                CefRefPtr<CefListValue> encoded_args,
                                std::shared_ptr<MuonSharedBufferPayload>
                                    shared_payload,
                                Completion completion) {
  CEF_REQUIRE_UI_THREAD();
  const auto function_iterator = impl_->functions_by_id.find(function_id);
  if (function_iterator == impl_->functions_by_id.end()) {
    MuonPluginCallResult result;
    result.success = false;
    result.error_message = "Unknown muon plugin function";
    completion(result);
    return;
  }

  auto* function = function_iterator->second;
  MuonDecodedArguments decoded_args;
  std::string error_message;
  if (!DecodeMuonPluginArguments(impl_.get(), context,
                                  function->metadata.arg_types, encoded_args,
                                  std::move(shared_payload), &decoded_args,
                                  &error_message)) {
    MuonPluginCallResult result;
    result.success = false;
    result.error_message = error_message;
    decoded_args.ResetRendererFunctionBorrows();
    completion(result);
    return;
  }

  auto* dispatcher = cardio::unsafe_get_current_dispatcher();
  if (dispatcher == nullptr) {
    MuonPluginCallResult result;
    result.success = false;
    result.error_message = "muon main dispatcher is unavailable";
    decoded_args.ResetRendererFunctionBorrows();
    completion(result);
    return;
  }

  muon_internal::FireAndForgetOnDispatcher(
      dispatcher,
      [impl = impl_.get(),
       function,
       call_id,
       renderer_context_id = context.renderer_context_id,
       decoded_args = std::move(decoded_args),
       completion]() mutable {
    InvokeMuonTrafficFunction(
        impl,
        &impl->renderer_side,
        function->function_ref,
        function->metadata.return_type,
        call_id,
        renderer_context_id,
        std::move(decoded_args),
        std::move(completion));
  });
}

void MuonPluginRuntime::InvokeProxy(
    const MuonPluginInvocationContext& context,
    uint32_t proxy_id,
    int call_id,
    CefRefPtr<CefListValue> encoded_args,
    std::shared_ptr<MuonSharedBufferPayload> shared_payload,
    Completion completion) {
  CEF_REQUIRE_UI_THREAD();
  MuonFunctionProxy proxy;
  if (!TryGetMuonFunctionProxy(impl_.get(), proxy_id, &proxy) ||
      proxy.function_type.type != MUON_TYPE_FUNCTION ||
      proxy.function_type.function_return_type.empty()) {
    MuonPluginCallResult result;
    result.success = false;
    result.error_message = "Unknown muon function proxy";
    completion(result);
    return;
  }

  MuonDecodedArguments decoded_args;
  std::string error_message;
  if (!DecodeMuonPluginArguments(impl_.get(), context,
                                  proxy.function_type.function_arg_types,
                                  encoded_args, std::move(shared_payload),
                                  &decoded_args,
                                  &error_message)) {
    MuonPluginCallResult result;
    result.success = false;
    result.error_message = error_message;
    decoded_args.ResetRendererFunctionBorrows();
    completion(result);
    return;
  }

  auto* dispatcher = cardio::unsafe_get_current_dispatcher();
  if (dispatcher == nullptr) {
    MuonPluginCallResult result;
    result.success = false;
    result.error_message = "muon main dispatcher is unavailable";
    decoded_args.ResetRendererFunctionBorrows();
    completion(result);
    return;
  }

  muon_internal::FireAndForgetOnDispatcher(
      dispatcher,
      [impl = impl_.get(),
       proxy,
       call_id,
       renderer_context_id = context.renderer_context_id,
       decoded_args = std::move(decoded_args),
       completion]() mutable {
    InvokeMuonTrafficFunction(
        impl,
        &impl->renderer_side,
        proxy.function_ref,
        proxy.function_type.function_return_type[0],
        call_id,
        renderer_context_id,
        std::move(decoded_args),
        std::move(completion));
  });
}

uint32_t MuonPluginRuntime::RegisterPluginFunctionProxy(
    const MuonPluginInvocationContext& context,
    muon_native_function function,
    const MuonTypeMetadata& function_type) {
  CEF_REQUIRE_UI_THREAD();
  const auto owner_id = CreateMuonFunctionOwnerId(
      context, context.renderer_context_id);
  return RegisterMuonFunctionProxyForOwner(impl_.get(), owner_id, function,
                                            function_type);
}

bool MuonPluginRuntime::CreateSharedBufferMessage(
    const std::string& message_name,
    int call_id,
    size_t value_index,
    const muon_buffer_view& buffer_view,
    MuonCreatedSharedBufferMessage* created_message,
    std::string* error_message) {
  if (buffer_view.data == nullptr && buffer_view.size != 0) {
    *error_message = "Buffer view data is null";
    return false;
  }
  const auto sources = std::vector<MuonSharedBufferSource>{
      {value_index, buffer_view.data, static_cast<size_t>(buffer_view.size)},
  };
  return CreateMuonRuntimeSharedBufferMessage(
      impl_.get(), message_name, call_id, 0, sources, created_message,
      error_message);
}

void MuonPluginRuntime::CompleteRendererFunctionCall(
    CefRefPtr<CefProcessMessage> message,
    std::shared_ptr<MuonSharedBufferPayload> shared_payload) {
  CEF_REQUIRE_UI_THREAD();
  if (!message || message->GetName().ToString() !=
                      kMuonRendererFunctionResultMessageName) {
    return;
  }
  const auto args = message->GetArgumentList();
  if (!args || args->GetSize() < 1) {
    return;
  }

  const auto call_id = static_cast<uint32_t>(args->GetInt(0));
  MuonPendingRendererFunctionCall pending_call;
  const auto pending_iterator =
      impl_->pending_renderer_function_calls.find(call_id);
  if (pending_iterator == impl_->pending_renderer_function_calls.end()) {
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
  if (args->GetSize() < 3) {
    complete(nullptr, "Renderer function result is invalid");
    return;
  }
  const auto success = args->GetBool(1);
  if (!success) {
    const auto error_message = args->GetString(2).ToString();
    complete(nullptr, error_message.c_str());
    return;
  }
  if (pending_call.source == nullptr ||
      pending_call.source->function_type.function_return_type.empty()) {
    complete(nullptr, "Renderer function return type is invalid");
    return;
  }

  const auto& expected_type =
      pending_call.source->function_type.function_return_type[0];
  const auto returned_type = static_cast<muon_value_type>(args->GetInt(2));
  if (returned_type != expected_type.type) {
    complete(nullptr, "Renderer function returned an unexpected type");
    return;
  }
  if (expected_type.type != MUON_TYPE_VOID && args->GetSize() < 4) {
    complete(nullptr, "Renderer function result value is missing");
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
      bool_storage = args->GetBool(3);
      complete(&bool_storage, nullptr);
      return;
    case MUON_TYPE_I8:
      i8_storage = static_cast<int8_t>(args->GetInt(3));
      complete(&i8_storage, nullptr);
      return;
    case MUON_TYPE_U8:
      u8_storage = static_cast<uint8_t>(args->GetInt(3));
      complete(&u8_storage, nullptr);
      return;
    case MUON_TYPE_I16:
      i16_storage = static_cast<int16_t>(args->GetInt(3));
      complete(&i16_storage, nullptr);
      return;
    case MUON_TYPE_U16:
      u16_storage = static_cast<uint16_t>(args->GetInt(3));
      complete(&u16_storage, nullptr);
      return;
    case MUON_TYPE_I32:
      i32_storage = args->GetInt(3);
      complete(&i32_storage, nullptr);
      return;
    case MUON_TYPE_U32:
      u32_storage = static_cast<uint32_t>(args->GetDouble(3));
      complete(&u32_storage, nullptr);
      return;
    case MUON_TYPE_I64:
      if (args->GetType(3) != VTYPE_STRING ||
          !ParseMuonInt64(args->GetString(3).ToString(), &i64_storage)) {
        complete(nullptr, "Renderer function returned a non-i64 value");
        return;
      }
      complete(&i64_storage, nullptr);
      return;
    case MUON_TYPE_U64:
      if (args->GetType(3) != VTYPE_STRING ||
          !ParseMuonUInt64(args->GetString(3).ToString(), &u64_storage)) {
        complete(nullptr, "Renderer function returned a non-u64 value");
        return;
      }
      complete(&u64_storage, nullptr);
      return;
    case MUON_TYPE_F32:
      f32_storage = static_cast<float>(args->GetDouble(3));
      complete(&f32_storage, nullptr);
      return;
    case MUON_TYPE_F64:
      f64_storage = args->GetDouble(3);
      complete(&f64_storage, nullptr);
      return;
    case MUON_TYPE_POINTER:
      if (!GetMuonPointerListValue(args, 3, &pointer_storage)) {
        complete(nullptr, "Renderer function returned a non-pointer value");
        return;
      }
      complete(&pointer_storage, nullptr);
      return;
    case MUON_TYPE_STRING:
      if (args->GetType(3) == VTYPE_NULL) {
        complete(&string_pointer, nullptr);
        return;
      }
      string_storage = args->GetString(3).ToString();
      string_pointer = string_storage.c_str();
      complete(&string_pointer, nullptr);
      return;
    case MUON_TYPE_BUFFER_VIEW: {
      if (args->GetType(3) != VTYPE_DICTIONARY || !shared_payload) {
        complete(nullptr,
                 "Renderer function returned a non-buffer_view value");
        return;
      }
      MuonSharedBufferEntry placeholder;
      if (!ReadMuonSharedBufferPlaceholder(args->GetDictionary(3),
                                            &placeholder) ||
          placeholder.value_index != 3) {
        complete(nullptr,
                 "Renderer function returned an invalid buffer_view value");
        return;
      }
      MuonSharedBufferEntry entry;
      if (!FindMuonSharedBufferEntry(*shared_payload, 3, &entry) ||
          entry.offset != placeholder.offset ||
          entry.size != placeholder.size) {
        complete(nullptr, "Renderer function buffer_view payload is missing");
        return;
      }
      buffer_storage.data = GetMuonSharedBufferEntryData(*shared_payload,
                                                          entry);
      buffer_storage.size = static_cast<uintptr_t>(entry.size);
      if (entry.size > 0 && buffer_storage.data == nullptr) {
        complete(nullptr, "Renderer function buffer_view payload is invalid");
        return;
      }
      complete(&buffer_storage, nullptr);
      return;
    }
    case MUON_TYPE_FUNCTION: {
      if (args->GetType(3) == VTYPE_NULL) {
        complete(&function_storage, nullptr);
        return;
      }
      if (args->GetType(3) != VTYPE_DICTIONARY) {
        complete(nullptr, "Renderer function result is not a function");
        return;
      }
      const auto encoded_function = args->GetDictionary(3);
      if (!encoded_function) {
        complete(nullptr, "Renderer function result is invalid");
        return;
      }
      if (encoded_function->HasKey(kMuonFunctionArgumentProxyIdKey)) {
        MuonFunctionProxy proxy;
        const auto proxy_id = static_cast<uint32_t>(
            encoded_function->GetInt(kMuonFunctionArgumentProxyIdKey));
        if (!TryGetMuonFunctionProxy(impl_.get(), proxy_id, &proxy) ||
            !AreEqualMuonTypes(proxy.function_type, expected_type)) {
          complete(nullptr, "Renderer returned an unknown function proxy");
          return;
        }
        function_storage = proxy.function;
        complete(&function_storage, nullptr);
        return;
      }

      if (!encoded_function->HasKey(kMuonFunctionArgumentContextIdKey) ||
          !encoded_function->HasKey(kMuonFunctionArgumentFunctionIdKey)) {
        complete(nullptr, "Renderer function result is invalid");
        return;
      }
      const auto renderer_context_id =
          encoded_function->GetInt(kMuonFunctionArgumentContextIdKey);
      const auto function_id =
          encoded_function->GetInt(kMuonFunctionArgumentFunctionIdKey);
      std::string error_message;
      MuonRendererFunctionBorrow renderer_function_borrow;
      if (!GetOrCreateMuonRendererFunction(
              impl_.get(), pending_call.source->context, renderer_context_id,
              function_id, expected_type, &function_storage,
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

void MuonPluginRuntime::ReleaseFunctionContext(
    const MuonPluginInvocationContext& context,
    int renderer_context_id) {
  CEF_REQUIRE_UI_THREAD();
  ReleaseMuonBuiltinExecutorContext(renderer_context_id);
  const auto owner_id = CreateMuonFunctionOwnerId(context,
                                                   renderer_context_id);
  std::vector<MuonRendererFunctionSource*> sources;
  const auto owner_iterator =
      impl_->renderer_sources_by_owner.find(owner_id);
  if (owner_iterator != impl_->renderer_sources_by_owner.end()) {
    sources.assign(owner_iterator->second.begin(),
                   owner_iterator->second.end());
    impl_->renderer_sources_by_owner.erase(owner_iterator);
  }

  for (auto* source : sources) {
    if (source == nullptr) {
      continue;
    }
    source->context_valid = false;
    source->renderer_lease_active = false;
    source->context.frame = nullptr;
    const auto source_iterator =
        impl_->renderer_functions_by_source.find(source->source_id);
    if (source_iterator != impl_->renderer_functions_by_source.end() &&
        source_iterator->second == source) {
      impl_->renderer_functions_by_source.erase(source_iterator);
    }
  }

  std::vector<MuonPendingRendererFunctionCall> pending_calls;
  auto pending_iterator = impl_->pending_renderer_function_calls.begin();
  while (pending_iterator != impl_->pending_renderer_function_calls.end()) {
    auto* source = pending_iterator->second.source;
    if (source == nullptr || source->owner_id != owner_id) {
      ++pending_iterator;
      continue;
    }
    pending_calls.push_back(std::move(pending_iterator->second));
    pending_iterator =
        impl_->pending_renderer_function_calls.erase(pending_iterator);
  }

  for (auto* source : sources) {
    ReleaseMuonRendererFunctionBridgeRetainIfIdle(source);
  }
  for (auto& pending_call : pending_calls) {
    CompleteMuonRendererFunctionWithError(
        pending_call.completion, "Renderer function context was released");
    pending_call.completion = nullptr;
  }

  std::vector<uint32_t> proxy_ids;
  const auto proxy_iterator = impl_->proxy_ids_by_owner.find(owner_id);
  if (proxy_iterator != impl_->proxy_ids_by_owner.end()) {
    proxy_ids = proxy_iterator->second;
    impl_->proxy_ids_by_owner.erase(proxy_iterator);
  }
  for (const auto proxy_id : proxy_ids) {
    ReleaseMuonFunctionProxy(impl_.get(), proxy_id);
  }
}
