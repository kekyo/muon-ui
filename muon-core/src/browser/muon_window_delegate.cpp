/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "browser/muon_window_delegate.h"

#include "browser/muon_window_state.h"
#include "browser/muon_window_title.h"

#include "include/cef_browser.h"
#include "include/cef_task.h"

MuonWindowDelegate::MuonWindowDelegate(
    CefRefPtr<CefBrowserView> browser_view,
    bool is_devtools,
    MuonBrowserInitialWindowState initial_window_state)
    : browser_view_(browser_view),
      is_devtools_(is_devtools),
      initial_window_state_(initial_window_state) {}

class ApplyInitialWindowStateTask final : public CefTask {
 public:
  ApplyInitialWindowStateTask(
      CefRefPtr<CefWindow> window,
      MuonBrowserInitialWindowState initial_window_state)
      : window_(window), initial_window_state_(initial_window_state) {}

  void Execute() override {
    if (!window_) {
      return;
    }
    switch (initial_window_state_) {
      case kMuonBrowserInitialWindowStateMinimized:
        window_->Minimize();
        break;
      case kMuonBrowserInitialWindowStateMaximized:
        window_->Maximize();
        break;
      case kMuonBrowserInitialWindowStateFullscreen:
        SetMuonWindowFullscreen(window_, true);
        break;
      case kMuonBrowserInitialWindowStateNormal:
      case kMuonBrowserInitialWindowStateHidden:
        break;
    }
  }

 private:
  CefRefPtr<CefWindow> window_;
  const MuonBrowserInitialWindowState initial_window_state_;

  IMPLEMENT_REFCOUNTING(ApplyInitialWindowStateTask);
  DISALLOW_COPY_AND_ASSIGN(ApplyInitialWindowStateTask);
};

static void ApplyInitialWindowState(
    CefRefPtr<CefWindow> window,
    MuonBrowserInitialWindowState initial_window_state) {
  switch (initial_window_state) {
    case kMuonBrowserInitialWindowStateHidden:
      break;
    case kMuonBrowserInitialWindowStateNormal:
    case kMuonBrowserInitialWindowStateMinimized:
    case kMuonBrowserInitialWindowStateMaximized:
    case kMuonBrowserInitialWindowStateFullscreen:
      window->Show();
      CefPostTask(TID_UI,
                  new ApplyInitialWindowStateTask(window, initial_window_state));
      break;
  }
}

void MuonWindowDelegate::OnWindowCreated(CefRefPtr<CefWindow> window) {
  window->SetTitle(is_devtools_ ? GetMuonDevToolsWindowTitle()
                                : GetMuonDefaultWindowTitle());
  window->AddChildView(browser_view_);
  ApplyInitialWindowState(window, initial_window_state_);
}

void MuonWindowDelegate::OnWindowDestroyed(CefRefPtr<CefWindow> window) {
  browser_view_ = nullptr;
}

bool MuonWindowDelegate::CanClose(CefRefPtr<CefWindow> window) {
  if (!browser_view_) {
    return true;
  }

  auto browser = browser_view_->GetBrowser();
  if (browser) {
    return browser->GetHost()->TryCloseBrowser();
  }
  return true;
}

CefSize MuonWindowDelegate::GetPreferredSize(CefRefPtr<CefView> view) {
  return CefSize(1024, 768);
}

cef_show_state_t MuonWindowDelegate::GetInitialShowState(
    CefRefPtr<CefWindow> window) {
  (void)window;
  return GetMuonCefInitialShowState(initial_window_state_);
}

cef_runtime_style_t MuonWindowDelegate::GetWindowRuntimeStyle() {
  return is_devtools_ ? CEF_RUNTIME_STYLE_CHROME : CEF_RUNTIME_STYLE_ALLOY;
}

#if defined(OS_LINUX)
bool MuonWindowDelegate::GetLinuxWindowProperties(
    CefRefPtr<CefWindow> window,
    CefLinuxWindowProperties& properties) {
  CefString(&properties.wayland_app_id).FromASCII("muon");
  CefString(&properties.wm_class_class).FromASCII("muon");
  CefString(&properties.wm_class_name).FromASCII("muon");
  CefString(&properties.wm_role_name)
      .FromASCII(is_devtools_ ? "devtools" : "browser");
  return true;
}
#endif
