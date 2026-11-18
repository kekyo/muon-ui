/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 */
#pragma once
#include "services/muon_service_io.h"
#include <chrono>
#include <map>
#include <mutex>

/** Cancellation shared by the owner dispatcher and the active platform transport. */
class MuonServiceIoCancellation final {
 public:
  std::atomic_bool cancelled{false}; /**< Worker-visible cancellation for file operations. */
  /** Records the first cause and interrupts any active system operation. */
  void Cancel(bool timeout = false);
  /** Installs a thread-safe interrupter. An already cancelled operation interrupts immediately. */
  void Bind(std::function<void()> interrupt);
  /** Throws the first recorded error if cancellation was requested. */
  void Check() const;
 private:
  std::mutex mutex_;
  std::function<void()> interrupt_;
  std::atomic_bool timed_out_{false};
};

/** One validated HTTP hop. Platform transports never follow redirects themselves. */
struct MuonServiceHttpHop {
  std::string scheme, host, origin, target; /**< Normalized scheme, unbracketed host, serialized origin and origin-form target. */
  uint16_t port = 0; /**< Effective TCP port. */
  std::string method; /**< Validated HTTP method. */
  std::map<std::string, std::string> headers; /**< Lowercase user headers. */
  std::vector<uint8_t> body; /**< Owned request content. */
  int timeout_ms = 30000; /**< Remaining transfer budget. */
};

/** Single-hop response. Transports enforce payload and header bounds while reading. */
struct MuonServiceHttpResult {
  int status = 0; /**< HTTP status, including error responses. */
  std::map<std::string, std::string> headers; /**< Lowercase field names. */
  std::vector<uint8_t> body; /**< At most 1 MiB of body data. */
};

inline constexpr size_t MuonHttpBodyLimit = 1024 * 1024;
inline constexpr size_t MuonHttpHeaderLimit = 64 * 1024;
/** @return Total request deadline, checked before an operation enters the platform pool. */
int MuonServiceHttpTimeout(const std::string& parameters);
/** Validates policy before every hop, forwards cancellation, and encodes the requested result. */
MuonServiceIoReply RunMuonServiceHttp(const std::string& policy, const MuonServiceIoRequest& request,
    const MuonServiceHttpTransport& transport, MuonServiceIoCancellation& cancel);
/** @return Validated normalized URL components. Throws invalid-input for unsupported authority or syntax. */
MuonServiceHttpHop ParseMuonServiceHttpUrl(const std::string& url);
/** Stores a response header, combining repeated values and normalizing the name. */
void AddMuonServiceHttpHeader(MuonServiceHttpResult& result, std::string name, std::string value);
