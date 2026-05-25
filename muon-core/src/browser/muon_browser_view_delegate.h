/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#pragma once

#include "include/views/cef_browser_view.h"

/**
 * Browser view delegate that creates matching popup window delegates.
 */
class MuonBrowserViewDelegate final : public CefBrowserViewDelegate {
 public:
  /**
   * Creates a browser view delegate.
   *
   * @param is_devtools Whether the view is for DevTools.
   */
  explicit MuonBrowserViewDelegate(bool is_devtools);

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

  IMPLEMENT_REFCOUNTING(MuonBrowserViewDelegate);
  DISALLOW_COPY_AND_ASSIGN(MuonBrowserViewDelegate);
};
