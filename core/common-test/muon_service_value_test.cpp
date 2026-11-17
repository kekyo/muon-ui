/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "services/muon_service_value.h"

#include <iostream>
#include <limits>
#include <stdexcept>

static void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

static void Reject(const muon_service_value& value) {
  try { (void)CopyMuonServiceValue(value); }
  catch (const std::invalid_argument&) { return; }
  throw std::runtime_error("invalid native service value was accepted");
}

int main() {
  auto text = std::string("hello\0\xf0\x9f\x98\x80", 10);
  auto bytes = std::vector<uint8_t>{0, 1, 127, 255};
  muon_service_value members[4]{};
  members[0].kind = MUON_SERVICE_VALUE_STRING;
  members[0].data.string_value = {text.data(), text.size()};
  members[1].kind = MUON_SERVICE_VALUE_BYTES;
  members[1].data.bytes_value = {bytes.data(), bytes.size()};
  members[2].kind = MUON_SERVICE_VALUE_INT64;
  members[2].data.int64_value = std::numeric_limits<int64_t>::min();
  members[3].kind = MUON_SERVICE_VALUE_UINT64;
  members[3].data.uint64_value = std::numeric_limits<uint64_t>::max();
  muon_service_value root{};
  root.kind = MUON_SERVICE_VALUE_ARRAY;
  root.data.array_value = {members, 4};
  auto owned = CopyMuonServiceValue(root);
  text.assign("changed");
  bytes.assign(4, 42);
  members[2].data.int64_value = 0;
  const auto& values = std::get<MuonServiceValue::Array>(owned.data);
  Require(std::get<std::string>(values[0].data) == std::string("hello\0\xf0\x9f\x98\x80", 10),
          "copied UTF-8 strings must preserve embedded NUL and survive source mutation");
  Require(std::get<MuonServiceValue::Bytes>(values[1].data) == MuonServiceValue::Bytes({0, 1, 127, 255}),
          "copied binary values must own their bytes");
  Require(std::get<int64_t>(values[2].data) == std::numeric_limits<int64_t>::min() &&
          std::get<uint64_t>(values[3].data) == std::numeric_limits<uint64_t>::max(),
          "64-bit limits must never round through floating point");

  MuonNativeServiceValues native({std::move(owned), MuonServiceValue{true},
      MuonServiceValue{int32_t{-123}}, MuonServiceValue{uint32_t{456}},
      MuonServiceValue{0.25}, MuonServiceValue{MuonServiceCallback{99}}, MuonServiceValue{}});
  auto moved = std::move(native);
  Require(moved.Get().size() == 7, "moving argument ownership must preserve all native views");
  const auto roundtrip = CopyMuonServiceValue(moved.Get()[0]);
  Require(std::get<MuonServiceValue::Array>(roundtrip.data).size() == 4,
          "nested native views must remain valid after moving their owner");
  Require(moved.Get()[1].data.boolean_value == 1 && moved.Get()[2].data.int32_value == -123 &&
          moved.Get()[3].data.uint32_value == 456 && moved.Get()[4].data.float64_value == 0.25 &&
          moved.Get()[5].data.callback_id == 99 && moved.Get()[6].kind == MUON_SERVICE_VALUE_VOID,
          "native views must preserve primitive and callback identities");

  muon_service_value invalid{};
  invalid.kind = static_cast<muon_service_value_kind>(999);
  Reject(invalid);
  invalid.kind = MUON_SERVICE_VALUE_STRING;
  invalid.data.string_value = {nullptr, 1};
  Reject(invalid);
  invalid.data.string_value = {"\xc0\xaf", 2};
  Reject(invalid);
  invalid.data.string_value = {"\xed\xa0\x80", 3};
  Reject(invalid);
  invalid.data.string_value = {"\xf4\x90\x80\x80", 4};
  Reject(invalid);
  invalid.data.string_value = {"\xe3\x81", 2};
  Reject(invalid);
  invalid.kind = MUON_SERVICE_VALUE_BYTES;
  invalid.data.bytes_value = {nullptr, 1};
  Reject(invalid);
  invalid.kind = MUON_SERVICE_VALUE_BOOLEAN;
  invalid.data.boolean_value = 2;
  Reject(invalid);
  invalid.kind = MUON_SERVICE_VALUE_FLOAT64;
  invalid.data.float64_value = std::numeric_limits<double>::infinity();
  Reject(invalid);
  invalid.kind = MUON_SERVICE_VALUE_CALLBACK;
  invalid.data.callback_id = 0;
  Reject(invalid);

  std::vector<muon_service_value> nested(18);
  for (size_t index = 0; index + 1 < nested.size(); ++index) {
    nested[index].kind = MUON_SERVICE_VALUE_ARRAY;
    nested[index].data.array_value = {&nested[index + 1], 1};
  }
  Reject(nested.front());
  nested[16].data.array_value = {nullptr, 0};
  Reject(nested.front());
  auto large = std::vector<uint8_t>(600 * 1024);
  members[0].kind = MUON_SERVICE_VALUE_BYTES;
  members[0].data.bytes_value = {large.data(), large.size()};
  members[1] = members[0];
  root.data.array_value = {members, 2};
  Reject(root);
  invalid.kind = MUON_SERVICE_VALUE_ARRAY;
  invalid.data.array_value = {nullptr, 1};
  Reject(invalid);
  invalid.data.array_value = {nullptr, std::numeric_limits<size_t>::max()};
  Reject(invalid);
  invalid.kind = MUON_SERVICE_VALUE_BYTES;
  invalid.data.bytes_value = {nullptr, std::numeric_limits<size_t>::max()};
  Reject(invalid);
  invalid.data.bytes_value = {nullptr, 0};
  Require(std::get<MuonServiceValue::Bytes>(CopyMuonServiceValue(invalid).data).empty(),
          "empty native buffers may use a null data pointer");
  std::cout << "native service values: passed\n";
}
