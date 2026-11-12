/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#pragma once
#include "services/muon_quickjs_service_host.h"
#include "services/muon_service_plugin_pool.h"

struct MuonQuickJsServicePluginsImpl;

/** Coordinates bounded native acquisition before a service heap is created. */
class MuonQuickJsServicePlugins final {
 public:
  /** @param pool Process-wide plugin pool, outliving this coordinator.
   * @param delay Owner-dispatcher timer with an idempotent cancellation handle. */
  MuonQuickJsServicePlugins(MuonServicePluginPool& pool,
      decltype(MuonQuickJsServicePlatform::delay) delay);
  /** Requires all pending acquisitions and unused leases to have drained. */
  ~MuonQuickJsServicePlugins();
  /** @param host Host that calls Attach through its platform extension factory.
   * @param spec Loaded bundle, including its absolute startup deadline.
   * @param generation Exact lifecycle generation.
   * @param complete Startup success or failure, delivered once on the owner thread.
   * @remarks An expired acquisition never creates a heap; a late lease is closed. */
  void Start(MuonQuickJsServiceHost& host, const MuonQuickJsServiceSpec& spec,
      uint64_t generation, std::function<void(const std::string&)> complete);
  /** @param id Service identity supplied to Start.
   * @param generation Exact lifecycle generation.
   * @param engine Newly created owner-thread engine.
   * @param wake Deferred engine job pump.
   * @param fault Deferred generation failure.
   * @returns Module and cleanup hooks, or empty hooks for a service without plugins. */
  MuonQuickJsServiceExtension Attach(const std::string& id, uint64_t generation,
      MuonQuickJsEngine& engine, std::function<void()> wake,
      std::function<void(const std::string&)> fault);
  /** @returns True after acquisition and unused-lease cleanup complete. */
  bool IsIdle() const;
 private:
  std::shared_ptr<MuonQuickJsServicePluginsImpl> impl_;
};
