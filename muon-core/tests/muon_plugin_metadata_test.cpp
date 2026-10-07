/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "plugins/muon_plugin_metadata.h"
#include "plugins/muon_cef_plugin_metadata.h"
#include "plugins/muon_cef_rpc.h"
#include "plugins/muon_js_bridge.h"

#include "include/cef_app.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

static bool Expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << "\n";
    return false;
  }
  return true;
}

static bool RunNamespaceSetupScriptRoundtripTest() {
  const auto setup_script =
      std::string("Object.defineProperty(namespace, 'answer', { value: 42 });");
  const auto namespaces = std::vector<MuonNamespaceMetadata>{
      {"muon.test.setup", setup_script, {"publicRaw"}},
  };

  MuonFunctionMetadata function;
  function.id = 42;
  function.plugin_namespace = "muon.test.setup";
  function.js_name = "__raw";
  function.public_name = "publicRaw";
  function.arg_types.push_back(CreateMuonPrimitiveType(MUON_TYPE_STRING));
  function.return_type = CreateMuonPrimitiveType(MUON_TYPE_STRING);

  const auto encoded = CreateMuonRendererMetadata(namespaces, {function});
  const auto decoded = ReadMuonRendererMetadata(encoded);
  return Expect(decoded.namespaces.size() == 1,
                "namespace metadata was not decoded") &&
         Expect(decoded.namespaces[0].plugin_namespace == "muon.test.setup",
                "namespace name changed during metadata roundtrip") &&
         Expect(decoded.namespaces[0].setup_script == setup_script,
                "setup script changed during metadata roundtrip") &&
         Expect(decoded.namespaces[0].allowed_function_names.size() == 1,
                "allowed setup function names were not decoded") &&
         Expect(decoded.namespaces[0].allowed_function_names[0] == "publicRaw",
                "allowed setup function name changed during roundtrip") &&
         Expect(decoded.functions.size() == 1,
                "function metadata was not decoded") &&
         Expect(decoded.functions[0].plugin_namespace == "muon.test.setup",
                "function namespace changed during metadata roundtrip") &&
         Expect(decoded.functions[0].js_name == "__raw",
                "function name changed during metadata roundtrip") &&
         Expect(decoded.functions[0].public_name == "publicRaw",
                "function public name changed during metadata roundtrip");
}

static bool RunCefRpcDecodeTest() {
  const auto bridge = CreateMuonCefRpcBridge();
  MuonRpcOwner owner;
  owner.browser_id = 3;
  owner.frame_id = "frame-rpc";
  owner.context_id = 7;

  auto function_type = MuonTypeMetadata{};
  function_type.type = MUON_TYPE_FUNCTION;
  function_type.function_arg_types.push_back(
      CreateMuonPrimitiveType(MUON_TYPE_STRING));
  function_type.function_return_type.push_back(
      CreateMuonPrimitiveType(MUON_TYPE_BOOL));
  const auto expected_types = std::vector<MuonTypeMetadata>{
      CreateMuonPrimitiveType(MUON_TYPE_I32),
      CreateMuonPrimitiveType(MUON_TYPE_U64),
      function_type,
  };
  const auto encoded = CefListValue::Create();
  encoded->SetSize(expected_types.size());
  encoded->SetInt(0, -42);
  encoded->SetString(1, "18446744073709551615");
  const auto encoded_function = CefDictionaryValue::Create();
  encoded_function->SetInt("context_id", owner.context_id);
  encoded_function->SetInt("function_id", 11);
  encoded_function->SetString("type_key",
                              CreateMuonTypeCanonicalKey(function_type));
  encoded->SetDictionary(2, encoded_function);

  auto values = std::vector<MuonRpcValue>{};
  auto error_message = std::string{};
  if (!Expect(bridge->DecodeArguments(owner, expected_types, encoded, nullptr,
                                      &values, &error_message),
              "CEF RPC arguments were not decoded: " + error_message)) {
    return false;
  }
  return Expect(values.size() == 3, "CEF RPC argument count changed") &&
         Expect(values[0].i32_value == -42,
                "CEF RPC i32 value changed") &&
         Expect(values[1].u64_value ==
                    std::numeric_limits<uint64_t>::max(),
                "CEF RPC u64 value changed") &&
         Expect(values[2].function.kind ==
                    MuonRpcFunctionKind::RendererSource,
                "CEF RPC renderer function kind changed") &&
         Expect(values[2].function.function_id == 11,
                "CEF RPC renderer function id changed");
}

static bool RunCefRpcSharedBufferDecodeTest() {
  const auto bridge = CreateMuonCefRpcBridge();
  const auto bytes = std::vector<uint8_t>{2, 4, 6, 8};
  MuonCreatedSharedBufferMessage created;
  auto error_message = std::string{};
  if (!Expect(CreateMuonSharedBufferMessage(
                  kMuonPluginCallSharedMessageName, 13, 17,
                  {{0, bytes.data(), bytes.size()}}, &created,
                  &error_message),
              "CEF shared RPC payload was not created: " + error_message)) {
    return false;
  }
  auto decoded_call_id = 0;
  auto payload = std::shared_ptr<MuonSharedBufferPayload>{};
  if (!Expect(DecodeMuonSharedBufferPayload(
                  created.message, &decoded_call_id, &payload,
                  &error_message),
              "CEF shared RPC payload was not decoded: " + error_message)) {
    return false;
  }
  const auto encoded = CefListValue::Create();
  encoded->SetSize(1);
  encoded->SetDictionary(0,
                         CreateMuonSharedBufferPlaceholder(created.entries[0]));
  MuonRpcOwner owner;
  owner.browser_id = 5;
  owner.frame_id = "frame-buffer";
  owner.context_id = 17;
  auto values = std::vector<MuonRpcValue>{};
  if (!Expect(bridge->DecodeArguments(
                  owner,
                  {CreateMuonPrimitiveType(MUON_TYPE_BUFFER_VIEW)}, encoded,
                  payload, &values, &error_message),
              "CEF shared RPC argument was not decoded: " + error_message)) {
    return false;
  }
  const auto* decoded_bytes = static_cast<const uint8_t*>(
      GetMuonRpcBinaryData(values[0].binary));
  const auto runtime_services = bridge->CreateRuntimeServices();
  const auto allocated = runtime_services.allocate_buffer(6, &error_message);
  return Expect(decoded_call_id == 13,
                "CEF shared RPC call id changed") &&
         Expect(decoded_bytes != nullptr && decoded_bytes[0] == 2 &&
                    decoded_bytes[3] == 8,
                "CEF shared RPC bytes changed") &&
         Expect(allocated != nullptr && allocated->GetSize() == 6 &&
                    allocated->GetData() != nullptr,
                "CEF runtime transport buffer was not allocated");
}

static bool RunCefRendererResultDecodeTest() {
  const auto bridge = CreateMuonCefRpcBridge();
  MuonRpcOwner owner;
  owner.browser_id = 9;
  owner.frame_id = "frame-result";
  owner.context_id = 23;
  const auto message =
      CefProcessMessage::Create(kMuonRendererFunctionResultMessageName);
  const auto args = message->GetArgumentList();
  args->SetSize(5);
  args->SetInt(0, 29);
  args->SetBool(1, true);
  args->SetInt(2, MUON_TYPE_STRING);
  args->SetString(3, "completed");
  args->SetInt(4, owner.context_id);
  MuonRpcRendererFunctionResult result;
  auto error_message = std::string{};
  return Expect(bridge->DecodeRendererFunctionResult(
                    owner, CreateMuonPrimitiveType(MUON_TYPE_STRING), message,
                    nullptr, &result, &error_message),
                "CEF renderer result was not decoded: " + error_message) &&
         Expect(result.success && result.call_id == 29,
                "CEF renderer result metadata changed") &&
         Expect(result.value.string_value == "completed",
                "CEF renderer result value changed");
}

int main(int argc, char* argv[]) {
  CefMainArgs main_args(argc, argv);
  const auto exit_code = CefExecuteProcess(main_args, nullptr, nullptr);
  if (exit_code >= 0) {
    return exit_code;
  }

  CefSettings settings;
  settings.no_sandbox = true;
  if (argc >= 2) {
    const auto resource_dir = std::string(argv[1]);
    CefString(&settings.resources_dir_path).FromString(resource_dir);
    CefString(&settings.locales_dir_path).FromString(resource_dir + "/locales");
    CefString(&settings.browser_subprocess_path)
        .FromString(resource_dir + "/muon");
    CefString(&settings.root_cache_path)
        .FromString(resource_dir + "/metadata-test-cache");
  }
  if (!CefInitialize(main_args, settings, nullptr, nullptr)) {
    std::cerr << "failed to initialize CEF\n";
    return 1;
  }
  const auto passed = RunNamespaceSetupScriptRoundtripTest() &&
                      RunCefRpcDecodeTest() &&
                      RunCefRpcSharedBufferDecodeTest() &&
                      RunCefRendererResultDecodeTest();
  CefShutdown();
  return passed ? 0 : 1;
}
