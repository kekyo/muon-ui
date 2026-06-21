/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "browser/muon_window_delegate.h"

#include "browser/muon_title_bar.h"
#include "browser/muon_window_state.h"
#include "browser/muon_window_title.h"

#include "include/cef_browser.h"
#include "include/cef_task.h"
#include "include/views/cef_box_layout.h"
#include "include/views/cef_panel.h"

#include <utility>

MuonWindowDelegate::MuonWindowDelegate(
    CefRefPtr<CefBrowserView> browser_view,
    bool is_devtools,
    MuonBrowserInitialWindowState initial_window_state,
    bool initial_title_bar_visibility,
    MuonTitleBarManifest title_bar_manifest,
    MuonTitleBarBackgroundColor title_bar_background_color)
    : browser_view_(browser_view),
      is_devtools_(is_devtools),
      initial_window_state_(initial_window_state),
      initial_title_bar_visibility_(initial_title_bar_visibility),
      title_bar_manifest_(std::move(title_bar_manifest)),
      title_bar_background_color_(title_bar_background_color) {}

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
  const auto title = is_devtools_ ? GetMuonDevToolsWindowTitle()
                                  : GetMuonDefaultWindowTitle();
  window->SetTitle(title);
  if (UseCustomTitleBar()) {
    title_bar_controller_ = new MuonTitleBarController(
        title_bar_manifest_, title_bar_background_color_);
    title_bar_controller_->SetTitle(title);
    title_bar_controller_->SetActive(window->IsActive());
    title_bar_view_ = title_bar_controller_->CreateBrowserView();
    title_bar_controller_->SetVisible(initial_title_bar_visibility_);

    CefBoxLayoutSettings settings;
    settings.horizontal = false;
    auto layout = window->SetToBoxLayout(settings);
    if (browser_view_) {
      window->AddChildView(browser_view_);
      if (layout) {
        layout->SetFlexForView(browser_view_, 1);
      }
    }
    if (title_bar_view_) {
      window->AddChildViewAt(title_bar_view_, 0);
      RegisterMuonTitleBarView(window, title_bar_view_);
    }
    title_bar_controller_->AttachWindow(window);
    RegisterMuonTitleBarBrowserView(window, browser_view_);
    auto browser_id = 0;
    if (browser_view_) {
      const auto browser = browser_view_->GetBrowser();
      if (browser) {
        browser_id = browser->GetIdentifier();
      }
    }
    RegisterMuonTitleBarController(
        window, title_bar_controller_, browser_id);
    SetRegisteredMuonTitleBarVisibility(
        window, initial_title_bar_visibility_);
  } else if (browser_view_) {
    window->AddChildView(browser_view_);
  }
  ApplyInitialWindowState(window, initial_window_state_);
}

void MuonWindowDelegate::OnWindowDestroyed(CefRefPtr<CefWindow> window) {
  UnregisterMuonTitleBarController(window);
  if (title_bar_controller_) {
    title_bar_controller_->DetachWindow();
  }
  title_bar_controller_ = nullptr;
  title_bar_view_ = nullptr;
  browser_view_ = nullptr;
}

void MuonWindowDelegate::OnWindowActivationChanged(CefRefPtr<CefWindow> window,
                                                   bool active) {
  (void)window;
  if (title_bar_controller_) {
    title_bar_controller_->SetActive(active);
  }
}

void MuonWindowDelegate::OnWindowBoundsChanged(CefRefPtr<CefWindow> window,
                                               const CefRect& new_bounds) {
  (void)new_bounds;
  if (title_bar_controller_) {
    title_bar_controller_->SetMaximized(window && window->IsMaximized());
    title_bar_controller_->UpdateDraggableRegions();
  }
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
  (void)view;
  const auto height =
      UseCustomTitleBar() && initial_title_bar_visibility_
          ? 768 + title_bar_manifest_.height
          : 768;
  return CefSize(1024, height);
}

bool MuonWindowDelegate::IsFrameless(CefRefPtr<CefWindow> window) {
  (void)window;
  return UseCustomTitleBar();
}

cef_show_state_t MuonWindowDelegate::GetInitialShowState(
    CefRefPtr<CefWindow> window) {
  (void)window;
  return GetMuonCefInitialShowState(initial_window_state_);
}

cef_runtime_style_t MuonWindowDelegate::GetWindowRuntimeStyle() {
  return is_devtools_ ? CEF_RUNTIME_STYLE_CHROME : CEF_RUNTIME_STYLE_ALLOY;
}

bool MuonWindowDelegate::UseCustomTitleBar() const {
  return !is_devtools_ && IsCustomMuonTitleBar(title_bar_manifest_);
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
