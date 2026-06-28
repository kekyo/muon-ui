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
const int32Min = -2147483648;
const int32Max = 2147483647;
const isWindowBoundsInteger = (value) =>
  typeof value === "number" &&
  Number.isSafeInteger(value) &&
  value >= int32Min &&
  value <= int32Max;
const normalizeWindowBounds = (bounds) => {
  if (typeof bounds !== "object" || bounds === null) {
    throw new TypeError("Invalid window bounds");
  }
  const { x, y, width, height } = bounds;
  if (
    !isWindowBoundsInteger(x) ||
    !isWindowBoundsInteger(y) ||
    !isWindowBoundsInteger(width) ||
    !isWindowBoundsInteger(height) ||
    width <= 0 ||
    height <= 0
  ) {
    throw new TypeError("Invalid window bounds");
  }
  return { x, y, width, height };
};
const properties = {};
if (isAllowed("getWindowBounds")) {
  properties.getWindowBounds = {
    enumerable: true,
    configurable: false,
    writable: false,
    value: async () => JSON.parse(await namespace.__getWindowBounds()),
  };
}
if (isAllowed("setWindowBounds")) {
  properties.setWindowBounds = {
    enumerable: true,
    configurable: false,
    writable: false,
    value: (bounds) => {
      const normalized = normalizeWindowBounds(bounds);
      return namespace.__setWindowBounds(
        normalized.x,
        normalized.y,
        normalized.width,
        normalized.height,
      );
    },
  };
}
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

static const std::array<MuonTypeMetadata, 4> kMuonBuiltinBrowserWindowBoundsArgs =
    {{
        {CreateMuonPrimitiveType(MUON_TYPE_I32)},
        {CreateMuonPrimitiveType(MUON_TYPE_I32)},
        {CreateMuonPrimitiveType(MUON_TYPE_I32)},
        {CreateMuonPrimitiveType(MUON_TYPE_I32)},
    }};

static const std::array<MuonBuiltinBrowserFunctionDefinition, 22>
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
        {"__getWindowBounds",
         MuonBuiltinBrowserFunctionKind::GetWindowBounds,
         "getWindowBounds",
         nullptr,
         0,
         CreateMuonPrimitiveType(MUON_TYPE_STRING)},
        {"__setWindowBounds",
         MuonBuiltinBrowserFunctionKind::SetWindowBounds,
         "setWindowBounds",
         kMuonBuiltinBrowserWindowBoundsArgs.data(),
         kMuonBuiltinBrowserWindowBoundsArgs.size(),
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
