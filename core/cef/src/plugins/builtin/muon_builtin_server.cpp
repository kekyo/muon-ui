/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "plugins/builtin/muon_builtin_server.h"
#include "plugins/builtin/muon_builtin_completion.h"
#include "server/muon_server_service.h"
#include "muon_server_renderer_generated.h"
#include <cstdio>
#include <cstring>

static std::shared_ptr<MuonServerService> server_service;
static const muon_plugin_helpers* server_helpers;

static cardio::promise<void> DispatchAsync(std::shared_ptr<MuonServerService> service,
    muon_completion_func completion, std::string json,
    std::vector<uint8_t> body, std::string owner) {
  try {
    if (!service) throw std::runtime_error("Server service is unavailable");
    auto result = co_await service->dispatch(std::move(json), std::move(body), std::move(owner));
    auto view = muon_buffer_view{nullptr, 0};
    auto handle = muon_shared_buffer_handle{};
    char message[MUON_COMPLETION_ERROR_MESSAGE_CAPACITY] = {};
    auto error = muon_error_buffer{message, sizeof(message)};
    if (!server_helpers->allocate_shared_buffer(result.size(), &view, &handle, &error))
      throw std::runtime_error(message[0] ? message : "Cannot allocate server result");
    std::memcpy(view.data, result.data(), result.size());
    // Transfer host-owned storage: completion is delivered on a later dispatch.
    completion(&view, nullptr);
  } catch (const std::exception& error) {
    muon_internal::CompleteMuonError(completion, error.what());
  } catch (...) {
    muon_internal::CompleteMuonError(completion, "Server operation failed");
  }
}
static void Dispatch(muon_completion_func completion, const char* json,
                     muon_buffer_view body, const char* owner) {
  auto bytes = std::vector<uint8_t>{};
  if (body.size) {
    const auto begin = static_cast<const uint8_t*>(body.data);
    bytes.assign(begin, begin + body.size);
  }
  cardio::fire_and_forget(DispatchAsync(server_service, completion,
      json ? json : "", std::move(bytes), owner ? owner : ""));
}
static cardio::promise<void> StopAsync(std::shared_ptr<MuonServerService> service,
    muon_plugin_stop_completion completion, void* data) {
  try { if (service) co_await service->stop(); }
  catch (const std::exception& error) {
    std::fprintf(stderr, "muon server shutdown failed: %s\n", error.what());
  }
  completion(data);
}
static void Stop(muon_plugin_stop_completion completion, void* data) {
  cardio::fire_and_forget(StopAsync(server_service, completion, data));
}
static void ReleaseOwner(const char* owner) {
  if (server_service) server_service->release_owner(owner ? owner : "");
}
static const muon_type_descriptor buffer_type = {MUON_TYPE_BUFFER_VIEW, nullptr};
static const muon_type_descriptor arguments[] = {
    {MUON_TYPE_STRING, nullptr}, buffer_type, {MUON_TYPE_STRING, nullptr}};
static const muon_plugin_function_metadata dispatch_function = {
    "__dispatch", reinterpret_cast<muon_native_function>(&Dispatch),
    {3, arguments, &buffer_type}, "createServer"};
static const muon_plugin_function_metadata* const functions[] = {&dispatch_function, nullptr};
static const muon_plugin_namespace server_namespace = {"muon.server", kMuonServerSetupScript, functions};
static const muon_plugin_namespace* const namespaces[] = {&server_namespace, nullptr};
static const muon_plugin_metadata metadata = {namespaces, &Stop, &ReleaseOwner};

void InitializeMuonBuiltinServer(const muon_plugin_init_context* context) {
  server_helpers = context->helpers;
  server_service = CreateMuonServerService();
}
const muon_plugin_metadata* GetMuonBuiltinServerMetadata() { return &metadata; }
void ShutdownMuonBuiltinServer() {
  server_service.reset();
  server_helpers = nullptr;
}
