/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once

#include "include/cef_browser.h"
#include "include/cef_keyboard_handler.h"

/**
 * Handles browser shortcuts for key events delivered by non-browser views.
 */
class MuonBrowserShortcutHandler : public virtual CefBaseRefCounted {
 public:
  /**
   * Handles a browser shortcut for a key event.
   *
   * @param browser Browser associated with the active window.
   * @param event CEF key event.
   * @param is_keyboard_shortcut Output flag set when the event is handled.
   * @return true when the event was handled.
   */
  virtual bool HandleBrowserShortcut(CefRefPtr<CefBrowser> browser,
                                     const CefKeyEvent& event,
                                     bool* is_keyboard_shortcut) = 0;
};
