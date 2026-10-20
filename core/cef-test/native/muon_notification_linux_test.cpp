/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "notifications/muon_notification_linux.h"
#include <gio/gio.h>
#include <iostream>
#include <stdexcept>

using namespace muon_internal;
static void Expect(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
static cardio::promise<void> Control(GDBusConnection* connection,
                                    const char* method, GVariant* parameters) {
  auto* result = co_await cardio::gio::submit<GVariant*>(
      [=](GCancellable* cancel, GAsyncReadyCallback callback, gpointer data) {
        g_dbus_connection_call(connection, "dev.muon.NotificationTest", "/dev/muon/NotificationTest",
            "dev.muon.NotificationTest", method, parameters, G_VARIANT_TYPE_UNIT,
            G_DBUS_CALL_FLAGS_NONE, -1, cancel, callback, data);
      }, [](GObject* source, GAsyncResult* result, GError** error) {
        return g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, error);
      });
  g_variant_unref(result);
}
static cardio::promise<void> RunTests() {
  auto backend = CreateMuonLinuxNotificationBackend("Muon native test", "dev.muon.Test", "test", "");
  const auto caps = co_await backend.capabilities();
  Expect(caps.available && caps.actions, "running service capabilities were not read");
  Expect(!caps.persistence.has_value(), "server persistence advertisement claimed retention after process exit");
  auto* connection = co_await cardio::gio::submit<GDBusConnection*>(
      [](GCancellable* cancel, GAsyncReadyCallback callback, gpointer data) {
        g_bus_get(G_BUS_TYPE_SESSION, cancel, callback, data);
      }, [](GObject*, GAsyncResult* result, GError** error) { return g_bus_get_finish(result, error); });
  auto received = cardio::promise_source<std::string>{};
  backend.set_activation([&](std::string token, std::optional<std::string> action, std::string) {
    received.resolve(token + ":" + action.value_or("<activate>"));
  });
  auto post = ParseMuonNotificationPost(R"({"id":"same","title":"Title","body":"<plain & body>","actions":[{"id":"default","title":"Named action"}]})", "");
  const auto first = co_await backend.post(post, "first-token");
  const auto second = co_await backend.post(post, "second-token");
  Expect(first.id != 0 && second.id != first.id && first.owner == second.owner,
         "update reused a service ID or lost the owner epoch");
  co_await backend.cancel(first);
  co_await Control(connection, "Activate", g_variant_new("(us)", second.id, "muon:0"));
  Expect((co_await received.get_promise()) == "second-token:default", "named default action became a normal activation");
  co_await Control(connection, "SetCapabilities", g_variant_new("(@as)", g_variant_new_strv(nullptr, 0)));
  const auto changed = co_await backend.capabilities();
  Expect(changed.available && !changed.actions && changed.persistence == false, "capabilities were stale");
  auto rejected = false;
  co_await Control(connection, "FailNotify", g_variant_new("(b)", TRUE));
  try { co_await backend.post(post, "failed-token"); } catch (const std::exception&) { rejected = true; }
  Expect(rejected, "Notify error was reported as success");
  // A recycled numeric ID must not be canceled for another owner epoch.
  auto stale = second; stale.owner = "another-bus/:1.2";
  co_await Control(connection, "FailClose", g_variant_new("(b)", TRUE));
  co_await backend.cancel(stale);
  rejected = false;
  try { co_await backend.cancel(second); } catch (const std::exception&) { rejected = true; }
  Expect(rejected, "CloseNotification error was reported as success");
  backend.stop();
  g_object_unref(connection);
}
static cardio::promise<void> RunAndQuit(GMainLoop* loop, bool& passed) {
  try { co_await RunTests(); passed = true; }
  catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
  g_main_loop_quit(loop);
}
int main() {
  auto group = cardio::dispatcher_group_glib();
  auto dispatcher = cardio::dispatcher_host_glib_auto(group);
  cardio::set_current_dispatcher(&dispatcher);
  auto* loop = g_main_loop_new(nullptr, FALSE);
  auto passed = false;
  auto pending = RunAndQuit(loop, passed);
  if (!pending.is_ready()) g_main_loop_run(loop);
  g_main_loop_unref(loop);
  group.shutdown();
  cardio::set_current_dispatcher(nullptr);
  return passed ? 0 : 1;
}
