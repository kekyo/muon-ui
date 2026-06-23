/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#pragma once

#include "include/views/cef_window.h"

#include <vector>

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
