/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#pragma once

#include <cardio.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>

/** A nonblocking TCP connection owned by its creating cardio dispatcher.
 * One read and one write may run concurrently. The caller must retain each
 * buffer until its operation completes and explicitly close the connection.
 */
struct MuonServerSocket {
  /** Reads available bytes; zero means EOF unless the supplied buffer is empty.
   * Receiving EOF does not disable writes. Concurrent reads are rejected.
   */
  std::function<cardio::promise<size_t>(std::span<uint8_t>)> read;
  /** Writes every byte to the OS, waiting for capacity when necessary.
   * Completion does not acknowledge receipt by the peer. Concurrent writes
   * are rejected. An error may follow a successful partial write.
   */
  std::function<cardio::promise<void>(std::span<const uint8_t>)> write;
  /** Cancels outstanding operations, awaits their release and closes once.
   * Await write before close when all supplied bytes must be sent.
   */
  std::function<cardio::promise<void>()> close;
};

/** An IPv4 loopback listener owned by its creating cardio dispatcher. */
struct MuonServerListener {
  /** The actual bound port, including an OS-selected port requested with zero. */
  uint16_t port;
  /** Accepts one connection. Concurrent accepts are rejected. */
  std::function<cardio::promise<std::shared_ptr<MuonServerSocket>>()> accept;
  /** Cancels a pending accept and closes once; accepted sockets remain open. */
  std::function<cardio::promise<void>()> close;
};

/** Owns native listeners and connections on the creating cardio dispatcher.
 * Stop must complete before the dispatcher is shut down. No worker or polling
 * loop is added by this service.
 */
struct MuonServerSockets {
  /** Binds 127.0.0.1 synchronously; zero is available for internal test drivers.
   * Throws on bind failure or after stop has begun.
   */
  std::function<std::shared_ptr<MuonServerListener>(uint16_t)> listen;
  /** Closes all listeners and connections and awaits cancellation cleanup. */
  std::function<cardio::promise<void>()> stop;
};

/** Creates a TCP service on the current cardio dispatcher.
 * All operations must be called on that dispatcher.
 * @return A service that must be stopped before dispatcher shutdown.
 */
std::shared_ptr<MuonServerSockets> CreateMuonServerSockets();
