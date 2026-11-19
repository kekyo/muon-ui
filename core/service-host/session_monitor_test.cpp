/* Exercise the desktop portal protocol in a private session bus. */
#include "muon_service_session_monitor.h"
#include <gio/gio.h>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>

static constexpr char path[] = "/org/freedesktop/portal/desktop";
static constexpr char portal[] = "org.freedesktop.portal.Desktop";
static constexpr char inhibit[] = "org.freedesktop.portal.Inhibit";
static constexpr char xml[] = R"(<node><interface name="org.freedesktop.portal.Inhibit">
<method name="CreateMonitor"><arg type="s" direction="in"/><arg type="a{sv}" direction="in"/><arg type="o" direction="out"/></method>
<method name="QueryEndResponse"><arg type="o" direction="in"/></method>
</interface></node>)";

struct Test {
  GMainLoop* loop;
  GDBusConnection* bus;
  std::string client;
  std::string session;
  int acknowledgements = 0;
  int ended = 0;
};

static void State(const Test& test, const std::string& session, guint32 value) {
  GVariantBuilder fields;
  g_variant_builder_init(&fields, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&fields, "{sv}", "session-state", g_variant_new_uint32(value));
  g_dbus_connection_emit_signal(test.bus, test.client.c_str(), path, inhibit, "StateChanged",
      g_variant_new("(oa{sv})", session.c_str(), &fields), nullptr);
}

static void Method(GDBusConnection*, const gchar* sender, const gchar*, const gchar*,
    const gchar* method, GVariant* parameters, GDBusMethodInvocation* reply, gpointer data) {
  auto& test = *static_cast<Test*>(data);
  if (std::string(method) == "CreateMonitor") {
    auto* options = g_variant_get_child_value(parameters, 1);
    const char *token = nullptr, *session_token = nullptr;
    if (!g_variant_lookup(options, "handle_token", "&s", &token) ||
        !g_variant_lookup(options, "session_handle_token", "&s", &session_token)) std::abort();
    test.client = sender;
    auto name = test.client.substr(1);
    std::replace(name.begin(), name.end(), '.', '_');
    const auto request = std::string(path) + "/request/" + name + "/" + token;
    test.session = std::string(path) + "/session/" + name + "/" + session_token;
    g_variant_unref(options);
    g_dbus_method_invocation_return_value(reply, g_variant_new("(o)", request.c_str()));
    GVariantBuilder results;
    g_variant_builder_init(&results, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&results, "{sv}", "session_handle", g_variant_new_string(test.session.c_str()));
    g_dbus_connection_emit_signal(test.bus, sender, request.c_str(), "org.freedesktop.portal.Request",
        "Response", g_variant_new("(ua{sv})", 0U, &results), nullptr);
    State(test, "/another/session", 3);
    State(test, test.session, 2);
  } else {
    const char* session = nullptr;
    g_variant_get(parameters, "(&o)", &session);
    if (test.session != session || test.ended != 0) std::abort();
    g_dbus_method_invocation_return_value(reply, g_variant_new("()"));
    if (++test.acknowledgements == 1) {
      State(test, test.session, 1); // The session manager cancelled logout.
      State(test, test.session, 2);
    } else State(test, test.session, 3);
  }
}

int main() {
  auto* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
  if (!bus) return 1;
  auto* loop = g_main_loop_new(nullptr, FALSE);
  Test test{loop, bus, {}, {}};
  auto* info = g_dbus_node_info_new_for_xml(xml, nullptr);
  const GDBusInterfaceVTable handlers{Method, nullptr, nullptr, {nullptr}};
  const auto object = g_dbus_connection_register_object(bus, path, info->interfaces[0], &handlers, &test, nullptr, nullptr);
  const auto name = g_bus_own_name_on_connection(bus, portal, G_BUS_NAME_OWNER_FLAGS_NONE, nullptr, nullptr, nullptr, nullptr);
  auto monitor = WatchMuonServiceSession([&] { ++test.ended; g_main_loop_quit(loop); });
  const auto guard = g_timeout_add_seconds(10, [](gpointer) -> gboolean { std::abort(); }, nullptr);
  g_main_loop_run(loop);
  g_source_remove(guard);
  monitor();
  g_bus_unown_name(name);
  g_dbus_connection_unregister_object(bus, object);
  g_dbus_node_info_unref(info);
  g_object_unref(bus);
  g_main_loop_unref(loop);
  if (test.acknowledgements != 2 || test.ended != 1) return 1;
  std::cout << "Desktop portal query, cancellation and ending: passed\n";
}
