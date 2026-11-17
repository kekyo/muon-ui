/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "services/muon_service_value.h"

#include <cmath>
#include <deque>
#include <stdexcept>
#include <string_view>
#include <type_traits>

struct ValueBudget {
  size_t bytes = 1024 * 1024;
  size_t nodes = 65536;
};

static void Require(bool condition) {
  if (!condition) throw std::invalid_argument("Invalid or excessive service value");
}

static void Enter(ValueBudget& budget, size_t depth) {
  Require(depth <= 16 && budget.nodes > 0);
  --budget.nodes;
}

static void Bytes(ValueBudget& budget, const void* data, size_t size) {
  Require(size <= budget.bytes && (size == 0 || data));
  budget.bytes -= size;
}

static void Utf8(std::string_view value) {
  for (size_t index = 0; index < value.size();) {
    const auto first = static_cast<uint8_t>(value[index++]);
    if (first < 0x80) continue;
    const auto length = first >= 0xc2 && first <= 0xdf ? 1 :
        first >= 0xe0 && first <= 0xef ? 2 : first >= 0xf0 && first <= 0xf4 ? 3 : 0;
    Require(length != 0 && value.size() - index >= static_cast<size_t>(length));
    uint32_t scalar = first & ((1u << (6 - length)) - 1);
    for (int part = 0; part < length; ++part) {
      const auto next = static_cast<uint8_t>(value[index++]);
      Require((next & 0xc0) == 0x80);
      scalar = (scalar << 6) | (next & 0x3f);
    }
    Require(scalar >= (length == 1 ? 0x80u : length == 2 ? 0x800u : 0x10000u) &&
        scalar <= 0x10ffff && !(scalar >= 0xd800 && scalar <= 0xdfff));
  }
}

static MuonServiceValue Copy(const muon_service_value& value, ValueBudget& budget, size_t depth) {
  Enter(budget, depth);
  const auto& data = value.data;
  switch (value.kind) {
    case MUON_SERVICE_VALUE_VOID: return {};
    case MUON_SERVICE_VALUE_BOOLEAN:
      Require(data.boolean_value <= 1);
      return {data.boolean_value != 0};
    case MUON_SERVICE_VALUE_INT32: return {data.int32_value};
    case MUON_SERVICE_VALUE_UINT32: return {data.uint32_value};
    case MUON_SERVICE_VALUE_INT64: return {data.int64_value};
    case MUON_SERVICE_VALUE_UINT64: return {data.uint64_value};
    case MUON_SERVICE_VALUE_FLOAT64:
      Require(std::isfinite(data.float64_value));
      return {data.float64_value};
    case MUON_SERVICE_VALUE_STRING: {
      const auto& text = data.string_value;
      Bytes(budget, text.data, text.size);
      const auto view = text.size ? std::string_view(text.data, text.size) : std::string_view{};
      Utf8(view);
      return {std::string(view)};
    }
    case MUON_SERVICE_VALUE_BYTES: {
      const auto& bytes = data.bytes_value;
      Bytes(budget, bytes.data, bytes.size);
      return {bytes.size ? MuonServiceValue::Bytes(bytes.data, bytes.data + bytes.size) : MuonServiceValue::Bytes{}};
    }
    case MUON_SERVICE_VALUE_ARRAY: {
      const auto& array = data.array_value;
      Require(depth < 16 && array.size <= budget.nodes && (array.size == 0 || array.data));
      auto result = MuonServiceValue::Array{};
      result.reserve(array.size);
      for (size_t index = 0; index < array.size; ++index)
        result.push_back(Copy(array.data[index], budget, depth + 1));
      return {std::move(result)};
    }
    case MUON_SERVICE_VALUE_CALLBACK:
      Require(data.callback_id != 0);
      return {MuonServiceCallback{data.callback_id}};
    default: throw std::invalid_argument("Unknown service value kind");
  }
}

MuonServiceValue CopyMuonServiceValue(const muon_service_value& value) {
  ValueBudget budget;
  return Copy(value, budget, 0);
}

struct MuonNativeServiceValuesImpl {
  std::vector<MuonServiceValue> owned;
  std::vector<muon_service_value> values;
  std::deque<std::vector<muon_service_value>> arrays;
};

static muon_service_value View(const MuonServiceValue& value,
    MuonNativeServiceValuesImpl& storage, ValueBudget& budget, size_t depth) {
  Enter(budget, depth);
  muon_service_value result{};
  std::visit([&](const auto& data) {
    using T = std::decay_t<decltype(data)>;
    if constexpr (std::is_same_v<T, std::monostate>) {
      result.kind = MUON_SERVICE_VALUE_VOID;
    } else if constexpr (std::is_same_v<T, bool>) {
      result.kind = MUON_SERVICE_VALUE_BOOLEAN; result.data.boolean_value = data;
    } else if constexpr (std::is_same_v<T, int32_t>) {
      result.kind = MUON_SERVICE_VALUE_INT32; result.data.int32_value = data;
    } else if constexpr (std::is_same_v<T, uint32_t>) {
      result.kind = MUON_SERVICE_VALUE_UINT32; result.data.uint32_value = data;
    } else if constexpr (std::is_same_v<T, int64_t>) {
      result.kind = MUON_SERVICE_VALUE_INT64; result.data.int64_value = data;
    } else if constexpr (std::is_same_v<T, uint64_t>) {
      result.kind = MUON_SERVICE_VALUE_UINT64; result.data.uint64_value = data;
    } else if constexpr (std::is_same_v<T, double>) {
      Require(std::isfinite(data));
      result.kind = MUON_SERVICE_VALUE_FLOAT64; result.data.float64_value = data;
    } else if constexpr (std::is_same_v<T, std::string>) {
      Bytes(budget, data.data(), data.size());
      Utf8(data);
      result.kind = MUON_SERVICE_VALUE_STRING;
      result.data.string_value = {data.data(), data.size()};
    } else if constexpr (std::is_same_v<T, MuonServiceValue::Bytes>) {
      Bytes(budget, data.data(), data.size());
      result.kind = MUON_SERVICE_VALUE_BYTES;
      result.data.bytes_value = {data.data(), data.size()};
    } else if constexpr (std::is_same_v<T, MuonServiceValue::Array>) {
      Require(depth < 16 && data.size() <= budget.nodes);
      auto& array = storage.arrays.emplace_back();
      array.reserve(data.size());
      for (const auto& child : data) array.push_back(View(child, storage, budget, depth + 1));
      result.kind = MUON_SERVICE_VALUE_ARRAY;
      result.data.array_value = {array.data(), array.size()};
    } else if constexpr (std::is_same_v<T, MuonServiceCallback>) {
      Require(data.token != 0);
      result.kind = MUON_SERVICE_VALUE_CALLBACK; result.data.callback_id = data.token;
    }
  }, value.data);
  return result;
}

MuonNativeServiceValues::MuonNativeServiceValues(std::vector<MuonServiceValue> values)
    : impl_(std::make_unique<MuonNativeServiceValuesImpl>()) {
  ValueBudget budget;
  Require(values.size() <= budget.nodes);
  impl_->owned = std::move(values);
  impl_->values.reserve(impl_->owned.size());
  for (const auto& value : impl_->owned) impl_->values.push_back(View(value, *impl_, budget, 0));
}

MuonNativeServiceValues::~MuonNativeServiceValues() = default;
MuonNativeServiceValues::MuonNativeServiceValues(MuonNativeServiceValues&&) noexcept = default;
MuonNativeServiceValues& MuonNativeServiceValues::operator=(MuonNativeServiceValues&&) noexcept = default;

std::span<const muon_service_value> MuonNativeServiceValues::Get() const {
  return impl_ ? std::span<const muon_service_value>(impl_->values) : std::span<const muon_service_value>{};
}
