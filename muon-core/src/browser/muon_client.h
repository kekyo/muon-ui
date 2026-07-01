/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#pragma once

#include "app/muon_app_storage.h"
#include "browser/muon_browser_shortcut_handler.h"
#include "browser/muon_builtin_browser.h"
#include "browser/muon_title_bar.h"
#include "browser/muon_window_delegate.h"
#include "config/muon_config.h"
#include "network/muon_network_policy.h"
#include "plugins/muon_plugin_runtime.h"
#include "plugins/muon_shared_buffer.h"

#include "include/cef_client.h"
#include "include/cef_dialog_handler.h"
#include "include/cef_display_handler.h"
#include "include/cef_drag_handler.h"
#include "include/cef_request_handler.h"
#include "include/cef_urlrequest.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_window.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

/**
 * Main browser client handling browser callbacks.
 */
class MuonClient final : public CefClient,
                          public MuonBrowserShortcutHandler,
                          public CefLifeSpanHandler,
                          public CefDisplayHandler,
                          public CefContextMenuHandler,
                          public CefCommandHandler,
                          public CefKeyboardHandler,
                          public CefDialogHandler,
                          public CefDragHandler,
                          public CefRequestHandler,
                          public MuonWindowCloseHandler {
 public:
  /**
   * Creates the browser client.
   *
   * @param plugin_runtime Browser-process plugin runtime.
   * @param network_policy Browser network access policy.
   * @param plugin_page_policy Page URL policy for plugin API calls.
   * @param unsafe_parent_access_policy Popup URL policy for JavaScript parent
   * access.
   * @param shutdown_requester Callback that records a process shutdown request.
   * @param app_storage Asset storage used for browser UI assets.
   * @param browser_config Browser keyboard shortcut configuration.
   * @param title_bar_manifest Parsed title bar provider manifest.
   * @param title_bar_background_color Explicit title bar background color.
   * @param has_initial_title_bar_icon Whether initial_title_bar_icon is valid.
   * @param initial_title_bar_icon Initial title bar icon data.
   * @param linux_desktop_id Desktop identifier for Linux window metadata.
   */
  MuonClient(std::shared_ptr<MuonPluginRuntime> plugin_runtime,
             std::shared_ptr<MuonNetworkPolicy> network_policy,
             std::shared_ptr<MuonNetworkPolicy> plugin_page_policy,
             std::shared_ptr<MuonNetworkPolicy> unsafe_parent_access_policy,
             std::function<bool(int32_t)> shutdown_requester,
             std::shared_ptr<MuonAppStorage> app_storage,
             const MuonBrowserConfig& browser_config,
             MuonTitleBarManifest title_bar_manifest =
                 CreateNativeMuonTitleBarManifest(),
             MuonTitleBarBackgroundColor title_bar_background_color = {},
             bool has_initial_title_bar_icon = false,
             MuonTitleBarIcon initial_title_bar_icon = {},
             std::string linux_desktop_id = "muon");

  /**
   * Returns this object as the browser lifetime handler.
   */
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override;

  /**
   * Returns this object as the display handler.
   */
  CefRefPtr<CefDisplayHandler> GetDisplayHandler() override;

  /**
   * Returns this object as the context menu handler.
   */
  CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override;

  /**
   * Returns this object as the Chrome command handler.
   */
  CefRefPtr<CefCommandHandler> GetCommandHandler() override;

  /**
   * Returns this object as the keyboard event handler.
   */
  CefRefPtr<CefKeyboardHandler> GetKeyboardHandler() override;

  /**
   * Returns this object as the dialog handler.
   */
  CefRefPtr<CefDialogHandler> GetDialogHandler() override;

  /**
   * Returns this object as the drag event handler.
   */
  CefRefPtr<CefDragHandler> GetDragHandler() override;

  /**
   * Returns this object as the request handler.
   */
  CefRefPtr<CefRequestHandler> GetRequestHandler() override;

  /**
   * Returns the resource request handler that applies the network policy.
   *
   * @param browser Browser that originated the request.
   * @param frame Frame that originated the request.
   * @param request Browser request.
   * @param is_navigation Whether the request is a navigation.
   * @param is_download Whether the request is a download.
   * @param request_initiator Origin that initiated the request.
   * @param disable_default_handling Output flag for default handling.
   * @return Resource request handler for normal browser requests.
   */
  CefRefPtr<CefResourceRequestHandler> GetResourceRequestHandler(
      CefRefPtr<CefBrowser> browser,
      CefRefPtr<CefFrame> frame,
      CefRefPtr<CefRequest> request,
      bool is_navigation,
      bool is_download,
      const CefString& request_initiator,
      bool& disable_default_handling) override;

  /**
   * Clears page draggable regions before main frame navigation.
   *
   * @param browser Browser that is navigating.
   * @param frame Frame that is navigating.
   * @param request Navigation request.
   * @param user_gesture Whether navigation was initiated by user gesture.
   * @param is_redirect Whether navigation is a redirect.
   * @return false to allow navigation.
   */
  bool OnBeforeBrowse(CefRefPtr<CefBrowser> browser,
                      CefRefPtr<CefFrame> frame,
                      CefRefPtr<CefRequest> request,
                      bool user_gesture,
                      bool is_redirect) override;

  /**
   * Handles popup browser creation requests from window.open and target links.
   *
   * @param browser Browser that originated the popup.
   * @param frame Frame that originated the popup.
   * @param popup_id Popup identifier scoped to the opener browser.
   * @param target_url Initial popup navigation URL.
   * @param target_frame_name Requested target frame name.
   * @param target_disposition Requested popup disposition.
   * @param user_gesture Whether creation was initiated by a user gesture.
   * @param popupFeatures Requested popup window features.
   * @param windowInfo Mutable native window information.
   * @param client Mutable popup browser client.
   * @param settings Mutable popup browser settings.
   * @param extra_info Mutable renderer startup metadata.
   * @param no_javascript_access Output flag for opener script access.
   * @return true when popup creation is cancelled.
   */
  bool OnBeforePopup(CefRefPtr<CefBrowser> browser,
                     CefRefPtr<CefFrame> frame,
                     int popup_id,
                     const CefString& target_url,
                     const CefString& target_frame_name,
                     CefLifeSpanHandler::WindowOpenDisposition
                         target_disposition,
                     bool user_gesture,
                     const CefPopupFeatures& popupFeatures,
                     CefWindowInfo& windowInfo,
                     CefRefPtr<CefClient>& client,
                     CefBrowserSettings& settings,
                     CefRefPtr<CefDictionaryValue>& extra_info,
                     bool* no_javascript_access) override;

  /**
   * Tracks a newly created normal browser window.
   *
   * @param browser Newly created browser.
   */
  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;

  /**
   * Quits the message loop when all tracked normal browsers are closed.
   *
   * @param browser Browser that is about to finish closing.
   */
  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;

  /**
   * Restores the initial title bar icon when the main-frame address changes.
   *
   * @param browser Browser whose address changed.
   * @param frame Frame whose address changed.
   * @param url New frame URL.
   */
  void OnAddressChange(CefRefPtr<CefBrowser> browser,
                       CefRefPtr<CefFrame> frame,
                       const CefString& url) override;

  /**
   * Updates the native window title from the page title.
   *
   * @param browser Browser whose title changed.
   * @param title Page title reported by CEF.
   */
  void OnTitleChange(CefRefPtr<CefBrowser> browser,
                     const CefString& title) override;

  /**
   * Starts a title bar icon update from page favicon candidates.
   *
   * @param browser Browser whose favicon candidates changed.
   * @param icon_urls Favicon candidate URLs reported by CEF.
   */
  void OnFaviconURLChange(CefRefPtr<CefBrowser> browser,
                          const std::vector<CefString>& icon_urls) override;

  /**
   * Logs JavaScript console output through the unified Muon logger.
   *
   * @param browser Browser that received console output.
   * @param level CEF console severity.
   * @param message Console message text.
   * @param source Console source URL.
   * @param line Source line number.
   * @return true to suppress CEF's default console output.
   */
  bool OnConsoleMessage(CefRefPtr<CefBrowser> browser,
                        cef_log_severity_t level,
                        const CefString& message,
                        const CefString& source,
                        int line) override;

  /**
   * Removes Chromium default context menu items.
   *
   * @param browser Browser that received the context menu request.
   * @param frame Frame that received the context menu request.
   * @param params Context menu parameters from CEF.
   * @param model Context menu model to modify.
   */
  void OnBeforeContextMenu(CefRefPtr<CefBrowser> browser,
                           CefRefPtr<CefFrame> frame,
                           CefRefPtr<CefContextMenuParams> params,
                           CefRefPtr<CefMenuModel> model) override;

  /**
   * Blocks Chromium default browser commands controlled by muon.json.
   *
   * @param browser Browser that received the command.
   * @param command_id Chrome command identifier.
   * @param disposition Intended command target.
   * @return true when the command was handled.
   */
  bool OnChromeCommand(CefRefPtr<CefBrowser> browser,
                       int command_id,
                       cef_window_open_disposition_t disposition) override;

  /**
   * Handles configured browser keyboard shortcuts.
   *
   * @param browser Browser that received the key event.
   * @param event CEF key event.
   * @param os_event Platform event handle from CEF.
   * @param is_keyboard_shortcut Output flag set when the event is handled.
   * @return true when the event was handled.
   */
  bool OnPreKeyEvent(CefRefPtr<CefBrowser> browser,
                     const CefKeyEvent& event,
                     CefEventHandle os_event,
                     bool* is_keyboard_shortcut) override;

  /**
   * Handles browser shortcuts for keyboard events delivered outside the
   * browser view focus path.
   *
   * @param browser Browser associated with the active window.
   * @param event CEF key event.
   * @param is_keyboard_shortcut Output flag set when the event is handled.
   * @return true when the event was handled.
   */
  bool HandleBrowserShortcut(CefRefPtr<CefBrowser> browser,
                             const CefKeyEvent& event,
                             bool* is_keyboard_shortcut) override;

  /**
   * Runs CEF file input dialogs through the Muon UI dialog provider.
   *
   * @return true when the Muon UI provider accepted the dialog request.
   */
  bool OnFileDialog(CefRefPtr<CefBrowser> browser,
                    CefDialogHandler::FileDialogMode mode,
                    const CefString& title,
                    const CefString& default_file_path,
                    const std::vector<CefString>& accept_filters,
                    const std::vector<CefString>& accept_extensions,
                    const std::vector<CefString>& accept_descriptions,
                    CefRefPtr<CefFileDialogCallback> callback) override;

  /**
   * Applies page CSS app-region rectangles to the host window.
   *
   * @param browser Browser whose page regions changed.
   * @param frame Frame whose document produced the regions.
   * @param regions Draggable regions reported by CEF.
   */
  void OnDraggableRegionsChanged(
      CefRefPtr<CefBrowser> browser,
      CefRefPtr<CefFrame> frame,
      const std::vector<CefDraggableRegion>& regions) override;

  /**
   * Handles messages sent from the renderer process.
   *
   * @param browser Browser that received the message.
   * @param frame Frame that received the message.
   * @param source_process Process that sent the message.
   * @param message Message sent from the source process.
   * @return true when the message was handled.
   */
  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefFrame> frame,
                                CefProcessId source_process,
                                CefRefPtr<CefProcessMessage> message) override;

  /**
   * Cancels pending filesystem dialogs and retries closing the owner window.
   *
   * @param browser Browser that owns the pending dialog.
   * @param window Window to close after the dialog finishes.
   * @return true when a pending dialog close retry was scheduled.
   */
  bool RequestCloseAfterPendingFsDialog(CefRefPtr<CefBrowser> browser,
                                        CefRefPtr<CefWindow> window) override;

  /**
   * Returns whether the browser owns a pending filesystem dialog call.
   *
   * @param browser_id Browser identifier to query.
   * @return true when at least one filesystem dialog is still pending.
   */
  bool HasPendingFsDialogCallForBrowser(int browser_id) const override;

 private:
  struct PendingSharedPayload {
    std::shared_ptr<MuonSharedBufferPayload> payload;
    std::string error_message;
    bool has_error = false;
  };

  struct PendingPluginCall {
    MuonPluginInvocationContext context;
    CefRefPtr<CefBrowser> browser;
    CefRefPtr<CefFrame> frame;
    CefRefPtr<CefListValue> encoded_args;
    int call_id = 0;
    uint32_t function_id = 0;
    bool proxy_call = false;
  };

  struct ModalBrowserViewDisableState {
    CefRefPtr<CefBrowserView> browser_view;
    CefRefPtr<CefOverlayController> overlay_controller;
    bool restore_browser_view_enabled = false;
    int depth = 0;
  };

  static bool IsKnownBrowserShortcut(const CefKeyEvent& event);
  static bool GetBrowserViewAndWindow(CefRefPtr<CefBrowser> browser,
                                      CefRefPtr<CefBrowserView>* browser_view,
                                      CefRefPtr<CefWindow>* window,
                                      std::string* error_message);
  static bool ReadFsDialogModal(CefRefPtr<CefListValue> encoded_args);
  static bool CreateFsDialogArgsWithOwnerBrowserId(
      CefRefPtr<CefListValue> encoded_args,
      int browser_id,
      CefWindowHandle owner_window_handle,
      CefRefPtr<CefListValue>* target,
      std::string* error_message);
  static std::string CreatePendingSharedKey(const std::string& message_name,
                                            int renderer_context_id,
                                            int call_id);
  static bool IsPluginPageAllowed(
      CefRefPtr<CefFrame> frame,
      const std::shared_ptr<MuonNetworkPolicy>& plugin_page_policy);
  static bool IsPopupTargetUrlKnown(const std::string& url);
  static void SetFullscreen(CefRefPtr<CefBrowser> browser, bool fullscreen);
  static void ToggleFullscreen(CefRefPtr<CefBrowser> browser);
  static void ZoomBrowser(CefRefPtr<CefBrowser> browser,
                          cef_zoom_command_t command);
  bool IsFsDialogFunction(uint32_t function_id) const;
  bool BeginModalBrowserViewDisable(const PendingPluginCall& call,
                                    int* browser_id,
                                    CefWindowHandle* owner_window_handle,
                                    std::string* error_message);
  void EndModalBrowserViewDisable(int browser_id);
  void ClearModalBrowserViewDisable(int browser_id);
  void BeginPendingFsDialogCall(int browser_id);
  void EndPendingFsDialogCall(int browser_id);
  void RequestMessageLoopQuit(bool post_task);
  void QuitMessageLoopWhenIdle();
  bool PrepareShutdown(int32_t exit_code,
                       std::vector<CefRefPtr<CefBrowser>>* browsers,
                       bool* should_start_shutdown,
                       std::string* error_message);
  void DispatchBuiltinBrowserCall(MuonBuiltinBrowserFunctionKind kind,
                                  const PendingPluginCall& call);
  uint64_t BeginTitleBarIconUpdateForBrowser(int browser_id);
  bool IsCurrentTitleBarIconUpdate(int browser_id, uint64_t generation) const;
  void RestoreInitialTitleBarIconForBrowser(CefRefPtr<CefBrowser> browser,
                                            uint64_t generation);
  void StartFaviconTitleBarIconUpdate(CefRefPtr<CefBrowser> browser,
                                      std::vector<std::string> icon_urls);
  void ContinueFaviconTitleBarIconUpdate(CefRefPtr<CefBrowser> browser,
                                         std::vector<std::string> icon_urls,
                                         size_t icon_url_index,
                                         uint64_t generation);
  void CompleteFaviconTitleBarIconUpdate(CefRefPtr<CefBrowser> browser,
                                         std::vector<std::string> icon_urls,
                                         size_t icon_url_index,
                                         uint64_t generation,
                                         const std::string& mime_type,
                                         std::vector<uint8_t> data);
  bool SetTitleBarIconForBrowser(CefRefPtr<CefBrowser> browser,
                                 const MuonTitleBarIcon* icon,
                                 std::string* error_message);
  void DispatchPluginCall(const PendingPluginCall& call,
                          std::shared_ptr<MuonSharedBufferPayload> payload);
  void RejectPluginCall(const PendingPluginCall& call,
                        const std::string& error_message);
  void SendPluginResult(const MuonPluginInvocationContext& context,
                        CefRefPtr<CefFrame> frame,
                        int call_id,
                        const MuonPluginCallResult& result);

  bool shutdown_started_ = false;
  MuonBrowserConfig browser_config_;
  MuonTitleBarManifest title_bar_manifest_;
  MuonTitleBarBackgroundColor title_bar_background_color_;
  std::string linux_desktop_id_;
  bool has_initial_title_bar_icon_ = false;
  MuonTitleBarIcon initial_title_bar_icon_;
  std::function<bool(int32_t)> shutdown_requester_;
  std::shared_ptr<MuonAppStorage> app_storage_;
  std::shared_ptr<MuonPluginRuntime> plugin_runtime_;
  std::shared_ptr<MuonNetworkPolicy> network_policy_;
  std::shared_ptr<MuonNetworkPolicy> plugin_page_policy_;
  std::shared_ptr<MuonNetworkPolicy> unsafe_parent_access_policy_;
  std::map<int, CefRefPtr<CefBrowser>> browsers_by_id_;
  int pending_fs_dialog_calls_ = 0;
  std::map<int, int> pending_fs_dialog_calls_by_browser_;
  std::map<int, CefRefPtr<CefWindow>> close_windows_after_pending_fs_dialogs_;
  bool quit_message_loop_after_pending_fs_dialogs_ = false;
  int quit_message_loop_after_pending_fs_dialogs_browser_id_ = 0;
  bool message_loop_quit_requested_ = false;
  std::map<std::string, PendingPluginCall> pending_plugin_calls_;
  std::map<std::string, PendingSharedPayload> pending_plugin_call_payloads_;
  std::map<int, CefRefPtr<CefProcessMessage>>
      pending_renderer_function_result_messages_;
  std::map<int, PendingSharedPayload>
      pending_renderer_function_result_payloads_;
  std::map<int, ModalBrowserViewDisableState>
      modal_browser_view_disable_states_;
  std::map<int, uint64_t> title_bar_icon_update_generations_;
  std::map<int, CefRefPtr<CefURLRequest>> pending_favicon_requests_;

  IMPLEMENT_REFCOUNTING(MuonClient);
  DISALLOW_COPY_AND_ASSIGN(MuonClient);
};
