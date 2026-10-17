/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once

#include "include/views/cef_window.h"

#include <functional>

/**
 * Covers an owner's client area while a native modal dialog is pending.
 *
 * @param window Owner window, accessed only on the CEF UI thread.
 * @param present_dialog Callback that presents the owner's current dialog.
 * @return Overlay owned by the caller until the last dialog completes.
 */
CefRefPtr<CefOverlayController> CreateMuonModalInputOverlay(
    CefRefPtr<CefWindow> window,
    std::function<void()> present_dialog);

/**
 * Returns whether a window currently blocks input for a native dialog.
 * @param window Owner window.
 * @return true while the owner has a registered overlay.
 */
bool IsMuonModalInputBlocked(CefRefPtr<CefWindow> window);

/**
 * Redirects an owner's user interaction to its current native dialog.
 * @param window Owner window; hidden or closing owners are not presented.
 * @return true when the interaction belongs to a blocked owner.
 */
bool PresentMuonModalDialog(CefRefPtr<CefWindow> window);

/**
 * Updates input coverage after the owner has been laid out or resized.
 * @param window Owner window.
 */
void UpdateMuonModalInputBounds(CefRefPtr<CefWindow> window);

/**
 * Removes an owner's input routing before destroying its overlay or window.
 * @param window Owner window.
 */
void UnregisterMuonModalInput(CefRefPtr<CefWindow> window);
