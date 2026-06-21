/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "browser/muon_browser_view_delegate.h"

#include "browser/muon_window_delegate.h"

#include "include/cef_task.h"
#include "include/views/cef_window.h"

#include <utility>

class EnablePopupOpenerBrowserViewTask final : public CefTask {
 public:
  explicit EnablePopupOpenerBrowserViewTask(
      CefRefPtr<CefBrowserView> browser_view)
      : browser_view_(browser_view) {}

  void Execute() override {
    EnablePopupOpenerBrowserView(browser_view_);
  }

  static void EnablePopupOpenerBrowserView(
      CefRefPtr<CefBrowserView> browser_view) {
    if (!browser_view || !browser_view->IsValid()) {
      return;
    }
    const auto window = browser_view->GetWindow();
    if (window && window->IsValid() && !window->IsEnabled()) {
      window->SetEnabled(true);
    }
    if (!browser_view->IsEnabled()) {
      browser_view->SetEnabled(true);
    }
  }

 private:
  CefRefPtr<CefBrowserView> browser_view_;

  IMPLEMENT_REFCOUNTING(EnablePopupOpenerBrowserViewTask);
  DISALLOW_COPY_AND_ASSIGN(EnablePopupOpenerBrowserViewTask);
};

MuonBrowserViewDelegate::MuonBrowserViewDelegate(
    bool is_devtools,
    MuonTitleBarManifest title_bar_manifest,
    MuonTitleBarBackgroundColor title_bar_background_color)
    : is_devtools_(is_devtools),
      title_bar_manifest_(std::move(title_bar_manifest)),
      title_bar_background_color_(title_bar_background_color) {}

CefRefPtr<CefBrowserViewDelegate>
MuonBrowserViewDelegate::GetDelegateForPopupBrowserView(
    CefRefPtr<CefBrowserView> browser_view,
    const CefBrowserSettings& settings,
    CefRefPtr<CefClient> client,
    bool is_devtools) {
  return new MuonBrowserViewDelegate(
      is_devtools, title_bar_manifest_, title_bar_background_color_);
}

bool MuonBrowserViewDelegate::OnPopupBrowserViewCreated(
    CefRefPtr<CefBrowserView> browser_view,
    CefRefPtr<CefBrowserView> popup_browser_view,
    bool is_devtools) {
  CefWindow::CreateTopLevelWindow(
      new MuonWindowDelegate(popup_browser_view, is_devtools,
                             kMuonBrowserInitialWindowStateNormal,
                             title_bar_manifest_,
                             title_bar_background_color_));
  if (!is_devtools) {
    // Popups are modeless in Muon even when they keep an opener reference.
    EnablePopupOpenerBrowserViewTask::EnablePopupOpenerBrowserView(browser_view);
    CefPostTask(TID_UI, new EnablePopupOpenerBrowserViewTask(browser_view));
    CefPostDelayedTask(TID_UI,
                       new EnablePopupOpenerBrowserViewTask(browser_view), 50);
  }
  return true;
}

cef_runtime_style_t MuonBrowserViewDelegate::GetBrowserRuntimeStyle() {
  return is_devtools_ ? CEF_RUNTIME_STYLE_CHROME : CEF_RUNTIME_STYLE_ALLOY;
}
