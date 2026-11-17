/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "services/muon_service_interface.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

template <typename Action>
static void Reject(Action action) {
  auto rejected = false;
  try { action(); } catch (const std::invalid_argument&) { rejected = true; }
  assert(rejected);
}

static MuonServiceType Type(const std::string& json) {
  return ParseMuonServiceInterface("{\"schemaVersion\":1,\"methods\":{\"echo\":{"
      "\"parameters\":[" + json + "],\"returns\":\"void\",\"errors\":[]}},\"events\":{}}")
      .methods.at("echo").signature.parameters.at(0);
}

int main() {
  const auto schema = ParseMuonServiceInterface(R"({"schemaVersion":1,"methods":{
    "echo":{"parameters":["int64","bytes",{"array":{"callback":{
      "parameters":["string"],"returns":"uint64"}}}],"returns":"float64",
      "errors":["unavailable"]}},"events":{"progress":{"array":"int32"}}})");
  const auto& method = schema.methods.at("echo");
  assert(method.signature.parameters.size() == 3);
  assert(method.signature.parameters[0].kind == MUON_SERVICE_VALUE_INT64);
  assert(method.signature.parameters[2].element->callback->parameters[0].kind ==
      MUON_SERVICE_VALUE_STRING);
  assert(method.errors == std::vector<std::string>{"unavailable"});
  assert(schema.events.at("progress").element->kind == MUON_SERVICE_VALUE_INT32);
  for (const auto* json : {
      R"({"schemaVersion":2,"methods":{},"events":{}})",
      R"({"schemaVersion":1,"methods":{},"events":{},"extra":0})",
      R"({"schemaVersion":1,"methods":{},"events":{"a":"void"}})",
      R"({"schemaVersion":1,"methods":{},"events":{"a":"pointer"}})",
      R"({"schemaVersion":1,"methods":{},"events":{"a":"bytes","a":"string"}})",
      R"({"schemaVersion":1,"methods":{},"events":{"a\u0000":"string"}})",
      R"({"schemaVersion":1,"methods":{"a":{"parameters":[],"returns":{"callback":{"parameters":[],"returns":"void"}},"errors":[]}},"events":{}})",
      R"({"schemaVersion":1,"methods":{"a":{"parameters":[],"returns":"void","errors":["oops","oops"]}},"events":{}})",
      R"({"schemaVersion":1,"methods":{},"methods":{},"events":{}})"}) {
    Reject([&] { ParseMuonServiceInterface(json); });
  }
  Reject([] { Type(R"({"callback":{"parameters":[{"callback":{"parameters":[],"returns":"void"}}],"returns":"void"}})"); });
  auto nested = std::string(R"({"callback":{"parameters":[{"array":"string"}],"returns":"void"}})");
  for (auto index = 0; index != 16; ++index) nested = "{\"array\":" + nested + "}";
  Type(nested);
  Reject([&] { Type("{\"array\":" + nested + "}"); });

  const auto i64 = Type("\"int64\"");
  const auto u64 = Type("\"uint64\"");
  assert(std::get<int64_t>(DecodeMuonServiceValue(i64, "\"-9223372036854775808\"").data) ==
      std::numeric_limits<int64_t>::min());
  assert(EncodeMuonServiceValue(u64, {std::numeric_limits<uint64_t>::max()}) ==
      "\"18446744073709551615\"");
  for (const auto* json : {"123", "\"01\"", "\"+1\"", "\"-0\"", "\"9223372036854775808\""})
    Reject([&] { DecodeMuonServiceValue(i64, json); });
  Reject([&] { DecodeMuonServiceValue(u64, "\"-1\""); });
  Reject([&] { EncodeMuonServiceValue(i64, {int32_t{1}}); });
  const auto i32 = Type("\"int32\"");
  assert(std::get<int32_t>(DecodeMuonServiceValue(i32, "1.0").data) == 1);
  for (const auto* json : {"true", "\"1\"", "1.1", "2147483648"})
    Reject([&] { DecodeMuonServiceValue(i32, json); });

  const auto f64 = Type("\"float64\"");
  for (const auto value : {-0.0, std::numeric_limits<double>::denorm_min(),
      std::numeric_limits<double>::max(), 1e-7, -3.25}) {
    const auto decoded = std::get<double>(DecodeMuonServiceValue(f64,
        EncodeMuonServiceValue(f64, {value})).data);
    assert(decoded == value && std::signbit(decoded) == std::signbit(value));
  }
  assert(std::get<double>(DecodeMuonServiceValue(f64, "\"1e-07\"").data) == 1e-7);
  assert(std::get<double>(DecodeMuonServiceValue(f64, "\"0e-999\"").data) == 0);
  for (const auto* json : {"1", "\"01\"", "\"+1\"", "\".5\"", "\"1.\"", "\"1e999\"", "\"1e-999\"", "\"nan\"", "\"0x1p0\""})
    Reject([&] { DecodeMuonServiceValue(f64, json); });
  Reject([&] { EncodeMuonServiceValue(f64, {std::numeric_limits<double>::infinity()}); });

  const auto bytes = Type("\"bytes\"");
  const auto binary = MuonServiceValue::Bytes{0, 127, 128, 255};
  assert(EncodeMuonServiceValue(bytes, {binary}) == "\"AH+A/w==\"");
  assert(std::get<MuonServiceValue::Bytes>(DecodeMuonServiceValue(bytes, "\"AH+A/w==\"").data) == binary);
  assert(std::get<MuonServiceValue::Bytes>(DecodeMuonServiceValue(bytes, "\"\"").data).empty());
  for (const auto* json : {"null", "\"A\"", "\"AB==\"", "\"AAB=\"", "\"====\"", "\"AA-_\"", "\"AA==AAAA\""})
    Reject([&] { DecodeMuonServiceValue(bytes, json); });
  const auto string = Type("\"string\"");
  const auto text = std::string("日本\0🙂", 11);
  assert(std::get<std::string>(DecodeMuonServiceValue(string,
      EncodeMuonServiceValue(string, {text})).data) == text);
  Reject([&] { DecodeMuonServiceValue(string, "\"\\ud800\""); });
  Reject([&] { EncodeMuonServiceValue(string, {std::string("\xed\xa0\x80")}); });
  const auto callback = Type(R"({"callback":{"parameters":[],"returns":"void"}})");
  assert(std::get<MuonServiceCallback>(DecodeMuonServiceValue(callback, "\"9\"").data).token == 9);
  Reject([&] { DecodeMuonServiceValue(callback, "\"0\""); });
  Reject([&] { EncodeMuonServiceValue(callback, {MuonServiceCallback{0}}); });
  const auto array = Type(R"({"array":"uint64"})");
  const auto values = DecodeMuonServiceValue(array, "[\"0\",\"18446744073709551615\"]");
  assert(EncodeMuonServiceValue(array, values) == "[\"0\",\"18446744073709551615\"]");
  Reject([&] { DecodeMuonServiceValue(array, "[0]"); });
  Reject([&] { EncodeMuonServiceValue(string, {std::string(1024 * 1024 + 1, 'a')}); });
  Reject([&] { EncodeMuonServiceValue(Type(R"({"array":"bytes"})"),
      {MuonServiceValue::Array{{MuonServiceValue::Bytes(600000)}, {MuonServiceValue::Bytes(600000)}}}); });
  const auto types = std::vector<MuonServiceType>{i64, bytes};
  const auto arguments = DecodeMuonServiceArguments(types, "[\"42\",\"AH+A/w==\"]");
  assert(EncodeMuonServiceArguments(types, arguments) == "[\"42\",\"AH+A/w==\"]");
  Reject([&] { DecodeMuonServiceArguments(types, "[\"42\"]"); });
  Reject([&] { EncodeMuonServiceArguments({bytes, bytes},
      {{MuonServiceValue::Bytes(600000)}, {MuonServiceValue::Bytes(600000)}}); });
  std::cout << "Service interface and portable JSON values passed\n";
}
