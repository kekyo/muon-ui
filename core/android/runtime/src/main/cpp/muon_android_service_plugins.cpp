/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "muon_android_service_plugins.h"
#include "muon_cardio_post.h"
#include <android/log.h>
#include <dlfcn.h>
#include <yyjson.h>
#include <set>
#include <stdexcept>
#include <thread>

static std::string Text(yyjson_val* value) {
  if (!yyjson_is_str(value)) throw std::invalid_argument("Expected a service plugin string");
  auto result = std::string(yyjson_get_str(value), yyjson_get_len(value));
  if (result.find('\0') != std::string::npos)
    throw std::invalid_argument("Service plugin string contains NUL");
  return result;
}

static std::vector<std::string> Strings(yyjson_val* value) {
  if (!yyjson_is_arr(value)) throw std::invalid_argument("Expected service plugin function paths");
  std::vector<std::string> result;
  size_t index, count;
  yyjson_val* item;
  yyjson_arr_foreach(value, index, count, item) {
    auto text = Text(item);
    if (text.empty()) throw std::invalid_argument("Empty service plugin function path");
    result.push_back(std::move(text));
  }
  return result;
}

std::unique_ptr<MuonServicePluginPool> CreateMuonAndroidServicePlugins(
    const std::string& registry, std::vector<MuonServicePluginDefinition> definitions,
    cardio::dispatcher& dispatcher) {
  const auto document = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>(
      yyjson_read(registry.data(), registry.size(), 0), yyjson_doc_free);
  auto* plugins = document ? yyjson_doc_get_root(document.get()) : nullptr;
  if (!yyjson_is_arr(plugins)) throw std::invalid_argument("Invalid service plugin registry");
  std::set<std::string> names;
  std::vector<MuonPluginRuntimeLoadEntry> entries;
  size_t index, count;
  yyjson_val* value;
  yyjson_arr_foreach(plugins, index, count, value) {
    MuonPluginRuntimeLoadEntry entry;
    entry.plugin = Text(yyjson_obj_get(value, "name"));
    if (entry.plugin.empty() || entry.plugin == "internal" ||
        entry.plugin.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos ||
        !names.insert(entry.plugin).second)
      throw std::invalid_argument("Invalid or duplicate service plugin name");
    // APK signatures cover these bytes. The class loader resolves extracted and
    // uncompressed ZIP libraries identically to the existing page plugin loader.
    entry.has_library_locator = true;
    entry.library_locator = Text(yyjson_obj_get(value, "library"));
    if (entry.library_locator.empty() || entry.library_locator.front() != '/')
      throw std::invalid_argument("Service plugin library is not an APK-resolved locator");
    auto error = std::string{};
    if (!CreateMuonPluginPolicy(Strings(yyjson_obj_get(value, "allow")), &entry.plugin_policy, &error))
      throw std::invalid_argument(error);
    entry.has_expected_function_paths = true;
    entry.expected_function_paths = Strings(yyjson_obj_get(value, "expectedFunctions"));
    auto* config = yyjson_obj_get(value, "config");
    if (!yyjson_is_arr(config)) throw std::invalid_argument("Invalid service plugin configuration");
    size_t config_index, config_count;
    yyjson_val* setting;
    yyjson_arr_foreach(config, config_index, config_count, setting)
      entry.config.push_back({Text(yyjson_obj_get(setting, "key")), Text(yyjson_obj_get(setting, "value"))});
    entries.push_back(std::move(entry));
  }
  if (entries.empty() && definitions.empty()) return {};
  if (entries.empty() || definitions.empty())
    throw std::invalid_argument("Service native plugin grants and registry disagree");
  MuonPluginRuntimeServices services;
  services.dispatcher = &dispatcher;
  services.is_owner_thread = [owner = std::this_thread::get_id()] {
    return std::this_thread::get_id() == owner;
  };
  services.post_owner_task = [&dispatcher](std::function<void()> task) {
    // Native completion may arrive on another thread. It must never use the
    // process's main-thread JNIEnv or Handler bookkeeping directly.
    muon_internal::FireAndForgetOnDispatcher(&dispatcher, std::move(task));
    return true;
  };
  services.allocate_buffer = [](size_t size, std::string*) { return CreateMuonRpcOwnedBuffer(size); };
  services.emit_log = [](MuonPluginRuntimeLogSource, muon_log_level level, const std::string& text) {
    const int priorities[] = {ANDROID_LOG_DEBUG, ANDROID_LOG_INFO, ANDROID_LOG_WARN,
                             ANDROID_LOG_ERROR, ANDROID_LOG_FATAL};
    const auto index = static_cast<unsigned>(level);
    __android_log_write(index < 5 ? priorities[index] : ANDROID_LOG_INFO, "MuonServicePlugin", text.c_str());
  };
  services.open_library = [](const std::string& locator, std::string* error) {
    dlerror();
    auto* library = dlopen(locator.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!library && error) {
      const auto diagnostic = dlerror();
      *error = diagnostic ? diagnostic : "Cannot load service native plugin";
    }
    return library;
  };
  services.find_symbol = [](void* library, const char* name) { return dlsym(library, name); };
  services.close_library = [](void* library) { dlclose(library); };
  return std::make_unique<MuonServicePluginPool>(std::filesystem::path{}, std::move(entries),
      std::move(definitions), std::move(services));
}
