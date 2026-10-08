/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once

#include "muon_plugin_api.h"

#include <string>
#include <vector>

/**
 * C++ owned recursive type descriptor used after plugin metadata validation.
 */
struct MuonTypeMetadata {
  muon_value_type type = MUON_TYPE_VOID;
  std::vector<MuonTypeMetadata> function_arg_types;
  std::vector<MuonTypeMetadata> function_return_type;
};

/**
 * Returns a primitive type descriptor.
 */
MuonTypeMetadata CreateMuonPrimitiveType(muon_value_type type);

/**
 * Returns true when the value type is supported by the ABI.
 */
bool IsSupportedMuonValueType(muon_value_type type);

/**
 * Returns true when type can appear as a non-function argument leaf.
 */
bool IsSupportedMuonArgumentLeafType(muon_value_type type);

/**
 * Returns true when type can appear as a non-function return leaf.
 */
bool IsSupportedMuonReturnLeafType(muon_value_type type);

/**
 * Converts a C type descriptor into owned metadata.
 *
 * @param source Descriptor to convert.
 * @param allow_void Whether void is valid at this position.
 * @param target Receives converted metadata.
 * @param error_message Receives a validation diagnostic.
 */
bool ConvertMuonTypeDescriptor(const muon_type_descriptor* source,
                                bool allow_void,
                                MuonTypeMetadata* target,
                                std::string* error_message);

/**
 * Converts a C function signature into owned argument and return metadata.
 */
bool ConvertMuonFunctionSignature(
    const muon_function_signature& source,
    std::vector<MuonTypeMetadata>* arg_types,
    MuonTypeMetadata* return_type,
    std::string* error_message);

/**
 * Returns a stable canonical key for a recursive type descriptor.
 */
std::string CreateMuonTypeCanonicalKey(const MuonTypeMetadata& type);

/**
 * Returns a stable canonical key for a function type.
 */
std::string CreateMuonFunctionTypeCanonicalKey(
    const std::vector<MuonTypeMetadata>& arg_types,
    const MuonTypeMetadata& return_type);

/**
 * Returns true when two recursive type descriptors are structurally equal.
 */
bool AreEqualMuonTypes(const MuonTypeMetadata& first,
                        const MuonTypeMetadata& second);

/**
 * Returns a stable lowercase name for a plugin value type.
 */
const char* GetMuonValueTypeName(muon_value_type type);
