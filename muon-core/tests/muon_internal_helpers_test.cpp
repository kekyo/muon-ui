/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "muon_json_helpers.h"
#include "muon_string_helpers.h"
#include "plugins/builtin/muon_builtin_completion.h"
#include "plugins/builtin/muon_builtin_environment_helpers.h"

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

struct CompletionCapture {
  bool called = false;
  bool has_result = false;
  bool bool_result = false;
  std::string string_result;
  std::string error;
};

static CompletionCapture* g_completion_capture = nullptr;

static void CaptureVoidCompletion(const void* result, const char* error) {
  if (g_completion_capture == nullptr) {
    return;
  }
  g_completion_capture->called = true;
  g_completion_capture->has_result = result != nullptr;
  if (error != nullptr) {
    g_completion_capture->error = error;
  } else {
    g_completion_capture->error.clear();
  }
}

static void CaptureBoolCompletion(const void* result, const char* error) {
  CaptureVoidCompletion(result, error);
  if (g_completion_capture == nullptr || result == nullptr) {
    return;
  }
  g_completion_capture->bool_result = *static_cast<const bool*>(result);
}

static void CaptureStringCompletion(const void* result, const char* error) {
  CaptureVoidCompletion(result, error);
  if (g_completion_capture == nullptr || result == nullptr) {
    return;
  }
  const auto* string_result = *static_cast<const char* const*>(result);
  if (string_result != nullptr) {
    g_completion_capture->string_result = string_result;
  }
}

static bool TestCompletionHelpers() {
  CompletionCapture capture;
  g_completion_capture = &capture;

  muon_internal::CompleteMuonVoid(CaptureVoidCompletion);
  if (!Expect(capture.called, "void completion was not called") ||
      !Expect(!capture.has_result, "void completion produced a result") ||
      !Expect(capture.error.empty(), "void completion produced an error")) {
    g_completion_capture = nullptr;
    return false;
  }

  capture = {};
  muon_internal::CompleteMuonBool(CaptureBoolCompletion, true);
  if (!Expect(capture.called, "bool completion was not called") ||
      !Expect(capture.has_result, "bool completion did not produce result") ||
      !Expect(capture.bool_result, "bool completion result changed") ||
      !Expect(capture.error.empty(), "bool completion produced an error")) {
    g_completion_capture = nullptr;
    return false;
  }

  capture = {};
  muon_internal::CompleteMuonString(CaptureStringCompletion, "muon");
  if (!Expect(capture.called, "string completion was not called") ||
      !Expect(capture.has_result, "string completion did not produce result") ||
      !Expect(capture.string_result == "muon",
              "string completion result changed") ||
      !Expect(capture.error.empty(), "string completion produced an error")) {
    g_completion_capture = nullptr;
    return false;
  }

  capture = {};
  muon_internal::CompleteMuonError(CaptureVoidCompletion, "boom");
  if (!Expect(capture.called, "error completion was not called") ||
      !Expect(!capture.has_result, "error completion produced a result") ||
      !Expect(capture.error == "boom", "error completion message changed")) {
    g_completion_capture = nullptr;
    return false;
  }

  muon_internal::CompleteMuonVoid(nullptr);
  muon_internal::CompleteMuonBool(nullptr, false);
  muon_internal::CompleteMuonString(nullptr, "ignored");
  muon_internal::CompleteMuonError(nullptr, "ignored");
  g_completion_capture = nullptr;
  return true;
}

static bool TestEnvironmentHelpers() {
#if defined(_WIN32)
  return true;
#else
  constexpr char kKey[] = "MUON_INTERNAL_HELPERS_TEST_ENTRY";
  constexpr char kValue[] = "muon-value";
  if (setenv(kKey, kValue, 1) != 0) {
    return Expect(false, "failed to set test environment variable");
  }

  const auto entries = muon_internal::GetMuonEnvironmentEntries();
  auto found = false;
  for (const auto& entry : entries) {
    if (entry.first == kKey) {
      found = true;
      if (!Expect(entry.second == kValue,
                  "environment helper value changed")) {
        unsetenv(kKey);
        return false;
      }
      break;
    }
  }
  unsetenv(kKey);
  return Expect(found, "environment helper did not enumerate test variable");
#endif
}

int main() {
  return TestStringHelpers() && TestJsonHelpers() && TestCompletionHelpers() &&
                 TestEnvironmentHelpers()
             ? 0
             : 1;
}
