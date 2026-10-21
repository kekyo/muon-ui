/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "server/muon_server_http.h"
#include <llhttp.h>
#include <algorithm>
#include <array>
#include <set>
extern "C" {
#include "sha1.h"
}

static std::string HttpLower(std::string value) {
  for (auto& ch : value) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
  return value;
}
static std::string HttpBase64(std::span<const uint8_t> bytes, bool url) {
  const auto alphabet = url ? "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_" :
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  auto result = std::string{};
  uint32_t accumulator = 0;
  auto bits = 0;
  for (const auto byte : bytes) {
    accumulator = (accumulator << 8) | byte;
    bits += 8;
    while (bits >= 6) { bits -= 6; result += alphabet[(accumulator >> bits) & 63]; }
  }
  if (bits) result += alphabet[(accumulator << (6 - bits)) & 63];
  if (!url) while (result.size() % 4) result += '=';
  return result;
}
static bool HttpDecodeBase64(const std::string& text, bool url, std::vector<uint8_t>& bytes) {
  const auto alphabet = std::string(url ? "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_" :
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/");
  uint32_t accumulator = 0;
  auto bits = 0;
  for (const auto ch : text) {
    if (!url && ch == '=') break;
    const auto value = alphabet.find(ch);
    if (value == std::string::npos) return false;
    accumulator = (accumulator << 6) | static_cast<uint32_t>(value);
    bits += 6;
    if (bits >= 8) { bits -= 8; bytes.push_back(static_cast<uint8_t>(accumulator >> bits)); }
  }
  return HttpBase64(bytes, url) == text;
}
static bool HttpTokens(const std::string& text, std::vector<std::string>& tokens) {
  auto start = size_t{0};
  while (true) {
    const auto end = text.find(',', start);
    auto token = text.substr(start, end == std::string::npos ? end : end - start);
    const auto first = token.find_first_not_of(" \t");
    if (first == std::string::npos) return false;
    token = token.substr(first, token.find_last_not_of(" \t") - first + 1);
    if (!std::all_of(token.begin(), token.end(), [](unsigned char ch) {
      return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
          (ch >= '0' && ch <= '9') || std::string("!#$%&'*+-.^_`|~").find(ch) != std::string::npos;
    })) return false;
    tokens.push_back(std::move(token));
    if (tokens.size() > 64) return false;
    if (end == std::string::npos) return true;
    start = end + 1;
  }
}
MuonServerWebSocketHandshake PrepareMuonServerWebSocket(MuonServerHttpRequest& request) {
  if (!request.upgrade || !request.http11 || request.method != "GET" || !request.body.empty()) return {};
  auto result = MuonServerWebSocketHandshake{};
  auto key = std::string{};
  auto keys = 0;
  auto versions = 0;
  auto upgrades = 0;
  auto connection_upgrade = false;
  auto protocols = std::vector<std::string>{};
  for (const auto& [name, value] : request.headers) {
    const auto lower = HttpLower(name);
    if (lower == "sec-websocket-key") { ++keys; key = value; }
    else if (lower == "sec-websocket-version") { if (++versions != 1 || value != "13") return {}; }
    else if (lower == "upgrade") { if (++upgrades != 1 || HttpLower(value) != "websocket") return {}; }
    else if (lower == "connection") {
      auto tokens = std::vector<std::string>{};
      if (!HttpTokens(value, tokens)) return {};
      for (const auto& token : tokens) if (HttpLower(token) == "upgrade") connection_upgrade = true;
    } else if (lower == "sec-websocket-protocol") {
      if (!HttpTokens(value, protocols)) return {};
    } else if (lower == "transfer-encoding" || (lower == "content-length" && value != "0")) return {};
  }
  auto decoded = std::vector<uint8_t>{};
  if (keys != 1 || versions != 1 || upgrades != 1 || !connection_upgrade ||
      key.size() != 24 || !HttpDecodeBase64(key, false, decoded) || decoded.size() != 16) return {};
  auto unique = std::set<std::string>{};
  for (const auto& protocol : protocols) {
    if (!unique.insert(protocol).second) return {};
    if (protocol.starts_with("muon.bearer.")) {
      if (!result.bearer.empty() || protocol.size() > 5474) return {};
      auto token = std::vector<uint8_t>{};
      if (!HttpDecodeBase64(protocol.substr(12), true, token) || token.empty() || token.size() > 4096) return {};
      result.bearer.assign(token.begin(), token.end());
    } else {
      if (protocol.size() > 128) return {};
      result.protocols.push_back(protocol);
    }
  }
  if (unique.contains("muon.auth") != !result.bearer.empty()) return {};
  const auto challenge = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  auto context = SHA1_CTX{};
  auto digest = std::array<uint8_t, SHA1_DIGEST_LENGTH>{};
  SHA1Init(&context);
  SHA1Update(&context, reinterpret_cast<const uint8_t*>(challenge.data()), static_cast<unsigned int>(challenge.size()));
  SHA1Final(digest.data(), &context);
  result.accept = HttpBase64(digest, false);
  std::erase_if(request.headers, [](const auto& header) { return HttpLower(header.first) == "sec-websocket-protocol"; });
  if (!result.protocols.empty()) {
    auto offered = std::string{};
    for (const auto& protocol : result.protocols) { if (!offered.empty()) offered += ", "; offered += protocol; }
    request.headers.emplace_back("Sec-WebSocket-Protocol", std::move(offered));
  }
  result.valid = true;
  return result;
}

static constexpr size_t kMaxHttpBody = 1024 * 1024;
static constexpr size_t kMaxHttpHeaders = 32 * 1024;
struct HttpParserState {
  llhttp_settings_t settings{};
  llhttp_t parser{};
  std::shared_ptr<MuonServerHttpRequest> request = std::make_shared<MuonServerHttpRequest>();
  std::string field;
  std::string value;
  size_t header_bytes = 0;
  size_t header_wire_bytes = 0;
  uint32_t header_tail = 0;
  bool header_end = false;
  size_t received = 0;
  size_t header_count = 0;
  int error_status = 0;
  bool headers_complete = false;
  bool complete = false;
};
static HttpParserState& HttpState(llhttp_t* parser) {
  return *static_cast<HttpParserState*>(parser->data);
}
static int HttpLimit(HttpParserState& state, int status) {
  state.error_status = status;
  llhttp_set_error_reason(&state.parser, "HTTP request exceeds server limits");
  return HPE_USER;
}
static MuonServerHttpParseResult HttpResult(
    HttpParserState& state, llhttp_errno_t error, size_t consumed) {
  if (error != HPE_OK && error != HPE_PAUSED && error != HPE_PAUSED_UPGRADE && !state.error_status)
    state.error_status = 400;
  return {consumed, state.error_status,
      state.complete && !state.error_status ? state.request : nullptr};
}
static MuonServerHttpParseResult ConsumeHttp(
    const std::shared_ptr<HttpParserState>& state, std::span<const uint8_t> bytes) {
  if (state->error_status || state->complete) return HttpResult(*state, HPE_OK, 0);
  if (bytes.empty()) return HttpResult(*state, HPE_OK, 0);
  // Count the raw header block too: llhttp discards optional whitespace before
  // invoking value callbacks. This delimiter scan does not parse HTTP syntax;
  // llhttp still validates the complete block with its strict defaults.
  for (const auto byte : bytes) {
    if (state->header_end) break;
    if (++state->header_wire_bytes > kMaxHttpHeaders) {
      HttpLimit(*state, 431);
      return HttpResult(*state, HPE_USER, 0);
    }
    state->header_tail = (state->header_tail << 8) | byte;
    state->header_end = state->header_tail == 0x0d0a0d0a;
  }
  // Bound framing overhead as well as decoded body/header storage, including
  // unfinished lines and chunk extensions that are not retained as strings.
  if (bytes.size() > kMaxHttpBody + 64 * 1024 - state->received) {
    HttpLimit(*state, state->headers_complete ? 413 : 431);
    return HttpResult(*state, HPE_USER, 0);
  }
  const auto data = reinterpret_cast<const char*>(bytes.data());
  const auto error = llhttp_execute(&state->parser, data, bytes.size());
  auto consumed = bytes.size();
  if (error != HPE_OK) consumed = static_cast<size_t>(llhttp_get_error_pos(&state->parser) - data);
  state->received += consumed;
  if (!state->headers_complete && state->received > kMaxHttpHeaders)
    HttpLimit(*state, 431);
  return HttpResult(*state, error, consumed);
}

std::shared_ptr<MuonServerHttpParser> CreateMuonServerHttpParser() {
  auto state = std::make_shared<HttpParserState>();
  llhttp_settings_init(&state->settings);
  state->settings.on_url = [](llhttp_t* parser, const char* data, size_t size) {
    auto& state = HttpState(parser);
    if (size > 8192 - state.request->target.size()) return HttpLimit(state, 400);
    state.request->target.append(data, size);
    return 0;
  };
  state->settings.on_header_field = [](llhttp_t* parser, const char* data, size_t size) {
    auto& state = HttpState(parser);
    if (size > kMaxHttpHeaders - state.header_bytes) return HttpLimit(state, 431);
    state.header_bytes += size;
    if (!state.headers_complete) state.field.append(data, size);
    return 0;
  };
  state->settings.on_header_value = [](llhttp_t* parser, const char* data, size_t size) {
    auto& state = HttpState(parser);
    if (size > kMaxHttpHeaders - state.header_bytes) return HttpLimit(state, 431);
    state.header_bytes += size;
    if (!state.headers_complete) state.value.append(data, size);
    return 0;
  };
  state->settings.on_header_value_complete = [](llhttp_t* parser) {
    auto& state = HttpState(parser);
    if (++state.header_count > 128) return HttpLimit(state, 431);
    if (!state.headers_complete) {
      state.request->headers.emplace_back(std::move(state.field), std::move(state.value));
      state.field.clear();
      state.value.clear();
    }
    return 0;
  };
  state->settings.on_headers_complete = [](llhttp_t* parser) {
    auto& state = HttpState(parser);
    state.headers_complete = true;
    state.request->method = llhttp_method_name(static_cast<llhttp_method_t>(llhttp_get_method(parser)));
    state.request->upgrade = llhttp_get_upgrade(parser) != 0;
    state.request->http11 = llhttp_get_http_major(parser) == 1 && llhttp_get_http_minor(parser) == 1;
    if (llhttp_get_http_major(parser) != 1 || llhttp_get_http_minor(parser) > 1) return HttpLimit(state, 400);
    if (parser->content_length > kMaxHttpBody) return HttpLimit(state, 413);
    return 0;
  };
  state->settings.on_body = [](llhttp_t* parser, const char* data, size_t size) {
    auto& state = HttpState(parser);
    auto& body = state.request->body;
    if (size > kMaxHttpBody - body.size()) return HttpLimit(state, 413);
    body.insert(body.end(), data, data + size);
    return 0;
  };
  state->settings.on_message_complete = [](llhttp_t* parser) {
    HttpState(parser).complete = true;
    return static_cast<int>(HPE_PAUSED);
  };
  llhttp_init(&state->parser, HTTP_REQUEST, &state->settings);
  state->parser.data = state.get();
  return std::make_shared<MuonServerHttpParser>(MuonServerHttpParser{
      [state](std::span<const uint8_t> bytes) { return ConsumeHttp(state, bytes); },
      [state] {
        if (!state->complete && !state->error_status) {
          const auto error = llhttp_finish(&state->parser);
          if (error != HPE_OK || !state->complete) state->error_status = 400;
        }
        return HttpResult(*state, HPE_OK, 0);
      }});
}
