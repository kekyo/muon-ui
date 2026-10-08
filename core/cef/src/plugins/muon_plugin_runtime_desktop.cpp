/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "plugins/muon_plugin_runtime.h"

#include "browser/muon_builtin_browser.h"
#include "config/muon_paths.h"
#include "log/muon_log.h"
#include "plugins/builtin/muon_builtin.h"
#include "plugins/builtin/muon_builtin_executor.h"
#include "plugins/builtin/muon_builtin_fs.h"
#include "plugins/builtin/muon_builtin_fs_dialogs_plugin.h"

#include <cardio.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

static MuonLogLevel ConvertDesktopLogLevel(muon_log_level level) {
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

static void AddMetadataNamespaces(
    const muon_plugin_metadata* metadata,
    std::vector<std::string>* namespaces) {
  if (metadata == nullptr || namespaces == nullptr ||
      metadata->namespaces == nullptr) {
    return;
  }
  for (auto entry = metadata->namespaces; *entry != nullptr; ++entry) {
    if ((*entry)->plugin_namespace != nullptr) {
      namespaces->emplace_back((*entry)->plugin_namespace);
    }
  }
}

static std::shared_ptr<MuonPluginRuntimePlatformAdapter>
CreateDesktopPlatformAdapter(
    std::function<void*(void* handle, const char* symbol)> find_symbol) {
  auto adapter = std::make_shared<MuonPluginRuntimePlatformAdapter>();
  AddMetadataNamespaces(GetMuonBuiltinPluginMetadata(),
                        &adapter->reserved_namespaces);
  AddMetadataNamespaces(GetMuonBuiltinFsDialogsPluginMetadata(),
                        &adapter->reserved_namespaces);

  MuonPluginRuntimePlatformNamespace browser_namespace;
  browser_namespace.plugin_namespace =
      GetMuonBuiltinBrowserPluginNamespace();
  browser_namespace.setup_script = GetMuonBuiltinBrowserSetupScript();
  adapter->reserved_namespaces.push_back(
      browser_namespace.plugin_namespace);
  for (const auto& definition : GetMuonBuiltinBrowserFunctionDefinitions()) {
    MuonPluginRuntimePlatformFunction function;
    function.js_name =
        definition.js_name == nullptr ? "" : definition.js_name;
    function.public_name = definition.filter_name == nullptr
                               ? function.js_name
                               : definition.filter_name;
    if (definition.arg_types != nullptr && definition.arg_count > 0) {
      function.arg_types.assign(
          definition.arg_types,
          definition.arg_types + definition.arg_count);
    }
    function.return_type = definition.return_type;
    function.route_id = static_cast<uint32_t>(definition.kind);
    browser_namespace.functions.push_back(std::move(function));
  }
  adapter->namespaces.push_back(std::move(browser_namespace));

  adapter->initialize = [](
      const muon_plugin_init_context* context,
      MuonPluginRuntimePlatformInitialization* initialization,
      std::string* error_message) {
    if (context == nullptr || initialization == nullptr ||
        error_message == nullptr) {
      return false;
    }
    initialization->plugins.clear();
    initialization->cancel_owner_operations.clear();
    auto platform_error = std::string{};
    if (!InitializeMuonBuiltinFs(context, &platform_error)) {
      *error_message =
          "Built-in filesystem plugin failed: " + platform_error;
      return false;
    }
    if (!InitializeMuonBuiltinFsDialogs(context, &platform_error)) {
      ShutdownMuonBuiltinFs();
      *error_message =
          "Built-in filesystem dialogs plugin failed: " + platform_error;
      return false;
    }
    if (!InitializeMuonBuiltinExecutor(
            context, cardio::unsafe_get_current_dispatcher(),
            &platform_error)) {
      ShutdownMuonBuiltinFsDialogs();
      ShutdownMuonBuiltinFs();
      *error_message =
          "Built-in executor plugin failed: " + platform_error;
      return false;
    }
    initialization->plugins.push_back(
        {GetMuonBuiltinPluginMetadata(), "<builtin muon>"});
    initialization->plugins.push_back(
        {GetMuonBuiltinFsDialogsPluginMetadata(),
         "<builtin muon fs dialogs>"});
    initialization->cancel_owner_operations.push_back(
        &muon_builtin_fs_dialogs_cancel_owner_browser);
    return true;
  };
  adapter->shutdown = []() {
    ShutdownMuonBuiltinExecutor();
    ShutdownMuonBuiltinFsDialogs();
    ShutdownMuonBuiltinFs();
  };
  adapter->release_context = [](int renderer_context_id) {
    ReleaseMuonBuiltinExecutorContext(renderer_context_id);
    ReleaseMuonBuiltinFsContext(renderer_context_id);
  };
  adapter->library_loaded = [find_symbol = std::move(find_symbol)](
      void* handle,
      std::vector<std::function<void(int)>>* cancel_owner_operations) {
    if (handle == nullptr || cancel_owner_operations == nullptr ||
        !find_symbol) {
      return;
    }
    const auto address = find_symbol(
        handle, "muon_builtin_fs_dialogs_cancel_owner_browser");
    const auto cancel_owner = reinterpret_cast<void (*)(int)>(address);
    if (cancel_owner != nullptr) {
      cancel_owner_operations->push_back(cancel_owner);
    }
  };
  return adapter;
}

static void InstallDesktopRuntimeServices(
    MuonPluginRuntimeServices* services) {
  services->emit_log = [](
      MuonPluginRuntimeLogSource source,
      muon_log_level level,
      const std::string& message) {
    LogMuonMessage(source == MuonPluginRuntimeLogSource::Plugin
                       ? kMuonLogSourcePlugin
                       : kMuonLogSourceMuon,
                   ConvertDesktopLogLevel(level), message);
  };
#if defined(_WIN32)
  services->open_library = [](
      const std::string& locator,
      std::string* error_message) -> void* {
    const auto path = std::filesystem::path(locator);
    auto* handle = LoadLibraryW(path.wstring().c_str());
    if (handle == nullptr && error_message != nullptr) {
      *error_message = "LoadLibrary failed with error " +
                       std::to_string(GetLastError());
    }
    return handle;
  };
  services->find_symbol = [](void* handle, const char* symbol) -> void* {
    if (handle == nullptr || symbol == nullptr) {
      return nullptr;
    }
    return reinterpret_cast<void*>(
        GetProcAddress(static_cast<HMODULE>(handle), symbol));
  };
  services->close_library = [](void* handle) {
    if (handle != nullptr) {
      FreeLibrary(static_cast<HMODULE>(handle));
    }
  };
#else
  services->open_library = [](
      const std::string& locator,
      std::string* error_message) -> void* {
    dlerror();
    auto* handle = dlopen(locator.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr && error_message != nullptr) {
      const auto* error = dlerror();
      *error_message = error == nullptr ? "dlopen failed" : error;
    }
    return handle;
  };
  services->find_symbol = [](void* handle, const char* symbol) -> void* {
    return handle == nullptr || symbol == nullptr
               ? nullptr
               : dlsym(handle, symbol);
  };
  services->close_library = [](void* handle) {
    if (handle != nullptr) {
      dlclose(handle);
    }
  };
#endif
  services->platform_adapter =
      CreateDesktopPlatformAdapter(services->find_symbol);
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
    std::vector<MuonPluginRuntimeLoadEntry> plugins,
    MuonPluginRuntimeServices services) {
  InstallDesktopRuntimeServices(&services);
  services.dispatcher = &cardio::get_current_dispatcher();
  return std::make_shared<MuonPluginRuntime>(
      ResolveMuonPluginDirectory(plugin_path), std::move(plugins),
      std::move(services));
}
