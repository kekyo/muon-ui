/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "server/muon_server_websocket.h"
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>

static void Expect(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
static std::vector<uint8_t> Frame(uint8_t opcode, std::span<const uint8_t> body,
    bool final = true, bool masked = true) {
  auto bytes = std::vector<uint8_t>{static_cast<uint8_t>((final ? 0x80 : 0) | opcode)};
  const auto mask = static_cast<uint8_t>(masked ? 0x80 : 0);
  if (body.size() < 126) bytes.push_back(mask | static_cast<uint8_t>(body.size()));
  else if (body.size() <= 65535) {
    bytes.push_back(mask | 126);
    bytes.push_back(static_cast<uint8_t>(body.size() >> 8));
    bytes.push_back(static_cast<uint8_t>(body.size()));
  } else {
    bytes.push_back(mask | 127);
    for (int shift = 56; shift >= 0; shift -= 8)
      bytes.push_back(static_cast<uint8_t>(static_cast<uint64_t>(body.size()) >> shift));
  }
  const auto key = std::array<uint8_t, 4>{0x11, 0x22, 0x33, 0x44};
  if (masked) bytes.insert(bytes.end(), key.begin(), key.end());
  for (size_t i = 0; i < body.size(); ++i)
    bytes.push_back(body[i] ^ (masked ? key[i % key.size()] : 0));
  return bytes;
}
static std::span<const uint8_t> Bytes(const std::string& text) {
  return {reinterpret_cast<const uint8_t*>(text.data()), text.size()};
}
static void Append(std::vector<uint8_t>& target, const std::vector<uint8_t>& tail) {
  target.insert(target.end(), tail.begin(), tail.end());
}
static std::vector<uint8_t> Drain(const std::shared_ptr<MuonServerWebSocketCodec>& codec) {
  auto bytes = std::vector<uint8_t>{};
  while (codec->status().write_pending) {
    const auto part = codec->take_output();
    Expect(!part.empty() && part.size() <= 65536, "Output stalled or exceeded 64 KiB");
    Append(bytes, part);
  }
  return bytes;
}
static void ExpectFailureClose(const std::shared_ptr<MuonServerWebSocketCodec>& codec, uint16_t code) {
  Expect(!codec->status().read_enabled, "Invalid input left reading enabled");
  const auto bytes = Drain(codec);
  const auto expected = std::vector<uint8_t>{0x88, 2, static_cast<uint8_t>(code >> 8), static_cast<uint8_t>(code)};
  Expect(bytes == expected, "Protocol failure did not emit the expected close code");
  Expect(codec->status().close_sent && codec->status().code == code, "Failure close status was lost");
}
template<typename T> static void ExpectThrows(const T& operation, const char* message) {
  auto threw = false;
  try { operation(); } catch (const std::exception&) { threw = true; }
  Expect(threw, message);
}

static void TestMessages() {
  const auto text = std::string("hello \xe6\x97\xa5\xf0\x9f\x98\x80");
  const auto frame = Frame(1, Bytes(text));
  for (size_t split = 1; split < frame.size(); ++split) {
    const auto codec = CreateMuonServerWebSocketCodec();
    Expect(codec->receive(std::span(frame).first(split)).empty(), "Partial frame was delivered");
    const auto messages = codec->receive(std::span(frame).subspan(split));
    Expect(messages.size() == 1 && !messages[0].binary &&
        messages[0].body == std::vector<uint8_t>(text.begin(), text.end()), "Split text frame did not decode");
  }
  const auto codec = CreateMuonServerWebSocketCodec();
  auto fragmented = Frame(1, std::array<uint8_t, 2>{'a', 0xe6}, false);
  Append(fragmented, Frame(9, Bytes("ping")));
  Append(fragmented, Frame(0, std::array<uint8_t, 2>{0x97, 0xa5}));
  const auto messages = codec->receive(fragmented);
  Expect(messages.size() == 1 && messages[0].body == std::vector<uint8_t>({'a', 0xe6, 0x97, 0xa5}),
      "Fragmented text or interleaved ping changed the message");
  Expect(Drain(codec) == Frame(10, Bytes("ping"), true, false), "Ping did not generate the exact unmasked pong");
  const auto binary = std::vector<uint8_t>{0, 255, 128, 13, 10};
  const auto received = codec->receive(Frame(2, binary));
  Expect(received.size() == 1 && received[0].binary && received[0].body == binary, "Binary message changed");
  Expect(codec->receive(Frame(10, {})).empty(), "Pong became an application message");
  Expect(!codec->status().write_pending, "Receiving pong generated more output");
}

static void TestInvalidFrames() {
  const auto invalid_frames = std::vector<std::vector<uint8_t>>{
    Frame(1, Bytes("plain"), true, false), Frame(0, Bytes("orphan")),
    Frame(3, {}),
    {0xc1, 0x80, 0, 0, 0, 0},
  };
  for (const auto& invalid : invalid_frames) {
    const auto codec = CreateMuonServerWebSocketCodec();
    Expect(codec->receive(invalid).empty(), "Malformed frame became an application message");
    ExpectFailureClose(codec, 1002);
  }
  for (const auto& invalid : {Frame(9, {}, false), Frame(9, std::vector<uint8_t>(126, 1))}) {
    const auto codec = CreateMuonServerWebSocketCodec();
    ExpectThrows([&] { codec->receive(invalid); }, "Fatal malformed control frame was accepted");
    Expect(!codec->status().read_enabled, "Fatal frame failure left reads enabled");
  }
  for (const auto& invalid : std::vector<std::vector<uint8_t>>{
      {0xff}, {0xc0, 0xaf}, {0xed, 0xa0, 0x80}, {0xf4, 0x90, 0x80, 0x80}, {0xe6, 0x97}}) {
    const auto codec = CreateMuonServerWebSocketCodec();
    Expect(codec->receive(Frame(1, invalid)).empty(), "Invalid UTF-8 text was delivered");
    ExpectFailureClose(codec, 1007);
  }
}

static void TestClose() {
  const auto payload = std::vector<uint8_t>{3, 232, 'b', 'y', 'e'};
  const auto frame = Frame(8, payload);
  for (size_t split = 1; split < frame.size(); ++split) {
    const auto codec = CreateMuonServerWebSocketCodec();
    codec->receive(std::span(frame).first(split));
    Expect(codec->receive(std::span(frame).subspan(split)).empty(), "Close became an application message");
    const auto status = codec->status();
    Expect(status.close_received && status.code == 1000 && status.reason == "bye", "Peer close details changed");
    Expect(Drain(codec) == Frame(8, payload, true, false), "Peer close was not echoed");
    ExpectThrows([&] { codec->queue(false, Bytes("late")); }, "Data was queued after peer close");
  }
  const auto empty = CreateMuonServerWebSocketCodec();
  empty->receive(Frame(8, {}));
  Expect(empty->status().close_received && empty->status().code == 1005, "Empty close lost no-status code");
  Expect(Drain(empty) == Frame(8, {}, true, false), "Empty close gained a synthetic wire code");
  for (const uint16_t code : {1004, 1005, 1006, 1012, 1015, 2000, 5000}) {
    const auto codec = CreateMuonServerWebSocketCodec();
    codec->receive(Frame(8, std::array<uint8_t, 2>{static_cast<uint8_t>(code >> 8), static_cast<uint8_t>(code)}));
    ExpectFailureClose(codec, 1002);
  }
  const auto local = CreateMuonServerWebSocketCodec();
  local->close(4000, "done");
  local->close(1000, "ignored");
  ExpectThrows([&] { local->queue(true, {}); }, "Data was queued after local close");
  Expect(Drain(local) == Frame(8, std::array<uint8_t, 6>{15, 160, 'd', 'o', 'n', 'e'}, true, false), "Local close changed");
  Expect(local->status().read_enabled && !local->status().close_received, "Local close did not permit a peer reply");
  local->receive(Frame(8, std::array<uint8_t, 2>{3, 232}));
  Expect(local->status().close_received && local->status().code == 1000, "Peer reply did not take precedence");

  const auto coalesced = CreateMuonServerWebSocketCodec();
  const auto body = std::vector<uint8_t>(8192, 42);
  auto data_and_close = Frame(2, body);
  const auto close_payload = std::array<uint8_t, 5>{3, 232, 0xe6, 0x97, 0xa5};
  Append(data_and_close, Frame(8, close_payload));
  const auto preceding = coalesced->receive(data_and_close);
  Expect(preceding.size() == 1 && preceding[0].binary && preceding[0].body == body,
      "Close discarded the preceding complete message");
  Expect(coalesced->status().close_received && coalesced->status().reason == "\xe6\x97\xa5",
      "Close following bulk data lost its UTF-8 reason");
  Expect(Drain(coalesced) == Frame(8, close_payload, true, false), "Coalesced close was not echoed");
}

static void TestShortClose() {
  const auto short_close = CreateMuonServerWebSocketCodec();
  short_close->receive(Frame(8, std::array<uint8_t, 1>{3}));
  ExpectFailureClose(short_close, 1002);
}

static void TestCloseReason() {
  const auto valid_frame = Frame(8, std::array<uint8_t, 5>{3, 232, 0xe6, 0x97, 0xa5});
  for (size_t split = 1; split < valid_frame.size(); ++split) {
    const auto codec = CreateMuonServerWebSocketCodec();
    codec->receive(std::span(valid_frame).first(split));
    codec->receive(std::span(valid_frame).subspan(split));
    Expect(codec->status().close_received && codec->status().reason == "\xe6\x97\xa5",
        "Valid close UTF-8 was rejected at an input split");
  }
  for (const auto& reason : std::vector<std::vector<uint8_t>>{{0xff}, {0xc0, 0xaf}, {0xe6, 0x97}}) {
    const auto codec = CreateMuonServerWebSocketCodec();
    auto payload = std::vector<uint8_t>{3, 232};
    Append(payload, reason);
    const auto frame = Frame(8, payload);
    for (const auto byte : frame) codec->receive(std::span(&byte, 1));
    ExpectFailureClose(codec, 1007);
  }
  const auto valid = CreateMuonServerWebSocketCodec();
  const auto frame = Frame(8, std::array<uint8_t, 5>{3, 232, 0xe6, 0x97, 0xa5});
  for (const auto byte : frame) valid->receive(std::span(&byte, 1));
  Expect(valid->status().close_received && valid->status().reason == "\xe6\x97\xa5", "Split UTF-8 close reason was rejected");
}

static void TestLimits() {
  const auto exact = CreateMuonServerWebSocketCodec();
  const auto body = std::vector<uint8_t>(1024 * 1024, 42);
  const auto complete = exact->receive(Frame(2, body));
  Expect(complete.size() == 1 && complete[0].body == body, "Maximum inbound message was not accepted");
  const auto oversized = CreateMuonServerWebSocketCodec();
  const auto large = Frame(2, std::vector<uint8_t>(1024 * 1024 + 1, 0));
  // Wslay reports a new frame after the first payload byte. It still rejects
  // the declared size before allocating or retaining the advertised body.
  oversized->receive(std::span(large).first(15));
  ExpectFailureClose(oversized, 1009);
  const auto fragments = CreateMuonServerWebSocketCodec();
  fragments->receive(Frame(2, std::vector<uint8_t>(1024 * 1024, 0), false));
  fragments->receive(Frame(0, std::array<uint8_t, 1>{1}));
  ExpectFailureClose(fragments, 1009);
  const auto aggregate = CreateMuonServerWebSocketCodec();
  auto many = Frame(2, std::vector<uint8_t>(600000, 1));
  Append(many, Frame(2, std::vector<uint8_t>(600000, 2)));
  const auto messages = aggregate->receive(many);
  Expect(messages.size() <= 1, "Receive result exceeded 1 MiB");
  ExpectFailureClose(aggregate, 1009);
  const auto count = CreateMuonServerWebSocketCodec();
  many.clear();
  for (size_t i = 0; i < 33; ++i) Append(many, Frame(1, {}));
  Expect(count->receive(many).size() <= 32, "Receive result exceeded 32 messages");
  ExpectFailureClose(count, 1009);
  const auto pings = CreateMuonServerWebSocketCodec();
  many.clear();
  for (size_t i = 0; i < 256; ++i) Append(many, Frame(9, {}));
  pings->receive(many);
  ExpectFailureClose(pings, 1009);
}

static void TestOutbound() {
  const auto codec = CreateMuonServerWebSocketCodec();
  auto body = std::vector<uint8_t>(1024 * 1024);
  for (size_t i = 0; i < body.size(); ++i) body[i] = static_cast<uint8_t>(i * 17);
  codec->queue(true, body);
  ExpectThrows([&] { codec->queue(false, Bytes("one more")); }, "Send queue exceeded 1 MiB");
  Expect(Drain(codec) == Frame(2, body, true, false), "Large outbound frame was truncated, masked or reordered");
  for (size_t i = 0; i < 128; ++i) codec->queue(false, {});
  ExpectThrows([&] { codec->queue(false, {}); }, "Send queue exceeded 128 messages");
  Expect(Drain(codec).size() == 256, "Empty queued messages were lost");
  const auto text = std::string("\xe6\x97\xa5");
  codec->queue(false, Bytes(text));
  Expect(Drain(codec) == Frame(1, Bytes(text), true, false), "Valid outbound UTF-8 changed");
  ExpectThrows([&] { codec->queue(false, std::array<uint8_t, 1>{0xff}); }, "Invalid outbound UTF-8 was accepted");
  ExpectThrows([&] { codec->queue(true, std::vector<uint8_t>(1024 * 1024 + 1)); }, "Oversized outbound message was accepted");
  for (const uint16_t code : {0, 999, 1004, 1005, 1006, 1012, 1015, 2000, 5000})
    ExpectThrows([&] { codec->close(code, ""); }, "Invalid outbound close code was accepted");
  ExpectThrows([&] { codec->close(1000, std::string(124, 'x')); }, "Oversized close reason was accepted");
  ExpectThrows([&] { codec->close(1000, "\xff"); }, "Invalid outbound close reason was accepted");
  codec->close(1000, std::string(123, 'x'));
  Expect(Drain(codec).size() == 127, "Maximum close reason was rejected");
}

int main(int argc, char** argv) {
  const auto tests = std::array<std::pair<const char*, void (*)()>, 7>{{
    {"messages", TestMessages}, {"invalid", TestInvalidFrames}, {"close", TestClose},
    {"short-close", TestShortClose}, {"close-reason", TestCloseReason},
    {"limits", TestLimits}, {"outbound", TestOutbound},
  }};
  try {
    for (const auto& [name, test] : tests) {
      if (argc > 1 && std::string(argv[1]) != name) continue;
      test();
      std::cout << name << " passed\n";
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
