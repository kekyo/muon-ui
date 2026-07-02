/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#pragma once

#include "plugins/muon_plugin_value.h"
#include "plugins/muon_type_metadata.h"

#include "include/cef_values.h"

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

/**
 * Creates a CEF dictionary for one recursive type descriptor.
 *
 * @param type Type metadata to serialize.
 */
CefRefPtr<CefDictionaryValue> CreateMuonTypeMetadataDictionary(
    const MuonTypeMetadata& type);

/**
 * Reads a recursive type descriptor from a CEF dictionary.
 *
 * @param dictionary Dictionary to read.
 * @param allow_void Whether void is valid at this position.
 * @param type Receives decoded metadata.
 */
bool ReadMuonTypeMetadataDictionary(CefRefPtr<CefDictionaryValue> dictionary,
                                     bool allow_void,
                                     MuonTypeMetadata* type);

/**
 * Creates renderer startup metadata from plugin function metadata.
 *
 * @param namespaces Namespaces to serialize.
 * @param functions Functions to serialize.
 */
CefRefPtr<CefDictionaryValue> CreateMuonRendererMetadata(
    const std::vector<MuonNamespaceMetadata>& namespaces,
    const std::vector<MuonFunctionMetadata>& functions);

/**
 * Reads renderer startup metadata.
 *
 * @param extra_info Browser extra info dictionary passed to the renderer.
 */
MuonRendererMetadata ReadMuonRendererMetadata(
    CefRefPtr<CefDictionaryValue> extra_info);

/**
 * Writes a renderer URL hint for popup browser startup.
 *
 * @param extra_info Browser extra info dictionary passed to the renderer.
 * @param url Initial popup target URL.
 */
void WriteMuonRendererUrlHint(CefRefPtr<CefDictionaryValue> extra_info,
                              const std::string& url);

/**
 * Reads a renderer URL hint for popup browser startup.
 *
 * @param extra_info Browser extra info dictionary passed to the renderer.
 * @return Initial popup target URL, or an empty string when absent.
 */
std::string ReadMuonRendererUrlHint(CefRefPtr<CefDictionaryValue> extra_info);
