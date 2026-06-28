/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#pragma once

#include "config/muon_config.h"

#include "include/views/cef_window.h"

/**
 * Returns the CEF show state for the configured initial browser window state.
 *
 * @param initial_window_state Configured initial browser window state.
 * @return CEF show state requested when the native window is created.
 */
cef_show_state_t GetMuonCefInitialShowState(
    MuonBrowserInitialWindowState initial_window_state);

/**
 * Requests fullscreen state for a CEF window.
 *
 * @param window Window to update.
 * @param fullscreen Whether fullscreen should be entered or exited.
 */
void SetMuonWindowFullscreen(CefRefPtr<CefWindow> window, bool fullscreen);

/**
 * Returns initial top-level window bounds centered inside the display work area.
 *
 * @param work_area Display work area excluding OS panels and docks.
 * @param preferred_size Preferred window size requested by the delegate.
 * @return Window bounds clamped to fit inside the work area.
 */
CefRect GetMuonCenteredWindowBounds(const CefRect& work_area,
                                    const CefSize& preferred_size);

/**
 * Shows a CEF window and restores it from minimized/iconified state.
 *
 * @param window Window to show.
 */
void ShowMuonWindow(CefRefPtr<CefWindow> window);
