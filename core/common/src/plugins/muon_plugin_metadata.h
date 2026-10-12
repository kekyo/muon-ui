/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once

#include "plugins/muon_type_metadata.h"

#include <cstdint>
#include <string>
#include <vector>

/**
 * JavaScript-visible metadata for one plugin function.
 */
struct MuonFunctionMetadata {
  uint32_t id = 0;
  std::string plugin_namespace;
  /**
   * Internal JavaScript function name injected in simple mode.
   */
  std::string js_name;
  /**
   * Public function name used by filters and capability imports.
   */
  std::string public_name;
  std::vector<MuonTypeMetadata> arg_types;
  MuonTypeMetadata return_type = CreateMuonPrimitiveType(MUON_TYPE_VOID);
};

/**
 * JavaScript-visible metadata for one plugin namespace.
 */
struct MuonNamespaceMetadata {
  std::string plugin_namespace;
  std::string setup_script;
  std::vector<std::string> allowed_function_names;
};

/**
 * Renderer startup metadata for plugin namespaces and functions.
 */
struct MuonRendererMetadata {
  std::vector<MuonNamespaceMetadata> namespaces;
  std::vector<MuonFunctionMetadata> functions;
};

/**
 * Returns true when name can be exposed as a JavaScript property.
 *
 * @param name JavaScript property name.
 */
bool IsValidMuonJsIdentifier(const std::string& name);

/**
 * Splits and validates a dot-notation plugin namespace.
 *
 * @param plugin_namespace Dot-notation namespace.
 * @param segments Receives namespace segments when non-null.
 */
bool SplitMuonPluginNamespace(const std::string& plugin_namespace,
                               std::vector<std::string>* segments);

/**
 * Returns true when plugin_namespace is a valid dot-notation namespace.
 *
 * @param plugin_namespace Dot-notation namespace.
 */
bool IsValidMuonPluginNamespace(const std::string& plugin_namespace);

/**
 * Creates the full JavaScript public path for one plugin function.
 *
 * @param plugin_namespace Dot-notation plugin namespace.
 * @param js_name JavaScript function property name.
 */
std::string CreateMuonFunctionPublicPath(
    const std::string& plugin_namespace,
    const std::string& js_name);

/**
 * Creates the full JavaScript public path for one plugin function.
 *
 * @param function Function metadata.
 */
std::string CreateMuonFunctionPublicPath(
    const MuonFunctionMetadata& function);
