/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace muon_internal {

/** One application-defined notification action. */
struct MuonNotificationAction {
  /** Application action identifier. */
  std::string id;
  /** User-visible button label. */
  std::string title;
};

/** Validated notification content, independent of an OS backend. */
struct MuonNotificationPost {
  /** Application-owned notification identifier. */
  std::string id;
  /** User-visible title. */
  std::string title;
  /** User-visible plain-text body. */
  std::string body;
  /** Application category; not an OS category. */
  std::string category;
  /** Serialized JSON value, including null when omitted. */
  std::string data = "null";
  /** Up to three additional actions. */
  std::vector<MuonNotificationAction> actions;
};

/** OS handle scoped to the service instance which issued it. */
struct MuonNotificationHandle {
  /** Backend name; future backends must not interpret another backend's ID. */
  std::string backend;
  /** D-Bus GUID and unique service owner, identifying one service epoch. */
  std::string owner;
  /** Backend identifier, interpreted only with backend and owner. */
  uint32_t id = 0;
};

/** An unconsumed notification revision and its reserved event capacity. */
struct MuonNotificationRevision {
  /** Opaque, unpredictable revision token and eventual event ID. */
  std::string token;
  /** Persisted application content. */
  MuonNotificationPost post;
  /** Milliseconds since the Unix epoch when posted. */
  int64_t created_at = 0;
  /** Current native notification handle. */
  MuonNotificationHandle handle;
};

/** Persisted interaction, retained until acknowledged or expired. */
struct MuonNotificationEvent {
  /** Stable revision token. */
  std::string event_id;
  /** Application notification identifier. */
  std::string id;
  /** Empty optional means the ordinary activation action. */
  std::optional<std::string> action_id;
  /** Serialized application JSON payload. */
  std::string data;
  /** Time the interaction was received. */
  int64_t received_at = 0;
  /** Internal monotonic delivery cursor, never exposed on the event. */
  uint64_t cursor = 0;
};

/** Acknowledged revision retained for bounded duplicate diagnostics. */
struct MuonNotificationAcknowledgement {
  /** Acknowledged event ID. */
  std::string token;
  /** Time the application acknowledged it. */
  int64_t acknowledged_at = 0;
};

/** Complete application-owned notification state. */
struct MuonNotificationSnapshot {
  /** Last allocated event cursor. */
  uint64_t cursor = 0;
  /** Unconsumed revisions; combined with pending, limited to 256. */
  std::vector<MuonNotificationRevision> revisions;
  /** Unacknowledged interactions. */
  std::vector<MuonNotificationEvent> pending;
  /** At most 1024 acknowledgement records. */
  std::vector<MuonNotificationAcknowledgement> acknowledged;
};

/** Validates the shared post JSON; throws on unknown fields or invalid values.
 * @param json Serialized public post options.
 * @param default_category Category used only when the post omits one.
 * @return Validated application content.
 */
MuonNotificationPost ParseMuonNotificationPost(
    const std::string& json, const std::string& default_category);
/** Validates a public identifier without changing its contents.
 * @param value Identifier to validate; throws if blank, invalid, or too long.
 */
void ValidateMuonNotificationIdentifier(const std::string& value);
/** Serializes one UTF-8 string as a JSON string value.
 * @param value Unescaped UTF-8 text.
 * @return A quoted JSON string.
 */
std::string QuoteMuonNotificationString(const std::string& value);
/** Removes records outside the public 30-day retention window.
 * @param state State to prune.
 * @param now Current Unix time in milliseconds.
 */
void PruneMuonNotifications(MuonNotificationSnapshot& state, int64_t now);
/** Reserves one revision, invalidating only an unconsumed same-ID revision.
 * @param state State to update; throws when its combined capacity is full.
 * @param post Validated application content.
 * @param token New unpredictable revision token.
 * @param now Current Unix time in milliseconds.
 */
void ReserveMuonNotification(MuonNotificationSnapshot& state,
                             MuonNotificationPost post,
                             std::string token,
                             int64_t now);
/** Converts the first valid action to one pending event; unknown tokens are ignored.
 * @param state State to update.
 * @param token Revision selected by the backend.
 * @param action_id Named action, or nullopt for ordinary activation.
 * @param now Current Unix time in milliseconds.
 * @return Whether a new event was created.
 */
bool ActivateMuonNotification(MuonNotificationSnapshot& state,
                              const std::string& token,
                              const std::optional<std::string>& action_id,
                              int64_t now);
/** Acknowledges one pending event; unknown event IDs are a no-op.
 * @param state State to update.
 * @param token Event ID acknowledged by the application.
 * @param now Current Unix time in milliseconds.
 */
void AcknowledgeMuonNotification(MuonNotificationSnapshot& state,
                                 const std::string& token, int64_t now);
/** Serializes only the public fields of pending events after the given cursor.
 * @param state Current snapshot.
 * @param after_cursor Exclusive lower cursor bound; zero includes all events.
 * @return Public event array as JSON.
 */
std::string SerializeMuonNotificationEvents(
    const MuonNotificationSnapshot& state, uint64_t after_cursor);
/** Serializes the versioned durable snapshot.
 * @param state Snapshot to save.
 * @return Versioned JSON containing all retained records.
 */
std::string SerializeMuonNotificationSnapshot(const MuonNotificationSnapshot& state);
/** Reads and validates the durable snapshot; corruption is never reset silently.
 * @param json Persisted snapshot text.
 * @return Validated snapshot, or throws on corruption.
 */
MuonNotificationSnapshot ParseMuonNotificationSnapshot(const std::string& json);

}  // namespace muon_internal
