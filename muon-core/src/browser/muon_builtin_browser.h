/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#pragma once

#include "plugins/muon_type_metadata.h"

#include <cstddef>
#include <span>

/**
 * Built-in browser operation exposed under window.muon.browser.
 */
enum class MuonBuiltinBrowserFunctionKind {
  None,
  Reload,
  HardReload,
  ToggleFullscreen,
  EnterFullscreen,
  ExitFullscreen,
  ZoomIn,
  ZoomOut,
  ResetZoom,
  Show,
  Hide,
  Focus,
  Blur,
  Minimize,
  Maximize,
  Restore,
  SetTitleBarVisibility,
  Close,
  Shutdown,
};

/**
 * Static definition for one built-in browser operation.
 */
struct MuonBuiltinBrowserFunctionDefinition {
  /**
   * JavaScript function name under window.muon.browser.
   */
  const char* js_name = nullptr;
  /**
   * Native operation represented by this function.
   */
  MuonBuiltinBrowserFunctionKind kind = MuonBuiltinBrowserFunctionKind::None;
  /**
   * Public function name used for allow filtering and setup scripts.
   */
  const char* filter_name = nullptr;
  /**
   * Function argument types.
   */
  const MuonTypeMetadata* arg_types = nullptr;
  /**
   * Number of function argument types.
   */
  size_t arg_count = 0;
  /**
   * Promise resolution type for this function.
   */
  MuonTypeMetadata return_type = CreateMuonPrimitiveType(MUON_TYPE_VOID);
};

/**
 * Returns the JavaScript namespace for built-in browser operations.
 */
const char* GetMuonBuiltinBrowserPluginNamespace();

/**
 * Returns built-in browser function definitions in renderer exposure order.
 *
 * These definitions are intentionally not C ABI plugin metadata. Their ids are
 * assigned by the plugin runtime and dispatched by browser kind.
 */
std::span<const MuonBuiltinBrowserFunctionDefinition>
GetMuonBuiltinBrowserFunctionDefinitions();

/**
 * Returns the setup script for browser wrapper functions.
 */
const char* GetMuonBuiltinBrowserSetupScript();
