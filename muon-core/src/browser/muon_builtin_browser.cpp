/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "browser/muon_builtin_browser.h"

#include <array>

static constexpr char kMuonBuiltinBrowserPluginNamespace[] = "muon.browser";
static constexpr char kMuonBuiltinBrowserSetupScript[] = R"JS(
const ownerCloseEventName = "muon-owner-browser-close";
const dispatchOwnerBrowserClose = () => {
  if (
    typeof globalThis.dispatchEvent !== "function" ||
    typeof globalThis.Event !== "function"
  ) {
    return;
  }
  globalThis.dispatchEvent(new globalThis.Event(ownerCloseEventName));
};
const abortModalFsDialogs = () => {
  const abortDialogs = globalThis.__muonAbortModalFsDialogs;
  if (typeof abortDialogs === "function") {
    abortDialogs();
  }
};
const properties = {};
if (isAllowed("close")) {
  properties.close = {
    enumerable: true,
    configurable: false,
    writable: false,
    value: () => {
      abortModalFsDialogs();
      dispatchOwnerBrowserClose();
      return new Promise((resolve) => {
        globalThis.setTimeout(resolve, 0);
      }).then(() => namespace.__close());
    },
  };
}
if (isAllowed("shutdown")) {
  properties.shutdown = {
    enumerable: true,
    configurable: false,
    writable: false,
    value: (exitCode = 0) => namespace.__shutdown(exitCode),
  };
}
if (isAllowed("recycle")) {
  properties.recycle = {
    enumerable: true,
    configurable: false,
    writable: false,
    value: () => namespace.__recycle(),
  };
}
Object.defineProperties(namespace, properties);
)JS";

static const std::array<MuonTypeMetadata, 1> kMuonBuiltinBrowserShutdownArgs = {
    {CreateMuonPrimitiveType(MUON_TYPE_I32)},
};

static const std::array<MuonTypeMetadata, 1>
    kMuonBuiltinBrowserTitleBarVisibilityArgs = {
        {CreateMuonPrimitiveType(MUON_TYPE_BOOL)},
};

static const std::array<MuonTypeMetadata, 1> kMuonBuiltinBrowserTitleBarIconArgs =
    {
        {CreateMuonPrimitiveType(MUON_TYPE_STRING)},
};

static const std::array<MuonBuiltinBrowserFunctionDefinition, 20>
    kMuonBuiltinBrowserFunctions = {{
        {"reload", MuonBuiltinBrowserFunctionKind::Reload},
        {"hardReload", MuonBuiltinBrowserFunctionKind::HardReload},
        {"toggleFullscreen", MuonBuiltinBrowserFunctionKind::ToggleFullscreen},
        {"enterFullscreen", MuonBuiltinBrowserFunctionKind::EnterFullscreen},
        {"exitFullscreen", MuonBuiltinBrowserFunctionKind::ExitFullscreen},
        {"zoomIn", MuonBuiltinBrowserFunctionKind::ZoomIn},
        {"zoomOut", MuonBuiltinBrowserFunctionKind::ZoomOut},
        {"resetZoom", MuonBuiltinBrowserFunctionKind::ResetZoom},
        {"show", MuonBuiltinBrowserFunctionKind::Show},
        {"hide", MuonBuiltinBrowserFunctionKind::Hide},
        {"focus", MuonBuiltinBrowserFunctionKind::Focus},
        {"blur", MuonBuiltinBrowserFunctionKind::Blur},
        {"minimize", MuonBuiltinBrowserFunctionKind::Minimize},
        {"maximize", MuonBuiltinBrowserFunctionKind::Maximize},
        {"restore", MuonBuiltinBrowserFunctionKind::Restore},
        {"setTitleBarVisibility",
         MuonBuiltinBrowserFunctionKind::SetTitleBarVisibility,
         nullptr,
         kMuonBuiltinBrowserTitleBarVisibilityArgs.data(),
         kMuonBuiltinBrowserTitleBarVisibilityArgs.size(),
         CreateMuonPrimitiveType(MUON_TYPE_VOID)},
        {"setTitleBarIcon",
         MuonBuiltinBrowserFunctionKind::SetTitleBarIcon,
         nullptr,
         kMuonBuiltinBrowserTitleBarIconArgs.data(),
         kMuonBuiltinBrowserTitleBarIconArgs.size(),
         CreateMuonPrimitiveType(MUON_TYPE_VOID)},
        {"__close",
         MuonBuiltinBrowserFunctionKind::Close,
         "close"},
        {"__shutdown",
         MuonBuiltinBrowserFunctionKind::Shutdown,
         "shutdown",
         kMuonBuiltinBrowserShutdownArgs.data(),
         kMuonBuiltinBrowserShutdownArgs.size(),
         CreateMuonPrimitiveType(MUON_TYPE_VOID)},
        {"__recycle",
         MuonBuiltinBrowserFunctionKind::Recycle,
         "recycle"},
    }};

const char* GetMuonBuiltinBrowserPluginNamespace() {
  return kMuonBuiltinBrowserPluginNamespace;
}

std::span<const MuonBuiltinBrowserFunctionDefinition>
GetMuonBuiltinBrowserFunctionDefinitions() {
  return kMuonBuiltinBrowserFunctions;
}

const char* GetMuonBuiltinBrowserSetupScript() {
  return kMuonBuiltinBrowserSetupScript;
}
