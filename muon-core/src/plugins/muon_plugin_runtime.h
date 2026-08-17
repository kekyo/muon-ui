/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once

#include "browser/muon_builtin_browser.h"
#include "plugins/muon_plugin_metadata.h"
#include "plugins/muon_plugin_policy.h"
#include "rpc/muon_rpc.h"

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

  /** Counts across this host-process plugin runtime. */
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
   * Whether library_directory overrides the runtime-wide plugin directory.
   */
  bool has_library_directory = false;
  /**
   * Directory containing this framework-managed plugin library.
   */
  std::filesystem::path library_directory;
  /**
   * Whether expected_signature is configured for this external plugin.
   */
  bool has_expected_signature = false;
  /**
   * Expected lowercase SHA-256 signature for this external plugin library.
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
 * Platform services required by the CEF-independent plugin runtime.
 */
struct MuonPluginRuntimeServices {
  /** Returns whether the caller is on the serialized runtime owner thread. */
  std::function<bool()> is_owner_thread;

  /** Posts work to the serialized runtime owner thread. */
  std::function<bool(std::function<void()> task)> post_owner_task;

  /** Allocates transport-capable writable binary storage. */
  std::function<std::shared_ptr<MuonRpcBufferStorage>(
      size_t size,
      std::string* error_message)> allocate_buffer;

  /** Returns whether an RPC owner can currently receive host messages. */
  std::function<bool(const MuonRpcOwner& owner)> is_owner_available;

  /** Sends one typed host-to-renderer RPC message. */
  std::function<bool(const MuonRpcMessage& message,
                     std::string* error_message)> send_message;
};

/**
 * Host-process runtime that owns plugin libraries and invokes functions.
 */
class MuonPluginRuntime final {
 public:
  /**
   * Completion callback used after a native plugin call finishes.
   */
  using Completion = std::function<void(const MuonRpcCallResult& result)>;

  /**
   * Completion callback used after all loaded plugins finish stopping.
   */
  using StopCompletion = std::function<void()>;

  /**
   * Creates a plugin runtime and loads explicit plugins from plugin_directory.
   *
   * @param plugin_directory Directory containing plugin shared libraries.
   * @param plugins Explicit plugin load entries.
   * @param services Platform thread, transport, and buffer services.
   */
  MuonPluginRuntime(std::filesystem::path plugin_directory,
                    std::vector<MuonPluginRuntimeLoadEntry> plugins,
                    MuonPluginRuntimeServices services);

  /**
   * Stops pending plugin work and unloads plugin libraries.
   */
  ~MuonPluginRuntime();

  /**
   * Returns metadata for all JavaScript-visible functions.
   */
  const std::vector<MuonFunctionMetadata>& GetFunctions() const;

  /**
   * Returns metadata for all JavaScript-visible namespaces.
   */
  const std::vector<MuonNamespaceMetadata>& GetNamespaces() const;

  /**
   * Returns true when plugin startup validation succeeded.
   */
  bool IsReady() const;

  /**
   * Returns the plugin startup validation error, when one occurred.
   */
  std::string GetStartupError() const;

  /**
   * Stops loaded plugins asynchronously.
   *
   * Repeated calls are coalesced. Every supplied completion runs on the
   * runtime owner thread after all plugin stop callbacks have completed.
   *
   * @param completion Callback invoked after plugin shutdown completes.
   */
  void Stop(StopCompletion completion);

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
   * @param owner_browser_id Platform browser identifier for the opener window.
   */
  void CancelFsDialogsForOwner(int owner_browser_id);

  /**
   * Resolves the argument types required to decode one invocation.
   *
   * Proxy calls are accepted only when the owner and wrapper lease match.
   *
   * @param request Invocation routing metadata; arguments may be empty.
   * @param argument_types Receives the recursive argument types.
   * @param error_message Receives a validation diagnostic.
   * @return true when the target exists and is callable by the owner.
   */
  bool GetCallArgumentTypes(const MuonRpcCallRequest& request,
                            std::vector<MuonTypeMetadata>* argument_types,
                            std::string* error_message) const;

  /**
   * Invokes a fully decoded plugin or plugin-proxy request.
   *
   * @param request Typed invocation owned by one renderer context.
   * @param completion Completion callback on the runtime owner thread.
   */
  void Invoke(const MuonRpcCallRequest& request, Completion completion);

  /**
   * Resolves the return type for one pending renderer-owned function call.
   *
   * @param owner Renderer context that owns the pending source function.
   * @param call_id Runtime-wide renderer callback call identifier.
   * @param return_type Receives the recursive return type.
   * @return true when the pending call belongs to owner.
   */
  bool GetRendererFunctionReturnType(const MuonRpcOwner& owner,
                                     uint32_t call_id,
                                     MuonTypeMetadata* return_type) const;

  /**
   * Completes a renderer-owned function call initiated by a plugin pointer.
   *
   * @param result Typed renderer result received from the platform adapter.
   */
  void CompleteRendererFunctionCall(
      const MuonRpcRendererFunctionResult& result);

  /**
   * Releases one plugin function proxy wrapper lease.
   *
   * @param release Typed owner and wrapper lease to release.
   */
  void ReleasePluginFunctionProxy(
      const MuonRpcPluginProxyRelease& release);

  /**
   * Notifies loaded plugins and releases function sources owned by a renderer
   * V8 context.
   *
   * @param release Typed renderer context release notification.
   */
  void ReleaseFunctionContext(const MuonRpcContextReleased& release);

  /**
   * Releases function sources and proxy leases owned by one browser frame.
   *
   * @param browser_id Browser whose frame was destroyed.
   * @param frame_id Platform frame identifier.
   */
  void ReleaseFunctionFrame(int browser_id, const std::string& frame_id);

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
   * @param owner Browser, frame, and renderer context owner.
   * @return Current owner, global, and libffi closure counts.
   */
  MuonFunctionWrapperDiagnostics GetFunctionWrapperDiagnostics(
      const MuonRpcOwner& owner) const;
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
 * Creates the host-process plugin runtime.
 */
std::shared_ptr<MuonPluginRuntime> CreateMuonPluginRuntime(
    std::filesystem::path plugin_path,
    std::vector<MuonPluginRuntimeLoadEntry> plugins,
    MuonPluginRuntimeServices services);
