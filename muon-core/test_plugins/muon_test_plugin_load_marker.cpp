/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "muon_plugin_api.h"

#include <cstdio>
#include <cstdlib>
#include <stdint.h>

static void write_load_marker() {
  const auto* path = std::getenv("MUON_TEST_PLUGIN_LOAD_MARKER");
  if (path == nullptr || path[0] == '\0') {
    return;
  }
  auto* file = std::fopen(path, "ab");
  if (file == nullptr) {
    return;
  }
  std::fputs("loaded\n", file);
  std::fclose(file);
}

struct LoadMarker {
  LoadMarker() { write_load_marker(); }
};

static LoadMarker load_marker;

extern "C" void load_marker_name(muon_completion_func comp) {
  const auto* result = "load-marker";
  comp(&result, nullptr);
}

static const muon_type_descriptor type_string = {
    MUON_TYPE_STRING,
    nullptr,
};

static const muon_plugin_function_metadata load_marker_functions[] = {
    {
        "loadMarkerName",
        reinterpret_cast<muon_native_function>(&load_marker_name),
        {0, nullptr, &type_string},
        nullptr,
    },
};

static const muon_plugin_function_metadata* const load_marker_functions_pointers[] = {
    &load_marker_functions[0],
    nullptr,
};

static const muon_plugin_namespace load_marker_namespaces[] = {
    {
        "muon.test.loadMarker",
        nullptr,
        load_marker_functions_pointers,
    },
};

static const muon_plugin_namespace* const load_marker_namespaces_pointers[] = {
    &load_marker_namespaces[0],
    nullptr,
};

static const muon_plugin_metadata load_marker_metadata = {
    load_marker_namespaces_pointers,
};

extern "C" const muon_plugin_metadata* muon_init_plugin(
    const muon_plugin_helpers* helpers) {
  (void)helpers;
  return &load_marker_metadata;
}
