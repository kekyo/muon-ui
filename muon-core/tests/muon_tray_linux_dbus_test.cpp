/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "browser/muon_tray.h"
#include "browser/muon_icon.h"
#include "browser/muon_title_bar.h"

#include <gio/gio.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr int kTimeoutMs = 3000;

bool Expect(bool condition, const std::string& message) {
  if (!condition) {
    std::fprintf(stderr, "%s\n", message.c_str());
    return false;
  }
  return true;
}

void ClearGError(GError** error) {
  if (error != nullptr && *error != nullptr) {
    g_error_free(*error);
    *error = nullptr;
  }
}

template <typename Predicate>
bool WaitUntil(Predicate predicate, int timeout_ms = kTimeoutMs) {
  const auto deadline = g_get_monotonic_time() +
                        static_cast<gint64>(timeout_ms) * 1000;
  while (g_get_monotonic_time() < deadline) {
    while (g_main_context_iteration(nullptr, FALSE)) {
    }
    if (predicate()) {
      return true;
    }
    g_usleep(10000);
  }
  while (g_main_context_iteration(nullptr, FALSE)) {
  }
  return predicate();
}

class TestDbusSession final {
 public:
  TestDbusSession() = default;
  ~TestDbusSession() {
    if (bus_ != nullptr) {
      g_test_dbus_down(bus_);
      g_object_unref(bus_);
    }
  }

  bool Start() {
    bus_ = g_test_dbus_new(G_TEST_DBUS_NONE);
    if (bus_ == nullptr) {
      return false;
    }
    g_test_dbus_up(bus_);
    return true;
  }

 private:
  GTestDBus* bus_ = nullptr;
};

struct AsyncCall {
  GVariant* result = nullptr;
  GError* error = nullptr;
  bool done = false;
};

void FinishAsyncCall(GObject* source_object,
                     GAsyncResult* result,
                     gpointer user_data) {
  auto* call = static_cast<AsyncCall*>(user_data);
  call->result =
      g_dbus_connection_call_finish(G_DBUS_CONNECTION(source_object), result,
                                    &call->error);
  call->done = true;
}

bool CallDbus(GDBusConnection* connection,
              const std::string& bus_name,
              const char* object_path,
              const char* interface_name,
              const char* method_name,
              GVariant* parameters,
              const GVariantType* reply_type,
              GVariant** result,
              std::string* error_message,
              bool expect_error = false) {
  AsyncCall call;
  g_dbus_connection_call(connection, bus_name.c_str(), object_path,
                         interface_name, method_name, parameters, reply_type,
                         G_DBUS_CALL_FLAGS_NONE, kTimeoutMs, nullptr,
                         FinishAsyncCall, &call);
  if (!WaitUntil([&call]() { return call.done; })) {
    *error_message = "D-Bus call timed out: " + bus_name + " " + method_name;
    return false;
  }
  if (call.error != nullptr) {
    *error_message = call.error->message;
    g_error_free(call.error);
    return expect_error;
  }
  if (expect_error) {
    if (call.result != nullptr) {
      g_variant_unref(call.result);
    }
    *error_message = "D-Bus call unexpectedly succeeded: " + bus_name + " " +
                     method_name;
    return false;
  }
  if (result != nullptr) {
    *result = call.result;
  } else if (call.result != nullptr) {
    g_variant_unref(call.result);
  }
  return true;
}

GVariant* UnwrapDbusProperty(GVariant* properties_get_result) {
  if (properties_get_result == nullptr) {
    return nullptr;
  }
  auto* variant = g_variant_get_child_value(properties_get_result, 0);
  if (variant == nullptr) {
    return nullptr;
  }
  auto* value = g_variant_get_variant(variant);
  g_variant_unref(variant);
  return value;
}

bool ReadStringProperty(GDBusConnection* connection,
                        const std::string& bus_name,
                        const char* object_path,
                        const char* interface_name,
                        const char* property_name,
                        std::string* value,
                        std::string* error_message) {
  GVariant* result = nullptr;
  if (!CallDbus(connection, bus_name, object_path,
                "org.freedesktop.DBus.Properties", "Get",
                g_variant_new("(ss)", interface_name, property_name),
                G_VARIANT_TYPE("(v)"), &result, error_message)) {
    return false;
  }
  auto* property = UnwrapDbusProperty(result);
  g_variant_unref(result);
  if (property == nullptr ||
      (!g_variant_is_of_type(property, G_VARIANT_TYPE_STRING) &&
       !g_variant_is_of_type(property, G_VARIANT_TYPE_OBJECT_PATH))) {
    if (property != nullptr) {
      g_variant_unref(property);
    }
    *error_message =
        std::string("Property is not a string-like value: ") + property_name;
    return false;
  }
  value->assign(g_variant_get_string(property, nullptr));
  g_variant_unref(property);
  return true;
}

bool ReadIconPixmapProperty(GDBusConnection* connection,
                            const std::string& bus_name,
                            std::vector<uint8_t>* bytes,
                            std::string* error_message) {
  GVariant* result = nullptr;
  if (!CallDbus(connection, bus_name, "/StatusNotifierItem",
                "org.freedesktop.DBus.Properties", "Get",
                g_variant_new("(ss)", "org.freedesktop.StatusNotifierItem",
                              "IconPixmap"),
                G_VARIANT_TYPE("(v)"), &result, error_message)) {
    return false;
  }
  auto* property = UnwrapDbusProperty(result);
  g_variant_unref(result);
  if (property == nullptr ||
      !g_variant_is_of_type(property, G_VARIANT_TYPE("a(iiay)"))) {
    if (property != nullptr) {
      g_variant_unref(property);
    }
    *error_message = "IconPixmap has an unexpected type";
    return false;
  }
  if (g_variant_n_children(property) != 1) {
    g_variant_unref(property);
    *error_message = "IconPixmap should contain one bitmap";
    return false;
  }
  auto* pixmap = g_variant_get_child_value(property, 0);
  auto width = gint{0};
  auto height = gint{0};
  GVariant* raw_bytes = nullptr;
  g_variant_get(pixmap, "(ii@ay)", &width, &height, &raw_bytes);
  auto byte_count = gsize{0};
  const auto* data = g_variant_get_fixed_array(raw_bytes, &byte_count, 1);
  bytes->assign(static_cast<const uint8_t*>(data),
                static_cast<const uint8_t*>(data) + byte_count);
  g_variant_unref(raw_bytes);
  g_variant_unref(pixmap);
  g_variant_unref(property);
  return width == 1 && height == 1;
}

class FakeStatusNotifierWatcher final {
 public:
  FakeStatusNotifierWatcher() = default;
  ~FakeStatusNotifierWatcher() { Stop(); }

  bool Start(std::string* error_message) {
    GError* error = nullptr;
    connection_ = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (connection_ == nullptr) {
      *error_message = error != nullptr ? error->message : "No test bus";
      ClearGError(&error);
      return false;
    }

    static constexpr char kWatcherXml[] = R"XML(
<node>
  <interface name="org.freedesktop.StatusNotifierWatcher">
    <method name="RegisterStatusNotifierItem">
      <arg name="service" type="s" direction="in"/>
    </method>
    <method name="RegisterStatusNotifierHost">
      <arg name="service" type="s" direction="in"/>
    </method>
    <property name="RegisteredStatusNotifierItems" type="as" access="read"/>
    <property name="IsStatusNotifierHostRegistered" type="b" access="read"/>
    <property name="ProtocolVersion" type="i" access="read"/>
    <signal name="StatusNotifierItemRegistered">
      <arg name="service" type="s"/>
    </signal>
    <signal name="StatusNotifierItemUnregistered">
      <arg name="service" type="s"/>
    </signal>
    <signal name="StatusNotifierHostRegistered"/>
  </interface>
</node>
)XML";
    node_info_ = g_dbus_node_info_new_for_xml(kWatcherXml, &error);
    if (node_info_ == nullptr) {
      *error_message =
          error != nullptr ? error->message : "Watcher XML parse failed";
      ClearGError(&error);
      return false;
    }
    auto* interface_info = g_dbus_node_info_lookup_interface(
        node_info_, "org.freedesktop.StatusNotifierWatcher");
    if (interface_info == nullptr) {
      *error_message = "Watcher interface is missing";
      return false;
    }

    static GDBusInterfaceVTable vtable = {
        &FakeStatusNotifierWatcher::MethodCall,
        &FakeStatusNotifierWatcher::GetProperty,
        nullptr,
        {nullptr},
    };
    registration_id_ = g_dbus_connection_register_object(
        connection_, "/StatusNotifierWatcher", interface_info, &vtable, this,
        nullptr, &error);
    if (registration_id_ == 0) {
      *error_message =
          error != nullptr ? error->message : "Watcher export failed";
      ClearGError(&error);
      return false;
    }

    owner_id_ = g_bus_own_name_on_connection(
        connection_, "org.freedesktop.StatusNotifierWatcher",
        G_BUS_NAME_OWNER_FLAGS_NONE, &FakeStatusNotifierWatcher::NameAcquired,
        nullptr, this, nullptr);
    if (!WaitUntil([this]() { return name_acquired_; })) {
      *error_message = "Watcher name was not acquired";
      return false;
    }
    return true;
  }

  void Stop() {
    if (owner_id_ != 0) {
      g_bus_unown_name(owner_id_);
      owner_id_ = 0;
    }
    if (registration_id_ != 0 && connection_ != nullptr) {
      g_dbus_connection_unregister_object(connection_, registration_id_);
      registration_id_ = 0;
    }
    if (node_info_ != nullptr) {
      g_dbus_node_info_unref(node_info_);
      node_info_ = nullptr;
    }
    if (connection_ != nullptr) {
      g_object_unref(connection_);
      connection_ = nullptr;
    }
  }

  GDBusConnection* connection() const { return connection_; }

  const std::vector<std::string>& registered_items() const {
    return registered_items_;
  }

 private:
  GDBusConnection* connection_ = nullptr;
  GDBusNodeInfo* node_info_ = nullptr;
  guint registration_id_ = 0;
  guint owner_id_ = 0;
  bool name_acquired_ = false;
  std::vector<std::string> registered_items_;

  static void NameAcquired(GDBusConnection* connection,
                           const gchar* name,
                           gpointer user_data) {
    (void)connection;
    (void)name;
    static_cast<FakeStatusNotifierWatcher*>(user_data)->name_acquired_ = true;
  }

  static void MethodCall(GDBusConnection* connection,
                         const gchar* sender,
                         const gchar* object_path,
                         const gchar* interface_name,
                         const gchar* method_name,
                         GVariant* parameters,
                         GDBusMethodInvocation* invocation,
                         gpointer user_data) {
    (void)sender;
    (void)object_path;
    (void)interface_name;
    auto* watcher = static_cast<FakeStatusNotifierWatcher*>(user_data);
    const auto method = std::string(method_name == nullptr ? "" : method_name);
    if (method == "RegisterStatusNotifierItem") {
      const char* service = nullptr;
      g_variant_get(parameters, "(&s)", &service);
      watcher->registered_items_.emplace_back(service == nullptr ? "" : service);
      g_dbus_method_invocation_return_value(invocation, nullptr);
      g_dbus_connection_emit_signal(
          connection, nullptr, "/StatusNotifierWatcher",
          "org.freedesktop.StatusNotifierWatcher",
          "StatusNotifierItemRegistered",
          g_variant_new("(s)", service == nullptr ? "" : service), nullptr);
      return;
    }
    if (method == "RegisterStatusNotifierHost") {
      g_dbus_method_invocation_return_value(invocation, nullptr);
      g_dbus_connection_emit_signal(
          connection, nullptr, "/StatusNotifierWatcher",
          "org.freedesktop.StatusNotifierWatcher",
          "StatusNotifierHostRegistered", nullptr, nullptr);
      return;
    }
    g_dbus_method_invocation_return_error(
        invocation, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
        "Unknown watcher method: %s", method.c_str());
  }

  static GVariant* GetProperty(GDBusConnection* connection,
                               const gchar* sender,
                               const gchar* object_path,
                               const gchar* interface_name,
                               const gchar* property_name,
                               GError** error,
                               gpointer user_data) {
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    auto* watcher = static_cast<FakeStatusNotifierWatcher*>(user_data);
    const auto property =
        std::string(property_name == nullptr ? "" : property_name);
    if (property == "RegisteredStatusNotifierItems") {
      GVariantBuilder builder;
      g_variant_builder_init(&builder, G_VARIANT_TYPE("as"));
      for (const auto& item : watcher->registered_items_) {
        g_variant_builder_add(&builder, "s", item.c_str());
      }
      return g_variant_builder_end(&builder);
    }
    if (property == "IsStatusNotifierHostRegistered") {
      return g_variant_new_boolean(TRUE);
    }
    if (property == "ProtocolVersion") {
      return g_variant_new_int32(0);
    }
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                "Unknown watcher property: %s", property.c_str());
    return nullptr;
  }
};

MuonBrowserTrayIcon CreateIcon(uint8_t red,
                               uint8_t green,
                               uint8_t blue,
                               uint8_t alpha) {
  MuonIconBitmap bitmap;
  bitmap.pixel_width = 1;
  bitmap.pixel_height = 1;
  bitmap.rgba = {red, green, blue, alpha};
  MuonBrowserTrayIcon icon;
  icon.bitmap = std::make_shared<const MuonIconBitmap>(std::move(bitmap));
  return icon;
}

bool TestTitleBarIconBitmapReuse() {
  MuonIconBitmap bitmap;
  bitmap.rgba = {1, 2, 3, 4};
  bitmap.pixel_width = 1;
  bitmap.pixel_height = 1;

  MuonTitleBarIcon title_bar_icon;
  title_bar_icon.bitmap =
      std::make_shared<const MuonIconBitmap>(std::move(bitmap));
  MuonBrowserTrayIcon tray_icon;
  std::string error_message;
  if (!Expect(LoadMuonBrowserTrayIconFromTitleBarIcon(
                  title_bar_icon, "shared title bar icon", &tray_icon,
                  &error_message),
              error_message) ||
      !Expect(tray_icon.bitmap == title_bar_icon.bitmap,
              "tray icon did not reuse the decoded title bar bitmap")) {
    return false;
  }

  MuonIconBitmap invalid_bitmap;
  invalid_bitmap.rgba = {1, 2, 3, 4};
  invalid_bitmap.pixel_width = 2;
  invalid_bitmap.pixel_height = 2;
  MuonTitleBarIcon invalid_title_bar_icon;
  invalid_title_bar_icon.bitmap =
      std::make_shared<const MuonIconBitmap>(std::move(invalid_bitmap));
  auto invalid_tray_icon = MuonBrowserTrayIcon{};
  error_message.clear();
  return Expect(!LoadMuonBrowserTrayIconFromTitleBarIcon(
                    invalid_title_bar_icon, "invalid title bar icon",
                    &invalid_tray_icon, &error_message),
                "tray icon accepted an invalid decoded bitmap") &&
         Expect(!invalid_tray_icon.bitmap,
                "rejected tray icon retained an invalid bitmap");
}

std::vector<MuonBrowserTrayMenuItem> CreateMenuItems() {
  MuonBrowserTrayMenuItem open;
  open.type = kMuonBrowserTrayMenuItemCommand;
  open.id = "open";
  open.label = "Open";

  MuonBrowserTrayMenuItem check;
  check.type = kMuonBrowserTrayMenuItemCheckbox;
  check.id = "check";
  check.label = "Check";
  check.checked = false;

  MuonBrowserTrayMenuItem radio;
  radio.type = kMuonBrowserTrayMenuItemRadio;
  radio.id = "radio";
  radio.label = "Radio";
  radio.checked = false;

  return {open, check, radio};
}

bool CallMenuEvent(GDBusConnection* connection,
                   const std::string& bus_name,
                   const std::string& menu_path,
                   int id,
                   std::string* error_message) {
  auto* data = g_variant_new_variant(g_variant_new_string(""));
  return CallDbus(connection, bus_name, menu_path.c_str(),
                  "com.canonical.dbusmenu", "Event",
                  g_variant_new("(is@vu)", id, "clicked", data, 0U), nullptr,
                  nullptr, error_message);
}

bool TestLinuxStatusNotifierItemBackend() {
  TestDbusSession session;
  if (!Expect(session.Start(), "failed to start private D-Bus session")) {
    return false;
  }

  std::string error_message;
  FakeStatusNotifierWatcher watcher;
  if (!Expect(watcher.Start(&error_message), error_message)) {
    return false;
  }

  auto service = CreateMuonBrowserTrayService("muon-test");
  auto events = std::vector<MuonBrowserTrayEvent>{};
  auto callback = [&events](const MuonBrowserTrayEvent& event) {
    events.push_back(event);
  };

  MuonBrowserTrayOptions options;
  options.id = "tray-test";
  options.tooltip = "Initial tooltip";
  options.menu_items = CreateMenuItems();

  std::string tray_id;
  if (!Expect(service->CreateTray(7, options, CreateIcon(1, 2, 3, 4), callback,
                                  &tray_id, &error_message),
              error_message) ||
      !Expect(tray_id == "tray-test", "unexpected tray id") ||
      !Expect(WaitUntil([&watcher]() {
                return !watcher.registered_items().empty();
              }),
              "watcher did not receive tray registration")) {
    return false;
  }

  const auto item_bus_name = watcher.registered_items().back();
  std::string property_value;
  if (!Expect(ReadStringProperty(watcher.connection(), item_bus_name,
                                 "/StatusNotifierItem",
                                 "org.freedesktop.StatusNotifierItem", "Id",
                                 &property_value, &error_message),
              error_message) ||
      !Expect(property_value == "tray-test", "unexpected SNI Id property")) {
    return false;
  }

  if (!Expect(ReadStringProperty(watcher.connection(), item_bus_name,
                                 "/StatusNotifierItem",
                                 "org.freedesktop.StatusNotifierItem", "Status",
                                 &property_value, &error_message),
              error_message) ||
      !Expect(property_value == "Active", "unexpected SNI Status property")) {
    return false;
  }

  std::vector<uint8_t> icon_bytes;
  if (!Expect(ReadIconPixmapProperty(watcher.connection(), item_bus_name,
                                     &icon_bytes, &error_message),
              error_message) ||
      !Expect(icon_bytes == std::vector<uint8_t>({4, 1, 2, 3}),
              "unexpected SNI IconPixmap bytes")) {
    return false;
  }

  if (!Expect(service->SetTrayIcon(7, tray_id, CreateIcon(5, 6, 7, 8),
                                   &error_message),
              error_message) ||
      !Expect(ReadIconPixmapProperty(watcher.connection(), item_bus_name,
                                     &icon_bytes, &error_message),
              error_message) ||
      !Expect(icon_bytes == std::vector<uint8_t>({8, 5, 6, 7}),
              "updated SNI IconPixmap bytes were not observed")) {
    return false;
  }

  if (!Expect(service->SetTrayTooltip(7, tray_id, "Updated tooltip",
                                      &error_message),
              error_message)) {
    return false;
  }

  std::string menu_path;
  if (!Expect(ReadStringProperty(watcher.connection(), item_bus_name,
                                 "/StatusNotifierItem",
                                 "org.freedesktop.StatusNotifierItem", "Menu",
                                 &menu_path, &error_message),
              error_message) ||
      !Expect(menu_path == "/StatusNotifierItem/Menu",
              "unexpected SNI Menu property")) {
    return false;
  }

  if (!Expect(service->SetTrayMenu(7, tray_id, CreateMenuItems(), callback,
                                   &error_message),
              error_message)) {
    return false;
  }
  GVariantBuilder property_names;
  g_variant_builder_init(&property_names, G_VARIANT_TYPE("as"));
  GVariant* layout = nullptr;
  if (!Expect(CallDbus(watcher.connection(), item_bus_name, menu_path.c_str(),
                       "com.canonical.dbusmenu", "GetLayout",
                       g_variant_new("(ii@as)", 0, -1,
                                     g_variant_builder_end(&property_names)),
                       G_VARIANT_TYPE("(u(ia{sv}av))"), &layout,
                       &error_message),
              error_message)) {
    return false;
  }
  guint revision = 0;
  GVariant* root = nullptr;
  g_variant_get(layout, "(u@(ia{sv}av))", &revision, &root);
  auto root_id = gint{-1};
  GVariant* root_properties = nullptr;
  GVariant* children = nullptr;
  g_variant_get(root, "(i@a{sv}@av)", &root_id, &root_properties, &children);
  const auto child_count = g_variant_n_children(children);
  g_variant_unref(children);
  g_variant_unref(root_properties);
  g_variant_unref(root);
  g_variant_unref(layout);
  if (!Expect(revision >= 1, "unexpected dbusmenu revision") ||
      !Expect(root_id == 0, "unexpected dbusmenu root id") ||
      !Expect(child_count == 3, "unexpected dbusmenu child count")) {
    return false;
  }

  if (!Expect(CallDbus(watcher.connection(), item_bus_name,
                       "/StatusNotifierItem",
                       "org.freedesktop.StatusNotifierItem", "Activate",
                       g_variant_new("(ii)", 12, 34), nullptr, nullptr,
                       &error_message),
              error_message) ||
      !Expect(WaitUntil([&events]() { return !events.empty(); }),
              "activate event was not delivered") ||
      !Expect(events.back().type == kMuonBrowserTrayEventActivate,
              "unexpected activate event type") ||
      !Expect(events.back().x == 12 && events.back().y == 34,
              "unexpected activate event coordinates")) {
    return false;
  }

  if (!Expect(CallMenuEvent(watcher.connection(), item_bus_name, menu_path, 2,
                            &error_message),
              error_message) ||
      !Expect(WaitUntil([&events]() { return events.size() >= 2; }),
              "menu event was not delivered") ||
      !Expect(events.back().type == kMuonBrowserTrayEventMenu,
              "unexpected menu event type") ||
      !Expect(events.back().menu_id == "check",
              "unexpected menu event id") ||
      !Expect(events.back().checked, "checkbox menu event was not toggled")) {
    return false;
  }

  if (!Expect(service->RemoveTray(7, tray_id, &error_message),
              error_message) ||
      !Expect(WaitUntil([&]() {
                return CallDbus(watcher.connection(), item_bus_name,
                                "/StatusNotifierItem",
                                "org.freedesktop.StatusNotifierItem",
                                "Activate", g_variant_new("(ii)", 0, 0),
                                nullptr, nullptr, &error_message, true);
              }),
              "removed tray item still accepted D-Bus calls")) {
    return false;
  }

  std::string generated_id;
  options.id.clear();
  if (!Expect(service->CreateTray(7, options, CreateIcon(9, 10, 11, 12),
                                  callback, &generated_id, &error_message),
              error_message) ||
      !Expect(!generated_id.empty(), "generated tray id is empty")) {
    return false;
  }
  service->RemoveTraysForBrowser(7);
  if (!Expect(!service->RemoveTray(7, generated_id, &error_message),
              "browser cleanup did not remove the generated tray")) {
    return false;
  }

  return true;
}

bool TestLinuxTitleBarFollowingTrayIcons() {
  TestDbusSession session;
  if (!Expect(session.Start(), "failed to start private D-Bus session")) {
    return false;
  }

  std::string error_message;
  FakeStatusNotifierWatcher watcher;
  if (!Expect(watcher.Start(&error_message), error_message)) {
    return false;
  }

  auto service = CreateMuonBrowserTrayService("muon-test");
  auto callback = [](const MuonBrowserTrayEvent& event) { (void)event; };

  MuonBrowserTrayOptions fixed_options;
  fixed_options.id = "fixed";

  MuonBrowserTrayOptions follow_options;
  follow_options.id = "follow";
  follow_options.follow_title_bar_icon = true;

  std::string fixed_id;
  std::string follow_id;
  if (!Expect(service->CreateTray(7, fixed_options, CreateIcon(1, 2, 3, 4),
                                  callback, &fixed_id, &error_message),
              error_message) ||
      !Expect(service->CreateTray(7, follow_options, CreateIcon(5, 6, 7, 8),
                                  callback, &follow_id, &error_message),
              error_message) ||
      !Expect(WaitUntil([&watcher]() {
                return watcher.registered_items().size() >= 2;
              }),
              "watcher did not receive tray registrations")) {
    return false;
  }

  const auto fixed_bus_name = watcher.registered_items()[0];
  const auto follow_bus_name = watcher.registered_items()[1];
  std::vector<uint8_t> fixed_icon_bytes;
  std::vector<uint8_t> follow_icon_bytes;
  if (!Expect(ReadIconPixmapProperty(watcher.connection(), fixed_bus_name,
                                     &fixed_icon_bytes, &error_message),
              error_message) ||
      !Expect(ReadIconPixmapProperty(watcher.connection(), follow_bus_name,
                                     &follow_icon_bytes, &error_message),
              error_message) ||
      !Expect(fixed_icon_bytes == std::vector<uint8_t>({4, 1, 2, 3}),
              "fixed tray icon had unexpected initial bytes") ||
      !Expect(follow_icon_bytes == std::vector<uint8_t>({8, 5, 6, 7}),
              "following tray icon had unexpected initial bytes")) {
    return false;
  }

  service->SetFollowingTrayIconForBrowser(7, CreateIcon(9, 10, 11, 12));
  if (!Expect(ReadIconPixmapProperty(watcher.connection(), fixed_bus_name,
                                     &fixed_icon_bytes, &error_message),
              error_message) ||
      !Expect(ReadIconPixmapProperty(watcher.connection(), follow_bus_name,
                                     &follow_icon_bytes, &error_message),
              error_message) ||
      !Expect(fixed_icon_bytes == std::vector<uint8_t>({4, 1, 2, 3}),
              "fixed tray icon followed a title bar icon update") ||
      !Expect(follow_icon_bytes == std::vector<uint8_t>({12, 9, 10, 11}),
              "following tray icon did not observe a title bar icon update")) {
    return false;
  }

  if (!Expect(service->SetTrayIcon(7, follow_id, CreateIcon(21, 22, 23, 24),
                                   &error_message),
              error_message)) {
    return false;
  }
  service->SetFollowingTrayIconForBrowser(7, CreateIcon(31, 32, 33, 34));
  if (!Expect(ReadIconPixmapProperty(watcher.connection(), follow_bus_name,
                                     &follow_icon_bytes, &error_message),
              error_message) ||
      !Expect(follow_icon_bytes == std::vector<uint8_t>({24, 21, 22, 23}),
              "setTrayIcon should stop title bar icon following")) {
    return false;
  }

  return true;
}

}  // namespace

int main() {
  if (!TestTitleBarIconBitmapReuse() ||
      !TestLinuxStatusNotifierItemBackend() ||
      !TestLinuxTitleBarFollowingTrayIcons()) {
    return 1;
  }
  return 0;
}
