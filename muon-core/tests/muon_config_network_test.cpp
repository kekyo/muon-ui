/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "config/muon_config.h"
#include "config/muon_startup.h"
#include "browser/muon_browser_background_color.h"
#include "network/muon_network_policy.h"
#include "plugins/muon_plugin_policy.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstdint>
#include <string>
#include <vector>

static bool Expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << "\n";
    return false;
  }
  return true;
}

static std::filesystem::path CreateTestDirectory() {
  std::error_code error;
  const auto base = std::filesystem::temp_directory_path(error);
  if (error) {
    return {};
  }
  const auto unique =
      std::to_string(std::chrono::steady_clock::now()
                         .time_since_epoch()
                         .count());
  const auto directory = base / ("muon-config-network-" + unique);
  if (!std::filesystem::create_directories(directory, error) || error) {
    return {};
  }
  return directory;
}

static bool WriteFile(const std::filesystem::path& path,
                      const std::string& content) {
  std::ofstream output(path, std::ios::binary);
  if (!output) {
    return false;
  }
  output << content;
  return static_cast<bool>(output);
}

static bool LoadConfigExpectSuccess(const std::filesystem::path& path,
                                    MuonConfig* config) {
  std::string error_message;
  return Expect(LoadMuonConfig(path, config, &error_message), error_message);
}

static bool LoadConfigExpectFailure(const std::filesystem::path& path,
                                    const std::string& expected_message) {
  MuonConfig config;
  std::string error_message;
  return Expect(!LoadMuonConfig(path, &config, &error_message),
                "invalid config was accepted") &&
         Expect(error_message.find(expected_message) != std::string::npos,
                "config error did not contain: " + expected_message);
}

static bool LoadConfigFilesExpectSuccess(
    const std::vector<std::filesystem::path>& paths,
    MuonConfig* config) {
  std::string error_message;
  return Expect(LoadMuonConfigFiles(paths, config, &error_message),
                error_message);
}

static bool LoadConfigFilesExpectFailure(
    const std::vector<std::filesystem::path>& paths,
    const std::string& expected_message) {
  MuonConfig config;
  std::string error_message;
  return Expect(!LoadMuonConfigFiles(paths, &config, &error_message),
                "invalid config file sequence was accepted") &&
         Expect(error_message.find(expected_message) != std::string::npos,
                "config files error did not contain: " + expected_message);
}

static void SetTestLaunchSource(const std::string& launch_source) {
  std::string executable = "muon";
  std::string switch_argument =
      std::string(kMuonLaunchSourceSwitchPrefix) + launch_source;
  char* argv[] = {executable.data(), switch_argument.data()};
  SetMuonStartupCommandLine(2, argv);
}

static void SetTestDefaultLaunchSource() {
  std::string executable = "muon";
  char* argv[] = {executable.data()};
  SetMuonStartupCommandLine(1, argv);
}

static void SetEnvironment(const char* name, const std::filesystem::path& path) {
#if defined(_WIN32)
  _putenv_s(name, path.string().c_str());
#else
  setenv(name, path.string().c_str(), 1);
#endif
}

static bool ExpectShortcut(const MuonKeyboardShortcut& shortcut,
                           bool enabled,
                           int windows_key_code,
                           uint32_t modifiers,
                           const std::string& message,
                           bool accepts_shift_variant = false) {
  return Expect(shortcut.enabled == enabled, message + " enabled changed") &&
         Expect(shortcut.windows_key_code == windows_key_code,
                message + " key code changed") &&
         Expect(shortcut.modifiers == modifiers,
                message + " modifiers changed") &&
         Expect(shortcut.accepts_shift_variant == accepts_shift_variant,
                message + " shift variant changed");
}

static bool ExpectBrowserBackgroundSystem(
    const MuonBrowserBackgroundColorConfig& background_color,
    const std::string& message) {
  return Expect(background_color.mode == kMuonBrowserBackgroundColorSystem,
                message + " mode changed") &&
         Expect(background_color.red == 0, message + " red changed") &&
         Expect(background_color.green == 0, message + " green changed") &&
         Expect(background_color.blue == 0, message + " blue changed");
}

static bool ExpectBrowserBackgroundRgb(
    const MuonBrowserBackgroundColorConfig& background_color,
    uint8_t red,
    uint8_t green,
    uint8_t blue,
    const std::string& message) {
  return Expect(background_color.mode == kMuonBrowserBackgroundColorRgb,
                message + " mode changed") &&
         Expect(background_color.red == red, message + " red changed") &&
         Expect(background_color.green == green,
                message + " green changed") &&
         Expect(background_color.blue == blue, message + " blue changed");
}

static bool ExpectResolvedBrowserBackgroundColor(
    const MuonResolvedBrowserBackgroundColor& resolved_color,
    uint8_t red,
    uint8_t green,
    uint8_t blue,
    const std::string& message) {
  return Expect(resolved_color.has_color, message + " was not resolved") &&
         Expect(CefColorGetA(resolved_color.color) == 0xff,
                message + " alpha changed") &&
         Expect(CefColorGetR(resolved_color.color) == red,
                message + " red changed") &&
         Expect(CefColorGetG(resolved_color.color) == green,
                message + " green changed") &&
         Expect(CefColorGetB(resolved_color.color) == blue,
                message + " blue changed");
}

static bool ExpectLogDefaults(const MuonLogConfig& log,
                              const std::string& message) {
  return Expect(log.level == kMuonLogLevelInfo,
                message + " global level changed") &&
         Expect(log.output.type == kMuonLogOutputStderr,
                message + " output type changed") &&
         Expect(log.output.path.empty(), message + " output path changed") &&
         Expect(log.muon == kMuonLogLevelInfo,
                message + " muon level changed") &&
         Expect(log.cef == kMuonLogLevelWarning,
                message + " cef level changed") &&
         Expect(log.console == kMuonLogLevelDebug,
                message + " console level changed") &&
         Expect(log.plugin == kMuonLogLevelInfo,
                message + " plugin level changed");
}

static bool ExpectBrowserDefaults(const MuonBrowserConfig& browser,
                                  const std::filesystem::path& profile,
                                  const std::string& message) {
  return Expect(browser.start_page == "asset://main/index.html",
                message + " start URL changed") &&
         Expect(browser.profile == profile,
                message + " profile path changed") &&
         Expect(browser.initial_window_state ==
                    kMuonBrowserInitialWindowStateNormal,
                message + " initial window state changed") &&
         Expect(browser.title_bar == kMuonBrowserTitleBarMuon,
                message + " title bar mode changed") &&
         Expect(browser.initial_title_bar_visibility,
                message + " initial title bar visibility changed") &&
         ExpectBrowserBackgroundSystem(browser.background_color,
                                       message + " background color") &&
         Expect(browser.plugin.allow.size() == 1,
                message + " plugin page allowlist count changed") &&
         Expect(browser.plugin.allow[0] == "asset://main/**",
                message + " plugin page pattern changed") &&
         Expect(browser.allow_unsafe_javascript_parent_access.empty(),
                message + " unsafe parent access allowlist count changed") &&
         ExpectShortcut(browser.devtools, false, 0, 0,
                        message + " devtools shortcut") &&
         ExpectShortcut(browser.reload, false, 0, 0,
                        message + " reload shortcut") &&
         ExpectShortcut(browser.hard_reload, false, 0, 0,
                        message + " hardReload shortcut") &&
         ExpectShortcut(browser.fullscreen, false, 0, 0,
                        message + " fullscreen shortcut") &&
         ExpectShortcut(browser.zoom_in, false, 0, 0,
                        message + " zoomIn shortcut") &&
         ExpectShortcut(browser.zoom_out, false, 0, 0,
                        message + " zoomOut shortcut") &&
         ExpectShortcut(browser.reset_zoom, false, 0, 0,
                        message + " resetZoom shortcut") &&
         ExpectShortcut(browser.recycle, false, 0, 0,
                        message + " recycle shortcut");
}

static bool ExpectDebuggerConfig(const MuonDebuggerConfig& cdp,
                                 bool enable,
                                 int port,
                                 const std::string& message) {
  return Expect(cdp.enable == enable, message + " enable changed") &&
         Expect(cdp.port == port, message + " port changed");
}

static bool ExpectAuthorizedOrigin(
    const MuonAuthorizedOriginConfig& origin,
    const std::string& scheme,
    const std::string& domain,
    int port,
    const std::string& message) {
  return Expect(origin.scheme == scheme, message + " scheme changed") &&
         Expect(origin.domain == domain, message + " domain changed") &&
         Expect(origin.port == port, message + " port changed");
}

static void WriteVarUint(std::vector<uint8_t>* bytes, uint64_t value) {
  do {
    auto next = static_cast<uint8_t>(value & 0x7f);
    value >>= 7;
    if (value != 0) {
      next = static_cast<uint8_t>(next | 0x80);
    }
    bytes->push_back(next);
  } while (value != 0);
}

static void WriteRawString(std::vector<uint8_t>* bytes,
                           const std::string& value) {
  WriteVarUint(bytes, value.size());
  bytes->insert(bytes->end(), value.begin(), value.end());
}

static void WriteTlvString(std::vector<uint8_t>* bytes,
                           const std::string& value) {
  bytes->push_back(4);
  WriteRawString(bytes, value);
}

static void WriteTlvBinary(std::vector<uint8_t>* bytes,
                           const std::vector<uint8_t>& value) {
  bytes->push_back(5);
  WriteVarUint(bytes, value.size());
  bytes->insert(bytes->end(), value.begin(), value.end());
}

static void WriteTlvBool(std::vector<uint8_t>* bytes, bool value) {
  bytes->push_back(value ? 2 : 1);
}

static void BeginTlvObject(std::vector<uint8_t>* bytes, size_t entry_count) {
  bytes->push_back(7);
  WriteVarUint(bytes, entry_count);
}

static std::vector<uint8_t> CreateEmbeddedConfigPayload() {
  std::vector<uint8_t> bytes;
  BeginTlvObject(&bytes, 4);

  WriteRawString(&bytes, "asset");
  BeginTlvObject(&bytes, 3);
  WriteRawString(&bytes, "sourcePath");
  WriteTlvString(&bytes, "assets.zip");
  WriteRawString(&bytes, "signature");
  WriteTlvBinary(
      &bytes,
      {0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e,
       0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d});
  WriteRawString(&bytes, "salt");
  WriteTlvBinary(&bytes, {0x0a, 0x10, 0xff});

  WriteRawString(&bytes, "browser");
  BeginTlvObject(&bytes, 3);
  WriteRawString(&bytes, "startPage");
  WriteTlvString(&bytes, "https://embedded.example/app");
  WriteRawString(&bytes, "profilePath");
  WriteTlvString(&bytes, "profiles/embedded");
  WriteRawString(&bytes, "backgroundColor");
  WriteTlvBinary(&bytes, {0x12, 0x3a, 0xbc});

  WriteRawString(&bytes, "cdp");
  BeginTlvObject(&bytes, 1);
  WriteRawString(&bytes, "enable");
  WriteTlvBool(&bytes, true);

  WriteRawString(&bytes, "plugin");
  BeginTlvObject(&bytes, 1);
  WriteRawString(&bytes, "path");
  WriteTlvString(&bytes, "plugins");

  return bytes;
}

static bool RunConfigLoadingTest(const std::filesystem::path& test_directory) {
  MuonConfig config;
  SetTestDefaultLaunchSource();
  if (!LoadConfigExpectSuccess(test_directory / "missing.json", &config)) {
    return false;
  }
  std::error_code error;
  const auto current_profile =
      (std::filesystem::current_path(error) / ".profile").lexically_normal();
  if (error) {
    std::cerr << "failed to resolve current directory\n";
    return false;
  }
  if (!Expect(config.network.allow.size() == 1,
              "missing muon.json did not produce the default allowlist") ||
      !Expect(config.network.allow[0] == "asset://**",
              "missing muon.json default network allowlist is wrong") ||
      !Expect(!config.asset.has_from,
              "missing muon.json configured asset.sourcePath") ||
      !Expect(!config.asset.has_signature,
              "missing muon.json configured asset.signature") ||
      !Expect(!config.asset.has_salt,
              "missing muon.json configured asset.salt") ||
      !Expect(config.network.authorized_origin.empty(),
              "missing muon.json did not produce an empty authorized origin "
              "list") ||
      !ExpectBrowserDefaults(config.browser, current_profile,
                             "missing browser config") ||
      !Expect(config.plugin.plugins.empty(),
              "missing muon.json did not produce an empty plugin list") ||
      !Expect(config.plugin.path == std::filesystem::path("./plugins"),
              "missing muon.json default plugin path is wrong") ||
      !ExpectLogDefaults(config.log, "missing log config") ||
      !ExpectDebuggerConfig(config.cdp, false, 9222,
                            "missing cdp config") ||
      !Expect(config.default_version_policy == "tested",
              "missing muon.json defaultVersionPolicy is wrong") ||
      !Expect(config.desktop_id == "muon",
              "missing muon.json desktopId is wrong")) {
    return false;
  }

  const auto no_network_path = test_directory / "no-network.json";
  if (!Expect(WriteFile(no_network_path, "{}"),
              "failed to write no-network config") ||
      !LoadConfigExpectSuccess(no_network_path, &config)) {
    return false;
  }
  if (!Expect(config.network.allow.size() == 1,
              "missing network.allow did not produce the default allowlist") ||
      !Expect(config.network.allow[0] == "asset://**",
              "missing network.allow default pattern is wrong") ||
      !Expect(config.network.authorized_origin.empty(),
              "missing network.authorizedOrigin default list is not empty") ||
      !ExpectBrowserDefaults(config.browser, test_directory / ".profile",
                             "default browser config") ||
      !Expect(config.plugin.plugins.empty(),
              "missing plugins did not produce an empty plugin list") ||
      !Expect(config.plugin.path == std::filesystem::path("./plugins"),
              "missing plugin.path default is wrong") ||
      !ExpectLogDefaults(config.log, "default log config") ||
      !ExpectDebuggerConfig(config.cdp, false, 9222,
                            "default cdp config") ||
      !Expect(config.default_version_policy == "tested",
              "default defaultVersionPolicy is wrong") ||
      !Expect(config.desktop_id == "muon", "default desktopId is wrong")) {
    return false;
  }

  const auto default_policy_path =
      test_directory / "default-version-policy.json";
  if (!Expect(WriteFile(default_policy_path,
                        R"({"bootstrap":{"defaultVersionPolicy":"compat-latest","desktopId":"com.example.App"}})"),
              "failed to write defaultVersionPolicy config") ||
      !LoadConfigExpectSuccess(default_policy_path, &config)) {
    return false;
  }
  if (!Expect(config.default_version_policy == "compat-latest",
              "defaultVersionPolicy was not parsed") ||
      !Expect(config.desktop_id == "com.example.App",
              "desktopId was not parsed")) {
    return false;
  }

  const auto root_default_policy_path =
      test_directory / "root-default-version-policy.json";
  if (!Expect(WriteFile(root_default_policy_path,
                        R"({"defaultVersionPolicy":"compat-latest"})"),
              "failed to write root defaultVersionPolicy config") ||
      !LoadConfigExpectSuccess(root_default_policy_path, &config)) {
    return false;
  }
  if (!Expect(config.default_version_policy == "tested",
              "root defaultVersionPolicy should be ignored") ||
      !Expect(config.desktop_id == "muon",
              "root config should keep default desktopId")) {
    return false;
  }

  const auto network_without_allow_path =
      test_directory / "network-without-allow.json";
  if (!Expect(WriteFile(network_without_allow_path, R"({"network":{}})"),
              "failed to write network-without-allow config") ||
      !LoadConfigExpectSuccess(network_without_allow_path, &config)) {
    return false;
  }
  if (!Expect(config.network.allow.size() == 1,
              "network object without allow did not keep the default") ||
      !Expect(config.network.allow[0] == "asset://**",
              "network object without allow default pattern is wrong") ||
      !Expect(config.browser.plugin.allow.size() == 1,
              "network object without allow changed plugin page allowlist") ||
      !Expect(config.browser.plugin.allow[0] == "asset://main/**",
              "network object without allow plugin page pattern is wrong")) {
    return false;
  }

  const auto empty_plugins_path = test_directory / "empty-plugins.json";
  if (!Expect(WriteFile(empty_plugins_path,
                        R"({"plugin":{"path":"custom-plugins","plugins":[]}})"),
              "failed to write empty plugins config") ||
      !LoadConfigExpectSuccess(empty_plugins_path, &config)) {
    return false;
  }
  if (!Expect(config.plugin.plugins.empty(),
              "explicit empty plugins did not produce an empty plugin list") ||
      !Expect(config.plugin.path == test_directory / "custom-plugins",
              "explicit plugin.path was not parsed")) {
    return false;
  }

  const auto empty_network_allow_path =
      test_directory / "empty-network-allow.json";
  if (!Expect(WriteFile(empty_network_allow_path,
                        R"({"network":{"allow":[]}})"),
              "failed to write empty network allow config") ||
      !LoadConfigExpectSuccess(empty_network_allow_path, &config)) {
    return false;
  }
  if (!Expect(config.network.allow.empty(),
              "explicit empty network.allow did not override the default") ||
      !Expect(config.network.authorized_origin.empty(),
              "explicit empty network.allow changed authorized origin list") ||
      !Expect(config.browser.plugin.allow.size() == 1,
              "explicit empty network.allow changed plugin page allowlist") ||
      !Expect(config.browser.plugin.allow[0] == "asset://main/**",
              "explicit empty network.allow changed plugin page pattern") ||
      !Expect(config.plugin.plugins.empty(),
              "explicit empty network.allow changed plugin list")) {
    return false;
  }

  const auto empty_browser_plugin_allow_path =
      test_directory / "empty-browser-plugin-allow.json";
  if (!Expect(WriteFile(empty_browser_plugin_allow_path,
                        R"({"browser":{"plugin":{"allow":[]}}})"),
              "failed to write empty browser plugin allow config") ||
      !LoadConfigExpectSuccess(empty_browser_plugin_allow_path, &config)) {
    return false;
  }
  if (!Expect(config.browser.plugin.allow.empty(),
              "explicit empty browser.plugin.allow did not override the "
              "default") ||
      !Expect(config.network.allow.size() == 1,
              "explicit empty browser.plugin.allow changed network allowlist") ||
      !Expect(config.network.allow[0] == "asset://**",
              "explicit empty browser.plugin.allow changed network pattern")) {
    return false;
  }

  const auto empty_unsafe_parent_access_path =
      test_directory / "empty-unsafe-parent-access.json";
  if (!Expect(WriteFile(empty_unsafe_parent_access_path,
                        R"({"browser":{"allowUnsafeJavaScriptParentAccess":[]}})"),
              "failed to write empty unsafe parent access config") ||
      !LoadConfigExpectSuccess(empty_unsafe_parent_access_path, &config)) {
    return false;
  }
  if (!Expect(config.browser.allow_unsafe_javascript_parent_access.empty(),
              "explicit empty browser.allowUnsafeJavaScriptParentAccess did "
              "not override the default") ||
      !Expect(config.browser.plugin.allow.size() == 1,
              "explicit empty browser.allowUnsafeJavaScriptParentAccess "
              "changed plugin page allowlist") ||
      !Expect(config.browser.plugin.allow[0] == "asset://main/**",
              "explicit empty browser.allowUnsafeJavaScriptParentAccess "
              "changed plugin page pattern")) {
    return false;
  }

  const auto system_background_color_path =
      test_directory / "system-background-color.json";
  if (!Expect(WriteFile(system_background_color_path,
                        R"({"browser":{"backgroundColor":"system"}})"),
              "failed to write system background color config") ||
      !LoadConfigExpectSuccess(system_background_color_path, &config)) {
    return false;
  }
  if (!ExpectBrowserBackgroundSystem(config.browser.background_color,
                                     "explicit system background color")) {
    return false;
  }

  const auto empty_authorized_origin_path =
      test_directory / "empty-authorized-origin.json";
  if (!Expect(WriteFile(empty_authorized_origin_path,
                        R"({"network":{"authorizedOrigin":[]}})"),
              "failed to write empty authorizedOrigin config") ||
      !LoadConfigExpectSuccess(empty_authorized_origin_path, &config)) {
    return false;
  }
  if (!Expect(config.network.allow.size() == 1,
              "explicit empty network.authorizedOrigin changed network "
              "allowlist") ||
      !Expect(config.network.allow[0] == "asset://**",
              "explicit empty network.authorizedOrigin changed network "
              "pattern") ||
      !Expect(config.network.authorized_origin.empty(),
              "explicit empty network.authorizedOrigin did not produce an "
              "empty list")) {
    return false;
  }

  const auto allow_path = test_directory / "allow.json";
  if (!Expect(WriteFile(allow_path,
                        R"({"asset":{"sourcePath":"packed/assets.zip","signature":"A9993E364706816ABA3E25717850C26C9CD0D89D","salt":"0A10ff"},"browser":{"startPage":"https://example.com/app","profilePath":"profiles/custom","initialWindowState":"maximized","initialTitleBarVisibility":false,"initialTitleBarIcon":"icons/app.png","backgroundColor":"#123abc","titleBarType":"native","allowUnsafeJavaScriptParentAccess":["asset://main/**","https://example.com/popups/**"],"plugin":{"allow":["asset://main/**","data:**"]}},"network":{"allow":["data:**","https://example.com/**"],"authorizedOrigin":[{"scheme":"HTTPS","domain":"LOGIN.LIVE.COM"},{"scheme":"http","domain":"LOCALHOST","port":8080}]},"cdp":{"enable":true,"port":9333},"plugin":{"path":"./custom-plugins","plugins":[{"name":"internal","allow":["muon.browser.*","muon.fs.readFile"]},{"name":"foobar","allow":["foobar.*"]}]}})"),
              "failed to write allow config") ||
      !LoadConfigExpectSuccess(allow_path, &config)) {
    return false;
  }
  return Expect(config.network.allow.size() == 2,
                "network.allow pattern count is wrong") &&
         Expect(config.network.allow[0] == "data:**",
                "first network.allow pattern changed") &&
         Expect(config.network.allow[1] == "https://example.com/**",
                "second network.allow pattern changed") &&
         Expect(config.network.authorized_origin.size() == 2,
                "network.authorizedOrigin entry count is wrong") &&
         ExpectAuthorizedOrigin(config.network.authorized_origin[0], "https",
                                "login.live.com", 0,
                                "first network.authorizedOrigin") &&
         ExpectAuthorizedOrigin(config.network.authorized_origin[1], "http",
                                "localhost", 8080,
                                "second network.authorizedOrigin") &&
         Expect(config.browser.start_page == "https://example.com/app",
                "browser.startPage was not parsed") &&
         Expect(config.browser.profile == test_directory / "profiles/custom",
                "browser.profilePath was not parsed") &&
         Expect(config.browser.initial_window_state ==
                    kMuonBrowserInitialWindowStateMaximized,
                "browser.initialWindowState was not parsed") &&
         ExpectBrowserBackgroundRgb(config.browser.background_color, 0x12,
                                    0x3a, 0xbc,
                                    "browser.backgroundColor was not parsed") &&
         Expect(config.browser.title_bar == kMuonBrowserTitleBarNative,
                "browser.titleBarType was not parsed") &&
         Expect(!config.browser.initial_title_bar_visibility,
                "browser.initialTitleBarVisibility was not parsed") &&
         Expect(config.browser.has_initial_title_bar_icon,
                "browser.initialTitleBarIcon was not marked present") &&
         Expect(config.browser.initial_title_bar_icon == "icons/app.png",
                "browser.initialTitleBarIcon was not parsed") &&
         Expect(config.browser.plugin.allow.size() == 2,
                "browser.plugin.allow pattern count is wrong") &&
         Expect(config.browser.plugin.allow[0] == "asset://main/**",
                "first browser.plugin.allow pattern changed") &&
         Expect(config.browser.plugin.allow[1] == "data:**",
                "second browser.plugin.allow pattern changed") &&
         Expect(config.browser.allow_unsafe_javascript_parent_access.size() ==
                    2,
                "browser.allowUnsafeJavaScriptParentAccess pattern count is "
                "wrong") &&
         Expect(config.browser.allow_unsafe_javascript_parent_access[0] ==
                    "asset://main/**",
                "first browser.allowUnsafeJavaScriptParentAccess pattern "
                "changed") &&
         Expect(config.browser.allow_unsafe_javascript_parent_access[1] ==
                    "https://example.com/popups/**",
                "second browser.allowUnsafeJavaScriptParentAccess pattern "
                "changed") &&
         ExpectDebuggerConfig(config.cdp, true, 9333,
                              "cdp config") &&
         Expect(config.plugin.path == test_directory / "custom-plugins",
                "plugin.path was not parsed") &&
         Expect(config.plugin.plugins.size() == 2,
                "plugins entry count is wrong") &&
         Expect(config.plugin.plugins[0].name == "internal",
                "internal plugin name changed") &&
         Expect(config.plugin.plugins[0].allow.size() == 2,
                "internal plugin allow pattern count is wrong") &&
         Expect(config.plugin.plugins[0].allow[0] == "muon.browser.*",
                "first internal plugin allow pattern changed") &&
         Expect(config.plugin.plugins[0].allow[1] == "muon.fs.readFile",
                "second internal plugin allow pattern changed") &&
         Expect(config.plugin.plugins[1].name == "foobar",
                "external plugin name changed") &&
         Expect(config.plugin.plugins[1].allow.size() == 1,
                "external plugin allow pattern count is wrong") &&
         Expect(config.plugin.plugins[1].allow[0] == "foobar.*",
                "external plugin allow pattern changed") &&
         Expect(config.asset.has_from, "asset.sourcePath was not parsed") &&
         Expect(config.asset.from == test_directory / "packed/assets.zip",
                "asset.sourcePath was not resolved from config directory") &&
         Expect(config.asset.has_signature,
                "asset.signature was not parsed") &&
         Expect(config.asset.signature ==
                    "a9993e364706816aba3e25717850c26c9cd0d89d",
                "asset.signature was not normalized") &&
         Expect(config.asset.has_salt, "asset.salt was not parsed") &&
         Expect(config.asset.salt.size() == 3,
                "asset.salt byte count changed") &&
         Expect(config.asset.salt[0] == 0x0a && config.asset.salt[1] == 0x10 &&
                    config.asset.salt[2] == 0xff,
                "asset.salt bytes changed");
}

static bool RunConfigJson5LoadingTest(
    const std::filesystem::path& test_directory) {
  MuonConfig config;
  const auto json_path = test_directory / "json5-content.json";
  if (!Expect(WriteFile(json_path,
                        R"({
  // JSON5 comment
  browser: {
    startPage: 'https://example.com/json5',
  },
  network: {
    allow: [
      'https://json5.example/**',
    ],
  },
})"),
              "failed to write JSON5 config") ||
      !LoadConfigExpectSuccess(json_path, &config)) {
    return false;
  }
  return Expect(config.browser.start_page == "https://example.com/json5",
                "JSON5 browser.startPage was not parsed") &&
         Expect(config.network.allow.size() == 1,
                "JSON5 network.allow count is wrong") &&
         Expect(config.network.allow[0] == "https://json5.example/**",
                "JSON5 network.allow pattern changed");
}

static bool RunLaunchSourceProfilePathTest(
    const std::filesystem::path& test_directory) {
  MuonConfig config;

  SetTestLaunchSource("none");
  const auto none_path = test_directory / "profile-none.json";
  if (!Expect(WriteFile(none_path, "{}"),
              "failed to write none profile config") ||
      !LoadConfigExpectSuccess(none_path, &config)) {
    return false;
  }
  if (!Expect(config.browser.profile == test_directory / ".profile",
              "none launch source should use config-relative default profile")) {
    return false;
  }

  SetTestLaunchSource("normal");
  const auto normal_path = test_directory / "profile-normal.json";
#if defined(_WIN32)
  const auto user_data_home = test_directory / "local-app-data";
  SetEnvironment("LOCALAPPDATA", user_data_home);
#else
  const auto user_data_home = test_directory / "xdg-data";
  SetEnvironment("XDG_DATA_HOME", user_data_home);
#endif
  if (!Expect(WriteFile(normal_path, "{}"),
              "failed to write normal profile config") ||
      !LoadConfigExpectSuccess(normal_path, &config)) {
    return false;
  }
  if (!Expect(config.browser.profile == user_data_home / "muon" / "profile",
              "normal launch source should use user data default profile")) {
    return false;
  }

  const auto explicit_path = test_directory / "profile-explicit.json";
  if (!Expect(WriteFile(explicit_path,
                        R"({"browser":{"profilePath":"profiles/custom"}})"),
              "failed to write explicit profile config") ||
      !LoadConfigExpectSuccess(explicit_path, &config)) {
    return false;
  }
  return Expect(config.browser.profile == test_directory / "profiles/custom",
                "explicit browser.profilePath should override launch source");
}

static bool ExpectDefaultConfigStart(
    const std::filesystem::path& test_directory,
    const std::string& expected_start,
    const std::string& message) {
  MuonConfig config;
  return LoadConfigExpectSuccess(test_directory / "muon.json", &config) &&
         Expect(config.browser.start_page == expected_start, message);
}

static bool RunDefaultConfigSearchOrderTest(
    const std::filesystem::path& test_directory) {
  const auto json5_path = test_directory / "muon.json5";
  const auto jsonc_path = test_directory / "muon.jsonc";
  const auto json_path = test_directory / "muon.json";
  if (!Expect(WriteFile(json5_path,
                        R"({browser:{startPage:'https://example.com/json5',},})"),
              "failed to write muon.json5") ||
      !Expect(WriteFile(jsonc_path,
                        R"({browser:{startPage:'https://example.com/jsonc',},})"),
              "failed to write muon.jsonc") ||
      !Expect(WriteFile(json_path,
                        R"({browser:{startPage:'https://example.com/json',},})"),
              "failed to write muon.json")) {
    return false;
  }

  std::error_code error;
  if (!ExpectDefaultConfigStart(test_directory, "https://example.com/json5",
                                "muon.json5 was not preferred") ||
      !Expect(std::filesystem::remove(json5_path, error) && !error,
              "failed to remove muon.json5") ||
      !ExpectDefaultConfigStart(test_directory, "https://example.com/jsonc",
                                "muon.jsonc was not preferred") ||
      !Expect(std::filesystem::remove(jsonc_path, error) && !error,
              "failed to remove muon.jsonc") ||
      !ExpectDefaultConfigStart(test_directory, "https://example.com/json",
                                "muon.json fallback was not used")) {
    return false;
  }
  return true;
}

static bool RunCommandLineConfigPathTest(
    const std::filesystem::path& test_directory) {
  std::vector<std::filesystem::path> config_paths;
  std::string error_message;
  if (!Expect(GetMuonConfigPathsFromCommandLine(
                  {"muon", "--ozone-platform=x11"}, &config_paths,
                  &error_message),
              error_message) ||
      !Expect(config_paths.empty(),
              "command line without -c produced config paths")) {
    return false;
  }

  const auto first_path = test_directory / "first.json";
  const auto second_path = test_directory / "second.json";
  if (!Expect(GetMuonConfigPathsFromCommandLine(
                  {"muon", "-c", first_path.string(), "--disable-gpu", "-c",
                   second_path.string()},
                  &config_paths, &error_message),
              error_message) ||
      !Expect(config_paths.size() == 2,
              "multiple -c entries were not collected") ||
      !Expect(config_paths[0] == first_path.lexically_normal(),
              "first -c path changed") ||
      !Expect(config_paths[1] == second_path.lexically_normal(),
              "second -c path changed")) {
    return false;
  }

  return Expect(!GetMuonConfigPathsFromCommandLine(
                    {"muon", "-c"}, &config_paths, &error_message),
                "missing -c path was accepted") &&
         Expect(error_message.find("requires a path") != std::string::npos,
                "missing -c path error message is wrong");
}

static bool RunEmbeddedConfigLoadingTest(
    const std::filesystem::path& test_directory) {
  const auto runtime_directory = test_directory / "embedded-runtime";
  std::error_code error;
  if (!Expect(std::filesystem::create_directories(runtime_directory, error) &&
                  !error,
              "failed to create embedded runtime directory")) {
    return false;
  }

  const auto payload = CreateEmbeddedConfigPayload();
  std::vector<uint8_t> slot;
  std::string error_message;
  if (!Expect(CreateMuonEmbeddedConfigSlot(payload, &slot, &error_message),
              error_message)) {
    return false;
  }

  MuonConfig config;
  std::vector<std::filesystem::path> config_paths;
  auto embedded = false;
  if (!Expect(LoadMuonStartupConfigFromEmbeddedSlot(
                  {"muon", "-c"}, slot.data(), slot.size(), runtime_directory,
                  &config, &config_paths, &embedded, &error_message),
              error_message)) {
    return false;
  }

  return Expect(embedded, "embedded config was not detected") &&
         Expect(config_paths.empty(),
                "embedded config did not ignore command-line config paths") &&
         Expect(config.browser.start_page == "https://embedded.example/app",
                "embedded browser.startPage was not parsed") &&
         Expect(config.browser.profile ==
                    runtime_directory / "profiles/embedded",
                "embedded browser.profilePath was not resolved from executable "
                "directory") &&
         ExpectBrowserBackgroundRgb(config.browser.background_color, 0x12,
                                    0x3a, 0xbc,
                                    "embedded browser.backgroundColor") &&
         Expect(config.cdp.enable, "embedded cdp.enable was not parsed") &&
         Expect(config.plugin.path == runtime_directory / "plugins",
                "embedded plugin.path was not resolved from executable "
                "directory") &&
         Expect(config.asset.has_from,
                "embedded asset.sourcePath was not parsed") &&
         Expect(config.asset.from == runtime_directory / "assets.zip",
                "embedded asset.sourcePath was not resolved from executable "
                "directory") &&
         Expect(config.asset.has_signature,
                "embedded asset.signature was not parsed") &&
         Expect(config.asset.signature ==
                    "a9993e364706816aba3e25717850c26c9cd0d89d",
                "embedded binary asset.signature was not restored") &&
         Expect(config.asset.has_salt, "embedded asset.salt was not parsed") &&
         Expect(config.asset.salt.size() == 3,
                "embedded asset.salt byte count changed") &&
         Expect(config.asset.salt[0] == 0x0a && config.asset.salt[1] == 0x10 &&
                    config.asset.salt[2] == 0xff,
                "embedded binary asset.salt was not restored");
}

static bool RunEmbeddedConfigEmptySlotTest(
    const std::filesystem::path& test_directory) {
  std::vector<uint8_t> slot(kMuonEmbeddedConfigSlotSize);
  for (auto index = size_t{0}; index < slot.size(); ++index) {
    slot[index] = GetMuonEmbeddedConfigEmptySlotByte(index);
  }

  const auto config_path = test_directory / "non-embedded-command.json";
  if (!Expect(WriteFile(
                  config_path,
                  R"({"browser":{"startPage":"https://command.example/app"}})"),
              "failed to write command-line config")) {
    return false;
  }

  MuonConfig config;
  std::vector<std::filesystem::path> config_paths;
  auto embedded = true;
  std::string error_message;
  if (!Expect(LoadMuonStartupConfigFromEmbeddedSlot(
                  {"muon", "-c", config_path.string()}, slot.data(),
                  slot.size(), test_directory, &config, &config_paths,
                  &embedded, &error_message),
              error_message)) {
    return false;
  }

  return Expect(!embedded, "empty slot was treated as embedded config") &&
         Expect(config_paths.size() == 1,
                "non-embedded startup did not preserve -c config path") &&
         Expect(config.browser.start_page == "https://command.example/app",
                "non-embedded startup did not load command-line config");
}

static bool RunConfigOverrideLoadingTest(
    const std::filesystem::path& test_directory) {
  const auto first_directory = test_directory / "override-first";
  const auto second_directory = test_directory / "override-second";
  std::error_code error;
  if (!Expect(std::filesystem::create_directories(first_directory, error) &&
                  !error,
              "failed to create first override directory") ||
      !Expect(std::filesystem::create_directories(second_directory, error) &&
                  !error,
              "failed to create second override directory")) {
    return false;
  }

  const auto first_path = first_directory / "muon.json";
  const auto second_path = second_directory / "override.json";
  if (!Expect(WriteFile(
                  first_path,
                  R"({"asset":{"sourcePath":"assets-first","signature":"1111111111111111111111111111111111111111","salt":"11"},"log":{"level":"warning","output":{"type":"file","path":"logs/first.log"},"sources":{"console":"error"}},"browser":{"startPage":"https://first.example/app","profilePath":"profiles/first","initialWindowState":"hidden","backgroundColor":"111111","titleBarType":"native","plugin":{"allow":["asset://first/**"]},"keybind":{"devtools":"f12"}},"network":{"allow":["https://first.example/**","data:**"],"authorizedOrigin":[{"scheme":"https","domain":"first.example"},{"scheme":"https","domain":"same.example"}]},"cdp":{"enable":false,"port":9333},"plugin":{"path":"plugins-first","plugins":[{"name":"internal","allow":["muon.fs.*"]}]}})"),
              "failed to write first override config") ||
      !Expect(WriteFile(
                  second_path,
                  R"({"asset":{"sourcePath":"assets-second.zip","signature":"2222222222222222222222222222222222222222","salt":"22ff"},"log":{"level":"debug","sources":{"plugin":"off"}},"browser":{"startPage":"https://second.example/app","initialWindowState":"fullscreen","backgroundColor":"ABCDEF","titleBarType":"muon","plugin":{"allow":["asset://first/**","asset://second/**"]}},"network":{"allow":["data:**","https://second.example/**"],"authorizedOrigin":[{"domain":"same.example","scheme":"https"},{"scheme":"https","domain":"same.example","port":443},{"scheme":"http","domain":"added.example"}]},"cdp":{"enable":true},"plugin":{"path":"plugins-second","plugins":[{"allow":["muon.fs.*"],"name":"internal"},{"name":"foobar","allow":["foobar.*"]}]}})"),
              "failed to write second override config")) {
    return false;
  }

  MuonConfig config;
  if (!LoadConfigFilesExpectSuccess({first_path, second_path}, &config)) {
    return false;
  }

  if (!Expect(config.browser.start_page == "https://second.example/app",
              "later scalar browser.startPage did not override") ||
      !Expect(config.browser.profile == first_directory / "profiles/first",
              "browser.profilePath was not resolved from its config directory") ||
      !Expect(config.browser.initial_window_state ==
                  kMuonBrowserInitialWindowStateFullscreen,
              "later scalar browser.initialWindowState did not override") ||
      !ExpectBrowserBackgroundRgb(
          config.browser.background_color, 0xab, 0xcd, 0xef,
          "later scalar browser.backgroundColor did not override") ||
      !Expect(config.browser.title_bar == kMuonBrowserTitleBarMuon,
              "later scalar browser.titleBarType did not override") ||
      !Expect(config.browser.plugin.allow.size() == 2,
              "browser.plugin.allow array was not merged by equality") ||
      !Expect(config.browser.plugin.allow[0] == "asset://first/**",
              "browser.plugin.allow existing value changed") ||
      !Expect(config.browser.plugin.allow[1] == "asset://second/**",
              "browser.plugin.allow unique value was not appended") ||
      !ExpectShortcut(config.browser.devtools, true, 0x7B, 0,
                      "merged devtools shortcut") ||
      !Expect(config.network.allow.size() == 3,
              "network.allow array was not merged by equality") ||
      !Expect(config.network.allow[0] == "https://first.example/**",
              "network.allow existing first value changed") ||
      !Expect(config.network.allow[1] == "data:**",
              "network.allow duplicate value changed") ||
      !Expect(config.network.allow[2] == "https://second.example/**",
              "network.allow unique value was not appended") ||
      !Expect(config.network.authorized_origin.size() == 4,
              "network.authorizedOrigin array was not merged by equality") ||
      !ExpectAuthorizedOrigin(config.network.authorized_origin[0], "https",
                              "first.example", 0,
                              "first network.authorizedOrigin changed") ||
      !ExpectAuthorizedOrigin(config.network.authorized_origin[1], "https",
                              "same.example", 0,
                              "duplicate network.authorizedOrigin changed") ||
      !ExpectAuthorizedOrigin(config.network.authorized_origin[2], "https",
                              "same.example", 443,
                              "distinct network.authorizedOrigin was not "
                              "appended") ||
      !ExpectAuthorizedOrigin(config.network.authorized_origin[3], "http",
                              "added.example", 0,
                              "appended network.authorizedOrigin") ||
      !ExpectDebuggerConfig(config.cdp, true, 9333,
                            "merged cdp config") ||
      !Expect(config.plugin.path == second_directory / "plugins-second",
              "plugin.path was not resolved from the later config directory") ||
      !Expect(config.plugin.plugins.size() == 2,
              "plugin.plugins array was not merged by equality") ||
      !Expect(config.plugin.plugins[0].name == "internal",
              "plugin.plugins duplicate entry changed") ||
      !Expect(config.plugin.plugins[0].allow.size() == 1,
              "plugin.plugins duplicate entry allow changed") ||
      !Expect(config.plugin.plugins[0].allow[0] == "muon.fs.*",
              "plugin.plugins duplicate entry allow value changed") ||
      !Expect(config.plugin.plugins[1].name == "foobar",
              "plugin.plugins unique entry name changed") ||
      !Expect(config.plugin.plugins[1].allow[0] == "foobar.*",
              "plugin.plugins unique entry allow changed")) {
    return false;
  }
  if (!Expect(config.asset.has_from,
              "merged config did not preserve asset.sourcePath") ||
      !Expect(config.asset.from == second_directory / "assets-second.zip",
              "asset.sourcePath was not resolved from the later config directory") ||
      !Expect(config.asset.has_signature,
              "merged config did not preserve asset.signature") ||
      !Expect(config.asset.signature ==
                  "2222222222222222222222222222222222222222",
              "asset.signature was not overridden by the later config") ||
      !Expect(config.asset.has_salt,
              "merged config did not preserve asset.salt") ||
      !Expect(config.asset.salt.size() == 2,
              "merged asset.salt byte count changed") ||
      !Expect(config.asset.salt[0] == 0x22 && config.asset.salt[1] == 0xff,
              "asset.salt was not overridden by the later config")) {
    return false;
  }

  if (!Expect(config.log.level == kMuonLogLevelDebug,
              "later log.level did not override") ||
      !Expect(config.log.output.type == kMuonLogOutputFile,
              "log.output object was not preserved by merge") ||
      !Expect(config.log.output.path == first_directory / "logs/first.log",
              "log.output.path was not resolved from its config directory") ||
      !Expect(config.log.muon == kMuonLogLevelDebug,
              "merged log.level did not set muon baseline") ||
      !Expect(config.log.cef == kMuonLogLevelDebug,
              "merged log.level did not set cef baseline") ||
      !Expect(config.log.console == kMuonLogLevelError,
              "merged log.sources.console was not preserved") ||
      !Expect(config.log.plugin == kMuonLogLevelOff,
              "merged log.sources.plugin was not applied")) {
    return false;
  }

  const auto explicit_search_directory = test_directory / "explicit-search";
  if (!Expect(std::filesystem::create_directories(explicit_search_directory,
                                                  error) &&
                  !error,
              "failed to create explicit search directory") ||
      !Expect(WriteFile(explicit_search_directory / "muon.jsonc",
                        R"({browser:{startPage:"https://jsonc.example/app"}})"),
              "failed to write explicit search config") ||
      !LoadConfigFilesExpectSuccess(
          {explicit_search_directory / "muon.json"}, &config) ||
      !Expect(config.browser.start_page == "https://jsonc.example/app",
              "explicit muon.json path did not search muon.jsonc")) {
    return false;
  }

  return LoadConfigFilesExpectFailure(
             {test_directory / "missing-explicit.json"}, "does not exist") &&
         LoadConfigFilesExpectFailure(
             {test_directory / "missing-search" / "muon.json"},
             "does not exist");
}

static bool RunBrowserConfigLoadingTest(
    const std::filesystem::path& test_directory) {
  MuonConfig config;
  SetTestDefaultLaunchSource();
  const auto browser_path = test_directory / "browser.json";
  if (!Expect(WriteFile(
                  browser_path,
                  R"({"browser":{"keybind":{"devtools":" f12 ","reload":"F5","hardReload":"ctrl+shift+r","fullscreen":"f11","zoomIn":"ctrl+plus","zoomOut":"ctrl+minus","resetZoom":"ctrl+0","recycle":"ctrl+shift+f10"}}})"),
              "failed to write browser config") ||
      !LoadConfigExpectSuccess(browser_path, &config)) {
    return false;
  }
  if (!ExpectShortcut(config.browser.devtools, true, 0x7B, 0,
                      "f12 devtools shortcut") ||
      !ExpectShortcut(config.browser.reload, true, 0x74, 0,
                      "f5 reload shortcut") ||
      !ExpectShortcut(config.browser.hard_reload, true, 0x52,
                      kMuonShortcutModifierControl |
                          kMuonShortcutModifierShift,
                      "ctrl+shift+r hardReload shortcut") ||
      !ExpectShortcut(config.browser.fullscreen, true, 0x7A, 0,
                      "f11 fullscreen shortcut") ||
      !ExpectShortcut(config.browser.zoom_in, true, 0xBB,
                      kMuonShortcutModifierControl,
                      "ctrl+plus zoomIn shortcut", true) ||
      !ExpectShortcut(config.browser.zoom_out, true, 0xBD,
                      kMuonShortcutModifierControl,
                      "ctrl+minus zoomOut shortcut") ||
      !ExpectShortcut(config.browser.reset_zoom, true, 0x30,
                      kMuonShortcutModifierControl,
                      "ctrl+0 resetZoom shortcut") ||
      !ExpectShortcut(config.browser.recycle, true, 0x79,
                      kMuonShortcutModifierControl |
                          kMuonShortcutModifierShift,
                      "ctrl+shift+f10 recycle shortcut") ||
      !Expect(config.browser.start_page == "asset://main/index.html",
              "browser shortcut config changed default start URL") ||
      !Expect(config.browser.profile == test_directory / ".profile",
              "browser shortcut config changed default profile path") ||
      !Expect(config.browser.plugin.allow.size() == 1,
              "browser shortcut config changed default plugin page allowlist") ||
      !Expect(config.browser.plugin.allow[0] == "asset://main/**",
              "browser shortcut config default plugin page pattern is wrong")) {
    return false;
  }

  struct InitialWindowStateCase {
    const char* value;
    MuonBrowserInitialWindowState expected;
  };
  const InitialWindowStateCase initial_window_state_cases[] = {
      {"normal", kMuonBrowserInitialWindowStateNormal},
      {"hidden", kMuonBrowserInitialWindowStateHidden},
      {"minimized", kMuonBrowserInitialWindowStateMinimized},
      {"maximized", kMuonBrowserInitialWindowStateMaximized},
      {"fullscreen", kMuonBrowserInitialWindowStateFullscreen},
  };
  for (const auto& test_case : initial_window_state_cases) {
    const auto path =
        test_directory /
        (std::string("browser-initial-window-state-") + test_case.value +
         ".json");
    const auto content =
        std::string(R"({"browser":{"initialWindowState":")") +
        test_case.value + R"("}})";
    if (!Expect(WriteFile(path, content),
                "failed to write browser initial window state config") ||
        !LoadConfigExpectSuccess(path, &config) ||
        !Expect(config.browser.initial_window_state == test_case.expected,
                std::string("browser.initialWindowState was not parsed: ") +
                    test_case.value)) {
      return false;
    }
  }

  const auto initial_title_bar_visible_path =
      test_directory / "browser-initial-title-bar-visible.json";
  if (!Expect(WriteFile(initial_title_bar_visible_path,
                        R"({"browser":{"initialTitleBarVisibility":true}})"),
              "failed to write visible initial title bar config") ||
      !LoadConfigExpectSuccess(initial_title_bar_visible_path, &config) ||
      !Expect(config.browser.initial_title_bar_visibility,
              "browser.initialTitleBarVisibility true was not parsed")) {
    return false;
  }

  const auto initial_title_bar_hidden_path =
      test_directory / "browser-initial-title-bar-hidden.json";
  if (!Expect(WriteFile(initial_title_bar_hidden_path,
                        R"({"browser":{"initialTitleBarVisibility":false}})"),
              "failed to write hidden initial title bar config") ||
      !LoadConfigExpectSuccess(initial_title_bar_hidden_path, &config) ||
      !Expect(!config.browser.initial_title_bar_visibility,
              "browser.initialTitleBarVisibility false was not parsed")) {
    return false;
  }

  const auto combo_path = test_directory / "browser-combo.json";
  if (!Expect(WriteFile(
                  combo_path,
                  R"({"browser":{"keybind":{"devtools":" shift + F9 ","reload":"CONTROL + SHIFT + i","zoomIn":"meta + equal"}}})"),
              "failed to write browser combo config") ||
      !LoadConfigExpectSuccess(combo_path, &config)) {
    return false;
  }
  return ExpectShortcut(config.browser.devtools, true, 0x78,
                        kMuonShortcutModifierShift,
                        "shift+f9 devtools shortcut") &&
         ExpectShortcut(config.browser.reload, true, 0x49,
                        kMuonShortcutModifierControl |
                            kMuonShortcutModifierShift,
                        "ctrl+shift+i reload shortcut") &&
         ExpectShortcut(config.browser.zoom_in, true, 0xBB,
                        kMuonShortcutModifierMeta,
                        "meta+equal zoomIn shortcut");
}

static bool RunLogConfigLoadingTest(
    const std::filesystem::path& test_directory) {
  MuonConfig config;
  const auto file_log_path = test_directory / "file-log.json";
  if (!Expect(WriteFile(
                  file_log_path,
                  R"({"log":{"level":"error","output":{"type":"file","path":"logs/muon.log"},"sources":{"console":"debug","plugin":"off"}}})"),
              "failed to write file log config") ||
      !LoadConfigExpectSuccess(file_log_path, &config)) {
    return false;
  }
  if (!Expect(config.log.level == kMuonLogLevelError,
              "log.level was not parsed") ||
      !Expect(config.log.output.type == kMuonLogOutputFile,
              "log.output.type file was not parsed") ||
      !Expect(config.log.output.path == test_directory / "logs/muon.log",
              "log.output.path was not parsed") ||
      !Expect(config.log.muon == kMuonLogLevelError,
              "log.level did not set muon source baseline") ||
      !Expect(config.log.cef == kMuonLogLevelError,
              "log.level did not set cef source baseline") ||
      !Expect(config.log.console == kMuonLogLevelDebug,
              "log.sources.console did not override baseline") ||
      !Expect(config.log.plugin == kMuonLogLevelOff,
              "log.sources.plugin did not parse off")) {
    return false;
  }

  const auto partial_sources_path = test_directory / "log-sources.json";
  if (!Expect(WriteFile(partial_sources_path,
                        R"({"log":{"sources":{"plugin":"debug"}}})"),
              "failed to write partial log sources config") ||
      !LoadConfigExpectSuccess(partial_sources_path, &config)) {
    return false;
  }
  return Expect(config.log.level == kMuonLogLevelInfo,
                "partial sources changed global log level") &&
         Expect(config.log.muon == kMuonLogLevelInfo,
                "partial sources did not use global muon baseline") &&
         Expect(config.log.cef == kMuonLogLevelInfo,
                "partial sources did not use global cef baseline") &&
         Expect(config.log.console == kMuonLogLevelInfo,
                "partial sources did not use global console baseline") &&
         Expect(config.log.plugin == kMuonLogLevelDebug,
                "partial sources plugin override was not parsed");
}

static bool RunConfigValidationTest(
    const std::filesystem::path& test_directory) {
  const auto invalid_json_path = test_directory / "invalid-json.json";
  const auto invalid_bootstrap_path =
      test_directory / "invalid-bootstrap.json";
  const auto invalid_network_path = test_directory / "invalid-network.json";
  const auto invalid_allow_path = test_directory / "invalid-allow.json";
  const auto invalid_entry_path = test_directory / "invalid-entry.json";
  const auto invalid_default_version_policy_type_path =
      test_directory / "invalid-default-version-policy-type.json";
  const auto invalid_default_version_policy_path =
      test_directory / "invalid-default-version-policy.json";
  const auto invalid_desktop_id_type_path =
      test_directory / "invalid-desktop-id-type.json";
  const auto empty_desktop_id_path =
      test_directory / "empty-desktop-id.json";
  const auto invalid_authorized_origin_path =
      test_directory / "invalid-authorized-origin.json";
  const auto invalid_authorized_origin_entry_path =
      test_directory / "invalid-authorized-origin-entry.json";
  const auto missing_authorized_origin_scheme_path =
      test_directory / "missing-authorized-origin-scheme.json";
  const auto invalid_authorized_origin_scheme_path =
      test_directory / "invalid-authorized-origin-scheme.json";
  const auto missing_authorized_origin_domain_path =
      test_directory / "missing-authorized-origin-domain.json";
  const auto invalid_authorized_origin_domain_path =
      test_directory / "invalid-authorized-origin-domain.json";
  const auto invalid_authorized_origin_port_path =
      test_directory / "invalid-authorized-origin-port.json";
  const auto out_of_range_authorized_origin_port_path =
      test_directory / "out-of-range-authorized-origin-port.json";
  const auto invalid_plugin_path = test_directory / "invalid-plugin.json";
  const auto legacy_plugins_path = test_directory / "legacy-plugins.json";
  const auto invalid_plugin_path_type_path =
      test_directory / "invalid-plugin-path-type.json";
  const auto empty_plugin_path_path =
      test_directory / "empty-plugin-path.json";
  const auto invalid_plugins_path = test_directory / "invalid-plugins.json";
  const auto invalid_plugin_entry_object_path =
      test_directory / "invalid-plugin-entry-object.json";
  const auto missing_plugin_name_path =
      test_directory / "missing-plugin-name.json";
  const auto invalid_plugin_name_type_path =
      test_directory / "invalid-plugin-name-type.json";
  const auto invalid_plugin_name_path =
      test_directory / "invalid-plugin-name.json";
  const auto duplicate_plugin_path =
      test_directory / "duplicate-plugin.json";
  const auto missing_plugin_allow_path =
      test_directory / "missing-plugin-allow.json";
  const auto invalid_plugin_allow_path =
      test_directory / "invalid-plugin-allow.json";
  const auto invalid_plugin_entry_path =
      test_directory / "invalid-plugin-entry.json";
  const auto invalid_asset_path = test_directory / "invalid-asset.json";
  const auto invalid_asset_from_type_path =
      test_directory / "invalid-asset-from-type.json";
  const auto empty_asset_from_path =
      test_directory / "empty-asset-from.json";
  const auto invalid_asset_signature_type_path =
      test_directory / "invalid-asset-signature-type.json";
  const auto short_asset_signature_path =
      test_directory / "short-asset-signature.json";
  const auto long_asset_signature_path =
      test_directory / "long-asset-signature.json";
  const auto non_hex_asset_signature_path =
      test_directory / "non-hex-asset-signature.json";
  const auto invalid_asset_salt_type_path =
      test_directory / "invalid-asset-salt-type.json";
  const auto odd_asset_salt_path =
      test_directory / "odd-asset-salt.json";
  const auto non_hex_asset_salt_path =
      test_directory / "non-hex-asset-salt.json";
  return Expect(WriteFile(invalid_json_path, "{"),
                "failed to write invalid JSON config") &&
         Expect(WriteFile(invalid_bootstrap_path, R"({"bootstrap":true})"),
                "failed to write invalid bootstrap config") &&
         Expect(WriteFile(invalid_network_path, R"({"network":true})"),
                "failed to write invalid network config") &&
         Expect(WriteFile(invalid_allow_path,
                          R"({"network":{"allow":"data:**"}})"),
                "failed to write invalid allow config") &&
         Expect(WriteFile(invalid_entry_path,
                          R"({"network":{"allow":["data:**",42]}})"),
                "failed to write invalid allow entry config") &&
         Expect(WriteFile(invalid_default_version_policy_type_path,
                          R"({"bootstrap":{"defaultVersionPolicy":42}})"),
                "failed to write invalid defaultVersionPolicy type config") &&
         Expect(WriteFile(invalid_default_version_policy_path,
                          R"({"bootstrap":{"defaultVersionPolicy":"invalid"}})"),
                "failed to write invalid defaultVersionPolicy config") &&
         Expect(WriteFile(invalid_desktop_id_type_path,
                          R"({"bootstrap":{"desktopId":42}})"),
                "failed to write invalid desktopId type config") &&
         Expect(WriteFile(empty_desktop_id_path,
                          R"({"bootstrap":{"desktopId":"   "}})"),
                "failed to write empty desktopId config") &&
         Expect(WriteFile(invalid_authorized_origin_path,
                          R"({"network":{"authorizedOrigin":true}})"),
                "failed to write invalid authorizedOrigin config") &&
         Expect(WriteFile(invalid_authorized_origin_entry_path,
                          R"({"network":{"authorizedOrigin":[true]}})"),
                "failed to write invalid authorizedOrigin entry config") &&
         Expect(WriteFile(missing_authorized_origin_scheme_path,
                          R"({"network":{"authorizedOrigin":[{"domain":"login.live.com"}]}})"),
                "failed to write missing authorizedOrigin scheme config") &&
         Expect(WriteFile(invalid_authorized_origin_scheme_path,
                          R"({"network":{"authorizedOrigin":[{"scheme":42,"domain":"login.live.com"}]}})"),
                "failed to write invalid authorizedOrigin scheme config") &&
         Expect(WriteFile(missing_authorized_origin_domain_path,
                          R"({"network":{"authorizedOrigin":[{"scheme":"https"}]}})"),
                "failed to write missing authorizedOrigin domain config") &&
         Expect(WriteFile(invalid_authorized_origin_domain_path,
                          R"({"network":{"authorizedOrigin":[{"scheme":"https","domain":"*.live.com"}]}})"),
                "failed to write invalid authorizedOrigin domain config") &&
         Expect(WriteFile(invalid_authorized_origin_port_path,
                          R"({"network":{"authorizedOrigin":[{"scheme":"https","domain":"login.live.com","port":"443"}]}})"),
                "failed to write invalid authorizedOrigin port config") &&
         Expect(WriteFile(out_of_range_authorized_origin_port_path,
                          R"({"network":{"authorizedOrigin":[{"scheme":"https","domain":"login.live.com","port":65536}]}})"),
                "failed to write out-of-range authorizedOrigin port config") &&
         Expect(WriteFile(invalid_plugin_path, R"({"plugin":true})"),
                "failed to write invalid plugin config") &&
         Expect(WriteFile(legacy_plugins_path, R"({"plugins":[]})"),
                "failed to write legacy plugins config") &&
         Expect(WriteFile(invalid_plugin_path_type_path,
                          R"({"plugin":{"path":42}})"),
                "failed to write invalid plugin.path type config") &&
         Expect(WriteFile(empty_plugin_path_path,
                          R"({"plugin":{"path":""}})"),
                "failed to write empty plugin.path config") &&
         Expect(WriteFile(invalid_plugins_path,
                          R"({"plugin":{"plugins":true}})"),
                "failed to write invalid plugins config") &&
         Expect(WriteFile(invalid_plugin_entry_object_path,
                          R"({"plugin":{"plugins":[true]}})"),
                "failed to write invalid plugin entry object config") &&
         Expect(WriteFile(missing_plugin_name_path,
                          R"({"plugin":{"plugins":[{"allow":["muon.**"]}]}})"),
                "failed to write missing plugin name config") &&
         Expect(WriteFile(invalid_plugin_name_type_path,
                          R"({"plugin":{"plugins":[{"name":42,"allow":["muon.**"]}]}})"),
                "failed to write invalid plugin name type config") &&
         Expect(WriteFile(invalid_plugin_name_path,
                          R"({"plugin":{"plugins":[{"name":"../bad","allow":["muon.**"]}]}})"),
                "failed to write invalid plugin name config") &&
         Expect(WriteFile(duplicate_plugin_path,
                          R"({"plugin":{"plugins":[{"name":"internal","allow":["muon.**"]},{"name":"internal","allow":["muon.fs.*"]}]}})"),
                "failed to write duplicate plugin config") &&
         Expect(WriteFile(missing_plugin_allow_path,
                          R"({"plugin":{"plugins":[{"name":"internal"}]}})"),
                "failed to write missing plugin allow config") &&
         Expect(WriteFile(invalid_plugin_allow_path,
                          R"({"plugin":{"plugins":[{"name":"internal","allow":"muon.**"}]}})"),
                "failed to write invalid plugin allow config") &&
         Expect(WriteFile(invalid_plugin_entry_path,
                          R"({"plugin":{"plugins":[{"name":"internal","allow":["muon.**",42]}]}})"),
                "failed to write invalid plugin allow entry config") &&
         Expect(WriteFile(invalid_asset_path, R"({"asset":true})"),
                "failed to write invalid asset config") &&
         Expect(WriteFile(invalid_asset_from_type_path,
                          R"({"asset":{"sourcePath":42}})"),
                "failed to write invalid asset.sourcePath type config") &&
         Expect(WriteFile(empty_asset_from_path,
                          R"({"asset":{"sourcePath":""}})"),
                "failed to write empty asset.sourcePath config") &&
         Expect(WriteFile(invalid_asset_signature_type_path,
                          R"({"asset":{"signature":42}})"),
                "failed to write invalid asset.signature type config") &&
         Expect(WriteFile(short_asset_signature_path,
                          R"({"asset":{"signature":"a9993e364706816aba3e25717850c26c9cd0d89"}})"),
                "failed to write short asset.signature config") &&
         Expect(WriteFile(long_asset_signature_path,
                          R"({"asset":{"signature":"a9993e364706816aba3e25717850c26c9cd0d89d0"}})"),
                "failed to write long asset.signature config") &&
         Expect(WriteFile(non_hex_asset_signature_path,
                          R"({"asset":{"signature":"a9993e364706816aba3e25717850c26c9cd0d89x"}})"),
                "failed to write non-hex asset.signature config") &&
         Expect(WriteFile(invalid_asset_salt_type_path,
                          R"({"asset":{"salt":42}})"),
                "failed to write invalid asset.salt type config") &&
         Expect(WriteFile(odd_asset_salt_path,
                          R"({"asset":{"salt":"abc"}})"),
                "failed to write odd asset.salt config") &&
         Expect(WriteFile(non_hex_asset_salt_path,
                          R"({"asset":{"salt":"0x"}})"),
                "failed to write non-hex asset.salt config") &&
         LoadConfigExpectFailure(invalid_json_path, "Invalid muon.json") &&
         LoadConfigExpectFailure(invalid_bootstrap_path,
                                 "bootstrap must be an object") &&
         LoadConfigExpectFailure(invalid_network_path,
                                 "network must be an object") &&
         LoadConfigExpectFailure(invalid_allow_path,
                                 "network.allow must be an array") &&
         LoadConfigExpectFailure(invalid_entry_path,
                                 "network.allow entries must be strings") &&
         LoadConfigExpectFailure(invalid_default_version_policy_type_path,
                                 "bootstrap.defaultVersionPolicy must be a "
                                 "string") &&
         LoadConfigExpectFailure(invalid_default_version_policy_path,
                                 "bootstrap.defaultVersionPolicy has unknown "
                                 "value") &&
         LoadConfigExpectFailure(invalid_desktop_id_type_path,
                                 "bootstrap.desktopId must be a string") &&
         LoadConfigExpectFailure(empty_desktop_id_path,
                                 "bootstrap.desktopId must not be empty") &&
         LoadConfigExpectFailure(invalid_authorized_origin_path,
                                 "network.authorizedOrigin must be an array") &&
         LoadConfigExpectFailure(invalid_authorized_origin_entry_path,
                                 "network.authorizedOrigin[0] must be an "
                                 "object") &&
         LoadConfigExpectFailure(missing_authorized_origin_scheme_path,
                                 "network.authorizedOrigin[0].scheme is "
                                 "required") &&
         LoadConfigExpectFailure(invalid_authorized_origin_scheme_path,
                                 "network.authorizedOrigin[0].scheme must be "
                                 "a string") &&
         LoadConfigExpectFailure(missing_authorized_origin_domain_path,
                                 "network.authorizedOrigin[0].domain is "
                                 "required") &&
         LoadConfigExpectFailure(invalid_authorized_origin_domain_path,
                                 "without separators or wildcards") &&
         LoadConfigExpectFailure(invalid_authorized_origin_port_path,
                                 "network.authorizedOrigin[0].port must be an "
                                 "integer") &&
         LoadConfigExpectFailure(out_of_range_authorized_origin_port_path,
                                 "network.authorizedOrigin[0].port must be an "
                                 "integer") &&
         LoadConfigExpectFailure(invalid_plugin_path,
                                 "plugin must be an object") &&
         LoadConfigExpectFailure(legacy_plugins_path,
                                 "plugins is no longer supported") &&
         LoadConfigExpectFailure(invalid_plugin_path_type_path,
                                 "plugin.path must be a string") &&
         LoadConfigExpectFailure(empty_plugin_path_path,
                                 "plugin.path must not be empty") &&
         LoadConfigExpectFailure(invalid_plugins_path,
                                 "plugin.plugins must be an array") &&
         LoadConfigExpectFailure(invalid_plugin_entry_object_path,
                                 "plugin.plugins[0] must be an object") &&
         LoadConfigExpectFailure(missing_plugin_name_path,
                                 "plugin.plugins[0].name is required") &&
         LoadConfigExpectFailure(invalid_plugin_name_type_path,
                                 "plugin.plugins[0].name must be a string") &&
         LoadConfigExpectFailure(invalid_plugin_name_path,
                                 "plugin.plugins entry name must be a file "
                                 "name stem") &&
         LoadConfigExpectFailure(duplicate_plugin_path,
                                 "duplicate plugin entry") &&
         LoadConfigExpectFailure(missing_plugin_allow_path,
                                 "plugin.plugins[0].allow is required") &&
         LoadConfigExpectFailure(invalid_plugin_allow_path,
                                 "plugin.plugins[0].allow must be an array") &&
         LoadConfigExpectFailure(invalid_plugin_entry_path,
                                 "plugin.plugins[0].allow entries must be strings") &&
         LoadConfigExpectFailure(invalid_asset_path,
                                 "asset must be an object") &&
         LoadConfigExpectFailure(invalid_asset_from_type_path,
                                 "asset.sourcePath must be a string") &&
         LoadConfigExpectFailure(empty_asset_from_path,
                                 "asset.sourcePath must not be empty") &&
         LoadConfigExpectFailure(invalid_asset_signature_type_path,
                                 "asset.signature must be a string") &&
         LoadConfigExpectFailure(short_asset_signature_path,
                                 "asset.signature must be a 40-character "
                                 "SHA-1 hex string") &&
         LoadConfigExpectFailure(long_asset_signature_path,
                                 "asset.signature must be a 40-character "
                                 "SHA-1 hex string") &&
         LoadConfigExpectFailure(non_hex_asset_signature_path,
                                 "asset.signature must be a 40-character "
                                 "SHA-1 hex string") &&
         LoadConfigExpectFailure(invalid_asset_salt_type_path,
                                 "asset.salt must be a string") &&
         LoadConfigExpectFailure(odd_asset_salt_path,
                                 "asset.salt must be a hexadecimal byte "
                                 "string") &&
         LoadConfigExpectFailure(non_hex_asset_salt_path,
                                 "asset.salt must be a hexadecimal byte "
                                 "string");
}

static bool RunLogConfigValidationTest(
    const std::filesystem::path& test_directory) {
  const auto invalid_log_path = test_directory / "invalid-log.json";
  const auto invalid_level_path = test_directory / "invalid-log-level.json";
  const auto unknown_level_path =
      test_directory / "unknown-log-level.json";
  const auto invalid_output_path = test_directory / "invalid-log-output.json";
  const auto invalid_output_type_path =
      test_directory / "invalid-log-output-type.json";
  const auto unknown_output_type_path =
      test_directory / "unknown-log-output-type.json";
  const auto missing_file_path =
      test_directory / "missing-log-file-path.json";
  const auto non_file_path_path =
      test_directory / "non-file-log-path.json";
  const auto invalid_sources_path =
      test_directory / "invalid-log-sources.json";
  const auto unknown_source_path =
      test_directory / "unknown-log-source.json";
  const auto invalid_source_level_path =
      test_directory / "invalid-log-source-level.json";
  const auto unsupported_platform_output_path =
      test_directory / "unsupported-log-output.json";
  return Expect(WriteFile(invalid_log_path, R"({"log":true})"),
                "failed to write invalid log config") &&
         Expect(WriteFile(invalid_level_path, R"({"log":{"level":1}})"),
                "failed to write invalid log level config") &&
         Expect(WriteFile(unknown_level_path,
                          R"({"log":{"level":"trace"}})"),
                "failed to write unknown log level config") &&
         Expect(WriteFile(invalid_output_path,
                          R"({"log":{"output":true}})"),
                "failed to write invalid log output config") &&
         Expect(WriteFile(invalid_output_type_path,
                          R"({"log":{"output":{"type":1}}})"),
                "failed to write invalid log output type config") &&
         Expect(WriteFile(unknown_output_type_path,
                          R"({"log":{"output":{"type":"journald"}}})"),
                "failed to write unknown log output type config") &&
         Expect(WriteFile(missing_file_path,
                          R"({"log":{"output":{"type":"file"}}})"),
                "failed to write missing log file path config") &&
         Expect(WriteFile(non_file_path_path,
                          R"({"log":{"output":{"type":"stderr","path":"muon.log"}}})"),
                "failed to write non-file path config") &&
         Expect(WriteFile(invalid_sources_path,
                          R"({"log":{"sources":true}})"),
                "failed to write invalid log sources config") &&
         Expect(WriteFile(unknown_source_path,
                          R"({"log":{"sources":{"network":"debug"}}})"),
                "failed to write unknown log source config") &&
         Expect(WriteFile(invalid_source_level_path,
                          R"({"log":{"sources":{"console":1}}})"),
                "failed to write invalid log source level config") &&
         Expect(WriteFile(unsupported_platform_output_path,
                          R"({"log":{"output":{"type":"debug"}}})"),
                "failed to write unsupported log output config") &&
         LoadConfigExpectFailure(invalid_log_path,
                                 "log must be an object") &&
         LoadConfigExpectFailure(invalid_level_path,
                                 "log.level must be a string") &&
         LoadConfigExpectFailure(unknown_level_path,
                                 "log.level has unknown level") &&
         LoadConfigExpectFailure(invalid_output_path,
                                 "log.output must be an object") &&
         LoadConfigExpectFailure(invalid_output_type_path,
                                 "log.output.type must be a string") &&
         LoadConfigExpectFailure(unknown_output_type_path,
                                 "log.output.type is unknown") &&
         LoadConfigExpectFailure(missing_file_path,
                                 "log.output.path is required") &&
         LoadConfigExpectFailure(non_file_path_path,
                                 "log.output.path is only valid") &&
         LoadConfigExpectFailure(invalid_sources_path,
                                 "log.sources must be an object") &&
         LoadConfigExpectFailure(unknown_source_path,
                                 "log.sources has unknown source") &&
         LoadConfigExpectFailure(invalid_source_level_path,
                                 "log.sources.console must be a string") &&
         LoadConfigExpectFailure(unsupported_platform_output_path,
                                 "only supported on win32");
}

static bool RunDebuggerConfigValidationTest(
    const std::filesystem::path& test_directory) {
  const auto invalid_debugger_path = test_directory / "invalid-cdp.json";
  const auto invalid_debugger_enable_path =
      test_directory / "invalid-cdp-enable.json";
  const auto invalid_debugger_port_path =
      test_directory / "invalid-cdp-port.json";
  const auto out_of_range_debugger_port_path =
      test_directory / "out-of-range-cdp-port.json";
  return Expect(WriteFile(invalid_debugger_path, R"({"cdp":true})"),
                "failed to write invalid cdp config") &&
         Expect(WriteFile(invalid_debugger_enable_path,
                          R"({"cdp":{"enable":"true"}})"),
                "failed to write invalid cdp enable config") &&
         Expect(WriteFile(invalid_debugger_port_path,
                          R"({"cdp":{"port":"9222"}})"),
                "failed to write invalid cdp port config") &&
         Expect(WriteFile(out_of_range_debugger_port_path,
                          R"({"cdp":{"port":1023}})"),
                "failed to write out-of-range cdp port config") &&
         LoadConfigExpectFailure(invalid_debugger_path,
                                 "cdp must be an object") &&
         LoadConfigExpectFailure(invalid_debugger_enable_path,
                                 "cdp.enable must be a boolean") &&
         LoadConfigExpectFailure(invalid_debugger_port_path,
                                 "cdp.port must be an integer") &&
         LoadConfigExpectFailure(out_of_range_debugger_port_path,
                                 "cdp.port must be an integer");
}

static bool RunBrowserConfigValidationTest(
    const std::filesystem::path& test_directory) {
  const auto invalid_browser_path = test_directory / "invalid-browser.json";
  const auto invalid_keybinds_path =
      test_directory / "invalid-browser-keybind.json";
  const auto invalid_devtools_path =
      test_directory / "invalid-browser-devtools.json";
  const auto invalid_zoom_path =
      test_directory / "invalid-browser-zoom.json";
  const auto invalid_recycle_path =
      test_directory / "invalid-browser-recycle.json";
  const auto empty_shortcut_path =
      test_directory / "invalid-browser-empty-shortcut.json";
  const auto unknown_key_path =
      test_directory / "invalid-browser-unknown-key.json";
  const auto duplicate_modifier_path =
      test_directory / "invalid-browser-duplicate-modifier.json";
  const auto missing_key_path =
      test_directory / "invalid-browser-missing-key.json";
  const auto multiple_keys_path =
      test_directory / "invalid-browser-multiple-keys.json";
  const auto duplicate_assignment_path =
      test_directory / "invalid-browser-duplicate-assignment.json";
  const auto overlapping_assignment_path =
      test_directory / "invalid-browser-overlapping-assignment.json";
  const auto invalid_browser_plugin_path =
      test_directory / "invalid-browser-plugin.json";
  const auto invalid_browser_plugin_allow_path =
      test_directory / "invalid-browser-plugin-allow.json";
  const auto invalid_browser_plugin_entry_path =
      test_directory / "invalid-browser-plugin-entry.json";
  const auto invalid_unsafe_parent_access_path =
      test_directory / "invalid-unsafe-parent-access.json";
  const auto invalid_unsafe_parent_access_entry_path =
      test_directory / "invalid-unsafe-parent-access-entry.json";
  const auto invalid_browser_start_path =
      test_directory / "invalid-browser-start.json";
  const auto empty_browser_start_path =
      test_directory / "empty-browser-start.json";
  const auto invalid_browser_profile_path =
      test_directory / "invalid-browser-profile.json";
  const auto empty_browser_profile_path =
      test_directory / "empty-browser-profile.json";
  const auto invalid_initial_window_state_path =
      test_directory / "invalid-browser-initial-window-state.json";
  const auto empty_initial_window_state_path =
      test_directory / "empty-browser-initial-window-state.json";
  const auto unknown_initial_window_state_path =
      test_directory / "unknown-browser-initial-window-state.json";
  const auto invalid_initial_title_bar_visibility_path =
      test_directory / "invalid-browser-initial-title-bar-visibility.json";
  const auto invalid_initial_title_bar_icon_path =
      test_directory / "invalid-browser-initial-title-bar-icon.json";
  const auto empty_initial_title_bar_icon_path =
      test_directory / "empty-browser-initial-title-bar-icon.json";
  const auto invalid_background_color_path =
      test_directory / "invalid-browser-background-color.json";
  const auto empty_background_color_path =
      test_directory / "empty-browser-background-color.json";
  const auto short_background_color_path =
      test_directory / "short-browser-background-color.json";
  const auto alpha_background_color_path =
      test_directory / "alpha-browser-background-color.json";
  const auto named_background_color_path =
      test_directory / "named-browser-background-color.json";
  const auto invalid_title_bar_path =
      test_directory / "invalid-browser-title-bar.json";
  const auto empty_title_bar_path =
      test_directory / "empty-browser-title-bar.json";
  const auto unknown_title_bar_path =
      test_directory / "unknown-browser-title-bar.json";
  return Expect(WriteFile(invalid_browser_path, R"({"browser":true})"),
                "failed to write invalid browser config") &&
         Expect(WriteFile(invalid_browser_start_path,
                          R"({"browser":{"startPage":42}})"),
                "failed to write invalid browser start config") &&
         Expect(WriteFile(empty_browser_start_path,
                          R"({"browser":{"startPage":""}})"),
                "failed to write empty browser start config") &&
         Expect(WriteFile(invalid_browser_profile_path,
                          R"({"browser":{"profilePath":42}})"),
                "failed to write invalid browser profile config") &&
         Expect(WriteFile(empty_browser_profile_path,
                          R"({"browser":{"profilePath":""}})"),
                "failed to write empty browser profile config") &&
         Expect(WriteFile(invalid_initial_window_state_path,
                          R"({"browser":{"initialWindowState":42}})"),
                "failed to write invalid initial window state config") &&
         Expect(WriteFile(empty_initial_window_state_path,
                          R"({"browser":{"initialWindowState":""}})"),
                "failed to write empty initial window state config") &&
         Expect(WriteFile(unknown_initial_window_state_path,
                          R"({"browser":{"initialWindowState":"iconified"}})"),
                "failed to write unknown initial window state config") &&
         Expect(WriteFile(invalid_initial_title_bar_visibility_path,
                          R"({"browser":{"initialTitleBarVisibility":"hidden"}})"),
                "failed to write invalid initial title bar visibility config") &&
         Expect(WriteFile(invalid_initial_title_bar_icon_path,
                          R"({"browser":{"initialTitleBarIcon":42}})"),
                "failed to write invalid initial title bar icon config") &&
         Expect(WriteFile(empty_initial_title_bar_icon_path,
                          R"({"browser":{"initialTitleBarIcon":""}})"),
                "failed to write empty initial title bar icon config") &&
         Expect(WriteFile(invalid_background_color_path,
                          R"({"browser":{"backgroundColor":42}})"),
                "failed to write invalid background color config") &&
         Expect(WriteFile(empty_background_color_path,
                          R"({"browser":{"backgroundColor":""}})"),
                "failed to write empty background color config") &&
         Expect(WriteFile(short_background_color_path,
                          R"({"browser":{"backgroundColor":"#123"}})"),
                "failed to write short background color config") &&
         Expect(WriteFile(alpha_background_color_path,
                          R"({"browser":{"backgroundColor":"ff123456"}})"),
                "failed to write alpha background color config") &&
         Expect(WriteFile(named_background_color_path,
                          R"({"browser":{"backgroundColor":"black"}})"),
                "failed to write named background color config") &&
         Expect(WriteFile(invalid_title_bar_path,
                          R"({"browser":{"titleBarType":42}})"),
                "failed to write invalid title bar config") &&
         Expect(WriteFile(empty_title_bar_path,
                          R"({"browser":{"titleBarType":""}})"),
                "failed to write empty title bar config") &&
         Expect(WriteFile(unknown_title_bar_path,
                          R"({"browser":{"titleBarType":"system"}})"),
                "failed to write unknown title bar config") &&
         Expect(WriteFile(invalid_keybinds_path,
                          R"({"browser":{"keybind":true}})"),
                "failed to write invalid browser keybind config") &&
         Expect(WriteFile(invalid_devtools_path,
                          R"({"browser":{"keybind":{"devtools":true}}})"),
                "failed to write invalid devtools config") &&
         Expect(WriteFile(invalid_zoom_path,
                          R"({"browser":{"keybind":{"zoomIn":true}}})"),
                "failed to write invalid zoom config") &&
         Expect(WriteFile(invalid_recycle_path,
                          R"({"browser":{"keybind":{"recycle":true}}})"),
                "failed to write invalid recycle config") &&
         Expect(WriteFile(empty_shortcut_path,
                          R"({"browser":{"keybind":{"devtools":" "}}})"),
                "failed to write empty shortcut config") &&
         Expect(WriteFile(unknown_key_path,
                          R"({"browser":{"keybind":{"devtools":"ctrl+comma"}}})"),
                "failed to write unknown key config") &&
         Expect(WriteFile(duplicate_modifier_path,
                          R"({"browser":{"keybind":{"devtools":"ctrl+control+i"}}})"),
                "failed to write duplicate modifier config") &&
         Expect(WriteFile(missing_key_path,
                          R"({"browser":{"keybind":{"devtools":"shift+ctrl"}}})"),
                "failed to write missing key config") &&
         Expect(WriteFile(multiple_keys_path,
                          R"({"browser":{"keybind":{"devtools":"f5+r"}}})"),
                "failed to write multiple keys config") &&
         Expect(WriteFile(
                    duplicate_assignment_path,
                    R"({"browser":{"keybind":{"devtools":"shift+f9","recycle":"SHIFT + F9"}}})"),
                "failed to write duplicate assignment config") &&
         Expect(WriteFile(
                    overlapping_assignment_path,
                    R"({"browser":{"keybind":{"zoomIn":"ctrl+plus","resetZoom":"ctrl+equal"}}})"),
                "failed to write overlapping assignment config") &&
         Expect(WriteFile(invalid_browser_plugin_path,
                          R"({"browser":{"plugin":true}})"),
                "failed to write invalid browser plugin config") &&
         Expect(WriteFile(invalid_browser_plugin_allow_path,
                          R"({"browser":{"plugin":{"allow":"asset://main/**"}}})"),
                "failed to write invalid browser plugin allow config") &&
         Expect(WriteFile(invalid_browser_plugin_entry_path,
                          R"({"browser":{"plugin":{"allow":["asset://main/**",42]}}})"),
                "failed to write invalid browser plugin allow entry config") &&
         Expect(WriteFile(invalid_unsafe_parent_access_path,
                          R"({"browser":{"allowUnsafeJavaScriptParentAccess":"asset://main/**"}})"),
                "failed to write invalid unsafe parent access config") &&
         Expect(WriteFile(invalid_unsafe_parent_access_entry_path,
                          R"({"browser":{"allowUnsafeJavaScriptParentAccess":["asset://main/**",42]}})"),
                "failed to write invalid unsafe parent access entry config") &&
         LoadConfigExpectFailure(invalid_browser_path,
                                 "browser must be an object") &&
         LoadConfigExpectFailure(invalid_browser_start_path,
                                 "browser.startPage must be a string") &&
         LoadConfigExpectFailure(empty_browser_start_path,
                                 "browser.startPage must not be empty") &&
         LoadConfigExpectFailure(invalid_browser_profile_path,
                                 "browser.profilePath must be a string") &&
         LoadConfigExpectFailure(empty_browser_profile_path,
                                 "browser.profilePath must not be empty") &&
         LoadConfigExpectFailure(invalid_initial_window_state_path,
                                 "browser.initialWindowState must be a "
                                 "string") &&
         LoadConfigExpectFailure(empty_initial_window_state_path,
                                 "browser.initialWindowState must not be "
                                 "empty") &&
         LoadConfigExpectFailure(unknown_initial_window_state_path,
                                 "browser.initialWindowState has unknown "
                                 "value") &&
         LoadConfigExpectFailure(invalid_initial_title_bar_visibility_path,
                                 "browser.initialTitleBarVisibility must be a "
                                 "boolean") &&
         LoadConfigExpectFailure(invalid_initial_title_bar_icon_path,
                                 "browser.initialTitleBarIcon must be a "
                                 "string") &&
         LoadConfigExpectFailure(empty_initial_title_bar_icon_path,
                                 "browser.initialTitleBarIcon must not be "
                                 "empty") &&
         LoadConfigExpectFailure(invalid_background_color_path,
                                 "browser.backgroundColor must be a string") &&
         LoadConfigExpectFailure(empty_background_color_path,
                                 "browser.backgroundColor must not be empty") &&
         LoadConfigExpectFailure(short_background_color_path,
                                 "browser.backgroundColor has unknown value") &&
         LoadConfigExpectFailure(alpha_background_color_path,
                                 "browser.backgroundColor has unknown value") &&
         LoadConfigExpectFailure(named_background_color_path,
                                 "browser.backgroundColor has unknown value") &&
         LoadConfigExpectFailure(invalid_title_bar_path,
                                 "browser.titleBarType must be a string") &&
         LoadConfigExpectFailure(empty_title_bar_path,
                                 "browser.titleBarType must not be empty") &&
         LoadConfigExpectFailure(unknown_title_bar_path,
                                 "browser.titleBarType has unknown value") &&
         LoadConfigExpectFailure(invalid_keybinds_path,
                                 "browser.keybind must be an object") &&
         LoadConfigExpectFailure(invalid_devtools_path,
                                 "browser.keybind.devtools must be a string") &&
         LoadConfigExpectFailure(invalid_zoom_path,
                                 "browser.keybind.zoomIn must be a string") &&
         LoadConfigExpectFailure(invalid_recycle_path,
                                 "browser.keybind.recycle must be a string") &&
         LoadConfigExpectFailure(empty_shortcut_path,
                                 "shortcut must not be empty") &&
         LoadConfigExpectFailure(unknown_key_path, "unsupported key") &&
         LoadConfigExpectFailure(duplicate_modifier_path,
                                 "duplicate modifier") &&
         LoadConfigExpectFailure(missing_key_path, "must include a key") &&
         LoadConfigExpectFailure(multiple_keys_path, "multiple keys") &&
         LoadConfigExpectFailure(duplicate_assignment_path,
                                 "must not use overlapping shortcuts") &&
         LoadConfigExpectFailure(overlapping_assignment_path,
                                 "must not use overlapping shortcuts") &&
         LoadConfigExpectFailure(invalid_browser_plugin_path,
                                 "browser.plugin must be an object") &&
         LoadConfigExpectFailure(invalid_browser_plugin_allow_path,
                                 "browser.plugin.allow must be an array") &&
         LoadConfigExpectFailure(invalid_browser_plugin_entry_path,
                                 "browser.plugin.allow entries must be strings") &&
         LoadConfigExpectFailure(
             invalid_unsafe_parent_access_path,
             "browser.allowUnsafeJavaScriptParentAccess must be an array") &&
         LoadConfigExpectFailure(
             invalid_unsafe_parent_access_entry_path,
             "browser.allowUnsafeJavaScriptParentAccess entries must be "
             "strings");
}

static bool RunBrowserBackgroundColorResolutionTest() {
  MuonBrowserBackgroundColorConfig rgb_config;
  rgb_config.mode = kMuonBrowserBackgroundColorRgb;
  rgb_config.red = 0x12;
  rgb_config.green = 0x34;
  rgb_config.blue = 0x56;
  const auto rgb_resolved = ResolveMuonBrowserBackgroundColorForSystemScheme(
      rgb_config, kMuonSystemColorSchemeUnknown);

  MuonBrowserBackgroundColorConfig system_config;
  system_config.mode = kMuonBrowserBackgroundColorSystem;
  const auto system_dark_resolved =
      ResolveMuonBrowserBackgroundColorForSystemScheme(
          system_config, kMuonSystemColorSchemeDark);
  const auto system_light_resolved =
      ResolveMuonBrowserBackgroundColorForSystemScheme(
          system_config, kMuonSystemColorSchemeLight);
  const auto system_unknown_resolved =
      ResolveMuonBrowserBackgroundColorForSystemScheme(
          system_config, kMuonSystemColorSchemeUnknown);

  return ExpectResolvedBrowserBackgroundColor(
             rgb_resolved, 0x12, 0x34, 0x56,
             "explicit RGB background color") &&
         ExpectResolvedBrowserBackgroundColor(system_dark_resolved, 0, 0, 0,
                                             "system dark background color") &&
         ExpectResolvedBrowserBackgroundColor(
             system_light_resolved, 0xff, 0xff, 0xff,
             "system light background color") &&
         Expect(!system_unknown_resolved.has_color,
                "unknown system background color was resolved");
}

static bool RunNetworkPolicyTest() {
  std::shared_ptr<MuonNetworkPolicy> policy;
  std::string error_message;
  if (!Expect(CreateMuonNetworkPolicy(
                  {"asset://main/**", "data:**", "https://example.com/*",
                   "https://deep.example.com/**",
                   "https://*.wild.example.net/**",
                   "https://query.example.com/search?*", "literal:\\*"},
                  &policy, &error_message),
              error_message)) {
    return false;
  }

  return Expect(policy->IsAllowedUrl("asset://main/index.html"),
                "configured asset URL was not allowed") &&
         Expect(policy->IsAllowedUrl("asset://main/nested/index.html"),
                "configured nested asset URL was not allowed") &&
         Expect(!policy->IsAllowedUrl("asset://other/index.html"),
                "unconfigured asset origin was allowed") &&
         Expect(!policy->IsAllowedUrl("asset://main-other/index.html"),
                "unconfigured asset prefix was allowed") &&
         Expect(!policy->IsAllowedUrl("asset://main"),
                "asset URL outside the configured glob was allowed") &&
         Expect(policy->IsAllowedUrl("data:text/html,<title>ok</title>"),
                "matching data URL was not allowed") &&
         Expect(policy->IsAllowedUrl("https://example.com/allowed"),
                "matching HTTPS URL was not allowed") &&
         Expect(policy->IsAllowedUrl("https://deep.example.com/allowed/path"),
                "deep HTTPS URL was not allowed") &&
         Expect(policy->IsAllowedUrl(
                    "https://one.two.wild.example.net/allowed/path"),
                "wildcard host URL was not allowed") &&
         Expect(policy->IsAllowedUrl(
                    "https://query.example.com/search?term=ok"),
                "matching query URL was not allowed") &&
         Expect(policy->IsAllowedUrl("literal:*"),
                "escaped network wildcard was not treated as a literal") &&
         Expect(!policy->IsAllowedUrl("prefix-data:text/html"),
                "partial glob match was allowed") &&
         Expect(!policy->IsAllowedUrl("DATA:text/html,<title>ok</title>"),
                "case-insensitive network glob match was allowed") &&
         Expect(!policy->IsAllowedUrl("https://example.com/allowed/path"),
                "single network wildcard crossed a path separator") &&
         Expect(!policy->IsAllowedUrl(
                    "https://query.example.com/search?term=ok#fragment"),
                "single network wildcard crossed a hash separator") &&
         Expect(!policy->IsAllowedUrl("literal:x"),
                "escaped network wildcard matched other characters") &&
         Expect(!policy->IsAllowedUrl("https://blocked.example/"),
                "blocked URL was allowed");
}

static bool RunDefaultNetworkPolicyPatternTest() {
  std::shared_ptr<MuonNetworkPolicy> policy;
  std::string error_message;
  if (!Expect(CreateMuonNetworkPolicy({"asset://**"}, &policy, &error_message),
              error_message)) {
    return false;
  }

  return Expect(policy->IsAllowedUrl("asset://main/index.html"),
                "default network allowlist rejected the main asset namespace") &&
         Expect(policy->IsAllowedUrl("asset://other/index.html"),
                "default network allowlist rejected another asset namespace") &&
         Expect(!policy->IsAllowedUrl("data:text/html,<title>ok</title>"),
                "default network allowlist allowed a non-asset URL");
}

static bool RunNetworkPolicyEmptyTest() {
  std::shared_ptr<MuonNetworkPolicy> policy;
  std::string error_message;
  return Expect(CreateMuonNetworkPolicy({}, &policy, &error_message),
                error_message) &&
         Expect(!policy->IsAllowedUrl("asset://main/index.html"),
                "empty network allowlist allowed an asset URL") &&
         Expect(!policy->IsAllowedUrl("data:text/html,<title>ok</title>"),
                "empty network allowlist allowed a data URL");
}

static bool RunNetworkAuthorizedOriginPolicyTest() {
  std::shared_ptr<MuonNetworkPolicy> policy;
  std::string error_message;
  if (!Expect(CreateMuonNetworkPolicy(
                  {"asset://main/**"},
                  {{"https", "login.live.com", 0},
                   {"http", "localhost", 8080}},
                  &policy, &error_message),
              error_message)) {
    return false;
  }

  return Expect(policy->IsAllowedRequest("asset://main/index.html", true, ""),
                "glob-allowed URL was not allowed") &&
         Expect(policy->IsAllowedRequest("https://login.live.com/oauth", true,
                                         ""),
                "authorized origin target navigation was not allowed") &&
         Expect(policy->IsAllowedRequest("https://login.live.com:443/oauth",
                                         true, ""),
                "authorized origin target standard port was not allowed") &&
         Expect(!policy->IsAllowedRequest("https://blocked.example/", true,
                                          "https://login.live.com"),
                "top-level redirect to an unauthorized origin was allowed") &&
         Expect(policy->IsAllowedRequest("https://blocked.example/script.js",
                                         false,
                                         "https://login.live.com"),
                "authorized initiator request was not allowed") &&
         Expect(policy->IsAllowedRequest("https://blocked.example/script.js",
                                         false,
                                         "https://login.live.com:443"),
                "authorized initiator standard port was not allowed") &&
         Expect(policy->IsAllowedRequest("https://blocked.example/script.js",
                                         false,
                                         "http://localhost:8080"),
                "authorized non-standard port initiator was not allowed") &&
         Expect(!policy->IsAllowedRequest("https://blocked.example/script.js",
                                          false, "http://localhost"),
                "initiator without the configured non-standard port was "
                "allowed") &&
         Expect(!policy->IsAllowedRequest("https://blocked.example/script.js",
                                          false, "https://evil.example"),
                "unauthorized initiator request was allowed") &&
         Expect(!policy->IsAllowedRequest("https://blocked.example/script.js",
                                          false, ""),
                "empty initiator request was allowed") &&
         Expect(!policy->IsAllowedRequest("https://blocked.example/script.js",
                                          false, "not an origin"),
                "invalid initiator request was allowed") &&
         Expect(!policy->IsAllowedRequest("http://login.live.com/oauth", true,
                                          ""),
                "same domain with a different scheme was allowed") &&
         Expect(!policy->IsAllowedRequest(
                    "https://sub.login.live.com/oauth", true, ""),
                "subdomain of authorized origin was allowed");
}

static bool ExpectInvalidNetworkGlob(
    const std::vector<std::string>& allow_patterns) {
  std::shared_ptr<MuonNetworkPolicy> policy;
  std::string error_message;
  return Expect(!CreateMuonNetworkPolicy(allow_patterns, &policy,
                                         &error_message),
                "invalid network glob was accepted") &&
         Expect(error_message.find("Invalid network.allow glob") !=
                    std::string::npos,
                "invalid network glob error message is missing context");
}

static bool RunInvalidNetworkGlobTest() {
  return ExpectInvalidNetworkGlob({""}) &&
         ExpectInvalidNetworkGlob({"data:\\"}) &&
         ExpectInvalidNetworkGlob({"https://example.com/***"}) &&
         ExpectInvalidNetworkGlob({"https://example.com**"});
}

static bool ExpectInvalidBrowserPluginAllowGlob(
    const std::vector<std::string>& allow_patterns) {
  std::shared_ptr<MuonNetworkPolicy> policy;
  std::string error_message;
  return Expect(!CreateMuonUrlPolicy(allow_patterns, "browser.plugin.allow",
                                     &policy, &error_message),
                "invalid browser.plugin.allow glob was accepted") &&
         Expect(error_message.find("Invalid browser.plugin.allow glob") !=
                    std::string::npos,
                "invalid browser.plugin.allow glob error message is missing "
                "context");
}

static bool RunInvalidBrowserPluginAllowGlobTest() {
  return ExpectInvalidBrowserPluginAllowGlob({""}) &&
         ExpectInvalidBrowserPluginAllowGlob({"data:\\"}) &&
         ExpectInvalidBrowserPluginAllowGlob({"https://example.com/***"}) &&
         ExpectInvalidBrowserPluginAllowGlob({"https://example.com**"});
}

static bool ExpectInvalidUnsafeParentAccessGlob(
    const std::vector<std::string>& allow_patterns) {
  std::shared_ptr<MuonNetworkPolicy> policy;
  std::string error_message;
  return Expect(
             !CreateMuonUrlPolicy(
                 allow_patterns, "browser.allowUnsafeJavaScriptParentAccess",
                 &policy, &error_message),
             "invalid browser.allowUnsafeJavaScriptParentAccess glob was "
             "accepted") &&
         Expect(error_message.find(
                    "Invalid browser.allowUnsafeJavaScriptParentAccess glob") !=
                    std::string::npos,
                "invalid browser.allowUnsafeJavaScriptParentAccess glob error "
                "message is missing context");
}

static bool RunInvalidUnsafeParentAccessGlobTest() {
  return ExpectInvalidUnsafeParentAccessGlob({""}) &&
         ExpectInvalidUnsafeParentAccessGlob({"data:\\"}) &&
         ExpectInvalidUnsafeParentAccessGlob({"https://example.com/***"}) &&
         ExpectInvalidUnsafeParentAccessGlob({"https://example.com**"});
}

static bool RunPluginPolicyTest() {
  std::shared_ptr<MuonPluginPolicy> policy;
  std::string error_message;
  if (!Expect(CreateMuonPluginPolicy(
                  {"muon.fs.readFile", "muon.browser.*",
                   "muon.**.readFile", "literal.\\*", "case.Plugin"},
                  &policy, &error_message),
              error_message)) {
    return false;
  }
	  return Expect(policy->IsAllowedFunctionPath("muon.fs.readFile"),
	                "matching plugin function was not allowed") &&
	         Expect(policy->HasAllowPatterns(),
	                "non-empty plugin policy reported no allow patterns") &&
         Expect(policy->IsAllowedFunctionPath("muon.browser.reload"),
                "matching plugin wildcard was not allowed") &&
         Expect(policy->IsAllowedFunctionPath("muon.deep.nested.readFile"),
                "matching plugin deep wildcard was not allowed") &&
         Expect(policy->IsAllowedFunctionPath("literal.*"),
                "escaped plugin wildcard was not treated as a literal") &&
         Expect(policy->IsAllowedFunctionPath("case.Plugin"),
                "matching case-sensitive plugin function was not allowed") &&
         Expect(!policy->IsAllowedFunctionPath("prefix-muon.fs.readFile"),
                "partial plugin glob match was allowed") &&
         Expect(!policy->IsAllowedFunctionPath(
                    "muon.browser.navigation.reload"),
                "single plugin wildcard crossed a namespace separator") &&
         Expect(!policy->IsAllowedFunctionPath("muon.fs.writeFile"),
                "blocked plugin function was allowed") &&
         Expect(!policy->IsAllowedFunctionPath("literal.x"),
                "escaped plugin wildcard matched other characters") &&
         Expect(!policy->IsAllowedFunctionPath("case.plugin"),
                "case-insensitive plugin glob match was allowed");
}

static bool RunPluginPolicyDeepWildcardTest() {
  std::shared_ptr<MuonPluginPolicy> policy;
  std::string error_message;
  if (!Expect(CreateMuonPluginPolicy({"muon.**"}, &policy, &error_message),
              error_message)) {
    return false;
  }
  return Expect(policy->IsAllowedFunctionPath("muon.fs.writeFile"),
                "plugin deep wildcard did not match a nested function") &&
         Expect(policy->IsAllowedFunctionPath(
                    "muon.browser.navigation.reload"),
                "plugin deep wildcard did not cross namespace separators") &&
         Expect(!policy->IsAllowedFunctionPath("prefix-muon.fs.writeFile"),
                "plugin deep wildcard allowed a partial match") &&
         Expect(!policy->IsAllowedFunctionPath("Muon.fs.writeFile"),
                "plugin deep wildcard matched a different case");
}

static bool RunPluginPolicyEmptyTest() {
  std::shared_ptr<MuonPluginPolicy> policy;
  std::string error_message;
	  return Expect(CreateMuonPluginPolicy({}, &policy, &error_message),
	                error_message) &&
	         Expect(!policy->HasAllowPatterns(),
	                "empty plugin policy reported allow patterns") &&
	         Expect(!policy->IsAllowedFunctionPath("muon.browser.reload"),
	                "empty plugin allowlist allowed a function");
}

static bool ExpectInvalidPluginGlob(
    const std::vector<std::string>& allow_patterns) {
  std::shared_ptr<MuonPluginPolicy> policy;
  std::string error_message;
  return Expect(!CreateMuonPluginPolicy(allow_patterns, &policy,
                                        &error_message),
                "invalid plugin glob was accepted") &&
         Expect(error_message.find("Invalid plugin allow glob") !=
                    std::string::npos,
                "invalid plugin glob error message is missing context");
}

static bool RunPluginPolicyInvalidGlobTest() {
  return ExpectInvalidPluginGlob({""}) &&
         ExpectInvalidPluginGlob({"muon\\"}) &&
         ExpectInvalidPluginGlob({"muon.***"}) &&
         ExpectInvalidPluginGlob({"muon**readFile"});
}

int main() {
  const auto test_directory = CreateTestDirectory();
  if (!Expect(!test_directory.empty(), "failed to create test directory")) {
    return 1;
  }

  const auto passed = RunConfigLoadingTest(test_directory) &&
                      RunConfigJson5LoadingTest(test_directory) &&
                      RunLaunchSourceProfilePathTest(test_directory) &&
                      RunDefaultConfigSearchOrderTest(test_directory) &&
                      RunCommandLineConfigPathTest(test_directory) &&
                      RunEmbeddedConfigLoadingTest(test_directory) &&
                      RunEmbeddedConfigEmptySlotTest(test_directory) &&
                      RunConfigOverrideLoadingTest(test_directory) &&
                      RunBrowserConfigLoadingTest(test_directory) &&
                      RunLogConfigLoadingTest(test_directory) &&
                      RunConfigValidationTest(test_directory) &&
                      RunLogConfigValidationTest(test_directory) &&
                      RunDebuggerConfigValidationTest(test_directory) &&
                      RunBrowserConfigValidationTest(test_directory) &&
                      RunBrowserBackgroundColorResolutionTest() &&
                      RunNetworkPolicyTest() &&
                      RunDefaultNetworkPolicyPatternTest() &&
                      RunNetworkPolicyEmptyTest() &&
                      RunNetworkAuthorizedOriginPolicyTest() &&
                      RunInvalidNetworkGlobTest() &&
                      RunInvalidBrowserPluginAllowGlobTest() &&
                      RunInvalidUnsafeParentAccessGlobTest() &&
                      RunPluginPolicyTest() &&
                      RunPluginPolicyDeepWildcardTest() &&
                      RunPluginPolicyEmptyTest() &&
                      RunPluginPolicyInvalidGlobTest();

  std::error_code error;
  std::filesystem::remove_all(test_directory, error);
  return passed ? 0 : 1;
}
