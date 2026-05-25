/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#pragma once

#include "include/cef_app.h"
#include "include/cef_command_line.h"
#include "include/cef_render_process_handler.h"

#include "config/muon_config.h"
#include "network/muon_network_policy.h"
#include "plugins/muon_plugin_metadata.h"
#include "plugins/muon_v8_handler.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace cardio {
class dispatcher;
}

/**
 * CEF application entry point for browser and renderer process callbacks.
 */
class MuonApp final : public CefApp,
                       public CefBrowserProcessHandler,
                       public CefRenderProcessHandler {
 public:
  /**
   * Creates the application instance.
   */
  MuonApp(const MuonConfig& config,
          std::filesystem::path cef_log_path,
          std::vector<std::filesystem::path> config_paths,
          cardio::dispatcher* dispatcher);

  /**
   * Returns this object as the browser-process handler.
   */
  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override;

  /**
   * Returns this object as the renderer-process handler.
   */
  CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override;

  /**
   * Registers custom URL schemes in every CEF process.
   *
   * @param registrar CEF scheme registrar.
   */
  void OnRegisterCustomSchemes(
      CefRawPtr<CefSchemeRegistrar> registrar) override;

  /**
   * Applies logging command-line switches before CEF process startup.
   *
   * @param process_type CEF process type.
   * @param command_line Mutable process command line.
   */
  void OnBeforeCommandLineProcessing(
      const CefString& process_type,
      CefRefPtr<CefCommandLine> command_line) override;

  /**
   * Propagates Muon command-line config files to CEF child processes.
   *
   * @param command_line Mutable child process command line.
   */
  void OnBeforeChildProcessLaunch(
      CefRefPtr<CefCommandLine> command_line) override;

  /**
   * Returns the process exit code requested by startup validation.
   */
  int GetExitCode() const;

  /**
   * Requests process shutdown with the provided exit code.
   *
   * @param exit_code Process exit code to return from main.
   * @return true when the shutdown request was accepted.
   */
  bool RequestShutdown(int32_t exit_code);

  /**
   * Creates the initial browser window after CEF context initialization.
   */
  void OnContextInitialized() override;

  /**
   * Receives renderer startup metadata for a newly created browser.
   *
   * @param browser Browser created in the renderer process.
   * @param extra_info Browser metadata passed by the browser process.
   */
  void OnBrowserCreated(CefRefPtr<CefBrowser> browser,
                        CefRefPtr<CefDictionaryValue> extra_info) override;

  /**
   * Clears renderer startup state for a destroyed browser.
   *
   * @param browser Browser destroyed in the renderer process.
   */
  void OnBrowserDestroyed(CefRefPtr<CefBrowser> browser) override;

  /**
   * Registers plugin namespace JavaScript APIs for newly created V8 contexts.
   *
   * @param browser Browser that owns the frame.
   * @param frame Frame whose V8 context was created.
   * @param context Newly created V8 context.
   */
  void OnContextCreated(CefRefPtr<CefBrowser> browser,
                        CefRefPtr<CefFrame> frame,
                        CefRefPtr<CefV8Context> context) override;

  /**
   * Releases pending Promise state before a V8 context is destroyed.
   *
   * @param browser Browser that owns the frame.
   * @param frame Frame whose V8 context is released.
   * @param context V8 context being released.
   */
  void OnContextReleased(CefRefPtr<CefBrowser> browser,
                         CefRefPtr<CefFrame> frame,
                         CefRefPtr<CefV8Context> context) override;

  /**
   * Handles plugin result messages in the renderer process.
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

 private:
  MuonConfig config_;
  std::filesystem::path cef_log_path_;
  std::vector<std::filesystem::path> config_paths_;
  cardio::dispatcher* dispatcher_ = nullptr;
  std::shared_ptr<MuonNetworkPolicy> plugin_page_policy_;
  std::shared_ptr<MuonNetworkPolicy> unsafe_parent_access_policy_;
  std::string plugin_page_policy_error_;
  std::string unsafe_parent_access_policy_error_;
  MuonRendererMetadata renderer_metadata_;
  std::map<int, std::string> renderer_url_hints_by_browser_;
  std::map<int, CefRefPtr<MuonV8Handler>> v8_handlers_by_context_;
  int exit_code_ = 0;
  bool shutdown_requested_ = false;

  IMPLEMENT_REFCOUNTING(MuonApp);
  DISALLOW_COPY_AND_ASSIGN(MuonApp);
};
