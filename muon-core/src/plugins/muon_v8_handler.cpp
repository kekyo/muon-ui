/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "plugins/muon_v8_handler.h"

#include "plugins/muon_function_wrapper_lifecycle.h"
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
static constexpr char kMuonFunctionArgumentProxyLeaseTokenKey[] =
    "lease_token";
static constexpr char kMuonFunctionArgumentTypeKey[] = "type_key";
static constexpr char kMuonFunctionArgumentTypeDescriptorKey[] = "type";
static constexpr char kMuonPluginProxyReleasePropertyName[] = "release";
static constexpr char kMuonPluginProxyDispatchFunctionName[] =
    "__muon_plugin_proxy_dispatch";
static constexpr char kMuonPluginProxyMarkerPropertyName[] =
    "__muon_plugin_proxy_marker";
static constexpr char kMuonExecutorSpawnFunctionPath[] = "muon.executor.spawn";
static constexpr char kMuonExecutorLoadLibraryFunctionPath[] =
    "muon.executor.loadLibrary";
static constexpr double kMuonTwoTo63 = 9223372036854775808.0;
static constexpr double kMuonTwoTo64 = 18446744073709551616.0;
#if defined(MUON_TEST_BUILD)
static constexpr char kMuonFunctionWrapperOwnerKey[] = "owner";
static constexpr char kMuonFunctionWrapperGlobalKey[] = "global";
static constexpr char kMuonFunctionWrapperFfiClosuresKey[] = "ffiClosures";
static constexpr char kMuonFunctionWrapperSourcesKey[] = "sources";
static constexpr char kMuonFunctionWrapperBorrowsKey[] = "borrows";
static constexpr char kMuonFunctionWrapperProxiesKey[] = "proxies";
static constexpr char kMuonFunctionWrapperProxyLeasesKey[] = "proxyLeases";
static constexpr char kMuonFunctionWrapperFfiEnabledKey[] = "enabled";
static constexpr char kMuonFunctionWrapperFfiAllocKey[] = "alloc";
static constexpr char kMuonFunctionWrapperFfiFreeKey[] = "free";
static constexpr char kMuonFunctionWrapperFfiLiveKey[] = "live";
static constexpr char kMuonFunctionWrapperFfiHighWaterKey[] = "highWater";
static constexpr double kMuonMaximumSafeJavaScriptInteger =
    9007199254740991.0;
#endif
static int g_next_muon_v8_context_id = 1;
static std::map<const CefBaseRefCounted*, MuonPluginFunctionProxyState*>
    g_muon_plugin_proxy_states_by_user_data;

static int AcquireMuonV8ContextId() {
  if (g_next_muon_v8_context_id <= 0) {
    return 0;
  }
  const auto context_id = g_next_muon_v8_context_id;
  g_next_muon_v8_context_id =
      context_id == std::numeric_limits<int>::max() ? 0 : context_id + 1;
  return context_id;
}

class MuonPluginFunctionProxyState final : public CefBaseRefCounted {
 public:
  MuonPluginFunctionProxyState(
      CefRefPtr<MuonV8Handler> handler,
      CefRefPtr<CefV8Context> context,
      uint32_t proxy_id,
      std::string lease_token,
      MuonTypeMetadata function_type,
      uint64_t wrapper_id)
      : handler_(handler),
        context_(context),
        frame_(context ? context->GetFrame() : nullptr),
        context_id_(handler ? handler->GetContextId() : 0),
        proxy_id_(proxy_id),
        lease_token_(std::move(lease_token)),
        function_type_(std::move(function_type)),
        call_name_("__muon_proxy_" + std::to_string(proxy_id) + "_" +
                   std::to_string(wrapper_id)) {}

  bool Invoke(const CefV8ValueList& arguments,
              CefRefPtr<CefV8Value>& retval) {
    CEF_REQUIRE_RENDERER_THREAD();
    const auto promise = CefV8Value::CreatePromise();
    retval = promise;
    if (released_) {
      promise->RejectPromise("muon function proxy is released");
      return true;
    }
    if (!IsCurrentOwnerContext()) {
      promise->RejectPromise(
          "muon function proxy belongs to another V8 context");
      return true;
    }
    const auto handler = handler_;
    if (!handler) {
      promise->RejectPromise("muon function proxy context is unavailable");
      return true;
    }
    if (function_type_.type != MUON_TYPE_FUNCTION ||
        function_type_.function_return_type.empty()) {
      promise->RejectPromise("muon function proxy type is invalid");
      return true;
    }
    return handler->ExecutePluginCall(
        call_name_, function_type_.function_arg_types,
        function_type_.function_return_type[0], proxy_id_, lease_token_,
        false, std::string{}, std::string{}, arguments, promise);
  }

  bool ReleaseFromJavaScript(CefRefPtr<CefV8Value>& retval,
                             CefString& exception) {
    CEF_REQUIRE_RENDERER_THREAD();
    if (!IsCurrentOwnerContext()) {
      exception = "muon function proxy belongs to another V8 context";
      return true;
    }
    ReleaseLease();
    retval = CefV8Value::CreateUndefined();
    return true;
  }

  const CefBaseRefCounted* GetUserDataKey() const {
    return static_cast<const CefBaseRefCounted*>(this);
  }

  uint32_t GetProxyId() const { return proxy_id_; }

  const std::string& GetLeaseToken() const { return lease_token_; }

  const MuonTypeMetadata& GetFunctionType() const { return function_type_; }

  const std::string& GetCallName() const { return call_name_; }

  bool IsReleased() const { return released_; }

  bool IsCurrentOwnerContext() const {
    const auto current_context = CefV8Context::GetCurrentContext();
    const auto entered_context = CefV8Context::GetEnteredContext();
    return context_ && context_->IsValid() && current_context &&
           current_context->IsValid() && entered_context &&
           entered_context->IsValid() && context_->IsSame(current_context) &&
           context_->IsSame(entered_context);
  }

  void ReleaseForCreationFailure() {
    ReleaseLease();
    handler_ = nullptr;
    context_ = nullptr;
    frame_ = nullptr;
  }

  void ReleaseForContextRelease() {
    ReleaseLease();
    handler_ = nullptr;
    context_ = nullptr;
    frame_ = nullptr;
  }

 private:
  ~MuonPluginFunctionProxyState() override {
    ReleaseLease();
    const auto handler = handler_;
    if (handler) {
      handler->UnregisterPluginProxyState(GetUserDataKey(), this);
      return;
    }
    const auto iterator =
        g_muon_plugin_proxy_states_by_user_data.find(GetUserDataKey());
    if (iterator != g_muon_plugin_proxy_states_by_user_data.end() &&
        iterator->second == this) {
      g_muon_plugin_proxy_states_by_user_data.erase(iterator);
    }
  }

  void ReleaseLease() {
    if (released_) {
      return;
    }
    released_ = true;
    if (!frame_ || !frame_->IsValid()) {
      return;
    }
    const auto message =
        CefProcessMessage::Create(kMuonPluginProxyReleaseMessageName);
    if (!message) {
      return;
    }
    const auto args = message->GetArgumentList();
    if (!args) {
      return;
    }
    args->SetSize(3);
    args->SetInt(0, context_id_);
    args->SetInt(1, static_cast<int>(proxy_id_));
    args->SetString(2, lease_token_);
    frame_->SendProcessMessage(PID_BROWSER, message);
  }

  CefRefPtr<MuonV8Handler> handler_;
  CefRefPtr<CefV8Context> context_;
  CefRefPtr<CefFrame> frame_;
  int context_id_ = 0;
  uint32_t proxy_id_ = 0;
  std::string lease_token_;
  MuonTypeMetadata function_type_ = CreateMuonPrimitiveType(MUON_TYPE_VOID);
  std::string call_name_;
  bool released_ = false;

  IMPLEMENT_REFCOUNTING(MuonPluginFunctionProxyState);
  DISALLOW_COPY_AND_ASSIGN(MuonPluginFunctionProxyState);
};

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

#if defined(MUON_TEST_BUILD)
static bool ReadMuonDiagnosticCount(
    CefRefPtr<CefDictionaryValue> dictionary,
    const char* key,
    double* count) {
  if (!dictionary || key == nullptr || count == nullptr) {
    return false;
  }
  const auto type = dictionary->GetType(key);
  auto value = 0.0;
  if (type == VTYPE_INT) {
    value = static_cast<double>(dictionary->GetInt(key));
  } else if (type == VTYPE_DOUBLE) {
    value = dictionary->GetDouble(key);
  } else {
    return false;
  }
  if (!std::isfinite(value) || std::trunc(value) != value || value < 0.0 ||
      value > kMuonMaximumSafeJavaScriptInteger) {
    return false;
  }
  *count = value;
  return true;
}

static bool SetMuonDiagnosticCount(
    CefRefPtr<CefV8Value> object,
    const char* key,
    double count) {
  return object && key != nullptr &&
         object->SetValue(key, CefV8Value::CreateDouble(count),
                          V8_PROPERTY_ATTRIBUTE_NONE);
}

static CefRefPtr<CefV8Value> CreateMuonFunctionWrapperCountObject(
    CefRefPtr<CefDictionaryValue> dictionary,
    std::string* error_message) {
  if (!dictionary || dictionary->GetSize() != 4 ||
      error_message == nullptr) {
    if (error_message != nullptr) {
      *error_message = "Invalid function wrapper diagnostic count schema";
    }
    return nullptr;
  }
  auto sources = 0.0;
  auto borrows = 0.0;
  auto proxies = 0.0;
  auto proxy_leases = 0.0;
  if (!ReadMuonDiagnosticCount(dictionary, kMuonFunctionWrapperSourcesKey,
                               &sources) ||
      !ReadMuonDiagnosticCount(dictionary, kMuonFunctionWrapperBorrowsKey,
                               &borrows) ||
      !ReadMuonDiagnosticCount(dictionary, kMuonFunctionWrapperProxiesKey,
                               &proxies) ||
      !ReadMuonDiagnosticCount(dictionary,
                               kMuonFunctionWrapperProxyLeasesKey,
                               &proxy_leases)) {
    *error_message = "Invalid function wrapper diagnostic count schema";
    return nullptr;
  }

  const auto result = CefV8Value::CreateObject(nullptr, nullptr);
  if (!result ||
      !SetMuonDiagnosticCount(result, kMuonFunctionWrapperSourcesKey,
                              sources) ||
      !SetMuonDiagnosticCount(result, kMuonFunctionWrapperBorrowsKey,
                              borrows) ||
      !SetMuonDiagnosticCount(result, kMuonFunctionWrapperProxiesKey,
                              proxies) ||
      !SetMuonDiagnosticCount(result, kMuonFunctionWrapperProxyLeasesKey,
                              proxy_leases)) {
    *error_message = "Failed to create function wrapper diagnostic counts";
    return nullptr;
  }
  return result;
}

static CefRefPtr<CefV8Value> CreateMuonFfiClosureDiagnosticObject(
    CefRefPtr<CefDictionaryValue> dictionary,
    std::string* error_message) {
  if (!dictionary || dictionary->GetSize() != 5 ||
      dictionary->GetType(kMuonFunctionWrapperFfiEnabledKey) != VTYPE_BOOL ||
      error_message == nullptr) {
    if (error_message != nullptr) {
      *error_message = "Invalid FFI closure diagnostic schema";
    }
    return nullptr;
  }
  auto alloc = 0.0;
  auto released = 0.0;
  auto live = 0.0;
  auto high_water = 0.0;
  if (!ReadMuonDiagnosticCount(dictionary, kMuonFunctionWrapperFfiAllocKey,
                               &alloc) ||
      !ReadMuonDiagnosticCount(dictionary, kMuonFunctionWrapperFfiFreeKey,
                               &released) ||
      !ReadMuonDiagnosticCount(dictionary, kMuonFunctionWrapperFfiLiveKey,
                               &live) ||
      !ReadMuonDiagnosticCount(dictionary,
                               kMuonFunctionWrapperFfiHighWaterKey,
                               &high_water)) {
    *error_message = "Invalid FFI closure diagnostic schema";
    return nullptr;
  }
  if (alloc < released || live != alloc - released || high_water < live) {
    *error_message = "Inconsistent FFI closure diagnostic counts";
    return nullptr;
  }

  const auto result = CefV8Value::CreateObject(nullptr, nullptr);
  if (!result ||
      !result->SetValue(
          kMuonFunctionWrapperFfiEnabledKey,
          CefV8Value::CreateBool(
              dictionary->GetBool(kMuonFunctionWrapperFfiEnabledKey)),
          V8_PROPERTY_ATTRIBUTE_NONE) ||
      !SetMuonDiagnosticCount(result, kMuonFunctionWrapperFfiAllocKey,
                              alloc) ||
      !SetMuonDiagnosticCount(result, kMuonFunctionWrapperFfiFreeKey,
                              released) ||
      !SetMuonDiagnosticCount(result, kMuonFunctionWrapperFfiLiveKey,
                              live) ||
      !SetMuonDiagnosticCount(result, kMuonFunctionWrapperFfiHighWaterKey,
                              high_water)) {
    *error_message = "Failed to create FFI closure diagnostics";
    return nullptr;
  }
  return result;
}

static CefRefPtr<CefV8Value> CreateMuonFunctionWrapperDiagnosticObject(
    CefRefPtr<CefDictionaryValue> dictionary,
    std::string* error_message) {
  if (!dictionary || dictionary->GetSize() != 3 ||
      dictionary->GetType(kMuonFunctionWrapperOwnerKey) != VTYPE_DICTIONARY ||
      dictionary->GetType(kMuonFunctionWrapperGlobalKey) != VTYPE_DICTIONARY ||
      dictionary->GetType(kMuonFunctionWrapperFfiClosuresKey) !=
          VTYPE_DICTIONARY ||
      error_message == nullptr) {
    if (error_message != nullptr) {
      *error_message = "Invalid function wrapper diagnostic schema";
    }
    return nullptr;
  }
  const auto owner = CreateMuonFunctionWrapperCountObject(
      dictionary->GetDictionary(kMuonFunctionWrapperOwnerKey), error_message);
  const auto global = CreateMuonFunctionWrapperCountObject(
      dictionary->GetDictionary(kMuonFunctionWrapperGlobalKey), error_message);
  const auto ffi_closures = CreateMuonFfiClosureDiagnosticObject(
      dictionary->GetDictionary(kMuonFunctionWrapperFfiClosuresKey),
      error_message);
  if (!owner || !global || !ffi_closures) {
    return nullptr;
  }

  const auto result = CefV8Value::CreateObject(nullptr, nullptr);
  if (!result ||
      !result->SetValue(kMuonFunctionWrapperOwnerKey, owner,
                        V8_PROPERTY_ATTRIBUTE_NONE) ||
      !result->SetValue(kMuonFunctionWrapperGlobalKey, global,
                        V8_PROPERTY_ATTRIBUTE_NONE) ||
      !result->SetValue(kMuonFunctionWrapperFfiClosuresKey, ffi_closures,
                        V8_PROPERTY_ATTRIBUTE_NONE)) {
    *error_message = "Failed to create function wrapper diagnostics";
    return nullptr;
  }
  return result;
}
#endif

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
// muon therefore mirrors the Node-API int64 number boundary:
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

MuonV8Handler::FunctionTransfers::FunctionTransfers(MuonV8Handler* owner)
    : owner(owner) {}

MuonV8Handler::FunctionTransfers::~FunctionTransfers() {
  Reset();
}

MuonV8Handler::FunctionTransfers::FunctionTransfers(
    FunctionTransfers&& other) noexcept
    : owner(other.owner), function_ids(std::move(other.function_ids)) {
  other.owner = nullptr;
  other.function_ids.clear();
}

MuonV8Handler::FunctionTransfers&
MuonV8Handler::FunctionTransfers::operator=(
    FunctionTransfers&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  Reset();
  owner = other.owner;
  function_ids = std::move(other.function_ids);
  other.owner = nullptr;
  other.function_ids.clear();
  return *this;
}

void MuonV8Handler::FunctionTransfers::Add(int function_id) {
  function_ids.push_back(function_id);
}

void MuonV8Handler::FunctionTransfers::Reset() {
  if (owner != nullptr) {
    for (const auto function_id : function_ids) {
      owner->ReleaseFunctionTransfer(function_id);
    }
  }
  owner = nullptr;
  function_ids.clear();
}

bool MuonV8Handler::FunctionTransfers::Empty() const {
  return function_ids.empty();
}

MuonV8Handler::MuonV8Handler(std::vector<MuonFunctionMetadata> functions,
                               CefRefPtr<CefV8Context> context)
    : functions_(std::move(functions)),
      context_(context),
      context_id_(AcquireMuonV8ContextId()) {
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
  (void)object;
  const auto v8_name = name.ToString();
  if (v8_name == kMuonPluginProxyDispatchFunctionName) {
    if (arguments.size() < 2 || !arguments[0] ||
        !arguments[0]->IsObject() || !arguments[1] ||
        !arguments[1]->IsBool()) {
      exception = "Invalid muon function proxy invocation";
      return true;
    }
    const auto state = FindPluginProxyStateFromMarker(arguments[0]);
    if (state == nullptr) {
      exception = "Unknown muon function proxy";
      return true;
    }
    if (arguments[1]->GetBoolValue()) {
      if (arguments.size() != 2) {
        exception = "Invalid muon function proxy release";
        return true;
      }
      return state->ReleaseFromJavaScript(retval, exception);
    }
    CefV8ValueList call_arguments;
    call_arguments.reserve(arguments.size() - 2);
    for (auto index = size_t{2}; index < arguments.size(); ++index) {
      call_arguments.push_back(arguments[index]);
    }
    return state->Invoke(call_arguments, retval);
  }

#if defined(MUON_TEST_BUILD)
  if (v8_name == kMuonV8FunctionWrapperDiagnosticsFunctionName) {
    const auto promise = CefV8Value::CreatePromise();
    retval = promise;
    if (!arguments.empty()) {
      RejectPromise(promise,
                    "Function wrapper diagnostics do not accept arguments");
      return true;
    }
    const auto frame = context_ ? context_->GetFrame() : nullptr;
    if (!frame || !frame->IsValid()) {
      RejectPromise(promise, "muon frame is not available");
      return true;
    }
    if (next_diagnostic_request_id_ <= 0) {
      RejectPromise(promise,
                    "Function wrapper diagnostic request ids are exhausted");
      return true;
    }

    const auto request_id = next_diagnostic_request_id_;
    next_diagnostic_request_id_ =
        request_id == std::numeric_limits<int>::max() ? 0 : request_id + 1;
    const auto message = CefProcessMessage::Create(
        kMuonFunctionWrapperDiagnosticsRequestMessageName);
    const auto message_args = message ? message->GetArgumentList() : nullptr;
    if (!message_args) {
      RejectPromise(promise,
                    "Failed to create function wrapper diagnostic request");
      return true;
    }
    message_args->SetSize(2);
    message_args->SetInt(0, context_id_);
    message_args->SetInt(1, request_id);
    pending_diagnostic_promises_[request_id] = promise;
    frame->SendProcessMessage(PID_BROWSER, message);
    return true;
  }
#endif

  auto is_capability_call = v8_name == kMuonV8CapabilityCallFunctionName;
  auto function_iterator = function_indexes_by_v8_name_.find(v8_name);
  if (!is_capability_call &&
      function_iterator == function_indexes_by_v8_name_.end()) {
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
  if (is_capability_call) {
    if (arguments.size() != 3 || !arguments[0] || !arguments[0]->IsString() ||
        !arguments[1] || !arguments[1]->IsString() || !arguments[2] ||
        !arguments[2]->IsArray()) {
      RejectPromise(promise, "Invalid muon capability call");
      return true;
    }
    capability_id = arguments[0]->GetStringValue().ToString();
    capability_function_path = arguments[1]->GetStringValue().ToString();
    if (capability_id.empty() || capability_function_path.empty()) {
      RejectPromise(promise, "Invalid muon capability call");
      return true;
    }
    const auto public_function_iterator =
        function_indexes_by_public_path_.find(capability_function_path);
    if (public_function_iterator == function_indexes_by_public_path_.end()) {
      RejectPromise(promise,
                    "Unknown muon capability function: " +
                        capability_function_path);
      return true;
    }
    function_iterator =
        function_indexes_by_v8_name_.find(CreateMuonV8FunctionName(
            functions_[public_function_iterator->second].id));
    if (function_iterator == function_indexes_by_v8_name_.end()) {
      RejectPromise(promise,
                    "Unknown muon capability function: " +
                        capability_function_path);
      return true;
    }
    call_arguments.clear();
    const auto argument_count = arguments[2]->GetArrayLength();
    for (auto index = 0; index < argument_count; ++index) {
      call_arguments.push_back(arguments[2]->GetValue(index));
    }
  }
  const auto& function = functions_[function_iterator->second];
  function_name = CreateMuonFunctionPublicPath(function);
  arg_types = function.arg_types;
  return_type = function.return_type;
  function_id = function.id;

  return ExecutePluginCall(
      function_name, arg_types, return_type, function_id, std::string{},
      is_capability_call, capability_id, capability_function_path,
      call_arguments, promise);
}

bool MuonV8Handler::ExecutePluginCall(
    const std::string& function_name,
    const std::vector<MuonTypeMetadata>& arg_types,
    const MuonTypeMetadata& return_type,
    uint32_t function_id,
    const std::string& proxy_lease_token,
    bool is_capability_call,
    const std::string& capability_id,
    const std::string& capability_function_path,
    const CefV8ValueList& call_arguments,
    CefRefPtr<CefV8Value> promise) {
  const auto is_proxy_call = !proxy_lease_token.empty();

  if (next_call_id_ <= 0) {
    RejectPromise(promise, "muon call ids are exhausted");
    return true;
  }
  const auto call_id = next_call_id_;
  next_call_id_ =
      call_id == std::numeric_limits<int>::max() ? 0 : call_id + 1;
  const auto encoded_args = CefListValue::Create();
  std::vector<MuonSharedBufferSource> shared_sources;
  FunctionTransfers function_transfers(this);
  std::string error_message;
  if (!ValidateAndEncodeArguments(function_name, arg_types, call_arguments,
                                  encoded_args, &shared_sources,
                                  &function_transfers,
                                  &error_message)) {
    RejectPromise(promise, error_message);
    return true;
  }
  if ((function_name == kMuonExecutorSpawnFunctionPath ||
       function_name == kMuonExecutorLoadLibraryFunctionPath) &&
      encoded_args->GetSize() >= 3) {
    encoded_args->SetDouble(2, static_cast<double>(context_id_));
  }

  const auto context = CefV8Context::GetCurrentContext();
  const auto frame = context ? context->GetFrame() : nullptr;
  if (!frame || !frame->IsValid()) {
    RejectPromise(promise, "muon frame is not available");
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
  pending_promise.function_transfers = std::move(function_transfers);
  pending_promises_.emplace(call_id, std::move(pending_promise));

  const auto message = CefProcessMessage::Create(
      is_proxy_call ? kMuonPluginProxyCallMessageName
                    : kMuonPluginCallMessageName);
  const auto message_args = message->GetArgumentList();
  message_args->SetSize(is_capability_call ? 6 : is_proxy_call ? 5 : 4);
  message_args->SetInt(0, call_id);
  message_args->SetInt(1, static_cast<int>(function_id));
  message_args->SetList(2, encoded_args);
  message_args->SetInt(3, context_id_);
  if (is_proxy_call) {
    message_args->SetString(4, proxy_lease_token);
  } else if (is_capability_call) {
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
        auto pending_promise = std::move(pending_iterator->second);
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
        auto pending_promise = std::move(pending_iterator->second);
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

  auto pending_promise = std::move(pending_iterator->second);
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
        "muon plugin returned an unexpected result type");
    context_->Exit();
    return true;
  }

  pending_promise.promise->ResolvePromise(
      CreateV8ValueFromResult(pending_promise.return_type, message_args,
                              std::move(shared_payload)));
  context_->Exit();
  return true;
}

#if defined(MUON_TEST_BUILD)
bool MuonV8Handler::HandleFunctionWrapperDiagnosticsResultMessage(
    CefRefPtr<CefProcessMessage> message) {
  CEF_REQUIRE_RENDERER_THREAD();
  if (!message || message->GetName().ToString() !=
                      kMuonFunctionWrapperDiagnosticsResultMessageName) {
    return false;
  }

  const auto message_args = message->GetArgumentList();
  if (!message_args || message_args->GetSize() < 2 ||
      message_args->GetType(0) != VTYPE_INT ||
      message_args->GetType(1) != VTYPE_INT ||
      message_args->GetInt(0) != context_id_) {
    return true;
  }
  const auto request_id = message_args->GetInt(1);
  const auto pending_iterator =
      pending_diagnostic_promises_.find(request_id);
  if (pending_iterator == pending_diagnostic_promises_.end()) {
    return true;
  }
  const auto promise = pending_iterator->second;
  pending_diagnostic_promises_.erase(pending_iterator);

  if (!context_ || !context_->IsValid() || !context_->Enter()) {
    return true;
  }
  if (message_args->GetSize() != 5 ||
      message_args->GetType(2) != VTYPE_BOOL ||
      message_args->GetType(3) != VTYPE_DICTIONARY ||
      message_args->GetType(4) != VTYPE_STRING) {
    RejectPromise(promise,
                  "Invalid function wrapper diagnostic result message");
    context_->Exit();
    return true;
  }

  const auto success = message_args->GetBool(2);
  const auto diagnostics = message_args->GetDictionary(3);
  const auto browser_error = message_args->GetString(4).ToString();
  if (!success) {
    if (!diagnostics || diagnostics->GetSize() != 0 ||
        browser_error.empty()) {
      RejectPromise(promise,
                    "Invalid function wrapper diagnostic failure result");
    } else {
      RejectPromise(promise, browser_error);
    }
    context_->Exit();
    return true;
  }
  if (!browser_error.empty()) {
    RejectPromise(promise,
                  "Invalid function wrapper diagnostic success result");
    context_->Exit();
    return true;
  }

  auto error_message = std::string{};
  const auto value = CreateMuonFunctionWrapperDiagnosticObject(
      diagnostics, &error_message);
  if (!value) {
    RejectPromise(promise, error_message);
  } else {
    promise->ResolvePromise(value);
  }
  context_->Exit();
  return true;
}
#endif

void MuonV8Handler::RejectAllPendingPromises() {
  CEF_REQUIRE_RENDERER_THREAD();
  if (!context_ || !context_->IsValid() || !context_->Enter()) {
    pending_promises_.clear();
#if defined(MUON_TEST_BUILD)
    pending_diagnostic_promises_.clear();
#endif
    pending_result_messages_.clear();
    pending_result_payloads_.clear();
    pending_renderer_function_call_messages_.clear();
    pending_renderer_function_call_payloads_.clear();
    return;
  }

  for (const auto& entry : pending_promises_) {
    entry.second.promise->RejectPromise("muon V8 context was released");
  }
  pending_promises_.clear();
#if defined(MUON_TEST_BUILD)
  for (const auto& entry : pending_diagnostic_promises_) {
    entry.second->RejectPromise("muon V8 context was released");
  }
  pending_diagnostic_promises_.clear();
#endif
  pending_result_messages_.clear();
  pending_result_payloads_.clear();
  pending_renderer_function_call_messages_.clear();
  pending_renderer_function_call_payloads_.clear();
  context_->Exit();
}

void MuonV8Handler::ReleaseFunctionReferences() {
  CEF_REQUIRE_RENDERER_THREAD();
  std::vector<CefRefPtr<MuonPluginFunctionProxyState>> proxy_states;
  proxy_states.reserve(plugin_proxy_states_by_user_data_.size());
  for (const auto& entry : plugin_proxy_states_by_user_data_) {
    if (entry.second != nullptr) {
      proxy_states.push_back(entry.second);
    }
  }
  plugin_proxy_states_by_user_data_.clear();
  for (const auto& proxy_state : proxy_states) {
    proxy_state->ReleaseForContextRelease();
  }
  pending_renderer_function_result_transfers_.clear();
  function_references_.clear();
  pending_renderer_function_call_messages_.clear();
  pending_renderer_function_call_payloads_.clear();
#if defined(MUON_TEST_BUILD)
  pending_diagnostic_promises_.clear();
#endif
  plugin_proxy_dispatch_function_ = nullptr;
  plugin_proxy_factory_ = nullptr;
  context_ = nullptr;
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
    FunctionTransfers* function_transfers,
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
        auto function_error = std::string{};
        if (!EncodeFunctionArgument(expected_type, argument, encoded_function,
                                    function_transfers, &function_error)) {
          *error_message = "Invalid argument " + std::to_string(index) +
                           ": " +
                           (function_error.empty()
                                ? "function type mismatch"
                                : function_error);
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
    CefRefPtr<CefDictionaryValue> encoded_function,
    FunctionTransfers* function_transfers,
    std::string* error_message) {
  auto proxy_lookup_error = std::string{};
  const auto proxy_state =
      FindPluginProxyState(argument, &proxy_lookup_error);
  if (!proxy_lookup_error.empty()) {
    *error_message = proxy_lookup_error;
    return false;
  }
  if (proxy_state != nullptr) {
    if (proxy_state->IsReleased()) {
      *error_message = "function proxy is released";
      return false;
    }
    if (!proxy_state->IsCurrentOwnerContext()) {
      *error_message = "function proxy belongs to another V8 context";
      return false;
    }
    if (!AreEqualMuonTypes(proxy_state->GetFunctionType(), expected_type)) {
      *error_message = "function type mismatch";
      return false;
    }
    encoded_function->SetString(kMuonFunctionArgumentKindKey,
                                kMuonFunctionArgumentKindPluginProxy);
    encoded_function->SetInt(kMuonFunctionArgumentProxyIdKey,
                             static_cast<int>(proxy_state->GetProxyId()));
    encoded_function->SetString(kMuonFunctionArgumentProxyLeaseTokenKey,
                                proxy_state->GetLeaseToken());
    encoded_function->SetString(kMuonFunctionArgumentTypeKey,
                                CreateMuonTypeCanonicalKey(expected_type));
    encoded_function->SetDictionary(
        kMuonFunctionArgumentTypeDescriptorKey,
        CreateMuonTypeMetadataDictionary(expected_type));
    return true;
  }

  auto function_id = 0;
  if (!AcquireFunctionTransfer(argument, function_transfers, &function_id,
                               error_message)) {
    return false;
  }
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

bool MuonV8Handler::AcquireFunctionTransfer(
    CefRefPtr<CefV8Value> function,
    FunctionTransfers* function_transfers,
    int* function_id,
    std::string* error_message) {
  if (!function || !function_transfers || !function_id || !error_message) {
    return false;
  }
  for (auto& reference : function_references_) {
    if (reference.function && reference.function->IsSame(function)) {
      if (reference.pending_transfer_count ==
          std::numeric_limits<size_t>::max()) {
        *error_message = "renderer function transfer count is exhausted";
        return false;
      }
      reference.pending_transfer_count += 1;
      function_transfers->Add(reference.id);
      *function_id = reference.id;
      return true;
    }
  }

  if (function_references_.size() >= kMuonRendererFunctionReferenceLimit) {
    *error_message = "renderer function reference limit exceeded";
    return false;
  }
  if (next_function_id_ <= 0) {
    *error_message = "renderer function ids are exhausted";
    return false;
  }
  FunctionReference reference;
  reference.id = next_function_id_;
  next_function_id_ = reference.id == std::numeric_limits<int>::max()
                          ? 0
                          : reference.id + 1;
  reference.function = function;
  reference.pending_transfer_count = 1;
  function_transfers->Add(reference.id);
  *function_id = reference.id;
  function_references_.push_back(reference);
  return true;
}

void MuonV8Handler::ReleaseFunctionTransfer(int function_id) {
  for (auto& reference : function_references_) {
    if (reference.id == function_id) {
      if (reference.pending_transfer_count > 0) {
        reference.pending_transfer_count -= 1;
      }
      break;
    }
  }
  ReleaseFunctionReferenceIfUnused(function_id);
}

void MuonV8Handler::ReleaseFunctionReferenceIfUnused(int function_id) {
  for (auto iterator = function_references_.begin();
       iterator != function_references_.end(); ++iterator) {
    if (iterator->id == function_id) {
      if (iterator->pending_transfer_count == 0 &&
          iterator->source_lease_tokens.empty()) {
        function_references_.erase(iterator);
      }
      return;
    }
  }
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

bool MuonV8Handler::HandleRendererFunctionSourceAcquireMessage(
    CefRefPtr<CefProcessMessage> message) {
  CEF_REQUIRE_RENDERER_THREAD();
  if (!message || message->GetName().ToString() !=
                      kMuonRendererFunctionSourceAcquireMessageName) {
    return false;
  }
  const auto args = message->GetArgumentList();
  if (!args || args->GetSize() != 3 || args->GetType(0) != VTYPE_INT ||
      args->GetType(1) != VTYPE_INT || args->GetType(2) != VTYPE_STRING ||
      args->GetInt(0) != context_id_) {
    return true;
  }
  const auto function_id = args->GetInt(1);
  const auto lease_token = args->GetString(2).ToString();
  if (lease_token.empty()) {
    return true;
  }
  for (auto& reference : function_references_) {
    if (reference.id == function_id) {
      reference.source_lease_tokens.insert(lease_token);
      break;
    }
  }
  return true;
}

bool MuonV8Handler::HandleRendererFunctionSourceReleaseMessage(
    CefRefPtr<CefProcessMessage> message) {
  CEF_REQUIRE_RENDERER_THREAD();
  if (!message || message->GetName().ToString() !=
                      kMuonRendererFunctionSourceReleaseMessageName) {
    return false;
  }
  const auto args = message->GetArgumentList();
  if (!args || args->GetSize() != 3 || args->GetType(0) != VTYPE_INT ||
      args->GetType(1) != VTYPE_INT || args->GetType(2) != VTYPE_STRING ||
      args->GetInt(0) != context_id_) {
    return true;
  }
  const auto function_id = args->GetInt(1);
  const auto lease_token = args->GetString(2).ToString();
  for (auto& reference : function_references_) {
    if (reference.id == function_id) {
      reference.source_lease_tokens.erase(lease_token);
      break;
    }
  }
  ReleaseFunctionReferenceIfUnused(function_id);
  return true;
}

bool MuonV8Handler::HandleRendererFunctionResultConsumedMessage(
    CefRefPtr<CefProcessMessage> message) {
  CEF_REQUIRE_RENDERER_THREAD();
  if (!message || message->GetName().ToString() !=
                      kMuonRendererFunctionResultConsumedMessageName) {
    return false;
  }
  const auto args = message->GetArgumentList();
  if (!args || args->GetSize() != 2 || args->GetType(0) != VTYPE_INT ||
      args->GetType(1) != VTYPE_INT || args->GetInt(0) != context_id_) {
    return true;
  }
  pending_renderer_function_result_transfers_.erase(args->GetInt(1));
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
    if (reference.id == function_id) {
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
  args->SetSize(5);
  args->SetInt(0, call_id);
  args->SetInt(4, context_id_);
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
  FunctionTransfers function_transfers(this);
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
      auto function_error = std::string{};
      if (!EncodeFunctionArgument(return_type, value, encoded_function,
                                  &function_transfers, &function_error)) {
        args->SetBool(1, false);
        args->SetString(
            2, function_error.empty()
                   ? "Renderer function returned a function with an incompatible type"
                   : "Renderer function returned an invalid function: " +
                         function_error);
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
              kMuonRendererFunctionResultSharedMessageName, call_id,
              context_id_,
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
  if (!function_transfers.Empty() && args->GetBool(1)) {
    pending_renderer_function_result_transfers_.insert_or_assign(
        call_id, std::move(function_transfers));
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
          encoded_function->GetType(kMuonFunctionArgumentProxyIdKey) !=
              VTYPE_INT ||
          encoded_function->GetType(kMuonFunctionArgumentProxyLeaseTokenKey) !=
              VTYPE_STRING) {
        return CefV8Value::CreateUndefined();
      }
      const auto lease_token = encoded_function
                                   ->GetString(
                                       kMuonFunctionArgumentProxyLeaseTokenKey)
                                   .ToString();
      if (lease_token.empty()) {
        return CefV8Value::CreateUndefined();
      }
      return CreatePluginProxyFunction(
          static_cast<uint32_t>(
              encoded_function->GetInt(kMuonFunctionArgumentProxyIdKey)),
          lease_token, return_type);
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
          encoded_function->GetType(kMuonFunctionArgumentProxyIdKey) !=
              VTYPE_INT ||
          encoded_function->GetType(kMuonFunctionArgumentProxyLeaseTokenKey) !=
              VTYPE_STRING) {
        return CefV8Value::CreateUndefined();
      }
      const auto lease_token = encoded_function
                                   ->GetString(
                                       kMuonFunctionArgumentProxyLeaseTokenKey)
                                   .ToString();
      if (lease_token.empty()) {
        return CefV8Value::CreateUndefined();
      }
      auto function_type = value_type;
      const auto type_dictionary = encoded_function->GetDictionary(
          kMuonFunctionArgumentTypeDescriptorKey);
      if (type_dictionary) {
        ReadMuonTypeMetadataDictionary(type_dictionary, false,
                                        &function_type);
      }
      return CreatePluginProxyFunction(
          static_cast<uint32_t>(
              encoded_function->GetInt(kMuonFunctionArgumentProxyIdKey)),
          lease_token, function_type);
    }
    case MUON_TYPE_BUFFER_VIEW:
      return CreateV8ArrayBufferFromSharedPayload(values, index,
                                                  std::move(shared_payload));
    default:
      return CefV8Value::CreateUndefined();
  }
  return CefV8Value::CreateUndefined();
}

CefRefPtr<CefV8Value> MuonV8Handler::CreatePluginProxyFunction(
    uint32_t proxy_id,
    const std::string& lease_token,
    const MuonTypeMetadata& function_type) {
  if (next_proxy_wrapper_id_ == 0) {
    CefRefPtr<MuonPluginFunctionProxyState> exhausted_state =
        new MuonPluginFunctionProxyState(this, context_, proxy_id, lease_token,
                                         function_type, 0);
    exhausted_state->ReleaseForCreationFailure();
    return CefV8Value::CreateUndefined();
  }
  const auto wrapper_id = next_proxy_wrapper_id_;
  next_proxy_wrapper_id_ =
      wrapper_id == std::numeric_limits<uint64_t>::max() ? 0 : wrapper_id + 1;
  CefRefPtr<MuonPluginFunctionProxyState> state =
      new MuonPluginFunctionProxyState(this, context_, proxy_id, lease_token,
                                       function_type, wrapper_id);
  if (lease_token.empty() || function_type.type != MUON_TYPE_FUNCTION ||
      function_type.function_return_type.empty()) {
    state->ReleaseForCreationFailure();
    return CefV8Value::CreateUndefined();
  }

  auto error_message = std::string{};
  if (!EnsurePluginProxyFactory(&error_message)) {
    state->ReleaseForCreationFailure();
    return CefV8Value::CreateUndefined();
  }

  const auto marker = CefV8Value::CreateObject(nullptr, nullptr);
  if (!marker || !marker->SetUserData(state)) {
    state->ReleaseForCreationFailure();
    return CefV8Value::CreateUndefined();
  }
  CefV8ValueList arguments;
  arguments.push_back(plugin_proxy_dispatch_function_);
  arguments.push_back(marker);
  const auto created_values = plugin_proxy_factory_->ExecuteFunctionWithContext(
      context_, context_->GetGlobal(), arguments);
  if (!created_values || !created_values->IsArray() ||
      created_values->GetArrayLength() < 2) {
    if (plugin_proxy_factory_->HasException()) {
      plugin_proxy_factory_->ClearException();
    }
    state->ReleaseForCreationFailure();
    return CefV8Value::CreateUndefined();
  }
  const auto function = created_values->GetValue(0);
  const auto release_function = created_values->GetValue(1);
  if (!function || !function->IsFunction() || !release_function ||
      !release_function->IsFunction()) {
    state->ReleaseForCreationFailure();
    return CefV8Value::CreateUndefined();
  }
  const auto property_attributes =
      static_cast<CefV8Value::PropertyAttribute>(
          V8_PROPERTY_ATTRIBUTE_READONLY |
          V8_PROPERTY_ATTRIBUTE_DONTENUM |
          V8_PROPERTY_ATTRIBUTE_DONTDELETE);
  if (!function->SetValue(kMuonPluginProxyMarkerPropertyName, marker,
                          property_attributes) ||
      !function->SetValue(kMuonPluginProxyReleasePropertyName,
                          release_function, property_attributes)) {
    state->ReleaseForCreationFailure();
    return CefV8Value::CreateUndefined();
  }
  RegisterPluginProxyState(state->GetUserDataKey(), state.get());
  return function;
}

bool MuonV8Handler::EnsurePluginProxyFactory(std::string* error_message) {
  if (plugin_proxy_factory_ && plugin_proxy_dispatch_function_) {
    return true;
  }
  if (!context_) {
    *error_message = "muon function proxy context is unavailable";
    return false;
  }

  static constexpr char kFactorySource[] = R"JS(
((dispatch, marker) => {
  "use strict";
  const proxy = (...args) => dispatch(marker, false, ...args);
  const release = () => dispatch(marker, true);
  Object.defineProperty(proxy, Symbol.dispose, {
    configurable: false,
    enumerable: false,
    value: release,
    writable: false,
  });
  return [proxy, release];
})
)JS";
  CefRefPtr<CefV8Value> factory;
  CefRefPtr<CefV8Exception> exception;
  if (!context_->Eval(kFactorySource, "muon://plugin-proxy-factory", 1,
                      factory, exception) ||
      !factory || !factory->IsFunction()) {
    *error_message =
        "muon function proxy factory setup failed" +
        (exception ? ": " + exception->GetMessage().ToString()
                   : std::string{});
    return false;
  }
  const auto dispatch_function = CefV8Value::CreateFunction(
      kMuonPluginProxyDispatchFunctionName, this);
  if (!dispatch_function) {
    *error_message = "muon function proxy dispatcher setup failed";
    return false;
  }
  plugin_proxy_factory_ = factory;
  plugin_proxy_dispatch_function_ = dispatch_function;
  return true;
}

CefRefPtr<MuonPluginFunctionProxyState>
MuonV8Handler::FindPluginProxyState(
    CefRefPtr<CefV8Value> function,
    std::string* error_message) const {
  error_message->clear();
  if (!function || !function->IsFunction()) {
    return nullptr;
  }
  const auto marker = function->GetValue(kMuonPluginProxyMarkerPropertyName);
  if (!marker) {
    if (function->HasException()) {
      function->ClearException();
      *error_message = "muon function proxy metadata access failed";
    }
    return nullptr;
  }
  if (marker->IsUndefined()) {
    return nullptr;
  }
  const auto state = FindPluginProxyStateFromMarker(marker);
  if (!state) {
    *error_message = "muon function proxy metadata is invalid";
  }
  return state;
}

CefRefPtr<MuonPluginFunctionProxyState>
MuonV8Handler::FindPluginProxyStateFromMarker(
    CefRefPtr<CefV8Value> marker) const {
  if (!marker || !marker->IsObject()) {
    return nullptr;
  }
  const auto user_data = marker->GetUserData();
  if (!user_data) {
    return nullptr;
  }
  const auto iterator =
      g_muon_plugin_proxy_states_by_user_data.find(user_data.get());
  return iterator == g_muon_plugin_proxy_states_by_user_data.end()
             ? nullptr
             : CefRefPtr<MuonPluginFunctionProxyState>(iterator->second);
}

void MuonV8Handler::RegisterPluginProxyState(
    const CefBaseRefCounted* user_data,
    MuonPluginFunctionProxyState* state) {
  if (user_data != nullptr && state != nullptr) {
    plugin_proxy_states_by_user_data_[user_data] = state;
    g_muon_plugin_proxy_states_by_user_data[user_data] = state;
  }
}

void MuonV8Handler::UnregisterPluginProxyState(
    const CefBaseRefCounted* user_data,
    MuonPluginFunctionProxyState* state) {
  const auto iterator = plugin_proxy_states_by_user_data_.find(user_data);
  if (iterator != plugin_proxy_states_by_user_data_.end() &&
      iterator->second == state) {
    plugin_proxy_states_by_user_data_.erase(iterator);
  }
  const auto global_iterator =
      g_muon_plugin_proxy_states_by_user_data.find(user_data);
  if (global_iterator != g_muon_plugin_proxy_states_by_user_data.end() &&
      global_iterator->second == state) {
    g_muon_plugin_proxy_states_by_user_data.erase(global_iterator);
  }
}
