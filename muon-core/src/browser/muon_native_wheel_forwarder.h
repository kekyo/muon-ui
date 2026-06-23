/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#pragma once

#include "include/views/cef_window.h"

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
