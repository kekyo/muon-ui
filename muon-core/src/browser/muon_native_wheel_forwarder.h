/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#pragma once

#include "include/views/cef_window.h"

#include <vector>

/**
 * Snapshot entry for a top-level native window considered for wheel forwarding.
 *
 * @remarks
 * The entries are expected to be ordered from bottom-most to top-most.
 */
struct MuonNativeWheelForwarderTopLevelWindow {
  CefWindowHandle window_handle = 0;
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  bool visible = false;
  bool override_redirect = false;
};

/**
 * Builds unique native window handles that should be observed for page CSS
 * draggable-region input forwarding.
 *
 * @param root_window_handle Top-level native window handle.
 * @param child_window_handles Descendant native window handles.
 * @return Ordered handles to register for native input forwarding.
 */
std::vector<CefWindowHandle> GetMuonNativeForwarderWindowHandlesForRegistration(
    CefWindowHandle root_window_handle,
    const std::vector<CefWindowHandle>& child_window_handles);

/**
 * Selects the native window handle used for wheel forwarding.
 *
 * @param event_window_handle Native event window handle.
 * @param child_window_handle Native child window handle from the event, or null.
 * @param registered_window_handles Native window handles registered for forwarding.
 * @return Native window handle to use for page draggable-region lookup.
 */
CefWindowHandle GetMuonNativeWheelForwarderTargetWindowHandle(
    CefWindowHandle event_window_handle,
    CefWindowHandle child_window_handle,
    const std::vector<CefWindowHandle>& registered_window_handles);

/**
 * Selects the top-most registered native window at a screen point.
 *
 * @param screen_point Native root/screen point.
 * @param registered_window_handles Native window handles registered for forwarding.
 * @param top_level_windows Top-level windows in bottom-most to top-most order.
 * @return Registered top-level window handle at the point, or null when the
 * point is covered by another normal top-level window or no registered window
 * contains the point.
 */
CefWindowHandle GetMuonNativeWheelForwarderTopmostRegisteredWindowAtPoint(
    CefPoint screen_point,
    const std::vector<CefWindowHandle>& registered_window_handles,
    const std::vector<MuonNativeWheelForwarderTopLevelWindow>&
        top_level_windows);

/**
 * Returns whether a Windows non-client mouse down hit-test should start muon's
 * custom draggable-region window drag handling.
 *
 * @param native_hit_test Win32 WM_NCLBUTTONDOWN hit-test code.
 * @return true when muon should handle the hit as a title/page drag.
 */
bool ShouldHandleMuonWindowsNonClientDragHitTest(int native_hit_test);

/**
 * Registers native input forwarding for page CSS draggable regions.
 *
 * @param window Window whose native input events should be observed.
 */
void RegisterMuonNativeWheelForwarder(CefRefPtr<CefWindow> window);

/**
 * Unregisters native input forwarding for a window.
 *
 * @param window Window whose native input events should no longer be observed.
 */
void UnregisterMuonNativeWheelForwarder(CefRefPtr<CefWindow> window);

/**
 * Clears all native input forwarding registrations before CEF shutdown.
 */
void ClearMuonNativeWheelForwarders();
