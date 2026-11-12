/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "muon_service_startup.h"
#include "muon_service_policy.h"
#include "config/muon_autostart.h"
#include "muon_filesystem.h"
#include <yyjson.h>
#include <iostream>

static std::string StartupString(yyjson_val* value) {
  if (!yyjson_is_str(value)) throw std::invalid_argument("Invalid login startup string");
  auto result = std::string(yyjson_get_str(value), yyjson_get_len(value));
  if (result.find('\0') != std::string::npos) throw std::invalid_argument("Login startup cannot contain NUL");
  return result;
}

struct LoginRegistration {
  std::function<cardio::promise<void>(bool)> apply;
  bool refresh;
};

static LoginRegistration ParseStartup(const std::string& startup) {
  std::optional<MuonAutostartOptions> registration;
  bool refresh = false;
  if (!startup.empty()) {
    auto document = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>(
        yyjson_read(startup.data(), startup.size(), 0), yyjson_doc_free);
    auto* root = yyjson_doc_get_root(document.get());
    if (!yyjson_is_obj(root)) throw std::invalid_argument("Invalid login startup metadata");
    registration.emplace();
    registration->executable_path = muon_internal::FilesystemPathFromUtf8(
        StartupString(yyjson_obj_get(root, "executable")));
    registration->launch_source = StartupString(yyjson_obj_get(root, "source"));
    registration->appimage_id = StartupString(yyjson_obj_get(root, "id"));
    auto* arguments = yyjson_obj_get(root, "arguments");
    if (!yyjson_is_arr(arguments)) throw std::invalid_argument("Invalid login startup arguments");
    size_t index, count;
    yyjson_val* value;
    yyjson_arr_foreach(arguments, index, count, value)
      registration->arguments.push_back(StartupString(value));
    auto* requested = yyjson_obj_get(root, "refresh");
    if (!yyjson_is_bool(requested)) throw std::invalid_argument("Invalid login registration mode");
    refresh = yyjson_get_bool(requested);
  }
  return {[registration](bool enabled) -> cardio::promise<void> {
    if (!registration) co_return;
    co_await RunMuonServiceMetadata([registration, enabled] {
      std::string error;
      if (!SetMuonAutostart(*registration, enabled, &error)) throw std::runtime_error(error);
    });
  }, refresh};
}

cardio::promise<std::unique_ptr<MuonServiceLoginPolicy>> CreateMuonServiceLoginPolicy(
    std::filesystem::path directory, std::vector<MuonServiceDefinition> definitions,
    std::string startup) {
  const auto registration = ParseStartup(startup);
  std::map<std::string, bool> values;
  for (const auto& definition : definitions)
    values.emplace(definition.id, definition.user_autostart.value_or(definition.default_autostart));
  values.emplace("", (co_await LoadMuonServiceAutostart(directory / ".ui-autostart")).value_or(false));
  auto result = std::make_unique<MuonServiceLoginPolicy>(std::move(values),
      [directory](std::string id, bool enabled) {
        return SaveMuonServiceAutostart(id.empty() ? directory / ".ui-autostart" :
            directory / ".autostart" / id, enabled);
      }, registration.apply, [](const std::string& error) { std::cerr << "Login registration cleanup: " << error << '\n'; });
  if (registration.refresh) co_await result->Reconcile();
  co_return result;
}

cardio::promise<void> ConfigureMuonServiceLoginPolicy(MuonServiceLoginPolicy& policy, std::string startup) {
  auto registration = ParseStartup(startup);
  co_await policy.Configure(std::move(registration.apply), registration.refresh);
}
