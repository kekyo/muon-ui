/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "plugins/muon_cef_rpc.h"

#include "plugins/muon_cef_plugin_metadata.h"
#include "plugins/muon_js_bridge.h"

#include "include/cef_shared_process_message_builder.h"
#include "include/cef_task.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <system_error>
#include <utility>

static constexpr char kMuonFunctionKindKey[] = "kind";
static constexpr char kMuonFunctionKindPluginProxy[] = "plugin_proxy";
static constexpr char kMuonFunctionContextIdKey[] = "context_id";
static constexpr char kMuonFunctionIdKey[] = "function_id";
static constexpr char kMuonFunctionProxyIdKey[] = "proxy_id";
static constexpr char kMuonFunctionLeaseTokenKey[] = "lease_token";
static constexpr char kMuonFunctionTypeKey[] = "type_key";
static constexpr char kMuonFunctionTypeMetadataKey[] = "type";

class MuonCefRpcTask final : public CefTask {
 public:
  explicit MuonCefRpcTask(std::function<void()> task)
      : task_(std::move(task)) {}

  void Execute() override {
    if (task_) {
      task_();
    }
  }

 private:
  std::function<void()> task_;

  IMPLEMENT_REFCOUNTING(MuonCefRpcTask);
  DISALLOW_COPY_AND_ASSIGN(MuonCefRpcTask);
};

class MuonCefSharedMemoryStorage final : public MuonRpcBufferStorage {
 public:
  explicit MuonCefSharedMemoryStorage(
      std::shared_ptr<MuonSharedBufferPayload> payload)
      : payload_(std::move(payload)) {}

  void* GetData() override {
    return payload_ && payload_->region ? payload_->region->Memory() : nullptr;
  }

  const void* GetData() const override {
    return payload_ && payload_->region ? payload_->region->Memory() : nullptr;
  }

  size_t GetSize() const override {
    return payload_ && payload_->region ? payload_->region->Size() : 0;
  }

 private:
  std::shared_ptr<MuonSharedBufferPayload> payload_;
};

class MuonCefResultBufferStorage final : public MuonRpcBufferStorage {
 public:
  MuonCefResultBufferStorage(
      CefRefPtr<CefSharedProcessMessageBuilder> builder,
      size_t data_size)
      : builder_(builder), data_size_(data_size) {}

  void* GetData() override {
    if (!builder_ || !builder_->IsValid() || builder_->Memory() == nullptr) {
      return nullptr;
    }
    return static_cast<uint8_t*>(builder_->Memory()) +
           GetMuonSharedBufferSingleEntryDataOffset();
  }

  const void* GetData() const override {
    if (!builder_ || !builder_->IsValid() || builder_->Memory() == nullptr) {
      return nullptr;
    }
    return static_cast<const uint8_t*>(builder_->Memory()) +
           GetMuonSharedBufferSingleEntryDataOffset();
  }

  size_t GetSize() const override { return data_size_; }

  bool Build(int call_id,
             int context_id,
             size_t value_index,
             size_t data_offset,
             size_t data_size,
             MuonCreatedSharedBufferMessage* created,
             std::string* error_message) {
    if (created == nullptr || error_message == nullptr || !builder_ ||
        !builder_->IsValid() || builder_->Memory() == nullptr ||
        data_offset > data_size_ || data_size > data_size_ - data_offset) {
      if (error_message != nullptr) {
        *error_message = "Shared buffer allocation is no longer valid";
      }
      return false;
    }
    MuonSharedBufferEntry entry;
    entry.value_index = value_index;
    entry.offset = GetMuonSharedBufferSingleEntryDataOffset() + data_offset;
    entry.size = data_size;
    created->entries = {entry};
    if (!WriteMuonSharedBufferPayloadHeader(
            builder_->Memory(), builder_->Size(), call_id, context_id,
            created->entries, error_message)) {
      return false;
    }
    created->message = builder_->Build();
    if (!created->message) {
      *error_message = "Failed to build shared buffer payload";
      return false;
    }
    return true;
  }

 private:
  CefRefPtr<CefSharedProcessMessageBuilder> builder_;
  size_t data_size_ = 0;
};

struct MuonCefRpcBridgeImpl {
  MuonCefRpcBridge::FrameResolver frame_resolver;
  std::map<const MuonRpcBufferStorage*,
           std::weak_ptr<MuonCefResultBufferStorage>>
      result_buffers;
};

static bool GetMuonNumericValue(CefRefPtr<CefListValue> list,
                                size_t index,
                                double* value) {
  if (!list || value == nullptr) {
    return false;
  }
  const auto type = list->GetType(index);
  if (type == VTYPE_INT) {
    *value = static_cast<double>(list->GetInt(index));
    return true;
  }
  if (type == VTYPE_DOUBLE) {
    *value = list->GetDouble(index);
    return true;
  }
  return false;
}

static bool GetMuonPointerValue(CefRefPtr<CefListValue> list,
                                size_t index,
                                uintptr_t* value) {
  if (!list || value == nullptr) {
    return false;
  }
  if (list->GetType(index) == VTYPE_NULL) {
    *value = 0;
    return true;
  }
  auto number = 0.0;
  if (!GetMuonNumericValue(list, index, &number) ||
      !std::isfinite(number) || std::trunc(number) != number ||
      number < 0.0 ||
      number >= std::ldexp(1.0, std::numeric_limits<uintptr_t>::digits)) {
    return false;
  }
  *value = static_cast<uintptr_t>(number);
  return true;
}

static bool ParseMuonInt64(const std::string& source, int64_t* value) {
  if (value == nullptr || source.empty()) {
    return false;
  }
  auto parsed = int64_t{0};
  const auto begin = source.data();
  const auto end = begin + source.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc() || result.ptr != end) {
    return false;
  }
  *value = parsed;
  return true;
}

static bool ParseMuonUInt64(const std::string& source, uint64_t* value) {
  if (value == nullptr || source.empty()) {
    return false;
  }
  auto parsed = uint64_t{0};
  const auto begin = source.data();
  const auto end = begin + source.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc() || result.ptr != end) {
    return false;
  }
  *value = parsed;
  return true;
}

static bool DecodeMuonFunctionValue(
    const MuonRpcOwner& owner,
    const MuonTypeMetadata& expected_type,
    CefRefPtr<CefDictionaryValue> encoded,
    MuonRpcValue* value,
    std::string* error_message) {
  if (!encoded || value == nullptr || error_message == nullptr) {
    return false;
  }
  value->function.type = expected_type;
  const auto is_plugin_proxy =
      encoded->HasKey(kMuonFunctionProxyIdKey) ||
      (encoded->HasKey(kMuonFunctionKindKey) &&
       encoded->GetString(kMuonFunctionKindKey).ToString() ==
           kMuonFunctionKindPluginProxy);
  if (is_plugin_proxy) {
    if (encoded->GetType(kMuonFunctionKindKey) != VTYPE_STRING ||
        encoded->GetString(kMuonFunctionKindKey).ToString() !=
            kMuonFunctionKindPluginProxy ||
        encoded->GetType(kMuonFunctionProxyIdKey) != VTYPE_INT ||
        encoded->GetInt(kMuonFunctionProxyIdKey) <= 0 ||
        encoded->GetType(kMuonFunctionLeaseTokenKey) != VTYPE_STRING ||
        encoded->GetString(kMuonFunctionLeaseTokenKey).ToString().empty() ||
        encoded->GetType(kMuonFunctionTypeKey) != VTYPE_STRING ||
        encoded->GetString(kMuonFunctionTypeKey).ToString() !=
            CreateMuonTypeCanonicalKey(expected_type)) {
      *error_message = "Invalid plugin function proxy";
      return false;
    }
    value->function.kind = MuonRpcFunctionKind::PluginProxy;
    value->function.proxy_id =
        static_cast<uint32_t>(encoded->GetInt(kMuonFunctionProxyIdKey));
    value->function.lease_token =
        encoded->GetString(kMuonFunctionLeaseTokenKey).ToString();
    return true;
  }

  if (encoded->GetType(kMuonFunctionContextIdKey) != VTYPE_INT ||
      encoded->GetInt(kMuonFunctionContextIdKey) != owner.context_id ||
      encoded->GetType(kMuonFunctionIdKey) != VTYPE_INT ||
      encoded->GetInt(kMuonFunctionIdKey) <= 0 ||
      encoded->GetType(kMuonFunctionTypeKey) != VTYPE_STRING ||
      encoded->GetString(kMuonFunctionTypeKey).ToString() !=
          CreateMuonTypeCanonicalKey(expected_type)) {
    *error_message = "Invalid function argument";
    return false;
  }
  value->function.kind = MuonRpcFunctionKind::RendererSource;
  value->function.renderer_context_id = owner.context_id;
  value->function.function_id = encoded->GetInt(kMuonFunctionIdKey);
  return true;
}

static bool DecodeMuonValue(
    const MuonRpcOwner& owner,
    const MuonTypeMetadata& expected_type,
    CefRefPtr<CefListValue> encoded_values,
    size_t index,
    const std::shared_ptr<MuonSharedBufferPayload>& shared_payload,
    const std::shared_ptr<MuonRpcBufferStorage>& shared_storage,
    bool allow_void,
    MuonRpcValue* value,
    std::string* error_message) {
  if (!encoded_values || value == nullptr || error_message == nullptr) {
    return false;
  }
  *value = MuonRpcValue{};
  value->type = expected_type;
  switch (expected_type.type) {
    case MUON_TYPE_VOID:
      if (!allow_void) {
        *error_message = "Void arguments are not supported";
        return false;
      }
      return true;
    case MUON_TYPE_BOOL:
      if (encoded_values->GetType(index) != VTYPE_BOOL) {
        *error_message = "Invalid bool argument";
        return false;
      }
      value->bool_value = encoded_values->GetBool(index);
      return true;
    case MUON_TYPE_I8: {
      auto number = 0.0;
      if (!GetMuonNumericValue(encoded_values, index, &number) ||
          !std::isfinite(number) || std::trunc(number) != number ||
          number < static_cast<double>(std::numeric_limits<int8_t>::min()) ||
          number > static_cast<double>(std::numeric_limits<int8_t>::max())) {
        *error_message = "Invalid i8 argument";
        return false;
      }
      value->i8_value = static_cast<int8_t>(number);
      return true;
    }
    case MUON_TYPE_U8: {
      auto number = 0.0;
      if (!GetMuonNumericValue(encoded_values, index, &number) ||
          !std::isfinite(number) || std::trunc(number) != number ||
          number < 0.0 ||
          number > static_cast<double>(std::numeric_limits<uint8_t>::max())) {
        *error_message = "Invalid u8 argument";
        return false;
      }
      value->u8_value = static_cast<uint8_t>(number);
      return true;
    }
    case MUON_TYPE_I16: {
      auto number = 0.0;
      if (!GetMuonNumericValue(encoded_values, index, &number) ||
          !std::isfinite(number) || std::trunc(number) != number ||
          number < static_cast<double>(std::numeric_limits<int16_t>::min()) ||
          number > static_cast<double>(std::numeric_limits<int16_t>::max())) {
        *error_message = "Invalid i16 argument";
        return false;
      }
      value->i16_value = static_cast<int16_t>(number);
      return true;
    }
    case MUON_TYPE_U16: {
      auto number = 0.0;
      if (!GetMuonNumericValue(encoded_values, index, &number) ||
          !std::isfinite(number) || std::trunc(number) != number ||
          number < 0.0 ||
          number >
              static_cast<double>(std::numeric_limits<uint16_t>::max())) {
        *error_message = "Invalid u16 argument";
        return false;
      }
      value->u16_value = static_cast<uint16_t>(number);
      return true;
    }
    case MUON_TYPE_I32: {
      auto number = 0.0;
      if (!GetMuonNumericValue(encoded_values, index, &number) ||
          !std::isfinite(number) || std::trunc(number) != number ||
          number < static_cast<double>(std::numeric_limits<int32_t>::min()) ||
          number > static_cast<double>(std::numeric_limits<int32_t>::max())) {
        *error_message = "Invalid i32 argument";
        return false;
      }
      value->i32_value = static_cast<int32_t>(number);
      return true;
    }
    case MUON_TYPE_U32: {
      auto number = 0.0;
      if (!GetMuonNumericValue(encoded_values, index, &number) ||
          !std::isfinite(number) || std::trunc(number) != number ||
          number < 0.0 ||
          number >
              static_cast<double>(std::numeric_limits<uint32_t>::max())) {
        *error_message = "Invalid u32 argument";
        return false;
      }
      value->u32_value = static_cast<uint32_t>(number);
      return true;
    }
    case MUON_TYPE_I64:
      if (encoded_values->GetType(index) != VTYPE_STRING ||
          !ParseMuonInt64(encoded_values->GetString(index).ToString(),
                          &value->i64_value)) {
        *error_message = "Invalid i64 argument";
        return false;
      }
      return true;
    case MUON_TYPE_U64:
      if (encoded_values->GetType(index) != VTYPE_STRING ||
          !ParseMuonUInt64(encoded_values->GetString(index).ToString(),
                           &value->u64_value)) {
        *error_message = "Invalid u64 argument";
        return false;
      }
      return true;
    case MUON_TYPE_F32: {
      auto number = 0.0;
      if (!GetMuonNumericValue(encoded_values, index, &number) ||
          !std::isfinite(number) ||
          number < -static_cast<double>(std::numeric_limits<float>::max()) ||
          number > static_cast<double>(std::numeric_limits<float>::max())) {
        *error_message = "Invalid f32 argument";
        return false;
      }
      value->f32_value = static_cast<float>(number);
      return true;
    }
    case MUON_TYPE_F64:
      if (!GetMuonNumericValue(encoded_values, index, &value->f64_value) ||
          !std::isfinite(value->f64_value)) {
        *error_message = "Invalid f64 argument";
        return false;
      }
      return true;
    case MUON_TYPE_POINTER:
      if (!GetMuonPointerValue(encoded_values, index,
                               &value->pointer_value)) {
        *error_message = "Invalid pointer argument";
        return false;
      }
      return true;
    case MUON_TYPE_STRING:
      if (encoded_values->GetType(index) == VTYPE_NULL) {
        value->is_null = true;
        return true;
      }
      if (encoded_values->GetType(index) != VTYPE_STRING) {
        *error_message = "Invalid string argument";
        return false;
      }
      value->string_value = encoded_values->GetString(index).ToString();
      return true;
    case MUON_TYPE_FUNCTION:
      if (encoded_values->GetType(index) == VTYPE_NULL) {
        value->is_null = true;
        value->function.type = expected_type;
        return true;
      }
      if (encoded_values->GetType(index) != VTYPE_DICTIONARY ||
          !DecodeMuonFunctionValue(owner, expected_type,
                                   encoded_values->GetDictionary(index),
                                   value, error_message)) {
        if (error_message->empty()) {
          *error_message = "Invalid function argument";
        }
        return false;
      }
      return true;
    case MUON_TYPE_BUFFER_VIEW: {
      if (encoded_values->GetType(index) != VTYPE_DICTIONARY ||
          !shared_payload || !shared_storage) {
        *error_message = "Invalid buffer_view argument";
        return false;
      }
      MuonSharedBufferEntry placeholder;
      if (!ReadMuonSharedBufferPlaceholder(
              encoded_values->GetDictionary(index), &placeholder) ||
          placeholder.value_index != index) {
        *error_message = "Invalid buffer_view argument";
        return false;
      }
      MuonSharedBufferEntry entry;
      if (!FindMuonSharedBufferEntry(*shared_payload, index, &entry) ||
          entry.offset != placeholder.offset ||
          entry.size != placeholder.size) {
        *error_message = "Buffer_view shared payload is missing";
        return false;
      }
      value->binary.storage = shared_storage;
      value->binary.offset = entry.offset;
      value->binary.size = entry.size;
      if (!IsValidMuonRpcBinary(value->binary)) {
        *error_message = "Buffer_view shared payload is invalid";
        return false;
      }
      return true;
    }
    default:
      *error_message = "Unsupported argument type";
      return false;
  }
}

static CefRefPtr<CefDictionaryValue> EncodeMuonFunctionValue(
    const MuonRpcFunctionReference& function) {
  const auto encoded = CefDictionaryValue::Create();
  if (function.kind == MuonRpcFunctionKind::PluginProxy) {
    encoded->SetString(kMuonFunctionKindKey,
                       kMuonFunctionKindPluginProxy);
    encoded->SetInt(kMuonFunctionProxyIdKey,
                    static_cast<int>(function.proxy_id));
    encoded->SetString(kMuonFunctionLeaseTokenKey, function.lease_token);
  } else {
    encoded->SetInt(kMuonFunctionContextIdKey,
                    function.renderer_context_id);
    encoded->SetInt(kMuonFunctionIdKey, function.function_id);
  }
  encoded->SetString(kMuonFunctionTypeKey,
                     CreateMuonTypeCanonicalKey(function.type));
  encoded->SetDictionary(kMuonFunctionTypeMetadataKey,
                         CreateMuonTypeMetadataDictionary(function.type));
  return encoded;
}

static bool EncodeMuonValue(
    CefRefPtr<CefListValue> list,
    size_t index,
    const MuonRpcValue& value,
    const std::vector<MuonSharedBufferEntry>& shared_entries,
    std::string* error_message) {
  if (!list || error_message == nullptr) {
    return false;
  }
  switch (value.type.type) {
    case MUON_TYPE_VOID:
      list->SetNull(index);
      return true;
    case MUON_TYPE_BOOL:
      list->SetBool(index, value.bool_value);
      return true;
    case MUON_TYPE_I8:
      list->SetInt(index, value.i8_value);
      return true;
    case MUON_TYPE_U8:
      list->SetInt(index, value.u8_value);
      return true;
    case MUON_TYPE_I16:
      list->SetInt(index, value.i16_value);
      return true;
    case MUON_TYPE_U16:
      list->SetInt(index, value.u16_value);
      return true;
    case MUON_TYPE_I32:
      list->SetInt(index, value.i32_value);
      return true;
    case MUON_TYPE_U32:
      list->SetDouble(index, static_cast<double>(value.u32_value));
      return true;
    case MUON_TYPE_I64:
      list->SetString(index, std::to_string(value.i64_value));
      return true;
    case MUON_TYPE_U64:
      list->SetString(index, std::to_string(value.u64_value));
      return true;
    case MUON_TYPE_F32:
      if (!std::isfinite(value.f32_value)) {
        *error_message = "Cannot encode a non-finite f32 value";
        return false;
      }
      list->SetDouble(index, static_cast<double>(value.f32_value));
      return true;
    case MUON_TYPE_F64:
      if (!std::isfinite(value.f64_value)) {
        *error_message = "Cannot encode a non-finite f64 value";
        return false;
      }
      list->SetDouble(index, value.f64_value);
      return true;
    case MUON_TYPE_POINTER:
      list->SetDouble(index, static_cast<double>(value.pointer_value));
      return true;
    case MUON_TYPE_STRING:
      if (value.is_null) {
        list->SetNull(index);
      } else {
        list->SetString(index, value.string_value);
      }
      return true;
    case MUON_TYPE_FUNCTION:
      if (value.is_null) {
        list->SetNull(index);
        return true;
      }
      if (value.function.type.type != MUON_TYPE_FUNCTION ||
          value.function.type.function_return_type.empty()) {
        *error_message = "Invalid function value";
        return false;
      }
      list->SetDictionary(index, EncodeMuonFunctionValue(value.function));
      return true;
    case MUON_TYPE_BUFFER_VIEW: {
      MuonSharedBufferEntry entry;
      if (!FindMuonSharedBufferEntry(shared_entries, index, &entry)) {
        *error_message = "Missing shared buffer payload entry";
        return false;
      }
      list->SetDictionary(index, CreateMuonSharedBufferPlaceholder(entry));
      return true;
    }
    default:
      *error_message = "Unsupported encoded value type";
      return false;
  }
}

static bool CreateMuonCefSharedMessage(
    MuonCefRpcBridgeImpl* impl,
    const std::string& message_name,
    int call_id,
    int context_id,
    const std::vector<std::pair<size_t, MuonRpcBinary>>& binaries,
    MuonCreatedSharedBufferMessage* created,
    std::string* error_message) {
  if (impl == nullptr || created == nullptr || error_message == nullptr ||
      binaries.empty()) {
    return false;
  }
  created->message = nullptr;
  created->entries.clear();
  if (binaries.size() == 1 &&
      message_name == kMuonPluginResultSharedMessageName) {
    const auto& binary = binaries[0].second;
    const auto storage_iterator =
        impl->result_buffers.find(binary.storage.get());
    if (storage_iterator != impl->result_buffers.end()) {
      const auto storage = storage_iterator->second.lock();
      impl->result_buffers.erase(storage_iterator);
      if (storage) {
        return storage->Build(call_id, context_id, binaries[0].first,
                              binary.offset, binary.size, created,
                              error_message);
      }
    }
  }

  auto sources = std::vector<MuonSharedBufferSource>{};
  sources.reserve(binaries.size());
  for (const auto& indexed_binary : binaries) {
    if (!IsValidMuonRpcBinary(indexed_binary.second)) {
      *error_message = "Invalid shared buffer value";
      return false;
    }
    sources.push_back({indexed_binary.first,
                       GetMuonRpcBinaryData(indexed_binary.second),
                       indexed_binary.second.size});
  }
  return CreateMuonSharedBufferMessage(message_name, call_id, context_id,
                                       sources, created, error_message);
}

static CefRefPtr<CefFrame> ResolveMuonCefFrame(
    MuonCefRpcBridgeImpl* impl,
    const MuonRpcOwner& owner) {
  if (impl == nullptr || !impl->frame_resolver ||
      !IsValidMuonRpcOwner(owner)) {
    return nullptr;
  }
  const auto frame = impl->frame_resolver(owner);
  return frame && frame->IsValid() ? frame : nullptr;
}

static bool SendMuonCefProcessMessages(
    CefRefPtr<CefFrame> frame,
    CefRefPtr<CefProcessMessage> shared_message,
    CefRefPtr<CefProcessMessage> metadata_message,
    std::string* error_message) {
  if (!frame || !metadata_message || error_message == nullptr) {
    return false;
  }
  if (shared_message) {
    frame->SendProcessMessage(PID_RENDERER, shared_message);
  }
  frame->SendProcessMessage(PID_RENDERER, metadata_message);
  return true;
}

static bool SendMuonCallResult(MuonCefRpcBridgeImpl* impl,
                               const MuonRpcCallResult& result,
                               std::string* error_message) {
  const auto frame = ResolveMuonCefFrame(impl, result.owner);
  if (!frame) {
    *error_message = "Renderer frame is unavailable";
    return false;
  }
  const auto message =
      CefProcessMessage::Create(kMuonPluginResultMessageName);
  const auto args = message->GetArgumentList();
  args->SetSize(5);
  args->SetInt(0, static_cast<int>(result.call_id));
  args->SetBool(1, result.success);
  args->SetInt(4, result.owner.context_id);
  if (!result.success) {
    args->SetString(2, result.error_message);
    args->SetNull(3);
    return SendMuonCefProcessMessages(frame, nullptr, message,
                                      error_message);
  }

  args->SetInt(2, static_cast<int>(result.value.type.type));
  auto shared_message = MuonCreatedSharedBufferMessage{};
  auto encode_error = std::string{};
  if (result.value.type.type == MUON_TYPE_BUFFER_VIEW) {
    const auto binaries =
        std::vector<std::pair<size_t, MuonRpcBinary>>{{3,
                                                       result.value.binary}};
    if (!CreateMuonCefSharedMessage(
            impl, kMuonPluginResultSharedMessageName,
            static_cast<int>(result.call_id), result.owner.context_id,
            binaries, &shared_message, &encode_error)) {
      args->SetBool(1, false);
      args->SetString(2, encode_error.empty()
                             ? "Missing plugin buffer result payload"
                             : encode_error);
      args->SetNull(3);
    }
  }
  if (args->GetBool(1) &&
      !EncodeMuonValue(args, 3, result.value, shared_message.entries,
                       &encode_error)) {
    args->SetBool(1, false);
    args->SetString(2, encode_error.empty()
                           ? "Unsupported plugin result type"
                           : encode_error);
    args->SetNull(3);
    shared_message.message = nullptr;
  }
  return SendMuonCefProcessMessages(frame, shared_message.message, message,
                                    error_message);
}

static bool SendMuonRendererFunctionCall(
    MuonCefRpcBridgeImpl* impl,
    const MuonRpcRendererFunctionCall& call,
    std::string* error_message) {
  const auto frame = ResolveMuonCefFrame(impl, call.owner);
  if (!frame) {
    *error_message = "Renderer frame is unavailable";
    return false;
  }
  auto binaries = std::vector<std::pair<size_t, MuonRpcBinary>>{};
  for (auto index = size_t{0}; index < call.arguments.size(); ++index) {
    if (call.arguments[index].type.type == MUON_TYPE_BUFFER_VIEW) {
      binaries.push_back({index, call.arguments[index].binary});
    }
  }
  auto shared_message = MuonCreatedSharedBufferMessage{};
  if (!binaries.empty() &&
      !CreateMuonCefSharedMessage(
          impl, kMuonRendererFunctionCallSharedMessageName,
          static_cast<int>(call.call_id), call.owner.context_id, binaries,
          &shared_message, error_message)) {
    return false;
  }

  const auto encoded_values = CefListValue::Create();
  encoded_values->SetSize(call.arguments.size());
  for (auto index = size_t{0}; index < call.arguments.size(); ++index) {
    if (!EncodeMuonValue(encoded_values, index, call.arguments[index],
                         shared_message.entries, error_message)) {
      return false;
    }
  }
  const auto message =
      CefProcessMessage::Create(kMuonRendererFunctionCallMessageName);
  const auto args = message->GetArgumentList();
  args->SetSize(6);
  args->SetInt(0, static_cast<int>(call.call_id));
  args->SetInt(1, call.owner.context_id);
  args->SetInt(2, call.function_id);
  args->SetList(3, encoded_values);
  args->SetDictionary(4,
                      CreateMuonTypeMetadataDictionary(call.function_type));
  args->SetBool(5, call.expects_result);
  return SendMuonCefProcessMessages(frame, shared_message.message, message,
                                    error_message);
}

MuonCefRpcBridge::MuonCefRpcBridge(
    std::unique_ptr<MuonCefRpcBridgeImpl> impl)
    : impl_(std::move(impl)) {}

MuonCefRpcBridge::~MuonCefRpcBridge() = default;

MuonPluginRuntimeServices MuonCefRpcBridge::CreateRuntimeServices() {
  const auto weak_bridge = weak_from_this();
  MuonPluginRuntimeServices services;
  services.is_owner_thread = []() { return CefCurrentlyOn(TID_UI); };
  services.post_owner_task = [](std::function<void()> task) {
    return task && CefPostTask(TID_UI, new MuonCefRpcTask(std::move(task)));
  };
  services.allocate_buffer =
      [weak_bridge](size_t size, std::string* error_message) {
        const auto bridge = weak_bridge.lock();
        if (!bridge || !bridge->impl_) {
          if (error_message != nullptr) {
            *error_message = "CEF RPC bridge is unavailable";
          }
          return std::shared_ptr<MuonRpcBufferStorage>{};
        }
        auto payload_size = size_t{0};
        if (!GetMuonSharedBufferSingleEntryPayloadSize(size, &payload_size)) {
          if (error_message != nullptr) {
            *error_message = "Shared buffer size is too large";
          }
          return std::shared_ptr<MuonRpcBufferStorage>{};
        }
        const auto builder = CefSharedProcessMessageBuilder::Create(
            kMuonPluginResultSharedMessageName, payload_size);
        if (!builder || !builder->IsValid() || builder->Memory() == nullptr ||
            builder->Size() != payload_size) {
          if (error_message != nullptr) {
            *error_message = "Failed to allocate shared buffer";
          }
          return std::shared_ptr<MuonRpcBufferStorage>{};
        }
        auto result_buffer = bridge->impl_->result_buffers.begin();
        while (result_buffer != bridge->impl_->result_buffers.end()) {
          if (result_buffer->second.expired()) {
            result_buffer = bridge->impl_->result_buffers.erase(result_buffer);
          } else {
            ++result_buffer;
          }
        }
        const auto storage =
            std::make_shared<MuonCefResultBufferStorage>(builder, size);
        bridge->impl_->result_buffers[storage.get()] = storage;
        return std::static_pointer_cast<MuonRpcBufferStorage>(storage);
      };
  services.is_owner_available = [weak_bridge](const MuonRpcOwner& owner) {
    const auto bridge = weak_bridge.lock();
    return bridge && bridge->impl_ &&
           ResolveMuonCefFrame(bridge->impl_.get(), owner) != nullptr;
  };
  services.send_message =
      [weak_bridge](const MuonRpcMessage& message,
                    std::string* error_message) {
        const auto bridge = weak_bridge.lock();
        if (!bridge) {
          if (error_message != nullptr) {
            *error_message = "CEF RPC bridge is unavailable";
          }
          return false;
        }
        return bridge->SendMessage(message, error_message);
      };
  return services;
}

void MuonCefRpcBridge::AttachFrameResolver(FrameResolver resolver) {
  if (impl_) {
    impl_->frame_resolver = std::move(resolver);
  }
}

void MuonCefRpcBridge::DetachFrameResolver() {
  if (impl_) {
    impl_->frame_resolver = nullptr;
  }
}

bool MuonCefRpcBridge::DecodeArguments(
    const MuonRpcOwner& owner,
    const std::vector<MuonTypeMetadata>& expected_types,
    CefRefPtr<CefListValue> encoded_values,
    std::shared_ptr<MuonSharedBufferPayload> shared_payload,
    std::vector<MuonRpcValue>* values,
    std::string* error_message) const {
  if (values == nullptr || error_message == nullptr) {
    return false;
  }
  values->clear();
  error_message->clear();
  if (!encoded_values) {
    *error_message = "Missing argument list";
    return false;
  }
  if (encoded_values->GetSize() != expected_types.size()) {
    *error_message = "Invalid argument count";
    return false;
  }
  auto shared_storage = std::shared_ptr<MuonRpcBufferStorage>{};
  if (shared_payload) {
    shared_storage =
        std::make_shared<MuonCefSharedMemoryStorage>(shared_payload);
  }
  values->resize(expected_types.size());
  for (auto index = size_t{0}; index < expected_types.size(); ++index) {
    if (!DecodeMuonValue(owner, expected_types[index], encoded_values, index,
                         shared_payload, shared_storage, false,
                         &(*values)[index], error_message)) {
      values->clear();
      return false;
    }
  }
  return true;
}

bool MuonCefRpcBridge::DecodeRendererFunctionResult(
    const MuonRpcOwner& owner,
    const MuonTypeMetadata& expected_type,
    CefRefPtr<CefProcessMessage> message,
    std::shared_ptr<MuonSharedBufferPayload> shared_payload,
    MuonRpcRendererFunctionResult* result,
    std::string* error_message) const {
  if (result == nullptr || error_message == nullptr) {
    return false;
  }
  *result = MuonRpcRendererFunctionResult{};
  result->owner = owner;
  error_message->clear();
  if (!message || message->GetName().ToString() !=
                      kMuonRendererFunctionResultMessageName) {
    *error_message = "Renderer function result is invalid";
    return false;
  }
  const auto args = message->GetArgumentList();
  if (!args || args->GetSize() != 5 ||
      args->GetType(0) != VTYPE_INT || args->GetInt(0) <= 0 ||
      args->GetType(1) != VTYPE_BOOL ||
      args->GetType(4) != VTYPE_INT ||
      args->GetInt(4) != owner.context_id) {
    *error_message = "Renderer function result is invalid";
    return false;
  }
  result->call_id = static_cast<uint32_t>(args->GetInt(0));
  result->success = args->GetBool(1);
  if (!result->success) {
    if (args->GetType(2) != VTYPE_STRING) {
      *error_message = "Renderer function result is invalid";
      return false;
    }
    result->error_message = args->GetString(2).ToString();
    return true;
  }
  if (args->GetType(2) != VTYPE_INT ||
      static_cast<muon_value_type>(args->GetInt(2)) != expected_type.type) {
    *error_message = "Renderer function returned an unexpected type";
    return false;
  }
  auto shared_storage = std::shared_ptr<MuonRpcBufferStorage>{};
  if (shared_payload) {
    shared_storage =
        std::make_shared<MuonCefSharedMemoryStorage>(shared_payload);
  }
  if (!DecodeMuonValue(owner, expected_type, args, 3, shared_payload,
                       shared_storage, true, &result->value,
                       error_message)) {
    return false;
  }
  return true;
}

bool MuonCefRpcBridge::SendMessage(const MuonRpcMessage& message,
                                   std::string* error_message) {
  auto local_error = std::string{};
  auto* target_error = error_message != nullptr ? error_message : &local_error;
  target_error->clear();
  if (!impl_) {
    *target_error = "CEF RPC bridge is unavailable";
    return false;
  }
  if (const auto* result = std::get_if<MuonRpcCallResult>(&message)) {
    return SendMuonCallResult(impl_.get(), *result, target_error);
  }
  if (const auto* call =
          std::get_if<MuonRpcRendererFunctionCall>(&message)) {
    return SendMuonRendererFunctionCall(impl_.get(), *call, target_error);
  }
  if (const auto* lease =
          std::get_if<MuonRpcRendererFunctionLease>(&message)) {
    const auto frame = ResolveMuonCefFrame(impl_.get(), lease->owner);
    if (!frame) {
      *target_error = "Renderer frame is unavailable";
      return false;
    }
    const auto message_name =
        lease->acquire ? kMuonRendererFunctionSourceAcquireMessageName
                       : kMuonRendererFunctionSourceReleaseMessageName;
    const auto encoded = CefProcessMessage::Create(message_name);
    const auto args = encoded->GetArgumentList();
    args->SetSize(3);
    args->SetInt(0, lease->owner.context_id);
    args->SetInt(1, lease->function_id);
    args->SetString(2, lease->lease_token);
    return SendMuonCefProcessMessages(frame, nullptr, encoded, target_error);
  }
  if (const auto* consumed =
          std::get_if<MuonRpcRendererFunctionResultConsumed>(&message)) {
    const auto frame = ResolveMuonCefFrame(impl_.get(), consumed->owner);
    if (!frame) {
      *target_error = "Renderer frame is unavailable";
      return false;
    }
    const auto encoded = CefProcessMessage::Create(
        kMuonRendererFunctionResultConsumedMessageName);
    const auto args = encoded->GetArgumentList();
    args->SetSize(2);
    args->SetInt(0, consumed->owner.context_id);
    args->SetInt(1, static_cast<int>(consumed->call_id));
    return SendMuonCefProcessMessages(frame, nullptr, encoded, target_error);
  }
  *target_error = "Unsupported host-to-renderer RPC message";
  return false;
}

std::shared_ptr<MuonCefRpcBridge> CreateMuonCefRpcBridge() {
  return std::shared_ptr<MuonCefRpcBridge>(
      new MuonCefRpcBridge(std::make_unique<MuonCefRpcBridgeImpl>()));
}
