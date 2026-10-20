/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "notifications/muon_notification_portal.h"
#include "notifications/muon_notification_linux.h"
#include <gio/gio.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <unistd.h>
#include <vector>

using namespace muon_internal;
using Variant = std::unique_ptr<GVariant, decltype(&g_variant_unref)>;
static constexpr char kDesktop[] = "dev.muon.PortalTest";
static constexpr char kPortal[] = "org.freedesktop.portal.Desktop";
static constexpr char kPortalPath[] = "/org/freedesktop/portal/desktop";
static constexpr char kXml[] = R"xml(<node>
<interface name="org.freedesktop.host.portal.Registry">
 <method name="Register"><arg type="s" direction="in"/><arg type="a{sv}" direction="in"/></method>
</interface>
<interface name="org.freedesktop.portal.Notification">
 <property name="version" type="u" access="read"/>
 <method name="AddNotification"><arg type="s" direction="in"/><arg type="a{sv}" direction="in"/></method>
 <method name="RemoveNotification"><arg type="s" direction="in"/></method>
</interface></node>)xml";
static constexpr char kSystemdXml[] = R"xml(<node>
<interface name="org.freedesktop.systemd1.Manager">
 <method name="StartTransientUnit"><arg type="s" direction="in"/>
 <arg type="s" direction="in"/><arg type="a(sv)" direction="in"/>
 <arg type="a(sa(sv))" direction="in"/><arg type="o" direction="out"/></method>
 <signal name="JobRemoved"><arg type="u"/><arg type="o"/><arg type="s"/><arg type="s"/></signal>
</interface></node>)xml";

struct PortalFixture {
  std::map<std::string, std::string> registrations;
  std::string posted_id;
  std::string removed_id;
  GVariant* notification = nullptr;
  bool legacy = false;
  bool scoped = false;
  bool hold_scope = false;
  cardio::promise_source<void> scope_started;
  cardio::promise_source<void> registered;
  ~PortalFixture() { if (notification) g_variant_unref(notification); }
};

static void Expect(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
static void Invoked(GDBusConnection* connection, const gchar* sender, const gchar*,
    const gchar*, const gchar* method, GVariant* args,
    GDBusMethodInvocation* invocation, gpointer data) {
  auto& fixture = *static_cast<PortalFixture*>(data);
  auto value = Variant(g_variant_get_child_value(args, 0), g_variant_unref);
  const auto text = std::string(g_variant_get_string(value.get(), nullptr));
  if (std::string_view(method) == "StartTransientUnit") {
    auto properties = Variant(g_variant_get_child_value(args, 2), g_variant_unref);
    auto property = Variant(g_variant_get_child_value(properties.get(), 0), g_variant_unref);
    auto wrapped = Variant(g_variant_get_child_value(property.get(), 1), g_variant_unref);
    auto pids = Variant(g_variant_get_variant(wrapped.get()), g_variant_unref);
    auto pid = Variant(g_variant_get_child_value(pids.get(), 0), g_variant_unref);
    if (!text.starts_with("app-" + std::string(kDesktop) + "-") ||
        !text.ends_with(".scope") || g_variant_get_uint32(pid.get()) != guint32(getpid())) {
      g_dbus_method_invocation_return_dbus_error(invocation,
          "org.freedesktop.DBus.Error.InvalidArgs", "Wrong application scope");
      return;
    }
    fixture.scoped = true;
    if (fixture.hold_scope) {
      g_dbus_method_invocation_return_value(invocation,
          g_variant_new("(o)", "/org/freedesktop/systemd1/job/1"));
      fixture.scope_started.try_resolve();
      return;
    }
    // A fast job can complete before the method reply. Subscribers must
    // already be listening instead of relying on the reply's arrival order.
    g_dbus_connection_emit_signal(connection, sender, "/org/freedesktop/systemd1",
        "org.freedesktop.systemd1.Manager", "JobRemoved",
        g_variant_new("(uoss)", guint32{1}, "/org/freedesktop/systemd1/job/1",
                      text.c_str(), "done"), nullptr);
    g_dbus_method_invocation_return_value(invocation,
        g_variant_new("(o)", "/org/freedesktop/systemd1/job/1"));
    return;
  } else if (std::string_view(method) == "Register") {
    if (fixture.registrations.contains(sender)) {
      g_dbus_method_invocation_return_dbus_error(invocation,
          "org.freedesktop.portal.Error.Failed", "Peer already registered");
      return;
    }
    fixture.registrations[sender] = text;
    fixture.registered.try_resolve();
  } else if (!(fixture.legacy ? fixture.scoped : fixture.registrations.contains(sender))) {
    g_dbus_method_invocation_return_dbus_error(invocation,
        "org.freedesktop.portal.Error.Failed", "Portal used before registration");
    return;
  } else if (std::string_view(method) == "AddNotification") {
    fixture.posted_id = text;
    if (fixture.notification) g_variant_unref(fixture.notification);
    fixture.notification = g_variant_get_child_value(args, 1);
  } else if (std::string_view(method) == "RemoveNotification") {
    fixture.removed_id = text;
  }
  g_dbus_method_invocation_return_value(invocation, nullptr);
}
static GVariant* Property(GDBusConnection*, const gchar* sender, const gchar*,
    const gchar*, const gchar*, GError** error, gpointer data) {
  auto& fixture = *static_cast<PortalFixture*>(data);
  if (!(fixture.legacy ? fixture.scoped : fixture.registrations.contains(sender))) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Portal used before registration");
    return nullptr;
  }
  return g_variant_new_uint32(1);
}
static cardio::promise<GVariant*> Call(GDBusConnection* connection,
    const char* destination, const char* path, const char* interface,
    const char* method, GVariant* args, const GVariantType* type) {
  co_return co_await cardio::gio::submit<GVariant*>(
      [=](GCancellable* cancel, GAsyncReadyCallback ready, gpointer data) {
        g_dbus_connection_call(connection, destination, path, interface, method,
            args, type, G_DBUS_CALL_FLAGS_NONE, 5000, cancel, ready, data);
      }, [](GObject* source, GAsyncResult* result, GError** error) {
        return g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, error);
      });
}
static cardio::promise<void> Activate(GDBusConnection* connection, GVariant* target) {
  auto targets = GVariantBuilder{};
  g_variant_builder_init(&targets, G_VARIANT_TYPE("av"));
  g_variant_builder_add(&targets, "v", target);
  auto platform = GVariantBuilder{};
  g_variant_builder_init(&platform, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&platform, "{sv}", "desktop-startup-id",
                        g_variant_new_string("desktop_TIME1234"));
  auto reply = Variant(co_await Call(connection, kDesktop, "/dev/muon/PortalTest",
      "org.freedesktop.Application", "ActivateAction",
      g_variant_new("(s@av@a{sv})", "notification",
                    g_variant_builder_end(&targets), g_variant_builder_end(&platform)),
      G_VARIANT_TYPE_UNIT), g_variant_unref);
}
static cardio::promise<void> RestartPortal(GDBusConnection* original,
    MuonNotificationBackend& backend, const MuonNotificationHandle& handle) {
  auto* connection = co_await cardio::gio::submit<GDBusConnection*>(
      [](GCancellable* cancel, GAsyncReadyCallback ready, gpointer data) {
        g_dbus_connection_new_for_address(g_getenv("DBUS_SESSION_BUS_ADDRESS"),
            static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                              G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
            nullptr, cancel, ready, data);
      }, [](GObject*, GAsyncResult* result, GError** error) {
        return g_dbus_connection_new_for_address_finish(result, error);
      });
  auto bus = std::unique_ptr<GDBusConnection, decltype(&g_object_unref)>(connection, g_object_unref);
  auto fixture = PortalFixture{};
  auto registered = fixture.registered.get_promise();
  auto* info = g_dbus_node_info_new_for_xml(kXml, nullptr);
  const auto table = GDBusInterfaceVTable{Invoked, Property, nullptr, {nullptr}};
  const auto registry = g_dbus_connection_register_object(connection, kPortalPath,
      info->interfaces[0], &table, &fixture, nullptr, nullptr);
  const auto notification = g_dbus_connection_register_object(connection, kPortalPath,
      info->interfaces[1], &table, &fixture, nullptr, nullptr);
  g_dbus_node_info_unref(info);
  try {
    auto released = Variant(co_await Call(original, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "ReleaseName", g_variant_new("(s)", kPortal),
        G_VARIANT_TYPE("(u)")), g_variant_unref);
    auto owned = Variant(co_await Call(connection, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "RequestName", g_variant_new("(su)", kPortal, guint32{4}),
        G_VARIANT_TYPE("(u)")), g_variant_unref);
    // Registration must recover on service ownership change, without waiting
    // for another public API request to discover the new daemon.
    co_await registered;
    Expect(fixture.registrations.size() == 1 &&
           fixture.registrations.begin()->second == kDesktop,
           "Portal restart did not restore the application identity");
    co_await backend.cancel(handle);
    Expect(fixture.removed_id == handle.owner.substr(handle.owner.find('/') + 1),
           "Portal restart lost the durable notification handle");
    const auto caps = co_await backend.capabilities();
    Expect(caps.available && caps.launch == true,
           "Portal restart lost launch support");
  } catch (...) {
    g_dbus_connection_unregister_object(connection, registry);
    g_dbus_connection_unregister_object(connection, notification);
    g_dbus_connection_close(connection, nullptr, nullptr, nullptr);
    throw;
  }
  g_dbus_connection_unregister_object(connection, registry);
  g_dbus_connection_unregister_object(connection, notification);
  g_dbus_connection_close(connection, nullptr, nullptr, nullptr);
}
static cardio::promise<void> RunTests(const std::filesystem::path& directory, bool legacy, bool routed, bool stop_pending) {
  auto* connection = co_await cardio::gio::submit<GDBusConnection*>(
      [](GCancellable* cancel, GAsyncReadyCallback ready, gpointer data) {
        g_bus_get(G_BUS_TYPE_SESSION, cancel, ready, data);
      }, [](GObject*, GAsyncResult* result, GError** error) {
        return g_bus_get_finish(result, error);
      });
  auto bus = std::unique_ptr<GDBusConnection, decltype(&g_object_unref)>(connection, g_object_unref);
  auto fixture = PortalFixture{};
  fixture.legacy = legacy;
  fixture.hold_scope = stop_pending;
  auto scope_started = fixture.scope_started.get_promise();
  auto* info = g_dbus_node_info_new_for_xml(kXml, nullptr);
  const auto table = GDBusInterfaceVTable{Invoked, Property, nullptr, {nullptr}};
  const auto registry = legacy ? 0 : g_dbus_connection_register_object(connection, kPortalPath,
      info->interfaces[0], &table, &fixture, nullptr, nullptr);
  const auto notification = g_dbus_connection_register_object(connection, kPortalPath,
      info->interfaces[1], &table, &fixture, nullptr, nullptr);
  g_dbus_node_info_unref(info);
  auto owned = Variant(co_await Call(connection, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "RequestName", g_variant_new("(su)", kPortal, guint32{4}),
      G_VARIANT_TYPE("(u)")), g_variant_unref);
  auto* systemd_info = g_dbus_node_info_new_for_xml(kSystemdXml, nullptr);
  const auto systemd = g_dbus_connection_register_object(connection, "/org/freedesktop/systemd1",
      systemd_info->interfaces[0], &table, &fixture, nullptr, nullptr);
  g_dbus_node_info_unref(systemd_info);
  auto systemd_owned = Variant(co_await Call(connection, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "RequestName",
      g_variant_new("(su)", "org.freedesktop.systemd1", guint32{4}),
      G_VARIANT_TYPE("(u)")), g_variant_unref);
  auto backend = routed
      ? CreateMuonLinuxNotificationBackend("Portal Test", kDesktop, "portal-test", (directory / "launcher").string())
      : CreateMuonPortalNotificationBackend({"portal-test", kDesktop, (directory / "launcher").string()});
  try {
    auto received = std::vector<std::string>{};
    backend.set_activation([&](std::string token, std::optional<std::string> action, std::string context) {
      received.push_back(token + ":" + action.value_or("<activate>") + ":" + context);
    });
    if (stop_pending) {
      auto pending = backend.capabilities();
      co_await scope_started;
      auto ping = Variant(co_await Call(connection, kDesktop, "/dev/muon/PortalTest",
          "org.freedesktop.DBus.Peer", "Ping", nullptr, G_VARIANT_TYPE_UNIT), g_variant_unref);
      backend.stop();
      const auto stopped = co_await pending;
      Expect(!stopped.available, "Stopped Portal still advertised support");
      g_dbus_connection_unregister_object(connection, notification);
      g_dbus_connection_unregister_object(connection, systemd);
      co_return;
    }
    const auto caps = co_await backend.capabilities();
    Expect(caps.available && caps.actions && caps.persistence == true && caps.launch == true,
           "verified Portal registration did not support process activation");
    auto menu_context = std::string{};
    Expect(static_cast<bool>(backend.set_application_activation),
           "D-Bus desktop launch cannot present the application");
    backend.set_application_activation([&](std::string context) { menu_context = std::move(context); });
    auto platform = GVariantBuilder{};
    g_variant_builder_init(&platform, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&platform, "{sv}", "desktop-startup-id", g_variant_new_string("menu_TIME5678"));
    auto opened = Variant(co_await Call(connection, kDesktop, "/dev/muon/PortalTest",
        "org.freedesktop.Application", "Activate",
        g_variant_new("(@a{sv})", g_variant_builder_end(&platform)),
        G_VARIANT_TYPE_UNIT), g_variant_unref);
    Expect(menu_context == "menu_TIME5678" && received.empty(),
           "ordinary desktop launch lost context or manufactured an event");
    const auto token = std::string(64, 'a');
    const auto post = ParseMuonNotificationPost(
        R"({"id":"order","title":"Order ready","body":"Plain <body>","data":{"secret":42},"actions":[{"id":"default","title":"View order"}]})", "");
    const auto handle = co_await backend.post(post, token);
    Expect(fixture.posted_id == token, "Portal used a reusable logical ID for a revision");
    auto body = Variant(g_variant_lookup_value(fixture.notification, "body", G_VARIANT_TYPE_STRING), g_variant_unref);
    Expect(body && std::string(g_variant_get_string(body.get(), nullptr)) == "Plain <body>",
           "Portal changed the plain body");
    auto target = Variant(g_variant_lookup_value(fixture.notification, "default-action-target", G_VARIANT_TYPE("(sbs)")), g_variant_unref);
    Expect(target != nullptr, "Portal did not encode the default activation target");
    co_await Activate(connection, target.get());
    Expect(received == std::vector<std::string>{token + ":<activate>:desktop_TIME1234"},
           "default activation lost its revision or OS context");
    auto buttons = Variant(g_variant_lookup_value(fixture.notification, "buttons", G_VARIANT_TYPE("aa{sv}")), g_variant_unref);
    auto button = Variant(g_variant_get_child_value(buttons.get(), 0), g_variant_unref);
    auto action_target = Variant(g_variant_lookup_value(button.get(), "target", G_VARIANT_TYPE("(sbs)")), g_variant_unref);
    co_await Activate(connection, action_target.get());
    Expect(received.back() == token + ":default:desktop_TIME1234",
           "a named default action became a regular activation");
    auto stale = handle;
    stale.owner = "another-application/" + token;
    co_await backend.cancel(stale);
    Expect(fixture.removed_id.empty(), "Portal canceled another application's notification");
    co_await backend.cancel(handle);
    Expect(fixture.removed_id == token, "Portal cancellation lost its durable revision identifier");
    if (!legacy) co_await RestartPortal(connection, backend, handle);
    // Re-check the effective installation, including an update that moves its
    // launcher. Cached support must not outlive that ownership change.
    std::ofstream(directory / "applications" / (std::string(kDesktop) + ".desktop"))
        << "[Desktop Entry]\nType=Application\nDBusActivatable=true\nExec=/another/launcher\n"
        << "X-muon-Launcher=" << (directory / "launcher").string() << "\n";
    const auto changed = co_await backend.capabilities();
    Expect(!changed.available && changed.launch == false,
           "Portal advertised launch after installation ownership changed");
    backend.stop();
    fixture.scope_started.try_resolve();
  } catch (...) {
    backend.stop();
    fixture.scope_started.try_resolve();
    if (registry) g_dbus_connection_unregister_object(connection, registry);
    g_dbus_connection_unregister_object(connection, notification);
    g_dbus_connection_unregister_object(connection, systemd);
    throw;
  }
  if (registry) g_dbus_connection_unregister_object(connection, registry);
  g_dbus_connection_unregister_object(connection, notification);
  g_dbus_connection_unregister_object(connection, systemd);
}
static cardio::promise<void> RunAndQuit(const std::filesystem::path& directory,
    GMainLoop* loop, bool& passed, bool legacy, bool routed, bool stop_pending) {
  try { co_await RunTests(directory, legacy, routed, stop_pending); passed = true; }
  catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
  g_main_loop_quit(loop);
}
int main(int argc, char** argv) {
  const auto directory = std::filesystem::temp_directory_path() /
      ("muon-portal-" + std::to_string(getpid()));
  std::filesystem::create_directories(directory / "applications");
  std::filesystem::create_directories(directory / "dbus-1/services");
  const auto launcher = (directory / "launcher").string();
  std::ofstream(launcher) << "#!/bin/sh\nexit 0\n";
  std::filesystem::permissions(launcher, std::filesystem::perms::owner_all);
  std::ofstream(directory / "applications" / (std::string(kDesktop) + ".desktop"))
      << "[Desktop Entry]\nType=Application\nName=Muon Portal Test\nDBusActivatable=true\n"
      << "Exec=" << launcher << "\nX-muon-Launcher=" << launcher << "\n";
  std::ofstream(directory / "dbus-1/services" / (std::string(kDesktop) + ".service"))
      << "[D-BUS Service]\nName=" << kDesktop << "\nExec=\"" << launcher
      << "\" --muon-notification-activation\nX-muon-AppId=portal-test\n";
  setenv("XDG_DATA_HOME", directory.c_str(), 1);
  setenv("GIO_USE_VFS", "local", 1);
  auto group = cardio::dispatcher_group_glib();
  auto dispatcher = cardio::dispatcher_host_glib_auto(group);
  cardio::set_current_dispatcher(&dispatcher);
  auto* loop = g_main_loop_new(nullptr, FALSE);
  auto passed = false;
  const auto option = argc > 1 ? std::string_view(argv[1]) : std::string_view{};
  auto pending = RunAndQuit(directory, loop, passed,
      option == "--legacy" || option == "--stop", option == "--route", option == "--stop");
  if (!pending.is_ready()) g_main_loop_run(loop);
  g_main_loop_unref(loop);
  group.shutdown();
  cardio::set_current_dispatcher(nullptr);
  std::filesystem::remove_all(directory);
  return passed ? 0 : 1;
}
