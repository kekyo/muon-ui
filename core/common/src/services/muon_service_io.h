/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 */
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

/** Runs file work on a platform pool, completing on the owner dispatcher. */
using MuonServiceIoSubmit = std::function<void(std::function<void()>, std::function<void(std::string)>)>;

/** Immutable policy and platform adapter shared across a service's generations. */
struct MuonServiceIoSettings {
  std::string policy = "{}"; /**< Normalized per-service permissions from the application registry. */
  MuonServiceIoSubmit submit{}; /**< Platform asynchronous file executor. */
};

/** Owned buffers for one portable I/O request. */
struct MuonServiceIoRequest {
  std::string operation; /**< SDK operation name. */
  std::string parameters; /**< JSON object containing operation metadata. */
  std::vector<uint8_t> body; /**< Optional binary input. */
};

/** Owned completion, delivered once on the generation's dispatcher. */
struct MuonServiceIoReply {
  std::string metadata = "null"; /**< JSON metadata on success. */
  std::vector<uint8_t> body{}; /**< Successful binary output. */
  std::string code{}, message{}; /**< Empty on success, otherwise stable code and diagnostic. */
};

struct MuonServiceIoState;
/** Outstanding I/O and cancellation owned by one exact service generation. */
class MuonServiceIo final {
 public:
  /** @param directory Private data root. @param settings Grants and executor. @param post Queue on the owner dispatcher. */
  MuonServiceIo(std::string directory, MuonServiceIoSettings settings, std::function<void(std::function<void()>)> post);
  /** @param request Owned parameters and bytes. @param complete Deferred result. @return Idempotent cancellation callback. */
  std::function<void()> Run(MuonServiceIoRequest request, std::function<void(MuonServiceIoReply)> complete);
  /** Cancels all pending work and closes admission. @param drained Called after the last completion returns. */
  void Close(std::function<void()> drained);
 private:
  std::shared_ptr<MuonServiceIoState> state_;
};
