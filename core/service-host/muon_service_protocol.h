/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once

#include "services/muon_service_controller.h"

#include <map>
#include <set>

/** @param snapshot Copied state to encode without creating a reference.
 *  @return JSON object preserving 64-bit identities as strings. */
std::string SerializeMuonServiceSnapshot(const MuonServiceSnapshot& snapshot);

/** Pending operation associated with one authenticated transport connection. */
struct MuonServicePendingRequest {
  /** Connection identity assigned by the host, never trusted from JSON. */
  std::string owner;
  /** Client correlation identity, unique among that connection's pending calls. */
  std::string request;
};

/** A connection-owned state subscription that never reserves a runtime reference. */
struct MuonServiceObservation {
  std::string owner; /**< Authenticated owner of this subscription. */
  std::string service; /**< Configured logical service being observed. */
  std::string delivered; /**< Last JSON state delivered to the observer. */
  std::string latest; /**< Most recent coalesced JSON state. */
  std::optional<std::string> request; /**< At most one waiting next request. */
};

/** Native-only UI preference adapter; UI settings never acquire service references. */
struct MuonServiceUiPolicy {
  std::function<bool()> get; /**< Reads the committed UI auto-display preference. */
  /** Persists UI policy; completion receives empty text on success. */
  std::function<void(bool, std::function<void(const std::string&)>)> set;
  /** Updates trusted launch paths; not exposed by the renderer service API. */
  std::function<void(std::string, std::function<void(const std::string&)>)> configure{};
};

/**
 * Minimal lifetime protocol used by independent desktop service hosts.
 *
 * Requests carry a string requestId and an op (acquire, release, getState,
 * setAutostart, watchState, shutdown). Watches use acquire/next/release operations
 * and coalesce state while their consumer is busy. Watches and setting changes
 * never acquire a client reference. Service and reference identities are strings. Response
 * result/error envelopes use the same requestId. All methods run on the owner
 * dispatcher. Transport authentication and framing are platform responsibilities.
 */
class MuonServiceProtocol final {
 public:
  /** @param controller Serialized lifetime controller, which must outlive this object.
   *  @param send Writes a JSON response or event to an authenticated live owner.
   *  @param ui Native UI auto-display settings, absent in lifecycle-only drivers. */
  MuonServiceProtocol(MuonServiceController& controller,
                       std::function<void(const std::string&, const std::string&)> send,
                       MuonServiceUiPolicy ui = {});
  /** @param owner New host-assigned connection identity.
   *  @return True if the identity was newly registered. */
  bool OpenOwner(const std::string& owner);
  /** @param owner Ended connection whose reservations must be reclaimed. */
  void CloseOwner(const std::string& owner);
  /** @param owner Authenticated connection carrying the request.
   *  @param json One complete JSON frame, at most 1 MiB.
   *  @return False for malformed framing/identity or duplicated pending requestId;
   *      the transport must close the connection and call CloseOwner. */
  bool Receive(const std::string& owner, const std::string& json);
  /** @param event Ordered lifecycle event supplied through backend.notify. */
  void Notify(const MuonServiceEvent& event);

 private:
  MuonServiceController& controller_;
  std::function<void(const std::string&, const std::string&)> send_;
  std::set<std::string> owners_;
  std::map<uint64_t, MuonServicePendingRequest> pending_;
  std::map<uint64_t, MuonServicePendingRequest> policies_;
  std::map<uint64_t, MuonServiceObservation> observations_;
  uint64_t observation_id_ = 0;
  MuonServiceUiPolicy ui_;
  std::set<std::pair<std::string, std::string>> ui_pending_;
};
