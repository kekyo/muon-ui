/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

/** One complete, validated WebSocket application message. */
struct MuonServerWebSocketMessage {
  /** True for binary messages; false for validated UTF-8 text. */
  bool binary;
  /** Owned message bytes, limited to 1 MiB. */
  std::vector<uint8_t> body;
};

/** Current state of the server-side WebSocket frame codec. */
struct MuonServerWebSocketStatus {
  /** Whether more peer bytes may be supplied, including a close reply. */
  bool read_enabled;
  /** Whether take_output() can produce more protocol bytes. */
  bool write_pending;
  /** Whether a valid peer close frame has arrived. */
  bool close_received;
  /** Whether all local close bytes have been returned by take_output(). */
  bool close_sent;
  /** Peer close code, otherwise known local failure/close code, otherwise 1006.
   * A peer close with no code reports 1005. Neither 1005 nor 1006 is sent.
   * Automatically generated failure codes become known when output is taken.
   */
  uint16_t code;
  /** Peer close reason, otherwise the local close reason. */
  std::string reason;
};

/** Socket-independent, single-threaded RFC 6455 server frame codec. */
struct MuonServerWebSocketCodec {
  /** Consumes borrowed input and returns complete application messages.
   * One call returns at most 32 messages and 1 MiB in total. Invalid frames
   * or receive limits stop reads and queue a protocol close. Control frames
   * never become application messages; ping and close replies are automatic.
   * Fatal parser/callback errors throw; the caller must close the transport.
   */
  std::function<std::vector<MuonServerWebSocketMessage>(std::span<const uint8_t>)> receive;
  /** Copies a text or binary message into a bounded send queue.
   * Throws for invalid UTF-8 text, a closing codec, or a queue exceeding
   * 1 MiB / 128 messages. The caller can retry a capacity failure after drain.
   */
  std::function<void(bool, std::span<const uint8_t>)> queue;
  /** Queues a close; repeated calls after closing have no additional effect.
   * Accepts 1000..1011 except 1004/1005/1006, or 3000..4999, with a valid
   * UTF-8 reason of at most 123 bytes. Invalid arguments throw.
   */
  std::function<void(uint16_t, const std::string&)> close;
  /** Returns at most 64 KiB of unmasked protocol bytes in transmission order.
   * The caller must write every returned byte before taking more or closing
   * its socket; close_sent describes codec output, not OS send completion.
   */
  std::function<std::vector<uint8_t>()> take_output;
  /** Reads the current protocol state without advancing input or output. */
  std::function<MuonServerWebSocketStatus()> status;
};

/** Creates a bounded server codec without I/O, extensions or compression. */
std::shared_ptr<MuonServerWebSocketCodec> CreateMuonServerWebSocketCodec();
