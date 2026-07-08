/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "plugins/muon_type_metadata.h"

#include <iostream>
#include <string>
#include <utility>
#include <vector>

static bool Expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << "\n";
    return false;
  }
  return true;
}

static MuonTypeMetadata Type(muon_value_type type) {
  return CreateMuonPrimitiveType(type);
}

static MuonTypeMetadata FunctionType(
    std::vector<MuonTypeMetadata> arg_types,
    MuonTypeMetadata return_type) {
  MuonTypeMetadata type;
  type.type = MUON_TYPE_FUNCTION;
  type.function_arg_types = std::move(arg_types);
  type.function_return_type.push_back(std::move(return_type));
  return type;
}

static bool RunPrimitiveConversionTest() {
  const auto primitive_types = std::vector<muon_value_type>{
      MUON_TYPE_VOID, MUON_TYPE_BOOL, MUON_TYPE_I8,
      MUON_TYPE_U8,   MUON_TYPE_I16,  MUON_TYPE_U16,
      MUON_TYPE_I32,  MUON_TYPE_U32,  MUON_TYPE_I64,
      MUON_TYPE_U64,  MUON_TYPE_F32,  MUON_TYPE_F64,
      MUON_TYPE_STRING, MUON_TYPE_POINTER, MUON_TYPE_BUFFER_VIEW,
  };
  for (const auto value_type : primitive_types) {
    const muon_type_descriptor descriptor = {value_type, nullptr};
    MuonTypeMetadata converted;
    std::string error_message;
    if (!Expect(ConvertMuonTypeDescriptor(&descriptor, true, &converted,
                                           &error_message),
                error_message)) {
      return false;
    }
    if (!Expect(converted.type == value_type,
                "primitive type conversion changed the value type")) {
      return false;
    }
  }
  return true;
}

static bool RunNestedFunctionRoundtripTest() {
  const auto one_deep =
      FunctionType({Type(MUON_TYPE_I32)}, Type(MUON_TYPE_I32));
  const auto two_deep = FunctionType({one_deep}, Type(MUON_TYPE_BOOL));
  const auto three_deep = FunctionType({two_deep}, one_deep);
  const auto same_three_deep = FunctionType({two_deep}, one_deep);
  const auto different_three_deep =
      FunctionType({two_deep}, Type(MUON_TYPE_STRING));

  if (!Expect(CreateMuonTypeCanonicalKey(three_deep) ==
                  CreateMuonTypeCanonicalKey(same_three_deep),
              "same nested function type produced different canonical keys")) {
    return false;
  }
  if (!Expect(CreateMuonTypeCanonicalKey(three_deep) !=
                  CreateMuonTypeCanonicalKey(different_three_deep),
              "different nested function type produced the same key")) {
    return false;
  }

  const muon_type_descriptor public_i32 = {MUON_TYPE_I32, nullptr};
  const muon_type_descriptor public_bool = {MUON_TYPE_BOOL, nullptr};
  const muon_type_descriptor public_one_args[] = {public_i32};
  const muon_function_signature public_one_signature = {
      1,
      public_one_args,
      &public_i32,
  };
  const muon_type_descriptor public_one_function = {
      MUON_TYPE_FUNCTION,
      &public_one_signature,
  };
  const muon_type_descriptor public_two_args[] = {public_one_function};
  const muon_function_signature public_two_signature = {
      1,
      public_two_args,
      &public_bool,
  };
  const muon_type_descriptor public_two_function = {
      MUON_TYPE_FUNCTION,
      &public_two_signature,
  };
  const muon_type_descriptor public_three_args[] = {public_two_function};
  const muon_function_signature public_three_signature = {
      1,
      public_three_args,
      &public_one_function,
  };
  const muon_type_descriptor public_three_function = {
      MUON_TYPE_FUNCTION,
      &public_three_signature,
  };
  const muon_type_descriptor public_top_args[] = {public_two_function};
  const muon_function_signature public_top_signature = {
      1,
      public_top_args,
      &public_three_function,
  };

  std::vector<MuonTypeMetadata> roundtrip_args;
  MuonTypeMetadata roundtrip_return;
  std::string error_message;
  if (!Expect(ConvertMuonFunctionSignature(
                  public_top_signature, &roundtrip_args, &roundtrip_return,
                  &error_message),
              error_message)) {
    return false;
  }
  return Expect(roundtrip_args.size() == 1 &&
                    AreEqualMuonTypes(roundtrip_args[0], two_deep) &&
                    AreEqualMuonTypes(roundtrip_return, three_deep),
                "nested signature roundtrip changed the type structure");
}

static bool RunUnsupportedTypeTest() {
  const muon_type_descriptor unsupported = {
      static_cast<muon_value_type>(16),
      nullptr,
  };
  MuonTypeMetadata converted;
  std::string error_message;
  return Expect(!ConvertMuonTypeDescriptor(&unsupported, true, &converted,
                                            &error_message),
                "unsupported tra-ffic type was accepted");
}

static bool RunTrafficTypeMappingTest() {
  auto traffic_type = TRA_FFIC_TYPE_VOID;
  auto muon_type = MUON_TYPE_VOID;
  return Expect(ConvertMuonValueTypeToTraffic(MUON_TYPE_POINTER,
                                               &traffic_type),
                "pointer type did not convert to tra-ffic") &&
         Expect(traffic_type == TRA_FFIC_TYPE_POINTER,
                "pointer type converted to the wrong tra-ffic type") &&
         Expect(ConvertTrafficValueTypeToMuon(TRA_FFIC_TYPE_POINTER,
                                               &muon_type),
                "tra-ffic pointer type did not convert to muon") &&
         Expect(muon_type == MUON_TYPE_POINTER,
                "tra-ffic pointer type converted to the wrong muon type") &&
         Expect(ConvertMuonValueTypeToTraffic(MUON_TYPE_BUFFER_VIEW,
                                               &traffic_type),
                "buffer_view type did not convert to tra-ffic") &&
         Expect(traffic_type == TRA_FFIC_TYPE_BUFFER_VIEW,
                "buffer_view type converted to the wrong tra-ffic type") &&
         Expect(ConvertTrafficValueTypeToMuon(TRA_FFIC_TYPE_BUFFER_VIEW,
                                               &muon_type),
                "tra-ffic buffer_view type did not convert to muon") &&
         Expect(muon_type == MUON_TYPE_BUFFER_VIEW,
                "tra-ffic buffer_view type converted to the wrong muon type") &&
         Expect(CreateMuonTypeCanonicalKey(Type(MUON_TYPE_BUFFER_VIEW)) ==
                    "buffer_view",
                "buffer_view canonical key is invalid");
}

static bool RunTrafficSignatureAbiTest() {
  const auto nested =
      FunctionType({Type(MUON_TYPE_I32)}, Type(MUON_TYPE_BOOL));
  const auto storage = CreateMuonFunctionSignatureStorage({nested}, nested);
  const auto* signature = GetMuonFunctionSignature(storage.get());
  if (!Expect(signature != nullptr, "traffic signature was not created")) {
    return false;
  }
  if (!Expect(signature->abi == TRA_FFIC_SIGNATURE_ABI_COMPLETION,
              "traffic signature did not use completion ABI")) {
    return false;
  }
  if (!Expect(signature->arg_count == 1,
              "traffic signature changed the argument count")) {
    return false;
  }
  const auto* argument_signature =
      signature->arg_types[0].function_signature;
  if (!Expect(argument_signature != nullptr,
              "traffic argument function signature was not created")) {
    return false;
  }
  if (!Expect(argument_signature->abi == TRA_FFIC_SIGNATURE_ABI_COMPLETION,
              "traffic argument signature did not use completion ABI")) {
    return false;
  }
  const auto* return_signature =
      signature->return_type->function_signature;
  if (!Expect(return_signature != nullptr,
              "traffic return function signature was not created")) {
    return false;
  }
  return Expect(return_signature->abi == TRA_FFIC_SIGNATURE_ABI_COMPLETION,
                "traffic return signature did not use completion ABI");
}

int main() {
  const auto passed = RunPrimitiveConversionTest() &&
                      RunNestedFunctionRoundtripTest() &&
                      RunUnsupportedTypeTest() &&
                      RunTrafficTypeMappingTest() &&
                      RunTrafficSignatureAbiTest();
  return passed ? 0 : 1;
}
