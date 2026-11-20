/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "muon_android_service_peer.h"
#include "muon_android_service_transport.h"
#include "services/muon_service_limits.h"
#include <yyjson.h>
#include <charconv>
#include <cstdlib>
#include <deque>
#include <optional>
#include <set>
#include <stdexcept>

struct MuonAndroidServicePeerState {
  MuonServiceTransport transport;
  std::string owner;
  MuonServiceBroker* broker;
  std::function<void(std::function<void()>)> post;
  std::function<void()> disconnected;
  std::set<std::string> pending;
  std::deque<std::string> output;
  size_t output_bytes = 0;
  bool closed = false;
  bool writing = false;
  std::optional<cardio::promise<void>> reader;
  std::optional<cardio::promise<void>> writer;
};

static std::string Quote(const std::string& value) {
  return EncodeMuonServiceValue({MUON_SERVICE_VALUE_STRING, {}, {}}, {value});
}

static std::string Text(yyjson_val* object, const char* name) {
  auto* value = yyjson_obj_get(object, name);
  if (!yyjson_is_str(value)) throw std::invalid_argument("Missing service RPC string");
  return {yyjson_get_str(value), yyjson_get_len(value)};
}

static uint64_t Identity(yyjson_val* object, const char* name) {
  const auto text = Text(object, name);
  uint64_t value = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (!value || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
      std::to_string(value) != text) throw std::invalid_argument("Invalid service RPC identity");
  return value;
}

static void Close(std::shared_ptr<MuonAndroidServicePeerState> state) {
  if (state->closed) return;
  state->closed = true;
  state->transport.close();
  state->pending.clear();
  state->output.clear();
  state->output_bytes = 0;
  state->broker->CloseOwner(state->owner);
  state->disconnected();
}

static void StartWriter(const std::shared_ptr<MuonAndroidServicePeerState>& state);

static cardio::promise<void> Write(std::shared_ptr<MuonAndroidServicePeerState> state) {
  try {
    while (!state->closed && !state->output.empty()) {
      auto value = std::move(state->output.front());
      state->output.pop_front();
      state->output_bytes -= value.size();
      co_await state->transport.write(std::move(value));
    }
  } catch (const std::exception&) { Close(state); }
  state->post([state] {
    state->writer.reset();
    state->writing = false;
    StartWriter(state);
  });
}

static void StartWriter(const std::shared_ptr<MuonAndroidServicePeerState>& state) {
  if (state->closed || state->writing || state->output.empty()) return;
  state->writing = true;
  state->writer.emplace(Write(state));
}

static void Send(const std::shared_ptr<MuonAndroidServicePeerState>& state, std::string json) {
  if (state->closed) return;
  if (json.size() >= kMuonServiceMaxFrameBytes ||
      json.size() + 1 > kMuonServiceMaxQueuedBytes - state->output_bytes) { Close(state); return; }
  json += '\n';
  state->output_bytes += json.size();
  state->output.push_back(std::move(json));
  StartWriter(state);
}

static void Request(const std::shared_ptr<MuonAndroidServicePeerState>& state, const std::string& json) {
  const auto document = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>(
      yyjson_read(json.data(), json.size(), 0), &yyjson_doc_free);
  auto* root = yyjson_doc_get_root(document.get());
  if (!yyjson_is_obj(root)) throw std::invalid_argument("Invalid service RPC frame");
  const auto request = std::to_string(Identity(root, "requestId"));
  if (state->pending.size() >= 1024 || !state->pending.insert(request).second)
    throw std::invalid_argument("Duplicate or excessive service RPC requests");
  try {
    if (Text(root, "op") != "rpc") throw std::invalid_argument("Only service RPC is permitted on this stream");
    const auto reference = MuonServiceReference{Identity(root, "refId"), Text(root, "serviceId"),
        state->owner, Identity(root, "generation")};
    auto* payload = yyjson_obj_get(root, "payload");
    if (!yyjson_is_obj(payload)) throw std::invalid_argument("Service RPC payload must be an object");
    size_t length = 0;
    const auto encoded = std::unique_ptr<char, decltype(&std::free)>(yyjson_val_write(payload, 0, &length), &std::free);
    if (!encoded) throw std::bad_alloc();
    state->broker->Request(reference, Text(root, "action"), {encoded.get(), length},
        [state, request](const std::string& result) {
      if (state->closed || !state->pending.erase(request)) return;
      Send(state, "{\"requestId\":" + Quote(request) + ",\"result\":" + result + "}");
    });
  } catch (const std::exception& error) {
    if (state->pending.erase(request)) Send(state, "{\"requestId\":" + Quote(request) +
        ",\"error\":{\"code\":\"invalid-request\",\"message\":" + Quote(error.what()) + "}}");
  }
}

static cardio::promise<void> Read(std::shared_ptr<MuonAndroidServicePeerState> state) {
  try {
    std::string input;
    while (!state->closed) {
      const auto chunk = co_await state->transport.read();
      if (chunk.empty()) break;
      input += chunk;
      size_t end;
      while (!state->closed && (end = input.find('\n')) != std::string::npos) {
        if (end > kMuonServiceMaxFrameBytes) throw std::invalid_argument("Service RPC frame exceeds its limit");
        auto frame = input.substr(0, end);
        input.erase(0, end + 1);
        Request(state, frame);
      }
      if (input.size() > kMuonServiceMaxFrameBytes) throw std::invalid_argument("Service RPC frame exceeds its limit");
    }
  } catch (const std::exception&) { /* EOF and protocol failure terminate this page owner. */ }
  Close(state);
  state->post([state] { state->reader.reset(); });
}

std::shared_ptr<MuonAndroidServicePeer> OpenMuonAndroidServicePeer(
    int descriptor, std::string owner, MuonServiceBroker& broker,
    std::function<void(std::function<void()>)> post, std::function<void()> disconnected) {
  auto transport = AdoptMuonAndroidServiceTransport(descriptor);
  const auto state = std::make_shared<MuonAndroidServicePeerState>();
  state->transport = std::move(transport);
  state->owner = std::move(owner);
  state->broker = &broker;
  state->post = std::move(post);
  state->disconnected = std::move(disconnected);
  // Register the peer under its Binder owner before EOF can remove that owner.
  state->post([state] {
    if (!state->closed) state->reader.emplace(Read(state));
  });
  return std::make_shared<MuonAndroidServicePeer>(MuonAndroidServicePeer{[state] { Close(state); }});
}
