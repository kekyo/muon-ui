/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include <cardio.h>
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include <cstdio>
#include <deque>
#include <string>

// AT-SPI registrations belong to the calling connection. Keep one connection
// alive so a short-lived gdbus query cannot silently remove the event listener.
struct Observer {
  GMainLoop* loop = nullptr;
  GOutputStream* output = nullptr;
  std::deque<std::string> queue;
  cardio::promise<void> writing;
  bool active = false;
  bool failed = false;
};
static cardio::promise<void> Drain(Observer& state) {
  try {
    while (!state.queue.empty()) {
      const auto text = std::move(state.queue.front());
      state.queue.pop_front();
      co_await cardio::gio::write_all(state.output,
          std::as_bytes(std::span(text.data(), text.size())));
    }
  } catch (const std::exception& error) {
    std::fprintf(stderr, "AT-SPI output failed: %s\n", error.what());
    state.failed = true;
    g_main_loop_quit(state.loop);
  }
  state.active = false;
}
static void Emit(Observer& state, std::string message) {
  state.queue.push_back(std::move(message));
  if (!state.active) {
    state.active = true;
    state.writing = Drain(state);
  }
}
static void Changed(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                     const gchar*, GVariant*, gpointer data) {
  Emit(*static_cast<Observer*>(data), "changed\n");
}
static cardio::promise<void> Registry(GDBusConnection* connection,
                                      const char* method, GVariant* parameters) {
  auto* result = co_await cardio::gio::submit<GVariant*>(
      [=](GCancellable* cancel, GAsyncReadyCallback callback, gpointer data) {
        g_dbus_connection_call(connection, "org.a11y.atspi.Registry", "/org/a11y/atspi/registry",
            "org.a11y.atspi.Registry", method, parameters, G_VARIANT_TYPE_UNIT,
            G_DBUS_CALL_FLAGS_NONE, -1, cancel, callback, data);
      }, [](GObject* object, GAsyncResult* result, GError** error) {
        return g_dbus_connection_call_finish(G_DBUS_CONNECTION(object), result, error);
      });
  g_variant_unref(result);
}
static cardio::promise<void> Run(Observer& state, std::string address, std::string shell) {
  GDBusConnection* connection = nullptr;
  guint subscription = 0;
  auto* input = g_unix_input_stream_new(0, FALSE);
  try {
    connection = co_await cardio::gio::submit<GDBusConnection*>(
        [&address](GCancellable* cancel, GAsyncReadyCallback callback, gpointer data) {
          g_dbus_connection_new_for_address(address.c_str(),
              static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                                G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
              nullptr, cancel, callback, data);
        }, [](GObject*, GAsyncResult* result, GError** error) {
          return g_dbus_connection_new_for_address_finish(result, error);
        });
    subscription = g_dbus_connection_signal_subscribe(connection, shell.c_str(),
        "org.a11y.atspi.Event.Object", nullptr, nullptr, nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE, &Changed, &state, nullptr);
    co_await Registry(connection, "RegisterEvent",
        g_variant_new("(s@ass)", "object", g_variant_new_strv(nullptr, 0), shell.c_str()));
    Emit(state, "ready\n");
    auto byte = std::byte{};
    while (co_await cardio::gio::read(input, std::span<std::byte>(&byte, 1)) != 0) {}
    co_await Registry(connection, "DeregisterEvent", g_variant_new("(s)", "object"));
    if (state.active) co_await state.writing;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "AT-SPI observation failed: %s\n", error.what());
    state.failed = true;
  }
  if (connection) {
    if (subscription) g_dbus_connection_signal_unsubscribe(connection, subscription);
    g_object_unref(connection);
  }
  g_object_unref(input);
  g_main_loop_quit(state.loop);
}
int main(int argc, char** argv) {
  if (argc != 3) return 2;
  auto group = cardio::dispatcher_group_glib();
  auto dispatcher = cardio::dispatcher_host_glib_auto(group);
  cardio::set_current_dispatcher(&dispatcher);
  Observer state;
  state.loop = g_main_loop_new(nullptr, FALSE);
  state.output = g_unix_output_stream_new(1, FALSE);
  auto pending = Run(state, argv[1], argv[2]);
  if (!pending.is_ready()) g_main_loop_run(state.loop);
  g_object_unref(state.output);
  g_main_loop_unref(state.loop);
  group.shutdown();
  cardio::set_current_dispatcher(nullptr);
  return state.failed ? 1 : 0;
}
