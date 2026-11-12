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
#include "muon_filesystem.h"
#include "muon_service_renderer_generated.h"
#include "sha2.h"
#include <array>
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
static const muon_plugin_function_metadata* const functions[] = {
    &acquire_function, &release_function, &state_function, nullptr};
static const muon_plugin_namespace service_namespace{"muon.services", kMuonServiceSetupScript, functions};
static const muon_plugin_namespace* const namespaces[] = {&service_namespace, nullptr};
static const muon_plugin_metadata metadata{namespaces, &Stop, &ReleaseOwner};

void InitializeMuonBuiltinServices(const MuonConfig& config) {
  session.reset();
  shutdown_requested = false;
  if (config.service_registry.empty()) return;
  auto identity = config.app_id + "\n" + muon_internal::FilesystemPathToUtf8(config.browser.profile.lexically_normal());
#if defined(_WIN32)
  DWORD logon_session = 0;
  if (!ProcessIdToSessionId(GetCurrentProcessId(), &logon_session))
    throw std::system_error(GetLastError(), std::system_category(), "get service user session");
  identity += "\n" + std::to_string(logon_session);
#else
  if (const auto* logon_session = std::getenv("XDG_SESSION_ID")) identity += "\n" + std::string(logon_session);
#endif
  SHA256_CTX hash{};
  std::array<uint8_t, SHA256_DIGEST_LENGTH> bytes{};
  if (!SHA256_Init(&hash) || !SHA256_Update(&hash, reinterpret_cast<const uint8_t*>(identity.data()), identity.size()) ||
      !SHA256_Final(bytes.data(), &hash)) throw std::runtime_error("Cannot resolve service endpoint identity");
  std::string digest;
  for (const auto byte : bytes) {
    digest.push_back("0123456789abcdef"[byte >> 4]);
    digest.push_back("0123456789abcdef"[byte & 15]);
  }
#if defined(_WIN32)
  const auto endpoint = "\\\\.\\pipe\\muon-service-" + digest;
  const auto executable = GetMuonExecutableDirectory() / "muon-service-host.exe";
#else
  const auto directory = "/tmp/muon-svc-" + std::to_string(geteuid()) + "-" + digest;
  const auto endpoint = directory + "/control";
  const auto executable = GetMuonExecutableDirectory() / "muon-service-host";
#endif
  session = std::make_shared<MuonServiceSession>(MuonServiceProcessOptions{
      muon_internal::FilesystemPathToUtf8(executable),
      muon_internal::FilesystemPathToUtf8(config.service_registry), endpoint,
      muon_internal::FilesystemPathToUtf8(config.browser.profile / "services")});
  cardio::fire_and_forget(Start(session));
}

const muon_plugin_metadata* GetMuonBuiltinServicesMetadata() { return session ? &metadata : nullptr; }
void RequestMuonBuiltinServicesShutdown() { shutdown_requested = true; }
void ShutdownMuonBuiltinServices() { session.reset(); }
