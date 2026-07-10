/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "browser/muon_tray.h"

#include "browser/muon_icon.h"
#include "browser/muon_title_bar.h"

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <gio/gio.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <shellapi.h>
#include <windows.h>
#endif

bool LoadMuonBrowserTrayIconFromStorage(std::shared_ptr<MuonAppStorage> storage,
                                        const std::string& path,
                                        MuonBrowserTrayIcon* icon,
                                        std::string* error_message) {
  if (icon == nullptr || error_message == nullptr) {
    return false;
  }
  error_message->clear();
  MuonTitleBarIcon title_bar_icon;
  if (!LoadMuonTitleBarIconFromStorage(std::move(storage), path,
                                       &title_bar_icon, error_message)) {
    return false;
  }
  return LoadMuonBrowserTrayIconFromTitleBarIcon(title_bar_icon, path, icon,
                                                error_message);
}

bool LoadMuonBrowserTrayIconFromTitleBarIcon(
    const MuonTitleBarIcon& title_bar_icon,
    const std::string& source,
    MuonBrowserTrayIcon* icon,
    std::string* error_message) {
  if (icon == nullptr || error_message == nullptr) {
    return false;
  }
  error_message->clear();
  const auto diagnostic_source = source.empty() ? "title bar icon" : source;
  if (!title_bar_icon.bitmap) {
    *error_message = "Tray icon must be backed by PNG data: " +
                     diagnostic_source;
    return false;
  }
  if (!IsMuonIconBitmapWithinLimits(*title_bar_icon.bitmap)) {
    *error_message =
        "Tray icon must have a valid decoded PNG bitmap: " + diagnostic_source;
    return false;
  }
  MuonBrowserTrayIcon loaded_icon;
  loaded_icon.bitmap = title_bar_icon.bitmap;
  *icon = std::move(loaded_icon);
  return true;
}

struct MuonBrowserTrayKey {
  int browser_id = 0;
  std::string tray_id;

  bool operator<(const MuonBrowserTrayKey& other) const {
    return std::tie(browser_id, tray_id) <
           std::tie(other.browser_id, other.tray_id);
  }
};

struct MuonBrowserTrayRecord {
  int browser_id = 0;
  std::string tray_id;
  std::string tooltip;
  bool follow_title_bar_icon = false;
  MuonBrowserTrayIcon icon;
  std::vector<MuonBrowserTrayMenuItem> menu_items;
  MuonBrowserTrayEventCallback callback;

#if defined(__linux__)
  GDBusConnection* linux_connection = nullptr;
  guint linux_freedesktop_watcher_id = 0;
  guint linux_kde_watcher_id = 0;
  guint linux_name_owner_id = 0;
  guint linux_sni_registration_id = 0;
  guint linux_kde_sni_registration_id = 0;
  guint linux_menu_registration_id = 0;
  guint linux_revision = 1;
  bool linux_name_acquired = false;
  std::string linux_bus_name;
  std::string linux_item_path = "/StatusNotifierItem";
  std::string linux_menu_path = "/StatusNotifierItem/Menu";
  std::set<std::string> linux_registered_watcher_owners;
#elif defined(_WIN32)
  UINT windows_notify_id = 0;
  HICON windows_icon = nullptr;
#endif
};

class MuonBrowserTrayServiceImpl final : public MuonBrowserTrayService {
 public:
  explicit MuonBrowserTrayServiceImpl(std::string linux_desktop_id);
  ~MuonBrowserTrayServiceImpl() override;

  bool CreateTray(int browser_id,
                  const MuonBrowserTrayOptions& options,
                  MuonBrowserTrayIcon icon,
                  MuonBrowserTrayEventCallback callback,
                  std::string* tray_id,
                  std::string* error_message) override;
  bool SetTrayMenu(int browser_id,
                   const std::string& tray_id,
                   std::vector<MuonBrowserTrayMenuItem> menu_items,
                   MuonBrowserTrayEventCallback callback,
                   std::string* error_message) override;
  bool SetTrayIcon(int browser_id,
                   const std::string& tray_id,
                   MuonBrowserTrayIcon icon,
                   std::string* error_message) override;
  void SetFollowingTrayIconForBrowser(
      int browser_id,
      const MuonBrowserTrayIcon& icon) override;
  bool SetTrayTooltip(int browser_id,
                      const std::string& tray_id,
                      const std::string& tooltip,
                      std::string* error_message) override;
  bool RemoveTray(int browser_id,
                  const std::string& tray_id,
                  std::string* error_message) override;
  void RemoveTraysForBrowser(int browser_id) override;

 private:
  std::map<MuonBrowserTrayKey, std::unique_ptr<MuonBrowserTrayRecord>> records_;
  uint64_t next_generated_id_ = 1;
  uint64_t next_platform_id_ = 1;
  std::string linux_desktop_id_;

  MuonBrowserTrayRecord* FindRecord(int browser_id,
                                    const std::string& tray_id);
  std::string CreateGeneratedTrayId(int browser_id);
  bool ActivateMenuItem(MuonBrowserTrayRecord* record, int menu_serial);
  void DispatchActivation(MuonBrowserTrayRecord* record,
                          MuonBrowserTrayEventType type,
                          int x,
                          int y);

  bool PlatformCreateRecord(MuonBrowserTrayRecord* record,
                            std::string* error_message);
  void PlatformDestroyRecord(MuonBrowserTrayRecord* record);
  void PlatformUpdateIcon(MuonBrowserTrayRecord* record);
  void PlatformUpdateTooltip(MuonBrowserTrayRecord* record);
  void PlatformUpdateMenu(MuonBrowserTrayRecord* record);

#if defined(__linux__)
  GDBusNodeInfo* linux_sni_node_info_ = nullptr;
  GDBusNodeInfo* linux_menu_node_info_ = nullptr;
  std::map<std::string, std::string> linux_watcher_owners_by_name_;

  MuonBrowserTrayRecord* FindLinuxRecord(GDBusConnection* connection);
  MuonBrowserTrayRecord* FindLinuxRecordByBusName(const char* name);
  GDBusInterfaceInfo* GetLinuxSniInterfaceInfo(const char* interface_name);
  void RegisterLinuxStatusNotifierItem(MuonBrowserTrayRecord* record,
                                       const std::string& watcher_name,
                                       const std::string& watcher_owner);
  void RegisterLinuxStatusNotifierItemWithKnownWatchers(
      MuonBrowserTrayRecord* record);
  void EmitLinuxSniSignal(MuonBrowserTrayRecord* record,
                          const char* signal_name,
                          GVariant* parameters);
  GVariant* GetLinuxSniProperty(MuonBrowserTrayRecord* record,
                                const char* property_name,
                                GError** error);
  void HandleLinuxSniMethodCall(GDBusConnection* connection,
                                const char* method_name,
                                GVariant* parameters,
                                GDBusMethodInvocation* invocation);
  GVariant* CreateLinuxMenuLayout(MuonBrowserTrayRecord* record,
                                  int menu_serial);
  GVariant* CreateLinuxMenuProperties(MuonBrowserTrayRecord* record,
                                      int menu_serial);
  GVariant* CreateLinuxMenuProperty(MuonBrowserTrayRecord* record,
                                    int menu_serial,
                                    const char* property_name);
  GVariant* GetLinuxMenuProperty(MuonBrowserTrayRecord* record,
                                 const char* property_name,
                                 GError** error);
  void HandleLinuxMenuMethodCall(GDBusConnection* connection,
                                 const char* method_name,
                                 GVariant* parameters,
                                 GDBusMethodInvocation* invocation);

  static void LinuxSniMethodCall(GDBusConnection* connection,
                                 const gchar* sender,
                                 const gchar* object_path,
                                 const gchar* interface_name,
                                 const gchar* method_name,
                                 GVariant* parameters,
                                 GDBusMethodInvocation* invocation,
                                 gpointer user_data);
  static GVariant* LinuxSniGetProperty(GDBusConnection* connection,
                                       const gchar* sender,
                                       const gchar* object_path,
                                       const gchar* interface_name,
                                       const gchar* property_name,
                                       GError** error,
                                       gpointer user_data);
  static void LinuxMenuMethodCall(GDBusConnection* connection,
                                  const gchar* sender,
                                  const gchar* object_path,
                                  const gchar* interface_name,
                                  const gchar* method_name,
                                  GVariant* parameters,
                                  GDBusMethodInvocation* invocation,
                                  gpointer user_data);
  static GVariant* LinuxMenuGetProperty(GDBusConnection* connection,
                                        const gchar* sender,
                                        const gchar* object_path,
                                        const gchar* interface_name,
                                        const gchar* property_name,
                                        GError** error,
                                        gpointer user_data);
  static void LinuxNameAcquired(GDBusConnection* connection,
                                const gchar* name,
                                gpointer user_data);
  static void LinuxNameLost(GDBusConnection* connection,
                            const gchar* name,
                            gpointer user_data);
  static void LinuxWatcherAppeared(GDBusConnection* connection,
                                   const gchar* name,
                                   const gchar* name_owner,
                                   gpointer user_data);
  static void LinuxWatcherVanished(GDBusConnection* connection,
                                   const gchar* name,
                                   gpointer user_data);
#elif defined(_WIN32)
  HWND windows_message_window_ = nullptr;
  UINT next_windows_notify_id_ = 1;

  bool EnsureWindowsMessageWindow(std::string* error_message);
  HICON CreateWindowsIcon(const MuonBrowserTrayIcon& icon);
  bool AddOrModifyWindowsIcon(MuonBrowserTrayRecord* record,
                              DWORD message,
                              std::string* error_message);
  void ShowWindowsMenu(MuonBrowserTrayRecord* record);
  LRESULT HandleWindowsMessage(HWND hwnd,
                               UINT message,
                               WPARAM wparam,
                               LPARAM lparam);

  static LRESULT CALLBACK WindowsWndProc(HWND hwnd,
                                         UINT message,
                                         WPARAM wparam,
                                         LPARAM lparam);
#endif
};

MuonBrowserTrayServiceImpl::MuonBrowserTrayServiceImpl(
    std::string linux_desktop_id)
    : linux_desktop_id_(std::move(linux_desktop_id)) {
#if defined(__linux__)
  static constexpr char kSniXml[] = R"XML(
<node>
  <interface name="org.freedesktop.StatusNotifierItem">
    <property name="Category" type="s" access="read"/>
    <property name="Id" type="s" access="read"/>
    <property name="Title" type="s" access="read"/>
    <property name="Status" type="s" access="read"/>
    <property name="WindowId" type="u" access="read"/>
    <property name="IconName" type="s" access="read"/>
    <property name="IconPixmap" type="a(iiay)" access="read"/>
    <property name="OverlayIconName" type="s" access="read"/>
    <property name="OverlayIconPixmap" type="a(iiay)" access="read"/>
    <property name="AttentionIconName" type="s" access="read"/>
    <property name="AttentionIconPixmap" type="a(iiay)" access="read"/>
    <property name="AttentionMovieName" type="s" access="read"/>
    <property name="ToolTip" type="(sa(iiay)ss)" access="read"/>
    <property name="ItemIsMenu" type="b" access="read"/>
    <property name="Menu" type="o" access="read"/>
    <method name="ContextMenu">
      <arg name="x" type="i" direction="in"/>
      <arg name="y" type="i" direction="in"/>
    </method>
    <method name="Activate">
      <arg name="x" type="i" direction="in"/>
      <arg name="y" type="i" direction="in"/>
    </method>
    <method name="SecondaryActivate">
      <arg name="x" type="i" direction="in"/>
      <arg name="y" type="i" direction="in"/>
    </method>
    <method name="Scroll">
      <arg name="delta" type="i" direction="in"/>
      <arg name="orientation" type="s" direction="in"/>
    </method>
    <signal name="NewTitle"/>
    <signal name="NewIcon"/>
    <signal name="NewAttentionIcon"/>
    <signal name="NewOverlayIcon"/>
    <signal name="NewToolTip"/>
    <signal name="NewStatus">
      <arg name="status" type="s"/>
    </signal>
  </interface>
  <interface name="org.kde.StatusNotifierItem">
    <property name="Category" type="s" access="read"/>
    <property name="Id" type="s" access="read"/>
    <property name="Title" type="s" access="read"/>
    <property name="Status" type="s" access="read"/>
    <property name="WindowId" type="u" access="read"/>
    <property name="IconName" type="s" access="read"/>
    <property name="IconPixmap" type="a(iiay)" access="read"/>
    <property name="OverlayIconName" type="s" access="read"/>
    <property name="OverlayIconPixmap" type="a(iiay)" access="read"/>
    <property name="AttentionIconName" type="s" access="read"/>
    <property name="AttentionIconPixmap" type="a(iiay)" access="read"/>
    <property name="AttentionMovieName" type="s" access="read"/>
    <property name="ToolTip" type="(sa(iiay)ss)" access="read"/>
    <property name="ItemIsMenu" type="b" access="read"/>
    <property name="Menu" type="o" access="read"/>
    <method name="ContextMenu">
      <arg name="x" type="i" direction="in"/>
      <arg name="y" type="i" direction="in"/>
    </method>
    <method name="Activate">
      <arg name="x" type="i" direction="in"/>
      <arg name="y" type="i" direction="in"/>
    </method>
    <method name="SecondaryActivate">
      <arg name="x" type="i" direction="in"/>
      <arg name="y" type="i" direction="in"/>
    </method>
    <method name="Scroll">
      <arg name="delta" type="i" direction="in"/>
      <arg name="orientation" type="s" direction="in"/>
    </method>
    <signal name="NewTitle"/>
    <signal name="NewIcon"/>
    <signal name="NewAttentionIcon"/>
    <signal name="NewOverlayIcon"/>
    <signal name="NewToolTip"/>
    <signal name="NewStatus">
      <arg name="status" type="s"/>
    </signal>
  </interface>
</node>
)XML";
  static constexpr char kMenuXml[] = R"XML(
<node>
  <interface name="com.canonical.dbusmenu">
    <method name="GetLayout">
      <arg name="parentId" type="i" direction="in"/>
      <arg name="recursionDepth" type="i" direction="in"/>
      <arg name="propertyNames" type="as" direction="in"/>
      <arg name="revision" type="u" direction="out"/>
      <arg name="layout" type="(ia{sv}av)" direction="out"/>
    </method>
    <method name="GetGroupProperties">
      <arg name="ids" type="ai" direction="in"/>
      <arg name="propertyNames" type="as" direction="in"/>
      <arg name="properties" type="a(ia{sv})" direction="out"/>
    </method>
    <method name="GetProperty">
      <arg name="id" type="i" direction="in"/>
      <arg name="name" type="s" direction="in"/>
      <arg name="value" type="v" direction="out"/>
    </method>
    <method name="Event">
      <arg name="id" type="i" direction="in"/>
      <arg name="eventId" type="s" direction="in"/>
      <arg name="data" type="v" direction="in"/>
      <arg name="timestamp" type="u" direction="in"/>
    </method>
    <method name="EventGroup">
      <arg name="events" type="a(isvu)" direction="in"/>
      <arg name="idErrors" type="ai" direction="out"/>
    </method>
    <method name="AboutToShow">
      <arg name="id" type="i" direction="in"/>
      <arg name="needUpdate" type="b" direction="out"/>
    </method>
    <method name="AboutToShowGroup">
      <arg name="ids" type="ai" direction="in"/>
      <arg name="updatesNeeded" type="ai" direction="out"/>
      <arg name="idErrors" type="ai" direction="out"/>
    </method>
    <property name="Version" type="u" access="read"/>
    <property name="TextDirection" type="s" access="read"/>
    <property name="Status" type="s" access="read"/>
    <property name="IconThemePath" type="as" access="read"/>
    <signal name="LayoutUpdated">
      <arg name="revision" type="u"/>
      <arg name="parent" type="i"/>
    </signal>
    <signal name="ItemsPropertiesUpdated">
      <arg name="updatedProps" type="a(ia{sv})"/>
      <arg name="removedProps" type="a(ias)"/>
    </signal>
    <signal name="ItemActivationRequested">
      <arg name="id" type="i"/>
      <arg name="timestamp" type="u"/>
    </signal>
  </interface>
</node>
)XML";
  linux_sni_node_info_ = g_dbus_node_info_new_for_xml(kSniXml, nullptr);
  linux_menu_node_info_ = g_dbus_node_info_new_for_xml(kMenuXml, nullptr);
#endif
}

MuonBrowserTrayServiceImpl::~MuonBrowserTrayServiceImpl() {
  while (!records_.empty()) {
    PlatformDestroyRecord(records_.begin()->second.get());
    records_.erase(records_.begin());
  }
#if defined(__linux__)
  if (linux_sni_node_info_ != nullptr) {
    g_dbus_node_info_unref(linux_sni_node_info_);
  }
  if (linux_menu_node_info_ != nullptr) {
    g_dbus_node_info_unref(linux_menu_node_info_);
  }
#elif defined(_WIN32)
  if (windows_message_window_ != nullptr) {
    DestroyWindow(windows_message_window_);
  }
#endif
}

MuonBrowserTrayRecord* MuonBrowserTrayServiceImpl::FindRecord(
    int browser_id,
    const std::string& tray_id) {
  const auto iterator = records_.find({browser_id, tray_id});
  return iterator == records_.end() ? nullptr : iterator->second.get();
}

std::string MuonBrowserTrayServiceImpl::CreateGeneratedTrayId(int browser_id) {
  for (;;) {
    const auto candidate = "tray-" + std::to_string(browser_id) + "-" +
                           std::to_string(next_generated_id_++);
    if (records_.find({browser_id, candidate}) == records_.end()) {
      return candidate;
    }
  }
}

bool MuonBrowserTrayServiceImpl::CreateTray(
    int browser_id,
    const MuonBrowserTrayOptions& options,
    MuonBrowserTrayIcon icon,
    MuonBrowserTrayEventCallback callback,
    std::string* tray_id,
    std::string* error_message) {
  if (tray_id == nullptr || error_message == nullptr) {
    return false;
  }
  auto normalized_id =
      options.id.empty() ? CreateGeneratedTrayId(browser_id) : options.id;
  if (!IsValidMuonBrowserTrayId(normalized_id)) {
    *error_message = "Tray id is invalid: " + normalized_id;
    return false;
  }
  if (records_.find({browser_id, normalized_id}) != records_.end()) {
    *error_message = "Tray id is duplicated: " + normalized_id;
    return false;
  }

  auto record = std::make_unique<MuonBrowserTrayRecord>();
  record->browser_id = browser_id;
  record->tray_id = normalized_id;
  record->tooltip = options.tooltip;
  record->follow_title_bar_icon = options.follow_title_bar_icon;
  record->icon = std::move(icon);
  record->menu_items = options.menu_items;
  record->callback = std::move(callback);

  if (!PlatformCreateRecord(record.get(), error_message)) {
    PlatformDestroyRecord(record.get());
    return false;
  }

  *tray_id = normalized_id;
  records_[{browser_id, normalized_id}] = std::move(record);
  return true;
}

bool MuonBrowserTrayServiceImpl::SetTrayMenu(
    int browser_id,
    const std::string& tray_id,
    std::vector<MuonBrowserTrayMenuItem> menu_items,
    MuonBrowserTrayEventCallback callback,
    std::string* error_message) {
  auto* record = FindRecord(browser_id, tray_id);
  if (record == nullptr) {
    if (error_message != nullptr) {
      *error_message = "Tray was not found: " + tray_id;
    }
    return false;
  }
  record->menu_items = std::move(menu_items);
  record->callback = std::move(callback);
  PlatformUpdateMenu(record);
  return true;
}

bool MuonBrowserTrayServiceImpl::SetTrayIcon(int browser_id,
                                             const std::string& tray_id,
                                             MuonBrowserTrayIcon icon,
                                             std::string* error_message) {
  auto* record = FindRecord(browser_id, tray_id);
  if (record == nullptr) {
    if (error_message != nullptr) {
      *error_message = "Tray was not found: " + tray_id;
    }
    return false;
  }
  record->icon = std::move(icon);
  record->follow_title_bar_icon = false;
  PlatformUpdateIcon(record);
  return true;
}

void MuonBrowserTrayServiceImpl::SetFollowingTrayIconForBrowser(
    int browser_id,
    const MuonBrowserTrayIcon& icon) {
  for (auto& entry : records_) {
    if (entry.first.browser_id != browser_id ||
        !entry.second->follow_title_bar_icon) {
      continue;
    }
    entry.second->icon = icon;
    PlatformUpdateIcon(entry.second.get());
  }
}

bool MuonBrowserTrayServiceImpl::SetTrayTooltip(
    int browser_id,
    const std::string& tray_id,
    const std::string& tooltip,
    std::string* error_message) {
  auto* record = FindRecord(browser_id, tray_id);
  if (record == nullptr) {
    if (error_message != nullptr) {
      *error_message = "Tray was not found: " + tray_id;
    }
    return false;
  }
  record->tooltip = tooltip;
  PlatformUpdateTooltip(record);
  return true;
}

bool MuonBrowserTrayServiceImpl::RemoveTray(int browser_id,
                                            const std::string& tray_id,
                                            std::string* error_message) {
  const auto iterator = records_.find({browser_id, tray_id});
  if (iterator == records_.end()) {
    if (error_message != nullptr) {
      *error_message = "Tray was not found: " + tray_id;
    }
    return false;
  }
  PlatformDestroyRecord(iterator->second.get());
  records_.erase(iterator);
  return true;
}

void MuonBrowserTrayServiceImpl::RemoveTraysForBrowser(int browser_id) {
  std::vector<std::string> tray_ids;
  for (const auto& entry : records_) {
    if (entry.first.browser_id == browser_id) {
      tray_ids.push_back(entry.first.tray_id);
    }
  }
  for (const auto& tray_id : tray_ids) {
    std::string ignored_error;
    (void)RemoveTray(browser_id, tray_id, &ignored_error);
  }
}

bool MuonBrowserTrayServiceImpl::ActivateMenuItem(
    MuonBrowserTrayRecord* record,
    int menu_serial) {
  if (record == nullptr || menu_serial <= 0 ||
      static_cast<size_t>(menu_serial) > record->menu_items.size()) {
    return false;
  }
  auto& item = record->menu_items[static_cast<size_t>(menu_serial - 1)];
  if (item.type == kMuonBrowserTrayMenuItemSeparator || !item.enabled) {
    return false;
  }
  if (item.type == kMuonBrowserTrayMenuItemCheckbox) {
    item.checked = !item.checked;
    PlatformUpdateMenu(record);
  } else if (item.type == kMuonBrowserTrayMenuItemRadio) {
    for (auto& candidate : record->menu_items) {
      if (candidate.type == kMuonBrowserTrayMenuItemRadio) {
        candidate.checked = false;
      }
    }
    item.checked = true;
    PlatformUpdateMenu(record);
  }

  MuonBrowserTrayEvent event;
  event.type = kMuonBrowserTrayEventMenu;
  event.tray_id = record->tray_id;
  event.menu_id = item.id;
  event.checked = item.checked;
  if (record->callback) {
    record->callback(event);
  }
  return true;
}

void MuonBrowserTrayServiceImpl::DispatchActivation(
    MuonBrowserTrayRecord* record,
    MuonBrowserTrayEventType type,
    int x,
    int y) {
  if (record == nullptr || !record->callback) {
    return;
  }
  MuonBrowserTrayEvent event;
  event.type = type;
  event.tray_id = record->tray_id;
  event.x = x;
  event.y = y;
  record->callback(event);
}

#if defined(__linux__)
static GVariant* CreateLinuxEmptyStringArray() {
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE("as"));
  return g_variant_builder_end(&builder);
}

struct LinuxStatusNotifierWatcherTarget {
  const char* bus_name;
  const char* interface_name;
};

static constexpr LinuxStatusNotifierWatcherTarget
    kLinuxStatusNotifierWatcherTargets[] = {
        {"org.kde.StatusNotifierWatcher", "org.kde.StatusNotifierWatcher"},
        {"org.freedesktop.StatusNotifierWatcher",
         "org.freedesktop.StatusNotifierWatcher"},
};

static const char* FindLinuxStatusNotifierWatcherInterface(
    const std::string& bus_name) {
  for (const auto& target : kLinuxStatusNotifierWatcherTargets) {
    if (bus_name == target.bus_name) {
      return target.interface_name;
    }
  }
  return nullptr;
}

static GVariant* CreateLinuxIconPixmap(const MuonBrowserTrayIcon& icon) {
  GVariantBuilder pixmaps;
  g_variant_builder_init(&pixmaps, G_VARIANT_TYPE("a(iiay)"));
  if (icon.bitmap && IsMuonIconBitmapWithinLimits(*icon.bitmap)) {
    const auto& bitmap = *icon.bitmap;
    GVariantBuilder bytes;
    g_variant_builder_init(&bytes, G_VARIANT_TYPE("ay"));
    for (auto offset = size_t{0}; offset < bitmap.rgba.size();
         offset += 4) {
      g_variant_builder_add(&bytes, "y", bitmap.rgba[offset + 3]);
      g_variant_builder_add(&bytes, "y", bitmap.rgba[offset]);
      g_variant_builder_add(&bytes, "y", bitmap.rgba[offset + 1]);
      g_variant_builder_add(&bytes, "y", bitmap.rgba[offset + 2]);
    }
    g_variant_builder_add(&pixmaps, "(iiay)", bitmap.pixel_width,
                          bitmap.pixel_height, &bytes);
  }
  return g_variant_builder_end(&pixmaps);
}

MuonBrowserTrayRecord* MuonBrowserTrayServiceImpl::FindLinuxRecord(
    GDBusConnection* connection) {
  for (const auto& entry : records_) {
    if (entry.second->linux_connection == connection) {
      return entry.second.get();
    }
  }
  return nullptr;
}

MuonBrowserTrayRecord* MuonBrowserTrayServiceImpl::FindLinuxRecordByBusName(
    const char* name) {
  if (name == nullptr) {
    return nullptr;
  }
  for (const auto& entry : records_) {
    if (entry.second->linux_bus_name == name) {
      return entry.second.get();
    }
  }
  return nullptr;
}

GDBusInterfaceInfo* MuonBrowserTrayServiceImpl::GetLinuxSniInterfaceInfo(
    const char* interface_name) {
  return linux_sni_node_info_ == nullptr
             ? nullptr
             : g_dbus_node_info_lookup_interface(linux_sni_node_info_,
                                                 interface_name);
}

bool MuonBrowserTrayServiceImpl::PlatformCreateRecord(
    MuonBrowserTrayRecord* record,
    std::string* error_message) {
  if (record == nullptr || error_message == nullptr) {
    return false;
  }
  if (linux_sni_node_info_ == nullptr || linux_menu_node_info_ == nullptr) {
    *error_message = "Linux tray D-Bus interfaces are unavailable";
    return false;
  }

  GError* error = nullptr;
  auto* address =
      g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, nullptr, &error);
  if (address == nullptr) {
    *error_message = error != nullptr ? error->message
                                      : "D-Bus session address is unavailable";
    if (error != nullptr) {
      g_error_free(error);
    }
    return false;
  }

  record->linux_connection = g_dbus_connection_new_for_address_sync(
      address,
      static_cast<GDBusConnectionFlags>(
          G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
          G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
      nullptr, nullptr, &error);
  g_free(address);
  if (record->linux_connection == nullptr) {
    *error_message = error != nullptr ? error->message
                                      : "D-Bus session connection failed";
    if (error != nullptr) {
      g_error_free(error);
    }
    return false;
  }
  g_dbus_connection_set_exit_on_close(record->linux_connection, FALSE);

  auto* freedesktop_info =
      GetLinuxSniInterfaceInfo("org.freedesktop.StatusNotifierItem");
  auto* kde_info = GetLinuxSniInterfaceInfo("org.kde.StatusNotifierItem");
  auto* menu_info = g_dbus_node_info_lookup_interface(
      linux_menu_node_info_, "com.canonical.dbusmenu");
  if (freedesktop_info == nullptr || kde_info == nullptr ||
      menu_info == nullptr) {
    *error_message = "Linux tray D-Bus introspection is unavailable";
    return false;
  }

  static GDBusInterfaceVTable linux_sni_vtable = {
      &MuonBrowserTrayServiceImpl::LinuxSniMethodCall,
      &MuonBrowserTrayServiceImpl::LinuxSniGetProperty,
      nullptr,
      {nullptr},
  };
  static GDBusInterfaceVTable linux_menu_vtable = {
      &MuonBrowserTrayServiceImpl::LinuxMenuMethodCall,
      &MuonBrowserTrayServiceImpl::LinuxMenuGetProperty,
      nullptr,
      {nullptr},
  };

  record->linux_sni_registration_id = g_dbus_connection_register_object(
      record->linux_connection, record->linux_item_path.c_str(),
      freedesktop_info, &linux_sni_vtable, this, nullptr, &error);
  if (record->linux_sni_registration_id == 0) {
    *error_message = error != nullptr ? error->message
                                      : "Failed to export StatusNotifierItem";
    if (error != nullptr) {
      g_error_free(error);
    }
    return false;
  }
  record->linux_kde_sni_registration_id = g_dbus_connection_register_object(
      record->linux_connection, record->linux_item_path.c_str(), kde_info,
      &linux_sni_vtable, this, nullptr, &error);
  if (record->linux_kde_sni_registration_id == 0) {
    *error_message = error != nullptr ? error->message
                                      : "Failed to export KDE StatusNotifierItem";
    if (error != nullptr) {
      g_error_free(error);
    }
    return false;
  }
  record->linux_menu_registration_id = g_dbus_connection_register_object(
      record->linux_connection, record->linux_menu_path.c_str(), menu_info,
      &linux_menu_vtable, this, nullptr, &error);
  if (record->linux_menu_registration_id == 0) {
    *error_message =
        error != nullptr ? error->message : "Failed to export tray menu";
    if (error != nullptr) {
      g_error_free(error);
    }
    return false;
  }

  record->linux_bus_name = "org.freedesktop.StatusNotifierItem-" +
                           std::to_string(static_cast<unsigned long>(getpid())) +
                           "-" + std::to_string(next_platform_id_++);
  record->linux_name_owner_id = g_bus_own_name_on_connection(
      record->linux_connection, record->linux_bus_name.c_str(),
      G_BUS_NAME_OWNER_FLAGS_NONE, &LinuxNameAcquired, &LinuxNameLost, this,
      nullptr);
  record->linux_kde_watcher_id = g_bus_watch_name_on_connection(
      record->linux_connection, kLinuxStatusNotifierWatcherTargets[0].bus_name,
      G_BUS_NAME_WATCHER_FLAGS_NONE, &LinuxWatcherAppeared,
      &LinuxWatcherVanished, this, nullptr);
  record->linux_freedesktop_watcher_id = g_bus_watch_name_on_connection(
      record->linux_connection, kLinuxStatusNotifierWatcherTargets[1].bus_name,
      G_BUS_NAME_WATCHER_FLAGS_NONE, &LinuxWatcherAppeared,
      &LinuxWatcherVanished, this, nullptr);
  return true;
}

void MuonBrowserTrayServiceImpl::PlatformDestroyRecord(
    MuonBrowserTrayRecord* record) {
  if (record == nullptr) {
    return;
  }
  if (record->linux_kde_watcher_id != 0) {
    g_bus_unwatch_name(record->linux_kde_watcher_id);
    record->linux_kde_watcher_id = 0;
  }
  if (record->linux_freedesktop_watcher_id != 0) {
    g_bus_unwatch_name(record->linux_freedesktop_watcher_id);
    record->linux_freedesktop_watcher_id = 0;
  }
  if (record->linux_name_owner_id != 0) {
    g_bus_unown_name(record->linux_name_owner_id);
    record->linux_name_owner_id = 0;
  }
  if (record->linux_menu_registration_id != 0 &&
      record->linux_connection != nullptr) {
    g_dbus_connection_unregister_object(record->linux_connection,
                                        record->linux_menu_registration_id);
    record->linux_menu_registration_id = 0;
  }
  if (record->linux_kde_sni_registration_id != 0 &&
      record->linux_connection != nullptr) {
    g_dbus_connection_unregister_object(record->linux_connection,
                                        record->linux_kde_sni_registration_id);
    record->linux_kde_sni_registration_id = 0;
  }
  if (record->linux_sni_registration_id != 0 &&
      record->linux_connection != nullptr) {
    g_dbus_connection_unregister_object(record->linux_connection,
                                        record->linux_sni_registration_id);
    record->linux_sni_registration_id = 0;
  }
  if (record->linux_connection != nullptr) {
    g_dbus_connection_close_sync(record->linux_connection, nullptr, nullptr);
    g_object_unref(record->linux_connection);
    record->linux_connection = nullptr;
  }
}

void MuonBrowserTrayServiceImpl::RegisterLinuxStatusNotifierItem(
    MuonBrowserTrayRecord* record,
    const std::string& watcher_name,
    const std::string& watcher_owner) {
  if (record == nullptr || record->linux_connection == nullptr ||
      !record->linux_name_acquired) {
    return;
  }
  const auto* watcher_interface =
      FindLinuxStatusNotifierWatcherInterface(watcher_name);
  if (watcher_interface == nullptr ||
      record->linux_registered_watcher_owners.count(watcher_owner) != 0) {
    return;
  }
  record->linux_registered_watcher_owners.insert(watcher_owner);
  g_dbus_connection_call(
      record->linux_connection, watcher_name.c_str(), "/StatusNotifierWatcher",
      watcher_interface, "RegisterStatusNotifierItem",
      g_variant_new("(s)", record->linux_bus_name.c_str()), nullptr,
      G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, nullptr, nullptr);
}

void MuonBrowserTrayServiceImpl::RegisterLinuxStatusNotifierItemWithKnownWatchers(
    MuonBrowserTrayRecord* record) {
  for (const auto& target : kLinuxStatusNotifierWatcherTargets) {
    const auto iterator =
        linux_watcher_owners_by_name_.find(target.bus_name);
    if (iterator != linux_watcher_owners_by_name_.end()) {
      RegisterLinuxStatusNotifierItem(record, iterator->first,
                                      iterator->second);
    }
  }
}

void MuonBrowserTrayServiceImpl::EmitLinuxSniSignal(
    MuonBrowserTrayRecord* record,
    const char* signal_name,
    GVariant* parameters) {
  if (record == nullptr || record->linux_connection == nullptr) {
    return;
  }
  g_dbus_connection_emit_signal(record->linux_connection, nullptr,
                                record->linux_item_path.c_str(),
                                "org.freedesktop.StatusNotifierItem",
                                signal_name, parameters, nullptr);
  g_dbus_connection_emit_signal(record->linux_connection, nullptr,
                                record->linux_item_path.c_str(),
                                "org.kde.StatusNotifierItem", signal_name,
                                parameters == nullptr
                                    ? nullptr
                                    : g_variant_ref_sink(parameters),
                                nullptr);
}

void MuonBrowserTrayServiceImpl::PlatformUpdateIcon(
    MuonBrowserTrayRecord* record) {
  EmitLinuxSniSignal(record, "NewIcon", nullptr);
}

void MuonBrowserTrayServiceImpl::PlatformUpdateTooltip(
    MuonBrowserTrayRecord* record) {
  EmitLinuxSniSignal(record, "NewToolTip", nullptr);
}

void MuonBrowserTrayServiceImpl::PlatformUpdateMenu(
    MuonBrowserTrayRecord* record) {
  if (record == nullptr || record->linux_connection == nullptr) {
    return;
  }
  record->linux_revision += 1;
  g_dbus_connection_emit_signal(
      record->linux_connection, nullptr, record->linux_menu_path.c_str(),
      "com.canonical.dbusmenu", "LayoutUpdated",
      g_variant_new("(ui)", record->linux_revision, 0), nullptr);
}

GVariant* MuonBrowserTrayServiceImpl::GetLinuxSniProperty(
    MuonBrowserTrayRecord* record,
    const char* property_name,
    GError** error) {
  if (record == nullptr || property_name == nullptr) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "StatusNotifierItem is unavailable");
    return nullptr;
  }
  const auto property = std::string(property_name);
  if (property == "Category") {
    return g_variant_new_string("ApplicationStatus");
  }
  if (property == "Id") {
    return g_variant_new_string(record->tray_id.c_str());
  }
  if (property == "Title") {
    return g_variant_new_string(linux_desktop_id_.c_str());
  }
  if (property == "Status") {
    return g_variant_new_string("Active");
  }
  if (property == "WindowId") {
    return g_variant_new_uint32(0);
  }
  if (property == "IconName" || property == "OverlayIconName" ||
      property == "AttentionIconName" || property == "AttentionMovieName") {
    return g_variant_new_string("");
  }
  if (property == "IconPixmap") {
    return CreateLinuxIconPixmap(record->icon);
  }
  if (property == "OverlayIconPixmap" || property == "AttentionIconPixmap") {
    MuonBrowserTrayIcon empty_icon;
    return CreateLinuxIconPixmap(empty_icon);
  }
  if (property == "ToolTip") {
    MuonBrowserTrayIcon empty_icon;
    return g_variant_new("(s@a(iiay)ss)", "", CreateLinuxIconPixmap(empty_icon),
                         record->tooltip.c_str(), "");
  }
  if (property == "ItemIsMenu") {
    return g_variant_new_boolean(FALSE);
  }
  if (property == "Menu") {
    return g_variant_new_object_path(record->linux_menu_path.c_str());
  }
  g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
              "Unknown StatusNotifierItem property: %s", property_name);
  return nullptr;
}

void MuonBrowserTrayServiceImpl::HandleLinuxSniMethodCall(
    GDBusConnection* connection,
    const char* method_name,
    GVariant* parameters,
    GDBusMethodInvocation* invocation) {
  auto* record = FindLinuxRecord(connection);
  if (record == nullptr || method_name == nullptr) {
    g_dbus_method_invocation_return_error(
        invocation, G_IO_ERROR, G_IO_ERROR_FAILED,
        "StatusNotifierItem is unavailable");
    return;
  }
  const auto method = std::string(method_name);
  if (method == "Activate" || method == "SecondaryActivate" ||
      method == "ContextMenu") {
    auto x = gint{0};
    auto y = gint{0};
    g_variant_get(parameters, "(ii)", &x, &y);
    g_dbus_method_invocation_return_value(invocation, nullptr);
    if (method == "Activate") {
      DispatchActivation(record, kMuonBrowserTrayEventActivate, x, y);
    } else if (method == "SecondaryActivate") {
      DispatchActivation(record, kMuonBrowserTrayEventSecondaryActivate, x, y);
    }
    return;
  }
  if (method == "Scroll") {
    g_dbus_method_invocation_return_value(invocation, nullptr);
    return;
  }
  g_dbus_method_invocation_return_error(
      invocation, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
      "Unknown StatusNotifierItem method: %s", method_name);
}

GVariant* MuonBrowserTrayServiceImpl::CreateLinuxMenuProperties(
    MuonBrowserTrayRecord* record,
    int menu_serial) {
  GVariantBuilder properties;
  g_variant_builder_init(&properties, G_VARIANT_TYPE("a{sv}"));
  if (record == nullptr) {
    return g_variant_builder_end(&properties);
  }
  if (menu_serial == 0) {
    g_variant_builder_add(&properties, "{sv}", "children-display",
                          g_variant_new_string("submenu"));
    g_variant_builder_add(&properties, "{sv}", "visible",
                          g_variant_new_boolean(TRUE));
    return g_variant_builder_end(&properties);
  }
  if (menu_serial < 0 ||
      static_cast<size_t>(menu_serial) > record->menu_items.size()) {
    return g_variant_builder_end(&properties);
  }
  const auto& item = record->menu_items[static_cast<size_t>(menu_serial - 1)];
  g_variant_builder_add(&properties, "{sv}", "visible",
                        g_variant_new_boolean(TRUE));
  if (item.type == kMuonBrowserTrayMenuItemSeparator) {
    g_variant_builder_add(&properties, "{sv}", "type",
                          g_variant_new_string("separator"));
    return g_variant_builder_end(&properties);
  }
  g_variant_builder_add(&properties, "{sv}", "type",
                        g_variant_new_string("standard"));
  g_variant_builder_add(&properties, "{sv}", "label",
                        g_variant_new_string(item.label.c_str()));
  g_variant_builder_add(&properties, "{sv}", "enabled",
                        g_variant_new_boolean(item.enabled));
  if (item.type == kMuonBrowserTrayMenuItemCheckbox ||
      item.type == kMuonBrowserTrayMenuItemRadio) {
    g_variant_builder_add(
        &properties, "{sv}", "toggle-type",
        g_variant_new_string(item.type == kMuonBrowserTrayMenuItemCheckbox
                                 ? "checkmark"
                                 : "radio"));
    g_variant_builder_add(&properties, "{sv}", "toggle-state",
                          g_variant_new_int32(item.checked ? 1 : 0));
  }
  return g_variant_builder_end(&properties);
}

GVariant* MuonBrowserTrayServiceImpl::CreateLinuxMenuLayout(
    MuonBrowserTrayRecord* record,
    int menu_serial) {
  GVariantBuilder children;
  g_variant_builder_init(&children, G_VARIANT_TYPE("av"));
  if (record != nullptr && menu_serial == 0) {
    for (auto index = size_t{0}; index < record->menu_items.size(); ++index) {
      auto* child = CreateLinuxMenuLayout(record, static_cast<int>(index + 1));
      g_variant_builder_add(&children, "v", child);
    }
  }
  return g_variant_new("(i@a{sv}av)", menu_serial,
                       CreateLinuxMenuProperties(record, menu_serial),
                       &children);
}

GVariant* MuonBrowserTrayServiceImpl::CreateLinuxMenuProperty(
    MuonBrowserTrayRecord* record,
    int menu_serial,
    const char* property_name) {
  if (record == nullptr || property_name == nullptr) {
    return nullptr;
  }
  auto* properties = CreateLinuxMenuProperties(record, menu_serial);
  GVariant* value = nullptr;
  g_variant_lookup(properties, property_name, "v", &value);
  g_variant_unref(properties);
  return value;
}

GVariant* MuonBrowserTrayServiceImpl::GetLinuxMenuProperty(
    MuonBrowserTrayRecord* record,
    const char* property_name,
    GError** error) {
  if (record == nullptr || property_name == nullptr) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Menu is unavailable");
    return nullptr;
  }
  const auto property = std::string(property_name);
  if (property == "Version") {
    return g_variant_new_uint32(3);
  }
  if (property == "TextDirection") {
    return g_variant_new_string("ltr");
  }
  if (property == "Status") {
    return g_variant_new_string("normal");
  }
  if (property == "IconThemePath") {
    return CreateLinuxEmptyStringArray();
  }
  g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
              "Unknown dbusmenu property: %s", property_name);
  return nullptr;
}

void MuonBrowserTrayServiceImpl::HandleLinuxMenuMethodCall(
    GDBusConnection* connection,
    const char* method_name,
    GVariant* parameters,
    GDBusMethodInvocation* invocation) {
  auto* record = FindLinuxRecord(connection);
  if (record == nullptr || method_name == nullptr) {
    g_dbus_method_invocation_return_error(invocation, G_IO_ERROR,
                                          G_IO_ERROR_FAILED,
                                          "Menu is unavailable");
    return;
  }
  const auto method = std::string(method_name);
  if (method == "GetLayout") {
    auto parent_id = gint{0};
    auto recursion_depth = gint{0};
    GVariant* property_names = nullptr;
    g_variant_get(parameters, "(ii@as)", &parent_id, &recursion_depth,
                  &property_names);
    if (property_names != nullptr) {
      g_variant_unref(property_names);
    }
    (void)recursion_depth;
    g_dbus_method_invocation_return_value(
        invocation,
        g_variant_new("(u@(ia{sv}av))", record->linux_revision,
                      CreateLinuxMenuLayout(record, parent_id)));
    return;
  }
  if (method == "GetGroupProperties") {
    GVariant* ids = nullptr;
    GVariant* property_names = nullptr;
    g_variant_get(parameters, "(@ai@as)", &ids, &property_names);
    if (property_names != nullptr) {
      g_variant_unref(property_names);
    }
    GVariantBuilder result;
    g_variant_builder_init(&result, G_VARIANT_TYPE("a(ia{sv})"));
    if (ids != nullptr) {
      GVariantIter iterator;
      gint id = 0;
      g_variant_iter_init(&iterator, ids);
      while (g_variant_iter_next(&iterator, "i", &id)) {
        g_variant_builder_add(&result, "(i@a{sv})", id,
                              CreateLinuxMenuProperties(record, id));
      }
      g_variant_unref(ids);
    }
    g_dbus_method_invocation_return_value(invocation,
                                          g_variant_new("(a(ia{sv}))",
                                                        &result));
    return;
  }
  if (method == "GetProperty") {
    auto id = gint{0};
    const char* name = nullptr;
    g_variant_get(parameters, "(i&s)", &id, &name);
    auto* property = CreateLinuxMenuProperty(record, id, name);
    if (property == nullptr) {
      g_dbus_method_invocation_return_error(
          invocation, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
          "Unknown dbusmenu item property: %s", name == nullptr ? "" : name);
      return;
    }
    auto* property_value = g_variant_ref_sink(property);
    auto* variant = g_variant_new_variant(property_value);
    g_variant_unref(property_value);
    g_dbus_method_invocation_return_value(invocation,
                                          g_variant_new("(@v)", variant));
    return;
  }
  if (method == "Event") {
    auto id = gint{0};
    const char* event_id = nullptr;
    GVariant* data = nullptr;
    guint timestamp = 0;
    g_variant_get(parameters, "(i&svu)", &id, &event_id, &data, &timestamp);
    if (data != nullptr) {
      g_variant_unref(data);
    }
    (void)timestamp;
    g_dbus_method_invocation_return_value(invocation, nullptr);
    if (event_id != nullptr && std::string(event_id) == "clicked") {
      (void)ActivateMenuItem(record, id);
    }
    return;
  }
  if (method == "EventGroup") {
    GVariant* events = nullptr;
    g_variant_get(parameters, "(@a(isvu))", &events);
    if (events != nullptr) {
      GVariantIter iterator;
      gint id = 0;
      const char* event_id = nullptr;
      GVariant* data = nullptr;
      guint timestamp = 0;
      g_variant_iter_init(&iterator, events);
      while (g_variant_iter_next(&iterator, "(i&svu)", &id, &event_id, &data,
                                 &timestamp)) {
        if (data != nullptr) {
          g_variant_unref(data);
        }
        (void)timestamp;
        if (event_id != nullptr && std::string(event_id) == "clicked") {
          (void)ActivateMenuItem(record, id);
        }
      }
      g_variant_unref(events);
    }
    GVariantBuilder errors;
    g_variant_builder_init(&errors, G_VARIANT_TYPE("ai"));
    g_dbus_method_invocation_return_value(invocation,
                                          g_variant_new("(ai)", &errors));
    return;
  }
  if (method == "AboutToShow") {
    auto id = gint{0};
    g_variant_get(parameters, "(i)", &id);
    (void)id;
    g_dbus_method_invocation_return_value(invocation,
                                          g_variant_new("(b)", FALSE));
    return;
  }
  if (method == "AboutToShowGroup") {
    GVariant* ids = nullptr;
    g_variant_get(parameters, "(@ai)", &ids);
    if (ids != nullptr) {
      g_variant_unref(ids);
    }
    GVariantBuilder updates;
    GVariantBuilder errors;
    g_variant_builder_init(&updates, G_VARIANT_TYPE("ai"));
    g_variant_builder_init(&errors, G_VARIANT_TYPE("ai"));
    g_dbus_method_invocation_return_value(
        invocation, g_variant_new("(aiai)", &updates, &errors));
    return;
  }
  g_dbus_method_invocation_return_error(
      invocation, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
      "Unknown dbusmenu method: %s", method_name);
}

void MuonBrowserTrayServiceImpl::LinuxSniMethodCall(
    GDBusConnection* connection,
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
  static_cast<MuonBrowserTrayServiceImpl*>(user_data)
      ->HandleLinuxSniMethodCall(connection, method_name, parameters,
                                 invocation);
}

GVariant* MuonBrowserTrayServiceImpl::LinuxSniGetProperty(
    GDBusConnection* connection,
    const gchar* sender,
    const gchar* object_path,
    const gchar* interface_name,
    const gchar* property_name,
    GError** error,
    gpointer user_data) {
  (void)sender;
  (void)object_path;
  (void)interface_name;
  auto* service = static_cast<MuonBrowserTrayServiceImpl*>(user_data);
  return service->GetLinuxSniProperty(service->FindLinuxRecord(connection),
                                      property_name, error);
}

void MuonBrowserTrayServiceImpl::LinuxMenuMethodCall(
    GDBusConnection* connection,
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
  static_cast<MuonBrowserTrayServiceImpl*>(user_data)
      ->HandleLinuxMenuMethodCall(connection, method_name, parameters,
                                  invocation);
}

GVariant* MuonBrowserTrayServiceImpl::LinuxMenuGetProperty(
    GDBusConnection* connection,
    const gchar* sender,
    const gchar* object_path,
    const gchar* interface_name,
    const gchar* property_name,
    GError** error,
    gpointer user_data) {
  (void)sender;
  (void)object_path;
  (void)interface_name;
  auto* service = static_cast<MuonBrowserTrayServiceImpl*>(user_data);
  return service->GetLinuxMenuProperty(service->FindLinuxRecord(connection),
                                       property_name, error);
}

void MuonBrowserTrayServiceImpl::LinuxNameAcquired(
    GDBusConnection* connection,
    const gchar* name,
    gpointer user_data) {
  (void)connection;
  auto* service = static_cast<MuonBrowserTrayServiceImpl*>(user_data);
  auto* record = service->FindLinuxRecordByBusName(name);
  if (record == nullptr) {
    return;
  }
  record->linux_name_acquired = true;
  service->RegisterLinuxStatusNotifierItemWithKnownWatchers(record);
}

void MuonBrowserTrayServiceImpl::LinuxNameLost(GDBusConnection* connection,
                                               const gchar* name,
                                               gpointer user_data) {
  (void)connection;
  auto* service = static_cast<MuonBrowserTrayServiceImpl*>(user_data);
  auto* record = service->FindLinuxRecordByBusName(name);
  if (record != nullptr) {
    record->linux_name_acquired = false;
    record->linux_registered_watcher_owners.clear();
  }
}

void MuonBrowserTrayServiceImpl::LinuxWatcherAppeared(
    GDBusConnection* connection,
    const gchar* name,
    const gchar* name_owner,
    gpointer user_data) {
  (void)connection;
  if (name == nullptr || name_owner == nullptr) {
    return;
  }
  auto* service = static_cast<MuonBrowserTrayServiceImpl*>(user_data);
  service->linux_watcher_owners_by_name_[name] = name_owner;
  for (const auto& entry : service->records_) {
    service->RegisterLinuxStatusNotifierItem(entry.second.get(), name,
                                             name_owner);
  }
}

void MuonBrowserTrayServiceImpl::LinuxWatcherVanished(
    GDBusConnection* connection,
    const gchar* name,
    gpointer user_data) {
  (void)connection;
  if (name == nullptr) {
    return;
  }
  auto* service = static_cast<MuonBrowserTrayServiceImpl*>(user_data);
  service->linux_watcher_owners_by_name_.erase(name);
}

#elif defined(_WIN32)
static constexpr UINT kMuonTrayCallbackMessage = WM_APP + 0x4d3;
static constexpr wchar_t kMuonTrayWindowClassName[] =
    L"MuonBrowserTrayMessageWindow";

static std::wstring ToWideString(const std::string& value) {
  if (value.empty()) {
    return {};
  }
  const auto length = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                          static_cast<int>(value.size()),
                                          nullptr, 0);
  if (length <= 0) {
    return {};
  }
  std::wstring wide(static_cast<size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.data(),
                      static_cast<int>(value.size()), wide.data(), length);
  return wide;
}

bool MuonBrowserTrayServiceImpl::EnsureWindowsMessageWindow(
    std::string* error_message) {
  if (windows_message_window_ != nullptr) {
    return true;
  }
  const auto instance = GetModuleHandleW(nullptr);
  WNDCLASSEXW window_class = {};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = &MuonBrowserTrayServiceImpl::WindowsWndProc;
  window_class.hInstance = instance;
  window_class.lpszClassName = kMuonTrayWindowClassName;
  RegisterClassExW(&window_class);
  windows_message_window_ = CreateWindowExW(
      0, kMuonTrayWindowClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
      instance, this);
  if (windows_message_window_ == nullptr) {
    if (error_message != nullptr) {
      *error_message = "Failed to create tray message window";
    }
    return false;
  }
  SetWindowLongPtrW(windows_message_window_, GWLP_USERDATA,
                    reinterpret_cast<LONG_PTR>(this));
  return true;
}

HICON MuonBrowserTrayServiceImpl::CreateWindowsIcon(
    const MuonBrowserTrayIcon& icon) {
  if (!icon.bitmap || !IsMuonIconBitmapWithinLimits(*icon.bitmap)) {
    return nullptr;
  }
  const auto& bitmap = *icon.bitmap;
  BITMAPV5HEADER header = {};
  header.bV5Size = sizeof(header);
  header.bV5Width = bitmap.pixel_width;
  header.bV5Height = -bitmap.pixel_height;
  header.bV5Planes = 1;
  header.bV5BitCount = 32;
  header.bV5Compression = BI_BITFIELDS;
  header.bV5RedMask = 0x00ff0000;
  header.bV5GreenMask = 0x0000ff00;
  header.bV5BlueMask = 0x000000ff;
  header.bV5AlphaMask = 0xff000000;

  void* bits = nullptr;
  auto* screen = GetDC(nullptr);
  auto color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header),
                                DIB_RGB_COLORS, &bits, nullptr, 0);
  ReleaseDC(nullptr, screen);
  if (color == nullptr || bits == nullptr) {
    return nullptr;
  }

  auto* target = static_cast<uint8_t*>(bits);
  for (auto offset = size_t{0}; offset < bitmap.rgba.size(); offset += 4) {
    target[offset] = bitmap.rgba[offset + 2];
    target[offset + 1] = bitmap.rgba[offset + 1];
    target[offset + 2] = bitmap.rgba[offset];
    target[offset + 3] = bitmap.rgba[offset + 3];
  }

  auto mask =
      CreateBitmap(bitmap.pixel_width, bitmap.pixel_height, 1, 1, nullptr);
  ICONINFO info = {};
  info.fIcon = TRUE;
  info.hbmColor = color;
  info.hbmMask = mask;
  auto handle = CreateIconIndirect(&info);
  DeleteObject(color);
  if (mask != nullptr) {
    DeleteObject(mask);
  }
  return handle;
}

bool MuonBrowserTrayServiceImpl::AddOrModifyWindowsIcon(
    MuonBrowserTrayRecord* record,
    DWORD message,
    std::string* error_message) {
  if (record == nullptr || !EnsureWindowsMessageWindow(error_message)) {
    return false;
  }
  auto next_icon = CreateWindowsIcon(record->icon);
  if (next_icon == nullptr) {
    if (error_message != nullptr) {
      *error_message = "Failed to create Windows tray icon";
    }
    return false;
  }

  NOTIFYICONDATAW data = {};
  data.cbSize = sizeof(data);
  data.hWnd = windows_message_window_;
  data.uID = record->windows_notify_id;
  data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
  data.uCallbackMessage = kMuonTrayCallbackMessage;
  data.hIcon = next_icon;
  const auto tooltip = ToWideString(record->tooltip);
  if (!tooltip.empty()) {
    wcsncpy_s(data.szTip, tooltip.c_str(), _TRUNCATE);
  }
  if (!Shell_NotifyIconW(message, &data)) {
    DestroyIcon(next_icon);
    if (error_message != nullptr) {
      *error_message = "Shell_NotifyIconW failed";
    }
    return false;
  }
  if (message == NIM_ADD) {
    data.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &data);
  }
  if (record->windows_icon != nullptr) {
    DestroyIcon(record->windows_icon);
  }
  record->windows_icon = next_icon;
  return true;
}

bool MuonBrowserTrayServiceImpl::PlatformCreateRecord(
    MuonBrowserTrayRecord* record,
    std::string* error_message) {
  if (record == nullptr) {
    return false;
  }
  record->windows_notify_id = next_windows_notify_id_++;
  return AddOrModifyWindowsIcon(record, NIM_ADD, error_message);
}

void MuonBrowserTrayServiceImpl::PlatformDestroyRecord(
    MuonBrowserTrayRecord* record) {
  if (record == nullptr || windows_message_window_ == nullptr) {
    return;
  }
  NOTIFYICONDATAW data = {};
  data.cbSize = sizeof(data);
  data.hWnd = windows_message_window_;
  data.uID = record->windows_notify_id;
  Shell_NotifyIconW(NIM_DELETE, &data);
  if (record->windows_icon != nullptr) {
    DestroyIcon(record->windows_icon);
    record->windows_icon = nullptr;
  }
}

void MuonBrowserTrayServiceImpl::PlatformUpdateIcon(
    MuonBrowserTrayRecord* record) {
  std::string ignored_error;
  (void)AddOrModifyWindowsIcon(record, NIM_MODIFY, &ignored_error);
}

void MuonBrowserTrayServiceImpl::PlatformUpdateTooltip(
    MuonBrowserTrayRecord* record) {
  if (record == nullptr || windows_message_window_ == nullptr) {
    return;
  }
  NOTIFYICONDATAW data = {};
  data.cbSize = sizeof(data);
  data.hWnd = windows_message_window_;
  data.uID = record->windows_notify_id;
  data.uFlags = NIF_TIP;
  const auto tooltip = ToWideString(record->tooltip);
  if (!tooltip.empty()) {
    wcsncpy_s(data.szTip, tooltip.c_str(), _TRUNCATE);
  }
  Shell_NotifyIconW(NIM_MODIFY, &data);
}

void MuonBrowserTrayServiceImpl::PlatformUpdateMenu(
    MuonBrowserTrayRecord* record) {
  (void)record;
}

void MuonBrowserTrayServiceImpl::ShowWindowsMenu(
    MuonBrowserTrayRecord* record) {
  if (record == nullptr || windows_message_window_ == nullptr) {
    return;
  }
  auto menu = CreatePopupMenu();
  if (menu == nullptr) {
    return;
  }
  std::map<UINT, int> command_ids;
  auto command_id = UINT{1000};
  for (auto index = size_t{0}; index < record->menu_items.size(); ++index) {
    const auto& item = record->menu_items[index];
    if (item.type == kMuonBrowserTrayMenuItemSeparator) {
      AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
      continue;
    }
    auto flags = MF_STRING;
    if (!item.enabled) {
      flags |= MF_GRAYED;
    }
    if ((item.type == kMuonBrowserTrayMenuItemCheckbox ||
         item.type == kMuonBrowserTrayMenuItemRadio) &&
        item.checked) {
      flags |= MF_CHECKED;
    }
    const auto label = ToWideString(item.label);
    AppendMenuW(menu, flags, command_id, label.c_str());
    command_ids[command_id] = static_cast<int>(index + 1);
    command_id += 1;
  }
  POINT point = {};
  GetCursorPos(&point);
  SetForegroundWindow(windows_message_window_);
  const auto selected = TrackPopupMenuEx(
      menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, point.x, point.y,
      windows_message_window_, nullptr);
  DestroyMenu(menu);
  const auto iterator = command_ids.find(static_cast<UINT>(selected));
  if (iterator != command_ids.end()) {
    (void)ActivateMenuItem(record, iterator->second);
  }
}

LRESULT MuonBrowserTrayServiceImpl::HandleWindowsMessage(HWND hwnd,
                                                         UINT message,
                                                         WPARAM wparam,
                                                         LPARAM lparam) {
  (void)hwnd;
  if (message != kMuonTrayCallbackMessage) {
    return DefWindowProcW(hwnd, message, wparam, lparam);
  }
  MuonBrowserTrayRecord* record = nullptr;
  for (const auto& entry : records_) {
    if (entry.second->windows_notify_id == static_cast<UINT>(wparam)) {
      record = entry.second.get();
      break;
    }
  }
  if (record == nullptr) {
    return 0;
  }
  const auto event = LOWORD(lparam);
  if (event == WM_CONTEXTMENU || event == WM_RBUTTONUP) {
    ShowWindowsMenu(record);
    return 0;
  }
  POINT point = {};
  GetCursorPos(&point);
  if (event == NIN_SELECT || event == WM_LBUTTONUP ||
      event == WM_LBUTTONDBLCLK) {
    DispatchActivation(record, kMuonBrowserTrayEventActivate, point.x,
                       point.y);
    return 0;
  }
  if (event == WM_MBUTTONUP) {
    DispatchActivation(record, kMuonBrowserTrayEventSecondaryActivate, point.x,
                       point.y);
    return 0;
  }
  return 0;
}

LRESULT CALLBACK MuonBrowserTrayServiceImpl::WindowsWndProc(HWND hwnd,
                                                            UINT message,
                                                            WPARAM wparam,
                                                            LPARAM lparam) {
  auto* service = reinterpret_cast<MuonBrowserTrayServiceImpl*>(
      GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    service = static_cast<MuonBrowserTrayServiceImpl*>(create->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(service));
  }
  return service == nullptr
             ? DefWindowProcW(hwnd, message, wparam, lparam)
             : service->HandleWindowsMessage(hwnd, message, wparam, lparam);
}

#else
bool MuonBrowserTrayServiceImpl::PlatformCreateRecord(
    MuonBrowserTrayRecord* record,
    std::string* error_message) {
  (void)record;
  if (error_message != nullptr) {
    *error_message = "System tray is not supported on this platform";
  }
  return false;
}

void MuonBrowserTrayServiceImpl::PlatformDestroyRecord(
    MuonBrowserTrayRecord* record) {
  (void)record;
}

void MuonBrowserTrayServiceImpl::PlatformUpdateIcon(
    MuonBrowserTrayRecord* record) {
  (void)record;
}

void MuonBrowserTrayServiceImpl::PlatformUpdateTooltip(
    MuonBrowserTrayRecord* record) {
  (void)record;
}

void MuonBrowserTrayServiceImpl::PlatformUpdateMenu(
    MuonBrowserTrayRecord* record) {
  (void)record;
}
#endif

std::unique_ptr<MuonBrowserTrayService> CreateMuonBrowserTrayService(
    std::string linux_desktop_id) {
  return std::make_unique<MuonBrowserTrayServiceImpl>(
      std::move(linux_desktop_id));
}
