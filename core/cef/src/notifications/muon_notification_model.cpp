/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "notifications/muon_notification_model.h"
#include "yyjson.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <string_view>

namespace muon_internal {

static constexpr int64_t kRetentionMs = 30LL * 24 * 60 * 60 * 1000;
static constexpr uint64_t kMaximumCursor = 9007199254740991ULL;
using JsonDocument = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>;

static void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

static JsonDocument ReadJson(const std::string& json) {
  auto document = JsonDocument(yyjson_read(json.data(), json.size(), 0),
                               &yyjson_doc_free);
  Require(document != nullptr, "Invalid notification JSON");
  return document;
}

static std::string WriteJson(yyjson_val* value) {
  auto length = size_t{0};
  auto* bytes = yyjson_val_write(value, 0, &length);
  Require(bytes != nullptr, "Failed to serialize notification JSON");
  auto result = std::string(bytes, length);
  std::free(bytes);
  return result;
}

std::string QuoteMuonNotificationString(const std::string& value) {
  auto document = std::unique_ptr<yyjson_mut_doc, decltype(&yyjson_mut_doc_free)>(
      yyjson_mut_doc_new(nullptr), &yyjson_mut_doc_free);
  Require(document != nullptr, "Failed to allocate notification JSON");
  auto* string = yyjson_mut_strn(document.get(), value.data(), value.size());
  Require(string != nullptr, "Failed to allocate notification JSON string");
  yyjson_mut_doc_set_root(document.get(), string);
  auto length = size_t{0};
  auto* bytes = yyjson_mut_write(document.get(), 0, &length);
  Require(bytes != nullptr, "Invalid UTF-8 notification string");
  auto result = std::string(bytes, length);
  std::free(bytes);
  return result;
}

static std::string Quote(const std::string& value) {
  return QuoteMuonNotificationString(value);
}

static bool IsWhitespace(uint32_t value) {
  // Match ECMAScript String.trim(), including the byte order mark.
  return (value >= 9 && value <= 13) || value == 32 || value == 0xa0 ||
         value == 0x1680 || (value >= 0x2000 && value <= 0x200a) ||
         value == 0x2028 || value == 0x2029 || value == 0x202f ||
         value == 0x205f || value == 0x3000 || value == 0xfeff;
}

static void ValidateText(const std::string& value, size_t maximum,
                          bool require_nonblank) {
  auto units = size_t{0};
  auto nonblank = false;
  for (auto index = size_t{0}; index < value.size();) {
    auto first = static_cast<unsigned char>(value[index++]);
    uint32_t codepoint = first;
    auto remaining = size_t{0};
    uint32_t minimum = 0;
    if (first >= 0xc2 && first <= 0xdf) {
      codepoint &= 0x1f; remaining = 1; minimum = 0x80;
    } else if (first >= 0xe0 && first <= 0xef) {
      codepoint &= 0x0f; remaining = 2; minimum = 0x800;
    } else if (first >= 0xf0 && first <= 0xf4) {
      codepoint &= 0x07; remaining = 3; minimum = 0x10000;
    } else {
      Require(first < 0x80, "Invalid UTF-8 notification string");
    }
    Require(index + remaining <= value.size(), "Invalid UTF-8 notification string");
    for (auto part = size_t{0}; part < remaining; ++part) {
      const auto next = static_cast<unsigned char>(value[index++]);
      Require((next & 0xc0) == 0x80, "Invalid UTF-8 notification string");
      codepoint = (codepoint << 6) | (next & 0x3f);
    }
    Require(codepoint >= minimum && codepoint <= 0x10ffff &&
                !(codepoint >= 0xd800 && codepoint <= 0xdfff) && codepoint != 0,
            "Invalid notification string");
    units += codepoint > 0xffff ? 2 : 1;
    nonblank = nonblank || !IsWhitespace(codepoint);
  }
  Require(units <= maximum, "Notification string exceeds its length limit");
  Require(!require_nonblank || nonblank, "Notification string must not be blank");
}

void ValidateMuonNotificationIdentifier(const std::string& value) {
  ValidateText(value, 128, true);
}

static std::string String(yyjson_val* value) {
  Require(yyjson_is_str(value), "Notification field must be a string");
  return std::string(yyjson_get_str(value), yyjson_get_len(value));
}

static void Fields(yyjson_val* value,
                    std::initializer_list<std::string_view> allowed) {
  Require(yyjson_is_obj(value), "Notification options must be an object");
  auto seen = std::set<std::string>{};
  size_t index = 0, count = 0;
  yyjson_val* key = nullptr;
  yyjson_val* child = nullptr;
  yyjson_obj_foreach(value, index, count, key, child) {
    const auto name = String(key);
    Require(std::find(allowed.begin(), allowed.end(), name) != allowed.end() &&
                seen.insert(name).second,
            "Unknown or duplicate notification field");
  }
}

static void ValidateDataDepth(yyjson_val* value, size_t depth) {
  if (!yyjson_is_obj(value) && !yyjson_is_arr(value)) return;
  Require(depth < 64, "Notification data exceeds 64 containers");
  size_t index = 0, count = 0;
  yyjson_val* child = nullptr;
  if (yyjson_is_arr(value)) {
    yyjson_arr_foreach(value, index, count, child) {
      ValidateDataDepth(child, depth + 1);
    }
  } else {
    yyjson_val* key = nullptr;
    yyjson_obj_foreach(value, index, count, key, child) {
      ValidateDataDepth(child, depth + 1);
    }
  }
}

static std::string Payload(yyjson_val* value) {
  if (value == nullptr) return "null";
  ValidateDataDepth(value, 0);
  const auto json = WriteJson(value);
  Require(json.size() <= 16384, "Notification data exceeds 16384 UTF-8 bytes");
  return json;
}

static MuonNotificationPost ParsePost(yyjson_val* value,
                                      const std::string& default_category) {
  Fields(value, {"id", "title", "body", "category", "data", "actions"});
  auto post = MuonNotificationPost{};
  post.id = String(yyjson_obj_get(value, "id"));
  post.title = String(yyjson_obj_get(value, "title"));
  post.body = String(yyjson_obj_get(value, "body"));
  ValidateMuonNotificationIdentifier(post.id);
  ValidateText(post.title, 1024, true);
  ValidateText(post.body, 4096, false);
  const auto category = yyjson_obj_get(value, "category");
  post.category = category == nullptr ? default_category : String(category);
  if (category != nullptr || !post.category.empty())
    ValidateMuonNotificationIdentifier(post.category);
  post.data = Payload(yyjson_obj_get(value, "data"));
  if (const auto actions = yyjson_obj_get(value, "actions")) {
    Require(yyjson_is_arr(actions) && yyjson_arr_size(actions) <= 3,
            "Notification actions must contain at most three entries");
    auto action_ids = std::set<std::string>{};
    size_t index = 0, count = 0;
    yyjson_val* action = nullptr;
    yyjson_arr_foreach(actions, index, count, action) {
      Fields(action, {"id", "title"});
      auto item = MuonNotificationAction{
          String(yyjson_obj_get(action, "id")),
          String(yyjson_obj_get(action, "title"))};
      ValidateMuonNotificationIdentifier(item.id);
      ValidateText(item.title, 128, true);
      Require(action_ids.insert(item.id).second, "Duplicate notification action ID");
      post.actions.push_back(std::move(item));
    }
  }
  return post;
}

MuonNotificationPost ParseMuonNotificationPost(const std::string& json,
                                               const std::string& default_category) {
  Require(json.size() <= 131072, "Notification request is too large");
  const auto document = ReadJson(json);
  return ParsePost(yyjson_doc_get_root(document.get()), default_category);
}

void PruneMuonNotifications(MuonNotificationSnapshot& state, int64_t now) {
  const auto expired = [now](int64_t time) {
    return time <= now && now - time >= kRetentionMs;
  };
  std::erase_if(state.revisions, [&](const auto& item) { return expired(item.created_at); });
  std::erase_if(state.pending, [&](const auto& item) { return expired(item.received_at); });
  std::erase_if(state.acknowledged,
                [&](const auto& item) { return expired(item.acknowledged_at); });
  if (state.acknowledged.size() > 1024)
    state.acknowledged.erase(state.acknowledged.begin(),
                             state.acknowledged.end() - 1024);
}

void ReserveMuonNotification(MuonNotificationSnapshot& state,
                             MuonNotificationPost post, std::string token,
                             int64_t now) {
  PruneMuonNotifications(state, now);
  const auto existing = std::find_if(state.revisions.begin(), state.revisions.end(),
                                     [&](const auto& item) { return item.post.id == post.id; });
  Require(existing != state.revisions.end() ||
              state.revisions.size() + state.pending.size() < 256,
          "Notification event capacity is full");
  ValidateMuonNotificationIdentifier(token);
  Require(std::none_of(state.revisions.begin(), state.revisions.end(),
                        [&](const auto& item) { return item.token == token; }) &&
              std::none_of(state.pending.begin(), state.pending.end(),
                           [&](const auto& item) { return item.event_id == token; }) &&
              std::none_of(state.acknowledged.begin(), state.acknowledged.end(),
                           [&](const auto& item) { return item.token == token; }),
          "Duplicate notification revision token");
  if (existing != state.revisions.end()) state.revisions.erase(existing);
  state.revisions.push_back({std::move(token), std::move(post), now, {}});
}

bool ActivateMuonNotification(MuonNotificationSnapshot& state,
                              const std::string& token,
                              const std::optional<std::string>& action_id,
                              int64_t now) {
  PruneMuonNotifications(state, now);
  const auto revision = std::find_if(state.revisions.begin(), state.revisions.end(),
                                     [&](const auto& item) { return item.token == token; });
  if (revision == state.revisions.end()) return false;
  if (action_id && std::none_of(revision->post.actions.begin(), revision->post.actions.end(),
                                [&](const auto& item) { return item.id == *action_id; }))
    return false;
  Require(state.cursor < kMaximumCursor, "Notification event cursor is exhausted");
  state.pending.push_back({token, revision->post.id, action_id, revision->post.data,
                            now, ++state.cursor});
  state.revisions.erase(revision);
  return true;
}

void AcknowledgeMuonNotification(MuonNotificationSnapshot& state,
                                 const std::string& token, int64_t now) {
  PruneMuonNotifications(state, now);
  const auto event = std::find_if(state.pending.begin(), state.pending.end(),
                                  [&](const auto& item) { return item.event_id == token; });
  if (event == state.pending.end()) return;
  state.pending.erase(event);
  state.acknowledged.push_back({token, now});
  PruneMuonNotifications(state, now);
}

static std::string EventJson(const MuonNotificationEvent& event, bool persisted) {
  auto json = "{\"eventId\":" + Quote(event.event_id) + ",\"id\":" + Quote(event.id) +
              ",\"type\":" + Quote(event.action_id ? "action" : "activate") +
              ",\"actionId\":" + (event.action_id ? Quote(*event.action_id) : "null") +
              ",\"data\":" + event.data;
  if (persisted)
    json += ",\"receivedAt\":" + std::to_string(event.received_at) +
            ",\"cursor\":" + std::to_string(event.cursor);
  return json + "}";
}

std::string SerializeMuonNotificationEvents(const MuonNotificationSnapshot& state,
                                             uint64_t after_cursor) {
  auto json = std::string("[");
  for (const auto& event : state.pending) {
    if (event.cursor <= after_cursor) continue;
    if (json.size() > 1) json += ',';
    json += EventJson(event, false);
  }
  return json + "]";
}

static std::string PostJson(const MuonNotificationPost& post) {
  auto json = "{\"id\":" + Quote(post.id) + ",\"title\":" + Quote(post.title) +
              ",\"body\":" + Quote(post.body) + ",\"data\":" + post.data;
  if (!post.category.empty()) json += ",\"category\":" + Quote(post.category);
  json += ",\"actions\":[";
  auto first = true;
  for (const auto& action : post.actions) {
    if (!first) json += ',';
    first = false;
    json += "{\"id\":" + Quote(action.id) + ",\"title\":" + Quote(action.title) + "}";
  }
  return json + "]}";
}

std::string SerializeMuonNotificationSnapshot(const MuonNotificationSnapshot& state) {
  auto json = "{\"version\":1,\"cursor\":" + std::to_string(state.cursor) + ",\"revisions\":[";
  auto first = true;
  for (const auto& item : state.revisions) {
    if (!first) json += ',';
    first = false;
    json += "{\"token\":" + Quote(item.token) + ",\"post\":" + PostJson(item.post) +
            ",\"createdAt\":" + std::to_string(item.created_at) +
            ",\"handle\":{\"backend\":" + Quote(item.handle.backend) +
            ",\"owner\":" + Quote(item.handle.owner) +
            ",\"id\":" + std::to_string(item.handle.id) + "}}";
  }
  json += "],\"pending\":[";
  first = true;
  for (const auto& item : state.pending) {
    if (!first) json += ',';
    first = false;
    json += EventJson(item, true);
  }
  json += "],\"acknowledged\":[";
  first = true;
  for (const auto& item : state.acknowledged) {
    if (!first) json += ',';
    first = false;
    json += "{\"token\":" + Quote(item.token) +
            ",\"acknowledgedAt\":" + std::to_string(item.acknowledged_at) + "}";
  }
  return json + "]}";
}

static uint64_t Unsigned(yyjson_val* value, uint64_t maximum) {
  Require(yyjson_is_uint(value) && yyjson_get_uint(value) <= maximum,
          "Invalid notification snapshot number");
  return yyjson_get_uint(value);
}

static yyjson_val* Array(yyjson_val* value, const char* name, size_t limit) {
  const auto array = yyjson_obj_get(value, name);
  Require(yyjson_is_arr(array) && yyjson_arr_size(array) <= limit,
          "Invalid notification snapshot array");
  return array;
}

MuonNotificationSnapshot ParseMuonNotificationSnapshot(const std::string& json) {
  Require(json.size() <= 32 * 1024 * 1024, "Notification snapshot is too large");
  const auto document = ReadJson(json);
  auto* root = yyjson_doc_get_root(document.get());
  Fields(root, {"version", "cursor", "revisions", "pending", "acknowledged"});
  Require(Unsigned(yyjson_obj_get(root, "version"), 1) == 1,
          "Unsupported notification snapshot version");
  auto state = MuonNotificationSnapshot{};
  state.cursor = Unsigned(yyjson_obj_get(root, "cursor"), kMaximumCursor);
  auto tokens = std::set<std::string>{};
  auto ids = std::set<std::string>{};
  auto read_token = [&](yyjson_val* value) {
    auto token = String(value);
    ValidateMuonNotificationIdentifier(token);
    Require(tokens.insert(token).second, "Duplicate notification snapshot token");
    return token;
  };
  size_t index = 0, count = 0;
  yyjson_val* item = nullptr;
  yyjson_arr_foreach(Array(root, "revisions", 256), index, count, item) {
    Fields(item, {"token", "post", "createdAt", "handle"});
    auto revision = MuonNotificationRevision{};
    revision.token = read_token(yyjson_obj_get(item, "token"));
    revision.post = ParsePost(yyjson_obj_get(item, "post"), "");
    Require(ids.insert(revision.post.id).second, "Duplicate active notification ID");
    revision.created_at = static_cast<int64_t>(Unsigned(
        yyjson_obj_get(item, "createdAt"), std::numeric_limits<int64_t>::max()));
    const auto handle = yyjson_obj_get(item, "handle");
    Fields(handle, {"backend", "owner", "id"});
    revision.handle.backend = String(yyjson_obj_get(handle, "backend"));
    revision.handle.owner = String(yyjson_obj_get(handle, "owner"));
    revision.handle.id = static_cast<uint32_t>(Unsigned(
        yyjson_obj_get(handle, "id"), std::numeric_limits<uint32_t>::max()));
    Require(revision.handle.id == 0 || (!revision.handle.backend.empty() &&
                                       !revision.handle.owner.empty()),
            "Invalid notification backend handle");
    state.revisions.push_back(std::move(revision));
  }
  auto last_cursor = uint64_t{0};
  yyjson_arr_foreach(Array(root, "pending", 256), index, count, item) {
    Fields(item, {"eventId", "id", "type", "actionId", "data", "receivedAt", "cursor"});
    auto event = MuonNotificationEvent{};
    event.event_id = read_token(yyjson_obj_get(item, "eventId"));
    event.id = String(yyjson_obj_get(item, "id"));
    ValidateMuonNotificationIdentifier(event.id);
    const auto action = yyjson_obj_get(item, "actionId");
    if (!yyjson_is_null(action)) {
      event.action_id = String(action);
      ValidateMuonNotificationIdentifier(*event.action_id);
    }
    Require(String(yyjson_obj_get(item, "type")) ==
                (event.action_id ? "action" : "activate"),
            "Invalid notification event type");
    event.data = Payload(yyjson_obj_get(item, "data"));
    event.received_at = static_cast<int64_t>(Unsigned(
        yyjson_obj_get(item, "receivedAt"), std::numeric_limits<int64_t>::max()));
    event.cursor = Unsigned(yyjson_obj_get(item, "cursor"), state.cursor);
    Require(event.cursor > last_cursor, "Invalid notification event order");
    last_cursor = event.cursor;
    state.pending.push_back(std::move(event));
  }
  Require(state.revisions.size() + state.pending.size() <= 256,
          "Notification snapshot exceeds event capacity");
  yyjson_arr_foreach(Array(root, "acknowledged", 1024), index, count, item) {
    Fields(item, {"token", "acknowledgedAt"});
    state.acknowledged.push_back({read_token(yyjson_obj_get(item, "token")),
        static_cast<int64_t>(Unsigned(yyjson_obj_get(item, "acknowledgedAt"),
                                      std::numeric_limits<int64_t>::max()))});
  }
  return state;
}

}  // namespace muon_internal
