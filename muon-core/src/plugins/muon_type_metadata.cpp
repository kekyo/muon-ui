/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "plugins/muon_type_metadata.h"

#include <memory>
#include <string>

static constexpr uint32_t kMaxMuonTypeDepth = 16;

MuonTypeMetadata CreateMuonPrimitiveType(muon_value_type type) {
  MuonTypeMetadata metadata;
  metadata.type = type;
  return metadata;
}

bool IsSupportedMuonValueType(muon_value_type type) {
  return type == MUON_TYPE_VOID || type == MUON_TYPE_BOOL ||
         type == MUON_TYPE_I8 || type == MUON_TYPE_U8 ||
         type == MUON_TYPE_I16 || type == MUON_TYPE_U16 ||
         type == MUON_TYPE_I32 || type == MUON_TYPE_U32 ||
         type == MUON_TYPE_I64 || type == MUON_TYPE_U64 ||
         type == MUON_TYPE_F32 || type == MUON_TYPE_F64 ||
         type == MUON_TYPE_STRING || type == MUON_TYPE_POINTER ||
         type == MUON_TYPE_FUNCTION || type == MUON_TYPE_BUFFER_VIEW;
}

bool IsSupportedMuonArgumentLeafType(muon_value_type type) {
  return type != MUON_TYPE_VOID && type != MUON_TYPE_FUNCTION &&
         IsSupportedMuonValueType(type);
}

bool IsSupportedMuonReturnLeafType(muon_value_type type) {
  return type != MUON_TYPE_FUNCTION && IsSupportedMuonValueType(type);
}

static bool ConvertMuonTypeDescriptorWithDepth(
    const muon_type_descriptor* source,
    bool allow_void,
    uint32_t depth,
    MuonTypeMetadata* target,
    std::string* error_message) {
  if (source == nullptr) {
    *error_message = "Type descriptor is null";
    return false;
  }
  if (depth > kMaxMuonTypeDepth) {
    *error_message = "Function type nesting is too deep";
    return false;
  }
  if (!IsSupportedMuonValueType(source->type)) {
    *error_message = "Type descriptor has unsupported type";
    return false;
  }
  if (!allow_void && source->type == MUON_TYPE_VOID) {
    *error_message = "Void type is not valid here";
    return false;
  }

  target->type = source->type;
  target->function_arg_types.clear();
  target->function_return_type.clear();
  if (source->type != MUON_TYPE_FUNCTION) {
    if (source->function_signature != nullptr) {
      *error_message = "Primitive type unexpectedly has a function signature";
      return false;
    }
    return true;
  }

  if (source->function_signature == nullptr) {
    *error_message = "Function type is missing a signature";
    return false;
  }
  const auto& signature = *source->function_signature;
  if (signature.arg_count > 0 && signature.arg_types == nullptr) {
    *error_message = "Function type argument table is null";
    return false;
  }
  if (signature.return_type == nullptr) {
    *error_message = "Function type return descriptor is null";
    return false;
  }

  target->function_arg_types.resize(signature.arg_count);
  for (auto index = size_t{0}; index < signature.arg_count; ++index) {
    if (!ConvertMuonTypeDescriptorWithDepth(
            &signature.arg_types[index], false, depth + 1,
            &target->function_arg_types[index], error_message)) {
      return false;
    }
  }

  target->function_return_type.resize(1);
  return ConvertMuonTypeDescriptorWithDepth(
      signature.return_type, true, depth + 1,
      &target->function_return_type[0], error_message);
}

bool ConvertMuonTypeDescriptor(const muon_type_descriptor* source,
                                bool allow_void,
                                MuonTypeMetadata* target,
                                std::string* error_message) {
  return ConvertMuonTypeDescriptorWithDepth(source, allow_void, 0, target,
                                             error_message);
}

bool ConvertMuonFunctionSignature(
    const muon_function_signature& source,
    std::vector<MuonTypeMetadata>* arg_types,
    MuonTypeMetadata* return_type,
    std::string* error_message) {
  if (source.arg_count > 0 && source.arg_types == nullptr) {
    *error_message = "Function argument type table is null";
    return false;
  }
  if (source.return_type == nullptr) {
    *error_message = "Function return type is null";
    return false;
  }

  arg_types->clear();
  arg_types->resize(source.arg_count);
  for (auto index = size_t{0}; index < source.arg_count; ++index) {
    if (!ConvertMuonTypeDescriptor(&source.arg_types[index], false,
                                    &(*arg_types)[index], error_message)) {
      return false;
    }
  }

  return ConvertMuonTypeDescriptor(source.return_type, true, return_type,
                                    error_message);
}

std::unique_ptr<MuonFunctionSignatureStorage>
CreateMuonFunctionSignatureStorage(
    const std::vector<MuonTypeMetadata>& arg_types,
    const MuonTypeMetadata& return_type) {
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

  storage->signature.abi = TRA_FFIC_SIGNATURE_ABI_COMPLETION;
  storage->signature.arg_count =
      static_cast<uint32_t>(storage->argument_descriptors.size());
  storage->signature.arg_types = storage->argument_descriptors.empty()
                                     ? nullptr
                                     : storage->argument_descriptors.data();
  storage->signature.return_type = &storage->return_storage.descriptor;
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

std::string CreateMuonTypeCanonicalKey(const MuonTypeMetadata& type) {
  if (type.type != MUON_TYPE_FUNCTION) {
    return GetMuonValueTypeName(type.type);
  }
  if (type.function_return_type.empty()) {
    return "function(?)";
  }
  return CreateMuonFunctionTypeCanonicalKey(
      type.function_arg_types, type.function_return_type[0]);
}

std::string CreateMuonFunctionTypeCanonicalKey(
    const std::vector<MuonTypeMetadata>& arg_types,
    const MuonTypeMetadata& return_type) {
  auto key = std::string("function(");
  for (auto index = size_t{0}; index < arg_types.size(); ++index) {
    if (index > 0) {
      key += ",";
    }
    key += CreateMuonTypeCanonicalKey(arg_types[index]);
  }
  key += ")->";
  key += CreateMuonTypeCanonicalKey(return_type);
  return key;
}

bool AreEqualMuonTypes(const MuonTypeMetadata& first,
                        const MuonTypeMetadata& second) {
  if (first.type != second.type) {
    return false;
  }
  if (first.type != MUON_TYPE_FUNCTION) {
    return true;
  }
  if (first.function_arg_types.size() != second.function_arg_types.size() ||
      first.function_return_type.size() !=
          second.function_return_type.size()) {
    return false;
  }
  for (auto index = size_t{0}; index < first.function_arg_types.size(); ++index) {
    if (!AreEqualMuonTypes(first.function_arg_types[index],
                            second.function_arg_types[index])) {
      return false;
    }
  }
  if (first.function_return_type.empty()) {
    return second.function_return_type.empty();
  }
  return AreEqualMuonTypes(first.function_return_type[0],
                            second.function_return_type[0]);
}

const char* GetMuonValueTypeName(muon_value_type type) {
  switch (type) {
    case MUON_TYPE_VOID:
      return "void";
    case MUON_TYPE_BOOL:
      return "bool";
    case MUON_TYPE_I8:
      return "i8";
    case MUON_TYPE_U8:
      return "u8";
    case MUON_TYPE_I16:
      return "i16";
    case MUON_TYPE_U16:
      return "u16";
    case MUON_TYPE_I32:
      return "i32";
    case MUON_TYPE_U32:
      return "u32";
    case MUON_TYPE_I64:
      return "i64";
    case MUON_TYPE_U64:
      return "u64";
    case MUON_TYPE_F32:
      return "f32";
    case MUON_TYPE_F64:
      return "f64";
    case MUON_TYPE_STRING:
      return "string";
    case MUON_TYPE_POINTER:
      return "pointer";
    case MUON_TYPE_FUNCTION:
      return "function";
    case MUON_TYPE_BUFFER_VIEW:
      return "buffer_view";
    default:
      return "unknown";
  }
}
