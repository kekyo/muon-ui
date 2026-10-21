/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "server/muon_server_http.h"
#include <llhttp.h>

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
