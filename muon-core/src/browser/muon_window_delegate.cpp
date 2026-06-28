/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "browser/muon_window_delegate.h"

#include "browser/muon_native_wheel_forwarder.h"
#include "browser/muon_title_bar.h"
#include "browser/muon_window_state.h"
#include "browser/muon_window_title.h"
#include "log/muon_close_debug_log.h"

#include "include/cef_browser.h"
#include "include/cef_task.h"
#include "include/views/cef_box_layout.h"
#include "include/views/cef_display.h"
#include "include/views/cef_panel.h"

#include <sstream>
#include <utility>

MuonWindowDelegate::MuonWindowDelegate(
    CefRefPtr<CefBrowserView> browser_view,
    bool is_devtools,
    MuonBrowserInitialWindowState initial_window_state,
    bool initial_title_bar_visibility,
    MuonTitleBarManifest title_bar_manifest,
    MuonTitleBarBackgroundColor title_bar_background_color,
    CefRefPtr<MuonBrowserShortcutHandler> shortcut_handler,
    MuonWindowCloseHandler* close_handler)
    : browser_view_(browser_view),
      shortcut_handler_(shortcut_handler),
      close_handler_(close_handler),
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

static bool GetInitialWindowWorkArea(CefRefPtr<CefWindow> window,
                                     CefRect* work_area) {
  if (!window || work_area == nullptr) {
    return false;
  }
  auto display = window->GetDisplay();
  if (!display) {
    display = CefDisplay::GetPrimaryDisplay();
  }
  if (display) {
    const auto display_work_area = display->GetWorkArea();
    if (display_work_area.width > 0 && display_work_area.height > 0) {
      *work_area = display_work_area;
      return true;
    }
  }
  return false;
}

static int GetMuonWindowDelegateBrowserId(
    CefRefPtr<CefBrowserView> browser_view) {
  if (!browser_view) {
    return 0;
  }
  const auto browser = browser_view->GetBrowser();
  return browser ? browser->GetIdentifier() : 0;
}

void MuonWindowDelegate::OnWindowCreated(CefRefPtr<CefWindow> window) {
  const auto title = is_devtools_ ? GetMuonDevToolsWindowTitle()
                                  : GetMuonDefaultWindowTitle();
  {
    std::ostringstream log;
    log << "WindowDelegate OnWindowCreated begin this="
        << FormatMuonCloseDebugPointer(this)
        << " window=" << FormatMuonCloseDebugPointer(window.get())
        << " browser_view="
        << FormatMuonCloseDebugPointer(browser_view_.get())
        << " browser_id=" << GetMuonWindowDelegateBrowserId(browser_view_)
        << " custom_titlebar=" << FormatMuonCloseDebugBool(UseCustomTitleBar())
        << " is_devtools=" << FormatMuonCloseDebugBool(is_devtools_);
    AppendMuonCloseDebugLog(log.str());
  }
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
    if (!is_devtools_) {
      RegisterMuonNativeWheelForwarder(window);
    }
  } else if (browser_view_) {
    window->AddChildView(browser_view_);
    RegisterMuonTitleBarBrowserView(window, browser_view_);
    if (!is_devtools_) {
      SetRegisteredMuonTitleBarVisibility(
          window, initial_title_bar_visibility_);
    }
  }
  if (!initial_bounds_provided_) {
    window->CenterWindow(GetPreferredSize(nullptr));
  }
  ApplyInitialWindowState(window, initial_window_state_);
  {
    std::ostringstream log;
    log << "WindowDelegate OnWindowCreated end this="
        << FormatMuonCloseDebugPointer(this)
        << " window=" << FormatMuonCloseDebugPointer(window.get())
        << " browser_view="
        << FormatMuonCloseDebugPointer(browser_view_.get())
        << " browser_id=" << GetMuonWindowDelegateBrowserId(browser_view_)
        << " titlebar_controller="
        << FormatMuonCloseDebugPointer(title_bar_controller_.get())
        << " titlebar_view="
        << FormatMuonCloseDebugPointer(title_bar_view_.get());
    AppendMuonCloseDebugLog(log.str());
  }
}

void MuonWindowDelegate::OnWindowDestroyed(CefRefPtr<CefWindow> window) {
  {
    std::ostringstream log;
    log << "WindowDelegate OnWindowDestroyed begin this="
        << FormatMuonCloseDebugPointer(this)
        << " window=" << FormatMuonCloseDebugPointer(window.get())
        << " browser_view="
        << FormatMuonCloseDebugPointer(browser_view_.get())
        << " browser_id=" << GetMuonWindowDelegateBrowserId(browser_view_)
        << " titlebar_controller="
        << FormatMuonCloseDebugPointer(title_bar_controller_.get())
        << " titlebar_view="
        << FormatMuonCloseDebugPointer(title_bar_view_.get());
    AppendMuonCloseDebugLog(log.str());
  }
  UnregisterMuonNativeWheelForwarder(window);
  UnregisterMuonTitleBarController(window);
  if (title_bar_controller_) {
    UnregisterMuonTitleBarController(title_bar_controller_);
    title_bar_controller_->DetachWindow();
  }
  title_bar_controller_ = nullptr;
  title_bar_view_ = nullptr;
  browser_view_ = nullptr;
  shortcut_handler_ = nullptr;
  {
    std::ostringstream log;
    log << "WindowDelegate OnWindowDestroyed end this="
        << FormatMuonCloseDebugPointer(this)
        << " window=" << FormatMuonCloseDebugPointer(window.get());
    AppendMuonCloseDebugLog(log.str());
  }
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
    title_bar_controller_->UpdateDraggableRegions(window);
  }
}

bool MuonWindowDelegate::CanClose(CefRefPtr<CefWindow> window) {
  {
    std::ostringstream log;
    log << "WindowDelegate CanClose enter this="
        << FormatMuonCloseDebugPointer(this)
        << " window=" << FormatMuonCloseDebugPointer(window.get())
        << " browser_view="
        << FormatMuonCloseDebugPointer(browser_view_.get())
        << " browser_id=" << GetMuonWindowDelegateBrowserId(browser_view_)
        << " is_devtools=" << FormatMuonCloseDebugBool(is_devtools_);
    AppendMuonCloseDebugLog(log.str());
  }
  if (!browser_view_) {
    AppendMuonCloseDebugLog(
        "WindowDelegate CanClose return true reason=no_browser_view");
    return true;
  }

  auto browser = browser_view_->GetBrowser();
  if (browser) {
    const auto host = browser->GetHost();
    const auto ready_to_be_closed = host && host->IsReadyToBeClosed();
    const auto valid = browser->IsValid();
    const auto has_pending_fs_dialog =
        close_handler_ != nullptr &&
        close_handler_->HasPendingFsDialogCallForBrowser(
            browser->GetIdentifier());
    const auto result = host && host->TryCloseBrowser();
    const auto pending_fs_dialog_close_requested =
        !result && has_pending_fs_dialog &&
        close_handler_->RequestCloseAfterPendingFsDialog(browser, window);
    std::ostringstream log;
    log << "WindowDelegate CanClose browser this="
        << FormatMuonCloseDebugPointer(this)
        << " window=" << FormatMuonCloseDebugPointer(window.get())
        << " browser=" << FormatMuonCloseDebugPointer(browser.get())
        << " host=" << FormatMuonCloseDebugPointer(host.get())
        << " browser_id=" << browser->GetIdentifier()
        << " valid=" << FormatMuonCloseDebugBool(valid)
        << " ready_before_try="
        << FormatMuonCloseDebugBool(ready_to_be_closed)
        << " try_close_result=" << FormatMuonCloseDebugBool(result)
        << " pending_fs_dialog="
        << FormatMuonCloseDebugBool(has_pending_fs_dialog)
        << " pending_fs_dialog_close_requested="
        << FormatMuonCloseDebugBool(pending_fs_dialog_close_requested)
        << " return=" << FormatMuonCloseDebugBool(result);
    AppendMuonCloseDebugLog(log.str());
    return result;
  }
  AppendMuonCloseDebugLog(
      "WindowDelegate CanClose return true reason=no_browser");
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

CefRect MuonWindowDelegate::GetInitialBounds(CefRefPtr<CefWindow> window) {
  CefRect work_area;
  if (!GetInitialWindowWorkArea(window, &work_area)) {
    initial_bounds_provided_ = false;
    return CefRect();
  }
  initial_bounds_provided_ = true;
  return GetMuonCenteredWindowBounds(work_area, GetPreferredSize(nullptr));
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

bool MuonWindowDelegate::OnKeyEvent(CefRefPtr<CefWindow> window,
                                    const CefKeyEvent& event) {
  (void)window;
  if (is_devtools_ || !shortcut_handler_ || !browser_view_ ||
      !browser_view_->IsValid() ||
      browser_view_->HasFocus()) {
    return false;
  }
  return shortcut_handler_->HandleBrowserShortcut(browser_view_->GetBrowser(),
                                                 event, nullptr);
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
