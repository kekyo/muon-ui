/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#pragma once

#include "include/cef_browser.h"
#include "include/cef_task.h"

/**
 * CEF UI-thread task that opens DevTools for a browser.
 */
class ShowDevToolsTask final : public CefTask {
 public:
  /**
   * Creates a DevTools task for the specified browser.
   *
   * @param browser Browser whose DevTools window should be shown.
   */
  explicit ShowDevToolsTask(CefRefPtr<CefBrowser> browser);

  /**
   * Shows DevTools on the CEF UI thread.
   */
  void Execute() override;

 private:
  CefRefPtr<CefBrowser> browser_;

  IMPLEMENT_REFCOUNTING(ShowDevToolsTask);
  DISALLOW_COPY_AND_ASSIGN(ShowDevToolsTask);
};
