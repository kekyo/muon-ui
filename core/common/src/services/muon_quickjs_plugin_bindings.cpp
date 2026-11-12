/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "services/muon_service_plugin_pool_internal.h"
#include "quickjs/muon_quickjs_module.h"
#include "services/muon_service_limits.h"
#include <cassert>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

static constexpr size_t kMaximumCalls = 128;
static constexpr size_t kMaximumFunctions = 1024;
static JSClassID binding_class = 0;

struct MuonPluginJsValue {
  JSContext* context;
  JSValue value;
  MuonPluginJsValue(JSContext* context, JSValue value) : context(context), value(value) {}
  ~MuonPluginJsValue() { JS_FreeValue(context, value); }
  MuonPluginJsValue(const MuonPluginJsValue&) = delete;
  MuonPluginJsValue& operator=(const MuonPluginJsValue&) = delete;
  JSValue Take() { return std::exchange(value, JS_UNDEFINED); }
};

// These owned values never leave their heap. Native pending work holds only a
// weak binding and copied MuonRpcValue, so Detach can destroy the heap first.
struct MuonQuickJsPluginSource {
  JSValue function = JS_UNDEFINED;
  size_t transfers = 0;
  std::set<std::string> leases;
};
struct MuonQuickJsPluginTarget {
  MuonRpcCallKind kind = MuonRpcCallKind::Plugin;
  uint32_t function_id = 0;
  std::string proxy_lease;
  std::vector<MuonTypeMetadata> arguments;
  MuonTypeMetadata result;
};
struct MuonQuickJsPluginPending {
  JSValue resolve = JS_UNDEFINED;
  JSValue reject = JS_UNDEFINED;
  std::vector<int> transfers;
};
struct MuonQuickJsPluginBindingsImpl : std::enable_shared_from_this<MuonQuickJsPluginBindingsImpl> {
  std::shared_ptr<MuonServicePluginLeaseImpl> lease;
  MuonQuickJsEngine* engine = nullptr;
  JSContext* context = nullptr;
  bool faulted = false;
  std::function<void()> wake;
  std::function<void(const std::string&)> fault;
  JSValue holder = JS_UNDEFINED;
  JSValue callback_runner = JS_UNDEFINED;
  uint64_t next_target = 1;
  uint64_t next_call = 1;
  uint64_t next_source = 1;
  std::map<uint32_t, MuonQuickJsPluginTarget> targets;
  std::map<uint32_t, MuonQuickJsPluginPending> calls;
  std::map<int, MuonQuickJsPluginSource> sources;
  std::map<uint32_t, MuonTypeMetadata> callbacks;
  std::map<uint32_t, std::vector<int>> callback_transfers;
  std::map<std::string, JSValue> modules;

  void Detach();
  void Receive(const MuonRpcMessage& message);
  void Fail(const std::string& error);
  JSValue Function(uint32_t id, int operation);
  JSValue Invoke(uint32_t id, int argc, JSValueConst* argv);
  JSValue Decode(const MuonRpcValue& value);
  MuonRpcValue Encode(JSValueConst value, const MuonTypeMetadata& type,
      std::vector<int>& transfers, size_t& remaining);
  void ReleaseTransfers(const std::vector<int>& transfers);
  void CollectSource(int id);
  void Settle(uint32_t id, bool success, JSValueConst value);
  void Complete(const MuonRpcCallResult& result);
  void ReleaseProxy(uint32_t id);
};

static void ValidateType(const MuonTypeMetadata& type) {
  if (type.type == MUON_TYPE_POINTER) throw std::runtime_error("Raw pointer types are not available to QuickJS services");
  for (const auto& argument : type.function_arg_types) ValidateType(argument);
  for (const auto& result : type.function_return_type) ValidateType(result);
}

static std::string StringValue(JSContext* context, JSValueConst value) {
  size_t size = 0;
  const auto text = JS_ToCStringLen(context, &size, value);
  if (!text) throw std::runtime_error("Cannot convert plugin string");
  std::string result(text, size);
  JS_FreeCString(context, text);
  return result;
}

static void UseBytes(size_t size, size_t& remaining) {
  if (size > remaining) throw std::runtime_error("Plugin value byte budget exceeded");
  remaining -= size;
}

static void FinalizeHolder(JSRuntime*, JSValue value) {
  delete static_cast<std::weak_ptr<MuonQuickJsPluginBindingsImpl>*>(JS_GetOpaque(value, binding_class));
}

static JSValue Dispatch(JSContext* context, JSValueConst, int argc, JSValueConst* argv,
                        int operation, JSValue* data) {
  const auto weak = static_cast<std::weak_ptr<MuonQuickJsPluginBindingsImpl>*>(JS_GetOpaque(data[0], binding_class));
  const auto self = weak ? weak->lock() : nullptr;
  if (!self || !self->context || self->faulted || self->lease->closing) return JS_ThrowTypeError(context, "Service plugin owner is closed");
  uint32_t id = 0;
  if (JS_ToUint32(context, &id, data[1]) < 0) return JS_EXCEPTION;
  try {
    if (operation == 0) return self->Invoke(id, argc, argv);
    if (operation == 1 || operation == 2) {
      self->Settle(id, operation == 1, argc ? argv[0] : JS_UNDEFINED);
    } else if (operation == 3) self->ReleaseProxy(id);
    return JS_UNDEFINED;
  } catch (const std::exception& error) {
    return JS_ThrowTypeError(context, "%s", error.what());
  }
}

JSValue MuonQuickJsPluginBindingsImpl::Function(uint32_t id, int operation) {
  const JSValue data[]{holder, JS_NewUint32(context, id)};
  return JS_NewCFunctionData(context, Dispatch, 0, operation, 2, const_cast<JSValue*>(data));
}

void MuonQuickJsPluginBindingsImpl::Fail(const std::string& error) {
  if (faulted) return;
  faulted = true;
  const auto weak = weak_from_this();
  lease->pool->Post([weak, error] {
    const auto self = weak.lock();
    if (self && self->context && self->fault) self->fault(error);
  });
}

MuonRpcValue MuonQuickJsPluginBindingsImpl::Encode(JSValueConst value, const MuonTypeMetadata& type,
    std::vector<int>& transfers, size_t& remaining) {
  MuonRpcValue result;
  result.type = type;
  if ((type.type == MUON_TYPE_STRING || type.type == MUON_TYPE_FUNCTION) && JS_IsNull(value)) {
    result.is_null = true;
    return result;
  }
  switch (type.type) {
    case MUON_TYPE_VOID: return result;
    case MUON_TYPE_BOOL:
      if (!JS_IsBool(value)) throw std::runtime_error("Plugin boolean is required");
      result.bool_value = JS_ToBool(context, value) != 0;
      return result;
    case MUON_TYPE_I8: case MUON_TYPE_U8: case MUON_TYPE_I16: case MUON_TYPE_U16:
    case MUON_TYPE_I32: case MUON_TYPE_U32: case MUON_TYPE_F32: case MUON_TYPE_F64: {
      double number = 0;
      if (!JS_IsNumber(value) || JS_ToFloat64(context, &number, value) < 0 || !std::isfinite(number)) {
        throw std::runtime_error("Finite plugin number is required");
      }
      if (type.type == MUON_TYPE_F64) { result.f64_value = number; return result; }
      if (type.type == MUON_TYPE_F32) {
        result.f32_value = static_cast<float>(number);
        if (!std::isfinite(result.f32_value)) throw std::runtime_error("Plugin float32 is out of range");
        return result;
      }
      const double minimum = type.type == MUON_TYPE_I8 ? INT8_MIN : type.type == MUON_TYPE_I16 ? INT16_MIN : type.type == MUON_TYPE_I32 ? INT32_MIN : 0;
      const double maximum = type.type == MUON_TYPE_I8 ? INT8_MAX : type.type == MUON_TYPE_U8 ? UINT8_MAX : type.type == MUON_TYPE_I16 ? INT16_MAX : type.type == MUON_TYPE_U16 ? UINT16_MAX : type.type == MUON_TYPE_I32 ? INT32_MAX : UINT32_MAX;
      if (std::trunc(number) != number || number < minimum || number > maximum) throw std::runtime_error("Plugin integer is out of range");
      switch (type.type) {
        case MUON_TYPE_I8: result.i8_value = static_cast<int8_t>(number); break;
        case MUON_TYPE_U8: result.u8_value = static_cast<uint8_t>(number); break;
        case MUON_TYPE_I16: result.i16_value = static_cast<int16_t>(number); break;
        case MUON_TYPE_U16: result.u16_value = static_cast<uint16_t>(number); break;
        case MUON_TYPE_I32: result.i32_value = static_cast<int32_t>(number); break;
        case MUON_TYPE_U32: result.u32_value = static_cast<uint32_t>(number); break;
        default: break;
      }
      return result;
    }
    case MUON_TYPE_I64: case MUON_TYPE_U64: {
      if (!JS_IsBigInt(context, value)) throw std::runtime_error("Plugin 64-bit integer requires bigint");
      const auto text = StringValue(context, value);
      const auto parsed = type.type == MUON_TYPE_I64
          ? std::from_chars(text.data(), text.data() + text.size(), result.i64_value)
          : std::from_chars(text.data(), text.data() + text.size(), result.u64_value);
      if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) throw std::runtime_error("Plugin 64-bit integer is out of range");
      return result;
    }
    case MUON_TYPE_STRING:
      if (!JS_IsString(value)) throw std::runtime_error("Plugin string is required");
      result.string_value = StringValue(context, value);
      if (result.string_value.find('\0') != std::string::npos) throw std::runtime_error("Native plugin strings cannot contain NUL");
      UseBytes(result.string_value.size(), remaining);
      return result;
    case MUON_TYPE_BUFFER_VIEW: {
      size_t length = 0, offset = 0, element = 0, buffer_size = 0;
      auto buffer = JS_GetTypedArrayBuffer(context, value, &offset, &length, &element);
      const uint8_t* data = nullptr;
      if (JS_IsException(buffer)) {
        JS_FreeValue(context, JS_GetException(context));
        data = JS_GetArrayBuffer(context, &length, value);
        if (!data) {
          // Empty ArrayBuffers have a valid non-null QuickJS allocation; a
          // detached/non-buffer value reports an exception and is rejected.
          const auto error = JS_GetException(context);
          if (!JS_IsNull(error)) { JS_FreeValue(context, error); throw std::runtime_error("Plugin binary requires ArrayBuffer or typed array"); }
          JS_FreeValue(context, error);
        }
      } else {
        data = JS_GetArrayBuffer(context, &buffer_size, buffer);
        if (!data || offset > buffer_size || length > buffer_size - offset) {
          JS_FreeValue(context, buffer);
          throw std::runtime_error("Plugin binary view is detached or invalid");
        }
        data += offset;
      }
      if (length > remaining) { JS_FreeValue(context, buffer); throw std::runtime_error("Plugin value byte budget exceeded"); }
      result.binary.storage = CreateMuonRpcOwnedBuffer(length);
      result.binary.size = length;
      if (length) std::memcpy(result.binary.storage->GetData(), data, length);
      JS_FreeValue(context, buffer);
      UseBytes(length, remaining);
      return result;
    }
    case MUON_TYPE_FUNCTION: {
      if (!JS_IsFunction(context, value)) throw std::runtime_error("Plugin callback must be a function");
      ValidateType(type);
      if (sources.size() >= kMaximumFunctions || next_source > INT_MAX) throw std::runtime_error("Plugin callback limit exceeded");
      const auto id = static_cast<int>(next_source++);
      sources.emplace(id, MuonQuickJsPluginSource{JS_DupValue(context, value), 1, {}});
      transfers.push_back(id);
      result.function = {MuonRpcFunctionKind::RendererSource, lease->owner.context_id, id, 0, {}, type};
      return result;
    }
    default: throw std::runtime_error("Unsupported QuickJS plugin value");
  }
}

JSValue MuonQuickJsPluginBindingsImpl::Decode(const MuonRpcValue& value) {
  if (value.is_null) return JS_NULL;
  switch (value.type.type) {
    case MUON_TYPE_VOID: return JS_UNDEFINED;
    case MUON_TYPE_BOOL: return JS_NewBool(context, value.bool_value);
    case MUON_TYPE_I8: return JS_NewInt32(context, value.i8_value);
    case MUON_TYPE_U8: return JS_NewUint32(context, value.u8_value);
    case MUON_TYPE_I16: return JS_NewInt32(context, value.i16_value);
    case MUON_TYPE_U16: return JS_NewUint32(context, value.u16_value);
    case MUON_TYPE_I32: return JS_NewInt32(context, value.i32_value);
    case MUON_TYPE_U32: return JS_NewUint32(context, value.u32_value);
    case MUON_TYPE_I64: return JS_NewBigInt64(context, value.i64_value);
    case MUON_TYPE_U64: return JS_NewBigUint64(context, value.u64_value);
    case MUON_TYPE_F32: return JS_NewFloat64(context, value.f32_value);
    case MUON_TYPE_F64: return JS_NewFloat64(context, value.f64_value);
    case MUON_TYPE_STRING: return JS_NewStringLen(context, value.string_value.data(), value.string_value.size());
    case MUON_TYPE_BUFFER_VIEW: {
      if (!IsValidMuonRpcBinary(value.binary) || value.binary.size > kMuonServiceMaxFrameBytes) throw std::runtime_error("Invalid native plugin binary");
      auto buffer = JS_NewArrayBufferCopy(context,
          static_cast<const uint8_t*>(GetMuonRpcBinaryData(value.binary)), value.binary.size);
      if (JS_IsException(buffer)) return buffer;
      // QuickJS's public C constructor forwards directly to the intrinsic's
      // three-slot argument view; supply offset and length explicitly.
      JSValue arguments[]{buffer, JS_NewInt32(context, 0), JS_UNDEFINED};
      const auto array = JS_NewTypedArray(context, 3, arguments, JS_TYPED_ARRAY_UINT8);
      JS_FreeValue(context, buffer);
      return array;
    }
    case MUON_TYPE_FUNCTION: {
      ValidateType(value.type);
      if (value.function.kind == MuonRpcFunctionKind::RendererSource) {
        const auto source = sources.find(value.function.function_id);
        if (source == sources.end()) throw std::runtime_error("Plugin callback source expired");
        return JS_DupValue(context, source->second.function);
      }
      if (targets.size() >= kMaximumFunctions || next_target > UINT32_MAX || value.type.function_return_type.size() != 1) {
        lease->pool->runtime->ReleasePluginFunctionProxy({lease->owner, value.function.proxy_id, value.function.lease_token});
        throw std::runtime_error("Plugin proxy limit or signature is invalid");
      }
      const auto id = static_cast<uint32_t>(next_target++);
      targets.emplace(id, MuonQuickJsPluginTarget{MuonRpcCallKind::PluginProxy, value.function.proxy_id,
          value.function.lease_token, value.type.function_arg_types, value.type.function_return_type[0]});
      const auto function = Function(id, 0);
      if (JS_IsException(function)) { ReleaseProxy(id); return function; }
      auto release = Function(id, 3);
      if (JS_IsException(release) ||
          JS_DefinePropertyValueStr(context, function, "release", release, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(context, function); ReleaseProxy(id); return JS_EXCEPTION;
      }
      return function;
    }
    default: throw std::runtime_error("Unsupported native plugin result");
  }
}

void MuonQuickJsPluginBindingsImpl::CollectSource(int id) {
  const auto source = sources.find(id);
  if (source != sources.end() && !source->second.transfers && source->second.leases.empty()) {
    JS_FreeValue(context, source->second.function);
    sources.erase(source);
  }
}
void MuonQuickJsPluginBindingsImpl::ReleaseTransfers(const std::vector<int>& transfers) {
  if (!context) return;
  for (const auto id : transfers) {
    const auto source = sources.find(id);
    if (source == sources.end()) continue;
    assert(source->second.transfers);
    --source->second.transfers;
    CollectSource(id);
  }
}

JSValue MuonQuickJsPluginBindingsImpl::Invoke(uint32_t id, int argc, JSValueConst* argv) {
  const auto target = targets.find(id);
  if (target == targets.end()) throw std::runtime_error("Plugin function expired");
  if (static_cast<size_t>(argc) != target->second.arguments.size()) throw std::runtime_error("Plugin argument count differs");
  if (calls.size() >= kMaximumCalls || next_call > UINT32_MAX) throw std::runtime_error("Plugin pending call limit exceeded");
  MuonRpcCallRequest request;
  request.call_id = static_cast<uint32_t>(next_call++);
  request.kind = target->second.kind;
  request.function_id = target->second.function_id;
  request.proxy_lease_token = target->second.proxy_lease;
  std::vector<int> transfers;
  size_t remaining = kMuonServiceMaxFrameBytes;
  try {
    for (int index = 0; index < argc; ++index) request.arguments.push_back(Encode(argv[index], target->second.arguments[index], transfers, remaining));
  } catch (...) { ReleaseTransfers(transfers); throw; }
  JSValue resolving[2]{JS_UNDEFINED, JS_UNDEFINED};
  const auto promise = JS_NewPromiseCapability(context, resolving);
  if (JS_IsException(promise)) { ReleaseTransfers(transfers); return promise; }
  calls.emplace(request.call_id, MuonQuickJsPluginPending{resolving[0], resolving[1], std::move(transfers)});
  const auto weak = weak_from_this();
  lease->pool->Invoke(lease, std::move(request), [weak](const MuonRpcCallResult& result) {
    const auto self = weak.lock();
    if (self && self->context) self->Complete(result);
  });
  return promise;
}

void MuonQuickJsPluginBindingsImpl::Complete(const MuonRpcCallResult& result) {
  if (faulted) return;
  const auto found = calls.find(result.call_id);
  if (found == calls.end()) return;
  auto call = std::move(found->second);
  calls.erase(found);
  auto value = JS_UNDEFINED;
  try {
    value = result.success ? Decode(result.value) : JS_NewError(context);
    if (!result.success && !JS_IsException(value)) {
      auto message = JS_NewStringLen(context, result.error_message.data(), result.error_message.size());
      // Define own data properties: inherited setters are application code and
      // must not run from the native result delivery stack outside engine.Call.
      if (JS_IsException(message) ||
          JS_DefinePropertyValueStr(context, value, "message", message, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(context, value);
        value = JS_EXCEPTION;
      }
    }
    if (JS_IsException(value)) {
      MuonPluginJsValue error(context, JS_GetException(context));
      throw std::runtime_error("Cannot allocate native plugin result: " + engine->ExceptionText(error.value));
    }
    const auto returned = engine->Call(result.success ? call.resolve : call.reject, JS_UNDEFINED, {value});
    JS_FreeValue(context, returned);
  } catch (const std::exception& error) { Fail(error.what()); }
  JS_FreeValue(context, value); JS_FreeValue(context, call.resolve); JS_FreeValue(context, call.reject);
  ReleaseTransfers(call.transfers);
  wake();
}

void MuonQuickJsPluginBindingsImpl::Settle(uint32_t id, bool success, JSValueConst value) {
  const auto found = callbacks.find(id);
  if (found == callbacks.end()) return;
  const auto type = found->second;
  callbacks.erase(found);
  MuonRpcRendererFunctionResult result;
  result.owner = lease->owner; result.call_id = id; result.success = success;
  std::vector<int> transfers;
  try {
    if (success) {
      size_t remaining = kMuonServiceMaxFrameBytes;
      result.value = Encode(value, type, transfers, remaining);
    } else result.error_message = engine->ExceptionText(value);
  } catch (const std::exception& error) {
    ReleaseTransfers(transfers); transfers.clear();
    result.success = false; result.error_message = error.what();
  }
  if (!transfers.empty()) callback_transfers.emplace(id, std::move(transfers));
  // Completing a native callback can synchronously invoke its native parent.
  // Keep every such transition outside a QuickJS call/promise-job stack.
  const auto owner = lease;
  lease->pool->Post([owner, result] {
    if (!owner->closing && owner->pool->runtime) owner->pool->runtime->CompleteRendererFunctionCall(result);
  });
}

void MuonQuickJsPluginBindingsImpl::Receive(const MuonRpcMessage& message) {
  if (!context || faulted) return;
  if (const auto transition = std::get_if<MuonRpcRendererFunctionLease>(&message)) {
    const auto source = sources.find(transition->function_id);
    if (source != sources.end()) {
      if (transition->acquire) source->second.leases.insert(transition->lease_token);
      else source->second.leases.erase(transition->lease_token);
      CollectSource(transition->function_id);
    }
  } else if (const auto consumed = std::get_if<MuonRpcRendererFunctionResultConsumed>(&message)) {
    const auto found = callback_transfers.find(consumed->call_id);
    if (found != callback_transfers.end()) { ReleaseTransfers(found->second); callback_transfers.erase(found); }
  } else if (const auto call = std::get_if<MuonRpcRendererFunctionCall>(&message)) {
    const auto source = sources.find(call->function_id);
    if (source == sources.end() || callbacks.size() >= kMaximumCalls || call->function_type.function_return_type.size() != 1) {
      MuonRpcRendererFunctionResult result;
      result.owner = lease->owner; result.call_id = call->call_id;
      result.error_message = "Service plugin callback expired or limit exceeded";
      lease->pool->runtime->CompleteRendererFunctionCall(result);
      return;
    }
    callbacks.emplace(call->call_id, call->function_type.function_return_type[0]);
    auto arguments = JS_NewArray(context);
    auto ok = Function(call->call_id, 1);
    auto fail = Function(call->call_id, 2);
    try {
      if (JS_IsException(arguments) || JS_IsException(ok) || JS_IsException(fail)) throw std::runtime_error("Cannot allocate plugin callback");
      for (size_t index = 0; index < call->arguments.size(); ++index) {
        auto value = Decode(call->arguments[index]);
        if (JS_IsException(value) || JS_SetPropertyUint32(context, arguments, static_cast<uint32_t>(index), value) < 0) throw std::runtime_error("Cannot allocate plugin callback argument");
      }
      const auto result = engine->Call(callback_runner, JS_UNDEFINED, {source->second.function, arguments, ok, fail});
      JS_FreeValue(context, result);
    } catch (const std::exception& error) { Fail(error.what()); }
    JS_FreeValue(context, arguments); JS_FreeValue(context, ok); JS_FreeValue(context, fail);
    wake();
  }
}

void MuonQuickJsPluginBindingsImpl::ReleaseProxy(uint32_t id) {
  const auto found = targets.find(id);
  if (found == targets.end() || found->second.kind != MuonRpcCallKind::PluginProxy) return;
  const auto target = std::move(found->second);
  targets.erase(found);
  const auto owner = lease;
  lease->pool->Post([owner, target] {
    if (!owner->closing && owner->pool->runtime) owner->pool->runtime->ReleasePluginFunctionProxy({owner->owner, target.function_id, target.proxy_lease});
  });
}

void MuonQuickJsPluginBindingsImpl::Detach() {
  if (!context) return;
  for (const auto& [id, call] : calls) { (void)id; JS_FreeValue(context, call.resolve); JS_FreeValue(context, call.reject); }
  for (const auto& [id, source] : sources) { (void)id; JS_FreeValue(context, source.function); }
  for (const auto& [name, object] : modules) { (void)name; JS_FreeValue(context, object); }
  JS_FreeValue(context, callback_runner); JS_FreeValue(context, holder);
  calls.clear(); sources.clear(); modules.clear(); callbacks.clear(); callback_transfers.clear(); targets.clear();
  holder = JS_UNDEFINED; callback_runner = JS_UNDEFINED;
  context = nullptr; engine = nullptr; wake = {}; fault = {};
}

std::shared_ptr<MuonQuickJsPluginBindings> MuonServicePluginLease::Attach(MuonQuickJsEngine& engine,
    std::function<void()> wake, std::function<void(const std::string&)> fault) {
  if (impl_->closing || impl_->attached || !wake || !fault) throw std::invalid_argument("Invalid service plugin attachment");
  auto binding = std::make_shared<MuonQuickJsPluginBindingsImpl>();
  binding->lease = impl_; binding->engine = &engine; binding->context = engine.Context();
  binding->wake = std::move(wake); binding->fault = std::move(fault);
  try {
    if (!binding_class) JS_NewClassID(&binding_class);
    const JSClassDef definition{"MuonServicePluginOwner", FinalizeHolder, nullptr, nullptr, nullptr};
    if (JS_NewClass(engine.Runtime(), binding_class, &definition) < 0) throw std::runtime_error("Cannot register plugin owner class");
    binding->holder = JS_NewObjectClass(engine.Context(), static_cast<int>(binding_class));
    if (JS_IsException(binding->holder)) throw std::runtime_error("Cannot allocate plugin owner");
    JS_SetOpaque(binding->holder, new std::weak_ptr<MuonQuickJsPluginBindingsImpl>(binding));
    binding->callback_runner = engine.Evaluate(
        "(async (fn,args,ok,fail)=>{try{ok(await fn(...args))}catch(error){fail(error)}})",
        "<muon plugin callback>", JS_EVAL_TYPE_GLOBAL);
    for (const auto& ns : impl_->pool->runtime->GetNamespaces()) {
      MuonPluginJsValue object(binding->context, JS_NewObjectProto(binding->context, JS_NULL));
      MuonPluginJsValue allowed(binding->context, JS_NewArray(binding->context));
      if (JS_IsException(object.value) || JS_IsException(allowed.value)) throw std::runtime_error("Cannot allocate plugin namespace");
      std::set<std::string> public_names;
      for (const auto& function : impl_->pool->runtime->GetFunctions()) {
        if (function.plugin_namespace != ns.plugin_namespace || !impl_->policy->IsAllowedFunctionPath(CreateMuonFunctionPublicPath(function))) continue;
        for (const auto& argument : function.arg_types) ValidateType(argument);
        ValidateType(function.return_type);
        const auto id = static_cast<uint32_t>(binding->next_target++);
        binding->targets.emplace(id, MuonQuickJsPluginTarget{MuonRpcCallKind::Plugin, function.id, {}, function.arg_types, function.return_type});
        auto published = binding->Function(id, 0);
        if (JS_IsException(published) || JS_SetPropertyStr(binding->context, object.value, function.js_name.c_str(), published) < 0) throw std::runtime_error("Cannot publish plugin function");
        public_names.insert(function.public_name);
      }
      if (public_names.empty()) continue;
      uint32_t index = 0;
      for (const auto& name : public_names) {
        if (JS_SetPropertyUint32(binding->context, allowed.value, index++, JS_NewString(binding->context, name.c_str())) < 0) throw std::runtime_error("Cannot allocate plugin policy");
      }
      MuonPluginJsValue setup(binding->context, engine.Evaluate("((namespace,globalThis,allowed)=>{const isAllowed=(name)=>allowed.includes(name);\n" + ns.setup_script + "\n})", "<muon plugin setup>", JS_EVAL_TYPE_GLOBAL));
      MuonPluginJsValue global(binding->context, JS_GetGlobalObject(binding->context));
      MuonPluginJsValue result(binding->context, engine.Call(setup.value, JS_UNDEFINED, {object.value, global.value, allowed.value}));
      MuonPluginJsValue exports(binding->context, JS_NewObjectProto(binding->context, JS_NULL));
      if (JS_IsException(exports.value)) throw std::runtime_error("Cannot allocate plugin exports");
      for (const auto& name : public_names) {
        const auto atom = JS_NewAtom(binding->context, name.c_str());
        JSPropertyDescriptor descriptor{};
        const auto found = JS_GetOwnProperty(binding->context, &descriptor, object.value, atom);
        JS_FreeAtom(binding->context, atom);
        if (found < 0) throw std::runtime_error("Cannot inspect plugin public wrapper");
        MuonPluginJsValue value(binding->context, descriptor.value);
        MuonPluginJsValue getter(binding->context, descriptor.getter);
        MuonPluginJsValue setter(binding->context, descriptor.setter);
        // Do not invoke arbitrary getters outside the engine execution guard.
        if (!found || !JS_IsFunction(binding->context, value.value) ||
            !JS_IsUndefined(getter.value) || !JS_IsUndefined(setter.value) ||
            JS_SetPropertyStr(binding->context, exports.value, name.c_str(), value.Take()) < 0) throw std::runtime_error("Plugin public wrapper is unavailable: " + name);
      }
      auto specifier = ns.plugin_namespace;
      specifier[specifier.find('.')] = ':';
      binding->modules.emplace(specifier, exports.Take());
    }
  } catch (...) { binding->Detach(); throw; }
  impl_->attached = true;
  const auto weak = std::weak_ptr(binding);
  impl_->receive = [weak](const MuonRpcMessage& message) { if (const auto current = weak.lock()) current->Receive(message); };
  impl_->detach = [weak] { if (const auto current = weak.lock()) current->Detach(); };
  return std::shared_ptr<MuonQuickJsPluginBindings>(new MuonQuickJsPluginBindings(std::move(binding)));
}

MuonQuickJsPluginBindings::MuonQuickJsPluginBindings(std::shared_ptr<MuonQuickJsPluginBindingsImpl> impl) : impl_(std::move(impl)) {}
MuonQuickJsPluginBindings::~MuonQuickJsPluginBindings() { assert(!impl_->context); }
JSModuleDef* MuonQuickJsPluginBindings::LoadModule(JSContext* context, const char* name) {
  if (context != impl_->context || !name || impl_->lease->closing) { JS_ThrowTypeError(context, "Service plugin owner is closed"); return nullptr; }
  const auto found = impl_->modules.find(name);
  if (found == impl_->modules.end()) { JS_ThrowReferenceError(context, "Plugin module is not allowed: %s", name); return nullptr; }
  return CreateMuonQuickJsObjectModule(context, name, JS_DupValue(context, found->second));
}
void MuonQuickJsPluginBindings::Detach() { impl_->Detach(); }
void MuonQuickJsPluginBindings::Close(std::function<void()> complete) {
  impl_->Detach();
  impl_->lease->pool->Close(impl_->lease, std::move(complete));
}
