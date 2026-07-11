/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#pragma once

#include "plugins/muon_plugin_metadata.h"
#include "plugins/muon_shared_buffer.h"

#include "include/cef_process_message.h"
#include "include/cef_v8.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

/**
 * Internal global function name used by capability virtual modules.
 */
inline constexpr char kMuonV8CapabilityCallFunctionName[] =
    "__muon_plugin_call";

/**
 * Internal lifetime state attached to one JavaScript plugin function proxy.
 */
class MuonPluginFunctionProxyState;

/**
 * V8 handler for functions exposed under plugin namespace objects.
 */
class MuonV8Handler final : public CefV8Handler {
 public:
  /**
   * Creates a handler for one V8 context.
   *
   * @param functions Functions exposed under plugin namespace objects.
   * @param context V8 context that owns all promises handled by this object.
   */
  MuonV8Handler(std::vector<MuonFunctionMetadata> functions,
                 CefRefPtr<CefV8Context> context);

  /**
   * Handles JavaScript calls for functions exposed under plugin namespaces.
   *
   * @param name Function name called by JavaScript.
   * @param object JavaScript receiver object.
   * @param arguments Arguments passed by JavaScript.
   * @param retval Return value assigned to JavaScript.
   * @param exception Exception assigned when execution fails.
   * @return true when the function call was handled.
   */
  bool Execute(const CefString& name,
               CefRefPtr<CefV8Value> object,
               const CefV8ValueList& arguments,
               CefRefPtr<CefV8Value>& retval,
               CefString& exception) override;

  /**
   * Resolves or rejects a pending JavaScript Promise from browser IPC.
   *
   * @param message Result process message from the browser process.
   * @return true when the message was handled.
   */
  bool HandleResultMessage(CefRefPtr<CefProcessMessage> message);

  /**
   * Receives a shared-memory companion message for a plugin result.
   */
  bool HandleResultSharedMessage(CefRefPtr<CefProcessMessage> message);

  /**
   * Invokes a renderer-owned function for a plugin-owned native callback.
   */
  bool HandleRendererFunctionCallMessage(CefRefPtr<CefProcessMessage> message);

  /**
   * Receives a shared-memory companion message for a renderer function call.
   */
  bool HandleRendererFunctionCallSharedMessage(
      CefRefPtr<CefProcessMessage> message);

  /**
   * Records a browser source lease for a renderer-owned function.
   *
   * @param message Source lease acquisition message from the browser process.
   * @return true when the message was handled.
   */
  bool HandleRendererFunctionSourceAcquireMessage(
      CefRefPtr<CefProcessMessage> message);

  /**
   * Releases a browser source lease for a renderer-owned function.
   *
   * @param message Source lease release message from the browser process.
   * @return true when the message was handled.
   */
  bool HandleRendererFunctionSourceReleaseMessage(
      CefRefPtr<CefProcessMessage> message);

  /**
   * Completes the transfer of a renderer-owned function result.
   *
   * @param message Result consumption acknowledgment from the browser process.
   * @return true when the message was handled.
   */
  bool HandleRendererFunctionResultConsumedMessage(
      CefRefPtr<CefProcessMessage> message);

  /**
   * Rejects all pending promises before the owning V8 context is released.
   */
  void RejectAllPendingPromises();

  /**
   * Releases JavaScript function references and plugin proxy leases before
   * the owning V8 context is destroyed.
   */
  void ReleaseFunctionReferences();

  /**
   * Returns the renderer-local V8 context id.
   */
  int GetContextId() const;

  /**
   * Returns true when this handler owns the supplied V8 context.
   */
  bool IsForContext(CefRefPtr<CefV8Context> context) const;

 private:
  friend class MuonPluginFunctionProxyState;

  struct FunctionTransfers {
    explicit FunctionTransfers(MuonV8Handler* owner = nullptr);
    ~FunctionTransfers();
    FunctionTransfers(FunctionTransfers&& other) noexcept;
    FunctionTransfers& operator=(FunctionTransfers&& other) noexcept;
    FunctionTransfers(const FunctionTransfers&) = delete;
    FunctionTransfers& operator=(const FunctionTransfers&) = delete;

    void Add(int function_id);
    void Reset();
    bool Empty() const;

    MuonV8Handler* owner = nullptr;
    std::vector<int> function_ids;
  };

  struct PendingPromise {
    CefRefPtr<CefV8Value> promise;
    MuonTypeMetadata return_type = CreateMuonPrimitiveType(MUON_TYPE_VOID);
    FunctionTransfers function_transfers;
  };

  struct FunctionReference {
    int id = 0;
    CefRefPtr<CefV8Value> function;
    size_t pending_transfer_count = 0;
    std::set<std::string> source_lease_tokens;
  };

  struct PendingSharedPayload {
    std::shared_ptr<MuonSharedBufferPayload> payload;
    std::string error_message;
    bool has_error = false;
  };

  bool ValidateAndEncodeArguments(const std::string& function_name,
                                  const std::vector<MuonTypeMetadata>& arg_types,
                                  const CefV8ValueList& arguments,
                                  CefRefPtr<CefListValue> encoded_args,
                                  std::vector<MuonSharedBufferSource>* shared_sources,
                                  FunctionTransfers* function_transfers,
                                  std::string* error_message);
  bool ResolvePluginResultMessage(
      CefRefPtr<CefProcessMessage> message,
      std::shared_ptr<MuonSharedBufferPayload> shared_payload);
  bool EncodeFunctionArgument(const MuonTypeMetadata& expected_type,
                              CefRefPtr<CefV8Value> argument,
                              CefRefPtr<CefDictionaryValue> encoded_function,
                              FunctionTransfers* function_transfers,
                              std::string* error_message);
  bool ExecutePluginCall(const std::string& function_name,
                         const std::vector<MuonTypeMetadata>& arg_types,
                         const MuonTypeMetadata& return_type,
                         uint32_t function_id,
                         const std::string& proxy_lease_token,
                         bool is_capability_call,
                         const std::string& capability_id,
                         const std::string& capability_function_path,
                         const CefV8ValueList& call_arguments,
                         CefRefPtr<CefV8Value> promise);
  void RejectPromise(CefRefPtr<CefV8Value> promise,
                     const std::string& error_message) const;
  int AcquireFunctionTransfer(CefRefPtr<CefV8Value> function,
                              FunctionTransfers* function_transfers);
  void ReleaseFunctionTransfer(int function_id);
  void ReleaseFunctionReferenceIfUnused(int function_id);
  CefRefPtr<CefV8Value> CreateV8ValueFromResult(
      const MuonTypeMetadata& return_type,
      CefRefPtr<CefListValue> message_args,
      std::shared_ptr<MuonSharedBufferPayload> shared_payload);
  CefRefPtr<CefV8Value> CreateV8ValueFromEncodedValue(
      const MuonTypeMetadata& value_type,
      CefRefPtr<CefListValue> values,
      size_t index,
      std::shared_ptr<MuonSharedBufferPayload> shared_payload);
  CefRefPtr<CefV8Value> CreatePluginProxyFunction(
      uint32_t proxy_id,
      const std::string& lease_token,
      const MuonTypeMetadata& function_type);
  bool EnsurePluginProxyFactory(std::string* error_message);
  CefRefPtr<MuonPluginFunctionProxyState> FindPluginProxyState(
      CefRefPtr<CefV8Value> function,
      std::string* error_message) const;
  CefRefPtr<MuonPluginFunctionProxyState> FindPluginProxyStateFromMarker(
      CefRefPtr<CefV8Value> marker) const;
  void RegisterPluginProxyState(
      const CefBaseRefCounted* user_data,
      MuonPluginFunctionProxyState* state);
  void UnregisterPluginProxyState(
      const CefBaseRefCounted* user_data,
      MuonPluginFunctionProxyState* state);
  bool InvokeRendererFunctionCallMessage(
      CefRefPtr<CefProcessMessage> message,
      std::shared_ptr<MuonSharedBufferPayload> shared_payload);
  bool SendFunctionResult(int call_id,
                          const MuonTypeMetadata& return_type,
                          CefRefPtr<CefV8Value> value,
                          const CefString& exception);

  std::vector<MuonFunctionMetadata> functions_;
  std::map<std::string, size_t> function_indexes_by_v8_name_;
  std::map<std::string, size_t> function_indexes_by_public_path_;
  std::vector<FunctionReference> function_references_;
  std::map<const CefBaseRefCounted*, MuonPluginFunctionProxyState*>
      plugin_proxy_states_by_user_data_;
  std::map<int, PendingPromise> pending_promises_;
  std::map<int, CefRefPtr<CefProcessMessage>> pending_result_messages_;
  std::map<int, PendingSharedPayload> pending_result_payloads_;
  std::map<int, CefRefPtr<CefProcessMessage>>
      pending_renderer_function_call_messages_;
  std::map<int, PendingSharedPayload>
      pending_renderer_function_call_payloads_;
  std::map<int, FunctionTransfers>
      pending_renderer_function_result_transfers_;
  CefRefPtr<CefV8Context> context_;
  CefRefPtr<CefV8Value> plugin_proxy_dispatch_function_;
  CefRefPtr<CefV8Value> plugin_proxy_factory_;
  int context_id_ = 0;
  int next_call_id_ = 1;
  int next_function_id_ = 1;
  uint64_t next_proxy_wrapper_id_ = 1;

  IMPLEMENT_REFCOUNTING(MuonV8Handler);
  DISALLOW_COPY_AND_ASSIGN(MuonV8Handler);
};

/**
 * Creates the internal V8 function name used to dispatch one plugin function.
 */
std::string CreateMuonV8FunctionName(uint32_t function_id);
