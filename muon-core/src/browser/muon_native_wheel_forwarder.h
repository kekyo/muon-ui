/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#pragma once

#include "include/views/cef_window.h"

/**
 * Registers native wheel forwarding for page CSS draggable regions.
 *
 * @param window Window whose native wheel events should be observed.
 */
void RegisterMuonNativeWheelForwarder(CefRefPtr<CefWindow> window);

/**
 * Unregisters native wheel forwarding for a window.
 *
 * @param window Window whose native wheel events should no longer be observed.
 */
void UnregisterMuonNativeWheelForwarder(CefRefPtr<CefWindow> window);

/**
 * Clears all native wheel forwarding registrations before CEF shutdown.
 */
void ClearMuonNativeWheelForwarders();
