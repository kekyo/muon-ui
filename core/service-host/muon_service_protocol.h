/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once

#include "services/muon_service_controller.h"

#include <map>
#include <set>

/** Pending acquisition associated with one authenticated transport connection. */
struct MuonServicePendingRequest {
  /** Connection identity assigned by the host, never trusted from JSON. */
  std::string owner;
  /** Client correlation identity, unique among that connection's pending calls. */
  std::string request;
};

/**
 * Minimal lifetime protocol used by independent desktop service hosts.
 *
 * Requests carry a string requestId and an op (acquire, release, getState,
 * shutdown). Service and reference identities are string fields. Response
 * result/error envelopes use the same requestId. All methods run on the owner
 * dispatcher. Transport authentication and framing are platform responsibilities.
 */
class MuonServiceProtocol final {
 public:
  /** @param controller Serialized lifetime controller, which must outlive this object.
   *  @param send Writes a JSON response or event to an authenticated live owner. */
  MuonServiceProtocol(MuonServiceController& controller,
                       std::function<void(const std::string&, const std::string&)> send);
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
};
