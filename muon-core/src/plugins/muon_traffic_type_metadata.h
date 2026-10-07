/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once

#include "plugins/muon_traffic_adapter.h"
#include "plugins/muon_type_metadata.h"

#include <memory>
#include <vector>

/**
 * Owns a tra-ffic type descriptor and any nested signature it points to.
 */
struct MuonTypeDescriptorStorage {
  tra_ffic_type descriptor = {TRA_FFIC_TYPE_VOID, nullptr};
  std::unique_ptr<struct MuonFunctionSignatureStorage> function_signature;
};

/**
 * Owns a tra-ffic function signature and all nested descriptors it points to.
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
 * Creates owned tra-ffic signature storage from recursive metadata.
 *
 * @param arg_types Function argument metadata.
 * @param return_type Function result metadata.
 * @return Owned signature storage with stable nested descriptor pointers.
 */
std::unique_ptr<MuonFunctionSignatureStorage>
CreateMuonFunctionSignatureStorage(
    const std::vector<MuonTypeMetadata>& arg_types,
    const MuonTypeMetadata& return_type);

/**
 * Creates owned tra-ffic signature storage from recursive metadata.
 *
 * @param arg_types Function argument metadata.
 * @param return_type Function result metadata.
 * @param abi Native function ABI used by the signature.
 * @return Owned signature storage with stable nested descriptor pointers.
 */
std::unique_ptr<MuonFunctionSignatureStorage>
CreateMuonFunctionSignatureStorageForAbi(
    const std::vector<MuonTypeMetadata>& arg_types,
    const MuonTypeMetadata& return_type,
    tra_ffic_signature_abi abi);

/**
 * Creates owned tra-ffic type descriptor storage from recursive metadata.
 *
 * @param type Recursive type metadata.
 * @return Owned descriptor storage with stable nested signature pointers.
 */
MuonTypeDescriptorStorage CreateMuonTypeDescriptorStorage(
    const MuonTypeMetadata& type);

/**
 * Returns the tra-ffic signature pointer owned by storage.
 *
 * @param storage Signature storage to access.
 * @return Owned signature pointer, or null when storage is null.
 */
const tra_ffic_signature* GetMuonFunctionSignature(
    const MuonFunctionSignatureStorage* storage);
