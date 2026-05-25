/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "browser/muon_builtin_browser.h"
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
      "__close",         "__shutdown",
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
      MuonBuiltinBrowserFunctionKind::Close,
      MuonBuiltinBrowserFunctionKind::Shutdown,
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
  const auto shutdown = definitions.back();
  if (!Expect(shutdown.filter_name != nullptr &&
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

int main() {
  return TestBrowserFunctionDefinitions() && TestWindowTitleFallback() &&
                 TestInitialWindowShowState()
             ? 0
             : 1;
}
