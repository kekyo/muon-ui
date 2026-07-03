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
#include <string>
#include <vector>

/**
 * Internal global function name used by capability virtual modules.
 */
inline constexpr char kMuonV8CapabilityCallFunctionName[] =
    "__muon_plugin_call";

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
   * Rejects all pending promises before the owning V8 context is released.
   */
  void RejectAllPendingPromises();

  /**
   * Releases strong references to JavaScript function arguments.
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
  struct PendingPromise {
    CefRefPtr<CefV8Value> promise;
    MuonTypeMetadata return_type = CreateMuonPrimitiveType(MUON_TYPE_VOID);
  };

  struct FunctionReference {
    int id = 0;
    CefRefPtr<CefV8Value> function;
    bool plugin_proxy = false;
    uint32_t proxy_id = 0;
    MuonTypeMetadata function_type = CreateMuonPrimitiveType(
        MUON_TYPE_VOID);
    std::string proxy_name;
  };

  struct ProxyFunction {
    uint32_t proxy_id = 0;
    MuonTypeMetadata function_type;
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
                                  std::string* error_message);
  bool ResolvePluginResultMessage(
      CefRefPtr<CefProcessMessage> message,
      std::shared_ptr<MuonSharedBufferPayload> shared_payload);
  bool EncodeFunctionArgument(const MuonTypeMetadata& expected_type,
                              CefRefPtr<CefV8Value> argument,
                              CefRefPtr<CefDictionaryValue> encoded_function);
  void RejectPromise(CefRefPtr<CefV8Value> promise,
                     const std::string& error_message) const;
  int GetOrCreateFunctionId(CefRefPtr<CefV8Value> function);
  CefRefPtr<CefV8Value> CreateV8ValueFromResult(
      const MuonTypeMetadata& return_type,
      CefRefPtr<CefListValue> message_args,
      std::shared_ptr<MuonSharedBufferPayload> shared_payload);
  CefRefPtr<CefV8Value> CreateV8ValueFromEncodedValue(
      const MuonTypeMetadata& value_type,
      CefRefPtr<CefListValue> values,
      size_t index,
      std::shared_ptr<MuonSharedBufferPayload> shared_payload);
  CefRefPtr<CefV8Value> GetOrCreatePluginProxyFunction(
      uint32_t proxy_id,
      const MuonTypeMetadata& function_type);
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
  std::map<std::string, ProxyFunction> proxy_functions_by_name_;
  std::map<int, PendingPromise> pending_promises_;
  std::map<int, CefRefPtr<CefProcessMessage>> pending_result_messages_;
  std::map<int, PendingSharedPayload> pending_result_payloads_;
  std::map<int, CefRefPtr<CefProcessMessage>>
      pending_renderer_function_call_messages_;
  std::map<int, PendingSharedPayload>
      pending_renderer_function_call_payloads_;
  std::vector<FunctionReference> function_references_;
  CefRefPtr<CefV8Context> context_;
  int context_id_ = 0;
  int next_call_id_ = 1;
  int next_function_id_ = 1;

  IMPLEMENT_REFCOUNTING(MuonV8Handler);
  DISALLOW_COPY_AND_ASSIGN(MuonV8Handler);
};

/**
 * Creates the internal V8 function name used to dispatch one plugin function.
 */
std::string CreateMuonV8FunctionName(uint32_t function_id);
