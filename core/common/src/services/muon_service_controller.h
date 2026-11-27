/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once

#include "services/muon_service_lifecycle.h"

#include <functional>

/** Platform adapters called on the controller's owner dispatcher. */
struct MuonServiceBackend {
  /** Creates the recorded generation; completion reports empty text on success. */
  std::function<void(const MuonServiceSnapshot&,
                     std::function<void(const std::string&)>)> start;
  /** Disposes of the recorded generation, then invokes completion. */
  std::function<void(const MuonServiceSnapshot&, std::function<void()>)> stop;
  /** Atomically persists one override; failure must leave storage unchanged. */
  std::function<void(const std::string&, bool,
                     std::function<void(const std::string&)>)> save_autostart;
  /** Receives observations and request results in generation order. */
  std::function<void(const MuonServiceEvent&)> notify;
  /** Queues a task on the owner dispatcher; must never invoke it inline. */
  std::function<void(std::function<void()>)> post;
};

/** Private event queue and completion ownership. */
struct MuonServiceControllerImpl;

/**
 * Connects the common lifecycle to asynchronous runtime and storage adapters.
 *
 * All public operations and backend completions must run on one dispatcher.
 * A backend can complete inline, and notify can reenter the controller without
 * recursively delivering observations or overwriting another generation.
 * Dispatch is deferred: Acquire and SetAutostart return their identities before
 * notify can deliver a result. Call Activate after installing platform callbacks.
 * Shut down and drain the backend before destroying this controller.
 */
class MuonServiceController final {
 public:
  /** @param definitions Manifest services and loaded user preferences.
   *  @param backend Runtime, storage, and client notification adapters.
   *  @param resident_reason Launch cause for services with a saved resident reference. */
  MuonServiceController(const std::vector<MuonServiceDefinition>& definitions,
                         MuonServiceBackend backend, muon_service_start_reason resident_reason = MUON_SERVICE_START_APP);
  /** Requires a quiescent backend and settled lifecycle. */
  ~MuonServiceController();
  /** Controller identity cannot be copied. */
  MuonServiceController(const MuonServiceController&) = delete;
  /** Controller identity cannot be assigned. */
  MuonServiceController& operator=(const MuonServiceController&) = delete;

  /** Dispatches initial resident starts; repeated calls are harmless. */
  void Activate();
  /** Reconsiders resident demand when a platform startup restriction has lifted.
   *  @param id Configured service identity.
   *  @param generation Failed generation reported by the platform. */
  void RetryResidentStart(const std::string& id, uint64_t generation);
  /** @param id Configured service identity.
   *  @param owner Authenticated page or connection session.
   *  @return Owner-bound reservation; readiness is delivered through notify. */
  std::optional<MuonServiceReference> Acquire(const std::string& id,
                                             const std::string& owner);
  /** @param reference Exact owner-bound reference to release.
   *  @return True only for the first exact reference release. */
  bool Release(const MuonServiceReference& reference);
  /** @param owner Authenticated session that disconnected.
   *  @return Number of reservations recovered for that owner. */
  size_t ReleaseOwner(const std::string& owner);

  /** Retain or release a running generation for accepted work without a page or persisted setting.
   * @param id Configured service identity.
   * @param generation Generation owning the work.
   * @param active Whether this generation has unfinished work.
   * @return True when the running generation accepted the change.
   */
  bool SetWorkActive(const std::string& id, uint64_t generation, bool active);
  /** @param id Configured service identity.
   *  @param enabled Persistent user preference to commit.
   *  @return Nonzero persistence operation identity, or zero if rejected. */
  uint64_t SetAutostart(const std::string& id, bool enabled);
  /** Begins an OS-required stop; this is not a page-facing service stop API.
   *  @param id Configured service identity.
   *  @param generation Generation reported by the platform.
   *  @param reason OS restriction or deadline diagnostic. */
  void RequestRuntimeStop(const std::string& id, uint64_t generation,
                          const std::string& reason, MuonServiceStopCause cause = {MUON_SERVICE_STOP_HOST});
  /** Reports a runtime failure after all resources of that generation are gone.
   *  @param id Configured service identity.
   *  @param generation Terminated runtime generation.
   *  @param reason Runtime or OS failure diagnostic. */
  void RuntimeExited(const std::string& id, uint64_t generation,
                     const std::string& reason);
  /** Ends the whole application without rewriting saved preferences.
   *  @param reason Diagnostic sent to invalidated clients.
   *  @param cause Stop observation delivered to each active generation. */
  void Shutdown(const std::string& reason, MuonServiceStopCause cause = {MUON_SERVICE_STOP_HOST});
  /** @param reference Exact owner-bound reference to check.
   *  @return True only for a currently callable exact reference. */
  bool IsReady(const MuonServiceReference& reference) const;
  /** @param id Configured service identity.
   *  @return A non-owning observation, or no value for an unknown identity. */
  std::optional<MuonServiceSnapshot> GetSnapshot(const std::string& id) const;
  /** @return True when no runtime resources or persistence actions remain. */
  bool IsQuiescent() const;
  /** @return True after whole-application shutdown has rejected new work. */
  bool IsShuttingDown() const;

 private:
  std::shared_ptr<MuonServiceControllerImpl> impl_;
};
