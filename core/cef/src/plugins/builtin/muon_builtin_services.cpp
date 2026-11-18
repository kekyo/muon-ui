/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "plugins/builtin/muon_builtin_services.h"
#include "plugins/builtin/muon_builtin_completion.h"
#include "services/muon_service_session.h"
#include "config/muon_config.h"
#include "config/muon_paths.h"
#include "config/muon_autostart.h"
#include "config/muon_startup.h"
#include "muon_filesystem.h"
#include "muon_service_renderer_generated.h"
#include "sha2.h"
#include <yyjson.h>
#include <array>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <system_error>
#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

static std::shared_ptr<MuonServiceSession> session;
static bool shutdown_requested = false;

static cardio::promise<void> Start(std::shared_ptr<MuonServiceSession> state) {
  try { co_await state->Start(); }
  catch (const std::exception& error) { std::fprintf(stderr, "muon services startup: %s\n", error.what()); }
}

static cardio::promise<void> Invoke(std::shared_ptr<MuonServiceSession> state,
    muon_completion_func completion, std::string operation, std::string json, std::string owner) {
  try {
    if (!state) throw std::runtime_error("Services are disabled");
    muon_internal::CompleteMuonString(completion,
        co_await state->Call(std::move(owner), std::move(operation), std::move(json)));
  } catch (const std::exception& error) { muon_internal::CompleteMuonError(completion, error.what()); }
}

static void Acquire(muon_completion_func completion, const char* json, const char* owner) {
  cardio::fire_and_forget(Invoke(session, completion, "acquire", json ? json : "", owner ? owner : ""));
}
static void Release(muon_completion_func completion, const char* json, const char* owner) {
  cardio::fire_and_forget(Invoke(session, completion, "release", json ? json : "", owner ? owner : ""));
}
static void GetState(muon_completion_func completion, const char* json, const char* owner) {
  cardio::fire_and_forget(Invoke(session, completion, "getState", json ? json : "", owner ? owner : ""));
}
static void SetAutostart(muon_completion_func completion, const char* json, const char* owner) {
  cardio::fire_and_forget(Invoke(session, completion, "setAutostart", json ? json : "", owner ? owner : ""));
}
static void WatchState(muon_completion_func completion, const char* json, const char* owner) {
  cardio::fire_and_forget(Invoke(session, completion, "watchState", json ? json : "", owner ? owner : ""));
}
static void Rpc(muon_completion_func completion, const char* json, const char* owner) {
  cardio::fire_and_forget(Invoke(session, completion, "rpc", json ? json : "", owner ? owner : ""));
}
static void ReleaseOwner(const char* owner) { if (session && owner) session->ReleaseOwner(owner); }

static cardio::promise<void> StopAsync(std::shared_ptr<MuonServiceSession> state,
    bool shutdown, muon_plugin_stop_completion completion, void* data) {
  try { if (state) co_await state->Stop(shutdown); }
  catch (const std::exception& error) { std::fprintf(stderr, "muon services shutdown: %s\n", error.what()); }
  completion(data);
}
static void Stop(muon_plugin_stop_completion completion, void* data) {
  cardio::fire_and_forget(StopAsync(session, shutdown_requested, completion, data));
}

static const muon_type_descriptor string_type{MUON_TYPE_STRING, nullptr};
static const muon_type_descriptor arguments[] = {string_type, string_type};
static const muon_plugin_function_metadata acquire_function = {
    "__acquire", reinterpret_cast<muon_native_function>(&Acquire), {2, arguments, &string_type}, "acquire"};
static const muon_plugin_function_metadata release_function = {
    "__release", reinterpret_cast<muon_native_function>(&Release), {2, arguments, &string_type}, "release"};
static const muon_plugin_function_metadata state_function = {
    "__getState", reinterpret_cast<muon_native_function>(&GetState), {2, arguments, &string_type}, "getState"};
static const muon_plugin_function_metadata autostart_function = {
    "__setAutostart", reinterpret_cast<muon_native_function>(&SetAutostart), {2, arguments, &string_type}, "setAutostart"};
static const muon_plugin_function_metadata watch_function = {
    "__watchState", reinterpret_cast<muon_native_function>(&WatchState), {2, arguments, &string_type}, "watchState"};
static const muon_plugin_function_metadata rpc_function = {
    "__rpc", reinterpret_cast<muon_native_function>(&Rpc), {2, arguments, &string_type}, "rpc"};
static const muon_plugin_function_metadata* const functions[] = {
    &acquire_function, &release_function, &state_function, &autostart_function, &watch_function, &rpc_function, nullptr};
static const muon_plugin_namespace service_namespace{"muon.services", kMuonServiceSetupScript, functions};
static const muon_plugin_namespace* const namespaces[] = {&service_namespace, nullptr};
static const muon_plugin_metadata metadata{namespaces, &Stop, &ReleaseOwner};

static std::string IdentityDigest(const std::string& identity) {
  SHA256_CTX hash{};
  std::array<uint8_t, SHA256_DIGEST_LENGTH> bytes{};
  if (!SHA256_Init(&hash) || !SHA256_Update(&hash, reinterpret_cast<const uint8_t*>(identity.data()), identity.size()) ||
      !SHA256_Final(bytes.data(), &hash)) throw std::runtime_error("Cannot resolve service endpoint identity");
  std::string digest;
  for (const auto byte : bytes) {
    digest.push_back("0123456789abcdef"[byte >> 4]);
    digest.push_back("0123456789abcdef"[byte & 15]);
  }
  return digest;
}

static std::string StartupMetadata(const MuonConfig& config,
    const std::vector<std::filesystem::path>& paths, const std::string& identity, bool refresh) {
  if (config.service_development) return {};
  MuonAutostartOptions options;
  std::string error;
  if (!CreateDefaultMuonAutostartOptions(&options, &error)) throw std::runtime_error(error);
  if (options.launch_source != kMuonLaunchSourceNone && options.launch_source != kMuonLaunchSourceNormal)
    return {};
  if (options.appimage_id.empty() && !config.launcher_path.empty()) options.executable_path = config.launcher_path;
  // The registration identifies the persistent profile, not a particular login
  // session. The IPC endpoint below additionally isolates OS login sessions.
  options.appimage_id = config.app_id + "-" + IdentityDigest(identity).substr(0, 16);
  auto document = std::unique_ptr<yyjson_mut_doc, decltype(&yyjson_mut_doc_free)>(yyjson_mut_doc_new(nullptr), yyjson_mut_doc_free);
  auto* root = yyjson_mut_obj(document.get());
  yyjson_mut_doc_set_root(document.get(), root);
  yyjson_mut_obj_add_strcpy(document.get(), root, "executable",
      muon_internal::FilesystemPathToUtf8(options.executable_path).c_str());
  yyjson_mut_obj_add_strcpy(document.get(), root, "source", options.launch_source.c_str());
  yyjson_mut_obj_add_strcpy(document.get(), root, "id", options.appimage_id.c_str());
  yyjson_mut_obj_add_bool(document.get(), root, "refresh", refresh);
  auto* arguments = yyjson_mut_arr(document.get());
  yyjson_mut_obj_add_val(document.get(), root, "arguments", arguments);
  yyjson_mut_arr_add_str(document.get(), arguments, "--muon-autostart");
  yyjson_mut_arr_add_strcpy(document.get(), arguments, ("--muon-launch-from=" + options.launch_source).c_str());
  for (const auto& path : paths) {
    yyjson_mut_arr_add_str(document.get(), arguments, "-c");
    yyjson_mut_arr_add_strcpy(document.get(), arguments, muon_internal::FilesystemPathToUtf8(path).c_str());
  }
  size_t size = 0;
  auto json = std::unique_ptr<char, decltype(&std::free)>(yyjson_mut_write(document.get(), 0, &size), std::free);
  if (!json) throw std::bad_alloc();
  return {json.get(), size};
}

std::shared_ptr<MuonServiceSession> CreateMuonBuiltinServiceSession(const MuonConfig& config,
    const std::vector<std::filesystem::path>& config_paths, bool refresh, bool login_launch) {
  if (!config.service_endpoint.empty()) {
    // The development command owns startup and shutdown. A stale page must
    // never spawn a replacement after that supervisor has ended its session.
    return std::make_shared<MuonServiceSession>(MuonServiceProcessOptions{
        "", muon_internal::FilesystemPathToUtf8(config.service_registry), config.service_endpoint,
        muon_internal::FilesystemPathToUtf8(config.browser.profile / "services"), "", true});
  }
  auto identity = config.app_id + "\n" + muon_internal::FilesystemPathToUtf8(config.browser.profile.lexically_normal());
  const auto startup = StartupMetadata(config, config_paths, identity, refresh);
#if defined(_WIN32)
  DWORD logon_session = 0;
  if (!ProcessIdToSessionId(GetCurrentProcessId(), &logon_session))
    throw std::system_error(GetLastError(), std::system_category(), "get service user session");
  identity += "\n" + std::to_string(logon_session);
#else
  if (const auto* logon_session = std::getenv("XDG_SESSION_ID")) identity += "\n" + std::string(logon_session);
#endif
  const auto digest = IdentityDigest(identity);
#if defined(_WIN32)
  const auto endpoint = "\\\\.\\pipe\\muon-service-" + digest;
  const auto executable = GetMuonExecutableDirectory() / "muon-service-host.exe";
#else
  const auto directory = "/tmp/muon-svc-" + std::to_string(geteuid()) + "-" + digest;
  const auto endpoint = directory + "/control";
  const auto executable = GetMuonExecutableDirectory() / "muon-service-host";
#endif
  return std::make_shared<MuonServiceSession>(MuonServiceProcessOptions{
      muon_internal::FilesystemPathToUtf8(executable),
      muon_internal::FilesystemPathToUtf8(config.service_registry), endpoint,
      muon_internal::FilesystemPathToUtf8(config.browser.profile / "services"), startup, false, login_launch});
}

void InitializeMuonBuiltinServices(const MuonConfig& config,
    const std::vector<std::filesystem::path>& config_paths) {
  shutdown_requested = false;
  const auto arguments = GetMuonStartupCommandLine();
  const auto login = std::find(arguments.begin(), arguments.end(), "--muon-autostart") != arguments.end();
  session = CreateMuonBuiltinServiceSession(config, config_paths, !login, login);
  cardio::fire_and_forget(Start(session));
}

void GetMuonBuiltinUiAutostart(muon_completion_func completion) {
  cardio::fire_and_forget(Invoke(session, completion, "getUiAutostart", "{}", "@ui-policy"));
}

static cardio::promise<void> SetUiAutostart(std::shared_ptr<MuonServiceSession> state,
    muon_completion_func completion, bool enabled) {
  try {
    if (!state) throw std::runtime_error("UI service session is unavailable");
    co_await state->Call("@ui-policy", "setUiAutostart", enabled ? "{\"enabled\":true}" : "{\"enabled\":false}");
    muon_internal::CompleteMuonVoid(completion);
  } catch (const std::exception& error) { muon_internal::CompleteMuonError(completion, error.what()); }
}

void SetMuonBuiltinUiAutostart(muon_completion_func completion, bool enabled) {
  cardio::fire_and_forget(SetUiAutostart(session, completion, enabled));
}

const muon_plugin_metadata* GetMuonBuiltinServicesMetadata() { return session ? &metadata : nullptr; }
void RequestMuonBuiltinServicesShutdown() { shutdown_requested = true; }
void ShutdownMuonBuiltinServices() { session.reset(); }
