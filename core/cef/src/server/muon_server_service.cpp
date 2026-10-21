/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "server/muon_server_service.h"
#include "muon_cardio_post.h"
#include "muon_json_helpers.h"
#include "include/cef_server.h"
#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

using ServerBytes = std::vector<uint8_t>;
using ServerHeaders = std::vector<std::pair<std::string, std::string>>;
static constexpr size_t kMaxServerBody = 1024 * 1024;
static constexpr size_t kMaxServerRequests = 32;

struct ServerRequest {
  int connection;
  std::string method;
};
struct ServerBinding {
  std::string token;
  std::deque<ServerBytes> events;
  std::shared_ptr<cardio::promise_source<ServerBytes>> waiting;
};
struct ServerState {
  cardio::dispatcher* dispatcher;
  std::string id;
  std::string owner;
  std::string name;
  std::string host;
  uint16_t port;
  std::string url;
  std::unique_ptr<ServerBinding> binding;
  uint64_t next_binding = 0;
  uint64_t next_request = 0;
  std::map<std::string, ServerRequest> requests;
  std::set<int> connections;
  CefRefPtr<CefServer> server;
  bool starting = false;
  bool closing = false;
  std::shared_ptr<cardio::promise_source<void>> started;
  std::vector<std::shared_ptr<cardio::promise_source<void>>> stopped;
};
struct ServerServiceState {
  cardio::dispatcher* dispatcher;
  uint64_t next_server = 0;
  bool stopping = false;
  std::map<std::string, std::shared_ptr<ServerState>> servers;
};

static std::string QuoteServerString(const std::string& value) {
  std::string result;
  muon_internal::AppendJsonString(&result, value);
  return result;
}
static std::string RequiredServerString(yyjson_val* root, const char* key) {
  const auto value = yyjson_obj_get(root, key);
  if (!yyjson_is_str(value)) throw std::runtime_error("Missing server string option");
  return muon_internal::ReadJsonString(value);
}
static ServerBytes ServerEnvelope(const std::string& json, const ServerBytes& body = {}) {
  const auto size = static_cast<uint32_t>(json.size());
  auto result = ServerBytes{static_cast<uint8_t>(size >> 24),
      static_cast<uint8_t>(size >> 16), static_cast<uint8_t>(size >> 8),
      static_cast<uint8_t>(size)};
  result.insert(result.end(), json.begin(), json.end());
  result.insert(result.end(), body.begin(), body.end());
  return result;
}
static std::string LowerServerHeader(std::string value) {
  for (auto& ch : value) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
  return value;
}
static void ServerError(const std::shared_ptr<ServerState>& state,
                        int connection, int status) {
  if (state->server && state->connections.contains(connection))
    state->server->SendHttpResponse(connection, status, "text/plain", 0,
                                   {{"Connection", "close"}});
}
static void QueueServerEvent(const std::shared_ptr<ServerState>& state, ServerBytes event) {
  if (!state->binding) return;
  auto& binding = *state->binding;
  if (binding.waiting) {
    auto pending = std::move(binding.waiting);
    pending->resolve(std::move(event));
  } else {
    binding.events.push_back(std::move(event));
  }
}
static void DetachServer(const std::shared_ptr<ServerState>& state) {
  if (state->binding && state->binding->waiting) {
    auto pending = std::move(state->binding->waiting);
    pending->resolve(ServerEnvelope("null"));
  }
  state->binding.reset();
  for (const auto& [id, request] : state->requests) {
    (void)id;
    ServerError(state, request.connection, 503);
  }
  state->requests.clear();
}
static void BeginServerClose(const std::shared_ptr<ServerState>& state) {
  if (state->closing) return;
  state->closing = true;
  DetachServer(state);
  if (state->server) state->server->Shutdown();
}
static cardio::promise<void> CloseServer(std::shared_ptr<ServerState> state) {
  BeginServerClose(state);
  if (!state->server && !state->starting) co_return;
  auto source = std::make_shared<cardio::promise_source<void>>();
  auto pending = source->get_promise();
  state->stopped.push_back(source);
  co_await pending;
}

// CEF callbacks copy borrowed request data before crossing to the owning
// dispatcher. No JavaScript callback or mutable service state runs on CEF's
// dedicated server thread.
class MuonHttpServerHandler final : public CefServerHandler {
 public:
  explicit MuonHttpServerHandler(std::shared_ptr<ServerState> state) : state_(std::move(state)) {}
  void OnServerCreated(CefRefPtr<CefServer> server) override {
    const bool running = server->IsRunning();
    Post([state = state_, server, running] {
      if (running) state->server = server;
      if (running && state->closing) server->Shutdown();
      if (running) {
        state->starting = false;
        auto pending = std::move(state->started);
        if (state->closing) pending->reject(std::make_exception_ptr(std::runtime_error("Server closed during startup")));
        else pending->resolve();
      }
      // A failed start completes in OnServerDestroyed, so retry cannot overlap
      // the failed server's lifetime.
    });
  }
  void OnServerDestroyed(CefRefPtr<CefServer>) override {
    Post([state = state_] {
      state->server = nullptr;
      state->starting = false;
      state->connections.clear();
      if (state->started) {
        auto pending = std::move(state->started);
        pending->reject(std::make_exception_ptr(std::runtime_error("Cannot listen on the requested loopback port")));
      }
      auto stopped = std::move(state->stopped);
      for (const auto& pending : stopped) pending->resolve();
    });
  }
  void OnClientConnected(CefRefPtr<CefServer>, int connection) override {
    Post([state = state_, connection] {
      state->connections.insert(connection);
      if (state->closing || state->connections.size() > kMaxServerRequests)
        state->server->CloseConnection(connection);
    });
  }
  void OnClientDisconnected(CefRefPtr<CefServer>, int connection) override {
    Post([state = state_, connection] {
      state->connections.erase(connection);
      for (auto it = state->requests.begin(); it != state->requests.end();) {
        if (it->second.connection == connection) {
          QueueServerEvent(state, ServerEnvelope("{\"type\":\"disconnect\",\"requestId\":" + QuoteServerString(it->first) + "}"));
          it = state->requests.erase(it);
        } else ++it;
      }
    });
  }
  void OnHttpRequest(CefRefPtr<CefServer>, int connection,
                     const CefString&, CefRefPtr<CefRequest> request) override {
    auto headers = ServerHeaders{};
    CefRequest::HeaderMap incoming;
    request->GetHeaderMap(incoming);
    for (const auto& [name, value] : incoming) headers.emplace_back(name.ToString(), value.ToString());
    auto body = ServerBytes{};
    bool oversized = false;
    if (auto post = request->GetPostData()) {
      CefPostData::ElementVector elements;
      post->GetElements(elements);
      for (const auto& element : elements) {
        if (element->GetType() != PDE_TYPE_BYTES) { oversized = true; break; }
        const auto size = element->GetBytesCount();
        if (size > kMaxServerBody - body.size()) { oversized = true; break; }
        const auto offset = body.size();
        body.resize(offset + size);
        element->GetBytes(size, body.data() + offset);
      }
    }
    Post([state = state_, connection, headers = std::move(headers),
          body = std::move(body), oversized, method = request->GetMethod().ToString(),
          path = request->GetURL().ToString()] {
      if (!state->connections.contains(connection) || state->closing) return;
      auto hosts = size_t{0};
      bool allowed = true;
      const auto authority = state->host + ":" + std::to_string(state->port);
      for (const auto& [name, value] : headers) {
        const auto lower = LowerServerHeader(name);
        if (lower == "host") {
          ++hosts;
          if (LowerServerHeader(value) != authority &&
              LowerServerHeader(value) != "localhost:" + std::to_string(state->port)) allowed = false;
        } else if (lower == "origin") allowed = false;
      }
      if (!allowed || hosts != 1) { ServerError(state, connection, 403); return; }
      if (oversized) { ServerError(state, connection, 413); return; }
      if (!state->binding || state->requests.size() >= kMaxServerRequests ||
          state->binding->events.size() >= kMaxServerRequests * 2) {
        ServerError(state, connection, 503); return;
      }
      if (std::any_of(state->requests.begin(), state->requests.end(),
          [connection](const auto& entry) { return entry.second.connection == connection; })) {
        state->server->CloseConnection(connection); return;
      }
      if (!path.starts_with(state->url + "/")) { ServerError(state, connection, 400); return; }
      auto header_json = std::string("[");
      for (const auto& [name, value] : headers) {
        if (header_json.size() > 1) header_json += ',';
        header_json += '[' + QuoteServerString(name) + ',' + QuoteServerString(value) + ']';
      }
      header_json += ']';
      const auto request_id = std::to_string(++state->next_request);
      state->requests.emplace(request_id, ServerRequest{connection, method});
      QueueServerEvent(state, ServerEnvelope("{\"type\":\"request\",\"requestId\":" +
          QuoteServerString(request_id) + ",\"method\":" + QuoteServerString(method) +
          ",\"url\":" + QuoteServerString(path) +
          ",\"headers\":" + header_json + "}", body));
    });
  }
  void OnWebSocketRequest(CefRefPtr<CefServer>, int, const CefString&,
                         CefRefPtr<CefRequest>, CefRefPtr<CefCallback> callback) override { callback->Cancel(); }
  void OnWebSocketConnected(CefRefPtr<CefServer>, int) override {}
  void OnWebSocketMessage(CefRefPtr<CefServer>, int, const void*, size_t) override {}
 private:
  void Post(std::function<void()> callback) {
    muon_internal::FireAndForgetOnDispatcher(state_->dispatcher, std::move(callback));
  }
  std::shared_ptr<ServerState> state_;
  IMPLEMENT_REFCOUNTING(MuonHttpServerHandler);
};

static bool IsServerHeaderName(const std::string& name) {
  return !name.empty() && std::all_of(name.begin(), name.end(), [](unsigned char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') || std::string("!#$%&'*+-.^_`|~").find(ch) != std::string::npos;
  });
}
static void RespondToServerRequest(const std::shared_ptr<ServerState>& state,
                                  yyjson_val* root, const ServerBytes& body) {
  const auto request_id = RequiredServerString(root, "requestId");
  const auto found = state->requests.find(request_id);
  if (found == state->requests.end()) throw std::runtime_error("Server request is no longer active");
  const auto status = yyjson_obj_get(root, "status");
  if (!yyjson_is_uint(status) || yyjson_get_uint(status) < 200 || yyjson_get_uint(status) > 599)
    throw std::runtime_error("Invalid HTTP response status");
  if (body.size() > kMaxServerBody) throw std::runtime_error("Server response body exceeds 1 MiB");
  const auto headers = yyjson_obj_get(root, "headers");
  if (!yyjson_is_arr(headers)) throw std::runtime_error("Invalid HTTP response headers");
  auto extra = CefServer::HeaderMap{{"Connection", "close"}};
  auto content_type = std::string("application/octet-stream");
  for (size_t i = 0; i < yyjson_arr_size(headers); ++i) {
    const auto pair = yyjson_arr_get(headers, i);
    if (!yyjson_is_arr(pair) || yyjson_arr_size(pair) != 2 ||
        !yyjson_is_str(yyjson_arr_get(pair, 0)) || !yyjson_is_str(yyjson_arr_get(pair, 1)))
      throw std::runtime_error("Invalid HTTP response header pair");
    const auto name = muon_internal::ReadJsonString(yyjson_arr_get(pair, 0));
    const auto value = muon_internal::ReadJsonString(yyjson_arr_get(pair, 1));
    if (!IsServerHeaderName(name) || value.find_first_of("\r\n") != std::string::npos || value.find('\0') != std::string::npos)
      throw std::runtime_error("Invalid HTTP response header");
    const auto lower = LowerServerHeader(name);
    if (lower == "content-type") content_type = value;
    else if (lower != "content-length" && lower != "transfer-encoding" && lower != "connection") extra.emplace(name, value);
  }
  const auto connection = found->second.connection;
  const auto code = static_cast<int>(yyjson_get_uint(status));
  const auto no_body = found->second.method == "HEAD" || code == 204 || code == 205 || code == 304;
  state->server->SendHttpResponse(connection, code, content_type, no_body ? 0 : body.size(), extra);
  if (!no_body && !body.empty()) {
    state->server->SendRawData(connection, body.data(), body.size());
    state->server->CloseConnection(connection);
  }
  state->requests.erase(found);
}

static cardio::promise<ServerBytes> DispatchServer(
    std::shared_ptr<ServerServiceState> service, std::string json,
    ServerBytes body, std::string owner) {
  if (service->stopping || owner.empty()) throw std::runtime_error("Server service is unavailable");
  muon_internal::MuonJsonDocument document;
  yyjson_val* root = nullptr;
  std::string error;
  if (!muon_internal::ParseJsonObjectOptions(json.c_str(), &document, &root, &error)) throw std::runtime_error(error);
  const auto operation = RequiredServerString(root, "operation");
  if (operation == "create") {
    const auto options = yyjson_obj_get(root, "options");
    if (!yyjson_is_obj(options)) throw std::runtime_error("Missing server options");
    const auto name = RequiredServerString(options, "name");
    const auto port = yyjson_obj_get(options, "port");
    const auto host_value = yyjson_obj_get(options, "host");
    const auto host = host_value ? RequiredServerString(options, "host") : "127.0.0.1";
    if (name.empty() || name.size() > 128 || name.find('\0') != std::string::npos) throw std::runtime_error("Invalid server name");
    if (host != "127.0.0.1") throw std::runtime_error("Server host must be 127.0.0.1");
    if (!yyjson_is_uint(port) || yyjson_get_uint(port) < 1025 || yyjson_get_uint(port) > 65535) throw std::runtime_error("Server port must be between 1025 and 65535");
    for (const auto& [id, existing] : service->servers) {
      (void)id;
      if (existing->owner == owner && existing->name == name && !existing->closing) throw std::runtime_error("Server name is already in use");
    }
    if (service->servers.size() >= 32) throw std::runtime_error("Server capacity is full");
    auto state = std::make_shared<ServerState>();
    state->dispatcher = service->dispatcher;
    state->id = std::to_string(++service->next_server);
    state->owner = owner;
    state->name = name;
    state->host = host;
    state->port = static_cast<uint16_t>(yyjson_get_uint(port));
    state->url = "http://" + host + ":" + std::to_string(state->port);
    service->servers.emplace(state->id, state);
    co_return ServerEnvelope("{\"id\":" + QuoteServerString(state->id) + ",\"name\":" + QuoteServerString(name) + "}");
  }
  const auto id = RequiredServerString(root, "id");
  const auto found = service->servers.find(id);
  if (found == service->servers.end()) {
    if (operation == "close" || operation == "next") co_return ServerEnvelope("null");
    throw std::runtime_error("Server is closed");
  }
  auto state = found->second;
  if (state->owner != owner) throw std::runtime_error("Server belongs to another page");
  if (operation == "close") {
    co_await CloseServer(state);
    service->servers.erase(id);
    co_return ServerEnvelope("null");
  }
  if (state->closing) {
    if (operation == "next") co_return ServerEnvelope("null");
    throw std::runtime_error("Server is closing");
  }
  if (operation == "attach") {
    if (state->binding) throw std::runtime_error("Server already has a handler binding");
    state->binding = std::make_unique<ServerBinding>();
    state->binding->token = std::to_string(++state->next_binding);
    co_return ServerEnvelope("{\"binding\":" + QuoteServerString(state->binding->token) + "}");
  }
  if (!state->binding || RequiredServerString(root, "binding") != state->binding->token) {
    if (operation == "next") co_return ServerEnvelope("null");
    throw std::runtime_error("Server handler binding is no longer active");
  }
  if (operation == "detach") {
    DetachServer(state);
    co_return ServerEnvelope("null");
  }
  if (operation == "listen") {
    if (state->server || state->starting) throw std::runtime_error("Server is already listening or starting");
    state->starting = true;
    state->started = std::make_shared<cardio::promise_source<void>>();
    auto pending = state->started->get_promise();
    CefServer::CreateServer(state->host, state->port, 16, new MuonHttpServerHandler(state));
    co_await pending;
    co_return ServerEnvelope("{\"url\":" + QuoteServerString(state->url) + ",\"host\":" + QuoteServerString(state->host) + ",\"port\":" + std::to_string(state->port) + "}");
  }
  if (operation == "next") {
    if (!state->binding->events.empty()) {
      auto event = std::move(state->binding->events.front());
      state->binding->events.pop_front();
      co_return event;
    }
    if (state->binding->waiting) throw std::runtime_error("Server binding already has an event wait");
    state->binding->waiting = std::make_shared<cardio::promise_source<ServerBytes>>();
    auto pending = state->binding->waiting->get_promise();
    co_return co_await pending;
  }
  if (operation == "respond") {
    try {
      RespondToServerRequest(state, root, body);
    } catch (...) {
      // A malformed application response must not strand an accepted request.
      // Expired requests and bindings never affect another request or binding.
      const auto value = yyjson_obj_get(root, "requestId");
      if (yyjson_is_str(value)) {
        const auto request = state->requests.find(muon_internal::ReadJsonString(value));
        if (request != state->requests.end()) {
          ServerError(state, request->second.connection, 500);
          state->requests.erase(request);
        }
      }
      throw;
    }
    co_return ServerEnvelope("null");
  }
  throw std::runtime_error("Unknown server operation");
}

static cardio::promise<void> ReleaseServerOwner(
    std::shared_ptr<ServerServiceState> service, std::string owner) {
  auto owned = std::vector<std::shared_ptr<ServerState>>{};
  for (const auto& [id, state] : service->servers) {
    (void)id;
    if (state->owner == owner) { BeginServerClose(state); owned.push_back(state); }
  }
  for (const auto& state : owned) {
    co_await CloseServer(state);
    service->servers.erase(state->id);
  }
}
static cardio::promise<void> StopServerService(std::shared_ptr<ServerServiceState> service) {
  service->stopping = true;
  auto states = service->servers;
  for (const auto& [id, state] : states) { (void)id; BeginServerClose(state); }
  for (const auto& [id, state] : states) { (void)id; co_await CloseServer(state); }
  service->servers.clear();
}
std::shared_ptr<MuonServerService> CreateMuonServerService() {
  auto state = std::make_shared<ServerServiceState>();
  state->dispatcher = &cardio::get_current_dispatcher();
  return std::make_shared<MuonServerService>(MuonServerService{
      [state](std::string json, ServerBytes body, std::string owner) {
        return DispatchServer(state, std::move(json), std::move(body), std::move(owner));
      },
      [state](const std::string& owner) { cardio::fire_and_forget(ReleaseServerOwner(state, owner)); },
      [state] { return StopServerService(state); }});
}
