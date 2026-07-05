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
const contextMenuEventName = "muon-browser-context-menu-command";
const contextMenuWhenKeys = [
  "editable",
  "selection",
  "link",
  "image",
  "canCopy",
  "canPaste",
];
let contextMenuToken = "";
let contextMenuHandler = undefined;
let contextMenuListenerRegistered = false;
const createContextMenuToken = () => {
  const crypto = globalThis.crypto;
  if (
    crypto &&
    typeof crypto.getRandomValues === "function" &&
    typeof globalThis.Uint32Array === "function"
  ) {
    const values = new globalThis.Uint32Array(4);
    crypto.getRandomValues(values);
    return Array.from(values, (value) => value.toString(16).padStart(8, "0")).join("");
  }
  return `${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}`;
};
const ensureContextMenuListener = () => {
  if (contextMenuListenerRegistered || typeof globalThis.addEventListener !== "function") {
    return;
  }
  globalThis.addEventListener(contextMenuEventName, (event) => {
    const detail = event && event.detail;
    if (
      typeof detail !== "object" ||
      detail === null ||
      detail.token !== contextMenuToken ||
      typeof contextMenuHandler !== "function"
    ) {
      return;
    }
    contextMenuHandler(detail);
  });
  contextMenuListenerRegistered = true;
};
const normalizeContextMenuPlacement = (placement) => {
  if (placement === undefined) {
    return undefined;
  }
  if (placement === "start" || placement === "afterEdit" || placement === "end") {
    return placement;
  }
  throw new TypeError("Invalid context menu placement");
};
const normalizeContextMenuWhen = (when) => {
  if (when === undefined) {
    return undefined;
  }
  if (typeof when !== "object" || when === null || Array.isArray(when)) {
    throw new TypeError("Invalid context menu condition");
  }
  const normalized = {};
  for (const key of contextMenuWhenKeys) {
    const value = when[key];
    if (value === undefined) {
      continue;
    }
    if (typeof value !== "boolean") {
      throw new TypeError("Invalid context menu condition");
    }
    normalized[key] = value;
  }
  return normalized;
};
const normalizeContextMenuCommandId = (id) => {
  if (
    typeof id !== "string" ||
    id === "" ||
    /[\u0000-\u001f]/u.test(id) ||
    id.startsWith("muon.") ||
    id.startsWith("standard.")
  ) {
    throw new TypeError("Invalid context menu item id");
  }
  return id;
};
const normalizeContextMenuItem = (item) => {
  if (typeof item !== "object" || item === null || Array.isArray(item)) {
    throw new TypeError("Invalid context menu item");
  }
  const placement = normalizeContextMenuPlacement(item.placement);
  const when = normalizeContextMenuWhen(item.when);
  if (item.type === "separator") {
    return {
      type: "separator",
      ...(placement === undefined ? {} : { placement }),
      ...(when === undefined ? {} : { when }),
    };
  }
  if (item.type !== undefined && item.type !== "item") {
    throw new TypeError("Invalid context menu item type");
  }
  if (typeof item.label !== "string" || item.label === "") {
    throw new TypeError("Invalid context menu item label");
  }
  if (item.enabled !== undefined && typeof item.enabled !== "boolean") {
    throw new TypeError("Invalid context menu item enabled state");
  }
  return {
    id: normalizeContextMenuCommandId(item.id),
    label: item.label,
    enabled: item.enabled === undefined ? true : item.enabled,
    ...(placement === undefined ? {} : { placement }),
    ...(when === undefined ? {} : { when }),
  };
};
const normalizeContextMenuItems = (items) => {
  if (!Array.isArray(items)) {
    throw new TypeError("Invalid context menu items");
  }
  return items.map(normalizeContextMenuItem);
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
if (isAllowed("setContextMenuItems")) {
  properties.setContextMenuItems = {
    enumerable: true,
    configurable: false,
    writable: false,
    value: async (items, handler = undefined) => {
      if (handler !== undefined && typeof handler !== "function") {
        throw new TypeError("Invalid context menu handler");
      }
      const normalized = normalizeContextMenuItems(items);
      const token = createContextMenuToken();
      await namespace.__setContextMenuItems(JSON.stringify(normalized), token);
      contextMenuToken = token;
      contextMenuHandler = handler;
      ensureContextMenuListener();
    },
  };
}
if (isAllowed("clearContextMenuItems")) {
  properties.clearContextMenuItems = {
    enumerable: true,
    configurable: false,
    writable: false,
    value: async () => {
      contextMenuToken = "";
      contextMenuHandler = undefined;
      await namespace.__clearContextMenuItems();
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

static const std::array<MuonTypeMetadata, 2>
    kMuonBuiltinBrowserContextMenuItemsArgs = {{
        {CreateMuonPrimitiveType(MUON_TYPE_STRING)},
        {CreateMuonPrimitiveType(MUON_TYPE_STRING)},
    }};

static const std::array<MuonBuiltinBrowserFunctionDefinition, 24>
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
        {"__setContextMenuItems",
         MuonBuiltinBrowserFunctionKind::SetContextMenuItems,
         "setContextMenuItems",
         kMuonBuiltinBrowserContextMenuItemsArgs.data(),
         kMuonBuiltinBrowserContextMenuItemsArgs.size(),
         CreateMuonPrimitiveType(MUON_TYPE_VOID)},
        {"__clearContextMenuItems",
         MuonBuiltinBrowserFunctionKind::ClearContextMenuItems,
         "clearContextMenuItems"},
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
