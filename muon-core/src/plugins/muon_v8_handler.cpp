/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "plugins/muon_v8_handler.h"

#include "plugins/muon_js_bridge.h"
#include "plugins/muon_shared_buffer.h"

#include "include/cef_frame.h"
#include "include/cef_process_message.h"
#include "include/cef_shared_process_message_builder.h"
#include "include/cef_values.h"
#include "include/wrapper/cef_helpers.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <system_error>
#include <utility>

static constexpr char kMuonFunctionArgumentContextIdKey[] = "context_id";
static constexpr char kMuonFunctionArgumentFunctionIdKey[] = "function_id";
static constexpr char kMuonFunctionArgumentKindKey[] = "kind";
static constexpr char kMuonFunctionArgumentKindPluginProxy[] = "plugin_proxy";
static constexpr char kMuonFunctionArgumentProxyIdKey[] = "proxy_id";
static constexpr char kMuonFunctionArgumentTypeKey[] = "type_key";
static constexpr char kMuonFunctionArgumentTypeDescriptorKey[] = "type";
static constexpr double kMuonTwoTo63 = 9223372036854775808.0;
static constexpr double kMuonTwoTo64 = 18446744073709551616.0;
static int g_next_muon_v8_context_id = 1;

static bool GetNumericV8Value(CefRefPtr<CefV8Value> value, double* number) {
  if (value->IsInt()) {
    *number = static_cast<double>(value->GetIntValue());
    return true;
  }
  if (value->IsUInt()) {
    *number = static_cast<double>(value->GetUIntValue());
    return true;
  }
  if (value->IsDouble()) {
    *number = value->GetDoubleValue();
    return true;
  }
  return false;
}

static bool ContainsMuonNulCharacter(const std::string& value) {
  return value.find('\0') != std::string::npos;
}

static int64_t ReinterpretUInt64AsInt64(uint64_t value) {
  auto result = int64_t{0};
  std::memcpy(&result, &value, sizeof(result));
  return result;
}

static uint64_t ReinterpretInt64AsUInt64(int64_t value) {
  auto result = uint64_t{0};
  std::memcpy(&result, &value, sizeof(result));
  return result;
}

static uint64_t GetTruncatedDoubleUInt64Bits(double number) {
  const auto truncated = std::trunc(number);
  if (truncated >= -kMuonTwoTo63 && truncated < kMuonTwoTo63) {
    return ReinterpretInt64AsUInt64(static_cast<int64_t>(truncated));
  }
  auto modulo = std::fmod(truncated, kMuonTwoTo64);
  if (modulo < 0.0) {
    modulo += kMuonTwoTo64;
  }
  if (modulo >= kMuonTwoTo64) {
    modulo = 0.0;
  }
  if (modulo >= kMuonTwoTo63) {
    return (uint64_t{1} << 63) +
           static_cast<uint64_t>(modulo - kMuonTwoTo63);
  }
  return static_cast<uint64_t>(modulo);
}

static bool ParseMuonInt64String(const std::string& source, int64_t* value) {
  if (value == nullptr) {
    return false;
  }
  const auto begin = source.data();
  const auto end = begin + source.size();
  auto parsed = int64_t{0};
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc() || result.ptr != end) {
    return false;
  }
  *value = parsed;
  return true;
}

static bool ParseMuonUInt64String(const std::string& source, uint64_t* value) {
  if (value == nullptr) {
    return false;
  }
  const auto begin = source.data();
  const auto end = begin + source.size();
  auto parsed = uint64_t{0};
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc() || result.ptr != end) {
    return false;
  }
  *value = parsed;
  return true;
}

// CEF does not expose the V8 integer API needed for lossless 64-bit JS values.
// Muon therefore mirrors the Node-API int64 number boundary:
// napi_create_int64 creates a JS number and documents precision loss outside
// Number.MIN_SAFE_INTEGER/Number.MAX_SAFE_INTEGER, while napi_get_value_int64
// requires a JS number and maps non-finite values to zero.
// See:
// https://nodejs.org/api/n-api.html#napi_create_int64
// https://nodejs.org/api/n-api.html#napi_get_value_int64
static bool GetNodeApiInt64V8Value(CefRefPtr<CefV8Value> value,
                                   int64_t* result) {
  if (!value || result == nullptr) {
    return false;
  }
  auto number = 0.0;
  if (!GetNumericV8Value(value, &number)) {
    return false;
  }
  if (!std::isfinite(number)) {
    *result = 0;
    return true;
  }
  *result = ReinterpretUInt64AsInt64(GetTruncatedDoubleUInt64Bits(number));
  return true;
}

static CefRefPtr<CefV8Value> CreateNodeApiInt64V8Value(int64_t value) {
  return CefV8Value::CreateDouble(static_cast<double>(value));
}

static CefRefPtr<CefV8Value> CreateNodeApiInt64V8ValueFromString(
    const std::string& source) {
  auto value = int64_t{0};
  if (!ParseMuonInt64String(source, &value)) {
    return CefV8Value::CreateUndefined();
  }
  return CreateNodeApiInt64V8Value(value);
}

static CefRefPtr<CefV8Value> CreateNodeApiUInt64V8ValueFromString(
    const std::string& source) {
  auto value = uint64_t{0};
  if (!ParseMuonUInt64String(source, &value)) {
    return CefV8Value::CreateUndefined();
  }
  return CreateNodeApiInt64V8Value(ReinterpretUInt64AsInt64(value));
}

static bool GetPointerNumberV8Value(CefRefPtr<CefV8Value> value,
                                    double* number) {
  if (!value || number == nullptr) {
    return false;
  }
  if (value->IsNull() || value->IsUndefined()) {
    *number = 0.0;
    return true;
  }
  auto parsed = 0.0;
  if (!GetNumericV8Value(value, &parsed) || !std::isfinite(parsed) ||
      std::trunc(parsed) != parsed || parsed < 0.0 ||
      parsed >=
          std::ldexp(1.0, std::numeric_limits<uintptr_t>::digits)) {
    return false;
  }
  *number = parsed;
  return true;
}

static bool GetSizeFromV8Number(CefRefPtr<CefV8Value> value,
                                size_t* size) {
  if (size == nullptr) {
    return false;
  }
  auto number = 0.0;
  if (!value || !GetNumericV8Value(value, &number) ||
      !std::isfinite(number) || std::trunc(number) != number ||
      number < 0.0 ||
      number >= std::ldexp(1.0, std::numeric_limits<size_t>::digits)) {
    return false;
  }
  *size = static_cast<size_t>(number);
  return true;
}

static bool IsV8ArrayBufferView(CefRefPtr<CefV8Value> value) {
  if (!value || !value->IsObject()) {
    return false;
  }
  const auto context = CefV8Context::GetCurrentContext();
  const auto global = context ? context->GetGlobal() : nullptr;
  const auto array_buffer =
      global ? global->GetValue("ArrayBuffer") : nullptr;
  const auto is_view =
      array_buffer && array_buffer->IsObject()
          ? array_buffer->GetValue("isView")
          : nullptr;
  if (!is_view || !is_view->IsFunction()) {
    return false;
  }
  CefV8ValueList args;
  args.push_back(value);
  const auto result = is_view->ExecuteFunction(array_buffer, args);
  return result && result->IsBool() && result->GetBoolValue();
}

static bool GetV8BufferView(CefRefPtr<CefV8Value> value,
                            void** data,
                            size_t* size) {
  if (data == nullptr || size == nullptr || !value) {
    return false;
  }
  if (value->IsArrayBuffer()) {
    *data = value->GetArrayBufferData();
    *size = value->GetArrayBufferByteLength();
    return *size == 0 || *data != nullptr;
  }
  if (!IsV8ArrayBufferView(value)) {
    return false;
  }

  const auto buffer = value->GetValue("buffer");
  const auto byte_offset = value->GetValue("byteOffset");
  const auto byte_length = value->GetValue("byteLength");
  auto offset = size_t{0};
  auto length = size_t{0};
  if (!buffer || !buffer->IsArrayBuffer() ||
      !GetSizeFromV8Number(byte_offset, &offset) ||
      !GetSizeFromV8Number(byte_length, &length)) {
    return false;
  }
  const auto buffer_length = buffer->GetArrayBufferByteLength();
  if (offset > buffer_length || length > buffer_length - offset) {
    return false;
  }
  auto* base = static_cast<uint8_t*>(buffer->GetArrayBufferData());
  if (length > 0 && base == nullptr) {
    return false;
  }
  *data = length == 0 && base == nullptr ? nullptr : base + offset;
  *size = length;
  return true;
}

static bool ApplyMuonSharedBufferPlaceholders(
    CefRefPtr<CefListValue> values,
    const std::vector<MuonSharedBufferEntry>& entries,
    std::string* error_message) {
  for (const auto& entry : entries) {
    if (!values || entry.value_index >= values->GetSize()) {
      *error_message = "Shared buffer placeholder index is invalid";
      return false;
    }
    values->SetDictionary(entry.value_index,
                          CreateMuonSharedBufferPlaceholder(entry));
  }
  return true;
}

static CefRefPtr<CefV8Value> CreateV8ArrayBufferFromSharedPayload(
    CefRefPtr<CefListValue> values,
    size_t index,
    std::shared_ptr<MuonSharedBufferPayload> shared_payload) {
  if (!values || index >= values->GetSize() ||
      values->GetType(index) != VTYPE_DICTIONARY ||
      !shared_payload) {
    return CefV8Value::CreateUndefined();
  }
  MuonSharedBufferEntry placeholder;
  if (!ReadMuonSharedBufferPlaceholder(values->GetDictionary(index),
                                        &placeholder) ||
      placeholder.value_index != index) {
    return CefV8Value::CreateUndefined();
  }
  MuonSharedBufferEntry entry;
  if (!FindMuonSharedBufferEntry(*shared_payload, index, &entry) ||
      entry.offset != placeholder.offset || entry.size != placeholder.size) {
    return CefV8Value::CreateUndefined();
  }
  auto* data = GetMuonSharedBufferEntryData(*shared_payload, entry);
  if (entry.size > 0 && data == nullptr) {
    return CefV8Value::CreateUndefined();
  }
  const auto array_buffer =
      CefV8Value::CreateArrayBufferWithCopy(data, entry.size);
  return array_buffer ? array_buffer : CefV8Value::CreateUndefined();
}

std::string CreateMuonV8FunctionName(uint32_t function_id) {
  return "__muon_function_" + std::to_string(function_id);
}

MuonV8Handler::MuonV8Handler(std::vector<MuonFunctionMetadata> functions,
                               CefRefPtr<CefV8Context> context)
    : functions_(std::move(functions)),
      context_(context),
      context_id_(g_next_muon_v8_context_id) {
  g_next_muon_v8_context_id += 1;
  for (auto index = size_t{0}; index < functions_.size(); ++index) {
    function_indexes_by_v8_name_[CreateMuonV8FunctionName(
        functions_[index].id)] = index;
    function_indexes_by_public_path_[CreateMuonFunctionPublicPath(
        functions_[index])] = index;
  }
}

bool MuonV8Handler::Execute(const CefString& name,
                             CefRefPtr<CefV8Value> object,
                             const CefV8ValueList& arguments,
                             CefRefPtr<CefV8Value>& retval,
                             CefString& exception) {
  CEF_REQUIRE_RENDERER_THREAD();
  const auto v8_name = name.ToString();
  auto is_capability_call = v8_name == kMuonV8CapabilityCallFunctionName;
  auto function_iterator = function_indexes_by_v8_name_.find(v8_name);
  auto proxy_iterator = proxy_functions_by_name_.find(v8_name);
  if (!is_capability_call &&
      function_iterator == function_indexes_by_v8_name_.end() &&
      proxy_iterator == proxy_functions_by_name_.end()) {
    return false;
  }

  auto promise = CefV8Value::CreatePromise();
  retval = promise;

  auto function_name = v8_name;
  auto capability_id = std::string{};
  auto capability_function_path = std::string{};
  CefV8ValueList call_arguments = arguments;
  std::vector<MuonTypeMetadata> arg_types;
  auto return_type = CreateMuonPrimitiveType(MUON_TYPE_VOID);
  auto function_id = uint32_t{0};
  auto is_proxy_call = false;
  if (is_capability_call) {
    if (arguments.size() != 3 || !arguments[0] || !arguments[0]->IsString() ||
        !arguments[1] || !arguments[1]->IsString() || !arguments[2] ||
        !arguments[2]->IsArray()) {
      RejectPromise(promise, "Invalid Muon capability call");
      return true;
    }
    capability_id = arguments[0]->GetStringValue().ToString();
    capability_function_path = arguments[1]->GetStringValue().ToString();
    if (capability_id.empty() || capability_function_path.empty()) {
      RejectPromise(promise, "Invalid Muon capability call");
      return true;
    }
    const auto public_function_iterator =
        function_indexes_by_public_path_.find(capability_function_path);
    if (public_function_iterator == function_indexes_by_public_path_.end()) {
      RejectPromise(promise,
                    "Unknown Muon capability function: " +
                        capability_function_path);
      return true;
    }
    function_iterator =
        function_indexes_by_v8_name_.find(CreateMuonV8FunctionName(
            functions_[public_function_iterator->second].id));
    if (function_iterator == function_indexes_by_v8_name_.end()) {
      RejectPromise(promise,
                    "Unknown Muon capability function: " +
                        capability_function_path);
      return true;
    }
    call_arguments.clear();
    const auto argument_count = arguments[2]->GetArrayLength();
    for (auto index = 0; index < argument_count; ++index) {
      call_arguments.push_back(arguments[2]->GetValue(index));
    }
  }
  if (function_iterator != function_indexes_by_v8_name_.end()) {
    const auto& function = functions_[function_iterator->second];
    function_name = CreateMuonFunctionPublicPath(function);
    arg_types = function.arg_types;
    return_type = function.return_type;
    function_id = function.id;
  } else {
    const auto& proxy = proxy_iterator->second;
    if (proxy.function_type.type != MUON_TYPE_FUNCTION ||
        proxy.function_type.function_return_type.empty()) {
      RejectPromise(promise, "Muon function proxy type is invalid");
      return true;
    }
    arg_types = proxy.function_type.function_arg_types;
    return_type = proxy.function_type.function_return_type[0];
    function_id = proxy.proxy_id;
    is_proxy_call = true;
  }

  const auto call_id = next_call_id_;
  next_call_id_ += 1;
  const auto encoded_args = CefListValue::Create();
  std::vector<MuonSharedBufferSource> shared_sources;
  std::string error_message;
  if (!ValidateAndEncodeArguments(function_name, arg_types, call_arguments,
                                  encoded_args, &shared_sources,
                                  &error_message)) {
    RejectPromise(promise, error_message);
    return true;
  }

  const auto context = CefV8Context::GetCurrentContext();
  const auto frame = context ? context->GetFrame() : nullptr;
  if (!frame) {
    RejectPromise(promise, "Muon frame is not available");
    return true;
  }

  MuonCreatedSharedBufferMessage shared_message;
  if (!shared_sources.empty() &&
      !CreateMuonSharedBufferMessage(
          is_proxy_call ? kMuonPluginProxyCallSharedMessageName
                        : kMuonPluginCallSharedMessageName,
          call_id, context_id_, shared_sources, &shared_message,
          &error_message)) {
    RejectPromise(promise, error_message);
    return true;
  }
  if (!shared_message.entries.empty() &&
      !ApplyMuonSharedBufferPlaceholders(encoded_args,
                                          shared_message.entries,
                                          &error_message)) {
    RejectPromise(promise, error_message);
    return true;
  }
  PendingPromise pending_promise;
  pending_promise.promise = promise;
  pending_promise.return_type = return_type;
  pending_promises_[call_id] = pending_promise;

  const auto message = CefProcessMessage::Create(
      is_proxy_call ? kMuonPluginProxyCallMessageName
                    : kMuonPluginCallMessageName);
  const auto message_args = message->GetArgumentList();
  message_args->SetSize(is_capability_call ? 6 : 4);
  message_args->SetInt(0, call_id);
  message_args->SetInt(1, static_cast<int>(function_id));
  message_args->SetList(2, encoded_args);
  message_args->SetInt(3, context_id_);
  if (is_capability_call) {
    message_args->SetString(4, capability_id);
    message_args->SetString(5, capability_function_path);
  }
  if (shared_message.message) {
    frame->SendProcessMessage(PID_BROWSER, shared_message.message);
  }
  frame->SendProcessMessage(PID_BROWSER, message);
  return true;
}

bool MuonV8Handler::HandleResultMessage(CefRefPtr<CefProcessMessage> message) {
  CEF_REQUIRE_RENDERER_THREAD();
  if (!message || message->GetName().ToString() != kMuonPluginResultMessageName) {
    return false;
  }

  const auto message_args = message->GetArgumentList();
  if (!message_args || message_args->GetSize() < 3) {
    return true;
  }

  const auto call_id = message_args->GetInt(0);
  const auto needs_shared =
      message_args->GetSize() >= 4 && message_args->GetBool(1) &&
      CefListValueHasMuonSharedBufferPlaceholders(message_args);
  if (needs_shared) {
    const auto payload_iterator = pending_result_payloads_.find(call_id);
    if (payload_iterator == pending_result_payloads_.end()) {
      pending_result_messages_[call_id] = message;
      return true;
    }
    const auto pending_payload = payload_iterator->second;
    pending_result_payloads_.erase(payload_iterator);
    if (pending_payload.has_error) {
      const auto pending_iterator = pending_promises_.find(call_id);
      if (pending_iterator != pending_promises_.end()) {
        const auto pending_promise = pending_iterator->second;
        pending_promises_.erase(pending_iterator);
        if (context_ && context_->IsValid() && context_->Enter()) {
          pending_promise.promise->RejectPromise(pending_payload.error_message);
          context_->Exit();
        }
      }
      return true;
    }
    return ResolvePluginResultMessage(message, pending_payload.payload);
  }
  return ResolvePluginResultMessage(message, nullptr);
}

bool MuonV8Handler::HandleResultSharedMessage(
    CefRefPtr<CefProcessMessage> message) {
  CEF_REQUIRE_RENDERER_THREAD();
  if (!message || message->GetName().ToString() !=
                      kMuonPluginResultSharedMessageName) {
    return false;
  }
  auto call_id = 0;
  std::shared_ptr<MuonSharedBufferPayload> payload;
  std::string error_message;
  const auto decoded = DecodeMuonSharedBufferPayload(
      message, &call_id, &payload, &error_message);
  const auto metadata_iterator = pending_result_messages_.find(call_id);
  if (metadata_iterator != pending_result_messages_.end()) {
    const auto metadata = metadata_iterator->second;
    pending_result_messages_.erase(metadata_iterator);
    if (!decoded) {
      const auto pending_iterator = pending_promises_.find(call_id);
      if (pending_iterator != pending_promises_.end()) {
        const auto pending_promise = pending_iterator->second;
        pending_promises_.erase(pending_iterator);
        if (context_ && context_->IsValid() && context_->Enter()) {
          pending_promise.promise->RejectPromise(error_message);
          context_->Exit();
        }
      }
      return true;
    }
    return ResolvePluginResultMessage(metadata, payload);
  }

  PendingSharedPayload pending_payload;
  pending_payload.payload = payload;
  pending_payload.error_message = error_message;
  pending_payload.has_error = !decoded;
  pending_result_payloads_[call_id] = pending_payload;
  return true;
}

bool MuonV8Handler::ResolvePluginResultMessage(
    CefRefPtr<CefProcessMessage> message,
    std::shared_ptr<MuonSharedBufferPayload> shared_payload) {
  const auto message_args = message->GetArgumentList();
  if (!message_args || message_args->GetSize() < 3) {
    return true;
  }
  const auto call_id = message_args->GetInt(0);
  const auto pending_iterator = pending_promises_.find(call_id);
  if (pending_iterator == pending_promises_.end()) {
    return true;
  }

  const auto pending_promise = pending_iterator->second;
  pending_promises_.erase(pending_iterator);

  if (!context_ || !context_->IsValid() || !context_->Enter()) {
    return true;
  }

  const auto success = message_args->GetBool(1);
  if (!success) {
    pending_promise.promise->RejectPromise(message_args->GetString(2));
    context_->Exit();
    return true;
  }

  const auto returned_type =
      static_cast<muon_value_type>(message_args->GetInt(2));
  if (returned_type != pending_promise.return_type.type) {
    pending_promise.promise->RejectPromise(
        "Muon plugin returned an unexpected result type");
    context_->Exit();
    return true;
  }

  pending_promise.promise->ResolvePromise(
      CreateV8ValueFromResult(pending_promise.return_type, message_args,
                              std::move(shared_payload)));
  context_->Exit();
  return true;
}

void MuonV8Handler::RejectAllPendingPromises() {
  CEF_REQUIRE_RENDERER_THREAD();
  if (!context_ || !context_->IsValid() || !context_->Enter()) {
    pending_promises_.clear();
    pending_result_messages_.clear();
    pending_result_payloads_.clear();
    pending_renderer_function_call_messages_.clear();
    pending_renderer_function_call_payloads_.clear();
    return;
  }

  for (const auto& entry : pending_promises_) {
    entry.second.promise->RejectPromise("Muon V8 context was released");
  }
  pending_promises_.clear();
  pending_result_messages_.clear();
  pending_result_payloads_.clear();
  pending_renderer_function_call_messages_.clear();
  pending_renderer_function_call_payloads_.clear();
  context_->Exit();
}

void MuonV8Handler::ReleaseFunctionReferences() {
  CEF_REQUIRE_RENDERER_THREAD();
  function_references_.clear();
  proxy_functions_by_name_.clear();
  pending_renderer_function_call_messages_.clear();
  pending_renderer_function_call_payloads_.clear();
}

int MuonV8Handler::GetContextId() const {
  return context_id_;
}

bool MuonV8Handler::IsForContext(CefRefPtr<CefV8Context> context) const {
  return context_ && context && context_->IsSame(context);
}

bool MuonV8Handler::ValidateAndEncodeArguments(
    const std::string& function_name,
    const std::vector<MuonTypeMetadata>& arg_types,
    const CefV8ValueList& arguments,
    CefRefPtr<CefListValue> encoded_args,
    std::vector<MuonSharedBufferSource>* shared_sources,
    std::string* error_message) {
  if (arguments.size() != arg_types.size()) {
    *error_message = "Invalid argument count for " + function_name;
    return false;
  }

  encoded_args->SetSize(arguments.size());
  for (auto index = size_t{0}; index < arguments.size(); ++index) {
    const auto expected_type = arg_types[index];
    const auto argument = arguments[index];
    switch (expected_type.type) {
      case MUON_TYPE_BOOL:
        if (!argument->IsBool()) {
          *error_message = "Invalid argument " + std::to_string(index) +
                           ": expected bool";
          return false;
        }
        encoded_args->SetBool(index, argument->GetBoolValue());
        break;
      case MUON_TYPE_I8: {
        auto value = 0.0;
        if (!GetNumericV8Value(argument, &value) || !std::isfinite(value) ||
            std::trunc(value) != value ||
            value < static_cast<double>(std::numeric_limits<int8_t>::min()) ||
            value > static_cast<double>(std::numeric_limits<int8_t>::max())) {
          *error_message = "Invalid argument " + std::to_string(index) +
                           ": expected i8";
          return false;
        }
        encoded_args->SetInt(index, static_cast<int>(value));
        break;
      }
      case MUON_TYPE_U8: {
        auto value = 0.0;
        if (!GetNumericV8Value(argument, &value) || !std::isfinite(value) ||
            std::trunc(value) != value || value < 0.0 ||
            value > static_cast<double>(std::numeric_limits<uint8_t>::max())) {
          *error_message = "Invalid argument " + std::to_string(index) +
                           ": expected u8";
          return false;
        }
        encoded_args->SetInt(index, static_cast<int>(value));
        break;
      }
      case MUON_TYPE_I16: {
        auto value = 0.0;
        if (!GetNumericV8Value(argument, &value) || !std::isfinite(value) ||
            std::trunc(value) != value ||
            value < static_cast<double>(std::numeric_limits<int16_t>::min()) ||
            value > static_cast<double>(std::numeric_limits<int16_t>::max())) {
          *error_message = "Invalid argument " + std::to_string(index) +
                           ": expected i16";
          return false;
        }
        encoded_args->SetInt(index, static_cast<int>(value));
        break;
      }
      case MUON_TYPE_U16: {
        auto value = 0.0;
        if (!GetNumericV8Value(argument, &value) || !std::isfinite(value) ||
            std::trunc(value) != value || value < 0.0 ||
            value > static_cast<double>(std::numeric_limits<uint16_t>::max())) {
          *error_message = "Invalid argument " + std::to_string(index) +
                           ": expected u16";
          return false;
        }
        encoded_args->SetInt(index, static_cast<int>(value));
        break;
      }
      case MUON_TYPE_I32: {
        auto value = 0.0;
        if (!GetNumericV8Value(argument, &value) || !std::isfinite(value) ||
            std::trunc(value) != value ||
            value < static_cast<double>(std::numeric_limits<int32_t>::min()) ||
            value > static_cast<double>(std::numeric_limits<int32_t>::max())) {
          *error_message = "Invalid argument " + std::to_string(index) +
                           ": expected i32";
          return false;
        }
        encoded_args->SetInt(index, static_cast<int>(value));
        break;
      }
      case MUON_TYPE_U32: {
        auto value = 0.0;
        if (!GetNumericV8Value(argument, &value) || !std::isfinite(value) ||
            std::trunc(value) != value || value < 0.0 ||
            value > static_cast<double>(std::numeric_limits<uint32_t>::max())) {
          *error_message = "Invalid argument " + std::to_string(index) +
                           ": expected u32";
          return false;
        }
        encoded_args->SetDouble(index, value);
        break;
      }
      case MUON_TYPE_I64: {
        auto value = int64_t{0};
        if (!GetNodeApiInt64V8Value(argument, &value)) {
          *error_message = "Invalid argument " + std::to_string(index) +
                           ": expected i64";
          return false;
        }
        encoded_args->SetString(index, std::to_string(value));
        break;
      }
      case MUON_TYPE_U64: {
        auto value = int64_t{0};
        if (!GetNodeApiInt64V8Value(argument, &value)) {
          *error_message = "Invalid argument " + std::to_string(index) +
                           ": expected u64";
          return false;
        }
        encoded_args->SetString(
            index, std::to_string(ReinterpretInt64AsUInt64(value)));
        break;
      }
      case MUON_TYPE_F32: {
        auto value = 0.0;
        if (!GetNumericV8Value(argument, &value) || !std::isfinite(value) ||
            value < -static_cast<double>(std::numeric_limits<float>::max()) ||
            value > static_cast<double>(std::numeric_limits<float>::max())) {
          *error_message = "Invalid argument " + std::to_string(index) +
                           ": expected f32";
          return false;
        }
        encoded_args->SetDouble(index, value);
        break;
      }
      case MUON_TYPE_F64: {
        auto value = 0.0;
        if (!GetNumericV8Value(argument, &value) || !std::isfinite(value)) {
          *error_message = "Invalid argument " + std::to_string(index) +
                           ": expected f64";
          return false;
        }
        encoded_args->SetDouble(index, value);
        break;
      }
      case MUON_TYPE_POINTER: {
        auto value = 0.0;
        if (!GetPointerNumberV8Value(argument, &value)) {
          *error_message = "Invalid argument " + std::to_string(index) +
                           ": expected pointer";
          return false;
        }
        encoded_args->SetDouble(index, value);
        break;
      }
      case MUON_TYPE_STRING:
        if (argument->IsNull() || argument->IsUndefined()) {
          encoded_args->SetNull(index);
          break;
        }
        if (!argument->IsString()) {
          *error_message = "Invalid argument " + std::to_string(index) +
                           ": expected string";
          return false;
        }
        {
          const auto string_value = argument->GetStringValue().ToString();
          if (ContainsMuonNulCharacter(string_value)) {
            *error_message = "Invalid argument " + std::to_string(index) +
                             ": string must not contain NUL";
            return false;
          }
          encoded_args->SetString(index, string_value);
        }
        break;
      case MUON_TYPE_FUNCTION: {
        if (argument->IsNull() || argument->IsUndefined()) {
          encoded_args->SetNull(index);
          break;
        }
        if (!argument->IsFunction()) {
          *error_message = "Invalid argument " + std::to_string(index) +
                           ": expected function";
          return false;
        }
        auto encoded_function = CefDictionaryValue::Create();
        if (!EncodeFunctionArgument(expected_type, argument, encoded_function)) {
          *error_message = "Invalid argument " + std::to_string(index) +
                           ": function type mismatch";
          return false;
        }
        encoded_args->SetDictionary(index, encoded_function);
        break;
      }
      case MUON_TYPE_BUFFER_VIEW: {
        void* data = nullptr;
        auto size = size_t{0};
        if (!GetV8BufferView(argument, &data, &size)) {
          *error_message = "Invalid argument " + std::to_string(index) +
                           ": expected buffer_view";
          return false;
        }
        shared_sources->push_back({index, data, size});
        encoded_args->SetNull(index);
        break;
      }
      case MUON_TYPE_VOID:
        *error_message = "Void arguments are not supported";
        return false;
      default:
        *error_message = "Unsupported argument type";
        return false;
    }
  }

  return true;
}

bool MuonV8Handler::EncodeFunctionArgument(
    const MuonTypeMetadata& expected_type,
    CefRefPtr<CefV8Value> argument,
    CefRefPtr<CefDictionaryValue> encoded_function) {
  for (const auto& reference : function_references_) {
    if (reference.plugin_proxy && reference.function &&
        reference.function->IsSame(argument)) {
      if (!AreEqualMuonTypes(reference.function_type, expected_type)) {
        return false;
      }
      encoded_function->SetString(kMuonFunctionArgumentKindKey,
                                  kMuonFunctionArgumentKindPluginProxy);
      encoded_function->SetInt(kMuonFunctionArgumentProxyIdKey,
                               static_cast<int>(reference.proxy_id));
      encoded_function->SetString(kMuonFunctionArgumentTypeKey,
                                  CreateMuonTypeCanonicalKey(expected_type));
      encoded_function->SetDictionary(
          kMuonFunctionArgumentTypeDescriptorKey,
          CreateMuonTypeMetadataDictionary(expected_type));
      return true;
    }
  }

  const auto function_id = GetOrCreateFunctionId(argument);
  encoded_function->SetInt(kMuonFunctionArgumentContextIdKey, context_id_);
  encoded_function->SetInt(kMuonFunctionArgumentFunctionIdKey, function_id);
  encoded_function->SetString(kMuonFunctionArgumentTypeKey,
                              CreateMuonTypeCanonicalKey(expected_type));
  encoded_function->SetDictionary(kMuonFunctionArgumentTypeDescriptorKey,
                                  CreateMuonTypeMetadataDictionary(
                                      expected_type));
  return true;
}

void MuonV8Handler::RejectPromise(CefRefPtr<CefV8Value> promise,
                                   const std::string& error_message) const {
  promise->RejectPromise(error_message);
}

int MuonV8Handler::GetOrCreateFunctionId(CefRefPtr<CefV8Value> function) {
  for (const auto& reference : function_references_) {
    if (!reference.plugin_proxy && reference.function &&
        reference.function->IsSame(function)) {
      return reference.id;
    }
  }

  FunctionReference reference;
  reference.id = next_function_id_;
  next_function_id_ += 1;
  reference.function = function;
  function_references_.push_back(reference);
  return reference.id;
}

bool MuonV8Handler::HandleRendererFunctionCallMessage(
    CefRefPtr<CefProcessMessage> message) {
  CEF_REQUIRE_RENDERER_THREAD();
  if (!message || message->GetName().ToString() !=
                      kMuonRendererFunctionCallMessageName) {
    return false;
  }

  const auto message_args = message->GetArgumentList();
  if (!message_args || message_args->GetSize() < 5) {
    return true;
  }
  const auto call_id = message_args->GetInt(0);
  const auto encoded_args = message_args->GetList(3);
  const auto needs_shared =
      encoded_args && CefListValueHasMuonSharedBufferPlaceholders(encoded_args);
  if (needs_shared) {
    const auto payload_iterator =
        pending_renderer_function_call_payloads_.find(call_id);
    if (payload_iterator == pending_renderer_function_call_payloads_.end()) {
      pending_renderer_function_call_messages_[call_id] = message;
      return true;
    }
    const auto pending_payload = payload_iterator->second;
    pending_renderer_function_call_payloads_.erase(payload_iterator);
    if (pending_payload.has_error) {
      SendFunctionResult(call_id, CreateMuonPrimitiveType(MUON_TYPE_VOID),
                         nullptr, pending_payload.error_message);
      return true;
    }
    return InvokeRendererFunctionCallMessage(message, pending_payload.payload);
  }
  return InvokeRendererFunctionCallMessage(message, nullptr);
}

bool MuonV8Handler::HandleRendererFunctionCallSharedMessage(
    CefRefPtr<CefProcessMessage> message) {
  CEF_REQUIRE_RENDERER_THREAD();
  if (!message || message->GetName().ToString() !=
                      kMuonRendererFunctionCallSharedMessageName) {
    return false;
  }
  auto call_id = 0;
  std::shared_ptr<MuonSharedBufferPayload> payload;
  std::string error_message;
  const auto decoded = DecodeMuonSharedBufferPayload(
      message, &call_id, &payload, &error_message);
  const auto metadata_iterator =
      pending_renderer_function_call_messages_.find(call_id);
  if (metadata_iterator != pending_renderer_function_call_messages_.end()) {
    const auto metadata = metadata_iterator->second;
    pending_renderer_function_call_messages_.erase(metadata_iterator);
    if (!decoded) {
      SendFunctionResult(call_id, CreateMuonPrimitiveType(MUON_TYPE_VOID),
                         nullptr, error_message);
      return true;
    }
    return InvokeRendererFunctionCallMessage(metadata, payload);
  }

  PendingSharedPayload pending_payload;
  pending_payload.payload = payload;
  pending_payload.error_message = error_message;
  pending_payload.has_error = !decoded;
  pending_renderer_function_call_payloads_[call_id] = pending_payload;
  return true;
}

bool MuonV8Handler::InvokeRendererFunctionCallMessage(
    CefRefPtr<CefProcessMessage> message,
    std::shared_ptr<MuonSharedBufferPayload> shared_payload) {
  const auto message_args = message->GetArgumentList();
  if (!message_args || message_args->GetSize() < 5) {
    return true;
  }
  const auto call_id = message_args->GetInt(0);
  if (message_args->GetInt(1) != context_id_) {
    return true;
  }

  const auto function_id = message_args->GetInt(2);
  CefRefPtr<CefV8Value> function;
  for (const auto& reference : function_references_) {
    if (!reference.plugin_proxy && reference.id == function_id) {
      function = reference.function;
      break;
    }
  }
  if (!function || !function->IsFunction()) {
    SendFunctionResult(call_id, CreateMuonPrimitiveType(MUON_TYPE_VOID),
                       nullptr, "Renderer function is unavailable");
    return true;
  }

  MuonTypeMetadata function_type;
  if (!ReadMuonTypeMetadataDictionary(message_args->GetDictionary(4), false,
                                       &function_type) ||
      function_type.type != MUON_TYPE_FUNCTION ||
      function_type.function_return_type.empty()) {
    SendFunctionResult(call_id, CreateMuonPrimitiveType(MUON_TYPE_VOID),
                       nullptr, "Renderer function type is invalid");
    return true;
  }

  const auto encoded_args = message_args->GetList(3);
  if (!encoded_args ||
      encoded_args->GetSize() != function_type.function_arg_types.size()) {
    SendFunctionResult(call_id, function_type.function_return_type[0], nullptr,
                       "Renderer function argument count is invalid");
    return true;
  }

  if (!context_ || !context_->IsValid() || !context_->Enter()) {
    SendFunctionResult(call_id, function_type.function_return_type[0], nullptr,
                       "Renderer context is unavailable");
    return true;
  }

  CefV8ValueList v8_args;
  v8_args.reserve(function_type.function_arg_types.size());
  for (auto index = size_t{0}; index < function_type.function_arg_types.size();
       ++index) {
    v8_args.push_back(CreateV8ValueFromEncodedValue(
        function_type.function_arg_types[index], encoded_args, index,
        shared_payload));
  }

  const auto retval = function->ExecuteFunctionWithContext(
      context_, context_->GetGlobal(), v8_args);
  const auto return_type = function_type.function_return_type[0];
  SendFunctionResult(call_id, return_type, retval,
                     retval ? CefString() : CefString("Renderer function failed"));
  context_->Exit();
  return true;
}

bool MuonV8Handler::SendFunctionResult(int call_id,
                                        const MuonTypeMetadata& return_type,
                                        CefRefPtr<CefV8Value> value,
                                        const CefString& exception) {
  const auto frame = context_ ? context_->GetFrame() : nullptr;
  if (!frame) {
    return false;
  }

  const auto message =
      CefProcessMessage::Create(kMuonRendererFunctionResultMessageName);
  const auto args = message->GetArgumentList();
  args->SetSize(4);
  args->SetInt(0, call_id);
  if (!exception.empty()) {
    args->SetBool(1, false);
    args->SetString(2, exception);
    args->SetNull(3);
    frame->SendProcessMessage(PID_BROWSER, message);
    return true;
  }

  args->SetBool(1, true);
  args->SetInt(2, static_cast<int>(return_type.type));
  MuonCreatedSharedBufferMessage shared_message;
  switch (return_type.type) {
    case MUON_TYPE_VOID:
      args->SetNull(3);
      break;
    case MUON_TYPE_BOOL:
      if (!value || !value->IsBool()) {
        args->SetBool(1, false);
        args->SetString(2, "Renderer function returned a non-bool value");
        args->SetNull(3);
        break;
      }
      args->SetBool(3, value->GetBoolValue());
      break;
    case MUON_TYPE_I8: {
      auto number = 0.0;
      if (!value || !GetNumericV8Value(value, &number) ||
          !std::isfinite(number) || std::trunc(number) != number ||
          number < static_cast<double>(std::numeric_limits<int8_t>::min()) ||
          number > static_cast<double>(std::numeric_limits<int8_t>::max())) {
        args->SetBool(1, false);
        args->SetString(2, "Renderer function returned a non-i8 value");
        args->SetNull(3);
        break;
      }
      args->SetInt(3, static_cast<int>(number));
      break;
    }
    case MUON_TYPE_U8: {
      auto number = 0.0;
      if (!value || !GetNumericV8Value(value, &number) ||
          !std::isfinite(number) || std::trunc(number) != number ||
          number < 0.0 ||
          number > static_cast<double>(std::numeric_limits<uint8_t>::max())) {
        args->SetBool(1, false);
        args->SetString(2, "Renderer function returned a non-u8 value");
        args->SetNull(3);
        break;
      }
      args->SetInt(3, static_cast<int>(number));
      break;
    }
    case MUON_TYPE_I16: {
      auto number = 0.0;
      if (!value || !GetNumericV8Value(value, &number) ||
          !std::isfinite(number) || std::trunc(number) != number ||
          number < static_cast<double>(std::numeric_limits<int16_t>::min()) ||
          number > static_cast<double>(std::numeric_limits<int16_t>::max())) {
        args->SetBool(1, false);
        args->SetString(2, "Renderer function returned a non-i16 value");
        args->SetNull(3);
        break;
      }
      args->SetInt(3, static_cast<int>(number));
      break;
    }
    case MUON_TYPE_U16: {
      auto number = 0.0;
      if (!value || !GetNumericV8Value(value, &number) ||
          !std::isfinite(number) || std::trunc(number) != number ||
          number < 0.0 ||
          number > static_cast<double>(std::numeric_limits<uint16_t>::max())) {
        args->SetBool(1, false);
        args->SetString(2, "Renderer function returned a non-u16 value");
        args->SetNull(3);
        break;
      }
      args->SetInt(3, static_cast<int>(number));
      break;
    }
    case MUON_TYPE_I32: {
      auto number = 0.0;
      if (!value || !GetNumericV8Value(value, &number) ||
          !std::isfinite(number) || std::trunc(number) != number ||
          number < static_cast<double>(std::numeric_limits<int32_t>::min()) ||
          number > static_cast<double>(std::numeric_limits<int32_t>::max())) {
        args->SetBool(1, false);
        args->SetString(2, "Renderer function returned a non-i32 value");
        args->SetNull(3);
        break;
      }
      args->SetInt(3, static_cast<int>(number));
      break;
    }
    case MUON_TYPE_U32: {
      auto number = 0.0;
      if (!value || !GetNumericV8Value(value, &number) ||
          !std::isfinite(number) || std::trunc(number) != number ||
          number < 0.0 ||
          number > static_cast<double>(std::numeric_limits<uint32_t>::max())) {
        args->SetBool(1, false);
        args->SetString(2, "Renderer function returned a non-u32 value");
        args->SetNull(3);
        break;
      }
      args->SetDouble(3, number);
      break;
    }
    case MUON_TYPE_I64: {
      auto number = int64_t{0};
      if (!GetNodeApiInt64V8Value(value, &number)) {
        args->SetBool(1, false);
        args->SetString(2, "Renderer function returned a non-i64 value");
        args->SetNull(3);
        break;
      }
      args->SetString(3, std::to_string(number));
      break;
    }
    case MUON_TYPE_U64: {
      auto number = int64_t{0};
      if (!GetNodeApiInt64V8Value(value, &number)) {
        args->SetBool(1, false);
        args->SetString(2, "Renderer function returned a non-u64 value");
        args->SetNull(3);
        break;
      }
      args->SetString(3, std::to_string(ReinterpretInt64AsUInt64(number)));
      break;
    }
    case MUON_TYPE_F32: {
      auto number = 0.0;
      if (!value || !GetNumericV8Value(value, &number) ||
          !std::isfinite(number) ||
          number < -static_cast<double>(std::numeric_limits<float>::max()) ||
          number > static_cast<double>(std::numeric_limits<float>::max())) {
        args->SetBool(1, false);
        args->SetString(2, "Renderer function returned a non-f32 value");
        args->SetNull(3);
        break;
      }
      args->SetDouble(3, number);
      break;
    }
    case MUON_TYPE_F64: {
      auto number = 0.0;
      if (!value || !GetNumericV8Value(value, &number) ||
          !std::isfinite(number)) {
        args->SetBool(1, false);
        args->SetString(2, "Renderer function returned a non-f64 value");
        args->SetNull(3);
        break;
      }
      args->SetDouble(3, number);
      break;
    }
    case MUON_TYPE_POINTER: {
      auto number = 0.0;
      if (!GetPointerNumberV8Value(value, &number)) {
        args->SetBool(1, false);
        args->SetString(2, "Renderer function returned a non-pointer value");
        args->SetNull(3);
        break;
      }
      args->SetDouble(3, number);
      break;
    }
    case MUON_TYPE_STRING:
      if (value && (value->IsNull() || value->IsUndefined())) {
        args->SetNull(3);
        break;
      }
      if (!value || !value->IsString()) {
        args->SetBool(1, false);
        args->SetString(2, "Renderer function returned a non-string value");
        args->SetNull(3);
        break;
      }
      {
        const auto string_value = value->GetStringValue().ToString();
        if (ContainsMuonNulCharacter(string_value)) {
          args->SetBool(1, false);
          args->SetString(2,
                          "Renderer function returned a string containing NUL");
          args->SetNull(3);
          break;
        }
        args->SetString(3, string_value);
      }
      break;
    case MUON_TYPE_FUNCTION: {
      if (value && (value->IsNull() || value->IsUndefined())) {
        args->SetNull(3);
        break;
      }
      if (!value || !value->IsFunction()) {
        args->SetBool(1, false);
        args->SetString(2, "Renderer function returned a non-function value");
        args->SetNull(3);
        break;
      }
      const auto encoded_function = CefDictionaryValue::Create();
      if (!EncodeFunctionArgument(return_type, value, encoded_function)) {
        args->SetBool(1, false);
        args->SetString(2, "Renderer function returned a function with an incompatible type");
        args->SetNull(3);
        break;
      }
      args->SetDictionary(3, encoded_function);
      break;
    }
    case MUON_TYPE_BUFFER_VIEW: {
      void* data = nullptr;
      auto size = size_t{0};
      std::string error_message;
      if (!GetV8BufferView(value, &data, &size)) {
        args->SetBool(1, false);
        args->SetString(2,
                        "Renderer function returned a non-buffer_view value");
        args->SetNull(3);
        break;
      }
      const auto sources = std::vector<MuonSharedBufferSource>{
          {3, data, size},
      };
      if (!CreateMuonSharedBufferMessage(
              kMuonRendererFunctionResultSharedMessageName, call_id, 0,
              sources, &shared_message, &error_message)) {
        args->SetBool(1, false);
        args->SetString(2, error_message);
        args->SetNull(3);
        break;
      }
      MuonSharedBufferEntry entry;
      if (!FindMuonSharedBufferEntry(shared_message.entries, 3, &entry)) {
        args->SetBool(1, false);
        args->SetString(2, "Renderer function buffer_view payload is missing");
        args->SetNull(3);
        break;
      }
      args->SetDictionary(3, CreateMuonSharedBufferPlaceholder(entry));
      break;
    }
    default:
      args->SetBool(1, false);
      args->SetString(2, "Renderer function return type is unsupported");
      args->SetNull(3);
      break;
  }
  if (return_type.type == MUON_TYPE_BUFFER_VIEW && args->GetBool(1) &&
      shared_message.message) {
    frame->SendProcessMessage(PID_BROWSER, shared_message.message);
  }
  frame->SendProcessMessage(PID_BROWSER, message);
  return true;
}

CefRefPtr<CefV8Value> MuonV8Handler::CreateV8ValueFromResult(
    const MuonTypeMetadata& return_type,
    CefRefPtr<CefListValue> message_args,
    std::shared_ptr<MuonSharedBufferPayload> shared_payload) {
  switch (return_type.type) {
    case MUON_TYPE_VOID:
      return CefV8Value::CreateUndefined();
    case MUON_TYPE_BOOL:
      return CefV8Value::CreateBool(message_args->GetBool(3));
    case MUON_TYPE_I8:
      return CefV8Value::CreateInt(message_args->GetInt(3));
    case MUON_TYPE_U8:
      return CefV8Value::CreateUInt(
          static_cast<uint32_t>(message_args->GetInt(3)));
    case MUON_TYPE_I16:
      return CefV8Value::CreateInt(message_args->GetInt(3));
    case MUON_TYPE_U16:
      return CefV8Value::CreateUInt(
          static_cast<uint32_t>(message_args->GetInt(3)));
    case MUON_TYPE_I32:
      return CefV8Value::CreateInt(message_args->GetInt(3));
    case MUON_TYPE_U32:
      return CefV8Value::CreateUInt(
          static_cast<uint32_t>(message_args->GetDouble(3)));
    case MUON_TYPE_I64:
      return CreateNodeApiInt64V8ValueFromString(
          message_args->GetString(3).ToString());
    case MUON_TYPE_U64:
      return CreateNodeApiUInt64V8ValueFromString(
          message_args->GetString(3).ToString());
    case MUON_TYPE_F32:
      return CefV8Value::CreateDouble(message_args->GetDouble(3));
    case MUON_TYPE_F64:
      return CefV8Value::CreateDouble(message_args->GetDouble(3));
    case MUON_TYPE_POINTER:
      return CefV8Value::CreateDouble(message_args->GetDouble(3));
    case MUON_TYPE_STRING:
      if (message_args->GetType(3) == VTYPE_NULL) {
        return CefV8Value::CreateNull();
      }
      return CefV8Value::CreateString(message_args->GetString(3));
    case MUON_TYPE_FUNCTION: {
      if (message_args->GetType(3) == VTYPE_NULL) {
        return CefV8Value::CreateNull();
      }
      if (message_args->GetType(3) != VTYPE_DICTIONARY) {
        return CefV8Value::CreateUndefined();
      }
      const auto encoded_function = message_args->GetDictionary(3);
      if (!encoded_function ||
          !encoded_function->HasKey(kMuonFunctionArgumentProxyIdKey)) {
        return CefV8Value::CreateUndefined();
      }
      return GetOrCreatePluginProxyFunction(
          static_cast<uint32_t>(
              encoded_function->GetInt(kMuonFunctionArgumentProxyIdKey)),
          return_type);
    }
    case MUON_TYPE_BUFFER_VIEW:
      return CreateV8ArrayBufferFromSharedPayload(message_args, 3,
                                                  std::move(shared_payload));
    default:
      return CefV8Value::CreateUndefined();
  }
  return CefV8Value::CreateUndefined();
}

CefRefPtr<CefV8Value> MuonV8Handler::CreateV8ValueFromEncodedValue(
    const MuonTypeMetadata& value_type,
    CefRefPtr<CefListValue> values,
    size_t index,
    std::shared_ptr<MuonSharedBufferPayload> shared_payload) {
  switch (value_type.type) {
    case MUON_TYPE_VOID:
      return CefV8Value::CreateUndefined();
    case MUON_TYPE_BOOL:
      return CefV8Value::CreateBool(values->GetBool(index));
    case MUON_TYPE_I8:
      return CefV8Value::CreateInt(values->GetInt(index));
    case MUON_TYPE_U8:
      return CefV8Value::CreateUInt(static_cast<uint32_t>(
          values->GetInt(index)));
    case MUON_TYPE_I16:
      return CefV8Value::CreateInt(values->GetInt(index));
    case MUON_TYPE_U16:
      return CefV8Value::CreateUInt(static_cast<uint32_t>(
          values->GetInt(index)));
    case MUON_TYPE_I32:
      return CefV8Value::CreateInt(values->GetInt(index));
    case MUON_TYPE_U32:
      return CefV8Value::CreateUInt(
          static_cast<uint32_t>(values->GetDouble(index)));
    case MUON_TYPE_I64:
      return CreateNodeApiInt64V8ValueFromString(
          values->GetString(index).ToString());
    case MUON_TYPE_U64:
      return CreateNodeApiUInt64V8ValueFromString(
          values->GetString(index).ToString());
    case MUON_TYPE_F32:
      return CefV8Value::CreateDouble(values->GetDouble(index));
    case MUON_TYPE_F64:
      return CefV8Value::CreateDouble(values->GetDouble(index));
    case MUON_TYPE_POINTER:
      return CefV8Value::CreateDouble(values->GetDouble(index));
    case MUON_TYPE_STRING:
      if (values->GetType(index) == VTYPE_NULL) {
        return CefV8Value::CreateNull();
      }
      return CefV8Value::CreateString(values->GetString(index));
    case MUON_TYPE_FUNCTION: {
      if (values->GetType(index) == VTYPE_NULL) {
        return CefV8Value::CreateNull();
      }
      const auto encoded_function = values->GetDictionary(index);
      if (!encoded_function ||
          !encoded_function->HasKey(kMuonFunctionArgumentProxyIdKey)) {
        return CefV8Value::CreateUndefined();
      }
      auto function_type = value_type;
      const auto type_dictionary = encoded_function->GetDictionary(
          kMuonFunctionArgumentTypeDescriptorKey);
      if (type_dictionary) {
        ReadMuonTypeMetadataDictionary(type_dictionary, false,
                                        &function_type);
      }
      return GetOrCreatePluginProxyFunction(
          static_cast<uint32_t>(
              encoded_function->GetInt(kMuonFunctionArgumentProxyIdKey)),
          function_type);
    }
    case MUON_TYPE_BUFFER_VIEW:
      return CreateV8ArrayBufferFromSharedPayload(values, index,
                                                  std::move(shared_payload));
    default:
      return CefV8Value::CreateUndefined();
  }
  return CefV8Value::CreateUndefined();
}

CefRefPtr<CefV8Value> MuonV8Handler::GetOrCreatePluginProxyFunction(
    uint32_t proxy_id,
    const MuonTypeMetadata& function_type) {
  for (const auto& reference : function_references_) {
    if (reference.plugin_proxy && reference.proxy_id == proxy_id &&
        AreEqualMuonTypes(reference.function_type, function_type)) {
      return reference.function;
    }
  }

  const auto proxy_name = "__muon_proxy_" + std::to_string(proxy_id) + "_" +
                          std::to_string(next_function_id_);
  auto function = CefV8Value::CreateFunction(proxy_name, this);
  FunctionReference reference;
  reference.id = next_function_id_;
  next_function_id_ += 1;
  reference.function = function;
  reference.plugin_proxy = true;
  reference.proxy_id = proxy_id;
  reference.function_type = function_type;
  reference.proxy_name = proxy_name;
  function_references_.push_back(reference);

  ProxyFunction proxy;
  proxy.proxy_id = proxy_id;
  proxy.function_type = function_type;
  proxy_functions_by_name_[proxy_name] = proxy;
  return function;
}
