/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "plugins/muon_traffic_type_metadata.h"

#include <memory>

std::unique_ptr<MuonFunctionSignatureStorage>
CreateMuonFunctionSignatureStorage(
    const std::vector<MuonTypeMetadata>& arg_types,
    const MuonTypeMetadata& return_type) {
  return CreateMuonFunctionSignatureStorageForAbi(
      arg_types, return_type, TRA_FFIC_SIGNATURE_ABI_COMPLETION);
}

std::unique_ptr<MuonFunctionSignatureStorage>
CreateMuonFunctionSignatureStorageForAbi(
    const std::vector<MuonTypeMetadata>& arg_types,
    const MuonTypeMetadata& return_type,
    tra_ffic_signature_abi abi) {
  auto storage = std::make_unique<MuonFunctionSignatureStorage>();
  storage->argument_storage.reserve(arg_types.size());
  for (const auto& arg_type : arg_types) {
    storage->argument_storage.push_back(
        CreateMuonTypeDescriptorStorage(arg_type));
  }
  storage->return_storage = CreateMuonTypeDescriptorStorage(return_type);

  storage->argument_descriptors.reserve(storage->argument_storage.size());
  for (const auto& arg_storage : storage->argument_storage) {
    storage->argument_descriptors.push_back(arg_storage.descriptor);
  }

  storage->signature.abi = abi;
  storage->signature.arg_count =
      static_cast<uint32_t>(storage->argument_descriptors.size());
  storage->signature.arg_types = storage->argument_descriptors.empty()
                                     ? nullptr
                                     : storage->argument_descriptors.data();
  storage->signature.return_type = &storage->return_storage.descriptor;
  storage->signature.argument_passing = TRA_FFIC_ARGUMENT_PASSING_STACK;
  return storage;
}

const tra_ffic_signature* GetMuonFunctionSignature(
    const MuonFunctionSignatureStorage* storage) {
  if (storage == nullptr) {
    return nullptr;
  }
  return &storage->signature;
}

MuonTypeDescriptorStorage CreateMuonTypeDescriptorStorage(
    const MuonTypeMetadata& type) {
  MuonTypeDescriptorStorage storage;
  if (!ConvertMuonValueTypeToTraffic(type.type, &storage.descriptor.kind)) {
    storage.descriptor.kind = TRA_FFIC_TYPE_VOID;
  }
  storage.descriptor.function_signature = nullptr;
  if (type.type == MUON_TYPE_FUNCTION && !type.function_return_type.empty()) {
    storage.function_signature = CreateMuonFunctionSignatureStorage(
        type.function_arg_types, type.function_return_type[0]);
    storage.descriptor.function_signature =
        GetMuonFunctionSignature(storage.function_signature.get());
  }
  return storage;
}
