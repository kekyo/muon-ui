/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "server/muon_server_service.h"
#include "muon_cardio_post.h"
#include "muon_json_helpers.h"
#include "include/cef_parser.h"
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
  CefServer::HeaderMap response_headers;
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
  std::string bearer_token;
  std::set<std::string> allowed_origins;
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
static bool IsServerHeaderName(const std::string& name) {
  return !name.empty() && std::all_of(name.begin(), name.end(), [](unsigned char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') || std::string("!#$%&'*+-.^_`|~").find(ch) != std::string::npos;
  });
}
static bool IsServerBearerToken(const std::string& token) {
  if (token.empty() || token.size() > 4096) return false;
  bool padding = false;
  for (size_t i = 0; i < token.size(); ++i) {
    const auto ch = token[i];
    if (ch == '=') {
      if (i == 0) return false;
      padding = true;
    } else if (padding || !((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') || std::string("-._~+/").find(ch) != std::string::npos)) return false;
  }
  return true;
}
static bool IsServerOrigin(const std::string& origin) {
  if (origin.size() > 2048 || origin.find('*') != std::string::npos) return false;
  CefURLParts parts;
  if (!CefParseURL(origin, parts)) return false;
  const auto scheme = CefString(&parts.scheme).ToString();
  const auto host = CefString(&parts.host).ToString();
  const auto port = CefString(&parts.port).ToString();
  if ((scheme != "http" && scheme != "https" && scheme != "asset") || host.empty()) return false;
  // Requiring the serialized origin rejects paths, userinfo, whitespace and
  // opaque origins rather than silently broadening a caller's allowlist.
  const auto serialized = scheme + "://" + host + (port.empty() ? "" : ":" + port);
  return origin == serialized;
}
static CefServer::HeaderMap ServerResponseHeaders(const std::string& origin) {
  auto headers = CefServer::HeaderMap{{"Connection", "close"}, {"Vary", "Origin"}};
  if (!origin.empty()) {
    headers.emplace("Access-Control-Allow-Origin", origin);
    // Cookies and credentialed CORS are not enabled. Explicit Authorization
    // headers work with the default fetch credentials mode.
    headers.emplace("Access-Control-Expose-Headers", "*");
  }
  return headers;
}
static void ServerError(const std::shared_ptr<ServerState>& state,
                        int connection, int status,
                        CefServer::HeaderMap headers = ServerResponseHeaders("")) {
  if (state->server && state->connections.contains(connection)) {
    headers.emplace("Cache-Control", "no-store");
    if (status == 401) headers.emplace("WWW-Authenticate", "Bearer");
    state->server->SendHttpResponse(connection, status, "text/plain", 0, headers);
  }
}

struct ServerAccess {
  // Zero delegates to the page; a nonzero status completes natively.
  int status;
  CefServer::HeaderMap headers;
};
static bool HasServerBearer(const std::string& authorization, const std::string& token) {
  const auto separator = authorization.find(' ');
  if (separator == std::string::npos || LowerServerHeader(authorization.substr(0, separator)) != "bearer") return false;
  const auto start = authorization.find_first_not_of(' ', separator);
  if (start == std::string::npos || authorization.size() - start != token.size()) return false;
  // Check every byte of an equal-length token; do not reveal a matching prefix.
  unsigned char difference = 0;
  for (size_t i = 0; i < token.size(); ++i)
    difference |= static_cast<unsigned char>(authorization[start + i]) ^ static_cast<unsigned char>(token[i]);
  return difference == 0;
}
static ServerAccess CheckServerAccess(const ServerState& state,
                                      const std::string& method,
                                      const ServerHeaders& headers) {
  auto hosts = size_t{0};
  auto origins = size_t{0};
  auto authorizations = size_t{0};
  auto preflight_methods = size_t{0};
  auto preflight_headers = size_t{0};
  auto origin = std::string{};
  auto authorization = std::string{};
  auto requested_method = std::string{};
  auto requested_headers = std::string{};
  bool allowed = true;
  const auto port = ":" + std::to_string(state.port);
  for (const auto& [name, value] : headers) {
    const auto lower = LowerServerHeader(name);
    if (lower == "host") {
      ++hosts;
      if (LowerServerHeader(value) != state.host + port &&
          LowerServerHeader(value) != "localhost" + port) allowed = false;
    } else if (lower == "origin") { ++origins; origin = value; }
    else if (lower == "authorization") { ++authorizations; authorization = value; }
    else if (lower == "access-control-request-method") { ++preflight_methods; requested_method = value; }
    else if (lower == "access-control-request-headers") { ++preflight_headers; requested_headers = value; }
  }
  if (!allowed || hosts != 1 || origins > 1 ||
      (origins == 1 && !state.allowed_origins.contains(origin)))
    return {403, ServerResponseHeaders("")};
  auto response = ServerResponseHeaders(origin);
  if (method == "OPTIONS" && (preflight_methods || preflight_headers)) {
    const auto lower_method = LowerServerHeader(requested_method);
    if (origins != 1 || preflight_methods != 1 || preflight_headers > 1 ||
        !IsServerHeaderName(requested_method) || requested_method == "*" ||
        lower_method == "connect" || lower_method == "trace" || lower_method == "track" ||
        requested_headers.size() > 8192) return {400, std::move(response)};
    if (preflight_headers) {
      auto start = size_t{0};
      auto count = size_t{0};
      do {
        const auto end = requested_headers.find(',', start);
        auto name = requested_headers.substr(start, end == std::string::npos ? end : end - start);
        const auto first = name.find_first_not_of(" \t");
        if (first == std::string::npos) return {400, std::move(response)};
        name = name.substr(first, name.find_last_not_of(" \t") - first + 1);
        if (!IsServerHeaderName(name) || name == "*" || ++count > 64) return {400, std::move(response)};
        if (end == std::string::npos) break;
        start = end + 1;
      } while (true);
      // Explicitly list Authorization; CORS does not treat it as a wildcard.
      response.emplace("Access-Control-Allow-Headers", requested_headers);
    }
    response.emplace("Access-Control-Allow-Methods", requested_method);
    response.emplace("Vary", "Access-Control-Request-Method, Access-Control-Request-Headers");
    return {204, std::move(response)};
  }
  if (!state.bearer_token.empty() &&
      (authorizations != 1 || !HasServerBearer(authorization, state.bearer_token)))
    return {401, std::move(response)};
  return {0, std::move(response)};
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
    ServerError(state, request.connection, 503, request.response_headers);
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
      auto access = CheckServerAccess(*state, method, headers);
      if (access.status) { ServerError(state, connection, access.status, access.headers); return; }
      if (oversized) { ServerError(state, connection, 413, access.headers); return; }
      if (!state->binding || state->requests.size() >= kMaxServerRequests ||
          state->binding->events.size() >= kMaxServerRequests * 2) {
        ServerError(state, connection, 503, access.headers); return;
      }
      if (std::any_of(state->requests.begin(), state->requests.end(),
          [connection](const auto& entry) { return entry.second.connection == connection; })) {
        state->server->CloseConnection(connection); return;
      }
      if (!path.starts_with(state->url + "/")) { ServerError(state, connection, 400, access.headers); return; }
      auto header_json = std::string("[");
      for (const auto& [name, value] : headers) {
        if (header_json.size() > 1) header_json += ',';
        header_json += '[' + QuoteServerString(name) + ',' + QuoteServerString(value) + ']';
      }
      header_json += ']';
      const auto request_id = std::to_string(++state->next_request);
      state->requests.emplace(request_id, ServerRequest{connection, method, std::move(access.headers)});
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
  auto extra = found->second.response_headers;
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
    else if (lower != "content-length" && lower != "transfer-encoding" && lower != "connection" &&
        !lower.starts_with("access-control-")) extra.emplace(name, value);
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
    if (yyjson_obj_get(options, "bearerToken")) {
      state->bearer_token = RequiredServerString(options, "bearerToken");
      if (!IsServerBearerToken(state->bearer_token)) throw std::runtime_error("Invalid server bearer token format");
    }
    if (const auto origins = yyjson_obj_get(options, "allowedOrigins")) {
      if (!yyjson_is_arr(origins) || yyjson_arr_size(origins) > 64) throw std::runtime_error("Invalid server origin allowlist");
      for (size_t i = 0; i < yyjson_arr_size(origins); ++i) {
        const auto value = yyjson_arr_get(origins, i);
        if (!yyjson_is_str(value)) throw std::runtime_error("Invalid server origin");
        const auto origin = muon_internal::ReadJsonString(value);
        if (!IsServerOrigin(origin)) throw std::runtime_error("Server origin must be a serialized http, https or asset origin");
        state->allowed_origins.insert(origin);
      }
    }
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
          ServerError(state, request->second.connection, 500, request->second.response_headers);
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
