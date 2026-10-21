/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "server/muon_server_service.h"
#include "server/muon_server_http.h"
#include "server/muon_server_socket.h"
#include "server/muon_server_websocket.h"
#include "muon_json_helpers.h"
#include "include/cef_parser.h"
#include <array>
#include <charconv>
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

struct ServerResponse {
  int status;
  ServerHeaders headers;
  ServerBytes body;
  std::shared_ptr<cardio::promise_source<void>> sent;
  bool streaming = false;
  bool upgraded = false;
};
struct ServerConnection {
  uint64_t id;
  std::shared_ptr<MuonServerSocket> socket;
  std::string request_id;
  std::shared_ptr<cardio::promise_source<ServerResponse>> response;
  bool responding = false;
  std::shared_ptr<cardio::promise_source<void>> stream_end;
  bool writing = false;
  bool stream_complete = false;
  std::string binding;
  MuonServerWebSocketHandshake handshake;
  std::shared_ptr<MuonServerWebSocketCodec> websocket;
  std::string protocol;
  std::shared_ptr<cardio::promise_source<void>> websocket_ack;
  cardio::primitives::mutex websocket_write;
  cardio::cancellation_source websocket_timer;
  bool websocket_ready = false;
  bool websocket_sending = false;
  bool websocket_timer_started = false;
};
struct ServerRequest {
  std::shared_ptr<ServerConnection> connection;
  std::string method;
  ServerHeaders response_headers;
};
struct ServerBinding {
  std::string token;
  std::deque<ServerBytes> events;
  std::shared_ptr<cardio::promise_source<ServerBytes>> waiting;
  bool websocket = false;
};
struct ServerState {
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
  uint64_t next_connection = 0;
  std::map<std::string, ServerRequest> requests;
  std::map<uint64_t, std::shared_ptr<ServerConnection>> connections;
  std::shared_ptr<MuonServerListener> listener;
  bool accepting = false;
  bool closing = false;
  bool stopping_io = false;
  std::vector<std::shared_ptr<cardio::promise_source<void>>> stopped;
};
struct ServerServiceState {
  std::shared_ptr<MuonServerSockets> sockets;
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
static ServerHeaders ServerResponseHeaders(const std::string& origin) {
  auto headers = ServerHeaders{{"Connection", "close"}, {"Vary", "Origin"}};
  if (!origin.empty()) {
    headers.emplace_back("Access-Control-Allow-Origin", origin);
    // Cookies and credentialed CORS are not enabled. Explicit Authorization
    // headers work with the default fetch credentials mode.
    headers.emplace_back("Access-Control-Expose-Headers", "*");
  }
  return headers;
}
static ServerResponse ServerErrorResponse(int status, ServerHeaders headers = ServerResponseHeaders("")) {
  headers.emplace_back("Cache-Control", "no-store");
  if (status == 401) headers.emplace_back("WWW-Authenticate", "Bearer");
  return {status, std::move(headers), {}, {}};
}
static void ServerError(const std::shared_ptr<ServerState>&,
                        const std::shared_ptr<ServerConnection>& connection, int status,
                        ServerHeaders headers = ServerResponseHeaders("")) {
  if (connection->response && !connection->responding) {
    connection->responding = true;
    connection->response->try_resolve(ServerErrorResponse(status, std::move(headers)));
  }
}

struct ServerAccess {
  // Zero delegates to the page; a nonzero status completes natively.
  int status;
  ServerHeaders headers;
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
      response.emplace_back("Access-Control-Allow-Headers", requested_headers);
    }
    response.emplace_back("Access-Control-Allow-Methods", requested_method);
    response.emplace_back("Vary", "Access-Control-Request-Method, Access-Control-Request-Headers");
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
static void CheckServerStopped(const std::shared_ptr<ServerState>& state) {
  if (!state->closing || state->accepting || state->stopping_io || !state->connections.empty()) return;
  auto stopped = std::move(state->stopped);
  for (const auto& pending : stopped) pending->resolve();
}
static void CancelServerWebSocket(const std::shared_ptr<ServerConnection>& connection) {
  if (connection->websocket_ack)
    connection->websocket_ack->try_reject(std::make_exception_ptr(cardio::canceled_exception()));
  (void)connection->websocket_timer.cancel();
}
static void DetachServer(const std::shared_ptr<ServerState>& state) {
  if (state->binding && state->binding->waiting) {
    auto pending = std::move(state->binding->waiting);
    pending->resolve(ServerEnvelope("null"));
  }
  state->binding.reset();
  for (const auto& [id, request] : state->requests) {
    (void)id;
    if (request.connection->responding) {
      CancelServerWebSocket(request.connection);
      if (request.connection->stream_end)
        request.connection->stream_end->try_reject(std::make_exception_ptr(cardio::canceled_exception()));
      cardio::fire_and_forget(request.connection->socket->close());
    } else ServerError(state, request.connection, 503, request.response_headers);
  }
  state->requests.clear();
}
static cardio::promise<void> StopServerIo(std::shared_ptr<ServerState> state) {
  // Take the snapshot before awaiting: connection tasks remove themselves.
  const auto connections = state->connections;
  if (state->listener) co_await state->listener->close();
  state->listener.reset();
  for (const auto& [id, connection] : connections) {
    (void)id;
    CancelServerWebSocket(connection);
    if (connection->response)
      connection->response->try_reject(std::make_exception_ptr(cardio::canceled_exception()));
    if (connection->stream_end)
      connection->stream_end->try_reject(std::make_exception_ptr(cardio::canceled_exception()));
    co_await connection->socket->close();
  }
  state->stopping_io = false;
  CheckServerStopped(state);
}
static void BeginServerClose(const std::shared_ptr<ServerState>& state) {
  if (state->closing) return;
  state->closing = true;
  state->stopping_io = true;
  DetachServer(state);
  cardio::fire_and_forget(StopServerIo(state));
}
static cardio::promise<void> CloseServer(std::shared_ptr<ServerState> state) {
  BeginServerClose(state);
  if (!state->accepting && !state->stopping_io && state->connections.empty()) co_return;
  auto source = std::make_shared<cardio::promise_source<void>>();
  auto pending = source->get_promise();
  state->stopped.push_back(source);
  co_await pending;
}
static cardio::promise<void> WriteServerResponse(
    std::shared_ptr<ServerConnection> connection, const ServerResponse& response) {
  auto header = "HTTP/1.1 " + std::to_string(response.status) + " Response\r\n";
  for (const auto& [name, value] : response.headers) header += name + ": " + value + "\r\n";
  header += response.upgraded ? "\r\n" : response.streaming ? "Transfer-Encoding: chunked\r\n\r\n" :
      "Content-Length: " + std::to_string(response.body.size()) + "\r\n\r\n";
  co_await connection->socket->write(std::span(
      reinterpret_cast<const uint8_t*>(header.data()), header.size()));
  if (!response.body.empty()) co_await connection->socket->write(response.body);
}
static cardio::promise<void> MonitorServerConnection(std::shared_ptr<ServerConnection> connection) {
  auto failed = false;
  try {
    auto bytes = std::array<uint8_t, 1>{};
    const auto size = co_await connection->socket->read(bytes);
    // EOF can be shutdown(SHUT_WR): the peer may still receive our response.
    // No HTTP keep-alive/pipelining is advertised, so further input is rejected.
    if (size) throw std::runtime_error("Unexpected input after HTTP request");
  } catch (...) {
    failed = true;
    if (connection->response) connection->response->try_reject(std::current_exception());
    if (connection->stream_end) connection->stream_end->try_reject(std::current_exception());
  }
  if (failed) co_await connection->socket->close();
}
static cardio::promise<void> DrainServerWebSocket(std::shared_ptr<ServerConnection> connection) {
  // cardio awaits expose the stored value by reference; move the RAII handle
  // out before the temporary promise is released.
  auto lock = std::move(co_await connection->websocket_write.lock());
  while (connection->websocket->status().write_pending) {
    const auto bytes = connection->websocket->take_output();
    if (bytes.empty()) throw std::runtime_error("WebSocket output made no progress");
    co_await connection->socket->write(bytes);
  }
}
static cardio::promise<void> ExpireServerWebSocket(std::shared_ptr<ServerConnection> connection) {
  try {
    co_await cardio::promises::delay(5000, connection->websocket_timer.get_cancellation());
  } catch (const cardio::canceled_exception&) { co_return; }
  CancelServerWebSocket(connection);
  co_await connection->socket->close();
}
static void StartServerWebSocketTimeout(const std::shared_ptr<ServerConnection>& connection) {
  if (connection->websocket_timer_started) return;
  connection->websocket_timer_started = true;
  cardio::fire_and_forget(ExpireServerWebSocket(connection));
}
static cardio::promise<void> DeliverServerWebSocket(
    std::shared_ptr<ServerState> state, std::shared_ptr<ServerConnection> connection,
    const std::string& type, bool binary, const ServerBytes& body) {
  if (!state->binding || state->binding->token != connection->binding)
    throw cardio::canceled_exception();
  connection->websocket_ack = std::make_shared<cardio::promise_source<void>>();
  auto pending = connection->websocket_ack->get_promise();
  QueueServerEvent(state, ServerEnvelope("{\"type\":" + QuoteServerString(type) +
      ",\"requestId\":" + QuoteServerString(connection->request_id) +
      ",\"protocol\":" + QuoteServerString(connection->protocol) +
      ",\"binary\":" + (binary ? "true" : "false") + "}", body));
  co_await pending;
  connection->websocket_ack.reset();
}
static cardio::promise<void> RunServerWebSocket(
    std::shared_ptr<ServerState> state, std::shared_ptr<ServerConnection> connection,
    ServerBytes initial) {
  co_await DeliverServerWebSocket(state, connection, "ws_open", false, {});
  auto buffer = std::array<uint8_t, 16 * 1024>{};
  while (connection->websocket->status().read_enabled) {
    auto messages = std::vector<MuonServerWebSocketMessage>{};
    if (!initial.empty()) {
      messages = connection->websocket->receive(initial);
      initial.clear();
    } else {
      const auto size = co_await connection->socket->read(buffer);
      if (!size) break;
      messages = connection->websocket->receive(std::span(buffer.data(), size));
    }
    co_await DrainServerWebSocket(connection);
    if (connection->websocket->status().close_sent) StartServerWebSocketTimeout(connection);
    for (const auto& message : messages) {
      co_await DeliverServerWebSocket(state, connection, "ws_message", message.binary, message.body);
    }
  }
}
static cardio::promise<void> RunServerConnection(
    std::shared_ptr<ServerState> state, std::shared_ptr<ServerConnection> connection) {
  auto acknowledged = std::shared_ptr<cardio::promise_source<void>>{};
  auto monitor = cardio::resolved();
  auto clean_websocket = false;
  try {
    const auto parser = CreateMuonServerHttpParser();
    auto buffer = std::array<uint8_t, 16 * 1024>{};
    auto parsed = MuonServerHttpParseResult{0, 0, {}};
    auto remaining = ServerBytes{};
    while (!parsed.request && !parsed.error_status) {
      const auto size = co_await connection->socket->read(buffer);
      parsed = size ? parser->consume(std::span(buffer.data(), size)) : parser->finish();
      if (parsed.request && parsed.consumed < size)
        remaining.assign(buffer.begin() + parsed.consumed, buffer.begin() + size);
    }
    auto response = ServerErrorResponse(parsed.error_status ? parsed.error_status : 400);
    if (parsed.request && !state->closing) {
      auto incoming = *parsed.request;
      if (incoming.upgrade) {
        connection->handshake = PrepareMuonServerWebSocket(incoming);
        if (connection->handshake.valid && !connection->handshake.bearer.empty()) {
          if (state->bearer_token.empty() || !IsServerBearerToken(connection->handshake.bearer))
            connection->handshake.valid = false;
          // An existing Authorization becomes a duplicate and is rejected by
          // the common policy; credentials cannot override each other.
          incoming.headers.emplace_back("Authorization", "Bearer " + connection->handshake.bearer);
          connection->handshake.bearer.clear();
        }
      }
      auto access = CheckServerAccess(*state, incoming.method, incoming.headers);
      const auto target = incoming.target.starts_with("/") ? state->url + incoming.target : incoming.target;
      if (access.status) response = ServerErrorResponse(access.status, std::move(access.headers));
      else if (!target.starts_with(state->url + "/") ||
          (incoming.upgrade && !connection->handshake.valid))
        response = ServerErrorResponse(400, std::move(access.headers));
      else if (!state->binding || state->requests.size() >= kMaxServerRequests ||
          state->binding->events.size() >= kMaxServerRequests * 2)
        response = ServerErrorResponse(503, std::move(access.headers));
      else if (incoming.upgrade && !state->binding->websocket)
        response = ServerErrorResponse(400, std::move(access.headers));
      else {
        if (incoming.upgrade) std::erase_if(incoming.headers, [](const auto& header) {
          return LowerServerHeader(header.first) == "authorization";
        });
        auto header_json = std::string("[");
        for (const auto& [name, value] : incoming.headers) {
          if (header_json.size() > 1) header_json += ',';
          header_json += '[' + QuoteServerString(name) + ',' + QuoteServerString(value) + ']';
        }
        header_json += ']';
        connection->request_id = std::to_string(++state->next_request);
        connection->binding = state->binding->token;
        connection->response = std::make_shared<cardio::promise_source<ServerResponse>>();
        auto pending = connection->response->get_promise();
        state->requests.emplace(connection->request_id,
            ServerRequest{connection, incoming.method, std::move(access.headers)});
        QueueServerEvent(state, ServerEnvelope("{\"type\":" +
            QuoteServerString(incoming.upgrade ? "ws_request" : "request") + ",\"requestId\":" +
            QuoteServerString(connection->request_id) + ",\"method\":" + QuoteServerString(incoming.method) +
            ",\"url\":" + QuoteServerString(target) + ",\"headers\":" + header_json + "}", incoming.body));
        if (!incoming.upgrade) monitor = MonitorServerConnection(connection);
        response = co_await pending;
      }
    }
    acknowledged = response.sent;
    co_await WriteServerResponse(connection, response);
    auto stream_finished = cardio::resolved();
    if (response.streaming) {
      connection->stream_end = std::make_shared<cardio::promise_source<void>>();
      stream_finished = connection->stream_end->get_promise();
    }
    if (response.upgraded) connection->websocket_ready = true;
    if (acknowledged) acknowledged->try_resolve();
    if (response.upgraded) {
      co_await RunServerWebSocket(state, connection, std::move(remaining));
      const auto status = connection->websocket->status();
      clean_websocket = status.close_received && status.close_sent;
    } else co_await stream_finished;
  } catch (...) {
    if (acknowledged) acknowledged->try_reject(std::current_exception());
  }
  CancelServerWebSocket(connection);
  connection->websocket_ready = false;
  // Closing cancels the monitor before its buffer and socket can be released.
  co_await connection->socket->close();
  co_await monitor;
  if (!connection->request_id.empty() && state->requests.erase(connection->request_id)) {
    if (state->binding && state->binding->token == connection->binding) {
      if (connection->websocket) {
        const auto status = connection->websocket->status();
        QueueServerEvent(state, ServerEnvelope("{\"type\":\"ws_close\",\"requestId\":" +
            QuoteServerString(connection->request_id) + ",\"code\":" +
            std::to_string(status.close_received ? status.code : 1006) + ",\"reason\":" +
            QuoteServerString(status.close_received ? status.reason : "") +
            ",\"wasClean\":" + (clean_websocket ? "true" : "false") + "}"));
      } else QueueServerEvent(state, ServerEnvelope("{\"type\":\"disconnect\",\"requestId\":" +
          QuoteServerString(connection->request_id) + "}"));
    }
  }
  state->connections.erase(connection->id);
  CheckServerStopped(state);
}
static cardio::promise<void> AcceptServerConnections(std::shared_ptr<ServerState> state) {
  try {
    while (!state->closing) {
      auto socket = co_await state->listener->accept();
      if (state->closing || state->connections.size() >= kMaxServerRequests) {
        co_await socket->close();
        continue;
      }
      auto connection = std::make_shared<ServerConnection>();
      connection->id = ++state->next_connection;
      connection->socket = std::move(socket);
      state->connections.emplace(connection->id, connection);
      cardio::fire_and_forget(RunServerConnection(state, connection));
    }
  } catch (...) {
    // Listener cancellation is expected during close. Unexpected accept errors
    // stop this server as well, rather than leaving a nonfunctional listener.
    if (!state->closing) BeginServerClose(state);
  }
  state->accepting = false;
  CheckServerStopped(state);
}

static cardio::promise<void> RespondToServerRequest(const std::shared_ptr<ServerState>& state,
                                  yyjson_val* root, const ServerBytes& body, bool streaming) {
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
        !lower.starts_with("access-control-")) extra.emplace_back(name, value);
  }
  const auto connection = found->second.connection;
  if (connection->responding) throw std::runtime_error("Server response has already started");
  const auto code = static_cast<int>(yyjson_get_uint(status));
  const auto no_body = found->second.method == "HEAD" || code == 204 || code == 205 || code == 304;
  if (streaming && (no_body || !body.empty()))
    throw std::runtime_error("Invalid streaming HTTP response");
  extra.emplace_back("Content-Type", content_type);
  auto sent = std::make_shared<cardio::promise_source<void>>();
  auto pending = sent->get_promise();
  connection->responding = true;
  connection->response->resolve(ServerResponse{code, std::move(extra), no_body ? ServerBytes{} : body, sent, streaming});
  co_await pending;
  if (!streaming) state->requests.erase(request_id);
}

static cardio::promise<void> AbortServerStream(std::shared_ptr<ServerConnection> connection) {
  CancelServerWebSocket(connection);
  const auto canceled = std::make_exception_ptr(cardio::canceled_exception());
  if (connection->response) connection->response->try_reject(canceled);
  if (connection->stream_end) connection->stream_end->try_reject(canceled);
  co_await connection->socket->close();
}
static cardio::promise<void> WriteServerChunk(std::shared_ptr<ServerState> state,
    const std::string& request_id, const ServerBytes& body, bool finish) {
  const auto found = state->requests.find(request_id);
  if (found == state->requests.end()) throw std::runtime_error("Server request is no longer active");
  const auto connection = found->second.connection;
  if (!connection->stream_end || connection->stream_complete || connection->writing)
    throw std::runtime_error("HTTP stream is unavailable or already writing");
  if ((!finish && (body.empty() || body.size() > 64 * 1024)) || (finish && !body.empty()))
    throw std::runtime_error("HTTP stream chunks must contain 1 to 65536 bytes");
  connection->writing = true;
  try {
    auto frame = ServerBytes{};
    if (finish) frame = {'0', '\r', '\n', '\r', '\n'};
    else {
      auto size = std::array<char, 16>{};
      const auto converted = std::to_chars(size.data(), size.data() + size.size(), body.size(), 16);
      frame.insert(frame.end(), size.data(), converted.ptr);
      frame.insert(frame.end(), {'\r', '\n'});
      frame.insert(frame.end(), body.begin(), body.end());
      frame.insert(frame.end(), {'\r', '\n'});
    }
    co_await connection->socket->write(frame);
    connection->writing = false;
    if (finish) {
      connection->stream_complete = true;
      state->requests.erase(request_id);
      connection->stream_end->try_resolve();
    }
  } catch (...) {
    connection->writing = false;
    connection->stream_end->try_reject(std::current_exception());
    throw;
  }
}

static cardio::promise<void> AcceptServerWebSocket(
    std::shared_ptr<ServerState> state, yyjson_val* root) {
  const auto found = state->requests.find(RequiredServerString(root, "requestId"));
  if (found == state->requests.end()) throw std::runtime_error("WebSocket request is no longer active");
  const auto connection = found->second.connection;
  if (!connection->handshake.valid || connection->responding)
    throw std::runtime_error("Request cannot upgrade to WebSocket");
  auto protocol = RequiredServerString(root, "protocol");
  const auto& offered = connection->handshake.protocols;
  if (protocol.empty() && std::find(offered.begin(), offered.end(), "muon.auth") != offered.end())
    protocol = "muon.auth";
  if (!protocol.empty() && std::find(offered.begin(), offered.end(), protocol) == offered.end())
    throw std::runtime_error("WebSocket protocol was not offered by the client");
  auto headers = ServerHeaders{{"Upgrade", "websocket"}, {"Connection", "Upgrade"},
      {"Sec-WebSocket-Accept", connection->handshake.accept}};
  if (!protocol.empty()) headers.emplace_back("Sec-WebSocket-Protocol", protocol);
  auto sent = std::make_shared<cardio::promise_source<void>>();
  auto pending = sent->get_promise();
  connection->protocol = std::move(protocol);
  connection->websocket = CreateMuonServerWebSocketCodec();
  connection->responding = true;
  connection->response->resolve(ServerResponse{101, std::move(headers), {}, sent, false, true});
  co_await pending;
}
static cardio::promise<void> SendServerWebSocket(
    std::shared_ptr<ServerConnection> connection, yyjson_val* root,
    const ServerBytes& body, bool close) {
  if (!connection->websocket_ready || connection->websocket_sending)
    throw std::runtime_error("WebSocket is unavailable or already sending");
  if (close) {
    const auto code = yyjson_obj_get(root, "code");
    if (!yyjson_is_uint(code) || yyjson_get_uint(code) > 65535)
      throw std::runtime_error("Invalid WebSocket close code");
    connection->websocket->close(static_cast<uint16_t>(yyjson_get_uint(code)), RequiredServerString(root, "reason"));
    StartServerWebSocketTimeout(connection);
  } else {
    const auto binary = yyjson_obj_get(root, "binary");
    if (!yyjson_is_bool(binary)) throw std::runtime_error("Invalid WebSocket message type");
    connection->websocket->queue(yyjson_get_bool(binary), body);
  }
  connection->websocket_sending = true;
  auto failure = std::exception_ptr{};
  try { co_await DrainServerWebSocket(connection); }
  catch (...) { failure = std::current_exception(); }
  connection->websocket_sending = false;
  if (failure) {
    CancelServerWebSocket(connection);
    co_await connection->socket->close();
    std::rethrow_exception(failure);
  }
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
    state->binding->websocket = yyjson_get_bool(yyjson_obj_get(root, "websocket"));
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
    if (state->listener || state->accepting) throw std::runtime_error("Server is already listening or starting");
    state->listener = service->sockets->listen(state->port);
    state->accepting = true;
    cardio::fire_and_forget(AcceptServerConnections(state));
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
  if (operation == "ws_accept") {
    co_await AcceptServerWebSocket(state, root);
    co_return ServerEnvelope("null");
  }
  if (operation == "ws_send" || operation == "ws_close" || operation == "ws_ack") {
    const auto request = state->requests.find(RequiredServerString(root, "requestId"));
    if (request == state->requests.end()) throw std::runtime_error("WebSocket is no longer active");
    const auto connection = request->second.connection;
    if (!connection->websocket) throw std::runtime_error("Request is not a WebSocket");
    if (operation == "ws_ack") {
      if (!connection->websocket_ack) throw std::runtime_error("WebSocket has no pending event");
      connection->websocket_ack->try_resolve();
    } else co_await SendServerWebSocket(connection, root, body, operation == "ws_close");
    co_return ServerEnvelope("null");
  }
  if (operation == "write" || operation == "finish" || operation == "abort") {
    const auto request_id = RequiredServerString(root, "requestId");
    const auto request = state->requests.find(request_id);
    if (request == state->requests.end()) {
      if (operation == "abort") co_return ServerEnvelope("null");
      throw std::runtime_error("Server request is no longer active");
    }
    const auto connection = request->second.connection;
    if (operation == "abort") co_await AbortServerStream(connection);
    else {
      auto failure = std::exception_ptr{};
      try { co_await WriteServerChunk(state, request_id, body, operation == "finish"); }
      catch (...) { failure = std::current_exception(); }
      if (failure) {
        co_await AbortServerStream(connection);
        std::rethrow_exception(failure);
      }
    }
    co_return ServerEnvelope("null");
  }
  if (operation == "respond" || operation == "start") {
    try {
      co_await RespondToServerRequest(state, root, body, operation == "start");
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
  co_await service->sockets->stop();
}
std::shared_ptr<MuonServerService> CreateMuonServerService() {
  auto state = std::make_shared<ServerServiceState>();
  state->sockets = CreateMuonServerSockets();
  return std::make_shared<MuonServerService>(MuonServerService{
      [state](std::string json, ServerBytes body, std::string owner) {
        return DispatchServer(state, std::move(json), std::move(body), std::move(owner));
      },
      [state](const std::string& owner) { cardio::fire_and_forget(ReleaseServerOwner(state, owner)); },
      [state] { return StopServerService(state); }});
}
