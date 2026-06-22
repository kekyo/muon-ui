/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#pragma once

#include "app/muon_app_storage.h"

#include "include/cef_client.h"
#include "include/cef_image.h"
#include "include/cef_values.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_window.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/**
 * Title bar rendering mode selected by libmuon-ui.
 */
enum class MuonTitleBarMode {
  /**
   * Use the platform native title bar.
   */
  Native,

  /**
   * Use a libmuon-ui provided custom BrowserView title bar.
   */
  Custom,
};

/**
 * Parsed libmuon-ui title bar provider manifest.
 */
struct MuonTitleBarManifest {
  /**
   * Title bar rendering mode.
   */
  MuonTitleBarMode mode = MuonTitleBarMode::Native;

  /**
   * Custom title bar height in DIP.
   */
  int height = 0;

  /**
   * Right-side non-draggable control region width in DIP.
   */
  int controls_width = 0;

  /**
   * HTML fragment rendered inside the internal title bar document.
   */
  std::string html;

  /**
   * CSS stylesheet rendered inside the internal title bar document.
   */
  std::string css;

  /**
   * JavaScript executed inside the internal title bar document.
   */
  std::string js;
};

/**
 * Optional explicit title bar background color.
 */
struct MuonTitleBarBackgroundColor {
  /**
   * Whether the explicit color should override the title bar theme.
   */
  bool has_color = false;

  /**
   * Red component for explicit RGB mode.
   */
  uint8_t red = 0;

  /**
   * Green component for explicit RGB mode.
   */
  uint8_t green = 0;

  /**
   * Blue component for explicit RGB mode.
   */
  uint8_t blue = 0;
};

/**
 * Loaded title bar icon data for native and Muon title bars.
 */
struct MuonTitleBarIcon {
  /**
   * CEF image used by native title bars.
   *
   * @remarks Non-PNG icons used by the Muon custom title bar leave this empty.
   */
  CefRefPtr<CefImage> image;

  /**
   * Image data URL used by the Muon custom title bar.
   */
  std::string data_url;
};

/**
 * Creates a native title bar manifest used as the safe fallback.
 */
MuonTitleBarManifest CreateNativeMuonTitleBarManifest();

/**
 * Loads a title bar icon from PNG bytes.
 *
 * @param data PNG byte buffer.
 * @param size PNG byte buffer size.
 * @param source Diagnostic source label used in error messages.
 * @param icon Receives the loaded icon.
 * @param error_message Receives a validation diagnostic.
 * @return true when a PNG icon was loaded.
 */
bool LoadMuonTitleBarIconFromPngBytes(const uint8_t* data,
                                      size_t size,
                                      const std::string& source,
                                      MuonTitleBarIcon* icon,
                                      std::string* error_message);

/**
 * Loads a title bar icon from asset storage.
 *
 * @param storage Asset storage backing asset:// resources.
 * @param path Icon asset path. Accepts asset://main/... or main-relative paths.
 * @param icon Receives the loaded icon.
 * @param error_message Receives a validation or loading diagnostic.
 * @return true when a PNG icon was loaded.
 */
bool LoadMuonTitleBarIconFromStorage(std::shared_ptr<MuonAppStorage> storage,
                                     const std::string& path,
                                     MuonTitleBarIcon* icon,
                                     std::string* error_message);

/**
 * Loads a title bar icon from asset storage for the Muon custom title bar.
 *
 * @param storage Asset storage backing asset:// resources.
 * @param path Icon asset path. Accepts asset://main/... or main-relative paths.
 * @param icon Receives the loaded icon.
 * @param error_message Receives a validation or loading diagnostic.
 * @return true when an icon data URL was loaded.
 */
bool LoadMuonTitleBarIconDataUrlFromStorage(
    std::shared_ptr<MuonAppStorage> storage,
    const std::string& path,
    MuonTitleBarIcon* icon,
    std::string* error_message);

/**
 * Loads a title bar icon from fetched response bytes.
 *
 * @param data Image byte buffer.
 * @param size Image byte buffer size.
 * @param mime_type Response MIME type.
 * @param source Diagnostic source label used in error messages.
 * @param require_png Whether non-PNG images should be rejected.
 * @param icon Receives the loaded icon.
 * @param error_message Receives a validation diagnostic.
 * @return true when an icon was loaded.
 */
bool LoadMuonTitleBarIconFromImageBytes(const uint8_t* data,
                                        size_t size,
                                        const std::string& mime_type,
                                        const std::string& source,
                                        bool require_png,
                                        MuonTitleBarIcon* icon,
                                        std::string* error_message);

/**
 * Returns whether the manifest requests a custom title bar.
 */
bool IsCustomMuonTitleBar(const MuonTitleBarManifest& manifest);

/**
 * Returns whether Linux native window-manager title bar decoration is available.
 *
 * @remarks Non-Linux platforms return true. On Linux, only an explicit or
 * detected X11 backend is treated as native-decoration capable.
 */
bool IsMuonNativeTitleBarSupported(
    const std::vector<std::string>& command_line,
    const char* xdg_session_type,
    const char* wayland_display,
    const char* display);

/**
 * Creates renderer startup metadata marking an internal title bar browser.
 */
CefRefPtr<CefDictionaryValue> CreateMuonTitleBarExtraInfo();

/**
 * Returns whether renderer startup metadata belongs to an internal title bar.
 */
bool IsMuonTitleBarExtraInfo(CefRefPtr<CefDictionaryValue> extra_info);

/**
 * Parses a libmuon-ui title bar manifest JSON string.
 *
 * @remarks Invalid or unsupported manifests fall back to native mode.
 */
MuonTitleBarManifest ParseMuonTitleBarManifest(const char* manifest_json);

/**
 * Browser client and state controller for an internal custom title bar view.
 */
class MuonTitleBarController final : public CefClient,
                                     public CefLifeSpanHandler,
                                     public CefLoadHandler,
                                     public CefRequestHandler {
 public:
  /**
   * Creates a title bar controller from a parsed custom manifest.
   */
  explicit MuonTitleBarController(
      MuonTitleBarManifest manifest,
      MuonTitleBarBackgroundColor background_color = {});

  /**
   * Creates the title bar BrowserView.
   */
  CefRefPtr<CefBrowserView> CreateBrowserView();

  /**
   * Attaches this title bar to the native top-level window.
   */
  void AttachWindow(CefRefPtr<CefWindow> window);

  /**
   * Detaches this title bar from the native top-level window.
   */
  void DetachWindow();

  /**
   * Updates the displayed title text.
   */
  void SetTitle(const std::string& title);

  /**
   * Updates the displayed title bar icon data URL.
   */
  void SetIconDataUrl(const std::string& icon_data_url);

  /**
   * Updates the active/inactive visual state.
   */
  void SetActive(bool active);

  /**
   * Updates the maximized/restore visual state.
   */
  void SetMaximized(bool maximized);

  /**
   * Updates title bar visibility state used for draggable regions.
   */
  void SetVisible(bool visible);

  /**
   * Recomputes draggable regions from the current window size.
   */
  void UpdateDraggableRegions();

  /**
   * Returns the configured title bar height in DIP.
   */
  int GetHeight() const;

  /**
   * Returns the configured right-side controls width in DIP.
   */
  int GetControlsWidth() const;

  /**
   * Handles an action requested by the internal title bar document.
   */
  void HandleAction(const std::string& action);

  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override;
  CefRefPtr<CefLoadHandler> GetLoadHandler() override;
  CefRefPtr<CefRequestHandler> GetRequestHandler() override;

  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;
  void OnLoadEnd(CefRefPtr<CefBrowser> browser,
                 CefRefPtr<CefFrame> frame,
                 int http_status_code) override;
  bool OnBeforeBrowse(CefRefPtr<CefBrowser> browser,
                      CefRefPtr<CefFrame> frame,
                      CefRefPtr<CefRequest> request,
                      bool user_gesture,
                      bool is_redirect) override;

 private:
  void SendState();
  void SendTitle();
  void SendIcon();
  void ExecuteJavaScript(const std::string& source);

  const MuonTitleBarManifest manifest_;
  const MuonTitleBarBackgroundColor background_color_;
  CefWindow* window_ = nullptr;
  CefRefPtr<CefBrowser> browser_;
  std::string title_ = "Muon";
  std::string icon_data_url_;
  bool active_ = true;
  bool maximized_ = false;
  bool visible_ = true;
  bool loaded_ = false;

  IMPLEMENT_REFCOUNTING(MuonTitleBarController);
  DISALLOW_COPY_AND_ASSIGN(MuonTitleBarController);
};

/**
 * Registers a custom title bar controller for a window.
 */
void RegisterMuonTitleBarController(
    CefRefPtr<CefWindow> window,
    CefRefPtr<MuonTitleBarController> controller,
    int browser_id);

/**
 * Registers the BrowserView used to render a custom title bar.
 */
void RegisterMuonTitleBarView(CefRefPtr<CefWindow> window,
                              CefRefPtr<CefBrowserView> title_bar_view);

/**
 * Associates an internal title bar window with its app BrowserView.
 */
void RegisterMuonTitleBarBrowserView(CefRefPtr<CefWindow> window,
                                     CefRefPtr<CefBrowserView> browser_view);

/**
 * Associates an already registered custom title bar window with a browser id.
 */
void RegisterMuonTitleBarBrowser(CefRefPtr<CefWindow> window, int browser_id);

/**
 * Associates an already registered custom title bar BrowserView with a browser
 * id.
 */
void RegisterMuonTitleBarBrowserViewBrowser(
    CefRefPtr<CefBrowserView> browser_view,
    int browser_id);

/**
 * Removes a custom title bar controller registration for a window.
 */
void UnregisterMuonTitleBarController(CefRefPtr<CefWindow> window);

/**
 * Clears all custom title bar registrations before CEF shutdown.
 */
void ClearMuonTitleBarRegistrations();

/**
 * Updates the registered custom title bar, if any.
 */
void SetRegisteredMuonTitleBarTitle(CefRefPtr<CefWindow> window,
                                    const std::string& title);

/**
 * Updates the registered custom title bar for a browser, if any.
 */
void SetRegisteredMuonTitleBarTitleForBrowser(int browser_id,
                                              const std::string& title);

/**
 * Updates the registered custom or native title bar icon, if any.
 */
void SetRegisteredMuonTitleBarIcon(CefRefPtr<CefWindow> window,
                                   CefRefPtr<CefImage> image,
                                   const std::string& icon_data_url);

/**
 * Updates the registered custom or native title bar icon for a browser, if any.
 */
void SetRegisteredMuonTitleBarIconForBrowser(
    int browser_id,
    CefRefPtr<CefImage> image,
    const std::string& icon_data_url);

/**
 * Updates the custom title bar view or native title bar visibility hint.
 *
 * @param window Window whose title bar visibility should change.
 * @param visible Whether the title bar should be visible.
 */
void SetRegisteredMuonTitleBarVisibility(CefRefPtr<CefWindow> window,
                                         bool visible);

/**
 * Updates the custom title bar view or native title bar visibility hint for a
 * browser.
 *
 * @param browser_id Browser identifier whose window should be updated.
 * @param visible Whether the title bar should be visible.
 */
void SetRegisteredMuonTitleBarVisibilityForBrowser(int browser_id,
                                                   bool visible);

/**
 * Returns the registered custom-titlebar window for a browser, if any.
 */
CefRefPtr<CefWindow> GetRegisteredMuonWindowForBrowser(int browser_id);
