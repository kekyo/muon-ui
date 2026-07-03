/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#pragma once

#include "browser/muon_builtin_browser.h"
#include "plugins/muon_plugin_metadata.h"
#include "plugins/muon_plugin_policy.h"
#include "plugins/muon_shared_buffer.h"

#include "include/cef_frame.h"
#include "include/cef_process_message.h"
#include "include/cef_values.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct MuonPluginRuntimeImpl;

/**
 * Explicit plugin load entry prepared from muon.json.
 */
struct MuonPluginRuntimeLoadEntry {
  /**
   * Plugin file stem, or the reserved internal plugin name.
   */
  std::string plugin;
  /**
   * Whether expected_sha1 is configured for this external plugin.
   */
  bool has_expected_sha1 = false;
  /**
   * Expected lowercase SHA-1 digest for this external plugin library.
   */
  std::string expected_sha1;
  /**
   * Function allow policy for this plugin entry.
   */
  std::shared_ptr<MuonPluginPolicy> plugin_policy;
};

/**
 * Renderer context that initiated one plugin call.
 */
struct MuonPluginInvocationContext {
  int browser_id = 0;
  std::string frame_id;
  int renderer_context_id = 0;
  CefRefPtr<CefFrame> frame;
};

/**
 * Browser-process runtime that owns plugin libraries and invokes functions.
 */
class MuonPluginRuntime final {
 public:
  /**
   * Completion callback used after a native plugin call finishes.
   */
  using Completion = std::function<void(const MuonPluginCallResult& result)>;

  /**
   * Creates a plugin runtime and loads explicit plugins from plugin_directory.
   *
   * @param plugin_directory Directory containing plugin shared libraries.
   * @param plugins Explicit plugin load entries.
   */
  MuonPluginRuntime(std::filesystem::path plugin_directory,
                    std::vector<MuonPluginRuntimeLoadEntry> plugins);

  /**
   * Stops pending plugin work and unloads plugin libraries.
   */
  ~MuonPluginRuntime();

  /**
   * Returns metadata for all JavaScript-visible functions.
   */
  const std::vector<MuonFunctionMetadata>& GetFunctions() const;

  /**
   * Returns true when plugin startup validation succeeded.
   */
  bool IsReady() const;

  /**
   * Returns the plugin startup validation error, when one occurred.
   */
  std::string GetStartupError() const;

  /**
   * Creates the renderer startup metadata dictionary.
   */
  CefRefPtr<CefDictionaryValue> CreateRendererMetadata() const;

  /**
   * Returns the built-in browser operation for a function id.
   *
   * @param function_id Renderer-visible function id.
   * @return Browser operation kind, or None for non-browser functions.
   */
  MuonBuiltinBrowserFunctionKind GetBuiltinBrowserFunctionKind(
      uint32_t function_id) const;

  /**
   * Cancels modal filesystem dialogs owned by the given browser.
   *
   * @param owner_browser_id CEF browser identifier for the opener window.
   */
  void CancelFsDialogsForOwner(int owner_browser_id);

  /**
   * Invokes a plugin function from browser-process IPC payload.
   *
   * @param function_id Function id assigned by this runtime.
   * @param encoded_args CEF list containing encoded JavaScript arguments.
   * @param completion Completion callback that runs on the browser UI thread.
   */
  void Invoke(const MuonPluginInvocationContext& context,
              uint32_t function_id,
              int call_id,
              CefRefPtr<CefListValue> encoded_args,
              std::shared_ptr<MuonSharedBufferPayload> shared_payload,
              Completion completion);

  /**
   * Invokes a plugin-owned function proxy from renderer-process IPC.
   */
  void InvokeProxy(const MuonPluginInvocationContext& context,
                   uint32_t proxy_id,
                   int call_id,
                   CefRefPtr<CefListValue> encoded_args,
                   std::shared_ptr<MuonSharedBufferPayload> shared_payload,
                   Completion completion);

  /**
   * Completes a renderer-owned function call initiated by a plugin pointer.
   */
  void CompleteRendererFunctionCall(
      CefRefPtr<CefProcessMessage> message,
      std::shared_ptr<MuonSharedBufferPayload> shared_payload);

  /**
   * Creates a shared buffer process message for one plugin-owned buffer view.
   */
  bool CreateSharedBufferMessage(
      const std::string& message_name,
      int call_id,
      size_t value_index,
      const muon_buffer_view& buffer_view,
      MuonCreatedSharedBufferMessage* created_message,
      std::string* error_message);

  /**
   * Registers one plugin-owned function wrapper for the renderer context.
   */
  uint32_t RegisterPluginFunctionProxy(
      const MuonPluginInvocationContext& context,
      muon_native_function function,
      const MuonTypeMetadata& function_type);

  /**
   * Releases function sources owned by a renderer V8 context.
   *
   * @param context Renderer context that was released.
   * @param renderer_context_id Id assigned by the renderer process.
   */
  void ReleaseFunctionContext(const MuonPluginInvocationContext& context,
                              int renderer_context_id);

 private:
  std::unique_ptr<MuonPluginRuntimeImpl> impl_;
};

/**
 * Resolves a muon.json plugin.path value from the executable directory when it
 * is still relative.
 */
std::filesystem::path ResolveMuonPluginDirectory(
    const std::filesystem::path& plugin_path);

/**
 * Creates the browser-process plugin runtime.
 */
std::shared_ptr<MuonPluginRuntime> CreateMuonPluginRuntime(
    std::filesystem::path plugin_path,
    std::vector<MuonPluginRuntimeLoadEntry> plugins);
