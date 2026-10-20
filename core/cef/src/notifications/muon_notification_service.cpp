/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "notifications/muon_notification_service.h"
#include "notifications/muon_notification_store.h"
#include "muon_cardio_post.h"
#include "yyjson.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>

namespace muon_internal {

struct NotificationLease {
  std::string owner;
  uint64_t after_cursor = 0;
  std::shared_ptr<cardio::promise_source<std::string>> pending;
};

struct MuonNotificationService::Impl {
  std::string path;
  std::string default_category;
  MuonNotificationBackend backend;
  std::function<int64_t()> clock;
  std::function<void(std::string)> on_activation;
  std::optional<std::string> pending_startup_id;
  cardio::dispatcher* dispatcher;
  cardio::primitives::mutex serial;
  MuonNotificationSnapshot state;
  bool loaded = false;
  bool stopping = false;
  uint64_t next_lease = 0;
  std::map<std::string, NotificationLease> leases;
  std::map<std::string, size_t> acquiring;
  std::set<std::string> released_owners;
};

using NotificationImpl = MuonNotificationService::Impl;

static void RequireRunning(const NotificationImpl& impl) {
  if (impl.stopping) throw std::runtime_error("Notification service is shutting down");
}

static cardio::promise<void> Load(NotificationImpl& impl) {
  if (!impl.loaded) {
    impl.state = co_await LoadMuonNotificationSnapshot(impl.path);
    impl.loaded = true;
  }
  PruneMuonNotifications(impl.state, impl.clock());
}

static cardio::promise<void> Commit(NotificationImpl& impl,
                                     MuonNotificationSnapshot state) {
  co_await SaveMuonNotificationSnapshot(impl.path, state);
  impl.state = std::move(state);
}

static std::string Batch(const NotificationImpl& impl, uint64_t cursor,
                          bool terminal) {
  return "{\"events\":" +
         (terminal ? std::string("null") : SerializeMuonNotificationEvents(impl.state, cursor)) +
         ",\"cursor\":" + std::to_string(impl.state.cursor) + "}";
}

static void Notify(NotificationImpl& impl) {
  for (auto& [token, lease] : impl.leases) {
    if (lease.pending && impl.state.cursor > lease.after_cursor) {
      auto pending = std::move(lease.pending);
      pending->resolve(Batch(impl, lease.after_cursor, false));
    }
  }
}

static void CloseLease(NotificationImpl& impl, NotificationLease& lease) {
  if (lease.pending) {
    auto pending = std::move(lease.pending);
    pending->resolve(Batch(impl, lease.after_cursor, true));
  }
}

static void Diagnose(const char* operation, const std::exception& error) {
  std::fprintf(stderr, "muon notifications %s failed: %s\n", operation, error.what());
}

static void BeginStop(NotificationImpl& impl) {
  if (impl.stopping) return;
  impl.stopping = true;
  impl.on_activation = {};
  impl.pending_startup_id.reset();
  for (auto& [token, lease] : impl.leases) CloseLease(impl, lease);
  impl.leases.clear();
  impl.backend.stop();
}

static void Present(NotificationImpl& impl, std::string startup_id) {
  if (impl.stopping) return;
  if (!impl.on_activation) {
    // Several early interactions need one window; retain the latest user time.
    impl.pending_startup_id = std::move(startup_id);
    return;
  }
  // A window failure cannot undo delivery or prevent OS cancellation.
  try { impl.on_activation(std::move(startup_id)); }
  catch (const std::exception& error) { Diagnose("activation window", error); }
}

static cardio::promise<void> Activate(std::shared_ptr<NotificationImpl> impl,
                                       std::string token,
                                       std::optional<std::string> action,
                                       std::string startup_id,
                                       cardio::promise<cardio::primitives::lock_handle> ticket) {
  try {
    auto lock = std::move(co_await ticket);
    co_await Load(*impl);
    const auto found = std::find_if(impl->state.revisions.begin(), impl->state.revisions.end(),
                                    [&](const auto& item) { return item.token == token; });
    if (found == impl->state.revisions.end()) co_return;
    const auto handle = found->handle;
    auto candidate = impl->state;
    if (!ActivateMuonNotification(candidate, token, action, impl->clock())) co_return;
    co_await Commit(*impl, std::move(candidate));
    // A failed OS cancellation must not suppress an already durable event.
    Notify(*impl);
    // Present the application only for a newly accepted durable interaction.
    Present(*impl, std::move(startup_id));
    if (handle.id != 0) co_await impl->backend.cancel(handle);
  } catch (const std::exception& error) {
    Diagnose("activation", error);
  }
}

MuonNotificationService::MuonNotificationService(
    std::string path, std::string default_category, MuonNotificationBackend backend,
    std::function<int64_t()> clock, std::function<void(std::string)> on_activation)
    : impl_(std::make_shared<Impl>()) {
  impl_->path = std::move(path);
  impl_->default_category = std::move(default_category);
  impl_->backend = std::move(backend);
  impl_->clock = std::move(clock);
  impl_->on_activation = std::move(on_activation);
  impl_->dispatcher = &cardio::get_current_dispatcher();
}

MuonNotificationService::~MuonNotificationService() {
  // Startup failure can bypass the plugin's asynchronous Stop hook. Release
  // apartment-bound backend resources here on the owning dispatcher, even if
  // an OS callback still retains the backend state on another thread.
  try { BeginStop(*impl_); }
  catch (const std::exception& error) { Diagnose("destruction", error); }
}

void MuonNotificationService::SetActivationHandler(std::function<void(std::string)> callback) {
  RequireRunning(*impl_);
  impl_->on_activation = std::move(callback);
  if (impl_->on_activation && impl_->pending_startup_id) {
    auto startup_id = std::move(*impl_->pending_startup_id);
    impl_->pending_startup_id.reset();
    Present(*impl_, std::move(startup_id));
  }
}

void MuonNotificationService::Start() {
  auto weak = std::weak_ptr<Impl>(impl_);
  if (impl_->backend.set_application_activation) {
    impl_->backend.set_application_activation([weak](std::string startup_id) {
      if (auto impl = weak.lock(); impl && !impl->stopping)
        Present(*impl, std::move(startup_id));
    });
  }
  impl_->backend.set_activation([weak](std::string token, std::optional<std::string> action, std::string startup_id) {
    if (auto impl = weak.lock(); impl && !impl->stopping) {
      // Reserve queue order in the signal callback. Stop must wait for this
      // accepted interaction even if its dispatcher task has not run yet.
      auto ticket = impl->serial.lock();
      FireAndForgetOnDispatcher(impl->dispatcher,
          [impl, token = std::move(token), action = std::move(action),
           startup_id = std::move(startup_id), ticket = std::move(ticket)]() mutable {
            cardio::fire_and_forget(Activate(impl, std::move(token), std::move(action),
                                            std::move(startup_id), std::move(ticket)));
          });
    }
  });
}

cardio::promise<void> MuonNotificationService::Post(std::string json) {
  auto impl = impl_;
  RequireRunning(*impl);
  auto post = ParseMuonNotificationPost(json, impl->default_category);
  auto lock = std::move(co_await impl->serial.lock());
  co_await Load(*impl);
  const auto capabilities = co_await impl->backend.capabilities();
  if (!capabilities.available)
    throw std::runtime_error("Notification service is unavailable: " + capabilities.reason);
  if (!post.actions.empty() && !capabilities.actions)
    throw std::runtime_error("Notification actions are unsupported by the service");
  const auto status = co_await impl->backend.status();
  if (status.enabled == false)
    throw std::runtime_error("Notifications are disabled: " + status.reason);
  const auto token = co_await CreateMuonNotificationToken();
  const auto before = impl->state;
  auto candidate = before;
  ReserveMuonNotification(candidate, post, token, impl->clock());
  co_await Commit(*impl, std::move(candidate));
  auto handle = MuonNotificationHandle{};
  auto failure = std::exception_ptr{};
  try { handle = co_await impl->backend.post(post, token); }
  catch (...) { failure = std::current_exception(); }
  if (failure) {
    // Restore the previous revision only after its rollback snapshot is saved.
    co_await Commit(*impl, before);
    std::rethrow_exception(failure);
  }
  candidate = impl->state;
  candidate.revisions.back().handle = handle;
  co_await Commit(*impl, std::move(candidate));
  for (const auto& old : before.revisions) {
    if (old.post.id == post.id && old.handle.id != 0)
      co_await impl->backend.cancel(old.handle);
  }
}

cardio::promise<void> MuonNotificationService::Cancel(std::string id) {
  auto impl = impl_;
  RequireRunning(*impl);
  ValidateMuonNotificationIdentifier(id);
  auto lock = std::move(co_await impl->serial.lock());
  co_await Load(*impl);
  const auto before = impl->state;
  auto candidate = before;
  std::erase_if(candidate.revisions, [&](const auto& item) { return item.post.id == id; });
  if (candidate.revisions.size() == before.revisions.size()) co_return;
  for (const auto& old : before.revisions) {
    if (old.post.id == id && old.handle.id != 0)
      co_await impl->backend.cancel(old.handle);
  }
  // Keep the handle available for a retry if the OS rejects cancellation.
  // Activations share this serial queue and see invalidation after success.
  co_await Commit(*impl, std::move(candidate));
}

static std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)> ReadOptions(
    const std::string& json, std::initializer_list<std::string_view> allowed) {
  auto document = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>(
      yyjson_read(json.data(), json.size(), 0), &yyjson_doc_free);
  if (!document || !yyjson_is_obj(yyjson_doc_get_root(document.get())))
    throw std::runtime_error("Notification options must be an object");
  size_t index = 0, count = 0;
  yyjson_val* key = nullptr;
  yyjson_val* value = nullptr;
  auto seen = std::set<std::string>{};
  yyjson_obj_foreach(yyjson_doc_get_root(document.get()), index, count, key, value) {
    const auto name = std::string(yyjson_get_str(key), yyjson_get_len(key));
    if (std::find(allowed.begin(), allowed.end(), name) == allowed.end() ||
        !seen.insert(name).second)
      throw std::runtime_error("Unknown notification option");
  }
  return document;
}

static std::string RequiredString(yyjson_val* value) {
  if (!yyjson_is_str(value)) throw std::runtime_error("Notification option must be a string");
  return std::string(yyjson_get_str(value), yyjson_get_len(value));
}

cardio::promise<std::string> MuonNotificationService::GetStatus(std::string json) {
  auto impl = impl_;
  RequireRunning(*impl);
  const auto document = ReadOptions(json, {"category"});
  const auto category = yyjson_obj_get(yyjson_doc_get_root(document.get()), "category");
  auto category_json = std::string("null");
  if (category != nullptr) {
    const auto id = RequiredString(category);
    ValidateMuonNotificationIdentifier(id);
    category_json = "{\"id\":" + QuoteMuonNotificationString(id) +
                    ",\"state\":\"unknown\",\"reason\":\"category-status-unavailable\"}";
  }
  auto lock = std::move(co_await impl->serial.lock());
  const auto status = co_await impl->backend.status();
  const auto state = !status.enabled.has_value() ? "unknown" : *status.enabled ? "enabled" : "disabled";
  const auto reason = status.reason.empty() ? "null" : QuoteMuonNotificationString(status.reason);
  co_return "{\"app\":{\"state\":" + QuoteMuonNotificationString(state) +
            ",\"reason\":" + reason + "},\"category\":" + category_json + "}";
}

static std::string Capability(std::optional<bool> supported, const std::string& reason) {
  if (supported == true) return "{\"state\":\"supported\",\"reason\":null}";
  return "{\"state\":" + QuoteMuonNotificationString(
      supported.has_value() ? "unsupported" : "unknown") +
      ",\"reason\":" + QuoteMuonNotificationString(reason) + "}";
}

cardio::promise<std::string> MuonNotificationService::GetCapabilities() {
  auto impl = impl_;
  RequireRunning(*impl);
  auto lock = std::move(co_await impl->serial.lock());
  const auto caps = co_await impl->backend.capabilities();
  const auto action_reason = caps.available ? "server-actions-unavailable" : caps.reason;
  co_return "{\"interaction\":" + Capability(caps.available && caps.actions, action_reason) +
            ",\"launch\":" + Capability(caps.launch, caps.launch_reason) +
            ",\"persistence\":" + Capability(caps.available ? caps.persistence : false,
                caps.available && caps.persistence.has_value()
                    ? "server-persistence-unavailable" : caps.reason) +
            ",\"actions\":" + Capability(caps.available && caps.actions, action_reason) +
            ",\"appStatus\":" + Capability(caps.app_status, "app-status-unavailable") +
            ",\"categoryStatus\":" + Capability(false, "category-status-unavailable") + "}";
}

cardio::promise<std::string> MuonNotificationService::GetPendingEvents() {
  auto impl = impl_;
  RequireRunning(*impl);
  auto lock = std::move(co_await impl->serial.lock());
  co_await Load(*impl);
  co_return SerializeMuonNotificationEvents(impl->state, 0);
}

cardio::promise<void> MuonNotificationService::AcknowledgeEvent(std::string token) {
  auto impl = impl_;
  RequireRunning(*impl);
  ValidateMuonNotificationIdentifier(token);
  auto lock = std::move(co_await impl->serial.lock());
  co_await Load(*impl);
  auto candidate = impl->state;
  AcknowledgeMuonNotification(candidate, token, impl->clock());
  if (candidate.pending.size() != impl->state.pending.size())
    co_await Commit(*impl, std::move(candidate));
}

struct NotificationAcquireGuard {
  NotificationImpl& impl;
  std::string owner;
  ~NotificationAcquireGuard() {
    const auto found = impl.acquiring.find(owner);
    if (found != impl.acquiring.end() && --found->second == 0) {
      impl.acquiring.erase(found);
      impl.released_owners.erase(owner);
    }
  }
};

cardio::promise<std::string> MuonNotificationService::WatchEvents(
    std::string json, std::string owner) {
  auto impl = impl_;
  RequireRunning(*impl);
  if (owner.empty()) throw std::runtime_error("Notification watch owner is unavailable");
  const auto document = ReadOptions(json, {"operation", "token", "afterCursor"});
  const auto root = yyjson_doc_get_root(document.get());
  const auto operation = RequiredString(yyjson_obj_get(root, "operation"));
  if (operation == "acquire") {
    if (yyjson_obj_size(root) != 1) throw std::runtime_error("Invalid watch acquire options");
    ++impl->acquiring[owner];
    auto guard = NotificationAcquireGuard{*impl, owner};
    auto lock = std::move(co_await impl->serial.lock());
    co_await Load(*impl);
    if (impl->stopping || impl->released_owners.contains(owner))
      throw std::runtime_error("Notification watch owner was released");
    const auto owner_count = std::count_if(impl->leases.begin(), impl->leases.end(),
        [&](const auto& entry) { return entry.second.owner == owner; });
    if (owner_count >= 16 || impl->leases.size() >= 128)
      throw std::runtime_error("Notification watch capacity is full");
    const auto token = "notification-watch:" + std::to_string(++impl->next_lease);
    impl->leases.emplace(token, NotificationLease{owner, impl->state.cursor, nullptr});
    co_return "{\"token\":" + QuoteMuonNotificationString(token) +
              ",\"cursor\":" + std::to_string(impl->state.cursor) + "}";
  }
  const auto token = RequiredString(yyjson_obj_get(root, "token"));
  const auto found = impl->leases.find(token);
  if (found != impl->leases.end() && found->second.owner != owner)
    throw std::runtime_error("Notification watch belongs to another renderer");
  if (operation == "release") {
    if (yyjson_obj_size(root) != 2) throw std::runtime_error("Invalid watch release options");
    if (found != impl->leases.end()) {
      CloseLease(*impl, found->second);
      impl->leases.erase(found);
    }
    co_return "{}";
  }
  if (operation != "next" || yyjson_obj_size(root) != 3)
    throw std::runtime_error("Invalid notification watch operation");
  const auto cursor_value = yyjson_obj_get(root, "afterCursor");
  if (!yyjson_is_uint(cursor_value) || yyjson_get_uint(cursor_value) > impl->state.cursor)
    throw std::runtime_error("Invalid notification watch cursor");
  if (found == impl->leases.end()) co_return Batch(*impl, 0, true);
  auto& lease = found->second;
  if (lease.pending) throw std::runtime_error("Notification watch already has a pending wait");
  const auto cursor = yyjson_get_uint(cursor_value);
  PruneMuonNotifications(impl->state, impl->clock());
  if (impl->state.cursor > cursor) co_return Batch(*impl, cursor, false);
  lease.after_cursor = cursor;
  lease.pending = std::make_shared<cardio::promise_source<std::string>>();
  auto pending = lease.pending->get_promise();
  co_return co_await pending;
}

void MuonNotificationService::ReleaseOwner(const std::string& owner) {
  if (impl_->acquiring.contains(owner)) impl_->released_owners.insert(owner);
  for (auto iterator = impl_->leases.begin(); iterator != impl_->leases.end();) {
    if (iterator->second.owner == owner) {
      CloseLease(*impl_, iterator->second);
      iterator = impl_->leases.erase(iterator);
    } else ++iterator;
  }
}

cardio::promise<void> MuonNotificationService::Stop() {
  auto impl = impl_;
  BeginStop(*impl);
  auto lock = std::move(co_await impl->serial.lock());
}

}  // namespace muon_internal
