/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once

#include "browser/muon_browser_shortcut_handler.h"
#include "browser/muon_title_bar.h"

#include "include/views/cef_browser_view.h"

#include <string>

class MuonWindowCloseHandler;

/**
 * Browser view delegate that creates matching popup window delegates.
 */
class MuonBrowserViewDelegate final : public CefBrowserViewDelegate {
 public:
  /**
   * Creates a browser view delegate.
   *
   * @param is_devtools Whether the view is for DevTools.
   * @param initial_title_bar_visibility Whether custom title bars start
   * visible.
   * @param title_bar_manifest Parsed title bar provider manifest.
   * @param title_bar_background_color Explicit title bar background color.
   * @param shortcut_handler Browser shortcut handler for window-level events.
   * @param close_handler Browser close handler for pending owner work.
   * @param linux_desktop_id Desktop identifier for Linux window metadata.
   */
  explicit MuonBrowserViewDelegate(
      bool is_devtools,
      bool initial_title_bar_visibility = true,
      MuonTitleBarManifest title_bar_manifest =
          CreateNativeMuonTitleBarManifest(),
      MuonTitleBarBackgroundColor title_bar_background_color = {},
      CefRefPtr<MuonBrowserShortcutHandler> shortcut_handler = nullptr,
      MuonWindowCloseHandler* close_handler = nullptr,
      std::string linux_desktop_id = "muon");

  /**
   * Creates delegates for popup browser views.
   *
   * @param browser_view Parent browser view.
   * @param settings Browser settings for the popup.
   * @param client Popup browser client.
   * @param is_devtools Whether the popup is for DevTools.
   * @return Delegate for the popup browser view.
   */
  CefRefPtr<CefBrowserViewDelegate> GetDelegateForPopupBrowserView(
      CefRefPtr<CefBrowserView> browser_view,
      const CefBrowserSettings& settings,
      CefRefPtr<CefClient> client,
      bool is_devtools) override;

  /**
   * Opens popup browser views in their own top-level window.
   *
   * @param browser_view Parent browser view.
   * @param popup_browser_view Newly created popup browser view.
   * @param is_devtools Whether the popup is for DevTools.
   * @return true when popup creation was handled.
   */
  bool OnPopupBrowserViewCreated(
      CefRefPtr<CefBrowserView> browser_view,
      CefRefPtr<CefBrowserView> popup_browser_view,
      bool is_devtools) override;

  /**
   * Returns the runtime style for browser or DevTools views.
   */
  cef_runtime_style_t GetBrowserRuntimeStyle() override;

 private:
  const bool is_devtools_;
  const bool initial_title_bar_visibility_;
  const MuonTitleBarManifest title_bar_manifest_;
  const MuonTitleBarBackgroundColor title_bar_background_color_;
  const std::string linux_desktop_id_;
  CefRefPtr<MuonBrowserShortcutHandler> shortcut_handler_;
  MuonWindowCloseHandler* close_handler_;

  IMPLEMENT_REFCOUNTING(MuonBrowserViewDelegate);
  DISALLOW_COPY_AND_ASSIGN(MuonBrowserViewDelegate);
};
