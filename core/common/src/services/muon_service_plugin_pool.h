/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#pragma once

#include "plugins/muon_plugin_runtime.h"
#include "quickjs/muon_quickjs_engine.h"

/** Native capabilities granted to one configured QuickJS service. */
struct MuonServicePluginDefinition {
  std::string id; /**< Logical service identity. */
  std::vector<std::string> allow; /**< Public function paths or glob patterns. */
};

struct MuonServicePluginPoolImpl;
struct MuonServicePluginLeaseImpl;
struct MuonQuickJsPluginBindingsImpl;
class MuonQuickJsPluginBindings;

/** Retains one generation's native owner and its shared plugin runtime. */
class MuonServicePluginLease final {
 public:
  /** Requires Close to have completed; destruction never fakes native completion. */
  ~MuonServicePluginLease();
  /**
   * Attaches native modules to one newly created engine, once per lease.
   * @param engine Owner-thread engine, alive until bindings.Detach().
   * @param wake Queues the engine's bounded promise-job pump.
   * @param fault Reports a deferred engine execution failure.
   * @returns Generation-specific module loader and cleanup handle.
   * @remarks Does not replace JSContext opaque data or the engine module loader.
   */
  std::shared_ptr<MuonQuickJsPluginBindings> Attach(MuonQuickJsEngine& engine,
      std::function<void()> wake, std::function<void(const std::string&)> fault);
  /**
   * Closes admission, invalidates the owner, and waits for real native cleanup.
   * @param complete Deferred callback after calls return and the final user's
   * plugin stop, runtime destruction, and library unload complete.
   * @remarks Idempotent; accepts multiple completion observers. Final-user Stop
   * begins before waiting for calls, so it can cancel plugin-owned I/O.
   */
  void Close(std::function<void()> complete);
 private:
  explicit MuonServicePluginLease(std::shared_ptr<MuonServicePluginLeaseImpl> impl);
  std::shared_ptr<MuonServicePluginLeaseImpl> impl_;
  friend struct MuonServicePluginPoolImpl;
};

/** QuickJS values belonging to exactly one native owner and execution generation. */
class MuonQuickJsPluginBindings final {
 public:
  /** Requires Detach before destruction, while the engine still exists. */
  ~MuonQuickJsPluginBindings();
  /** @returns A host module for an authorized muon: namespace, or JS exception.
   * @remarks Call only within the engine's protected module-loading turn. */
  JSModuleDef* LoadModule(JSContext* context, const char* name);
  /** Synchronously severs JS delivery and releases every C++-held JSValue.
   * Native callbacks and results subsequently fail or drain without using JS. */
  void Detach();
  /** @param complete Deferred native cleanup completion, equivalent to lease.Close. */
  void Close(std::function<void()> complete);
 private:
  explicit MuonQuickJsPluginBindings(std::shared_ptr<MuonQuickJsPluginBindingsImpl> impl);
  std::shared_ptr<MuonQuickJsPluginBindingsImpl> impl_;
  friend class MuonServicePluginLease;
};

/** Owns the service process's sole plugin runtime, shared across heap generations. */
class MuonServicePluginPool final {
 public:
  /**
   * @param directory Directory used by explicit plugin load entries.
   * @param plugins Process-wide verified load union; configurations must agree.
   * @param definitions Separate public capabilities for each service.
   * @param services Existing owner dispatcher, loader, allocator, and log adapter.
   * @remarks post_owner_task must always defer. Owner availability and message
   * delivery are replaced by the pool; no UI/platform namespaces are installed.
   */
  MuonServicePluginPool(std::filesystem::path directory,
      std::vector<MuonPluginRuntimeLoadEntry> plugins,
      std::vector<MuonServicePluginDefinition> definitions,
      MuonPluginRuntimeServices services);
  /** Requires IsIdle; pending native operations may still execute plugin code. */
  ~MuonServicePluginPool();
  /**
   * Acquires a fresh native owner, waiting while the previous runtime stops.
   * @param id Declared service identity.
   * @param generation Nonzero lifecycle generation, never truncated to owner ID.
   * @param complete Deferred lease or failure; a late unneeded lease must Close.
   */
  void Acquire(const std::string& id, uint64_t generation,
      std::function<void(std::shared_ptr<MuonServicePluginLease>, const std::string&)> complete);
  /** @returns True after all leases, native calls, posted work and unloads drain. */
  bool IsIdle() const;
 private:
  std::shared_ptr<MuonServicePluginPoolImpl> impl_;
};
