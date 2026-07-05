/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#pragma once

#include "app/muon_app_storage.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

/**
 * Muon browser tray menu item kind.
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
 * Parsed Muon browser tray menu item.
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
 * Parsed Muon browser tray creation options.
 */
struct MuonBrowserTrayOptions {
  /** Optional tray id. Empty means the browser service should generate one. */
  std::string id;
  /** Required tray icon asset path. */
  std::string icon_path;
  /** Optional tooltip text. Empty means no tooltip. */
  std::string tooltip;
  /** Initial tray menu items. */
  std::vector<MuonBrowserTrayMenuItem> menu_items;
};

/**
 * Decoded tray icon bitmap.
 */
struct MuonBrowserTrayIcon {
  /** Source PNG bytes loaded from Muon app storage. */
  std::vector<uint8_t> png_data;
  /** RGBA bitmap data decoded from the PNG. */
  std::vector<uint8_t> rgba;
  /** Bitmap width in physical pixels. */
  int pixel_width = 0;
  /** Bitmap height in physical pixels. */
  int pixel_height = 0;
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
 * Parses and validates a JSON array of Muon browser tray menu items.
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
 * Parses and validates Muon browser tray creation options JSON.
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
 * Loads a tray icon from Muon app storage.
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
 * Creates a platform tray service.
 *
 * @param linux_desktop_id Desktop id used for Linux tray metadata.
 * @return Platform tray service implementation.
 */
std::unique_ptr<MuonBrowserTrayService> CreateMuonBrowserTrayService(
    std::string linux_desktop_id);
