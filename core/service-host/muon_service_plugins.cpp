/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "muon_service_plugins.h"
#include "muon_filesystem.h"
#include <cardio.h>
#include <iostream>
#include <set>
#include <stdexcept>
#include <thread>
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

static std::string Text(yyjson_val* value) {
  if (!yyjson_is_str(value)) throw std::invalid_argument("Expected a service plugin string");
  const auto result = std::string(yyjson_get_str(value), yyjson_get_len(value));
  if (result.find('\0') != std::string::npos) throw std::invalid_argument("Service plugin string contains NUL");
  return result;
}

static std::vector<std::string> Strings(yyjson_val* value) {
  if (!yyjson_is_arr(value)) throw std::invalid_argument("Expected service plugin function paths");
  std::vector<std::string> result;
  size_t index, count;
  yyjson_val* item;
  yyjson_arr_foreach(value,index,count,item) {
    auto text = Text(item);
    if (text.empty()) throw std::invalid_argument("Empty service plugin function path");
    result.push_back(std::move(text));
  }
  return result;
}

std::unique_ptr<MuonServicePluginPool> CreateMuonDesktopServicePlugins(
    yyjson_val* registry, const std::filesystem::path& directory,
    cardio::dispatcher& dispatcher, std::function<void(std::function<void()>)> post) {
  std::vector<MuonServicePluginDefinition> definitions;
  size_t index, count;
  yyjson_val *key, *value;
  yyjson_obj_foreach(yyjson_obj_get(registry,"services"),index,count,key,value) {
    auto* allow = yyjson_obj_get(value,"plugins");
    if (!allow) continue;
    if (Text(yyjson_obj_get(value,"runtime")) != "quickjs")
      throw std::invalid_argument("Native module grants require QuickJS services");
    const auto paths = Strings(allow);
    if (!paths.empty()) definitions.push_back({Text(key),paths});
  }
  auto* plugins = yyjson_obj_get(registry,"plugins");
  if (!plugins && definitions.empty()) return {};
  if (!yyjson_is_arr(plugins)) throw std::invalid_argument("Missing service native plugin registry");
  std::set<std::string> names;
  std::vector<MuonPluginRuntimeLoadEntry> entries;
  yyjson_arr_foreach(plugins,index,count,value) {
    if (!yyjson_is_obj(value)) throw std::invalid_argument("Invalid service native plugin entry");
    MuonPluginRuntimeLoadEntry entry;
    entry.plugin = Text(yyjson_obj_get(value,"name"));
    if (entry.plugin.empty() || entry.plugin == "internal" ||
        entry.plugin.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos ||
        !names.insert(entry.plugin).second)
      throw std::invalid_argument("Invalid or duplicate service native plugin name");
    const auto path = muon_internal::ResolveFilesystemPath(directory,
        muon_internal::FilesystemPathFromUtf8(Text(yyjson_obj_get(value,"library"))));
#if defined(_WIN32)
    const auto filename = entry.plugin + ".dll";
#else
    const auto filename = entry.plugin + ".so";
#endif
    if (muon_internal::FilesystemPathToUtf8(path.filename()) != filename)
      throw std::invalid_argument("Service native plugin library must match its registration name");
    entry.has_library_directory = true;
    entry.library_directory = path.parent_path();
    entry.expected_signature = Text(yyjson_obj_get(value,"signature"));
    if (entry.expected_signature.size() != 64 ||
        entry.expected_signature.find_first_not_of("0123456789abcdef") != std::string::npos ||
        !Text(yyjson_obj_get(value,"salt")).empty())
      throw std::invalid_argument("Service native plugins require an artifact SHA-256 signature");
    entry.has_expected_signature = entry.has_signature_salt = true;
    auto error = std::string{};
    if (!CreateMuonPluginPolicy(Strings(yyjson_obj_get(value,"allow")),&entry.plugin_policy,&error))
      throw std::invalid_argument(error);
    entry.has_expected_function_paths = true;
    entry.expected_function_paths = Strings(yyjson_obj_get(value,"expectedFunctions"));
    if (auto* config = yyjson_obj_get(value,"config")) {
      if (!yyjson_is_obj(config)) throw std::invalid_argument("Invalid native plugin configuration");
      size_t setting_index, setting_count;
      yyjson_val *setting_key, *setting_value;
      yyjson_obj_foreach(config,setting_index,setting_count,setting_key,setting_value)
        entry.config.push_back({Text(setting_key),Text(setting_value)});
    }
    entries.push_back(std::move(entry));
  }
  if (entries.empty() && definitions.empty()) return {};
  if (entries.empty() || definitions.empty()) throw std::invalid_argument("Service native plugin grants and registry disagree");
  MuonPluginRuntimeServices services;
  services.dispatcher = &dispatcher;
  // GLib's dispatcher host is not marked thread-bound by the base class. This
  // process nevertheless runs its one owner dispatcher on the creating thread.
  services.is_owner_thread = [owner = std::this_thread::get_id()] {
    return std::this_thread::get_id() == owner;
  };
  services.post_owner_task = [post = std::move(post)](std::function<void()> task) {
    post(std::move(task)); return true;
  };
  services.allocate_buffer = [](size_t size,std::string*) { return CreateMuonRpcOwnedBuffer(size); };
  services.emit_log = [](MuonPluginRuntimeLogSource,muon_log_level,const std::string& text) {
    std::cerr << "service plugin: " << text << '\n';
  };
#if defined(_WIN32)
  services.open_library = [](const std::string& locator,std::string* error) {
    const auto path = muon_internal::FilesystemPathForIo(muon_internal::FilesystemPathFromUtf8(locator),
        muon_internal::FilesystemIoUsage::File);
    auto* library = LoadLibraryExW(path.c_str(),nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!library && error) *error = "Cannot load service plugin: Windows error " + std::to_string(GetLastError());
    return static_cast<void*>(library);
  };
  services.find_symbol = [](void* library,const char* name) {
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(library),name));
  };
  services.close_library = [](void* library) { FreeLibrary(static_cast<HMODULE>(library)); };
#else
  services.open_library = [](const std::string& locator,std::string* error) {
    dlerror();
    auto* library = dlopen(locator.c_str(),RTLD_NOW | RTLD_LOCAL);
    if (!library && error) {
      const auto diagnostic = dlerror();
      *error = diagnostic ? diagnostic : "Cannot load service native plugin";
    }
    return library;
  };
  services.find_symbol = [](void* library,const char* name) { return dlsym(library,name); };
  services.close_library = [](void* library) { dlclose(library); };
#endif
  return std::make_unique<MuonServicePluginPool>(directory,std::move(entries),std::move(definitions),std::move(services));
}
