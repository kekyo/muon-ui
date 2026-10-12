/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "muon_json_helpers.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace muon_internal {

MuonJsonDocument::MuonJsonDocument() = default;

MuonJsonDocument::MuonJsonDocument(yyjson_doc* source)
    : value(source) {}

MuonJsonDocument::~MuonJsonDocument() {
  reset(nullptr);
}

MuonJsonDocument::MuonJsonDocument(MuonJsonDocument&& other) noexcept
    : value(other.value) {
  other.value = nullptr;
}

MuonJsonDocument& MuonJsonDocument::operator=(
    MuonJsonDocument&& other) noexcept {
  if (this != &other) {
    reset(other.value);
    other.value = nullptr;
  }
  return *this;
}

yyjson_doc* MuonJsonDocument::get() const {
  return value;
}

void MuonJsonDocument::reset(yyjson_doc* source) {
  if (value != nullptr) {
    yyjson_doc_free(value);
  }
  value = source;
}

MuonMutableJsonDocument::MuonMutableJsonDocument() = default;

MuonMutableJsonDocument::MuonMutableJsonDocument(yyjson_mut_doc* source)
    : value(source) {}

MuonMutableJsonDocument::~MuonMutableJsonDocument() {
  reset(nullptr);
}

MuonMutableJsonDocument::MuonMutableJsonDocument(
    MuonMutableJsonDocument&& other) noexcept
    : value(other.value) {
  other.value = nullptr;
}

MuonMutableJsonDocument& MuonMutableJsonDocument::operator=(
    MuonMutableJsonDocument&& other) noexcept {
  if (this != &other) {
    reset(other.value);
    other.value = nullptr;
  }
  return *this;
}

yyjson_mut_doc* MuonMutableJsonDocument::get() const {
  return value;
}

void MuonMutableJsonDocument::reset(yyjson_mut_doc* source) {
  if (value != nullptr) {
    yyjson_mut_doc_free(value);
  }
  value = source;
}

std::string ReadJsonString(yyjson_val* value) {
  return std::string(yyjson_get_str(value), yyjson_get_len(value));
}

static void AppendJsonHex(std::string* target, uint8_t value) {
  constexpr char kHex[] = "0123456789abcdef";
  target->push_back(kHex[(value >> 4) & 0x0f]);
  target->push_back(kHex[value & 0x0f]);
}

void AppendJsonString(std::string* target, std::string_view value) {
  target->push_back('"');
  for (const auto character : value) {
    const auto byte = static_cast<uint8_t>(character);
    switch (character) {
      case '"':
        *target += "\\\"";
        break;
      case '\\':
        *target += "\\\\";
        break;
      case '\b':
        *target += "\\b";
        break;
      case '\f':
        *target += "\\f";
        break;
      case '\n':
        *target += "\\n";
        break;
      case '\r':
        *target += "\\r";
        break;
      case '\t':
        *target += "\\t";
        break;
      default:
        if (byte < 0x20) {
          *target += "\\u00";
          AppendJsonHex(target, byte);
        } else {
          target->push_back(character);
        }
        break;
    }
  }
  target->push_back('"');
}

std::string CreateJsonStringArray(
    const std::vector<std::string>& values) {
  std::string json = "[";
  for (auto index = size_t{0}; index < values.size(); ++index) {
    if (index > 0) {
      json += ",";
    }
    AppendJsonString(&json, values[index]);
  }
  json += "]";
  return json;
}

std::string CreateJsonStringObject(
    const std::vector<std::pair<std::string, std::string>>& entries) {
  std::string json = "{";
  for (auto index = size_t{0}; index < entries.size(); ++index) {
    if (index > 0) {
      json += ",";
    }
    AppendJsonString(&json, entries[index].first);
    json += ":";
    AppendJsonString(&json, entries[index].second);
  }
  json += "}";
  return json;
}

bool ParseJsonObjectOptions(const char* options_json,
                            MuonJsonDocument* document,
                            yyjson_val** root,
                            std::string* error_message) {
  if (options_json == nullptr) {
    *error_message = "Options JSON is required";
    return false;
  }
  yyjson_read_err read_error = {};
  auto* raw_document = yyjson_read_opts(
      const_cast<char*>(options_json), std::strlen(options_json),
      YYJSON_READ_NOFLAG, nullptr, &read_error);
  if (raw_document == nullptr) {
    *error_message = "Options JSON is invalid";
    return false;
  }
  *document = MuonJsonDocument(raw_document);
  *root = yyjson_doc_get_root(document->get());
  if (!yyjson_is_obj(*root)) {
    *error_message = "Options JSON root must be an object";
    return false;
  }
  return true;
}

}  // namespace muon_internal
