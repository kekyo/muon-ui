/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 */
#include "muon_service_session_monitor.h"
#include <gio/gio.h>
#include <algorithm>
#include <memory>
#include <string>

static constexpr char portal[] = "org.freedesktop.portal.Desktop";
static constexpr char desktop[] = "/org/freedesktop/portal/desktop";
static constexpr char inhibit[] = "org.freedesktop.portal.Inhibit";
static constexpr char request_interface[] = "org.freedesktop.portal.Request";

struct SessionMonitor {
  std::function<void()> ending;
  GDBusConnection* bus = nullptr;
  std::string owner, request, session;
  guint watch = 0, response = 0, changed = 0;
};

static void Disconnect(SessionMonitor& monitor) {
  if (!monitor.bus) return;
  g_dbus_connection_signal_unsubscribe(monitor.bus, monitor.changed);
  g_dbus_connection_signal_unsubscribe(monitor.bus, monitor.response);
  if (!monitor.session.empty() || !monitor.request.empty()) {
    const bool pending = monitor.session.empty();
    g_dbus_connection_call(monitor.bus, monitor.owner.c_str(),
        pending ? monitor.request.c_str() : monitor.session.c_str(),
        pending ? request_interface : "org.freedesktop.portal.Session", "Close",
        nullptr, nullptr, G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, nullptr, nullptr, nullptr);
  }
  monitor.request.clear();
  monitor.session.clear();
  g_clear_object(&monitor.bus);
}

static void Response(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*,
    GVariant* parameters, gpointer data) {
  auto& monitor = *static_cast<SessionMonitor*>(data);
  if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(ua{sv})"))) return;
  guint32 status = 1;
  GVariant* result = nullptr;
  g_variant_get(parameters, "(u@a{sv})", &status, &result);
  const gchar* session = nullptr;
  if (!status && g_variant_lookup(result, "session_handle", "&s", &session) &&
      g_variant_is_object_path(session)) monitor.session = session;
  g_variant_unref(result);
  monitor.request.clear();
}

static void StateChanged(GDBusConnection* bus, const gchar* sender, const gchar*, const gchar*,
    const gchar*, GVariant* parameters, gpointer data) {
  auto& monitor = *static_cast<SessionMonitor*>(data);
  if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(oa{sv})"))) return;
  const gchar* session = nullptr;
  GVariant* values = nullptr;
  g_variant_get(parameters, "(&o@a{sv})", &session, &values);
  guint32 state = 0;
  g_variant_lookup(values, "session-state", "u", &state);
  g_variant_unref(values);
  if (monitor.session != session) return;
  if (state == 2)
    g_dbus_connection_call(bus, sender, desktop, inhibit, "QueryEndResponse", g_variant_new("(o)", session),
        nullptr, G_DBUS_CALL_FLAGS_NO_AUTO_START, 1000, nullptr, nullptr, nullptr);
  else if (state == 3 && monitor.ending) monitor.ending();
}

static void Appeared(GDBusConnection* bus, const gchar*, const gchar* owner, gpointer data) {
  auto& monitor = *static_cast<SessionMonitor*>(data);
  Disconnect(monitor);
  monitor.bus = G_DBUS_CONNECTION(g_object_ref(bus));
  monitor.owner = owner;
  auto sender = std::string(g_dbus_connection_get_unique_name(bus)).substr(1);
  std::replace(sender.begin(), sender.end(), '.', '_');
  // Subscribe before calling CreateMonitor: Response may precede its method reply.
  const auto token = "muon_" + std::to_string(g_random_int());
  monitor.request = std::string(desktop) + "/request/" + sender + "/" + token;
  monitor.response = g_dbus_connection_signal_subscribe(bus, owner, request_interface, "Response",
      monitor.request.c_str(), nullptr, G_DBUS_SIGNAL_FLAGS_NONE, Response, data, nullptr);
  monitor.changed = g_dbus_connection_signal_subscribe(bus, owner, inhibit, "StateChanged",
      desktop, nullptr, G_DBUS_SIGNAL_FLAGS_NONE, StateChanged, data, nullptr);
  GVariantBuilder options;
  g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(token.c_str()));
  g_variant_builder_add(&options, "{sv}", "session_handle_token", g_variant_new_string(token.c_str()));
  g_dbus_connection_call(bus, owner, desktop, inhibit, "CreateMonitor",
      g_variant_new("(sa{sv})", "", &options), G_VARIANT_TYPE("(o)"),
      G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, nullptr, nullptr, nullptr);
}

std::function<void()> WatchMuonServiceSession(std::function<void()> ending) {
  const auto monitor = std::make_shared<SessionMonitor>();
  monitor->ending = std::move(ending);
  monitor->watch = g_bus_watch_name(G_BUS_TYPE_SESSION, portal, G_BUS_NAME_WATCHER_FLAGS_AUTO_START,
      Appeared, [](GDBusConnection*, const gchar*, gpointer data) {
        Disconnect(*static_cast<SessionMonitor*>(data));
      }, monitor.get(), nullptr);
  return [monitor] {
    if (monitor->watch) g_bus_unwatch_name(monitor->watch);
    monitor->watch = 0;
    Disconnect(*monitor);
  };
}
