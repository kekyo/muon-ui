/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

/**
 * Fixed byte size reserved in muon-core for an embedded muon.json payload.
 */
inline constexpr size_t kMuonEmbeddedConfigSlotSize = 64 * 1024;

/**
 * Unified log severity level.
 */
enum MuonLogLevel : uint32_t {
  /** Debug diagnostic output. */
  kMuonLogLevelDebug = 0,
  /** Informational output. */
  kMuonLogLevelInfo = 1,
  /** Warning output. */
  kMuonLogLevelWarning = 2,
  /** Error output. */
  kMuonLogLevelError = 3,
  /** Fatal error output. */
  kMuonLogLevelFatal = 4,
  /** Logging disabled. */
  kMuonLogLevelOff = 5,
};

/**
 * Unified log source.
 */
enum MuonLogSource : uint32_t {
  /** Muon host internals. */
  kMuonLogSourceMuon = 0,
  /** CEF and Chromium internals. */
  kMuonLogSourceCef = 1,
  /** JavaScript console output. */
  kMuonLogSourceConsole = 2,
  /** Native plugin output. */
  kMuonLogSourcePlugin = 3,
};

/**
 * Unified log sink type.
 */
enum MuonLogOutputType : uint32_t {
  /** Standard output stream. */
  kMuonLogOutputStdout = 0,
  /** Standard error stream. */
  kMuonLogOutputStderr = 1,
  /** Windows cdp output. */
  kMuonLogOutputDebug = 2,
  /** Windows event log. */
  kMuonLogOutputEventLog = 3,
  /** POSIX syslog. */
  kMuonLogOutputSyslog = 4,
  /** Append to a local file. */
  kMuonLogOutputFile = 5,
};

/**
 * Single unified log sink configuration.
 */
struct MuonLogOutputConfig {
  /** Selected sink type. */
  MuonLogOutputType type = kMuonLogOutputStderr;
  /**
   * File path used only when type is file. Relative paths are resolved from the
   * containing config file for explicit muon.json values.
   */
  std::filesystem::path path;
};

/**
 * Log section from muon.json.
 */
struct MuonLogConfig {
  /** Global baseline level. */
  MuonLogLevel level = kMuonLogLevelInfo;
  /** Single output sink. */
  MuonLogOutputConfig output;
  /** Muon host source level. */
  MuonLogLevel muon = kMuonLogLevelInfo;
  /** CEF source level. */
  MuonLogLevel cef = kMuonLogLevelWarning;
  /** JavaScript console source level. */
  MuonLogLevel console = kMuonLogLevelDebug;
  /** Native plugin source level. */
  MuonLogLevel plugin = kMuonLogLevelInfo;
};

/**
 * Modifier flags for a configured keyboard shortcut.
 */
enum MuonKeyboardShortcutModifier : uint32_t {
  /**
   * Shift key modifier.
   */
  kMuonShortcutModifierShift = 1 << 0,
  /**
   * Control key modifier.
   */
  kMuonShortcutModifierControl = 1 << 1,
  /**
   * Alt key modifier.
   */
  kMuonShortcutModifierAlt = 1 << 2,
  /**
   * Meta, command, or super key modifier.
   */
  kMuonShortcutModifierMeta = 1 << 3,
};

/**
 * Keyboard shortcut parsed from muon.json.
 */
struct MuonKeyboardShortcut {
  /**
   * Whether this shortcut is configured.
   */
  bool enabled = false;
  /**
   * Required modifier flags from MuonKeyboardShortcutModifier.
   */
  uint32_t modifiers = 0;
  /**
   * Windows virtual key code used by CEF keyboard events.
   */
  int windows_key_code = 0;
  /**
   * Whether a physical Shift modifier is accepted in addition to modifiers.
   */
  bool accepts_shift_variant = false;
};

/**
 * Browser plugin API exposure section from muon.json.
 */
struct MuonBrowserPluginConfig {
  /**
   * Glob patterns that allow Muon plugin APIs for full page URLs.
   */
  std::vector<std::string> allow = {"asset://main/**"};
};

/**
 * Initial main browser window state from muon.json.
 */
enum MuonBrowserInitialWindowState : uint32_t {
  /** Show the window normally. */
  kMuonBrowserInitialWindowStateNormal = 0,
  /** Hide the window after creation. */
  kMuonBrowserInitialWindowStateHidden = 1,
  /** Request minimized window state. */
  kMuonBrowserInitialWindowStateMinimized = 2,
  /** Request maximized window state. */
  kMuonBrowserInitialWindowStateMaximized = 3,
  /** Request fullscreen window state. */
  kMuonBrowserInitialWindowStateFullscreen = 4,
};

/**
 * Browser background color mode from muon.json.
 */
enum MuonBrowserBackgroundColorMode : uint32_t {
  /** Follow the operating system color scheme when available. */
  kMuonBrowserBackgroundColorSystem = 0,
  /** Use an explicit RGB background color. */
  kMuonBrowserBackgroundColorRgb = 1,
};

/**
 * Browser background color configuration from muon.json.
 */
struct MuonBrowserBackgroundColorConfig {
  /**
   * Background color interpretation mode.
   */
  MuonBrowserBackgroundColorMode mode = kMuonBrowserBackgroundColorSystem;
  /**
   * Red component for explicit RGB mode.
   */
  uint8_t red = 0;
  /**
   * Green component for explicit RGB mode.
   */
  uint8_t green = 0;
  /**
   * Blue component for explicit RGB mode.
   */
  uint8_t blue = 0;
};

/**
 * Browser section from muon.json.
 */
struct MuonBrowserConfig {
  /**
   * Initial URL loaded by the main browser.
   */
  std::string start_page = "asset://main/index.html";
  /**
   * CEF profile directory path. Relative paths are resolved from the muon
   * containing config file for explicit muon.json values. When omitted,
   * --muon-launch-from selects the default profile location.
   */
  std::filesystem::path profile = "./.profile";
  /**
   * Initial state requested for the main browser window.
   */
  MuonBrowserInitialWindowState initial_window_state =
      kMuonBrowserInitialWindowStateNormal;
  /**
   * Browser background color used before a document loads or when no document
   * color is specified.
   */
  MuonBrowserBackgroundColorConfig background_color;
  /**
   * Plugin API exposure configuration for browser pages.
   */
  MuonBrowserPluginConfig plugin;
  /**
   * Page URL patterns that keep unsafe JavaScript parent access for popups.
   */
  std::vector<std::string> allow_unsafe_javascript_parent_access;
  /**
   * Shortcut that opens DevTools.
   */
  MuonKeyboardShortcut devtools;
  /**
   * Shortcut that reloads the current page.
   */
  MuonKeyboardShortcut reload;
  /**
   * Shortcut that reloads the current page ignoring cached data.
   */
  MuonKeyboardShortcut hard_reload;
  /**
   * Shortcut that toggles the top-level window fullscreen state.
   */
  MuonKeyboardShortcut fullscreen;
  /**
   * Shortcut that increases page zoom.
   */
  MuonKeyboardShortcut zoom_in;
  /**
   * Shortcut that decreases page zoom.
   */
  MuonKeyboardShortcut zoom_out;
  /**
   * Shortcut that resets page zoom.
   */
  MuonKeyboardShortcut reset_zoom;
};

/**
 * Remote debugging connection section from muon.json.
 */
struct MuonDebuggerConfig {
  /**
   * Whether CEF remote debugging is enabled.
   */
  bool enable = false;
  /**
   * CEF remote debugging port.
   */
  int port = 9222;
};

/**
 * One network.authorizedOrigin entry from muon.json.
 */
struct MuonAuthorizedOriginConfig {
  /**
   * URL scheme matched as a lowercase exact string.
   */
  std::string scheme;
  /**
   * Domain matched as a lowercase exact string.
   */
  std::string domain;
  /**
   * Explicit port. Zero means the scheme default port is used when available.
   */
  int port = 0;
};

/**
 * Network section from muon.json.
 */
struct MuonNetworkConfig {
  /**
   * Glob patterns that allow full request URLs.
   */
  std::vector<std::string> allow = {"asset://**"};
  /**
   * Origins that authorize top-level navigation targets and initiated requests.
   */
  std::vector<MuonAuthorizedOriginConfig> authorized_origin;
};

/**
 * One plugin entry from the plugin.plugins array in muon.json.
 */
struct MuonPluginEntryConfig {
  /**
   * Plugin file stem, or the reserved internal plugin name.
   */
  std::string name;
  /**
   * Glob patterns that allow full plugin function paths.
   */
  std::vector<std::string> allow;
};

/**
 * Native plugin load configuration.
 */
struct MuonPluginConfig {
  /**
   * Directory path containing external plugin libraries.
   *
   * @remarks Relative paths are resolved from the muon executable directory for
   * defaults and from the containing config file for explicit muon.json values.
   */
  std::filesystem::path path = "./plugins";
  /**
   * Explicit plugin load and function access configuration.
   */
  std::vector<MuonPluginEntryConfig> plugins;
};

/**
 * Asset storage section from muon.json.
 */
struct MuonAssetConfig {
  /**
   * Whether asset.from was explicitly configured.
   */
  bool has_from = false;
  /**
   * Whether asset.signature was explicitly configured.
   */
  bool has_signature = false;
  /**
   * Whether asset.salt was explicitly configured.
   */
  bool has_salt = false;
  /**
   * Directory or ZIP file used as the asset:// backing storage.
   *
   * @remarks Relative paths are resolved from the containing config file for
   * explicit muon.json values. When omitted, executable-directory/assets is
   * used by the asset scheme handler.
   */
  std::filesystem::path from;
  /**
   * Expected SHA-1 digest for the salted ZIP file specified by asset.from.
   *
   * @remarks This value is normalized to lowercase hexadecimal while reading
   * muon.json.
   */
  std::string signature;
  /**
   * Additional bytes appended to the ZIP stream before signature comparison.
   *
   * @remarks This value is decoded from the hexadecimal asset.salt string while
   * reading muon.json. An explicitly empty string is represented as an empty
   * vector with has_salt set.
   */
  std::vector<uint8_t> salt;
};

/**
 * Root muon.json configuration.
 */
struct MuonConfig {
  /**
   * Default CEF version selection policy used when bootstrap settings omit one.
   */
  std::string default_version_policy = "tested";
  /**
   * Browser asset storage configuration.
   */
  MuonAssetConfig asset;
  /**
   * Unified log configuration.
   */
  MuonLogConfig log;
  /**
   * Browser keyboard shortcut configuration.
   */
  MuonBrowserConfig browser;
  /**
   * Browser network access configuration.
   */
  MuonNetworkConfig network;
  /**
   * Remote debugging connection configuration.
   */
  MuonDebuggerConfig cdp;
  /**
   * Native plugin load configuration.
   */
  MuonPluginConfig plugin;
};

/**
 * Returns the default configuration path for the running executable.
 *
 * @return executable-directory/muon.json, which is resolved by the loader to
 * muon.json5, muon.jsonc, or muon.json.
 */
std::filesystem::path GetDefaultMuonConfigPath();

/**
 * Loads a muon configuration file as JSON5.
 *
 * @remarks When path names muon.json, the containing directory is searched in
 * muon.json5, muon.jsonc, muon.json order.
 *
 * @param path Configuration file path or default muon.json request path.
 * @param config Receives parsed configuration. Missing files produce defaults.
 * @param error_message Receives a validation or read error on failure.
 * @return true when configuration was loaded or defaults were applied.
 */
bool LoadMuonConfig(const std::filesystem::path& path,
                    MuonConfig* config,
                    std::string* error_message);

/**
 * Extracts explicit muon config paths from process command-line arguments.
 *
 * @remarks Only exact `-c <path>` pairs are recognized. Relative paths are
 * resolved from the current working directory. Other arguments are ignored.
 *
 * @param command_line Process command line, including argv[0] when available.
 * @param config_paths Receives explicit config paths in command-line order.
 * @param error_message Receives a command-line error on failure.
 * @return true when command-line parsing succeeded.
 */
bool GetMuonConfigPathsFromCommandLine(
    const std::vector<std::string>& command_line,
    std::vector<std::filesystem::path>* config_paths,
    std::string* error_message);

/**
 * Loads and merges explicit muon configuration files as JSON5.
 *
 * @remarks Later files override earlier files. Object values with the same key
 * are recursively merged. Array values append elements that are not already
 * present by strict JSON value equality. All other values are replaced. Missing
 * explicit files fail the load.
 *
 * @param paths Configuration file paths in override order.
 * @param config Receives parsed configuration. Empty paths produce defaults.
 * @param error_message Receives a validation or read error on failure.
 * @return true when configuration was loaded or defaults were applied.
 */
bool LoadMuonConfigFiles(const std::vector<std::filesystem::path>& paths,
                         MuonConfig* config,
                         std::string* error_message);

/**
 * Loads executable-directory/muon.json5, muon.jsonc, or muon.json.
 *
 * @param config Receives parsed configuration.
 * @param error_message Receives a validation or read error on failure.
 * @return true when configuration was loaded or defaults were applied.
 */
bool LoadDefaultMuonConfig(MuonConfig* config, std::string* error_message);

/**
 * Returns the deterministic byte used for an empty embedded config slot.
 *
 * @param index Slot byte index.
 * @return Empty slot byte for the provided index.
 */
uint8_t GetMuonEmbeddedConfigEmptySlotByte(size_t index);

/**
 * Creates a full embedded config slot from an encoded TLV payload.
 *
 * @param payload Encoded embedded muon config TLV payload.
 * @param slot Receives a fixed-size slot image.
 * @param error_message Receives a validation error on failure.
 * @return true when the slot image was created.
 */
bool CreateMuonEmbeddedConfigSlot(const std::vector<uint8_t>& payload,
                                  std::vector<uint8_t>* slot,
                                  std::string* error_message);

/**
 * Loads startup configuration using an explicit embedded slot image.
 *
 * @remarks When the slot contains an embedded config, command-line `-c`
 * arguments are ignored. When the slot is empty, command-line and default file
 * loading follow the normal non-embedded startup path.
 *
 * @param command_line Process command line, including argv[0] when available.
 * @param slot Embedded slot bytes.
 * @param slot_size Embedded slot byte size.
 * @param embedded_base_directory Base directory for relative embedded paths.
 * @param config Receives parsed configuration.
 * @param config_paths Receives explicit non-embedded config paths.
 * @param embedded Receives whether embedded config was used.
 * @param error_message Receives a validation or read error on failure.
 * @return true when startup configuration was loaded.
 */
bool LoadMuonStartupConfigFromEmbeddedSlot(
    const std::vector<std::string>& command_line,
    const uint8_t* slot,
    size_t slot_size,
    const std::filesystem::path& embedded_base_directory,
    MuonConfig* config,
    std::vector<std::filesystem::path>* config_paths,
    bool* embedded,
    std::string* error_message);

/**
 * Loads startup configuration from the built-in embedded slot or files.
 *
 * @remarks Embedded config takes precedence over `-c` and the default
 * executable-directory muon.json lookup.
 *
 * @param command_line Process command line, including argv[0] when available.
 * @param config Receives parsed configuration.
 * @param config_paths Receives explicit non-embedded config paths.
 * @param embedded Receives whether embedded config was used.
 * @param error_message Receives a validation or read error on failure.
 * @return true when startup configuration was loaded.
 */
bool LoadMuonStartupConfig(
    const std::vector<std::string>& command_line,
    MuonConfig* config,
    std::vector<std::filesystem::path>* config_paths,
    bool* embedded,
    std::string* error_message);
