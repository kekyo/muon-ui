/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once

#include "plugins/muon_plugin_runtime.h"
#include "plugins/muon_shared_buffer.h"

#include "include/cef_frame.h"
#include "include/cef_process_message.h"
#include "include/cef_values.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

struct MuonCefRpcBridgeImpl;

/**
 * CEF wire codec and transport adapter for the platform-independent RPC model.
 */
class MuonCefRpcBridge final
    : public std::enable_shared_from_this<MuonCefRpcBridge> {
 public:
  /** Resolves a currently valid CEF frame for one RPC owner. */
  using FrameResolver =
      std::function<CefRefPtr<CefFrame>(const MuonRpcOwner& owner)>;

  /** Releases CEF transport state. */
  ~MuonCefRpcBridge();

  /**
   * Creates services used by the CEF-independent plugin runtime.
   *
   * @return Thread, binary allocation, and renderer transport services.
   */
  MuonPluginRuntimeServices CreateRuntimeServices();

  /**
   * Installs the browser-client frame resolver used by outgoing messages.
   *
   * @param resolver Resolver valid until DetachFrameResolver is called.
   */
  void AttachFrameResolver(FrameResolver resolver);

  /** Removes the browser-client frame resolver. */
  void DetachFrameResolver();

  /**
   * Decodes one CEF argument list into platform-independent RPC values.
   *
   * @param owner Renderer context that owns function references.
   * @param expected_types Declared recursive argument types.
   * @param encoded_values CEF argument values.
   * @param shared_payload Optional shared-memory companion payload.
   * @param values Receives decoded values.
   * @param error_message Receives a validation diagnostic.
   * @return true when every value is valid for its declared type.
   */
  bool DecodeArguments(
      const MuonRpcOwner& owner,
      const std::vector<MuonTypeMetadata>& expected_types,
      CefRefPtr<CefListValue> encoded_values,
      std::shared_ptr<MuonSharedBufferPayload> shared_payload,
      std::vector<MuonRpcValue>* values,
      std::string* error_message) const;

  /**
   * Decodes a renderer function result process message.
   *
   * @param owner Renderer context that sent the result.
   * @param expected_type Declared renderer function return type.
   * @param message CEF result metadata message.
   * @param shared_payload Optional shared-memory companion payload.
   * @param result Receives the typed result.
   * @param error_message Receives a malformed-wire diagnostic.
   * @return true when the process message is structurally valid.
   */
  bool DecodeRendererFunctionResult(
      const MuonRpcOwner& owner,
      const MuonTypeMetadata& expected_type,
      CefRefPtr<CefProcessMessage> message,
      std::shared_ptr<MuonSharedBufferPayload> shared_payload,
      MuonRpcRendererFunctionResult* result,
      std::string* error_message) const;

  /**
   * Encodes and sends one supported typed RPC message to its owner frame.
   *
   * @param message Typed host-to-renderer message.
   * @param error_message Receives an encoding or transport diagnostic.
   * @return true when every required CEF message was sent.
   */
  bool SendMessage(const MuonRpcMessage& message,
                   std::string* error_message);

 private:
  explicit MuonCefRpcBridge(std::unique_ptr<MuonCefRpcBridgeImpl> impl);

  std::unique_ptr<MuonCefRpcBridgeImpl> impl_;

  friend std::shared_ptr<MuonCefRpcBridge> CreateMuonCefRpcBridge();
};

/** Creates the CEF RPC codec and transport bridge. */
std::shared_ptr<MuonCefRpcBridge> CreateMuonCefRpcBridge();
