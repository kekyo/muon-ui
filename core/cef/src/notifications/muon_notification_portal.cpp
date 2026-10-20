/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "notifications/muon_notification_portal.h"
#include <gio/gio.h>
#include <array>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace muon_internal {
static constexpr char kPortal[] = "org.freedesktop.portal.Desktop";
static constexpr char kPortalPath[] = "/org/freedesktop/portal/desktop";
static constexpr char kNotification[] = "org.freedesktop.portal.Notification";
static constexpr char kApplicationXml[] = R"xml(<node>
<interface name="org.freedesktop.Application">
 <method name="Activate"><arg type="a{sv}" direction="in"/></method>
 <method name="ActivateAction">
  <arg type="s" direction="in"/><arg type="av" direction="in"/>
  <arg type="a{sv}" direction="in"/>
 </method>
</interface></node>)xml";
using Variant = std::unique_ptr<GVariant, decltype(&g_variant_unref)>;
using KeyFile = std::unique_ptr<GKeyFile, decltype(&g_key_file_unref)>;
using GlibString = std::unique_ptr<gchar, decltype(&g_free)>;
struct ObjectDeleter {
  void operator()(void* object) const { if (object) g_object_unref(object); }
};
template <typename T> using Object = std::unique_ptr<T, ObjectDeleter>;

struct PortalBackend {
  MuonPortalNotificationOptions options;
  Object<GDBusConnection> connection;
  cardio::primitives::mutex serial;
  MuonNotificationActivation activation;
  std::function<void(std::string)> application_activation;
  guint object_registration = 0;
  guint owner_subscription = 0;
  bool owns_name = false;
  bool stopped = false;
  std::string registered_owner;
  ~PortalBackend() {
    if (connection) {
      if (object_registration)
        g_dbus_connection_unregister_object(connection.get(), object_registration);
      if (owner_subscription)
        g_dbus_connection_signal_unsubscribe(connection.get(), owner_subscription);
      // This is a dedicated connection, never the process-wide GIO connection.
      // Closing releases the name without withdrawing persistent notifications.
      g_dbus_connection_close(connection.get(), nullptr, nullptr, nullptr);
    }
  }
};
static cardio::promise<void> Start(std::shared_ptr<PortalBackend> impl);

static cardio::promise<GVariant*> Call(GDBusConnection* connection,
    const char* destination, const char* path, const char* interface_name,
    const char* method, GVariant* parameters, const GVariantType* result_type) {
  co_return co_await cardio::gio::submit<GVariant*>(
      [=](GCancellable* cancel, GAsyncReadyCallback ready, gpointer data) {
        g_dbus_connection_call(connection, destination, path, interface_name,
            method, parameters, result_type, G_DBUS_CALL_FLAGS_NONE, -1,
            cancel, ready, data);
      }, [](GObject* source, GAsyncResult* result, GError** error) {
        return g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, error);
      });
}
static void RequireRunning(const PortalBackend& impl) {
  if (impl.stopped) throw std::runtime_error("Notification backend has stopped");
}
static void OwnerChanged(GDBusConnection*, const gchar*, const gchar*, const gchar*,
    const gchar*, GVariant* parameters, gpointer data) {
  const auto impl = static_cast<std::weak_ptr<PortalBackend>*>(data)->lock();
  if (!impl || impl->stopped ||
      !g_variant_is_of_type(parameters, G_VARIANT_TYPE("(sss)"))) return;
  const gchar* name = nullptr;
  const gchar* before = nullptr;
  const gchar* after = nullptr;
  g_variant_get(parameters, "(&s&s&s)", &name, &before, &after);
  if (*after) cardio::fire_and_forget(Start(impl));
}
static cardio::promise<KeyFile> ReadKeyFile(std::string relative) {
  auto directories = std::vector<std::filesystem::path>{g_get_user_data_dir()};
  for (auto entry = g_get_system_data_dirs(); *entry != nullptr; ++entry)
    directories.emplace_back(*entry);
  for (const auto& directory : directories) {
    const auto path = (directory / relative).string();
    auto file = Object<GFile>(g_file_new_for_path(path.c_str()));
    auto input = Object<GFileInputStream>{};
    try { input.reset(co_await cardio::gio::read(file.get())); }
    catch (const cardio::gio::gio_error& error) {
      if (error.domain() == G_IO_ERROR && error.code() == G_IO_ERROR_NOT_FOUND) continue;
      throw;
    }
    auto text = std::string{};
    auto buffer = std::array<std::byte, 4096>{};
    while (true) {
      const auto count = co_await cardio::gio::read(G_INPUT_STREAM(input.get()), buffer);
      if (!count) break;
      if (text.size() + count > 65536)
        throw std::runtime_error("Notification registration is too large");
      text.append(reinterpret_cast<const char*>(buffer.data()), count);
    }
    co_await cardio::gio::close(G_INPUT_STREAM(input.get()));
    auto key = KeyFile(g_key_file_new(), g_key_file_unref);
    if (!g_key_file_load_from_data(key.get(), text.data(), text.size(),
                                   G_KEY_FILE_NONE, nullptr))
      throw std::runtime_error("Invalid notification registration");
    co_return key;
  }
  throw std::runtime_error("Notification launch registration is missing");
}
static std::string KeyString(GKeyFile* key, const char* group, const char* name) {
  auto value = GlibString(g_key_file_get_string(key, group, name, nullptr), g_free);
  return value ? value.get() : "";
}
static bool DesktopCommandMatches(const std::string& command, const std::string& launcher) {
  gint count = 0;
  gchar** values = nullptr;
  if (!g_shell_parse_argv(command.c_str(), &count, &values, nullptr)) return false;
  auto arguments = std::unique_ptr<gchar*, decltype(&g_strfreev)>(values, g_strfreev);
  // The launcher uses env for literal '%' in its path because GIO checks the
  // executable before expanding Desktop Entry field codes.
  const auto offset = std::string_view(values[0]) == "/usr/bin/env" ? 1 : 0;
  if (count != offset + 1 && count != offset + 2) return false;
  if (count == offset + 2 && std::string_view(values[offset + 1]) != "--muon-launch-from=normal")
    return false;
  auto executable = std::string{};
  const auto quoted = std::string_view(values[offset]);
  for (size_t index = 0; index < quoted.size(); ++index) {
    if (quoted[index] == '%') {
      if (++index == quoted.size() || quoted[index] != '%') return false;
    }
    executable += quoted[index];
  }
  return executable == launcher;
}
static cardio::promise<void> VerifyRegistration(const MuonPortalNotificationOptions& options) {
  if (options.desktop_id.empty() || !g_dbus_is_name(options.desktop_id.c_str()) ||
      options.desktop_id.front() == ':' || options.launcher_path.empty() ||
      !std::filesystem::path(options.launcher_path).is_absolute())
    throw std::runtime_error("Notification launch identity is unavailable");
  auto desktop = std::move(co_await ReadKeyFile("applications/" + options.desktop_id + ".desktop"));
  if (KeyString(desktop.get(), "Desktop Entry", "X-muon-Launcher") != options.launcher_path ||
      KeyString(desktop.get(), "Desktop Entry", "Type") != "Application" ||
      g_key_file_get_boolean(desktop.get(), "Desktop Entry", "Hidden", nullptr) ||
      !DesktopCommandMatches(KeyString(desktop.get(), "Desktop Entry", "Exec"), options.launcher_path) ||
      !g_key_file_get_boolean(desktop.get(), "Desktop Entry", "DBusActivatable", nullptr))
    throw std::runtime_error("Desktop notification launch registration differs");
  auto service = std::move(co_await ReadKeyFile("dbus-1/services/" + options.desktop_id + ".service"));
  if (KeyString(service.get(), "D-BUS Service", "Name") != options.desktop_id ||
      KeyString(service.get(), "D-BUS Service", "X-muon-AppId") != options.application_id)
    throw std::runtime_error("D-Bus notification launch identity differs");
  const auto command = KeyString(service.get(), "D-BUS Service", "Exec");
  gint count = 0;
  gchar** arguments = nullptr;
  if (!g_shell_parse_argv(command.c_str(), &count, &arguments, nullptr))
    throw std::runtime_error("Invalid notification launch command");
  const auto matches = count == 2 && options.launcher_path == arguments[0] &&
      std::string_view(arguments[1]) == "--muon-notification-activation";
  g_strfreev(arguments);
  if (!matches) throw std::runtime_error("Notification launch command differs");
  auto launcher = Object<GFile>(g_file_new_for_path(options.launcher_path.c_str()));
  auto info = Object<GFileInfo>(co_await cardio::gio::query_info(launcher.get(),
      G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE,
      G_FILE_QUERY_INFO_NONE));
  if (g_file_info_get_file_type(info.get()) != G_FILE_TYPE_REGULAR ||
      !g_file_info_get_attribute_boolean(info.get(), G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE))
    throw std::runtime_error("Notification launcher is not executable");
}
static bool ValidToken(std::string_view token) {
  if (token.size() != 64) return false;
  for (const auto ch : token)
    if (!(ch >= '0' && ch <= '9') && !(ch >= 'a' && ch <= 'f')) return false;
  return true;
}
static std::string StartupContext(GVariant* platform) {
  const gchar* startup = "";
  if (!g_variant_lookup(platform, "desktop-startup-id", "&s", &startup))
    g_variant_lookup(platform, "activation-token", "&s", &startup);
  return startup;
}
static void Activated(GDBusConnection*, const gchar*, const gchar*, const gchar*,
    const gchar* method, GVariant* args, GDBusMethodInvocation* invocation, gpointer data) {
  const auto impl = static_cast<std::weak_ptr<PortalBackend>*>(data)->lock();
  if (!impl || impl->stopped || !impl->activation) {
    g_dbus_method_invocation_return_dbus_error(invocation,
        "org.freedesktop.DBus.Error.Failed", "Notification activation is stopping");
    return;
  }
  // Ordinary desktop activation has no notification event to synthesize.
  if (std::string_view(method) == "Activate") {
    auto platform = Variant(g_variant_get_child_value(args, 0), g_variant_unref);
    if (impl->application_activation)
      impl->application_activation(StartupContext(platform.get()));
    g_dbus_method_invocation_return_value(invocation, nullptr);
    return;
  }
  auto name = Variant(g_variant_get_child_value(args, 0), g_variant_unref);
  auto targets = Variant(g_variant_get_child_value(args, 1), g_variant_unref);
  if (std::string_view(g_variant_get_string(name.get(), nullptr)) != "notification" ||
      g_variant_n_children(targets.get()) != 1) {
    g_dbus_method_invocation_return_dbus_error(invocation,
        "org.freedesktop.DBus.Error.InvalidArgs", "Unknown notification action");
    return;
  }
  auto wrapped = Variant(g_variant_get_child_value(targets.get(), 0), g_variant_unref);
  auto target = Variant(g_variant_get_variant(wrapped.get()), g_variant_unref);
  if (!g_variant_is_of_type(target.get(), G_VARIANT_TYPE("(sbs)"))) {
    g_dbus_method_invocation_return_dbus_error(invocation,
        "org.freedesktop.DBus.Error.InvalidArgs", "Invalid notification action target");
    return;
  }
  const gchar* token = nullptr;
  const gchar* action = nullptr;
  gboolean has_action = FALSE;
  g_variant_get(target.get(), "(&sb&s)", &token, &has_action, &action);
  if (!ValidToken(token) || (!has_action && *action)) {
    g_dbus_method_invocation_return_dbus_error(invocation,
        "org.freedesktop.DBus.Error.InvalidArgs", "Invalid notification revision");
    return;
  }
  auto platform = Variant(g_variant_get_child_value(args, 2), g_variant_unref);
  try {
    impl->activation(token, has_action ? std::optional<std::string>{action} : std::nullopt,
                     StartupContext(platform.get()));
    g_dbus_method_invocation_return_value(invocation, nullptr);
  } catch (const std::exception& error) {
    g_dbus_method_invocation_return_dbus_error(invocation,
        "org.freedesktop.DBus.Error.Failed", error.what());
  }
}
static cardio::promise<void> Connect(std::shared_ptr<PortalBackend> impl) {
  if (impl->connection && g_dbus_connection_is_closed(impl->connection.get()))
    throw std::runtime_error("Notification session bus connection was closed");
  if (!impl->connection) {
    const auto* address = g_getenv("DBUS_SESSION_BUS_ADDRESS");
    if (!address || !*address) throw std::runtime_error("Session bus address is unavailable");
    impl->connection.reset(co_await cardio::gio::submit<GDBusConnection*>(
        [address](GCancellable* cancel, GAsyncReadyCallback ready, gpointer data) {
          g_dbus_connection_new_for_address(address,
              static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                                G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
              nullptr, cancel, ready, data);
        }, [](GObject*, GAsyncResult* result, GError** error) {
          return g_dbus_connection_new_for_address_finish(result, error);
        }));
    g_dbus_connection_set_exit_on_close(impl->connection.get(), FALSE);
  }
  RequireRunning(*impl);
  if (!impl->owner_subscription) {
    impl->owner_subscription = g_dbus_connection_signal_subscribe(impl->connection.get(),
        "org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged",
        "/org/freedesktop/DBus", kPortal, G_DBUS_SIGNAL_FLAGS_NONE, OwnerChanged,
        new std::weak_ptr<PortalBackend>(impl),
        [](gpointer value) { delete static_cast<std::weak_ptr<PortalBackend>*>(value); });
  }
  if (!impl->object_registration) {
    auto path = "/" + impl->options.desktop_id;
    for (auto& ch : path) {
      if (ch == '.') ch = '/';
      else if (ch == '-') ch = '_';
    }
    auto* info = g_dbus_node_info_new_for_xml(kApplicationXml, nullptr);
    const auto table = GDBusInterfaceVTable{Activated, nullptr, nullptr, {nullptr}};
    auto* weak = new std::weak_ptr<PortalBackend>(impl);
    impl->object_registration = g_dbus_connection_register_object(impl->connection.get(),
        path.c_str(), info->interfaces[0], &table, weak,
        [](gpointer value) { delete static_cast<std::weak_ptr<PortalBackend>*>(value); }, nullptr);
    g_dbus_node_info_unref(info);
    if (!impl->object_registration) {
      delete weak;
      throw std::runtime_error("Cannot export notification activation endpoint");
    }
  }
  if (!impl->owns_name) {
    // Export first: D-Bus can deliver a queued cold activation immediately
    // when RequestName succeeds, before this coroutine resumes.
    auto reply = Variant(co_await Call(impl->connection.get(), "org.freedesktop.DBus",
        "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
        g_variant_new("(su)", impl->options.desktop_id.c_str(), guint32{4}),
        G_VARIANT_TYPE("(u)")), g_variant_unref);
    guint32 result = 0;
    g_variant_get(reply.get(), "(u)", &result);
    if (result != 1 && result != 4)
      throw std::runtime_error("Notification activation name belongs to another process");
    impl->owns_name = true;
  }
}
static cardio::promise<void> Prepare(std::shared_ptr<PortalBackend> impl) {
  RequireRunning(*impl);
  co_await VerifyRegistration(impl->options);
  co_await Connect(impl);
  // Resolve each operation so a restarted portal is registered before any
  // Properties/Notification request on this otherwise private connection.
  auto started = Variant(co_await Call(impl->connection.get(), "org.freedesktop.DBus",
      "/org/freedesktop/DBus", "org.freedesktop.DBus", "StartServiceByName",
      g_variant_new("(su)", kPortal, guint32{0}), G_VARIANT_TYPE("(u)")), g_variant_unref);
  auto reply = Variant(co_await Call(impl->connection.get(), "org.freedesktop.DBus",
      "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetNameOwner",
      g_variant_new("(s)", kPortal), G_VARIANT_TYPE("(s)")), g_variant_unref);
  const gchar* owner = nullptr;
  g_variant_get(reply.get(), "(&s)", &owner);
  if (impl->registered_owner != owner) {
    try {
      auto registered = Variant(co_await Call(impl->connection.get(), owner, kPortalPath,
          "org.freedesktop.host.portal.Registry", "Register",
          g_variant_new("(s@a{sv})", impl->options.desktop_id.c_str(),
                        g_variant_new_array(G_VARIANT_TYPE("{sv}"), nullptr, 0)),
          G_VARIANT_TYPE_UNIT), g_variant_unref);
    } catch (const cardio::gio::gio_error& error) {
      if (error.domain() != G_DBUS_ERROR ||
          (error.code() != G_DBUS_ERROR_UNKNOWN_METHOD &&
           error.code() != G_DBUS_ERROR_UNKNOWN_INTERFACE)) throw;
      throw std::runtime_error("portal-host-registration-unavailable");
    }
    impl->registered_owner = owner;
  }
  RequireRunning(*impl);
}
static cardio::promise<MuonNotificationBackendCapabilities> Capabilities(
    std::shared_ptr<PortalBackend> impl) {
  auto lock = std::move(co_await impl->serial.lock());
  try {
    co_await Prepare(impl);
    auto reply = Variant(co_await Call(impl->connection.get(), impl->registered_owner.c_str(),
        kPortalPath, "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", kNotification, "version"), G_VARIANT_TYPE("(v)")), g_variant_unref);
    auto wrapped = Variant(g_variant_get_child_value(reply.get(), 0), g_variant_unref);
    auto version = Variant(g_variant_get_variant(wrapped.get()), g_variant_unref);
    if (!g_variant_is_of_type(version.get(), G_VARIANT_TYPE_UINT32) ||
        g_variant_get_uint32(version.get()) < 1)
      throw std::runtime_error("Portal notifications are unavailable");
    auto caps = MuonNotificationBackendCapabilities{true, true, true, ""};
    caps.launch = true;
    caps.launch_reason.clear();
    co_return caps;
  } catch (const std::exception& error) {
    auto caps = MuonNotificationBackendCapabilities{};
    caps.reason = caps.launch_reason = error.what();
    co_return caps;
  }
}
static cardio::promise<void> Start(std::shared_ptr<PortalBackend> impl) {
  auto lock = std::move(co_await impl->serial.lock());
  try { co_await Prepare(impl); }
  catch (const std::exception&) { /* Capability queries expose the diagnostic. */ }
}
static cardio::promise<MuonNotificationHandle> Post(
    std::shared_ptr<PortalBackend> impl, MuonNotificationPost post, std::string token) {
  auto lock = std::move(co_await impl->serial.lock());
  co_await Prepare(impl);
  if (!ValidToken(token)) throw std::runtime_error("Invalid notification revision");
  auto notification = GVariantBuilder{};
  g_variant_builder_init(&notification, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&notification, "{sv}", "title", g_variant_new_string(post.title.c_str()));
  g_variant_builder_add(&notification, "{sv}", "body", g_variant_new_string(post.body.c_str()));
  const auto* icon = impl->options.desktop_id.c_str();
  g_variant_builder_add(&notification, "{sv}", "icon",
      g_variant_new("(sv)", "themed", g_variant_new_strv(&icon, 1)));
  g_variant_builder_add(&notification, "{sv}", "default-action", g_variant_new_string("app.notification"));
  g_variant_builder_add(&notification, "{sv}", "default-action-target",
      g_variant_new("(sbs)", token.c_str(), FALSE, ""));
  auto buttons = GVariantBuilder{};
  g_variant_builder_init(&buttons, G_VARIANT_TYPE("aa{sv}"));
  for (const auto& action : post.actions) {
    auto button = GVariantBuilder{};
    g_variant_builder_init(&button, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&button, "{sv}", "label", g_variant_new_string(action.title.c_str()));
    g_variant_builder_add(&button, "{sv}", "action", g_variant_new_string("app.notification"));
    g_variant_builder_add(&button, "{sv}", "target",
        g_variant_new("(sbs)", token.c_str(), TRUE, action.id.c_str()));
    g_variant_builder_add_value(&buttons, g_variant_builder_end(&button));
  }
  g_variant_builder_add(&notification, "{sv}", "buttons", g_variant_builder_end(&buttons));
  auto reply = Variant(co_await Call(impl->connection.get(), impl->registered_owner.c_str(),
      kPortalPath, kNotification, "AddNotification",
      g_variant_new("(s@a{sv})", token.c_str(), g_variant_builder_end(&notification)),
      G_VARIANT_TYPE_UNIT), g_variant_unref);
  co_return MuonNotificationHandle{"portal", impl->options.desktop_id + "/" + token, 1};
}
static cardio::promise<void> Cancel(std::shared_ptr<PortalBackend> impl,
                                    MuonNotificationHandle handle) {
  const auto prefix = impl->options.desktop_id + "/";
  if (handle.backend != "portal" || !handle.owner.starts_with(prefix) ||
      !ValidToken(std::string_view(handle.owner).substr(prefix.size())) || handle.id != 1)
    co_return;
  auto lock = std::move(co_await impl->serial.lock());
  co_await Prepare(impl);
  auto reply = Variant(co_await Call(impl->connection.get(), impl->registered_owner.c_str(),
      kPortalPath, kNotification, "RemoveNotification",
      g_variant_new("(s)", handle.owner.c_str() + prefix.size()), G_VARIANT_TYPE_UNIT), g_variant_unref);
}
MuonNotificationBackend CreateMuonPortalNotificationBackend(
    MuonPortalNotificationOptions options) {
  auto impl = std::make_shared<PortalBackend>();
  impl->options = std::move(options);
  auto backend = MuonNotificationBackend{};
  backend.capabilities = [impl] { return Capabilities(impl); };
  backend.status = []() -> cardio::promise<MuonNotificationBackendStatus> {
    co_return MuonNotificationBackendStatus{std::nullopt, "app-status-unavailable"};
  };
  backend.post = [impl](MuonNotificationPost post, std::string token) {
    return Post(impl, std::move(post), std::move(token));
  };
  backend.cancel = [impl](MuonNotificationHandle handle) { return Cancel(impl, std::move(handle)); };
  backend.set_activation = [impl](MuonNotificationActivation callback) {
    impl->activation = std::move(callback);
    cardio::fire_and_forget(Start(impl));
  };
  backend.set_application_activation = [impl](std::function<void(std::string)> callback) {
    impl->application_activation = std::move(callback);
  };
  backend.stop = [impl] {
    impl->stopped = true;
    impl->activation = {};
    impl->application_activation = {};
    if (impl->connection) {
      if (impl->object_registration)
        g_dbus_connection_unregister_object(impl->connection.get(), impl->object_registration);
      if (impl->owner_subscription)
        g_dbus_connection_signal_unsubscribe(impl->connection.get(), impl->owner_subscription);
      impl->object_registration = 0;
      impl->owner_subscription = 0;
      g_dbus_connection_close(impl->connection.get(), nullptr, nullptr, nullptr);
    }
  };
  return backend;
}
}  // namespace muon_internal
