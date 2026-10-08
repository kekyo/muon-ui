/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once

#include "plugins/muon_plugin_metadata.h"

#include "include/cef_values.h"

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
