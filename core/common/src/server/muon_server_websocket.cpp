/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "server/muon_server_websocket.h"
#include <wslay/wslay.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <stdexcept>

static constexpr size_t kWebSocketMessageLimit = 1024 * 1024;
static constexpr size_t kWebSocketReceiveCount = 32;
static constexpr size_t kWebSocketQueueCount = 128;
static constexpr size_t kWebSocketOutputLimit = 64 * 1024;

struct MuonServerWebSocketState {
  wslay_event_context_ptr context = nullptr;
  std::span<const uint8_t> input;
  std::vector<uint8_t> output;
  std::vector<MuonServerWebSocketMessage> received;
  size_t received_bytes = 0;
  bool closing = false;
  bool peer_closed = false;
  uint16_t peer_code = 1006;
  std::string peer_reason;
  uint16_t local_code = 1006;
  std::string local_reason;
  bool awaiting_frame = true;
  uint8_t frame_opcode = 0;
  uint64_t frame_remaining = 0;
  std::array<uint8_t, 125> close_payload{};
  size_t close_length = 0;
  bool invalid_close = false;
  std::exception_ptr failure;
  ~MuonServerWebSocketState() { if (context) wslay_event_context_free(context); }
};

static bool IsWebSocketUtf8(std::span<const uint8_t> bytes) {
  for (size_t offset = 0; offset < bytes.size();) {
    const auto lead = bytes[offset++];
    if (lead < 0x80) continue;
    auto continuation = size_t{0};
    auto scalar = uint32_t{0};
    auto minimum = uint32_t{0};
    if (lead >= 0xc2 && lead <= 0xdf) { continuation = 1; scalar = lead & 0x1f; minimum = 0x80; }
    else if (lead >= 0xe0 && lead <= 0xef) { continuation = 2; scalar = lead & 0x0f; minimum = 0x800; }
    else if (lead >= 0xf0 && lead <= 0xf4) { continuation = 3; scalar = lead & 0x07; minimum = 0x10000; }
    else return false;
    if (continuation > bytes.size() - offset) return false;
    while (continuation-- > 0) {
      const auto next = bytes[offset++];
      if ((next & 0xc0) != 0x80) return false;
      scalar = (scalar << 6) | (next & 0x3f);
    }
    if (scalar < minimum || scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff)) return false;
  }
  return true;
}

static bool IsWebSocketCloseCode(uint16_t code) {
  // Match the received close-code range supported by Wslay release-1.1.1.
  return (code >= 1000 && code <= 1011 && code != 1004 && code != 1005 && code != 1006) ||
      (code >= 3000 && code <= 4999);
}

static void CheckWebSocketResult(int result) {
  if (result == WSLAY_ERR_NOMEM) throw std::bad_alloc();
  if (result != 0) throw std::runtime_error("WebSocket codec operation failed");
}

static void FailWebSocket(MuonServerWebSocketState& state, uint16_t code) {
  wslay_event_shutdown_read(state.context);
  if (!state.closing) {
    CheckWebSocketResult(wslay_event_queue_close(state.context, code, nullptr, 0));
    state.local_code = code;
    state.closing = true;
  }
}

static void FailWebSocketCallback(MuonServerWebSocketState& state) noexcept {
  state.failure = std::current_exception();
  wslay_event_shutdown_read(state.context);
  wslay_event_shutdown_write(state.context);
}

static void CheckWebSocketCall(MuonServerWebSocketState& state, int result) {
  if (state.failure) std::rethrow_exception(state.failure);
  try { CheckWebSocketResult(result); }
  catch (...) { FailWebSocketCallback(state); throw; }
}

static ssize_t ReadWebSocket(wslay_event_context_ptr context, uint8_t* bytes,
    size_t length, int, void* user) {
  auto& state = *static_cast<MuonServerWebSocketState*>(user);
  if (state.input.empty()) {
    wslay_event_set_error(context, WSLAY_ERR_WOULDBLOCK);
    return -1;
  }
  // Wslay 1.1.1 skips two bytes on EACH close payload chunk during UTF-8
  // validation. Feed close in single bytes and validate the complete reason
  // in the frame callback below. Bound other reads by Wslay's frame metadata
  // so a bulk read cannot also prefetch a following close frame.
  // https://github.com/tatsuhiro-t/wslay/blob/release-1.1.1/lib/wslay_event.c#L639-L658
  const auto frame_limit = state.awaiting_frame || state.frame_opcode == WSLAY_CONNECTION_CLOSE
      ? uint64_t{1} : state.frame_remaining;
  const auto count = std::min({length, state.input.size(), static_cast<size_t>(std::min<uint64_t>(frame_limit, length))});
  if (count == 0) {
    wslay_event_set_error(context, WSLAY_ERR_WOULDBLOCK);
    return -1;
  }
  std::memcpy(bytes, state.input.data(), count);
  state.input = state.input.subspan(count);
  return static_cast<ssize_t>(count);
}

static void StartWebSocketFrame(wslay_event_context_ptr context,
    const wslay_event_on_frame_recv_start_arg* frame, void* user) {
  auto& state = *static_cast<MuonServerWebSocketState*>(user);
  state.awaiting_frame = false;
  state.frame_opcode = frame->opcode;
  state.frame_remaining = frame->payload_length;
  state.close_length = 0;
  try {
    if (frame->opcode == WSLAY_PING && !state.closing &&
        (wslay_event_get_queued_msg_count(context) >= kWebSocketQueueCount ||
         frame->payload_length > kWebSocketMessageLimit - wslay_event_get_queued_msg_length(context)))
      FailWebSocket(state, 1009);
  } catch (...) { FailWebSocketCallback(state); }
}

static void ReceiveWebSocketChunk(wslay_event_context_ptr,
    const wslay_event_on_frame_recv_chunk_arg* chunk, void* user) {
  auto& state = *static_cast<MuonServerWebSocketState*>(user);
  state.frame_remaining -= chunk->data_length;
  if (state.frame_opcode == WSLAY_CONNECTION_CLOSE && chunk->data_length > 0) {
    // Wslay has already checked the RFC 6455 control-frame bound of 125 bytes.
    std::memcpy(state.close_payload.data() + state.close_length, chunk->data, chunk->data_length);
    state.close_length += chunk->data_length;
  }
}

static void FinishWebSocketFrame(wslay_event_context_ptr, void* user) {
  auto& state = *static_cast<MuonServerWebSocketState*>(user);
  state.awaiting_frame = true;
  if (state.frame_opcode != WSLAY_CONNECTION_CLOSE) return;
  // Also reject a one-byte close payload, which Wslay 1.1.1 treats as no-code.
  // https://github.com/tatsuhiro-t/wslay/blob/release-1.1.1/lib/wslay_event.c#L704-L723
  const auto code = state.close_length == 1 ? uint16_t{1002} :
      state.close_length >= 2 && !IsWebSocketUtf8(std::span(state.close_payload).subspan(2, state.close_length - 2))
          ? uint16_t{1007} : uint16_t{0};
  if (code == 0) return;
  state.invalid_close = true;
  try { FailWebSocket(state, code); }
  catch (...) { FailWebSocketCallback(state); }
}

static ssize_t WriteWebSocket(wslay_event_context_ptr context, const uint8_t* bytes,
    size_t length, int, void* user) {
  auto& state = *static_cast<MuonServerWebSocketState*>(user);
  const auto count = std::min(length, kWebSocketOutputLimit - state.output.size());
  if (count == 0) {
    wslay_event_set_error(context, WSLAY_ERR_WOULDBLOCK);
    return -1;
  }
  // take_output reserves the full bound before entering C; this cannot allocate.
  state.output.insert(state.output.end(), bytes, bytes + count);
  return static_cast<ssize_t>(count);
}

static void ReceiveWebSocketMessage(wslay_event_context_ptr context,
    const wslay_event_on_msg_recv_arg* message, void* user) {
  auto& state = *static_cast<MuonServerWebSocketState*>(user);
  try {
    if (message->opcode == WSLAY_CONNECTION_CLOSE) {
      if (state.invalid_close) return;
      state.peer_closed = true;
      state.peer_code = message->status_code == 0 ? 1005 : message->status_code;
      if (message->msg_length >= 2)
        state.peer_reason.assign(reinterpret_cast<const char*>(message->msg + 2), message->msg_length - 2);
      state.closing = true;
    } else if (message->opcode == WSLAY_TEXT_FRAME || message->opcode == WSLAY_BINARY_FRAME) {
      if (state.closing) return;
      if (state.received.size() == kWebSocketReceiveCount ||
          message->msg_length > kWebSocketMessageLimit - state.received_bytes) {
        FailWebSocket(state, 1009);
        return;
      }
      auto body = std::vector<uint8_t>{};
      if (message->msg_length > 0) body.assign(message->msg, message->msg + message->msg_length);
      state.received.push_back({message->opcode == WSLAY_BINARY_FRAME, std::move(body)});
      state.received_bytes += message->msg_length;
    }
    // Automatic pong responses must obey the same queue bounds as app writes.
    // Reserve one extra close frame for shutting down an overflowing queue.
    if (!state.closing && (wslay_event_get_queued_msg_count(context) > kWebSocketQueueCount ||
        wslay_event_get_queued_msg_length(context) > kWebSocketMessageLimit))
      FailWebSocket(state, 1009);
  } catch (...) { FailWebSocketCallback(state); }
}

std::shared_ptr<MuonServerWebSocketCodec> CreateMuonServerWebSocketCodec() {
  const auto state = std::make_shared<MuonServerWebSocketState>();
  const auto callbacks = wslay_event_callbacks{
    ReadWebSocket, WriteWebSocket, nullptr, StartWebSocketFrame, ReceiveWebSocketChunk,
    FinishWebSocketFrame, ReceiveWebSocketMessage,
  };
  CheckWebSocketResult(wslay_event_context_server_init(&state->context, &callbacks, state.get()));
  wslay_event_config_set_max_recv_msg_length(state->context, kWebSocketMessageLimit);
  auto codec = std::make_shared<MuonServerWebSocketCodec>();
  codec->receive = [state](std::span<const uint8_t> bytes) {
    if (state->failure) std::rethrow_exception(state->failure);
    state->received.clear();
    state->received_bytes = 0;
    state->input = bytes;
    auto result = 0;
    // A deliberately short callback read can leave an incomplete frame header
    // inside Wslay. Continue only while consuming this already supplied span.
    while (wslay_event_get_read_enabled(state->context)) {
      const auto previous = state->input.size();
      result = wslay_event_recv(state->context);
      if (result != 0 || state->failure || state->input.empty() || state->input.size() == previous) break;
    }
    state->input = {};
    CheckWebSocketCall(*state, result);
    return std::move(state->received);
  };
  codec->queue = [state](bool binary, std::span<const uint8_t> bytes) {
    if (state->failure) std::rethrow_exception(state->failure);
    if (state->closing || !wslay_event_get_read_enabled(state->context) ||
        !wslay_event_get_write_enabled(state->context))
      throw std::runtime_error("WebSocket is closing");
    if (bytes.size() > kWebSocketMessageLimit ||
        wslay_event_get_queued_msg_count(state->context) >= kWebSocketQueueCount ||
        bytes.size() > kWebSocketMessageLimit - wslay_event_get_queued_msg_length(state->context))
      throw std::length_error("WebSocket send queue limit exceeded");
    if (!binary && !IsWebSocketUtf8(bytes)) throw std::invalid_argument("WebSocket text must be UTF-8");
    const auto message = wslay_event_msg{static_cast<uint8_t>(binary ? WSLAY_BINARY_FRAME : WSLAY_TEXT_FRAME), bytes.data(), bytes.size()};
    CheckWebSocketResult(wslay_event_queue_msg(state->context, &message));
  };
  codec->close = [state](uint16_t code, const std::string& reason) {
    const auto bytes = std::span(reinterpret_cast<const uint8_t*>(reason.data()), reason.size());
    if (!IsWebSocketCloseCode(code) || reason.size() > 123 || !IsWebSocketUtf8(bytes))
      throw std::invalid_argument("Invalid WebSocket close code or reason");
    if (state->failure) std::rethrow_exception(state->failure);
    if (state->closing || !wslay_event_get_read_enabled(state->context) ||
        !wslay_event_get_write_enabled(state->context)) return;
    auto owned_reason = reason;
    CheckWebSocketResult(wslay_event_queue_close(state->context, code, bytes.data(), bytes.size()));
    state->local_code = code;
    state->local_reason = std::move(owned_reason);
    state->closing = true;
  };
  codec->take_output = [state] {
    if (state->failure) std::rethrow_exception(state->failure);
    state->output.clear();
    state->output.reserve(kWebSocketOutputLimit);
    CheckWebSocketCall(*state, wslay_event_send(state->context));
    return std::move(state->output);
  };
  codec->status = [state] {
    const auto close_sent = wslay_event_get_close_sent(state->context) != 0;
    const auto code = state->peer_closed ? state->peer_code :
        state->local_code != 1006 ? state->local_code :
        wslay_event_get_status_code_sent(state->context);
    return MuonServerWebSocketStatus{
      wslay_event_get_read_enabled(state->context) != 0,
      wslay_event_want_write(state->context) != 0,
      state->peer_closed, close_sent, code,
      state->peer_closed ? state->peer_reason : state->local_reason,
    };
  };
  return codec;
}
