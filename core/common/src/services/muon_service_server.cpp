/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 */
#include "services/muon_service_server.h"
#include "services/muon_service_http.h"
#include "server/muon_server_service.h"
#include "muon_json_helpers.h"
#include <set>

static bool ServiceOrigin(const std::string& origin) {
  try {
    if (origin.size() > 2048) return false;
    if (origin.starts_with("asset://")) {
      const auto host = origin.substr(8);
      return !host.empty() && host.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789.-") == std::string::npos;
    }
    return ParseMuonServiceHttpUrl(origin).origin == origin;
  } catch (const std::exception&) { return false; }
}
static cardio::promise<void> CloseListener(std::shared_ptr<MuonServerService> service, std::string id) {
  try {
    std::string options = "{\"operation\":\"close\",\"id\":";
    muon_internal::AppendJsonString(&options, id);
    co_await service->dispatch(options + "}", {}, "service");
  } catch (const std::exception&) { /* Another close can already have removed the listener. */ }
}
static cardio::promise<void> CloseAll(std::shared_ptr<MuonServerService> service, std::function<void()> drained) {
  try { co_await service->stop(); } catch (const std::exception&) { /* The listener has already stopped accepting. */ }
  drained();
}
struct ServiceServerCall { std::string id; bool cancelled = false; };
static cardio::promise<void> DispatchServiceServer(std::shared_ptr<MuonServerService> service,
    std::shared_ptr<ServiceServerCall> call, MuonServiceIoRequest request, std::function<void(MuonServiceIoReply)> complete) {
  MuonServiceIoReply result;
  try {
    const auto operation = request.operation.substr(7);
    static const std::set<std::string> operations{"create","attach","listen","next","respond","close","ws_accept","ws_send","ws_close","ws_ack","abort"};
    if (!operations.contains(operation)) throw std::invalid_argument("Unknown server operation");
    muon_internal::MuonJsonDocument doc; yyjson_val* root = nullptr; std::string error;
    if (!muon_internal::ParseJsonObjectOptions(request.parameters.c_str(), &doc, &root, &error) || yyjson_obj_get(root, "operation"))
      throw std::invalid_argument("Invalid service server parameters");
    if (auto id = yyjson_obj_get(root, "id"); yyjson_is_str(id)) call->id = muon_internal::ReadJsonString(id);
    if (auto text = yyjson_obj_get(root, "text")) {
      if (!yyjson_is_str(text) || !request.body.empty() || yyjson_get_len(text) > MuonHttpBodyLimit) throw std::invalid_argument("Invalid server text");
      const auto value = muon_internal::ReadJsonString(text); request.body.assign(value.begin(), value.end());
    }
    // Serialize parsed fields so caller-supplied whitespace or an embedded operation cannot alter dispatch.
    std::string json = "{\"operation\":\"" + operation + "\"";
    size_t i, n; yyjson_val *key, *value;
    yyjson_obj_foreach(root, i, n, key, value) {
      const auto name = muon_internal::ReadJsonString(key);
      if (name == "text") continue;
      json += ','; muon_internal::AppendJsonString(&json, name); json += ':';
      const auto encoded = std::unique_ptr<char, decltype(&std::free)>(yyjson_val_write(value, 0, nullptr), std::free);
      if (!encoded) throw std::bad_alloc();
      json += encoded.get();
    }
    const auto packet = co_await service->dispatch(json + '}', std::move(request.body), "service");
    if (packet.size() < 4) throw std::runtime_error("Invalid server response");
    const auto length = (size_t(packet[0]) << 24) | (size_t(packet[1]) << 16) | (size_t(packet[2]) << 8) | size_t(packet[3]);
    if (length > packet.size() - 4) throw std::runtime_error("Invalid server response length");
    result.metadata.assign(packet.begin() + 4, packet.begin() + 4 + length);
    result.body.assign(packet.begin() + 4 + length, packet.end());
    if (operation == "create" || (operation == "next" && result.metadata != "null")) {
      muon_internal::MuonJsonDocument metadata; yyjson_val* fields = nullptr;
      if (!muon_internal::ParseJsonObjectOptions(result.metadata.c_str(), &metadata, &fields, &error)) throw std::runtime_error(error);
      if (operation == "create") {
        call->id = muon_internal::ReadJsonString(yyjson_obj_get(fields, "id"));
        if (call->cancelled) co_await CloseListener(service, call->id);
      } else if (muon_internal::ReadJsonString(yyjson_obj_get(fields, "type")) == "ws_message" && !yyjson_get_bool(yyjson_obj_get(fields, "binary"))) {
        result.metadata.pop_back(); result.metadata += ",\"text\":";
        muon_internal::AppendJsonString(&result.metadata, std::string(result.body.begin(), result.body.end())); result.metadata += '}';
      }
    }
  } catch (const std::exception& error) { result.code = "io-error"; result.message = error.what(); }
  complete(std::move(result));
}
std::shared_ptr<MuonServiceServer> CreateMuonServiceListeners(const std::string& policy) {
  muon_internal::MuonJsonDocument doc; yyjson_val* root = nullptr; std::string error;
  if (!muon_internal::ParseJsonObjectOptions(policy.c_str(), &doc, &root, &error) ||
      !yyjson_is_true(yyjson_obj_get(yyjson_obj_get(root, "server"), "listen"))) return {};
  const auto service = CreateMuonServerService(ServiceOrigin);
  auto backend = std::make_shared<MuonServiceServer>();
  backend->run = [service](MuonServiceIoRequest request, std::function<void(MuonServiceIoReply)> complete) {
    auto call = std::make_shared<ServiceServerCall>();
    cardio::fire_and_forget(DispatchServiceServer(service, call, std::move(request), std::move(complete)));
    return [service, call] {
      call->cancelled = true;
      if (!call->id.empty()) cardio::fire_and_forget(CloseListener(service, call->id));
    };
  };
  backend->close = [service](std::function<void()> drained) { cardio::fire_and_forget(CloseAll(service, std::move(drained))); };
  return backend;
}
