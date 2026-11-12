/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once

#include "muon_service_api.h"
#include "services/muon_service_execution.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

/** Resolved, platform-independent inputs for one native service. */
struct MuonNativeServiceSpec {
  /** Logical identity within this host. */
  std::string id;
  /** Absolute path of the service library supplied by the application. */
  std::string library_path;
  /** Absolute private data directory, already created by the platform. */
  std::string data_directory;
  /** Immutable string configuration passed to the factory. */
  std::vector<std::pair<std::string, std::string>> config;
};

/** Platform operations; no operation depends on a UI runtime. */
struct MuonNativeServicePlatform {
  /**
   * Loads factory metadata or returns null and an error. The returned shared
   * pointer must retain the library until its final release, including while a
   * completion is returning through native code. Called on the owner dispatcher.
   */
  std::function<std::shared_ptr<const muon_service_factory>(
      const std::string& path, std::string& error)> load;
  /** Queues a task on the owner dispatcher; must never invoke it inline. */
  std::function<void(std::function<void()>)> post;
  /** Writes a copied diagnostic scoped to the logical service identity. */
  std::function<void(const std::string& id, muon_log_level level,
                     const std::string& message)> log;
};

/** Private library and instance ownership. */
struct MuonNativeServiceHostImpl;

/**
 * Executes the native service ABI on one externally owned dispatcher.
 *
 * The platform serializes all calls, native callbacks, and queued tasks on that
 * dispatcher. This host owns no worker thread. A loaded factory is shared by
 * active service IDs, while each ID owns an independent instance and context.
 * The caller must finish all starts, stops, and posted tasks before destruction.
 */
class MuonNativeServiceHost final {
 public:
  /** @param platform Loader, owner queue, and service-scoped diagnostics. */
  explicit MuonNativeServiceHost(MuonNativeServicePlatform platform);
  /** Requires IsIdle(); never unloads a running service as a substitute for stop. */
  ~MuonNativeServiceHost();
  /** Instance ownership cannot be copied. */
  MuonNativeServiceHost(const MuonNativeServiceHost&) = delete;
  /** Instance ownership cannot be assigned. */
  MuonNativeServiceHost& operator=(const MuonNativeServiceHost&) = delete;

  /**
   * Loads or reuses a factory and starts one generation.
   * @param spec Resolved library, identity, and immutable configuration.
   * @param generation Nonzero generation reserved by the lifecycle manager.
   * @param complete Deferred result, empty on success, otherwise a diagnostic.
   * @remarks Duplicate active identities fail; startup failure releases all
   *     host-owned resources before reporting completion. Factory startup owns
   *     rollback of implementation resources before reporting failure itself.
   */
  void Start(const MuonNativeServiceSpec& spec, uint64_t generation,
             std::function<void(const std::string&)> complete);

  /**
   * Disposes of a successfully started generation.
   * @param id Logical identity.
   * @param generation Generation that must match the active instance.
   * @param complete Deferred completion after instance resources are disposed.
   * @remarks The lifecycle manager calls this after successful Start. Repeated
   *     requests join the same stop. Unknown or stale generations complete
   *     without touching another generation. Stop during startup is a contract
   *     error and throws std::logic_error.
   */
  void Stop(const std::string& id, uint64_t generation,
            std::function<void()> complete);

  /** @param id Logical service identity.
   *  @param generation Exact running generation.
   *  @return Validated metadata for a running generation.
   *  @throws std::invalid_argument If the generation is not running. */
  const MuonServiceInterface& Interface(const std::string& id, uint64_t generation) const;

  /** @param id Logical service identity.
   *  @param generation Exact running generation.
   *  @return Interface JSON suitable for the page SDK, for a running generation.
   *  @throws std::invalid_argument If the generation is not running. */
  std::string InterfaceJson(const std::string& id, uint64_t generation) const;

  /**
   * Starts a typed call, retaining its inputs and factory through native completion.
   * @param id Logical service identity.
   * @param generation Exact running generation.
   * @param method Declared public method.
   * @param arguments Owned typed input values.
   * @param callback Dispatcher for callbacks supplied within those inputs.
   * @param complete Exactly one deferred terminal result, including cancellation.
   * @return Nonzero host call identity, distinct from transport request IDs.
   * @throws std::invalid_argument For stale generation or invalid method/arguments.
   */
  uint64_t Invoke(const std::string& id, uint64_t generation, const std::string& method,
      std::vector<MuonServiceValue> arguments, MuonServiceCallbackHandler callback,
      MuonServiceCallCompletion complete);

  /**
   * Cancels one call and invalidates its callbacks, preserving other invocations.
   * @param id Logical service identity.
   * @param generation Exact generation that owns the call.
   * @param call_id Host call identity returned by Invoke.
   * @remarks Unknown, stale, completed, and repeated cancellations have no effect.
   *     The native call and input storage remain alive until its final completion.
   */
  void Cancel(const std::string& id, uint64_t generation, uint64_t call_id);

  /** @param sink Event receiver on the owner dispatcher; empty detaches it. */
  void SetEventSink(MuonServiceEventSink sink);

  /** @returns Borrowed operations for a broker; this host must outlive them. */
  MuonServiceExecution Execution();

  /** @return True when every instance and host completion has settled. */
  bool IsIdle() const;

 private:
  std::shared_ptr<MuonNativeServiceHostImpl> impl_;
};
