/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#pragma once

#include <cardio.h>
#include <functional>
#include <memory>
#include <string>

/** One authenticated, ordered byte stream owned by a single native page session. */
struct MuonServiceTransport {
  /** Reads a nonempty chunk, or empty text on EOF; only one read runs at a time. */
  std::function<cardio::promise<std::string>()> read;
  /** Writes the complete owned chunk; only one write runs at a time. */
  std::function<cardio::promise<void>(std::string)> write;
  /**
   * Aborts the stream without throwing; repeated calls are harmless.
   * Pending reads/writes must settle. Their storage and handles remain valid
   * until those operations finish and the transport callbacks are destroyed.
   */
  std::function<void()> close;
};

/** Private connection, frame, and pending response ownership. */
struct MuonServiceClientImpl;

/**
 * Page-owned desktop service client, serialized on its cardio dispatcher.
 * Connection loss invalidates the entire native owner. This instance never
 * reconnects implicitly, so old references cannot address a replacement host.
 */
class MuonServiceClient final {
 public:
  /** @param connect Creates one authenticated stream; observes page cancellation. */
  explicit MuonServiceClient(std::function<cardio::promise<MuonServiceTransport>(
      cardio::cancellation)> connect);
  /** Aborts outstanding work; use Close() to await complete disposal explicitly. */
  ~MuonServiceClient();
  /** Native page session identity cannot be copied. */
  MuonServiceClient(const MuonServiceClient&) = delete;
  /** Native page session identity cannot be assigned. */
  MuonServiceClient& operator=(const MuonServiceClient&) = delete;
  /**
   * Invokes a permitted lifetime operation and decodes its response envelope.
   * @param operation acquire, release, getState, setAutostart, or watchState.
   * @param json Operation arguments; native owner and request identities are never
   *     accepted from the caller. Whole-application shutdown is not permitted.
   * @returns JSON result after success; rejects native, transport, and page errors.
   */
  cardio::promise<std::string> Call(std::string operation, std::string json);
  /**
   * Closes the native owner and awaits connection and I/O disposal.
   * @returns After all owned transport operations settle; safe to call repeatedly.
   */
  cardio::promise<void> Close();
  /** @returns True after disconnect or disposal; pending and future calls fail. */
  bool IsClosed() const;

 private:
  std::shared_ptr<MuonServiceClientImpl> impl_;
};
