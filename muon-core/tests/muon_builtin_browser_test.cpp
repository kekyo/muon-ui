/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "browser/muon_builtin_browser.h"
#include "browser/muon_native_wheel_forwarder.h"
#include "browser/muon_title_bar.h"
#include "browser/muon_window_delegate.h"
#include "browser/muon_window_state.h"
#include "browser/muon_window_title.h"
#include "config/muon_linux_display_backend.h"

#include <cstddef>
#include <cstdint>
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
      "__getWindowBounds",
      "__setWindowBounds",
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
      MuonBuiltinBrowserFunctionKind::GetWindowBounds,
      MuonBuiltinBrowserFunctionKind::SetWindowBounds,
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
  const auto set_title_bar_visibility = definitions[15];
  const auto set_title_bar_icon = definitions[16];
  const auto get_window_bounds = definitions[17];
  const auto set_window_bounds = definitions[18];
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
      !Expect(get_window_bounds.filter_name != nullptr &&
                  std::string(get_window_bounds.filter_name) ==
                      "getWindowBounds",
              "unexpected get window bounds filter name") ||
      !Expect(get_window_bounds.arg_count == 0,
              "unexpected get window bounds argument count") ||
      !Expect(get_window_bounds.return_type.type == MUON_TYPE_STRING,
              "unexpected get window bounds return type") ||
      !Expect(set_window_bounds.filter_name != nullptr &&
                  std::string(set_window_bounds.filter_name) ==
                      "setWindowBounds",
              "unexpected set window bounds filter name") ||
      !Expect(set_window_bounds.arg_count == 4,
              "unexpected set window bounds argument count") ||
      !Expect(set_window_bounds.arg_types != nullptr,
              "missing set window bounds argument metadata") ||
      !Expect(set_window_bounds.arg_types[0].type == MUON_TYPE_I32,
              "unexpected set window bounds x argument type") ||
      !Expect(set_window_bounds.arg_types[1].type == MUON_TYPE_I32,
              "unexpected set window bounds y argument type") ||
      !Expect(set_window_bounds.arg_types[2].type == MUON_TYPE_I32,
              "unexpected set window bounds width argument type") ||
      !Expect(set_window_bounds.arg_types[3].type == MUON_TYPE_I32,
              "unexpected set window bounds height argument type") ||
      !Expect(set_window_bounds.return_type.type == MUON_TYPE_VOID,
              "unexpected set window bounds return type") ||
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

#if defined(OS_LINUX)
static std::string ReadCefStructString(cef_string_t* value) {
  return CefString(value).ToString();
}

static void ClearLinuxWindowProperties(CefLinuxWindowProperties* properties) {
  cef_string_clear(&properties->wayland_app_id);
  cef_string_clear(&properties->wm_class_class);
  cef_string_clear(&properties->wm_class_name);
  cef_string_clear(&properties->wm_role_name);
}

static bool TestLinuxWindowPropertiesUseDesktopId() {
  auto browser =
      MuonWindowDelegate(nullptr, false, kMuonBrowserInitialWindowStateNormal,
                         true, CreateNativeMuonTitleBarManifest(), {}, nullptr,
                         nullptr, "com.example.App");
  CefLinuxWindowProperties properties = {};
  const auto populated = browser.GetLinuxWindowProperties(nullptr, properties);
  const auto wayland_app_id = ReadCefStructString(&properties.wayland_app_id);
  const auto wm_class_class = ReadCefStructString(&properties.wm_class_class);
  const auto wm_class_name = ReadCefStructString(&properties.wm_class_name);
  const auto wm_role_name = ReadCefStructString(&properties.wm_role_name);
  ClearLinuxWindowProperties(&properties);
  return Expect(populated, "Linux window properties were not populated") &&
         Expect(wayland_app_id == "com.example.App",
                "Wayland app ID did not use desktopId") &&
         Expect(wm_class_class == "com.example.App",
                "WM_CLASS class did not use desktopId") &&
         Expect(wm_class_name == "com.example.App",
                "WM_CLASS name did not use desktopId") &&
         Expect(wm_role_name == "browser",
                "browser window role name changed");
}
#endif

static bool BoundsAreInsideWorkArea(const CefRect& bounds,
                                    const CefRect& work_area) {
  return bounds.x >= work_area.x && bounds.y >= work_area.y &&
         bounds.x + bounds.width <= work_area.x + work_area.width &&
         bounds.y + bounds.height <= work_area.y + work_area.height;
}

static bool TestInitialWindowWorkAreaBounds() {
  const auto offset_work_area = CefRect(67, 34, 1000, 700);
  const auto centered =
      GetMuonCenteredWindowBounds(offset_work_area, CefSize(400, 300));
  const auto clamped =
      GetMuonCenteredWindowBounds(offset_work_area, CefSize(1200, 900));
  return Expect(BoundsAreInsideWorkArea(centered, offset_work_area),
                "centered initial window bounds escaped the work area") &&
         Expect(centered.x == 367 && centered.y == 234 &&
                    centered.width == 400 && centered.height == 300,
                "initial window bounds were not centered in offset work "
                "area") &&
         Expect(BoundsAreInsideWorkArea(clamped, offset_work_area),
                "clamped initial window bounds escaped the work area") &&
         Expect(clamped.x == 67 && clamped.y == 34 &&
                    clamped.width == 1000 && clamped.height == 700,
                "oversized initial window bounds were not clamped to work "
                "area");
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
  const auto native_controls = ParseMuonTitleBarManifest(
      R"({"mode":"custom","height":36,"controlsWidth":138,"nativeWindowControls":true,"html":"<div></div>","css":"body{}","js":"void 0;"})");
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
         Expect(!custom.native_window_controls,
                "custom title bar unexpectedly enabled native controls") &&
         Expect(native_controls.native_window_controls,
                "built-in title bar native controls flag was not accepted") &&
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

static bool TestLinuxDisplayBackendDetection() {
#if defined(OS_LINUX)
  return Expect(ResolveMuonLinuxDisplayBackend(
                    {"muon", "--ozone-platform=x11"}, "wayland",
                    "wayland-0", ":0") == kMuonLinuxDisplayBackendX11,
                "explicit X11 ozone platform should select X11") &&
         Expect(ResolveMuonLinuxDisplayBackend(
                    {"muon", "--ozone-platform=wayland"}, "x11", nullptr,
                    ":0") == kMuonLinuxDisplayBackendWayland,
                "explicit Wayland ozone platform should select Wayland") &&
         Expect(ResolveMuonLinuxDisplayBackend(
                    {"muon", "--ozone-platform-hint=x11"}, "wayland",
                    "wayland-0", ":0") == kMuonLinuxDisplayBackendX11,
                "explicit X11 ozone platform hint should select X11") &&
         Expect(ResolveMuonLinuxDisplayBackend({"muon"}, "wayland",
                                               "wayland-0", ":0") ==
                    kMuonLinuxDisplayBackendWayland,
                "Wayland session should select Wayland") &&
         Expect(ResolveMuonLinuxDisplayBackend({"muon"}, "x11", nullptr,
                                               ":0") ==
                    kMuonLinuxDisplayBackendX11,
                "X11 session should select X11") &&
         Expect(ResolveMuonLinuxDisplayBackend({"muon"}, nullptr, nullptr,
                                               nullptr) ==
                    kMuonLinuxDisplayBackendUnknown,
                "missing Linux display signals should remain unknown");
#else
  return true;
#endif
}

static bool TestLinuxWaylandVulkanMitigationDetection() {
#if defined(OS_LINUX)
  return Expect(ShouldDisableMuonCefVulkanForLinuxDisplayBackend(
                    {"muon", "--ozone-platform=wayland"}, "x11", nullptr,
                    ":0"),
                "explicit Wayland ozone platform should disable CEF Vulkan") &&
         Expect(ShouldDisableMuonCefVulkanForLinuxDisplayBackend(
                    {"muon"}, "wayland", "wayland-0", ":0"),
                "Wayland session should disable CEF Vulkan") &&
         Expect(!ShouldDisableMuonCefVulkanForLinuxDisplayBackend(
                    {"muon", "--ozone-platform=x11"}, "wayland",
                    "wayland-0", ":0"),
                "explicit X11 ozone platform should keep CEF Vulkan") &&
         Expect(!ShouldDisableMuonCefVulkanForLinuxDisplayBackend(
                    {"muon"}, "x11", nullptr, ":0"),
                "X11 session should keep CEF Vulkan");
#else
  return true;
#endif
}

static bool TestLinuxWaylandAngleOpenGlMitigationDetection() {
#if defined(OS_LINUX)
  return Expect(ShouldUseMuonCefAngleOpenGlForLinuxDisplayBackend(
                    {"muon", "--ozone-platform=wayland"}, "x11", nullptr,
                    ":0"),
                "explicit Wayland ozone platform should use ANGLE OpenGL") &&
         Expect(ShouldUseMuonCefAngleOpenGlForLinuxDisplayBackend(
                    {"muon"}, "wayland", "wayland-0", ":0"),
                "Wayland session should use ANGLE OpenGL") &&
         Expect(!ShouldUseMuonCefAngleOpenGlForLinuxDisplayBackend(
                    {"muon", "--use-gl=desktop"}, "wayland", "wayland-0",
                    ":0"),
                "explicit use-gl switch should keep CEF GL selection") &&
         Expect(!ShouldUseMuonCefAngleOpenGlForLinuxDisplayBackend(
                    {"muon", "--use-angle=vulkan"}, "wayland", "wayland-0",
                    ":0"),
                "explicit use-angle switch should keep CEF ANGLE selection") &&
         Expect(!ShouldUseMuonCefAngleOpenGlForLinuxDisplayBackend(
                    {"muon", "--ozone-platform=x11"}, "wayland",
                    "wayland-0", ":0"),
                "explicit X11 ozone platform should keep CEF GL selection");
#else
  return true;
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

static bool TestPageDraggableRegionSearchKeys() {
  const auto registered_window_keys = std::vector<std::uintptr_t>{10, 20};
  const auto specific =
      GetMuonPageDraggableRegionSearchKeys(10, registered_window_keys);
  const auto foreign =
      GetMuonPageDraggableRegionSearchKeys(30, registered_window_keys);
  const auto global =
      GetMuonPageDraggableRegionSearchKeys(0, registered_window_keys);
  return Expect(specific == std::vector<std::uintptr_t>{10},
                "specific draggable-region search fell back to other windows") &&
         Expect(foreign == std::vector<std::uintptr_t>{30},
                "foreign draggable-region search fell back to registered "
                "windows") &&
         Expect(global == registered_window_keys,
                "global draggable-region search did not use registered "
                "windows");
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

static bool TestNativeWheelForwarderTargetWindowSelection() {
  const auto event_registered =
      GetMuonNativeWheelForwarderTargetWindowHandle(
          10, 20, std::vector<CefWindowHandle>{10});
  const auto child_registered =
      GetMuonNativeWheelForwarderTargetWindowHandle(
          10, 20, std::vector<CefWindowHandle>{20});
  const auto fallback_child =
      GetMuonNativeWheelForwarderTargetWindowHandle(
          10, 20, std::vector<CefWindowHandle>{30});
  const auto fallback_event =
      GetMuonNativeWheelForwarderTargetWindowHandle(
          10, 0, std::vector<CefWindowHandle>{30});
  return Expect(event_registered == 10,
                "native wheel forwarder ignored registered event window") &&
         Expect(child_registered == 20,
                "native wheel forwarder ignored registered child window") &&
         Expect(fallback_child == 20,
                "native wheel forwarder did not fall back to child window") &&
         Expect(fallback_event == 10,
                "native wheel forwarder did not fall back to event window");
}

static bool TestNativeWheelForwarderTopmostRegisteredWindowAtPoint() {
  const auto registered_window_handles = std::vector<CefWindowHandle>{10};
  const auto registered =
      MuonNativeWheelForwarderTopLevelWindow{10, 0, 0, 100, 100, true, false};
  const auto hidden =
      MuonNativeWheelForwarderTopLevelWindow{10, 0, 0, 100, 100, false, false};
  const auto override_redirect =
      MuonNativeWheelForwarderTopLevelWindow{20, 0, 0, 100, 100, true, true};
  const auto foreign =
      MuonNativeWheelForwarderTopLevelWindow{20, 0, 0, 100, 100, true, false};

  const auto direct_hit =
      GetMuonNativeWheelForwarderTopmostRegisteredWindowAtPoint(
          CefPoint(20, 30), registered_window_handles, {registered});
  const auto outside =
      GetMuonNativeWheelForwarderTopmostRegisteredWindowAtPoint(
          CefPoint(120, 30), registered_window_handles, {registered});
  const auto hidden_hit =
      GetMuonNativeWheelForwarderTopmostRegisteredWindowAtPoint(
          CefPoint(20, 30), registered_window_handles, {hidden});
  const auto override_redirect_over_hit =
      GetMuonNativeWheelForwarderTopmostRegisteredWindowAtPoint(
          CefPoint(20, 30), registered_window_handles,
          {registered, override_redirect});
  const auto foreign_over_hit =
      GetMuonNativeWheelForwarderTopmostRegisteredWindowAtPoint(
          CefPoint(20, 30), registered_window_handles, {registered, foreign});

  return Expect(direct_hit == 10,
                "native wheel forwarder did not select registered top-level "
                "window at point") &&
         Expect(outside == 0,
                "native wheel forwarder selected window outside point") &&
         Expect(hidden_hit == 0,
                "native wheel forwarder selected hidden window") &&
         Expect(override_redirect_over_hit == 10,
                "native wheel forwarder did not ignore override-redirect "
                "overlay") &&
         Expect(foreign_over_hit == 0,
                "native wheel forwarder crossed into covered foreign window");
}

static bool TestWindowsNonClientDragHitTesting() {
  constexpr int kWindowsHitTestCaption = 2;
  constexpr int kWindowsHitTestLeft = 10;
  constexpr int kWindowsHitTestRight = 11;
  constexpr int kWindowsHitTestTop = 12;
  constexpr int kWindowsHitTestTopLeft = 13;
  constexpr int kWindowsHitTestTopRight = 14;
  constexpr int kWindowsHitTestBottom = 15;
  constexpr int kWindowsHitTestBottomLeft = 16;
  constexpr int kWindowsHitTestBottomRight = 17;

  return Expect(ShouldHandleMuonWindowsNonClientDragHitTest(
                    kWindowsHitTestCaption),
                "caption hit-test should start custom window drag") &&
         Expect(!ShouldHandleMuonWindowsNonClientDragHitTest(
                    kWindowsHitTestTop),
                "top resize hit-test should not start custom window drag") &&
         Expect(!ShouldHandleMuonWindowsNonClientDragHitTest(
                    kWindowsHitTestLeft),
                "left resize hit-test should not start custom window drag") &&
         Expect(!ShouldHandleMuonWindowsNonClientDragHitTest(
                    kWindowsHitTestRight),
                "right resize hit-test should not start custom window drag") &&
         Expect(!ShouldHandleMuonWindowsNonClientDragHitTest(
                    kWindowsHitTestTopLeft),
                "top-left resize hit-test should not start custom window "
                "drag") &&
         Expect(!ShouldHandleMuonWindowsNonClientDragHitTest(
                    kWindowsHitTestTopRight),
                "top-right resize hit-test should not start custom window "
                "drag") &&
         Expect(!ShouldHandleMuonWindowsNonClientDragHitTest(
                    kWindowsHitTestBottom),
                "bottom resize hit-test should not start custom window drag") &&
         Expect(!ShouldHandleMuonWindowsNonClientDragHitTest(
                    kWindowsHitTestBottomLeft),
                "bottom-left resize hit-test should not start custom window "
                "drag") &&
         Expect(!ShouldHandleMuonWindowsNonClientDragHitTest(
                    kWindowsHitTestBottomRight),
                "bottom-right resize hit-test should not start custom window "
                "drag");
}

static bool TestTitleBarControlHitTesting() {
  const auto window_size = CefSize(300, 200);
  return Expect(GetMuonTitleBarControlActionAtWindowPoint(
                    true, 36, 138, window_size, CefPoint(170, 10)) ==
                    MuonTitleBarControlAction::Minimize,
                "minimize title bar control was not hit") &&
         Expect(GetMuonTitleBarControlActionAtWindowPoint(
                    true, 36, 138, window_size, CefPoint(220, 10)) ==
                    MuonTitleBarControlAction::Maximize,
                "maximize title bar control was not hit") &&
         Expect(GetMuonTitleBarControlActionAtWindowPoint(
                    true, 36, 138, window_size, CefPoint(280, 10)) ==
                    MuonTitleBarControlAction::Close,
                "close title bar control was not hit") &&
         Expect(GetMuonTitleBarControlActionAtWindowPoint(
                    true, 36, 138, window_size, CefPoint(120, 10)) ==
                    MuonTitleBarControlAction::NoControl,
                "left title bar area was treated as a control") &&
         Expect(GetMuonTitleBarControlActionAtWindowPoint(
                    true, 36, 138, window_size, CefPoint(280, 40)) ==
                    MuonTitleBarControlAction::NoControl,
                "point below title bar was treated as a control") &&
         Expect(GetMuonTitleBarControlActionAtWindowPoint(
                    false, 36, 138, window_size, CefPoint(280, 10)) ==
                    MuonTitleBarControlAction::NoControl,
                "custom title bar without native controls was hit");
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

static bool TestTitleBarBrowserIdResolution() {
  return Expect(GetMuonResolvedTitleBarBrowserId(7, 42) == 7,
                "direct title bar browser id was not preserved") &&
         Expect(GetMuonResolvedTitleBarBrowserId(0, 42) == 42,
                "registered title bar browser id was not reused") &&
         Expect(GetMuonResolvedTitleBarBrowserId(0, 0) == 0,
                "missing title bar browser id should remain missing");
}

static bool TestTitleBarBrowserWindowRegistrationResolution() {
  return Expect(ShouldReplaceRegisteredMuonTitleBarWindowForBrowser(false,
                                                                    false),
                "plain title bar window registration should remain last-wins") &&
         Expect(ShouldReplaceRegisteredMuonTitleBarWindowForBrowser(false,
                                                                    true),
                "custom title bar controller window should replace a plain "
                "window") &&
         Expect(ShouldReplaceRegisteredMuonTitleBarWindowForBrowser(true,
                                                                    true),
                "custom title bar controller window should replace another "
                "controller window") &&
         Expect(!ShouldReplaceRegisteredMuonTitleBarWindowForBrowser(true,
                                                                     false),
                "plain window should not replace a custom title bar "
                "controller window");
}

static bool TestTitleBarControllerRegistrationRemoval() {
  char registered_window_storage = 0;
  char requested_window_storage = 0;
  char registered_controller_storage = 0;
  char requested_controller_storage = 0;
  const auto registered_window =
      reinterpret_cast<const CefWindow*>(&registered_window_storage);
  const auto requested_window =
      reinterpret_cast<const CefWindow*>(&requested_window_storage);
  const auto registered_controller =
      reinterpret_cast<const MuonTitleBarController*>(
          &registered_controller_storage);
  const auto requested_controller =
      reinterpret_cast<const MuonTitleBarController*>(
          &requested_controller_storage);

  return Expect(ShouldRemoveRegisteredMuonTitleBarController(
                    registered_window, registered_window,
                    registered_controller, nullptr),
                "same window should remove the title bar controller "
                "registration") &&
         Expect(ShouldRemoveRegisteredMuonTitleBarController(
                    registered_window, requested_window,
                    registered_controller, registered_controller),
                "same controller should remove the title bar controller "
                "registration even when CEF supplies another window wrapper") &&
         Expect(!ShouldRemoveRegisteredMuonTitleBarController(
                    registered_window, requested_window,
                    registered_controller, requested_controller),
                "unrelated window and controller should not remove the title "
                "bar controller registration");
}

static bool TestTitleBarIconNativeScaleFactors() {
  return Expect(GetMuonTitleBarIconPngScaleFactors(16, 16) ==
                    std::vector<float>{1.0f},
                "16px title bar icon should use the default scale only") &&
         Expect(GetMuonTitleBarIconPngScaleFactors(32, 32) ==
                    (std::vector<float>{1.0f, 2.0f}),
                "32px title bar icon should include a 16 DIP scale") &&
         Expect(GetMuonTitleBarIconPngScaleFactors(256, 256) ==
                    (std::vector<float>{1.0f, 2.0f}),
                "embedded default title bar icon should use physical 16 and "
                "32px native bitmaps") &&
         Expect(GetMuonTitleBarIconPngScaleFactors(32, 16) ==
                    std::vector<float>{1.0f},
                "non-square title bar icon should not add a native scale");
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
                 TestInitialWindowShowState() &&
#if defined(OS_LINUX)
                 TestLinuxWindowPropertiesUseDesktopId() &&
#endif
                 TestTitleBarManifestParsing() &&
                 TestInitialWindowWorkAreaBounds() &&
                 TestNativeTitleBarSupportDetection() &&
                 TestLinuxDisplayBackendDetection() &&
                 TestLinuxWaylandVulkanMitigationDetection() &&
                 TestLinuxWaylandAngleOpenGlMitigationDetection() &&
                 TestPageDraggableRegionHitTesting() &&
                 TestPageDraggableRegionSearchKeys() &&
                 TestNativeForwarderRegistersChildWindows() &&
                 TestNativeWheelForwarderTargetWindowSelection() &&
                 TestNativeWheelForwarderTopmostRegisteredWindowAtPoint() &&
                 TestWindowsNonClientDragHitTesting() &&
                 TestTitleBarControlHitTesting() &&
                 TestWindowIconUpdateBehavior() &&
                 TestTitleBarBrowserIdResolution() &&
                 TestTitleBarBrowserWindowRegistrationResolution() &&
                 TestTitleBarControllerRegistrationRemoval() &&
                 TestTitleBarIconNativeScaleFactors() &&
                 TestCustomTitleBarWindowDelegate()
             ? 0
             : 1;
}
