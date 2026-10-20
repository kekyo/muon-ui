/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "notifications/muon_notification_linux.h"
#include <gio/gio.h>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <utility>

namespace muon_internal {
static constexpr char kNotificationName[] = "org.freedesktop.Notifications";
static constexpr char kNotificationPath[] = "/org/freedesktop/Notifications";
using Variant = std::unique_ptr<GVariant, decltype(&g_variant_unref)>;

struct LinuxNotificationRecord {
  std::string token;
  std::map<std::string, std::optional<std::string>> actions;
};
struct LinuxNotificationBackend {
  std::string application_name;
  std::string desktop_id;
  GDBusConnection* connection = nullptr;
  guint signal_subscription = 0;
  guint owner_subscription = 0;
  std::string owner;
  std::map<uint32_t, LinuxNotificationRecord> records;
  MuonNotificationActivation activation;
  bool stopped = false;
  ~LinuxNotificationBackend() {
    if (connection != nullptr) {
      if (signal_subscription != 0)
        g_dbus_connection_signal_unsubscribe(connection, signal_subscription);
      if (owner_subscription != 0)
        g_dbus_connection_signal_unsubscribe(connection, owner_subscription);
      g_object_unref(connection);
    }
  }
};

static cardio::promise<GVariant*> Call(GDBusConnection* connection,
    const std::string& destination, const char* path, const char* interface_name,
    const char* method, GVariant* parameters, const GVariantType* result_type) {
  co_return co_await cardio::gio::submit<GVariant*>(
      [=](GCancellable* cancel, GAsyncReadyCallback callback, gpointer data) {
        g_dbus_connection_call(connection, destination.c_str(), path, interface_name,
            method, parameters, result_type, G_DBUS_CALL_FLAGS_NONE, -1,
            cancel, callback, data);
      }, [](GObject* source, GAsyncResult* result, GError** error) {
        return g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, error);
      });
}

static std::string Epoch(const LinuxNotificationBackend& impl) {
  const auto* guid = g_dbus_connection_get_guid(impl.connection);
  return std::string(guid == nullptr ? "" : guid) + "/" + impl.owner;
}
static void SetOwner(LinuxNotificationBackend& impl, std::string owner) {
  if (impl.owner != owner) {
    impl.records.clear();
    impl.owner = std::move(owner);
  }
}
static void OnOwnerChanged(GDBusConnection*, const gchar*, const gchar*,
    const gchar*, const gchar*, GVariant* parameters, gpointer data) {
  const auto impl = static_cast<std::weak_ptr<LinuxNotificationBackend>*>(data)->lock();
  if (!impl || impl->stopped || !g_variant_is_of_type(parameters, G_VARIANT_TYPE("(sss)"))) return;
  const gchar* name = nullptr;
  const gchar* previous = nullptr;
  const gchar* current = nullptr;
  g_variant_get(parameters, "(&s&s&s)", &name, &previous, &current);
  SetOwner(*impl, current);
}
static void OnNotificationSignal(GDBusConnection*, const gchar* sender, const gchar*,
    const gchar*, const gchar* member, GVariant* parameters, gpointer data) {
  const auto impl = static_cast<std::weak_ptr<LinuxNotificationBackend>*>(data)->lock();
  if (!impl || impl->stopped || sender == nullptr || impl->owner != sender) return;
  if (std::string_view(member) == "NotificationClosed" &&
      g_variant_is_of_type(parameters, G_VARIANT_TYPE("(uu)"))) {
    guint32 id = 0, reason = 0;
    g_variant_get(parameters, "(uu)", &id, &reason);
    impl->records.erase(id);
  } else if (std::string_view(member) == "ActionInvoked" &&
             g_variant_is_of_type(parameters, G_VARIANT_TYPE("(us)"))) {
    guint32 id = 0;
    const gchar* action = nullptr;
    g_variant_get(parameters, "(u&s)", &id, &action);
    const auto found = impl->records.find(id);
    if (found == impl->records.end()) return;
    const auto selected = found->second.actions.find(action);
    if (selected == found->second.actions.end() || !impl->activation) return;
    // Keep the mapping until durable acceptance/cancellation. If saving fails,
    // another user interaction may retry instead of losing the notification.
    impl->activation(found->second.token, selected->second);
  }
}
static void DeleteWeak(gpointer data) {
  delete static_cast<std::weak_ptr<LinuxNotificationBackend>*>(data);
}
static cardio::promise<void> Connect(std::shared_ptr<LinuxNotificationBackend> impl) {
  if (impl->connection && !g_dbus_connection_is_closed(impl->connection)) co_return;
  if (impl->connection) {
    g_dbus_connection_signal_unsubscribe(impl->connection, impl->signal_subscription);
    g_dbus_connection_signal_unsubscribe(impl->connection, impl->owner_subscription);
    g_object_unref(impl->connection);
    impl->connection = nullptr;
    impl->signal_subscription = impl->owner_subscription = 0;
    SetOwner(*impl, "");
  }
  impl->connection = co_await cardio::gio::submit<GDBusConnection*>(
      [](GCancellable* cancel, GAsyncReadyCallback callback, gpointer data) {
        g_bus_get(G_BUS_TYPE_SESSION, cancel, callback, data);
      }, [](GObject*, GAsyncResult* result, GError** error) { return g_bus_get_finish(result, error); });
  if (impl->stopped) co_return;
  impl->signal_subscription = g_dbus_connection_signal_subscribe(impl->connection,
      kNotificationName, kNotificationName, nullptr, kNotificationPath, nullptr,
      G_DBUS_SIGNAL_FLAGS_NONE, &OnNotificationSignal,
      new std::weak_ptr<LinuxNotificationBackend>(impl), &DeleteWeak);
  impl->owner_subscription = g_dbus_connection_signal_subscribe(impl->connection,
      "org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged",
      "/org/freedesktop/DBus", kNotificationName, G_DBUS_SIGNAL_FLAGS_NONE,
      &OnOwnerChanged, new std::weak_ptr<LinuxNotificationBackend>(impl), &DeleteWeak);
}
static cardio::promise<std::string> ResolveOwner(std::shared_ptr<LinuxNotificationBackend> impl) {
  co_await Connect(impl);
  auto reply = Variant(co_await Call(impl->connection, "org.freedesktop.DBus",
      "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetNameOwner",
      g_variant_new("(s)", kNotificationName), G_VARIANT_TYPE("(s)")), g_variant_unref);
  const gchar* owner = nullptr;
  g_variant_get(reply.get(), "(&s)", &owner);
  SetOwner(*impl, owner);
  co_return impl->owner;
}
static cardio::promise<std::set<std::string>> ReadCapabilities(
    std::shared_ptr<LinuxNotificationBackend> impl, const std::string& owner) {
  auto reply = Variant(co_await Call(impl->connection, owner, kNotificationPath,
      kNotificationName, "GetCapabilities", nullptr, G_VARIANT_TYPE("(as)")), g_variant_unref);
  auto capabilities = std::set<std::string>{};
  auto values = Variant(g_variant_get_child_value(reply.get(), 0), g_variant_unref);
  for (gsize index = 0; index < g_variant_n_children(values.get()); ++index) {
    auto value = Variant(g_variant_get_child_value(values.get(), index), g_variant_unref);
    capabilities.emplace(g_variant_get_string(value.get(), nullptr));
  }
  co_return capabilities;
}
static cardio::promise<MuonNotificationBackendCapabilities> Capabilities(
    std::shared_ptr<LinuxNotificationBackend> impl) {
  try {
    const auto owner = co_await ResolveOwner(impl);
    const auto values = co_await ReadCapabilities(impl, owner);
    // FDO's persistence advertisement does not establish sender-independent
    // lifetime: GNOME 46 destroys an application's source when its sender exits.
    // Keep post-exit retention distinct from support for resident notifications.
    const auto persistence = values.contains("persistence")
        ? std::optional<bool>{} : std::optional<bool>{false};
    co_return MuonNotificationBackendCapabilities{true, values.contains("actions"),
        persistence, persistence.has_value() ? "" : "post-exit-persistence-unverified"};
  } catch (const std::exception& error) {
    co_return MuonNotificationBackendCapabilities{false, false, false,
        std::string("notification-service-unavailable: ") + error.what()};
  }
}
static cardio::promise<MuonNotificationHandle> Post(
    std::shared_ptr<LinuxNotificationBackend> impl, MuonNotificationPost post,
    std::string token) {
  const auto owner = co_await ResolveOwner(impl);
  const auto epoch = Epoch(*impl);
  const auto capabilities = co_await ReadCapabilities(impl, owner);
  if (!post.actions.empty() && !capabilities.contains("actions"))
    throw std::runtime_error("Notification actions are unsupported by the service");
  auto record = LinuxNotificationRecord{std::move(token), {}};
  auto action_values = std::vector<std::string>{};
  if (capabilities.contains("actions")) {
    action_values = {"default", "Open"};
    record.actions.emplace("default", std::nullopt);
    for (size_t index = 0; index < post.actions.size(); ++index) {
      const auto key = "muon:" + std::to_string(index);
      action_values.push_back(key);
      action_values.push_back(post.actions[index].title);
      record.actions.emplace(key, post.actions[index].id);
    }
  }
  auto actions = std::vector<const gchar*>{};
  for (const auto& value : action_values) actions.push_back(value.c_str());
  auto hints = GVariantBuilder{};
  g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&hints, "{sv}", "desktop-entry", g_variant_new_string(impl->desktop_id.c_str()));
  // Capable services retain the notification until durable event acceptance.
  g_variant_builder_add(&hints, "{sv}", "resident", g_variant_new_boolean(TRUE));
  auto body = post.body;
  if (capabilities.contains("body-markup")) {
    auto* escaped = g_markup_escape_text(body.data(), static_cast<gssize>(body.size()));
    body = escaped;
    g_free(escaped);
  }
  auto reply = Variant(co_await Call(impl->connection, owner, kNotificationPath,
      kNotificationName, "Notify", g_variant_new("(susss@as@a{sv}i)",
          impl->application_name.c_str(), guint32{0}, impl->desktop_id.c_str(),
          post.title.c_str(), body.c_str(),
          g_variant_new_strv(actions.data(), static_cast<gssize>(actions.size())),
          g_variant_builder_end(&hints), gint32{-1}), G_VARIANT_TYPE("(u)")), g_variant_unref);
  guint32 id = 0;
  g_variant_get(reply.get(), "(u)", &id);
  if (id == 0) throw std::runtime_error("Notification service returned an invalid ID");
  if (impl->owner != owner) throw std::runtime_error("Notification service changed during submission");
  impl->records[id] = std::move(record);
  co_return MuonNotificationHandle{"fdo", epoch, id};
}
static cardio::promise<void> Cancel(std::shared_ptr<LinuxNotificationBackend> impl,
                                    MuonNotificationHandle handle) {
  if (handle.backend != "fdo" || handle.id == 0) co_return;
  const auto owner = co_await ResolveOwner(impl);
  if (handle.owner != Epoch(*impl)) co_return;
  auto result = Variant(co_await Call(impl->connection, owner, kNotificationPath,
      kNotificationName, "CloseNotification", g_variant_new("(u)", handle.id),
      G_VARIANT_TYPE_UNIT), g_variant_unref);
  impl->records.erase(handle.id);
}
MuonNotificationBackend CreateMuonLinuxNotificationBackend(
    std::string application_name, std::string desktop_id) {
  auto impl = std::make_shared<LinuxNotificationBackend>();
  impl->application_name = std::move(application_name);
  impl->desktop_id = std::move(desktop_id);
  auto backend = MuonNotificationBackend{};
  backend.capabilities = [impl] { return Capabilities(impl); };
  backend.post = [impl](MuonNotificationPost post, std::string token) {
    return Post(impl, std::move(post), std::move(token));
  };
  backend.cancel = [impl](MuonNotificationHandle handle) { return Cancel(impl, std::move(handle)); };
  backend.set_activation = [impl](MuonNotificationActivation callback) { impl->activation = std::move(callback); };
  backend.stop = [impl] {
    impl->stopped = true;
    impl->activation = {};
    if (impl->connection) {
      if (impl->signal_subscription) g_dbus_connection_signal_unsubscribe(impl->connection, impl->signal_subscription);
      if (impl->owner_subscription) g_dbus_connection_signal_unsubscribe(impl->connection, impl->owner_subscription);
      impl->signal_subscription = impl->owner_subscription = 0;
    }
  };
  backend.status = []() -> cardio::promise<MuonNotificationBackendStatus> {
    co_return MuonNotificationBackendStatus{std::nullopt, "app-status-unavailable"};
  };
  return backend;
}
}  // namespace muon_internal
