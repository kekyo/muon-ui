/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include <cardio.h>
#include <gio/gunixoutputstream.h>
#include <yyjson.h>

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

// This server runs only on a private test bus. Real desktop tests use the
// desktop's own service; these controls exercise failure and stale-event paths.
static constexpr char kNotificationPath[] = "/org/freedesktop/Notifications";
static constexpr char kNotificationInterface[] = "org.freedesktop.Notifications";
static constexpr char kControlName[] = "dev.muon.NotificationTest";
static constexpr char kControlPath[] = "/dev/muon/NotificationTest";
static constexpr char kInterfaceXml[] = R"xml(
<node>
  <interface name="org.freedesktop.Notifications">
    <method name="GetCapabilities"><arg type="as" direction="out"/></method>
    <method name="GetServerInformation">
      <arg type="s" direction="out"/><arg type="s" direction="out"/>
      <arg type="s" direction="out"/><arg type="s" direction="out"/>
    </method>
    <method name="Notify">
      <arg type="s" direction="in"/><arg type="u" direction="in"/>
      <arg type="s" direction="in"/><arg type="s" direction="in"/>
      <arg type="s" direction="in"/><arg type="as" direction="in"/>
      <arg type="a{sv}" direction="in"/><arg type="i" direction="in"/>
      <arg type="u" direction="out"/>
    </method>
    <method name="CloseNotification"><arg type="u" direction="in"/></method>
    <signal name="ActionInvoked"><arg type="u"/><arg type="s"/></signal>
    <signal name="NotificationClosed"><arg type="u"/><arg type="u"/></signal>
  </interface>
  <interface name="dev.muon.NotificationTest">
    <method name="Activate"><arg type="u" direction="in"/><arg type="s" direction="in"/></method>
    <method name="Dismiss"><arg type="u" direction="in"/></method>
    <method name="SetCapabilities"><arg type="as" direction="in"/></method>
    <method name="FailNotify"><arg type="b" direction="in"/></method>
    <method name="FailClose"><arg type="b" direction="in"/></method>
  </interface>
</node>)xml";

struct NotificationRecord {
  std::string sender;
  bool active = true;
};

struct TestServer {
  cardio::dispatcher_group_glib* group = nullptr;
  GDBusConnection* connection = nullptr;
  GOutputStream* output = nullptr;
  std::deque<std::string> output_queue;
  bool writing = false;
  cardio::promise<void> writer;
  std::map<guint32, NotificationRecord> records;
  std::vector<std::string> capabilities = {"actions", "body", "persistence"};
  guint32 next_id = 1;
  unsigned int acquired_names = 0;
  bool fail_notify = false;
  bool fail_close = false;
  bool failed = false;
};

static cardio::promise<void> DrainOutput(TestServer& server) {
  try {
    while (!server.output_queue.empty()) {
      const auto text = std::move(server.output_queue.front());
      server.output_queue.pop_front();
      co_await cardio::gio::write_all(server.output,
          std::as_bytes(std::span(text.data(), text.size())));
    }
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Notification test output failed: %s\n", error.what());
    server.failed = true;
    server.group->shutdown();
  }
  server.writing = false;
}

static void Observe(TestServer& server, std::string json) {
  server.output_queue.push_back(std::move(json) + "\n");
  if (!server.writing) {
    server.writing = true;
    server.writer = DrainOutput(server);
  }
}

static std::string DescribeNotification(guint32 id, GVariant* parameters) {
  const auto document = std::unique_ptr<yyjson_mut_doc, decltype(&yyjson_mut_doc_free)>(
      yyjson_mut_doc_new(nullptr), yyjson_mut_doc_free);
  auto* root = yyjson_mut_obj(document.get());
  yyjson_mut_doc_set_root(document.get(), root);
  yyjson_mut_obj_add_str(document.get(), root, "type", "notify");
  yyjson_mut_obj_add_uint(document.get(), root, "id", id);
  const char* names[] = {"app", nullptr, "icon", "title", "body"};
  for (gsize index = 0; index < 5; ++index) {
    auto* value = g_variant_get_child_value(parameters, index);
    if (names[index] == nullptr) {
      yyjson_mut_obj_add_uint(document.get(), root, "replacesId", g_variant_get_uint32(value));
    } else {
      yyjson_mut_obj_add_strcpy(document.get(), root, names[index], g_variant_get_string(value, nullptr));
    }
    g_variant_unref(value);
  }
  auto* actions = g_variant_get_child_value(parameters, 5);
  auto* action_array = yyjson_mut_arr(document.get());
  yyjson_mut_obj_add_val(document.get(), root, "actions", action_array);
  for (gsize index = 0; index < g_variant_n_children(actions); ++index) {
    auto* action = g_variant_get_child_value(actions, index);
    yyjson_mut_arr_add_strcpy(document.get(), action_array, g_variant_get_string(action, nullptr));
    g_variant_unref(action);
  }
  g_variant_unref(actions);
  auto* hints = g_variant_get_child_value(parameters, 6);
  for (const auto* key : {"desktop-entry", "category"}) {
    const gchar* value = nullptr;
    if (g_variant_lookup(hints, key, "&s", &value)) {
      yyjson_mut_obj_add_strcpy(document.get(), root, key, value);
    }
  }
  g_variant_unref(hints);
  const auto json = std::unique_ptr<char, decltype(&std::free)>(
      yyjson_mut_write(document.get(), 0, nullptr), std::free);
  if (json == nullptr) throw std::runtime_error("Cannot serialize notification observation");
  return json.get();
}

static void EmitSignal(TestServer& server, guint32 id, const char* signal, GVariant* arguments) {
  GError* error = nullptr;
  if (!g_dbus_connection_emit_signal(server.connection, server.records.at(id).sender.c_str(),
          kNotificationPath, kNotificationInterface, signal, arguments, &error)) {
    const auto message = std::string(error->message);
    g_error_free(error);
    throw std::runtime_error(message);
  }
}

static void OnMethodCall(GDBusConnection*, const gchar* sender, const gchar*,
                         const gchar* interface_name, const gchar* method_name,
                         GVariant* parameters, GDBusMethodInvocation* invocation,
                         gpointer data) {
  auto& server = *static_cast<TestServer*>(data);
  const auto method = std::string(method_name);
  try {
    if (std::string(interface_name) == kControlName) {
      if (method == "SetCapabilities") {
        gchar** values = nullptr;
        g_variant_get(parameters, "(^as)", &values);
        server.capabilities.clear();
        for (auto** item = values; *item != nullptr; ++item) server.capabilities.emplace_back(*item);
        g_strfreev(values);
      } else if (method == "FailNotify" || method == "FailClose") {
        gboolean value = FALSE;
        g_variant_get(parameters, "(b)", &value);
        (method == "FailNotify" ? server.fail_notify : server.fail_close) = value;
      } else {
        guint32 id = 0;
        const gchar* action = nullptr;
        if (method == "Activate") g_variant_get(parameters, "(u&s)", &id, &action);
        else g_variant_get(parameters, "(u)", &id);
        if (!server.records.contains(id)) throw std::runtime_error("Unknown notification ID");
        // Retain old records so the test can deliver an action already queued
        // before replacement or cancellation, even after the old UI is closed.
        if (method == "Activate") {
          EmitSignal(server, id, "ActionInvoked", g_variant_new("(us)", id, action));
        } else {
          server.records.at(id).active = false;
          EmitSignal(server, id, "NotificationClosed", g_variant_new("(uu)", id, 2U));
        }
      }
      g_dbus_method_invocation_return_value(invocation, nullptr);
      return;
    }
    if (method == "GetCapabilities") {
      auto values = std::vector<const gchar*>{};
      for (const auto& capability : server.capabilities) values.push_back(capability.c_str());
      g_dbus_method_invocation_return_value(invocation,
          g_variant_new("(@as)", g_variant_new_strv(values.data(), static_cast<gssize>(values.size()))));
    } else if (method == "GetServerInformation") {
      g_dbus_method_invocation_return_value(invocation,
          g_variant_new("(ssss)", "Muon Test", "Muon", "1", "1.2"));
    } else if (method == "Notify") {
      if (server.fail_notify) throw std::runtime_error("Injected Notify failure");
      guint32 replaces = 0;
      g_variant_get_child(parameters, 1, "u", &replaces);
      const auto id = replaces == 0 ? server.next_id++ : replaces;
      server.records[id] = {sender, true};
      const auto observation = DescribeNotification(id, parameters);
      g_dbus_method_invocation_return_value(invocation, g_variant_new("(u)", id));
      Observe(server, observation);
    } else if (method == "CloseNotification") {
      if (server.fail_close) throw std::runtime_error("Injected CloseNotification failure");
      guint32 id = 0;
      g_variant_get(parameters, "(u)", &id);
      if (!server.records.contains(id) || !server.records.at(id).active) {
        throw std::runtime_error("Notification is already closed");
      }
      server.records.at(id).active = false;
      EmitSignal(server, id, "NotificationClosed", g_variant_new("(uu)", id, 3U));
      g_dbus_method_invocation_return_value(invocation, nullptr);
      Observe(server, "{\"type\":\"close\",\"id\":" + std::to_string(id) + "}");
    }
  } catch (const std::exception& error) {
    g_dbus_method_invocation_return_dbus_error(invocation,
        "dev.muon.NotificationTest.Error", error.what());
  }
}

static void NameAcquired(GDBusConnection*, const gchar*, gpointer data) {
  auto& server = *static_cast<TestServer*>(data);
  if (++server.acquired_names == 2) Observe(server, "{\"type\":\"ready\"}");
}

static void NameLost(GDBusConnection*, const gchar* name, gpointer data) {
  auto& server = *static_cast<TestServer*>(data);
  std::fprintf(stderr, "Notification test server could not own %s\n", name);
  server.failed = true;
  server.group->shutdown();
}

static cardio::promise<void> StartServer(TestServer& server) {
  try {
    server.connection = co_await cardio::gio::submit<GDBusConnection*>(
        [](GCancellable* cancellation, GAsyncReadyCallback callback, gpointer data) {
          g_bus_get(G_BUS_TYPE_SESSION, cancellation, callback, data);
        },
        [](GObject*, GAsyncResult* result, GError** error) {
          return g_bus_get_finish(result, error);
        });
    GError* error = nullptr;
    const auto info = std::unique_ptr<GDBusNodeInfo, decltype(&g_dbus_node_info_unref)>(
        g_dbus_node_info_new_for_xml(kInterfaceXml, &error), g_dbus_node_info_unref);
    if (info == nullptr) {
      const auto message = std::string(error->message);
      g_error_free(error);
      throw std::runtime_error(message);
    }
    const GDBusInterfaceVTable table = {OnMethodCall, nullptr, nullptr, {nullptr}};
    for (unsigned int index = 0; index < 2; ++index) {
      if (g_dbus_connection_register_object(server.connection,
              index == 0 ? kNotificationPath : kControlPath,
              info->interfaces[index], &table, &server, nullptr, &error) == 0) {
        const auto message = std::string(error->message);
        g_error_free(error);
        throw std::runtime_error(message);
      }
    }
    for (const auto* name : {kNotificationInterface, kControlName}) {
      g_bus_own_name_on_connection(server.connection, name, G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE,
          NameAcquired, NameLost, &server, nullptr);
    }
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Notification test server failed: %s\n", error.what());
    server.failed = true;
    server.group->shutdown();
  }
}

int main() {
  auto group = cardio::dispatcher_group_glib();
  auto dispatcher = cardio::dispatcher_host_glib_auto(group);
  cardio::set_current_dispatcher(&dispatcher);
  auto server = TestServer{};
  server.group = &group;
  server.output = g_unix_output_stream_new(1, FALSE);
  auto startup = StartServer(server);
  dispatcher.park();
  cardio::set_current_dispatcher(nullptr);
  g_object_unref(server.output);
  if (server.connection != nullptr) g_object_unref(server.connection);
  return server.failed ? 1 : 0;
}
