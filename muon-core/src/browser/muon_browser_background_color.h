/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#pragma once

#include "config/muon_config.h"

#include "include/cef_browser.h"

/**
 * Operating system color scheme result used for browser background resolution.
 */
enum MuonSystemColorScheme : uint32_t {
  /** The operating system color scheme could not be determined. */
  kMuonSystemColorSchemeUnknown = 0,
  /** The operating system prefers a dark app color scheme. */
  kMuonSystemColorSchemeDark = 1,
  /** The operating system prefers a light app color scheme. */
  kMuonSystemColorSchemeLight = 2,
};

/**
 * Browser background color resolved for CEF.
 */
struct MuonResolvedBrowserBackgroundColor {
  /**
   * Whether a CEF background color should be assigned.
   */
  bool has_color = false;
  /**
   * Opaque ARGB CEF color assigned when has_color is true.
   */
  cef_color_t color = 0;
};

/**
 * Resolves a configured browser background color with an injected system
 * color scheme.
 *
 * @param background_color Configured browser background color.
 * @param system_color_scheme Detected or injected operating system color
 * scheme.
 * @return Resolved CEF color, or has_color=false when CEF defaults should be
 * used.
 */
MuonResolvedBrowserBackgroundColor ResolveMuonBrowserBackgroundColorForSystemScheme(
    const MuonBrowserBackgroundColorConfig& background_color,
    MuonSystemColorScheme system_color_scheme);

/**
 * Resolves a configured browser background color using the host system color
 * scheme.
 *
 * @param background_color Configured browser background color.
 * @return Resolved CEF color, or has_color=false when CEF defaults should be
 * used.
 */
MuonResolvedBrowserBackgroundColor ResolveMuonBrowserBackgroundColor(
    const MuonBrowserBackgroundColorConfig& background_color);

/**
 * Applies a configured browser background color to CEF browser settings.
 *
 * @param settings Browser settings that will be passed to CEF.
 * @param background_color Configured browser background color.
 */
void ApplyMuonBrowserBackgroundColor(
    CefBrowserSettings& settings,
    const MuonBrowserBackgroundColorConfig& background_color);
