/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#pragma once

#include "muon_plugin_api.h"
#include "plugins/muon_traffic_adapter.h"

#include <memory>
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
 * Owns a C ABI type descriptor and any nested signature it points to.
 */
struct MuonTypeDescriptorStorage {
  tra_ffic_type descriptor = {TRA_FFIC_TYPE_VOID, nullptr};
  std::unique_ptr<struct MuonFunctionSignatureStorage> function_signature;
};

/**
 * Owns a C ABI function signature and all nested descriptors it points to.
 */
struct MuonFunctionSignatureStorage {
  std::vector<MuonTypeDescriptorStorage> argument_storage;
  MuonTypeDescriptorStorage return_storage;
  std::vector<tra_ffic_type> argument_descriptors;
  tra_ffic_signature signature = {
      TRA_FFIC_SIGNATURE_ABI_COMPLETION,
      0,
      nullptr,
      nullptr,
      TRA_FFIC_ARGUMENT_PASSING_STACK,
  };
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
 * Creates owned C ABI signature storage from recursive metadata.
 *
 * @param arg_types Function argument metadata.
 * @param return_type Function result metadata.
 * @returns Owned signature storage with stable nested descriptor pointers.
 */
std::unique_ptr<MuonFunctionSignatureStorage>
CreateMuonFunctionSignatureStorage(
    const std::vector<MuonTypeMetadata>& arg_types,
    const MuonTypeMetadata& return_type);

/**
 * Creates owned C ABI type descriptor storage from recursive metadata.
 *
 * @param type Recursive type metadata.
 * @returns Owned descriptor storage with stable nested signature pointers.
 */
MuonTypeDescriptorStorage CreateMuonTypeDescriptorStorage(
    const MuonTypeMetadata& type);

/**
 * Returns the C ABI signature pointer owned by storage.
 */
const tra_ffic_signature* GetMuonFunctionSignature(
    const MuonFunctionSignatureStorage* storage);

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
