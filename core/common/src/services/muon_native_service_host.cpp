/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "services/muon_native_service_host.h"

#include <cassert>
#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>

enum class MuonNativeInstanceState { Starting, Running, Stopping };
struct MuonNativeServiceCall;

struct MuonNativeServiceEntry final
    : std::enable_shared_from_this<MuonNativeServiceEntry> {
  std::weak_ptr<MuonNativeServiceHostImpl> owner;
  MuonNativeServiceSpec spec;
  std::vector<muon_plugin_config_entry> config;
  muon_service_host host{};
  muon_service_context context{};
  std::shared_ptr<const muon_service_factory> factory;
  muon_service_instance instance{};
  MuonNativeInstanceState state = MuonNativeInstanceState::Starting;
  std::function<void(const std::string&)> started;
  std::vector<std::function<void()>> stopped;
  std::string interface_json;
  MuonServiceInterface interface;
  std::map<uint64_t, std::shared_ptr<MuonNativeServiceCall>> calls;
  size_t pending_callbacks = 0;
  size_t pending_events = 0;
  size_t pending_event_bytes = 0;
  bool stop_called = false;
  std::string startup_failure;
};

struct MuonNativeCallbackRequest {
  uint64_t id;
  const MuonServiceSignature* signature;
  muon_service_result_completion complete;
  void* data;
  bool settled = false;
};

struct MuonNativeServiceCall {
  std::weak_ptr<MuonNativeServiceCall> self;
  std::weak_ptr<MuonNativeServiceEntry> entry;
  muon_service_call_context context{};
  std::string method;
  const MuonServiceMethod* signature = nullptr;
  std::unique_ptr<MuonNativeServiceValues> arguments;
  std::map<uint64_t, const MuonServiceSignature*> tokens;
  std::map<uint64_t, std::shared_ptr<MuonNativeCallbackRequest>> callbacks;
  uint64_t next_callback = 0;
  MuonServiceCallbackHandler callback;
  MuonServiceCallCompletion complete;
  bool returned = false;
  bool cancelled = false;
};

struct MuonNativeServiceHostImpl {
  MuonNativeServicePlatform platform;
  std::map<std::string, std::weak_ptr<const muon_service_factory>> factories;
  std::map<std::string, std::shared_ptr<MuonNativeServiceEntry>> entries;
  size_t pending_tasks = 0;
  uint64_t next_call = 0;
  MuonServiceEventSink events;
};

static void NativeStopped(void* data);

static void Queue(const std::shared_ptr<MuonNativeServiceHostImpl>& impl,
                   std::function<void()> task) {
  ++impl->pending_tasks;
  impl->platform.post([impl, task = std::move(task)] {
    struct CompletionScope {
      MuonNativeServiceHostImpl& impl;
      ~CompletionScope() { --impl.pending_tasks; }
    } scope{*impl};
    task();
  });
}

static MuonServiceResult Failure(const std::string& code, const std::string& message) {
  return {{}, code, message};
}

static void MaybeStop(const std::shared_ptr<MuonNativeServiceEntry>& entry) {
  if (entry->state == MuonNativeInstanceState::Stopping && entry->calls.empty() && !entry->stop_called) {
    entry->stop_called = true;
    entry->instance.stop(entry->instance.user_data, &NativeStopped, entry.get());
  }
}

static void DeliverResult(const std::shared_ptr<MuonNativeServiceCall>& call, MuonServiceResult result) {
  if (!call->complete) return;
  const auto entry = call->entry.lock();
  const auto impl = entry->owner.lock();
  auto complete = std::move(call->complete);
  // Moving std::function does not guarantee an empty source (libc++ retains
  // small callable targets). Explicitly consume the single public completion.
  call->complete = nullptr;
  Queue(impl, [entry, call, complete = std::move(complete), result = std::move(result)]() mutable {
    complete(std::move(result));
  });
}

static std::string Diagnostic(const char* text) {
  if (!text) throw std::invalid_argument("Missing native error text");
  auto value = muon_service_value{};
  value.kind = MUON_SERVICE_VALUE_STRING;
  value.data.string_value = {text, std::strlen(text)};
  return std::get<std::string>(CopyMuonServiceValue(value).data);
}

static bool CommonError(const std::string& code) {
  return code == "cancelled" || code == "unavailable" || code == "invalid-result" ||
      code == "callback-failed" || code == "resource-exhausted";
}

static MuonServiceResult CheckResult(MuonServiceResult result, const MuonServiceType& type,
                                     const std::vector<std::string>& errors) {
  try {
    if (result.value && result.code.empty()) {
      EncodeMuonServiceValue(type, *result.value);
      return result;
    }
    if (!result.value && !result.code.empty() && (CommonError(result.code) ||
        std::find(errors.begin(), errors.end(), result.code) != errors.end())) {
      Diagnostic(result.message.c_str());
      return result;
    }
  } catch (const std::exception&) {}
  return Failure("invalid-result", "Service completion does not match its interface");
}

static void CompleteCallback(const std::shared_ptr<MuonNativeServiceCall>& call,
    const std::shared_ptr<MuonNativeCallbackRequest>& request, MuonServiceResult result) {
  if (call->returned || request->settled) return;
  request->settled = true;
  result = CheckResult(std::move(result), request->signature->returns, {});
  const auto entry = call->entry.lock();
  const auto impl = entry->owner.lock();
  Queue(impl, [entry, call, request, result = std::move(result)] {
    if (call->returned) return;
    call->callbacks.erase(request->id);
    --entry->pending_callbacks;
    if (result.value) {
      const auto values = MuonNativeServiceValues({*result.value});
      request->complete(request->data, values.Get().data(), nullptr);
    } else {
      const auto error = muon_service_error{result.code.c_str(), result.message.c_str()};
      request->complete(request->data, nullptr, &error);
    }
  });
}

static void NativeCallCompleted(void* data, const muon_service_value* value,
                                const muon_service_error* error) {
  const auto call = static_cast<MuonNativeServiceCall*>(data)->self.lock();
  assert(call && !call->returned);
  const auto entry = call->entry.lock();
  const auto impl = entry->owner.lock();
  auto result = Failure("invalid-result", "Native service returned an invalid completion");
  try {
    if (value && !error) result = {CopyMuonServiceValue(*value), {}, {}};
    else if (!value && error) result = Failure(Diagnostic(error->code), Diagnostic(error->message));
    result = CheckResult(std::move(result), call->signature->signature.returns, call->signature->errors);
  } catch (const std::exception&) {}
  call->returned = true;
  if (!call->callbacks.empty()) {
    result = Failure("invalid-result", "Native service completed before draining its callbacks");
    entry->pending_callbacks -= call->callbacks.size();
    call->callbacks.clear();
  }
  DeliverResult(call, std::move(result));
  // Keep both the instance and its argument views alive until native code has
  // returned through the completion, even when it completes inside cancel.
  Queue(impl, [entry, call] {
    entry->calls.erase(call->context.call_id);
    MaybeStop(entry);
  });
}

static int RequestCallback(void* data, uint64_t token, const muon_service_value* arguments,
    size_t count, muon_service_result_completion complete, void* completion_data) {
  const auto call = static_cast<MuonNativeServiceCall*>(data)->self.lock();
  assert(call && complete);
  const auto entry = call->entry.lock();
  const auto impl = entry->owner.lock();
  const auto found = call->tokens.find(token);
  if (call->cancelled || call->returned || found == call->tokens.end() ||
      entry->pending_callbacks >= 128 || call->callbacks.size() >= 32 ||
      call->next_callback == std::numeric_limits<uint64_t>::max()) return 0;
  auto values = std::vector<MuonServiceValue>{};
  try {
    if (count != found->second->parameters.size() || (count && !arguments))
      throw std::invalid_argument("Invalid callback arguments");
    for (size_t index = 0; index < count; ++index) values.push_back(CopyMuonServiceValue(arguments[index]));
    EncodeMuonServiceArguments(found->second->parameters, values);
  } catch (const std::exception&) {
    return 0;
  }
  const auto request = std::make_shared<MuonNativeCallbackRequest>(MuonNativeCallbackRequest{
      ++call->next_callback, found->second, complete, completion_data, false});
  call->callbacks.emplace(request->id, request);
  ++entry->pending_callbacks;
  Queue(impl, [entry, call, request, token, values = std::move(values)]() mutable {
    if (call->returned || request->settled) return;
    const auto weak = std::weak_ptr<MuonNativeServiceCall>(call);
    try {
      call->callback(token, *request->signature, std::move(values), [weak, request](MuonServiceResult result) {
        if (const auto active = weak.lock()) CompleteCallback(active, request, std::move(result));
      });
    } catch (const std::exception&) {
      CompleteCallback(call, request, Failure("callback-failed", "Page callback dispatch failed"));
    }
  });
  return 1;
}

static void CancelCall(const std::shared_ptr<MuonNativeServiceEntry>& entry,
                        const std::shared_ptr<MuonNativeServiceCall>& call) {
  if (call->returned || call->cancelled) return;
  call->cancelled = true;
  DeliverResult(call, Failure("cancelled", "Service invocation was cancelled"));
  for (const auto& [id, request] : call->callbacks) {
    CompleteCallback(call, request, Failure("cancelled", "Parent service invocation was cancelled"));
  }
  entry->instance.cancel(entry->instance.user_data, call->context.call_id);
}

static int EmitEvent(void* data, const char* name, const muon_service_value* value) {
  const auto entry = static_cast<MuonNativeServiceEntry*>(data)->shared_from_this();
  const auto impl = entry->owner.lock();
  if (!name || !value || entry->state != MuonNativeInstanceState::Running || entry->pending_events >= 256) return 0;
  const auto found = entry->interface.events.find(name);
  if (found == entry->interface.events.end()) return 0;
  try {
    auto copied = CopyMuonServiceValue(*value);
    const auto bytes = EncodeMuonServiceValue(found->second, copied).size();
    if (bytes > 8 * 1024 * 1024 - entry->pending_event_bytes) return 0;
    ++entry->pending_events;
    entry->pending_event_bytes += bytes;
    Queue(impl, [entry, impl, bytes, name = std::string(name), copied = std::move(copied)] {
      --entry->pending_events;
      entry->pending_event_bytes -= bytes;
      if (entry->state == MuonNativeInstanceState::Running && impl->events)
        impl->events(entry->spec.id, entry->context.generation, name, copied);
    });
    return 1;
  } catch (const std::exception&) { return 0; }
}

static void PostNativeTask(void* data, muon_service_task task, void* task_data) {
  const auto entry = static_cast<MuonNativeServiceEntry*>(data)->shared_from_this();
  const auto impl = entry->owner.lock();
  assert(impl && task);
  Queue(impl, [entry, task, task_data] { task(task_data); });
}

static void LogNativeMessage(void* data, muon_log_level level,
                             const char* message) {
  const auto& entry = *static_cast<MuonNativeServiceEntry*>(data);
  const auto impl = entry.owner.lock();
  assert(impl && message);
  impl->platform.log(entry.spec.id, level, message);
}

static void NativeStarted(void* data, const muon_service_instance* instance,
                           const char* error) {
  const auto entry = static_cast<MuonNativeServiceEntry*>(data)->shared_from_this();
  const auto impl = entry->owner.lock();
  assert(impl);
  auto failure = error ? std::string(error) : std::string{};
  if (failure.empty() && (!instance || !instance->stop ||
      (!entry->interface.methods.empty() && (!instance->invoke || !instance->cancel)))) {
    failure = "native service returned an invalid instance descriptor";
  }
  const auto rollback = !failure.empty() && !error && instance && instance->stop;
  if (failure.empty() || rollback) entry->instance = *instance;
  // Never erase the final library reference while returning through its start
  // or stop function. The platform queue also serializes synchronous factories
  // with asynchronously completing ones for the lifecycle manager.
  Queue(impl, [impl, entry, rollback, failure = std::move(failure)] {
    if (rollback) {
      // A successful native start already transferred ownership. Invalid RPC
      // operations do not permit unloading its live work without disposal.
      entry->startup_failure = failure;
      entry->state = MuonNativeInstanceState::Stopping;
      MaybeStop(entry);
      return;
    }
    auto complete = std::move(entry->started);
    if (failure.empty()) entry->state = MuonNativeInstanceState::Running;
    else impl->entries.erase(entry->spec.id);
    complete(failure);
  });
}

static void NativeStopped(void* data) {
  const auto entry = static_cast<MuonNativeServiceEntry*>(data)->shared_from_this();
  const auto impl = entry->owner.lock();
  assert(impl);
  Queue(impl, [impl, entry] {
    impl->entries.erase(entry->spec.id);
    if (!entry->startup_failure.empty()) {
      auto complete = std::move(entry->started);
      complete(entry->startup_failure);
    }
    auto completions = std::move(entry->stopped);
    for (const auto& complete : completions) complete();
  });
}

MuonNativeServiceHost::MuonNativeServiceHost(MuonNativeServicePlatform platform)
    : impl_(std::make_shared<MuonNativeServiceHostImpl>()) {
  if (!platform.load || !platform.post || !platform.log) {
    throw std::invalid_argument("native service platform operations are required");
  }
  impl_->platform = std::move(platform);
}

MuonNativeServiceHost::~MuonNativeServiceHost() {
  assert(IsIdle());
}

void MuonNativeServiceHost::Start(
    const MuonNativeServiceSpec& spec, uint64_t generation,
    std::function<void(const std::string&)> complete) {
  auto error = std::string{};
  if (spec.id.empty() || spec.library_path.empty() || generation == 0 ||
      spec.config.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
    error = "invalid native service identity or configuration";
  } else if (impl_->entries.contains(spec.id)) {
    error = "native service already has an active generation";
  }
  auto factory = std::shared_ptr<const muon_service_factory>{};
  if (error.empty()) {
    factory = impl_->factories[spec.library_path].lock();
    if (!factory) factory = impl_->platform.load(spec.library_path, error);
    if (error.empty() && !factory) error = "native service library has no factory";
    if (error.empty() && (factory->abi_version != MUON_SERVICE_ABI_VERSION ||
                          !factory->start)) {
      error = "native service factory ABI is incompatible";
    }
  }
  if (!error.empty()) {
    Queue(impl_, [complete = std::move(complete), error] { complete(error); });
    return;
  }
  impl_->factories[spec.library_path] = factory;
  auto entry = std::make_shared<MuonNativeServiceEntry>();
  entry->owner = impl_;
  entry->spec = spec;
  entry->factory = std::move(factory);
  entry->interface_json = entry->factory->interface_json ? entry->factory->interface_json :
      "{\"schemaVersion\":1,\"methods\":{},\"events\":{}}";
  try {
    entry->interface = ParseMuonServiceInterface(entry->interface_json);
  } catch (const std::exception& error) {
    Queue(impl_, [complete = std::move(complete), message = std::string(error.what())] { complete(message); });
    return;
  }
  entry->started = std::move(complete);
  for (const auto& [key, value] : entry->spec.config) {
    entry->config.push_back({key.c_str(), value.c_str()});
  }
  entry->host = {entry.get(), &PostNativeTask, &LogNativeMessage, &EmitEvent};
  entry->context = {&entry->host, entry->spec.id.c_str(), generation,
                    entry->spec.data_directory.c_str(),
                    static_cast<int32_t>(entry->config.size()),
                    entry->config.empty() ? nullptr : entry->config.data()};
  impl_->entries.emplace(spec.id, entry);
  entry->factory->start(&entry->context, &NativeStarted, entry.get());
}

void MuonNativeServiceHost::Stop(const std::string& id, uint64_t generation,
                                std::function<void()> complete) {
  const auto found = impl_->entries.find(id);
  if (found == impl_->entries.end() ||
      found->second->context.generation != generation) {
    Queue(impl_, std::move(complete));
    return;
  }
  const auto entry = found->second;
  if (entry->state == MuonNativeInstanceState::Starting) {
    throw std::logic_error("native service startup must complete before stopping");
  }
  entry->stopped.push_back(std::move(complete));
  if (entry->state == MuonNativeInstanceState::Stopping) return;
  entry->state = MuonNativeInstanceState::Stopping;
  for (const auto& [id, call] : entry->calls) CancelCall(entry, call);
  MaybeStop(entry);
}

static std::shared_ptr<MuonNativeServiceEntry> Running(
    const std::shared_ptr<MuonNativeServiceHostImpl>& impl, const std::string& id, uint64_t generation) {
  const auto found = impl->entries.find(id);
  if (found == impl->entries.end() || found->second->context.generation != generation ||
      found->second->state != MuonNativeInstanceState::Running)
    throw std::invalid_argument("Service generation is not running");
  return found->second;
}

const MuonServiceInterface& MuonNativeServiceHost::Interface(const std::string& id, uint64_t generation) const {
  return Running(impl_, id, generation)->interface;
}

std::string MuonNativeServiceHost::InterfaceJson(const std::string& id, uint64_t generation) const {
  return Running(impl_, id, generation)->interface_json;
}

static void CollectCallbacks(const MuonServiceType& type, const MuonServiceValue& value,
                             std::map<uint64_t, const MuonServiceSignature*>& tokens) {
  if (type.kind == MUON_SERVICE_VALUE_CALLBACK) {
    if (tokens.size() >= 1024 || !tokens.emplace(std::get<MuonServiceCallback>(value.data).token,
        type.callback.get()).second) throw std::invalid_argument("Duplicate or excessive service callback tokens");
  } else if (type.kind == MUON_SERVICE_VALUE_ARRAY) {
    for (const auto& item : std::get<MuonServiceValue::Array>(value.data)) CollectCallbacks(*type.element, item, tokens);
  }
}

uint64_t MuonNativeServiceHost::Invoke(const std::string& id, uint64_t generation, const std::string& method,
    std::vector<MuonServiceValue> arguments, MuonServiceCallbackHandler callback, MuonServiceCallCompletion complete) {
  const auto entry = Running(impl_, id, generation);
  const auto found = entry->interface.methods.find(method);
  if (found == entry->interface.methods.end() || !complete || !callback)
    throw std::invalid_argument("Unknown service method or missing completion");
  if (entry->calls.size() >= 128 || impl_->next_call == std::numeric_limits<uint64_t>::max())
    throw std::invalid_argument("Service invocation capacity exceeded");
  EncodeMuonServiceArguments(found->second.signature.parameters, arguments);
  auto call = std::make_shared<MuonNativeServiceCall>();
  call->self = call;
  call->entry = entry;
  call->method = method;
  call->signature = &found->second;
  for (size_t index = 0; index < arguments.size(); ++index)
    CollectCallbacks(found->second.signature.parameters[index], arguments[index], call->tokens);
  call->arguments = std::make_unique<MuonNativeServiceValues>(std::move(arguments));
  call->callback = std::move(callback);
  call->complete = std::move(complete);
  const auto identity = ++impl_->next_call;
  call->context = {identity, call.get(), &RequestCallback};
  entry->calls.emplace(identity, call);
  const auto values = call->arguments->Get();
  entry->instance.invoke(entry->instance.user_data, &call->context, call->method.c_str(),
      values.data(), values.size(), &NativeCallCompleted, call.get());
  return identity;
}

void MuonNativeServiceHost::Cancel(const std::string& id, uint64_t generation, uint64_t call_id) {
  const auto found = impl_->entries.find(id);
  if (found == impl_->entries.end() || found->second->context.generation != generation) return;
  const auto call = found->second->calls.find(call_id);
  if (call != found->second->calls.end()) CancelCall(found->second, call->second);
}

void MuonNativeServiceHost::SetEventSink(MuonServiceEventSink sink) { impl_->events = std::move(sink); }

bool MuonNativeServiceHost::IsIdle() const {
  return impl_->entries.empty() && impl_->pending_tasks == 0;
}
