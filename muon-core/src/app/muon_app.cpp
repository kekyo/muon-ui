/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "app/muon_app.h"

#include "app/muon_app_scheme.h"
#include "app/muon_app_storage.h"
#include "browser/muon_default_title_bar_icon.h"
#include "browser/muon_browser_background_color.h"
#include "browser/muon_browser_view_delegate.h"
#include "browser/muon_client.h"
#include "browser/muon_title_bar.h"
#include "browser/muon_title_bar_loader.h"
#include "config/muon_config.h"
#include "config/muon_linux_display_backend.h"
#include "config/muon_startup.h"
#include "log/muon_log.h"
#include "plugins/muon_js_bridge.h"
#include "network/muon_network_policy.h"
#include "plugins/builtin/muon_builtin_launcher.h"
#include "plugins/builtin/muon_builtin_environments.h"
#include "plugins/muon_plugin_policy.h"
#include "plugins/muon_plugin_runtime.h"
#include "plugins/muon_shared_buffer.h"
#include "plugins/muon_v8_handler.h"
#include "browser/muon_window_delegate.h"

#include "include/cef_frame.h"
#include "include/cef_process_message.h"
#include "include/cef_scheme.h"
#include "include/cef_v8.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_window.h"
#include "include/wrapper/cef_helpers.h"

#include <cstdlib>
#include <set>
#include <string>
#include <utility>
#include <vector>

class MuonNamespaceMarker final : public CefBaseRefCounted {
 public:
  MuonNamespaceMarker() = default;

 private:
  IMPLEMENT_REFCOUNTING(MuonNamespaceMarker);
  DISALLOW_COPY_AND_ASSIGN(MuonNamespaceMarker);
};

class MuonAllowedFunctionHandler final : public CefV8Handler {
 public:
  explicit MuonAllowedFunctionHandler(std::vector<std::string> allowed_names)
      : allowed_names_(allowed_names.begin(), allowed_names.end()) {}

  bool Execute(const CefString& name,
               CefRefPtr<CefV8Value> object,
               const CefV8ValueList& arguments,
               CefRefPtr<CefV8Value>& retval,
               CefString& exception) override {
    (void)name;
    (void)object;
    (void)exception;
    auto allowed = false;
    if (arguments.size() == 1 && arguments[0] && arguments[0]->IsString()) {
      allowed =
          allowed_names_.find(arguments[0]->GetStringValue().ToString()) !=
          allowed_names_.end();
    }
    retval = CefV8Value::CreateBool(allowed);
    return true;
  }

 private:
  std::set<std::string> allowed_names_;

  IMPLEMENT_REFCOUNTING(MuonAllowedFunctionHandler);
  DISALLOW_COPY_AND_ASSIGN(MuonAllowedFunctionHandler);
};

static CefRefPtr<CefV8Value> CreateMuonNamespaceObject() {
  const auto object = CefV8Value::CreateObject(nullptr, nullptr);
  if (object) {
    object->SetUserData(new MuonNamespaceMarker());
  }
  return object;
}

static MuonTitleBarManifest LoadConfiguredMuonTitleBarManifest(
    const MuonBrowserConfig& browser_config) {
  if (browser_config.title_bar == kMuonBrowserTitleBarMuon) {
    return LoadMuonTitleBarManifestFromUi();
  }
  if (!IsMuonNativeTitleBarSupported(
          GetMuonStartupCommandLine(), std::getenv("XDG_SESSION_TYPE"),
          std::getenv("WAYLAND_DISPLAY"), std::getenv("DISPLAY"))) {
    LogMuonMessage(
        kMuonLogSourceMuon, kMuonLogLevelWarning,
        "browser.titleBarType is native, but native title bar decoration is not "
        "available on the current Linux display backend. Falling back to the "
        "muon title bar.");
    return LoadMuonTitleBarManifestFromUi();
  }
  return CreateNativeMuonTitleBarManifest();
}

static MuonTitleBarBackgroundColor CreateMuonTitleBarBackgroundColor(
    const MuonBrowserBackgroundColorConfig& background_color) {
  if (background_color.mode != kMuonBrowserBackgroundColorRgb) {
    return {};
  }
  return {true, background_color.red, background_color.green,
          background_color.blue};
}

static CefString CreateCefPathString(const std::filesystem::path& path) {
  CefString value;
#if defined(_WIN32)
  value.FromWString(path.wstring());
#else
  value.FromString(path.string());
#endif
  return value;
}

static void AppendMuonConfigPathArguments(
    CefRefPtr<CefCommandLine> command_line,
    const std::vector<std::filesystem::path>& config_paths) {
  if (!command_line) {
    return;
  }
  for (const auto& config_path : config_paths) {
    command_line->AppendArgument("-c");
    command_line->AppendArgument(CreateCefPathString(config_path));
  }
}

static std::string TrimAsciiWhitespace(std::string value) {
  auto begin = size_t{0};
  while (begin < value.size() &&
         (value[begin] == ' ' || value[begin] == '\t' ||
          value[begin] == '\r' || value[begin] == '\n')) {
    ++begin;
  }
  auto end = value.size();
  while (end > begin &&
         (value[end - 1] == ' ' || value[end - 1] == '\t' ||
          value[end - 1] == '\r' || value[end - 1] == '\n')) {
    --end;
  }
  return value.substr(begin, end - begin);
}

static bool ContainsCommaSeparatedValue(const std::string& values,
                                        const std::string& expected) {
  auto start = size_t{0};
  while (start <= values.size()) {
    auto end = values.find(',', start);
    if (end == std::string::npos) {
      end = values.size();
    }
    if (TrimAsciiWhitespace(values.substr(start, end - start)) == expected) {
      return true;
    }
    if (end == values.size()) {
      break;
    }
    start = end + 1;
  }
  return false;
}

static void AppendCommaSeparatedSwitchValues(
    CefRefPtr<CefCommandLine> command_line,
    const char* switch_name,
    const std::vector<std::string>& added_values) {
  auto value = command_line->GetSwitchValue(switch_name).ToString();
  for (const auto& added_value : added_values) {
    if (ContainsCommaSeparatedValue(value, added_value)) {
      continue;
    }
    if (!value.empty()) {
      value += ",";
    }
    value += added_value;
  }

  CefString cef_value;
  cef_value.FromString(value);
  command_line->AppendSwitchWithValue(switch_name, cef_value);
}

static void AppendSwitchWithAsciiValue(CefRefPtr<CefCommandLine> command_line,
                                       const char* switch_name,
                                       const char* value) {
  CefString cef_value;
  cef_value.FromString(value);
  command_line->AppendSwitchWithValue(switch_name, cef_value);
}

static void ConfigureMuonCefLinuxDisplayCommandLine(
    CefRefPtr<CefCommandLine> command_line) {
#if defined(OS_LINUX)
  const auto startup_command_line = GetMuonStartupCommandLine();
  const auto* xdg_session_type = std::getenv("XDG_SESSION_TYPE");
  const auto* wayland_display = std::getenv("WAYLAND_DISPLAY");
  const auto* display = std::getenv("DISPLAY");
  if (!ShouldDisableMuonCefVulkanForLinuxDisplayBackend(
          startup_command_line, xdg_session_type, wayland_display, display)) {
    return;
  }
  command_line->AppendSwitch("disable-vulkan-surface");
  AppendCommaSeparatedSwitchValues(
      command_line, "disable-features",
      {"Vulkan", "VulkanFromANGLE", "DefaultANGLEVulkan"});
  if (ShouldUseMuonCefAngleOpenGlForLinuxDisplayBackend(
          startup_command_line, xdg_session_type, wayland_display, display) &&
      !command_line->HasSwitch("use-gl") &&
      !command_line->HasSwitch("use-angle")) {
    AppendSwitchWithAsciiValue(command_line, "use-gl", "angle");
    AppendSwitchWithAsciiValue(command_line, "use-angle", "gl");
  }
#else
  (void)command_line;
#endif
}

static bool IsMuonNamespaceObject(CefRefPtr<CefV8Value> value) {
  return value && value->IsObject() && value->IsUserCreated() &&
         value->GetUserData();
}

static bool GetOrCreateMuonNamespaceObject(
    CefRefPtr<CefV8Value> global,
    const std::string& plugin_namespace,
    CefV8Value::PropertyAttribute attribute,
    CefRefPtr<CefV8Value>* namespace_object,
    std::string* error_message) {
  if (!global || namespace_object == nullptr || error_message == nullptr) {
    return false;
  }

  std::vector<std::string> segments;
  if (!SplitMuonPluginNamespace(plugin_namespace, &segments)) {
    *error_message = "Invalid plugin namespace: " + plugin_namespace;
    return false;
  }

  auto current = global;
  std::string current_path;
  for (const auto& segment : segments) {
    current_path = current_path.empty() ? segment : current_path + "." + segment;
    if (current->HasValue(segment)) {
      const auto existing = current->GetValue(segment);
      if (!IsMuonNamespaceObject(existing)) {
        *error_message =
            "Plugin namespace conflicts with existing member: " + current_path;
        return false;
      }
      current = existing;
      continue;
    }

    const auto next = CreateMuonNamespaceObject();
    if (!next || !current->SetValue(segment, next, attribute)) {
      *error_message = "Failed to create plugin namespace: " + current_path;
      return false;
    }
    current = next;
  }

  *namespace_object = current;
  return true;
}

static bool IsMuonBlankDocumentUrl(const std::string& url) {
  return url.empty() || url.rfind("about:blank", 0) == 0 ||
         url.rfind("about:srcdoc", 0) == 0;
}

static bool ShouldExposeMuonApi(
    CefRefPtr<CefFrame> frame,
    const std::shared_ptr<MuonNetworkPolicy>& plugin_page_policy,
    const std::string& url_hint) {
  if (!frame || !frame->IsMain() || !plugin_page_policy) {
    return false;
  }
  auto url = frame->GetURL().ToString();
  if (IsMuonBlankDocumentUrl(url) && !url_hint.empty()) {
    url = url_hint;
  }
  if (url.rfind("devtools://", 0) == 0 ||
      url.rfind("chrome-devtools://", 0) == 0) {
    return false;
  }
  return plugin_page_policy->IsAllowedUrl(url);
}

static bool IsMuonInternalFunctionName(const std::string& name) {
  return name.rfind("__", 0) == 0;
}

static CefV8Value::PropertyAttribute GetMuonFunctionPropertyAttribute(
    const std::string& name) {
  auto attribute = V8_PROPERTY_ATTRIBUTE_READONLY |
                   V8_PROPERTY_ATTRIBUTE_DONTDELETE;
  if (IsMuonInternalFunctionName(name)) {
    attribute |= V8_PROPERTY_ATTRIBUTE_DONTENUM;
  }
  return static_cast<CefV8Value::PropertyAttribute>(attribute);
}

static std::string FormatMuonV8Exception(
    CefRefPtr<CefV8Exception> exception) {
  if (!exception) {
    return "unknown exception";
  }
  return exception->GetMessage().ToString() + " at " +
         exception->GetScriptResourceName().ToString() + ":" +
         std::to_string(exception->GetLineNumber());
}

static bool ExecuteMuonNamespaceSetupScript(
    CefRefPtr<CefV8Context> context,
    CefRefPtr<CefV8Value> namespace_object,
    const MuonNamespaceMetadata& plugin_namespace) {
  if (plugin_namespace.setup_script.empty()) {
    return true;
  }
  const auto source =
      std::string("(function(namespace, globalThis, isAllowed) {\n"
                  "\"use strict\";\n") +
      plugin_namespace.setup_script + "\n})";
  CefRefPtr<CefV8Value> setup_function;
  CefRefPtr<CefV8Exception> exception;
  if (!context->Eval(source,
                     "muon://plugin-setup/" +
                         plugin_namespace.plugin_namespace,
                     1, setup_function, exception) ||
      !setup_function || !setup_function->IsFunction()) {
    LogMuonMessage(kMuonLogSourceMuon, kMuonLogLevelError,
                   "Plugin namespace setup failed for " +
                       plugin_namespace.plugin_namespace + ": " +
                       FormatMuonV8Exception(exception));
    return false;
  }

  CefV8ValueList args;
  args.push_back(namespace_object);
  args.push_back(context->GetGlobal());
  args.push_back(CefV8Value::CreateFunction(
      "isAllowed",
      new MuonAllowedFunctionHandler(
          plugin_namespace.allowed_function_names)));
  const auto result = setup_function->ExecuteFunctionWithContext(
      context, context->GetGlobal(), args);
  if (!result) {
    const auto setup_exception = setup_function->GetException();
    LogMuonMessage(kMuonLogSourceMuon, kMuonLogLevelError,
                   "Plugin namespace setup failed for " +
                       plugin_namespace.plugin_namespace + ": " +
                       FormatMuonV8Exception(setup_exception));
    setup_function->ClearException();
    return false;
  }
  return true;
}

static bool AddMuonPluginFreezeRootName(
    const std::string& plugin_namespace,
    std::set<std::string>* root_names,
    std::string* error_message) {
  if (root_names == nullptr || error_message == nullptr) {
    return false;
  }
  std::vector<std::string> segments;
  if (!SplitMuonPluginNamespace(plugin_namespace, &segments) ||
      segments.empty()) {
    *error_message = "Invalid plugin namespace: " + plugin_namespace;
    return false;
  }
  root_names->insert(segments[0]);
  return true;
}

static bool FreezeMuonPluginDefinitions(
    CefRefPtr<CefV8Context> context,
    const MuonRendererMetadata& metadata,
    std::string* error_message) {
  if (!context || error_message == nullptr) {
    return false;
  }

  std::set<std::string> root_names;
  for (const auto& plugin_namespace : metadata.namespaces) {
    if (!AddMuonPluginFreezeRootName(plugin_namespace.plugin_namespace,
                                     &root_names, error_message)) {
      return false;
    }
  }
  for (const auto& function : metadata.functions) {
    if (!AddMuonPluginFreezeRootName(function.plugin_namespace, &root_names,
                                     error_message)) {
      return false;
    }
  }
  if (root_names.empty()) {
    return true;
  }

  const auto global = context->GetGlobal();
  if (!global) {
    *error_message = "Plugin global object is unavailable";
    return false;
  }

  CefV8ValueList roots;
  for (const auto& root_name : root_names) {
    const auto root = global->GetValue(root_name);
    if (!IsMuonNamespaceObject(root)) {
      *error_message = "Plugin namespace root is unavailable: " + root_name;
      return false;
    }
    roots.push_back(root);
  }

  const auto source = std::string(R"JS(
(function() {
"use strict";
const seen = new WeakSet();
const deepFreeze = (value) => {
  if (value === null || (typeof value !== "object" && typeof value !== "function")) {
    return;
  }
  if (seen.has(value)) {
    return;
  }
  seen.add(value);
  for (const key of Reflect.ownKeys(value)) {
    const descriptor = Object.getOwnPropertyDescriptor(value, key);
    if (descriptor === undefined) {
      continue;
    }
    if ("value" in descriptor) {
      deepFreeze(descriptor.value);
    } else {
      deepFreeze(descriptor.get);
      deepFreeze(descriptor.set);
    }
  }
  Object.freeze(value);
};
for (let index = 0; index < arguments.length; index += 1) {
  deepFreeze(arguments[index]);
}
})
)JS");
  CefRefPtr<CefV8Value> freeze_function;
  CefRefPtr<CefV8Exception> exception;
  if (!context->Eval(source, "muon://plugin-freeze", 1, freeze_function,
                     exception) ||
      !freeze_function || !freeze_function->IsFunction()) {
    *error_message = "Plugin definition freeze setup failed: " +
                     FormatMuonV8Exception(exception);
    return false;
  }

  const auto result = freeze_function->ExecuteFunctionWithContext(
      context, global, roots);
  if (!result) {
    const auto freeze_exception = freeze_function->GetException();
    *error_message = "Plugin definition freeze failed: " +
                     FormatMuonV8Exception(freeze_exception);
    freeze_function->ClearException();
    return false;
  }
  return true;
}

static int GetMuonResultContextId(CefRefPtr<CefProcessMessage> message) {
  if (!message) {
    return 0;
  }
  const auto message_name = message->GetName().ToString();
  if ((message_name == kMuonPluginResultSharedMessageName ||
       message_name == kMuonRendererFunctionCallSharedMessageName)) {
    auto call_id = 0;
    auto renderer_context_id = 0;
    std::string error_message;
    if (ReadMuonSharedBufferPayloadMetadata(
            message, &call_id, &renderer_context_id, &error_message)) {
      return renderer_context_id;
    }
    return 0;
  }
  const auto args = message->GetArgumentList();
  if (!args) {
    return 0;
  }
  if (message_name == kMuonPluginResultMessageName &&
      args->GetSize() >= 5) {
    return args->GetInt(4);
  }
  if (message_name == kMuonRendererFunctionCallMessageName &&
      args->GetSize() >= 2) {
    return args->GetInt(1);
  }
  if ((message_name == kMuonRendererFunctionSourceAcquireMessageName ||
       message_name == kMuonRendererFunctionSourceReleaseMessageName ||
       message_name == kMuonRendererFunctionResultConsumedMessageName) &&
      args->GetSize() >= 1) {
    return args->GetInt(0);
  }
#if defined(MUON_TEST_BUILD)
  if (message_name == kMuonFunctionWrapperDiagnosticsResultMessageName &&
      args->GetSize() >= 1 && args->GetType(0) == VTYPE_INT) {
    return args->GetInt(0);
  }
#endif
  return 0;
}

MuonApp::MuonApp(const MuonConfig& config,
                 std::filesystem::path cef_log_path,
                 std::vector<std::filesystem::path> config_paths,
                 cardio::dispatcher* dispatcher)
    : config_(config),
      cef_log_path_(std::move(cef_log_path)),
      config_paths_(std::move(config_paths)),
      dispatcher_(dispatcher) {
  if (!CreateMuonUrlPolicy(config_.browser.plugin.allow,
                           "plugin.pages", &plugin_page_policy_,
                           &plugin_page_policy_error_)) {
    plugin_page_policy_.reset();
  }
  if (!CreateMuonUrlPolicy(
          config_.browser.allow_unsafe_javascript_parent_access,
          "browser.allowUnsafeJavaScriptParentAccess",
          &unsafe_parent_access_policy_,
          &unsafe_parent_access_policy_error_)) {
    unsafe_parent_access_policy_.reset();
  }
  for (const auto& capability : config_.browser.plugin.capabilities) {
    if (plugin_capability_policies_.find(capability.id) !=
        plugin_capability_policies_.end()) {
      plugin_capability_policy_error_ =
          "Duplicate plugin.capabilities id: " + capability.id;
      break;
    }
    std::shared_ptr<MuonPluginPolicy> capability_policy;
    std::string capability_error;
    if (!CreateMuonPluginPolicy(capability.allow, &capability_policy,
                                &capability_error)) {
      plugin_capability_policy_error_ =
          "Invalid plugin.capabilities '" + capability.id + "': " +
          capability_error;
      break;
    }
    plugin_capability_policies_[capability.id] = capability_policy;
  }
}

CefRefPtr<CefBrowserProcessHandler> MuonApp::GetBrowserProcessHandler() {
  return this;
}

CefRefPtr<CefRenderProcessHandler> MuonApp::GetRenderProcessHandler() {
  return this;
}

void MuonApp::OnRegisterCustomSchemes(
    CefRawPtr<CefSchemeRegistrar> registrar) {
  RegisterMuonAppCustomScheme(registrar);
}

void MuonApp::OnBeforeCommandLineProcessing(
    const CefString& process_type,
    CefRefPtr<CefCommandLine> command_line) {
  (void)process_type;
  if (!command_line) {
    return;
  }
  ConfigureMuonCefLinuxDisplayCommandLine(command_line);
  CefString cef_log_path;
#if defined(_WIN32)
  cef_log_path.FromWString(cef_log_path_.wstring());
#else
  cef_log_path.FromString(cef_log_path_.string());
#endif
  command_line->AppendSwitchWithValue("log-file", cef_log_path);
  command_line->AppendSwitchWithValue(
      "log-severity",
      GetMuonCefLogSeveritySwitchValue(
          GetMuonLogSourceLevel(config_.log, kMuonLogSourceCef)));
}

void MuonApp::OnBeforeChildProcessLaunch(
    CefRefPtr<CefCommandLine> command_line) {
  AppendMuonConfigPathArguments(command_line, config_paths_);
}

int MuonApp::GetExitCode() const {
  return exit_code_;
}

bool MuonApp::RequestShutdown(int32_t exit_code) {
  CEF_REQUIRE_UI_THREAD();
  if (!shutdown_requested_) {
    exit_code_ = static_cast<int>(exit_code);
    shutdown_requested_ = true;
  }
  return true;
}

void MuonApp::OnContextInitialized() {
  CEF_REQUIRE_UI_THREAD();

  if (!plugin_page_policy_error_.empty()) {
    exit_code_ = 1;
    LogMuonMessage(kMuonLogSourceMuon, kMuonLogLevelError,
                   "muon startup failed: " + plugin_page_policy_error_);
    CefQuitMessageLoop();
    return;
  }
  if (!unsafe_parent_access_policy_error_.empty()) {
    exit_code_ = 1;
    LogMuonMessage(kMuonLogSourceMuon, kMuonLogLevelError,
                   "muon startup failed: " + unsafe_parent_access_policy_error_);
    CefQuitMessageLoop();
    return;
  }
  if (!plugin_capability_policy_error_.empty()) {
    exit_code_ = 1;
    LogMuonMessage(kMuonLogSourceMuon, kMuonLogLevelError,
                   "muon startup failed: " + plugin_capability_policy_error_);
    CefQuitMessageLoop();
    return;
  }

  std::shared_ptr<MuonNetworkPolicy> network_policy;
  std::string error_message;
  if (!CreateMuonNetworkPolicy(config_.network.allow,
                               config_.network.authorized_origin,
                               config_.network.local_access.loopback_origins,
                               config_.network.local_access
                                   .local_network_origins,
                               &network_policy, &error_message)) {
    exit_code_ = 1;
    LogMuonMessage(kMuonLogSourceMuon, kMuonLogLevelError,
                   "muon startup failed: " + error_message);
    CefQuitMessageLoop();
    return;
  }
  std::vector<MuonPluginRuntimeLoadEntry> plugins;
  for (const auto& plugin_config : config_.plugin.plugins) {
    std::shared_ptr<MuonPluginPolicy> plugin_policy;
    if (!CreateMuonPluginPolicy(plugin_config.allow, &plugin_policy,
                                &error_message)) {
      exit_code_ = 1;
      error_message =
          "Invalid plugin entry '" + plugin_config.name + "': " +
          error_message;
      LogMuonMessage(kMuonLogSourceMuon, kMuonLogLevelError,
                     "muon startup failed: " + error_message);
      CefQuitMessageLoop();
      return;
    }
    MuonPluginRuntimeLoadEntry plugin;
    plugin.plugin = plugin_config.name;
    plugin.has_expected_signature = plugin_config.has_signature;
    plugin.expected_signature = plugin_config.signature;
    plugin.has_signature_salt = plugin_config.has_salt;
    plugin.signature_salt = plugin_config.salt;
    plugin.plugin_policy = plugin_policy;
    for (const auto& config_entry : plugin_config.config) {
      plugin.config.push_back({config_entry.key, config_entry.value});
    }
    plugins.push_back(std::move(plugin));
  }

  InitializeMuonBuiltinLauncher(config_.default_version_policy);
  InitializeMuonBuiltinEnvironments(config_.config);
  const auto plugin_runtime =
      CreateMuonPluginRuntime(config_.plugin.path, std::move(plugins));
  if (!plugin_runtime->IsReady()) {
    exit_code_ = 1;
    error_message = plugin_runtime->GetStartupError();
    LogMuonMessage(kMuonLogSourceMuon, kMuonLogLevelError,
                   "muon startup failed: " + error_message);
    CefQuitMessageLoop();
    return;
  }
  const auto app_storage = CreateConfiguredMuonAppStorage(
      config_.asset.has_from, config_.asset.from,
      config_.asset.has_signature, config_.asset.signature,
      config_.asset.has_salt, config_.asset.salt, &error_message);
  if (!app_storage) {
    exit_code_ = 1;
    LogMuonMessage(kMuonLogSourceMuon, kMuonLogLevelError,
                   "muon startup failed: " + error_message);
    CefQuitMessageLoop();
    return;
  }
  if (!CefRegisterSchemeHandlerFactory(
          kMuonAppSchemeName, kMuonAppMainDomain,
          CreateMuonAppSchemeHandlerFactory(app_storage, dispatcher_))) {
    exit_code_ = 1;
    constexpr char error_message[] =
        "Failed to register asset scheme handler factory";
    LogMuonMessage(kMuonLogSourceMuon, kMuonLogLevelError,
                   std::string("muon startup failed: ") + error_message);
    CefQuitMessageLoop();
    return;
  }
  auto has_initial_title_bar_icon = false;
  MuonTitleBarIcon initial_title_bar_icon;
  if (config_.browser.has_initial_title_bar_icon) {
    if (!LoadMuonTitleBarIconFromStorage(
            app_storage, config_.browser.initial_title_bar_icon,
            &initial_title_bar_icon, &error_message)) {
      exit_code_ = 1;
      LogMuonMessage(kMuonLogSourceMuon, kMuonLogLevelError,
                     "muon startup failed: " + error_message);
      CefQuitMessageLoop();
      return;
    }
    has_initial_title_bar_icon = true;
  } else {
    if (!LoadDefaultMuonTitleBarIcon(&initial_title_bar_icon,
                                     &error_message)) {
      exit_code_ = 1;
      LogMuonMessage(kMuonLogSourceMuon, kMuonLogLevelError,
                     "muon startup failed: " + error_message);
      CefQuitMessageLoop();
      return;
    }
    has_initial_title_bar_icon = true;
  }
  const auto extra_info = plugin_runtime->CreateRendererMetadata();
  CefBrowserSettings browser_settings;
  ApplyMuonBrowserBackgroundColor(browser_settings,
                                  config_.browser.background_color);
  const auto title_bar_manifest =
      LoadConfiguredMuonTitleBarManifest(config_.browser);
  const auto title_bar_background_color =
      CreateMuonTitleBarBackgroundColor(config_.browser.background_color);
  CefRefPtr<MuonClient> client(
      new MuonClient(plugin_runtime, network_policy, plugin_page_policy_,
                     plugin_capability_policies_,
                     unsafe_parent_access_policy_,
                     [this](int32_t exit_code) {
                       return RequestShutdown(exit_code);
                     },
                     app_storage, config_.browser, title_bar_manifest,
                     title_bar_background_color, has_initial_title_bar_icon,
                     initial_title_bar_icon, config_.desktop_id));
  CefRefPtr<MuonBrowserShortcutHandler> shortcut_handler(client.get());
  auto* close_handler = static_cast<MuonWindowCloseHandler*>(client.get());
  auto browser_view = CefBrowserView::CreateBrowserView(
      client, config_.browser.start_page, browser_settings, extra_info, nullptr,
      new MuonBrowserViewDelegate(
          false, config_.browser.initial_title_bar_visibility,
          title_bar_manifest, title_bar_background_color, shortcut_handler,
          close_handler, config_.desktop_id));

  CefWindow::CreateTopLevelWindow(new MuonWindowDelegate(
      browser_view, false, config_.browser.initial_window_state,
      config_.browser.initial_title_bar_visibility,
      title_bar_manifest, title_bar_background_color, shortcut_handler,
      close_handler, config_.desktop_id));
}

void MuonApp::OnBrowserCreated(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefDictionaryValue> extra_info) {
  CEF_REQUIRE_RENDERER_THREAD();
  if (browser && IsMuonTitleBarExtraInfo(extra_info)) {
    renderer_title_bar_browsers_.insert(browser->GetIdentifier());
    return;
  }
  const auto renderer_metadata = ReadMuonRendererMetadata(extra_info);
  if (!renderer_metadata.functions.empty() ||
      !renderer_metadata.namespaces.empty()) {
    renderer_metadata_ = renderer_metadata;
  }
  const auto url_hint = ReadMuonRendererUrlHint(extra_info);
  if (browser && !url_hint.empty()) {
    renderer_url_hints_by_browser_[browser->GetIdentifier()] = url_hint;
  }
}

void MuonApp::OnBrowserDestroyed(CefRefPtr<CefBrowser> browser) {
  CEF_REQUIRE_RENDERER_THREAD();
  if (browser) {
    renderer_url_hints_by_browser_.erase(browser->GetIdentifier());
    renderer_title_bar_browsers_.erase(browser->GetIdentifier());
  }
}

void MuonApp::OnContextCreated(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefFrame> frame,
                                CefRefPtr<CefV8Context> context) {
  CEF_REQUIRE_RENDERER_THREAD();
  if (browser &&
      renderer_title_bar_browsers_.find(browser->GetIdentifier()) !=
          renderer_title_bar_browsers_.end()) {
    return;
  }
  auto url_hint = std::string{};
  if (browser) {
    const auto url_hint_iterator =
        renderer_url_hints_by_browser_.find(browser->GetIdentifier());
    if (url_hint_iterator != renderer_url_hints_by_browser_.end()) {
      url_hint = url_hint_iterator->second;
    }
  }
  if (!ShouldExposeMuonApi(frame, plugin_page_policy_, url_hint)) {
    return;
  }
  if (renderer_metadata_.functions.empty() &&
      renderer_metadata_.namespaces.empty()) {
    return;
  }

  const auto global = context->GetGlobal();
  CefRefPtr<MuonV8Handler> handler(
      new MuonV8Handler(renderer_metadata_.functions, context));
  if (handler->GetContextId() <= 0) {
    LogMuonMessage(kMuonLogSourceMuon, kMuonLogLevelError,
                   "Muon V8 context ids are exhausted");
    return;
  }
  const auto readonly_attribute = static_cast<CefV8Value::PropertyAttribute>(
      V8_PROPERTY_ATTRIBUTE_READONLY | V8_PROPERTY_ATTRIBUTE_DONTDELETE);
  if (config_.browser.plugin.mode == kMuonBrowserPluginModeValidate) {
    const auto bridge_attribute = static_cast<CefV8Value::PropertyAttribute>(
        V8_PROPERTY_ATTRIBUTE_READONLY | V8_PROPERTY_ATTRIBUTE_DONTDELETE |
        V8_PROPERTY_ATTRIBUTE_DONTENUM);
    global->SetValue(
        kMuonV8CapabilityCallFunctionName,
        CefV8Value::CreateFunction(kMuonV8CapabilityCallFunctionName, handler),
        bridge_attribute);
    v8_handlers_by_context_[handler->GetContextId()] = handler;
    return;
  }
  for (const auto& plugin_namespace : renderer_metadata_.namespaces) {
    CefRefPtr<CefV8Value> namespace_object;
    std::string error_message;
    if (!GetOrCreateMuonNamespaceObject(
            global, plugin_namespace.plugin_namespace, readonly_attribute,
            &namespace_object, &error_message)) {
      LogMuonMessage(kMuonLogSourceMuon, kMuonLogLevelError, error_message);
      return;
    }
  }
  for (const auto& function : renderer_metadata_.functions) {
    CefRefPtr<CefV8Value> namespace_object;
    std::string error_message;
    if (!GetOrCreateMuonNamespaceObject(
            global, function.plugin_namespace, readonly_attribute,
            &namespace_object, &error_message)) {
      LogMuonMessage(kMuonLogSourceMuon, kMuonLogLevelError, error_message);
      return;
    }
    if (namespace_object->HasValue(function.js_name)) {
      LogMuonMessage(kMuonLogSourceMuon, kMuonLogLevelError,
                     "Plugin function conflicts with existing member: " +
                         CreateMuonFunctionPublicPath(function));
      return;
    }
    namespace_object->SetValue(
        function.js_name,
        CefV8Value::CreateFunction(CreateMuonV8FunctionName(function.id),
                                   handler),
        GetMuonFunctionPropertyAttribute(function.js_name));
  }
  for (const auto& plugin_namespace : renderer_metadata_.namespaces) {
    CefRefPtr<CefV8Value> namespace_object;
    std::string error_message;
    if (!GetOrCreateMuonNamespaceObject(
            global, plugin_namespace.plugin_namespace, readonly_attribute,
            &namespace_object, &error_message)) {
      LogMuonMessage(kMuonLogSourceMuon, kMuonLogLevelError, error_message);
      return;
    }
    if (!ExecuteMuonNamespaceSetupScript(context, namespace_object,
                                         plugin_namespace)) {
      return;
    }
  }
#if defined(MUON_TEST_BUILD)
  {
    CefRefPtr<CefV8Value> muon_namespace;
    std::string diagnostic_error_message;
    if (!GetOrCreateMuonNamespaceObject(
            global, "muon", readonly_attribute, &muon_namespace,
            &diagnostic_error_message)) {
      LogMuonMessage(kMuonLogSourceMuon, kMuonLogLevelError,
                     diagnostic_error_message);
      return;
    }
    if (muon_namespace->HasValue(
            kMuonV8FunctionWrapperDiagnosticsFunctionName)) {
      LogMuonMessage(
          kMuonLogSourceMuon, kMuonLogLevelError,
          "Function wrapper diagnostic conflicts with existing member");
      return;
    }
    const auto diagnostic_attribute =
        static_cast<CefV8Value::PropertyAttribute>(
            V8_PROPERTY_ATTRIBUTE_READONLY |
            V8_PROPERTY_ATTRIBUTE_DONTDELETE |
            V8_PROPERTY_ATTRIBUTE_DONTENUM);
    if (!muon_namespace->SetValue(
            kMuonV8FunctionWrapperDiagnosticsFunctionName,
            CefV8Value::CreateFunction(
                kMuonV8FunctionWrapperDiagnosticsFunctionName, handler),
            diagnostic_attribute)) {
      LogMuonMessage(kMuonLogSourceMuon, kMuonLogLevelError,
                     "Failed to create function wrapper diagnostic API");
      return;
    }
  }
#endif
  std::string error_message;
  if (!FreezeMuonPluginDefinitions(context, renderer_metadata_,
                                   &error_message)) {
    LogMuonMessage(kMuonLogSourceMuon, kMuonLogLevelError, error_message);
    return;
  }
  v8_handlers_by_context_[handler->GetContextId()] = handler;
}

void MuonApp::OnContextReleased(CefRefPtr<CefBrowser> browser,
                                 CefRefPtr<CefFrame> frame,
                                 CefRefPtr<CefV8Context> context) {
  CEF_REQUIRE_RENDERER_THREAD();
  auto handler_iterator = v8_handlers_by_context_.end();
  for (auto iterator = v8_handlers_by_context_.begin();
       iterator != v8_handlers_by_context_.end(); ++iterator) {
    if (iterator->second->IsForContext(context)) {
      handler_iterator = iterator;
      break;
    }
  }
  if (handler_iterator == v8_handlers_by_context_.end()) {
    return;
  }

  handler_iterator->second->RejectAllPendingPromises();
  handler_iterator->second->ReleaseFunctionReferences();
  v8_handlers_by_context_.erase(handler_iterator);
}

bool MuonApp::OnProcessMessageReceived(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    CefProcessId source_process,
    CefRefPtr<CefProcessMessage> message) {
  CEF_REQUIRE_RENDERER_THREAD();
  if (source_process != PID_BROWSER) {
    return false;
  }

  const auto message_name = message->GetName().ToString();
  if (message_name != kMuonPluginResultSharedMessageName &&
      message_name != kMuonPluginResultMessageName &&
      message_name != kMuonRendererFunctionCallSharedMessageName &&
      message_name != kMuonRendererFunctionCallMessageName &&
      message_name != kMuonRendererFunctionSourceAcquireMessageName &&
      message_name != kMuonRendererFunctionSourceReleaseMessageName &&
      message_name != kMuonRendererFunctionResultConsumedMessageName
#if defined(MUON_TEST_BUILD)
      && message_name != kMuonFunctionWrapperDiagnosticsResultMessageName
#endif
  ) {
    return false;
  }
  const auto context_id = GetMuonResultContextId(message);
  const auto handler_iterator = v8_handlers_by_context_.find(context_id);
  if (handler_iterator == v8_handlers_by_context_.end()) {
    return true;
  }
  if (message_name == kMuonPluginResultSharedMessageName) {
    return handler_iterator->second->HandleResultSharedMessage(message);
  }
  if (message_name == kMuonPluginResultMessageName) {
    return handler_iterator->second->HandleResultMessage(message);
  }
  if (message_name == kMuonRendererFunctionCallSharedMessageName) {
    return handler_iterator->second->HandleRendererFunctionCallSharedMessage(
        message);
  }
  if (message_name == kMuonRendererFunctionCallMessageName) {
    return handler_iterator->second->HandleRendererFunctionCallMessage(message);
  }
  if (message_name == kMuonRendererFunctionSourceAcquireMessageName) {
    return handler_iterator->second
        ->HandleRendererFunctionSourceAcquireMessage(message);
  }
  if (message_name == kMuonRendererFunctionSourceReleaseMessageName) {
    return handler_iterator->second
        ->HandleRendererFunctionSourceReleaseMessage(message);
  }
  if (message_name == kMuonRendererFunctionResultConsumedMessageName) {
    return handler_iterator->second
        ->HandleRendererFunctionResultConsumedMessage(message);
  }
#if defined(MUON_TEST_BUILD)
  if (message_name == kMuonFunctionWrapperDiagnosticsResultMessageName) {
    return handler_iterator->second
        ->HandleFunctionWrapperDiagnosticsResultMessage(message);
  }
#endif
  return false;
}
