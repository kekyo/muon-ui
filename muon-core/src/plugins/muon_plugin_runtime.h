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

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct MuonPluginRuntimeImpl;

#if defined(MUON_TEST_BUILD)
/** Test-build counts for one function wrapper lifecycle scope. */
struct MuonFunctionWrapperDiagnosticCounts {
  /** Live renderer-owned function sources. */
  size_t sources = 0;

  /** Active bridge borrows of renderer-owned function sources. */
  size_t borrows = 0;

  /** Distinct plugin-owned native function proxy entries. */
  size_t proxies = 0;

  /** Renderer wrapper leases for plugin-owned native function proxies. */
  size_t proxy_leases = 0;
};

/** Test-build snapshot of function wrapper lifecycle state. */
struct MuonFunctionWrapperDiagnostics {
  /** Counts for the requesting browser, frame, and V8 context. */
  MuonFunctionWrapperDiagnosticCounts owner;

  /** Counts across this browser-process plugin runtime. */
  MuonFunctionWrapperDiagnosticCounts global;

  /** Whether libffi closure tracking is compiled into this build. */
  bool ffi_closures_enabled = false;

  /** Total tracked libffi closure allocations. */
  uint64_t ffi_closure_alloc = 0;

  /** Total tracked libffi closure releases. */
  uint64_t ffi_closure_free = 0;

  /** Currently live tracked libffi closures. */
  uint64_t ffi_closure_live = 0;

  /** Highest tracked libffi closure live count. */
  uint64_t ffi_closure_high_water = 0;
};
#endif

/**
 * Browser registration for one renderer-owned plugin function proxy wrapper.
 */
struct MuonPluginFunctionProxyRegistration {
  /** Runtime-wide proxy entry identifier. */
  uint32_t proxy_id = 0;

  /** Unique decimal token for this renderer wrapper lease. */
  std::string lease_token;
};

/**
 * String key-value plugin configuration entry prepared from muon.json.
 */
struct MuonPluginRuntimeConfigEntry {
  /**
   * Plugin-defined configuration key.
   */
  std::string key;
  /**
   * Plugin-defined configuration value.
   */
  std::string value;
};

/**
 * Explicit plugin load entry prepared from muon.json.
 */
struct MuonPluginRuntimeLoadEntry {
  /**
   * Plugin file stem, or the reserved internal plugin name.
   */
  std::string plugin;
  /**
   * Whether expected_signature is configured for this external plugin.
   */
  bool has_expected_signature = false;
  /**
   * Expected lowercase SHA-1 signature for this external plugin library.
   */
  std::string expected_signature;
  /**
   * Whether signature_salt is configured for this external plugin.
   */
  bool has_signature_salt = false;
  /**
   * Bytes appended to the plugin library before signature comparison.
   */
  std::vector<uint8_t> signature_salt;
  /**
   * Function allow policy for this plugin entry.
   */
  std::shared_ptr<MuonPluginPolicy> plugin_policy;
  /**
   * Plugin-defined string key-value configuration entries.
   */
  std::vector<MuonPluginRuntimeConfigEntry> config;
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
   *
   * @param context Renderer context that owns the wrapper lease.
   * @param proxy_id Runtime proxy entry identifier.
   * @param lease_token Unique wrapper lease token.
   * @param call_id Renderer call identifier.
   * @param encoded_args CEF list containing encoded JavaScript arguments.
   * @param shared_payload Optional shared-buffer argument payload.
   * @param completion Completion callback that runs on the browser UI thread.
   */
  void InvokeProxy(const MuonPluginInvocationContext& context,
                   uint32_t proxy_id,
                   const std::string& lease_token,
                   int call_id,
                   CefRefPtr<CefListValue> encoded_args,
                   std::shared_ptr<MuonSharedBufferPayload> shared_payload,
                   Completion completion);

  /**
   * Completes a renderer-owned function call initiated by a plugin pointer.
   *
   * @param context Actual browser and frame that sent the result.
   * @param message Renderer result process message.
   * @param shared_payload Optional shared-buffer result payload.
   */
  void CompleteRendererFunctionCall(
      const MuonPluginInvocationContext& context,
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
   *
   * @param context Renderer context that will own the wrapper lease.
   * @param function Plugin-owned function pointer.
   * @param function_type Function signature metadata.
   * @param registration Receives the proxy id and unique wrapper lease token.
   * @param error_message Receives a diagnostic on failure.
   * @return true when one wrapper lease was registered.
   */
  bool RegisterPluginFunctionProxy(
      const MuonPluginInvocationContext& context,
      muon_native_function function,
      const MuonTypeMetadata& function_type,
      MuonPluginFunctionProxyRegistration* registration,
      std::string* error_message);

  /**
   * Releases one plugin function proxy wrapper lease.
   *
   * @param context Renderer context requesting the release.
   * @param proxy_id Runtime proxy entry identifier.
   * @param lease_token Unique wrapper lease token.
   */
  void ReleasePluginFunctionProxy(
      const MuonPluginInvocationContext& context,
      uint32_t proxy_id,
      const std::string& lease_token);

  /**
   * Releases function sources owned by a renderer V8 context.
   *
   * @param context Renderer context that was released.
   * @param renderer_context_id Id assigned by the renderer process.
   */
  void ReleaseFunctionContext(const MuonPluginInvocationContext& context,
                              int renderer_context_id);

  /**
   * Releases all function sources and proxy leases owned by one browser.
   *
   * @param browser_id Browser whose renderer process was closed or terminated.
   */
  void ReleaseFunctionBrowser(int browser_id);

#if defined(MUON_TEST_BUILD)
  /**
   * Returns test-only function wrapper lifecycle diagnostics.
   *
   * @param context Actual browser, frame, and renderer context owner.
   * @return Current owner, global, and libffi closure counts.
   */
  MuonFunctionWrapperDiagnostics GetFunctionWrapperDiagnostics(
      const MuonPluginInvocationContext& context) const;
#endif

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
