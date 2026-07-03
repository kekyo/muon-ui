/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "plugins/muon_plugin_metadata.h"

#include "include/cef_app.h"

#include <iostream>
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
  const auto passed = RunNamespaceSetupScriptRoundtripTest();
  CefShutdown();
  return passed ? 0 : 1;
}
