/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "muon_json_helpers.h"
#include "muon_string_helpers.h"

#include "yyjson.h"

#include <cstdlib>
#include <iostream>
#include <string>

static bool Expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << "\n";
    return false;
  }
  return true;
}

static bool ExpectJsonStringValue(const std::string& json,
                                  const std::string& expected) {
  auto mutable_json = json;
  auto document = muon_internal::MuonJsonDocument(
      yyjson_read_opts(mutable_json.data(), mutable_json.size(),
                       YYJSON_READ_NOFLAG, nullptr, nullptr));
  auto* root = yyjson_doc_get_root(document.get());
  return Expect(yyjson_is_str(root), "json value is not a string") &&
         Expect(muon_internal::ReadJsonString(root) == expected,
                "json string value changed");
}

static bool TestStringHelpers() {
  return Expect(muon_internal::IsAsciiSpace(' '), "space was not detected") &&
         Expect(muon_internal::IsAsciiSpace('\n'),
                "newline was not detected") &&
         Expect(!muon_internal::IsAsciiSpace('x'),
                "non-space was detected as space") &&
         Expect(muon_internal::TrimAscii(" \tMuon\n") == "Muon",
                "ASCII trim result changed") &&
         Expect(muon_internal::ToLowerAscii("MuOn-ABC123") ==
                    "muon-abc123",
                "ASCII lower result changed") &&
         Expect(muon_internal::IsAsciiHexDigit('f'),
                "lower hex digit was not detected") &&
         Expect(muon_internal::IsAsciiHexDigit('A'),
                "upper hex digit was not detected") &&
         Expect(!muon_internal::IsAsciiHexDigit('g'),
                "non-hex digit was detected") &&
         Expect(muon_internal::DecodeAsciiHexByte('A', 'f') == 0xaf,
                "hex byte decode result changed");
}

static bool TestJsonHelpers() {
  std::string escaped;
  muon_internal::AppendJsonString(&escaped,
                                  "line\nquote\"slash\\control\x01");
  if (!ExpectJsonStringValue(escaped, "line\nquote\"slash\\control\x01")) {
    return false;
  }

  const auto array_json = muon_internal::CreateJsonStringArray(
      {"alpha", "quote\"", "line\nfeed"});
  auto mutable_array = array_json;
  auto array_document = muon_internal::MuonJsonDocument(
      yyjson_read_opts(mutable_array.data(), mutable_array.size(),
                       YYJSON_READ_NOFLAG, nullptr, nullptr));
  auto* array_root = yyjson_doc_get_root(array_document.get());
  if (!Expect(yyjson_is_arr(array_root), "json array was not valid") ||
      !Expect(yyjson_arr_size(array_root) == 3,
              "json array size changed") ||
      !Expect(muon_internal::ReadJsonString(yyjson_arr_get(array_root, 1)) ==
                  "quote\"",
              "json array entry changed")) {
    return false;
  }

  const auto object_json = muon_internal::CreateJsonStringObject(
      {{"name", "muon"}, {"path", "a\\b"}});
  auto mutable_object = object_json;
  auto object_document = muon_internal::MuonJsonDocument(
      yyjson_read_opts(mutable_object.data(), mutable_object.size(),
                       YYJSON_READ_NOFLAG, nullptr, nullptr));
  auto* object_root = yyjson_doc_get_root(object_document.get());
  if (!Expect(yyjson_is_obj(object_root), "json object was not valid") ||
      !Expect(muon_internal::ReadJsonString(
                  yyjson_obj_get(object_root, "path")) == "a\\b",
              "json object value changed")) {
    return false;
  }

  auto moved_document = std::move(object_document);
  return Expect(object_document.get() == nullptr,
                "moved-from JSON document still owns data") &&
         Expect(yyjson_doc_get_root(moved_document.get()) != nullptr,
                "moved JSON document lost data");
}

int main() {
  return TestStringHelpers() && TestJsonHelpers() ? 0 : 1;
}
