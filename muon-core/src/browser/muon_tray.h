/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#pragma once

#include "app/muon_app_storage.h"
#include "browser/muon_icon.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct MuonTitleBarIcon;

/**
 * muon browser tray menu item kind.
 */
enum MuonBrowserTrayMenuItemType {
  /** Normal command item that notifies JavaScript when selected. */
  kMuonBrowserTrayMenuItemCommand = 0,
  /** Separator item. */
  kMuonBrowserTrayMenuItemSeparator = 1,
  /** Checkbox command item. */
  kMuonBrowserTrayMenuItemCheckbox = 2,
  /** Radio command item. */
  kMuonBrowserTrayMenuItemRadio = 3,
};

/**
 * Parsed muon browser tray menu item.
 */
struct MuonBrowserTrayMenuItem {
  /** Item kind. */
  MuonBrowserTrayMenuItemType type = kMuonBrowserTrayMenuItemCommand;
  /** Application command id for command items. */
  std::string id;
  /** User-visible label for command items. */
  std::string label;
  /** Whether the command item is enabled. */
  bool enabled = true;
  /** Whether a checkbox or radio command item is checked. */
  bool checked = false;
};

/**
 * Parsed muon browser tray creation options.
 */
struct MuonBrowserTrayOptions {
  /** Optional tray id. Empty means the browser service should generate one. */
  std::string id;
  /** Optional tray icon asset path. Empty means follow the title bar icon. */
  std::string icon_path;
  /** Whether the tray icon follows the current title bar icon. */
  bool follow_title_bar_icon = false;
  /** Optional tooltip text. Empty means no tooltip. */
  std::string tooltip;
  /** Initial tray menu items. */
  std::vector<MuonBrowserTrayMenuItem> menu_items;
};

/**
 * Decoded tray icon bitmap.
 */
struct MuonBrowserTrayIcon {
  /** Immutable decoded PNG bitmap shared with native title/app icons. */
  MuonIconBitmapPtr bitmap;
};

/**
 * Native tray event kind delivered to JavaScript.
 */
enum MuonBrowserTrayEventType {
  /** Primary tray activation. */
  kMuonBrowserTrayEventActivate = 0,
  /** Secondary tray activation. */
  kMuonBrowserTrayEventSecondaryActivate = 1,
  /** Tray menu command selection. */
  kMuonBrowserTrayEventMenu = 2,
};

/**
 * Native tray event delivered by the platform backend.
 */
struct MuonBrowserTrayEvent {
  /** Event kind. */
  MuonBrowserTrayEventType type = kMuonBrowserTrayEventActivate;
  /** Browser-scoped tray id. */
  std::string tray_id;
  /** Menu command id for menu events. */
  std::string menu_id;
  /** Current checked state for checkbox and radio menu events. */
  bool checked = false;
  /** Screen X coordinate for activation events. */
  int x = 0;
  /** Screen Y coordinate for activation events. */
  int y = 0;
};

/**
 * Platform tray event callback.
 */
using MuonBrowserTrayEventCallback =
    std::function<void(const MuonBrowserTrayEvent&)>;

/**
 * System tray resource limits for the browser tray service.
 */
struct MuonBrowserTrayLimits {
  /** Maximum live tray items owned by one browser. */
  size_t max_per_browser = 16;
  /** Maximum live tray items owned by the whole process. */
  size_t max_global = 64;
};

/**
 * Test-only platform hook set for the browser tray service.
 *
 * @remarks When create_record is set, the service uses these hooks instead of
 * native D-Bus or Shell tray resources.
 */
struct MuonBrowserTrayPlatformHooks {
  /** Creates platform state for one tray record. */
  std::function<bool(int browser_id,
                     const std::string& tray_id,
                     std::string* error_message)>
      create_record;
  /** Destroys platform state for one tray record. */
  std::function<void(int browser_id, const std::string& tray_id)>
      destroy_record;
  /** Observes an icon update for one tray record. */
  std::function<void(int browser_id, const std::string& tray_id)> update_icon;
  /** Observes a tooltip update for one tray record. */
  std::function<void(int browser_id, const std::string& tray_id)>
      update_tooltip;
  /** Observes a menu update for one tray record. */
  std::function<void(int browser_id, const std::string& tray_id)> update_menu;
};

/**
 * Internal construction options for the browser tray service.
 */
struct MuonBrowserTrayServiceOptions {
  /** Desktop id used for Linux tray metadata. */
  std::string linux_desktop_id;
  /** Live tray resource limits. */
  MuonBrowserTrayLimits limits;
  /** Optional test-only platform hooks. */
  MuonBrowserTrayPlatformHooks platform_hooks;
};

/**
 * Platform-neutral system tray service.
 */
class MuonBrowserTrayService {
 public:
  virtual ~MuonBrowserTrayService() = default;

  /**
   * Creates a browser-owned tray item.
   *
   * @param browser_id Owning browser identifier.
   * @param options Parsed tray options.
   * @param icon Loaded tray icon.
   * @param callback Event callback for this tray item.
   * @param tray_id Receives the normalized or generated tray id.
   * @param error_message Receives a validation or platform error.
   * @return true when the tray item was created.
   */
  virtual bool CreateTray(int browser_id,
                          const MuonBrowserTrayOptions& options,
                          MuonBrowserTrayIcon icon,
                          MuonBrowserTrayEventCallback callback,
                          std::string* tray_id,
                          std::string* error_message) = 0;

  /**
   * Replaces menu items for an existing browser-owned tray item.
   */
  virtual bool SetTrayMenu(int browser_id,
                           const std::string& tray_id,
                           std::vector<MuonBrowserTrayMenuItem> menu_items,
                           MuonBrowserTrayEventCallback callback,
                           std::string* error_message) = 0;

  /**
   * Replaces the icon for an existing browser-owned tray item.
   */
  virtual bool SetTrayIcon(int browser_id,
                           const std::string& tray_id,
                           MuonBrowserTrayIcon icon,
                           std::string* error_message) = 0;

  /**
   * Replaces the icon for all title-bar-following tray items owned by a
   * browser.
   */
  virtual void SetFollowingTrayIconForBrowser(
      int browser_id,
      const MuonBrowserTrayIcon& icon) = 0;

  /**
   * Replaces the tooltip for an existing browser-owned tray item.
   */
  virtual bool SetTrayTooltip(int browser_id,
                              const std::string& tray_id,
                              const std::string& tooltip,
                              std::string* error_message) = 0;

  /**
   * Removes an existing browser-owned tray item.
   */
  virtual bool RemoveTray(int browser_id,
                          const std::string& tray_id,
                          std::string* error_message) = 0;

  /**
   * Removes all tray items owned by a browser.
   */
  virtual void RemoveTraysForBrowser(int browser_id) = 0;
};

/**
 * Returns whether a tray id or menu id is valid for public APIs.
 */
bool IsValidMuonBrowserTrayId(const std::string& id);

/**
 * Parses and validates a JSON array of muon browser tray menu items.
 *
 * @param items_json JSON array produced by the JavaScript wrapper.
 * @param items Receives normalized menu items on success.
 * @param error_message Receives a validation error on failure.
 * @return true when parsing succeeds.
 */
bool ParseMuonBrowserTrayMenuItemsJson(
    const std::string& items_json,
    std::vector<MuonBrowserTrayMenuItem>* items,
    std::string* error_message);

/**
 * Parses and validates muon browser tray creation options JSON.
 *
 * @param options_json JSON object produced by the JavaScript wrapper.
 * @param options Receives normalized options on success.
 * @param error_message Receives a validation error on failure.
 * @return true when parsing succeeds.
 */
bool ParseMuonBrowserTrayOptionsJson(const std::string& options_json,
                                     MuonBrowserTrayOptions* options,
                                     std::string* error_message);

/**
 * Creates a JSON detail object for a JavaScript tray CustomEvent.
 */
std::string CreateMuonBrowserTrayEventDetailJson(
    const std::string& token,
    const MuonBrowserTrayEvent& event);

/**
 * Loads a tray icon from muon app storage.
 *
 * @param storage Asset storage backing asset:// resources.
 * @param path Icon asset path. Accepts asset://main/... or main-relative paths.
 * @param icon Receives decoded tray icon data.
 * @param error_message Receives a validation or loading diagnostic.
 * @return true when a tray icon was loaded.
 */
bool LoadMuonBrowserTrayIconFromStorage(std::shared_ptr<MuonAppStorage> storage,
                                        const std::string& path,
                                        MuonBrowserTrayIcon* icon,
                                        std::string* error_message);

/**
 * Loads a tray icon from an already loaded title bar icon.
 *
 * @param title_bar_icon Title bar icon whose decoded bitmap is used for the
 * tray.
 * @param source Diagnostic source label used in error messages.
 * @param icon Receives decoded tray icon data.
 * @param error_message Receives a validation diagnostic.
 * @return true when a tray icon reused a valid decoded PNG bitmap.
 */
bool LoadMuonBrowserTrayIconFromTitleBarIcon(
    const MuonTitleBarIcon& title_bar_icon,
    const std::string& source,
    MuonBrowserTrayIcon* icon,
    std::string* error_message);

/**
 * Creates a platform tray service.
 *
 * @param linux_desktop_id Desktop id used for Linux tray metadata.
 * @return Platform tray service implementation.
 */
std::unique_ptr<MuonBrowserTrayService> CreateMuonBrowserTrayService(
    std::string linux_desktop_id);

/**
 * Creates a platform tray service with internal construction options.
 *
 * @param options Internal service options.
 * @return Platform tray service implementation.
 */
std::unique_ptr<MuonBrowserTrayService> CreateMuonBrowserTrayService(
    MuonBrowserTrayServiceOptions options);
