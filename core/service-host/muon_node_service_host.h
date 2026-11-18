/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once
#include "services/muon_service_execution.h"
#include "services/muon_service_reason.h"
#include <chrono>
#include <muon_plugin_api.h>

/** Resolved input for one independently owned Node.js service process. */
struct MuonNodeServiceSpec {
  std::string id; /**< Logical service identity. */
  std::string project; /**< Absolute npm project directory. */
  std::string entry; /**< Import specifier relative to that project. */
  std::string executable; /**< Absolute Node executable. */
  std::string bridge; /**< Absolute bundled service bridge. */
  std::string data_directory; /**< Writable service-private data directory. */
  std::vector<std::pair<std::string, std::string>> config; /**< String settings. */
  muon_service_start_reason start_reason = MUON_SERVICE_START_ACQUIRE; /**< Demand for this generation. */
  std::shared_ptr<MuonServiceCancellation> signal{}; /**< Shared cancellation during asynchronous initialization. */
};

/** Owner dispatcher callbacks, also used while draining child processes. */
struct MuonNodeServicePlatform {
  std::function<void(std::function<void()>)> post; /**< Never invokes inline. */
  /** Schedules a deferred deadline, returning its idempotent cancellation. */
  std::function<std::function<void()>(std::chrono::milliseconds,
      std::function<void()>)> delay;
  /** Copies diagnostics without retaining borrowed arguments. */
  std::function<void(const std::string&, muon_log_level, const std::string&)> log;
  /** Reports failure after the exact child generation has been reaped. */
  std::function<void(const std::string&, uint64_t, const std::string&)> exited;
  std::chrono::milliseconds lifecycle_timeout; /**< Startup and normal stop limit. */
};

struct MuonNodeServiceHostImpl; /**< Private process and request ownership. */

/** Runs service-owned Node children through the common typed execution API. */
class MuonNodeServiceHost final {
 public:
  /** @param platform Serialized owner callbacks and lifecycle deadline. */
  explicit MuonNodeServiceHost(MuonNodeServicePlatform platform);
  /** Requires IsIdle(): no child, transport I/O, or deferred task may remain. */
  ~MuonNodeServiceHost();
  MuonNodeServiceHost(const MuonNodeServiceHost&) = delete;
  MuonNodeServiceHost& operator=(const MuonNodeServiceHost&) = delete;
  /** Starts a copied spec; completion is deferred and empty on success. */
  void Start(const MuonNodeServiceSpec& spec, uint64_t generation,
      std::function<void(const std::string&)> complete);
  /** Completes after author stop, process exit, and transport resource drainage. */
  void Stop(const std::string& id, uint64_t generation, std::function<void()> complete, MuonServiceStopCause cause = {});
  /** @returns Borrowed operations; this host must outlive them. */
  MuonServiceExecution Execution();
  /** @returns Whether all children and owner callbacks have drained. */
  bool IsIdle() const;
 private:
  std::shared_ptr<MuonNodeServiceHostImpl> impl_;
};
