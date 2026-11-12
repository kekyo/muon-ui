/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once

#include "services/muon_service_execution.h"
#include "services/muon_service_controller.h"

/** Private owner-bound calls, callback requests, and event subscriptions. */
struct MuonServiceBrokerImpl;

/**
 * Routes typed service calls for exact page references on one owner dispatcher.
 * Calls and subscriptions never acquire independent lifetime references. Native
 * execution may outlive cancellation of its public result; the runtime retains
 * its own input and library ownership until native completion.
 */
class MuonServiceBroker final {
 public:
  /** @param controller Authenticated reference registry, outliving this broker.
   *  @param host Runtime dispatcher and event source, outliving this broker. */
  MuonServiceBroker(MuonServiceController& controller, MuonServiceExecution host);
  /** Cancels remaining RPC work and detaches the event sink. */
  ~MuonServiceBroker();
  /** Owner-bound communication state cannot be copied. */
  MuonServiceBroker(const MuonServiceBroker&) = delete;
  /** Owner-bound communication state cannot be assigned. */
  MuonServiceBroker& operator=(const MuonServiceBroker&) = delete;

  /**
   * Processes an authenticated operation; replies may be deferred.
   * @param reference Exact service, owner, reference, and generation identity.
   * @param action describe, begin, result, cancel, callbackNext, callbackResult,
   *     subscribe, eventNext, or unsubscribe.
   * @param payload JSON object containing operation-specific inputs.
   * @param reply One JSON result. May reenter the broker or close its owner.
   * @throws std::invalid_argument For stale references or malformed operations.
   * @remarks begin registers a call before acknowledging its ID. result and
   *     callbackNext can then wait independently. At most one request of each
   *     kind may be pending per call/subscription. IDs are positive decimal
   *     uint64 strings scoped to the exact reference and never reused there.
   */
  void Request(const MuonServiceReference& reference, const std::string& action,
               const std::string& payload, std::function<void(const std::string&)> reply);

  /** @param reference Reference to release and whose RPC resources to reclaim.
   *  @remarks Idempotent; leaves all other references and resident policy intact. */
  void Release(const MuonServiceReference& reference);
  /** @param owner Disconnected authenticated page whose references are reclaimed. */
  void CloseOwner(const std::string& owner);
  /** Rejects new RPC work and settles all pending public replies. */
  void Shutdown();

 private:
  std::shared_ptr<MuonServiceBrokerImpl> impl_;
};
