/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#pragma once

#include "notifications/muon_notification_model.h"
#include <cardio.h>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace muon_internal {

/** Backend capabilities, including features whose support cannot be established. */
struct MuonNotificationBackendCapabilities {
  /** Whether a notification service is currently reachable. */
  bool available = false;
  /** Whether the service supports notification actions. */
  bool actions = false;
  /** Retention after normal process exit; nullopt when the backend cannot establish it. */
  std::optional<bool> persistence = false;
  /** Diagnostic for missing or unknown backend support. */
  std::string reason;
  /** Whether the backend can read application-wide OS notification settings. */
  bool app_status = false;
};

/** Current application-wide permission, separate from backend availability. */
struct MuonNotificationBackendStatus {
  /** True when enabled, false when disabled, or nullopt when unobservable. */
  std::optional<bool> enabled;
  /** Reason for a disabled or unknown state; empty when no diagnostic is needed. */
  std::string reason;
};

/** Supplies only the revision and action selected by the native notification. */
using MuonNotificationActivation = std::function<void(
    std::string token, std::optional<std::string> action_id)>;

/** Platform operations delegated by the durable application notification service. */
struct MuonNotificationBackend {
  /** Reads current service capabilities without inferring user permission. */
  std::function<cardio::promise<MuonNotificationBackendCapabilities>()> capabilities;
  /** Posts a new OS revision; same application ID updates use distinct handles. */
  std::function<cardio::promise<MuonNotificationHandle>(
      MuonNotificationPost, std::string token)> post;
  /** Cancels only a handle issued by the same backend/service instance. */
  std::function<cardio::promise<void>(MuonNotificationHandle)> cancel;
  /** Replaces the callback for future platform activations. */
  std::function<void(MuonNotificationActivation)> set_activation;
  /** Stops platform signal delivery. Does not withdraw posted notifications. */
  std::function<void()> stop;
  /** Reads application-wide OS settings without prompting the user. */
  std::function<cardio::promise<MuonNotificationBackendStatus>()> status;
};

/**
 * Application-owned notification service with renderer-owned watch leases.
 * All operations and destruction run on the dispatcher used for construction.
 */
class MuonNotificationService {
 public:
  /** Private implementation; owned by the service and its in-flight operations. */
  struct Impl;
  /** Creates a service with one durable snapshot and a platform backend. */
  MuonNotificationService(std::string path, std::string default_category,
                           MuonNotificationBackend backend,
                           std::function<int64_t()> clock);
  /**
   * Releases the backend and watches if startup failed before explicit Stop.
   * Normal shutdown must await Stop to finish already accepted operations.
   */
  ~MuonNotificationService();
  /** Starts receiving backend interactions. Called once after shared ownership exists. */
  void Start();
  /** Posts validated JSON and commits its revision before returning success. */
  cardio::promise<void> Post(std::string json);
  /** Cancels the OS notification, then durably invalidates its unconsumed revision. */
  cardio::promise<void> Cancel(std::string id);
  /** Returns OS permission status and unknown category status when requested. */
  cardio::promise<std::string> GetStatus(std::string json);
  /** Returns capabilities separately from permission and launch support. */
  cardio::promise<std::string> GetCapabilities();
  /** Reads durable unacknowledged interactions. */
  cardio::promise<std::string> GetPendingEvents();
  /** Durably acknowledges one event. */
  cardio::promise<void> AcknowledgeEvent(std::string token);
  /** Acquires, waits on, or releases a watch lease belonging to the trusted owner. */
  cardio::promise<std::string> WatchEvents(std::string json, std::string owner);
  /** Releases only watches and pending acquires for this renderer owner. */
  void ReleaseOwner(const std::string& owner);
  /** Stops new work, releases leases, and awaits already accepted OS/store operations. */
  cardio::promise<void> Stop();

 private:
  std::shared_ptr<Impl> impl_;
};

}  // namespace muon_internal
