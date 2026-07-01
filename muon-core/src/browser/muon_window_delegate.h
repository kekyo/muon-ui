/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#pragma once

#include "browser/muon_browser_shortcut_handler.h"
#include "browser/muon_title_bar.h"
#include "config/muon_config.h"

#include "include/cef_browser.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_window.h"

#include <string>

/**
 * Handles browser-owned work that must finish before a window can close.
 */
class MuonWindowCloseHandler {
 public:
  /**
   * Cancels pending filesystem dialogs and retries closing the owner window.
   *
   * @param browser Browser that owns the pending dialog.
   * @param window Window to close after the dialog finishes.
   * @return true when a pending dialog close retry was scheduled.
   */
  virtual bool RequestCloseAfterPendingFsDialog(
      CefRefPtr<CefBrowser> browser,
      CefRefPtr<CefWindow> window) = 0;

  /**
   * Returns whether the browser owns a pending filesystem dialog call.
   *
   * @param browser_id Browser identifier to query.
   * @return true when at least one filesystem dialog is still pending.
   */
  virtual bool HasPendingFsDialogCallForBrowser(int browser_id) const = 0;

 protected:
  ~MuonWindowCloseHandler() = default;
};

/**
 * Top-level window delegate for browser and DevTools windows.
 */
class MuonWindowDelegate final : public CefWindowDelegate {
 public:
  /**
   * Creates a window delegate for a browser view.
   *
   * @param browser_view Browser view owned by the window.
   * @param is_devtools Whether the window is for DevTools.
   * @param initial_window_state Initial state requested for the window.
   * @param initial_title_bar_visibility Whether the custom title bar starts
   * visible.
   * @param title_bar_manifest Parsed title bar provider manifest.
   * @param title_bar_background_color Explicit title bar background color.
   * @param shortcut_handler Browser shortcut handler for window-level events.
   * @param close_handler Browser close handler for pending owner work.
   * @param linux_desktop_id Desktop identifier for Linux window metadata.
   */
  MuonWindowDelegate(CefRefPtr<CefBrowserView> browser_view,
                      bool is_devtools,
                      MuonBrowserInitialWindowState initial_window_state =
                          kMuonBrowserInitialWindowStateNormal,
                      bool initial_title_bar_visibility = true,
                      MuonTitleBarManifest title_bar_manifest =
                          CreateNativeMuonTitleBarManifest(),
                      MuonTitleBarBackgroundColor title_bar_background_color =
                          {},
                      CefRefPtr<MuonBrowserShortcutHandler> shortcut_handler =
                          nullptr,
                      MuonWindowCloseHandler* close_handler = nullptr,
                      std::string linux_desktop_id = "muon");

  /**
   * Attaches the browser view and applies the initial window state.
   *
   * @param window Newly created top-level window.
   */
  void OnWindowCreated(CefRefPtr<CefWindow> window) override;

  /**
   * Releases the browser view when the window is destroyed.
   *
   * @param window Destroyed top-level window.
   */
  void OnWindowDestroyed(CefRefPtr<CefWindow> window) override;

  /**
   * Propagates activation state to the custom title bar.
   *
   * @param window Window whose activation state changed.
   * @param active Whether the window is active.
   */
  void OnWindowActivationChanged(CefRefPtr<CefWindow> window,
                                 bool active) override;

  /**
   * Recomputes custom title bar state and draggable regions.
   *
   * @param window Window whose bounds changed.
   * @param new_bounds New bounds in DIP screen coordinates.
   */
  void OnWindowBoundsChanged(CefRefPtr<CefWindow> window,
                             const CefRect& new_bounds) override;

  /**
   * Allows closing after the browser host accepts the close request.
   *
   * @param window Window that may close.
   * @return true when the window can close.
   */
  bool CanClose(CefRefPtr<CefWindow> window) override;

  /**
   * Returns the default top-level window size.
   *
   * @param view View requesting a preferred size.
   * @return Preferred window size.
   */
  CefSize GetPreferredSize(CefRefPtr<CefView> view) override;

  /**
   * Returns initial bounds inside the display work area.
   *
   * @param window Window requesting its initial bounds.
   * @return Initial window bounds, or an empty rectangle to use CEF fallback.
   */
  CefRect GetInitialBounds(CefRefPtr<CefWindow> window) override;

  /**
   * Returns true when the window should use a frameless custom title bar.
   *
   * @param window Window being created.
   */
  bool IsFrameless(CefRefPtr<CefWindow> window) override;

  /**
   * Returns the native initial show state requested for the window.
   *
   * @param window Window requesting its initial show state.
   * @return CEF show state matching the configured initial window state.
   */
  cef_show_state_t GetInitialShowState(CefRefPtr<CefWindow> window) override;

  /**
   * Returns the runtime style for browser or DevTools windows.
   */
  cef_runtime_style_t GetWindowRuntimeStyle() override;

  /**
   * Handles keyboard events that did not reach the focused browser view.
   *
   * @param window Window that received the key event.
   * @param event CEF key event.
   * @return true when the event was handled.
   */
  bool OnKeyEvent(CefRefPtr<CefWindow> window,
                  const CefKeyEvent& event) override;

  /**
   * Sets Linux window metadata for desktop environments.
   *
   * @param window Window whose properties are requested.
   * @param properties Linux window properties to populate.
   * @return true when properties were populated.
   */
#if defined(OS_LINUX)
  bool GetLinuxWindowProperties(
      CefRefPtr<CefWindow> window,
      CefLinuxWindowProperties& properties) override;
#endif

 private:
  bool UseCustomTitleBar() const;

  CefRefPtr<CefBrowserView> browser_view_;
  CefRefPtr<CefBrowserView> title_bar_view_;
  CefRefPtr<MuonTitleBarController> title_bar_controller_;
  CefRefPtr<MuonBrowserShortcutHandler> shortcut_handler_;
  MuonWindowCloseHandler* close_handler_;
  const bool is_devtools_;
  const MuonBrowserInitialWindowState initial_window_state_;
  const bool initial_title_bar_visibility_;
  const MuonTitleBarManifest title_bar_manifest_;
  const MuonTitleBarBackgroundColor title_bar_background_color_;
  const std::string linux_desktop_id_;
  bool initial_bounds_provided_ = false;

  IMPLEMENT_REFCOUNTING(MuonWindowDelegate);
  DISALLOW_COPY_AND_ASSIGN(MuonWindowDelegate);
};
