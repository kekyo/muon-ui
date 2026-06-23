/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "browser/muon_builtin_browser.h"
#include "browser/muon_native_wheel_forwarder.h"
#include "browser/muon_title_bar.h"
#include "browser/muon_window_delegate.h"
#include "browser/muon_window_title.h"

#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

static bool Expect(bool condition, const char* message) {
  if (condition) {
    return true;
  }
  std::fprintf(stderr, "%s\n", message);
  return false;
}

static bool TestBrowserFunctionDefinitions() {
  const auto definitions = GetMuonBuiltinBrowserFunctionDefinitions();
  const auto expected_names = std::vector<std::string>{
      "reload",          "hardReload",     "toggleFullscreen",
      "enterFullscreen", "exitFullscreen", "zoomIn",
      "zoomOut",         "resetZoom",      "show",
      "hide",            "focus",          "blur",
      "minimize",        "maximize",       "restore",
      "setTitleBarVisibility",
      "setTitleBarIcon",
      "__close",         "__shutdown",     "__recycle",
  };
  const auto expected_kinds = std::vector<MuonBuiltinBrowserFunctionKind>{
      MuonBuiltinBrowserFunctionKind::Reload,
      MuonBuiltinBrowserFunctionKind::HardReload,
      MuonBuiltinBrowserFunctionKind::ToggleFullscreen,
      MuonBuiltinBrowserFunctionKind::EnterFullscreen,
      MuonBuiltinBrowserFunctionKind::ExitFullscreen,
      MuonBuiltinBrowserFunctionKind::ZoomIn,
      MuonBuiltinBrowserFunctionKind::ZoomOut,
      MuonBuiltinBrowserFunctionKind::ResetZoom,
      MuonBuiltinBrowserFunctionKind::Show,
      MuonBuiltinBrowserFunctionKind::Hide,
      MuonBuiltinBrowserFunctionKind::Focus,
      MuonBuiltinBrowserFunctionKind::Blur,
      MuonBuiltinBrowserFunctionKind::Minimize,
      MuonBuiltinBrowserFunctionKind::Maximize,
      MuonBuiltinBrowserFunctionKind::Restore,
      MuonBuiltinBrowserFunctionKind::SetTitleBarVisibility,
      MuonBuiltinBrowserFunctionKind::SetTitleBarIcon,
      MuonBuiltinBrowserFunctionKind::Close,
      MuonBuiltinBrowserFunctionKind::Shutdown,
      MuonBuiltinBrowserFunctionKind::Recycle,
  };

  if (!Expect(GetMuonBuiltinBrowserPluginNamespace() ==
                  std::string("muon.browser"),
              "unexpected browser namespace") ||
      !Expect(definitions.size() == expected_names.size(),
              "unexpected browser definition count") ||
      !Expect(expected_kinds.size() == expected_names.size(),
              "browser test expectation count mismatch")) {
    return false;
  }

  for (auto index = size_t{0}; index < expected_names.size(); ++index) {
    if (!Expect(definitions[index].js_name == expected_names[index],
                "unexpected browser function name") ||
        !Expect(definitions[index].kind == expected_kinds[index],
                "unexpected browser function kind")) {
      return false;
    }
  }
  const auto set_title_bar_visibility = definitions[expected_names.size() - 5];
  const auto set_title_bar_icon = definitions[expected_names.size() - 4];
  const auto shutdown = definitions[expected_names.size() - 2];
  const auto recycle = definitions.back();
  if (!Expect(set_title_bar_visibility.arg_count == 1,
              "unexpected title bar visibility argument count") ||
      !Expect(set_title_bar_visibility.arg_types != nullptr,
              "missing title bar visibility argument metadata") ||
      !Expect(set_title_bar_visibility.arg_types[0].type == MUON_TYPE_BOOL,
              "unexpected title bar visibility argument type") ||
      !Expect(set_title_bar_visibility.return_type.type == MUON_TYPE_VOID,
              "unexpected title bar visibility return type") ||
      !Expect(set_title_bar_icon.arg_count == 1,
              "unexpected title bar icon argument count") ||
      !Expect(set_title_bar_icon.arg_types != nullptr,
              "missing title bar icon argument metadata") ||
      !Expect(set_title_bar_icon.arg_types[0].type == MUON_TYPE_STRING,
              "unexpected title bar icon argument type") ||
      !Expect(set_title_bar_icon.return_type.type == MUON_TYPE_VOID,
              "unexpected title bar icon return type") ||
      !Expect(shutdown.filter_name != nullptr &&
                  std::string(shutdown.filter_name) == "shutdown",
              "unexpected browser shutdown filter name") ||
      !Expect(shutdown.arg_count == 1,
              "unexpected browser shutdown argument count") ||
      !Expect(shutdown.arg_types != nullptr,
              "missing browser shutdown argument metadata") ||
      !Expect(shutdown.arg_types[0].type == MUON_TYPE_I32,
              "unexpected browser shutdown argument type") ||
      !Expect(shutdown.return_type.type == MUON_TYPE_VOID,
              "unexpected browser shutdown return type") ||
      !Expect(recycle.filter_name != nullptr &&
                  std::string(recycle.filter_name) == "recycle",
              "unexpected browser recycle filter name") ||
      !Expect(recycle.arg_count == 0,
              "unexpected browser recycle argument count") ||
      !Expect(recycle.return_type.type == MUON_TYPE_VOID,
              "unexpected browser recycle return type") ||
      !Expect(GetMuonBuiltinBrowserSetupScript() != nullptr,
              "missing browser setup script")) {
    return false;
  }
  return true;
}

static bool TestWindowTitleFallback() {
  return Expect(GetMuonDefaultWindowTitle() == std::string("Muon"),
                "unexpected default window title") &&
         Expect(GetMuonDevToolsWindowTitle() == std::string("Muon DevTools"),
                "unexpected DevTools window title") &&
         Expect(GetMuonWindowTitleOrDefault("") == "Muon",
                "empty page title did not use default") &&
         Expect(GetMuonWindowTitleOrDefault("Page") == "Page",
                "non-empty page title was not preserved");
}

static bool TestInitialWindowShowState() {
  auto normal =
      MuonWindowDelegate(nullptr, false, kMuonBrowserInitialWindowStateNormal);
  auto hidden =
      MuonWindowDelegate(nullptr, false, kMuonBrowserInitialWindowStateHidden);
  auto minimized =
      MuonWindowDelegate(nullptr, false, kMuonBrowserInitialWindowStateMinimized);
  auto maximized =
      MuonWindowDelegate(nullptr, false, kMuonBrowserInitialWindowStateMaximized);
  auto fullscreen =
      MuonWindowDelegate(nullptr, false, kMuonBrowserInitialWindowStateFullscreen);
  return Expect(normal.GetInitialShowState(nullptr) == CEF_SHOW_STATE_NORMAL,
                "normal initial window state did not use normal show state") &&
         Expect(hidden.GetInitialShowState(nullptr) == CEF_SHOW_STATE_NORMAL,
                "hidden initial window state should use normal show state") &&
         Expect(minimized.GetInitialShowState(nullptr) ==
                    CEF_SHOW_STATE_MINIMIZED,
                "minimized initial window state did not use minimized show "
                "state") &&
         Expect(maximized.GetInitialShowState(nullptr) ==
                    CEF_SHOW_STATE_MAXIMIZED,
                "maximized initial window state did not use maximized show "
                "state") &&
         Expect(fullscreen.GetInitialShowState(nullptr) ==
                    CEF_SHOW_STATE_FULLSCREEN,
                "fullscreen initial window state did not use fullscreen show "
                "state");
}

static MuonTitleBarManifest CreateTestCustomTitleBarManifest() {
  MuonTitleBarManifest manifest;
  manifest.mode = MuonTitleBarMode::Custom;
  manifest.height = 36;
  manifest.controls_width = 138;
  manifest.html = "<div>title</div>";
  manifest.css = "body { margin: 0; }";
  manifest.js = "globalThis.__muonTitleBar = {};";
  return manifest;
}

static bool TestTitleBarManifestParsing() {
  const auto native = ParseMuonTitleBarManifest(R"({"mode":"native"})");
  const auto custom = ParseMuonTitleBarManifest(
      R"({"mode":"custom","height":36,"controlsWidth":138,"html":"<div></div>","css":"body{}","js":"void 0;"})");
  const auto invalid_json = ParseMuonTitleBarManifest("{");
  const auto unknown_mode = ParseMuonTitleBarManifest(R"({"mode":"other"})");
  const auto missing_fields =
      ParseMuonTitleBarManifest(R"({"mode":"custom","height":36})");
  const auto invalid_height = ParseMuonTitleBarManifest(
      R"({"mode":"custom","height":0,"controlsWidth":138,"html":"x","css":"x","js":"x"})");

  return Expect(!IsCustomMuonTitleBar(native),
                "native title bar manifest was treated as custom") &&
         Expect(IsCustomMuonTitleBar(custom),
                "valid custom title bar manifest was not accepted") &&
         Expect(custom.height == 36, "unexpected custom title bar height") &&
         Expect(custom.controls_width == 138,
                "unexpected custom title bar controls width") &&
         Expect(!IsCustomMuonTitleBar(invalid_json),
                "invalid JSON title bar manifest did not fall back") &&
         Expect(!IsCustomMuonTitleBar(unknown_mode),
                "unknown title bar mode did not fall back") &&
         Expect(!IsCustomMuonTitleBar(missing_fields),
                "incomplete custom title bar manifest did not fall back") &&
         Expect(!IsCustomMuonTitleBar(invalid_height),
                "invalid custom title bar height did not fall back");
}

static bool TestNativeTitleBarSupportDetection() {
#if defined(OS_LINUX)
  return Expect(IsMuonNativeTitleBarSupported({"muon", "--ozone-platform=x11"},
                                             "wayland", "wayland-0", ":0"),
                "explicit X11 ozone platform should allow native title bar") &&
         Expect(!IsMuonNativeTitleBarSupported(
                    {"muon", "--ozone-platform=wayland"}, "x11", nullptr,
                    ":0"),
                "explicit Wayland ozone platform should reject native title "
                "bar") &&
         Expect(!IsMuonNativeTitleBarSupported({"muon"}, "wayland",
                                               "wayland-0", ":0"),
                "Wayland session should reject native title bar") &&
         Expect(IsMuonNativeTitleBarSupported({"muon"}, "x11", nullptr, ":0"),
                "X11 session should allow native title bar") &&
         Expect(IsMuonNativeTitleBarSupported({"muon"}, nullptr, nullptr, ":0"),
                "DISPLAY-only environment should allow native title bar") &&
         Expect(!IsMuonNativeTitleBarSupported({"muon"}, nullptr, nullptr,
                                               nullptr),
                "unknown Linux display backend should reject native title bar");
#else
  return Expect(IsMuonNativeTitleBarSupported({"muon"}, nullptr, nullptr,
                                             nullptr),
                "non-Linux native title bar should remain supported");
#endif
}

static bool TestPageDraggableRegionHitTesting() {
  const auto regions = std::vector<CefDraggableRegion>{
      CefDraggableRegion(CefRect(10, 20, 200, 120), true),
      CefDraggableRegion(CefRect(70, 60, 48, 32), false),
  };
  return Expect(IsMuonPageDraggableRegionPoint(regions, CefPoint(20, 30)),
                "drag region point was not accepted") &&
         Expect(!IsMuonPageDraggableRegionPoint(regions, CefPoint(80, 70)),
                "no-drag region point was accepted") &&
         Expect(!IsMuonPageDraggableRegionPoint(regions, CefPoint(220, 40)),
                "outside point was accepted") &&
         Expect(!IsMuonPageDraggableRegionPoint({}, CefPoint(20, 30)),
                "empty regions accepted a point");
}

static bool TestNativeForwarderRegistersChildWindows() {
  const auto handles = GetMuonNativeForwarderWindowHandlesForRegistration(
      10, std::vector<CefWindowHandle>{20, 30});
  return Expect(handles.size() == 3,
                "native input forwarder did not include child windows") &&
         Expect(handles[0] == 10, "native input forwarder reordered root") &&
         Expect(handles[1] == 20,
                "native input forwarder omitted first child") &&
         Expect(handles[2] == 30,
                "native input forwarder omitted second child");
}

static bool TestWindowIconUpdateBehavior() {
  const auto native_icon =
      GetMuonWindowIconUpdateBehavior(true, "data:image/png;base64,icon");
  const auto data_url_only =
      GetMuonWindowIconUpdateBehavior(false, "data:image/svg+xml;base64,icon");
  const auto clear_icon = GetMuonWindowIconUpdateBehavior(false, "");

  return Expect(native_icon.window_icon_action == MuonWindowIconAction::Set,
                "native title bar icon image should update the window icon") &&
         Expect(native_icon.app_icon_action == MuonWindowIconAction::Set,
                "native title bar icon image should update the app icon") &&
         Expect(data_url_only.window_icon_action == MuonWindowIconAction::Keep,
                "data-url-only title bar icon should keep the window icon") &&
         Expect(data_url_only.app_icon_action == MuonWindowIconAction::Keep,
                "data-url-only title bar icon should keep the app icon") &&
         Expect(clear_icon.window_icon_action == MuonWindowIconAction::Clear,
                "clearing the title bar icon should clear the window icon") &&
         Expect(clear_icon.app_icon_action == MuonWindowIconAction::Clear,
                "clearing the title bar icon should clear the app icon");
}

static bool TestCustomTitleBarWindowDelegate() {
  const auto manifest = CreateTestCustomTitleBarManifest();
  auto browser =
      MuonWindowDelegate(nullptr, false, kMuonBrowserInitialWindowStateNormal,
                         true, manifest);
  auto hidden_title_bar =
      MuonWindowDelegate(nullptr, false, kMuonBrowserInitialWindowStateNormal,
                         false, manifest);
  auto devtools =
      MuonWindowDelegate(nullptr, true, kMuonBrowserInitialWindowStateNormal,
                         true, manifest);
  auto native =
      MuonWindowDelegate(nullptr, false, kMuonBrowserInitialWindowStateNormal,
                         true,
                         CreateNativeMuonTitleBarManifest());
  const auto browser_size = browser.GetPreferredSize(nullptr);
  const auto hidden_title_bar_size =
      hidden_title_bar.GetPreferredSize(nullptr);
  const auto devtools_size = devtools.GetPreferredSize(nullptr);
  const auto native_size = native.GetPreferredSize(nullptr);
  return Expect(browser.IsFrameless(nullptr),
                "custom browser window did not become frameless") &&
         Expect(browser_size.width == 1024 && browser_size.height == 804,
                "custom browser window did not include title bar height") &&
         Expect(hidden_title_bar_size.width == 1024 &&
                    hidden_title_bar_size.height == 768,
                "hidden custom title bar preferred size should exclude title "
                "bar height") &&
         Expect(!native.IsFrameless(nullptr),
                "native title bar window should not become frameless") &&
         Expect(native_size.width == 1024 && native_size.height == 768,
                "native title bar preferred size should remain unchanged") &&
         Expect(!devtools.IsFrameless(nullptr),
                "DevTools window should not use custom title bar") &&
         Expect(devtools_size.width == 1024 && devtools_size.height == 768,
                "DevTools preferred size should remain unchanged");
}

int main() {
  return TestBrowserFunctionDefinitions() && TestWindowTitleFallback() &&
                 TestInitialWindowShowState() && TestTitleBarManifestParsing() &&
                 TestNativeTitleBarSupportDetection() &&
                 TestPageDraggableRegionHitTesting() &&
                 TestNativeForwarderRegistersChildWindows() &&
                 TestWindowIconUpdateBehavior() &&
                 TestCustomTitleBarWindowDelegate()
             ? 0
             : 1;
}
