/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "services/muon_service_interface.h"

#include <yyjson.h>

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>

using JsonDocument = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>;
using MutableJsonDocument = std::unique_ptr<yyjson_mut_doc, decltype(&yyjson_mut_doc_free)>;

static void Require(bool condition) {
  if (!condition) throw std::invalid_argument("Invalid or excessive service interface/value");
}

static JsonDocument Read(const std::string& json) {
  Require(!json.empty() && json.size() <= 8 * 1024 * 1024);
  auto doc = JsonDocument(yyjson_read(json.data(), json.size(), 0), &yyjson_doc_free);
  Require(static_cast<bool>(doc));
  return doc;
}

static std::string Text(yyjson_val* value) {
  Require(yyjson_is_str(value));
  return {yyjson_get_str(value), yyjson_get_len(value)};
}

static void Fields(yyjson_val* value, std::initializer_list<std::string_view> fields) {
  Require(yyjson_is_obj(value) && yyjson_obj_size(value) == fields.size());
  auto remaining = std::set<std::string_view>(fields);
  auto iter = yyjson_obj_iter_with(value);
  while (auto* key = yyjson_obj_iter_next(&iter)) {
    Require(remaining.erase(std::string_view(yyjson_get_str(key), yyjson_get_len(key))) == 1);
  }
}

static bool Letter(char value) {
  return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
}

static std::string Name(yyjson_val* value) {
  auto name = Text(value);
  Require(!name.empty() && name.size() <= 128 && Letter(name.front()));
  for (const auto ch : name) {
    Require(Letter(ch) || (ch >= '0' && ch <= '9') || ch == '_' || ch == '.' || ch == '-');
  }
  return name;
}

static MuonServiceType ParseType(yyjson_val* value, bool allow_void, bool callbacks, size_t depth);

static std::vector<MuonServiceType> Parameters(yyjson_val* value, bool callbacks) {
  Require(yyjson_is_arr(value) && yyjson_arr_size(value) <= 32);
  auto result = std::vector<MuonServiceType>{};
  auto iter = yyjson_arr_iter_with(value);
  while (auto* item = yyjson_arr_iter_next(&iter)) result.push_back(ParseType(item, false, callbacks, 0));
  return result;
}

static MuonServiceType ParseType(yyjson_val* value, bool allow_void, bool callbacks, size_t depth) {
  Require(depth <= 16);
  if (yyjson_is_str(value)) {
    static const auto primitives = std::map<std::string, muon_service_value_kind>{
      {"void", MUON_SERVICE_VALUE_VOID}, {"boolean", MUON_SERVICE_VALUE_BOOLEAN},
      {"int32", MUON_SERVICE_VALUE_INT32}, {"uint32", MUON_SERVICE_VALUE_UINT32},
      {"int64", MUON_SERVICE_VALUE_INT64}, {"uint64", MUON_SERVICE_VALUE_UINT64},
      {"float64", MUON_SERVICE_VALUE_FLOAT64}, {"string", MUON_SERVICE_VALUE_STRING},
      {"bytes", MUON_SERVICE_VALUE_BYTES}};
    const auto found = primitives.find(Text(value));
    Require(found != primitives.end());
    Require(allow_void || found->second != MUON_SERVICE_VALUE_VOID);
    return {found->second, {}, {}};
  }
  if (auto* element = yyjson_obj_get(value, "array")) {
    Fields(value, {"array"});
    Require(depth < 16);
    return {MUON_SERVICE_VALUE_ARRAY,
      std::make_shared<const MuonServiceType>(ParseType(element, false, callbacks, depth + 1)), {}};
  }
  Require(callbacks);
  Fields(value, {"callback"});
  auto* signature = yyjson_obj_get(value, "callback");
  Fields(signature, {"parameters", "returns"});
  return {MUON_SERVICE_VALUE_CALLBACK, {}, std::make_shared<const MuonServiceSignature>(
      MuonServiceSignature{Parameters(yyjson_obj_get(signature, "parameters"), false),
        ParseType(yyjson_obj_get(signature, "returns"), true, false, 0)})};
}

MuonServiceInterface ParseMuonServiceInterface(const std::string& json) {
  Require(json.size() <= 1024 * 1024);
  const auto doc = Read(json);
  auto* root = yyjson_doc_get_root(doc.get());
  Fields(root, {"schemaVersion", "methods", "events"});
  auto* version = yyjson_obj_get(root, "schemaVersion");
  Require(yyjson_is_num(version) && yyjson_get_num(version) == 1);
  auto result = MuonServiceInterface{};
  auto* methods = yyjson_obj_get(root, "methods");
  Require(yyjson_is_obj(methods) && yyjson_obj_size(methods) <= 256);
  auto method_iter = yyjson_obj_iter_with(methods);
  while (auto* key = yyjson_obj_iter_next(&method_iter)) {
    const auto name = Name(key);
    auto* value = yyjson_obj_iter_get_val(key);
    Fields(value, {"parameters", "returns", "errors"});
    auto method = MuonServiceMethod{
      {Parameters(yyjson_obj_get(value, "parameters"), true),
        ParseType(yyjson_obj_get(value, "returns"), true, false, 0)}, {}};
    auto* errors = yyjson_obj_get(value, "errors");
    Require(yyjson_is_arr(errors) && yyjson_arr_size(errors) <= 64);
    auto unique = std::set<std::string>{};
    auto error_iter = yyjson_arr_iter_with(errors);
    while (auto* error = yyjson_arr_iter_next(&error_iter)) {
      auto code = Name(error);
      Require(unique.insert(code).second);
      method.errors.push_back(std::move(code));
    }
    Require(result.methods.emplace(name, std::move(method)).second);
  }
  auto* events = yyjson_obj_get(root, "events");
  Require(yyjson_is_obj(events) && yyjson_obj_size(events) <= 256);
  auto event_iter = yyjson_obj_iter_with(events);
  while (auto* key = yyjson_obj_iter_next(&event_iter)) {
    Require(result.events.emplace(Name(key), ParseType(yyjson_obj_iter_get_val(key), false, false, 0)).second);
  }
  return result;
}

struct ValueBudget {
  size_t bytes = 1024 * 1024;
  size_t nodes = 65536;
  void Spend(size_t size) { Require(size <= bytes); bytes -= size; }
  void Enter(size_t depth) { Require(depth <= 16 && nodes > 0); --nodes; }
};

template <typename Integer>
static Integer Decimal(yyjson_val* value) {
  const auto text = Text(value);
  Require(!text.empty() && text.size() <= 20);
  auto result = Integer{};
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
  Require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
      std::to_string(result) == text);
  return result;
}

static double Float(yyjson_val* value) {
  const auto text = Text(value);
  Require(!text.empty() && text.size() <= 32);
  // Checking the JSON number grammar also rejects from_chars extensions such
  // as nan, inf, a missing integer/fraction part, and leading zeroes.
  const auto number_doc = Read(text);
  Require(yyjson_is_num(yyjson_doc_get_root(number_doc.get())));
  Require(text.front() != ' ' && text.back() != ' ');
  auto result = double{};
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
  Require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && std::isfinite(result));
  return result;
}

static constexpr auto kBase64 = std::string_view("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/");

static std::string Base64(const MuonServiceValue::Bytes& bytes) {
  auto result = std::string{};
  result.reserve(((bytes.size() + 2) / 3) * 4);
  for (size_t index = 0; index < bytes.size(); index += 3) {
    const auto a = bytes[index];
    const auto b = index + 1 < bytes.size() ? bytes[index + 1] : 0;
    const auto c = index + 2 < bytes.size() ? bytes[index + 2] : 0;
    result.push_back(kBase64[a >> 2]);
    result.push_back(kBase64[((a & 3) << 4) | (b >> 4)]);
    result.push_back(index + 1 < bytes.size() ? kBase64[((b & 15) << 2) | (c >> 6)] : '=');
    result.push_back(index + 2 < bytes.size() ? kBase64[c & 63] : '=');
  }
  return result;
}

static MuonServiceValue::Bytes Unbase64(yyjson_val* value, ValueBudget& budget) {
  const auto text = Text(value);
  Require(text.size() % 4 == 0);
  const auto padding = text.ends_with("==") ? 2u : text.ends_with('=') ? 1u : 0u;
  Require(text.size() / 4 * 3 >= padding);
  const auto size = text.size() / 4 * 3 - padding;
  budget.Spend(size);
  auto result = MuonServiceValue::Bytes{};
  result.reserve(size);
  for (size_t index = 0; index < text.size(); index += 4) {
    const auto last = index + 4 == text.size();
    const auto a = kBase64.find(text[index]);
    const auto b = kBase64.find(text[index + 1]);
    const auto c = last && padding == 2 ? 0 : kBase64.find(text[index + 2]);
    const auto d = last && padding ? 0 : kBase64.find(text[index + 3]);
    Require(a < 64 && b < 64 && c < 64 && d < 64);
    Require(!(last && padding == 2 && (b & 15)) && !(last && padding == 1 && (c & 3)));
    result.push_back(static_cast<uint8_t>((a << 2) | (b >> 4)));
    if (result.size() < size) result.push_back(static_cast<uint8_t>((b << 4) | (c >> 2)));
    if (result.size() < size) result.push_back(static_cast<uint8_t>((c << 6) | d));
  }
  return result;
}

static MuonServiceValue Decode(const MuonServiceType& type, yyjson_val* value,
                               ValueBudget& budget, size_t depth) {
  budget.Enter(depth);
  switch (type.kind) {
    case MUON_SERVICE_VALUE_VOID: Require(yyjson_is_null(value)); return {std::monostate{}};
    case MUON_SERVICE_VALUE_BOOLEAN: Require(yyjson_is_bool(value)); return {yyjson_get_bool(value)};
    case MUON_SERVICE_VALUE_INT32:
    case MUON_SERVICE_VALUE_UINT32: {
      Require(yyjson_is_num(value));
      const auto number = yyjson_get_num(value);
      const auto signed_value = type.kind == MUON_SERVICE_VALUE_INT32;
      Require(std::isfinite(number) && std::trunc(number) == number &&
          number >= (signed_value ? -2147483648.0 : 0) &&
          number <= (signed_value ? 2147483647.0 : 4294967295.0));
      return signed_value ? MuonServiceValue{static_cast<int32_t>(number)} :
          MuonServiceValue{static_cast<uint32_t>(number)};
    }
    case MUON_SERVICE_VALUE_INT64: return {Decimal<int64_t>(value)};
    case MUON_SERVICE_VALUE_UINT64: return {Decimal<uint64_t>(value)};
    case MUON_SERVICE_VALUE_FLOAT64: return {Float(value)};
    case MUON_SERVICE_VALUE_STRING: {
      auto text = Text(value);
      budget.Spend(text.size());
      return {std::move(text)};
    }
    case MUON_SERVICE_VALUE_BYTES: return {Unbase64(value, budget)};
    case MUON_SERVICE_VALUE_CALLBACK: {
      const auto token = Decimal<uint64_t>(value);
      Require(token != 0);
      return {MuonServiceCallback{token}};
    }
    case MUON_SERVICE_VALUE_ARRAY: {
      Require(depth < 16 && yyjson_is_arr(value) && yyjson_arr_size(value) <= budget.nodes);
      auto result = MuonServiceValue::Array{};
      result.reserve(yyjson_arr_size(value));
      auto iter = yyjson_arr_iter_with(value);
      while (auto* item = yyjson_arr_iter_next(&iter)) result.push_back(Decode(*type.element, item, budget, depth + 1));
      return {std::move(result)};
    }
  }
  throw std::invalid_argument("Unknown service value type");
}

template <typename Type>
static const Type& Get(const MuonServiceValue& value) {
  const auto* data = std::get_if<Type>(&value.data);
  Require(data != nullptr);
  return *data;
}

static yyjson_mut_val* JsonString(yyjson_mut_doc* doc, const std::string& text) {
  return yyjson_mut_strncpy(doc, text.data(), text.size());
}

static yyjson_mut_val* Encode(yyjson_mut_doc* doc, const MuonServiceType& type,
    const MuonServiceValue& value, ValueBudget& budget, size_t depth) {
  budget.Enter(depth);
  switch (type.kind) {
    case MUON_SERVICE_VALUE_VOID: Get<std::monostate>(value); return yyjson_mut_null(doc);
    case MUON_SERVICE_VALUE_BOOLEAN: return yyjson_mut_bool(doc, Get<bool>(value));
    case MUON_SERVICE_VALUE_INT32: return yyjson_mut_sint(doc, Get<int32_t>(value));
    case MUON_SERVICE_VALUE_UINT32: return yyjson_mut_uint(doc, Get<uint32_t>(value));
    case MUON_SERVICE_VALUE_INT64: return JsonString(doc, std::to_string(Get<int64_t>(value)));
    case MUON_SERVICE_VALUE_UINT64: return JsonString(doc, std::to_string(Get<uint64_t>(value)));
    case MUON_SERVICE_VALUE_FLOAT64: {
      const auto number = Get<double>(value);
      Require(std::isfinite(number));
      char text[128];
      const auto encoded = std::to_chars(text, text + sizeof(text), number);
      Require(encoded.ec == std::errc{});
      return yyjson_mut_strncpy(doc, text, static_cast<size_t>(encoded.ptr - text));
    }
    case MUON_SERVICE_VALUE_STRING: {
      const auto& text = Get<std::string>(value);
      budget.Spend(text.size());
      return JsonString(doc, text);
    }
    case MUON_SERVICE_VALUE_BYTES: {
      const auto& bytes = Get<MuonServiceValue::Bytes>(value);
      budget.Spend(bytes.size());
      return JsonString(doc, Base64(bytes));
    }
    case MUON_SERVICE_VALUE_CALLBACK: {
      const auto token = Get<MuonServiceCallback>(value).token;
      Require(token != 0);
      return JsonString(doc, std::to_string(token));
    }
    case MUON_SERVICE_VALUE_ARRAY: {
      const auto& array = Get<MuonServiceValue::Array>(value);
      Require(depth < 16 && array.size() <= budget.nodes);
      auto* result = yyjson_mut_arr(doc);
      if (!result) throw std::bad_alloc();
      for (const auto& item : array) {
        if (!yyjson_mut_arr_append(result, Encode(doc, *type.element, item, budget, depth + 1))) throw std::bad_alloc();
      }
      return result;
    }
  }
  throw std::invalid_argument("Unknown service value type");
}

static MutableJsonDocument NewDocument() {
  auto doc = MutableJsonDocument(yyjson_mut_doc_new(nullptr), &yyjson_mut_doc_free);
  if (!doc) throw std::bad_alloc();
  return doc;
}

static std::string Write(yyjson_mut_doc* doc, yyjson_mut_val* root) {
  if (!root) throw std::bad_alloc();
  yyjson_mut_doc_set_root(doc, root);
  auto size = size_t{};
  const auto text = std::unique_ptr<char, decltype(&std::free)>(yyjson_mut_write(doc, 0, &size), &std::free);
  Require(static_cast<bool>(text));
  return {text.get(), size};
}

MuonServiceValue DecodeMuonServiceValue(const MuonServiceType& type, const std::string& json) {
  const auto doc = Read(json);
  auto budget = ValueBudget{};
  return Decode(type, yyjson_doc_get_root(doc.get()), budget, 0);
}

std::string EncodeMuonServiceValue(const MuonServiceType& type, const MuonServiceValue& value) {
  const auto doc = NewDocument();
  auto budget = ValueBudget{};
  return Write(doc.get(), Encode(doc.get(), type, value, budget, 0));
}

std::vector<MuonServiceValue> DecodeMuonServiceArguments(
    const std::vector<MuonServiceType>& types, const std::string& json) {
  const auto doc = Read(json);
  auto* root = yyjson_doc_get_root(doc.get());
  Require(types.size() <= 32 && yyjson_is_arr(root) && yyjson_arr_size(root) == types.size());
  auto budget = ValueBudget{};
  auto result = std::vector<MuonServiceValue>{};
  auto iter = yyjson_arr_iter_with(root);
  for (const auto& type : types) result.push_back(Decode(type, yyjson_arr_iter_next(&iter), budget, 0));
  return result;
}

std::string EncodeMuonServiceArguments(const std::vector<MuonServiceType>& types,
                                      const std::vector<MuonServiceValue>& values) {
  Require(types.size() <= 32 && types.size() == values.size());
  const auto doc = NewDocument();
  auto* root = yyjson_mut_arr(doc.get());
  if (!root) throw std::bad_alloc();
  auto budget = ValueBudget{};
  for (size_t index = 0; index < values.size(); ++index) {
    if (!yyjson_mut_arr_append(root, Encode(doc.get(), types[index], values[index], budget, 0))) throw std::bad_alloc();
  }
  return Write(doc.get(), root);
}
