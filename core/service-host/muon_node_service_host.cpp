/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "muon_node_service_host.h"
#include "node_process.h"
#include "services/muon_service_limits.h"
#include <yyjson.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <charconv>
#include <cstdlib>
#include <deque>
#include <set>
#include <stdexcept>

struct NodeServiceCall {
  uint64_t id;
  const MuonServiceMethod* method;
  MuonServiceCallbackHandler callback;
  MuonServiceCallCompletion complete;
  std::map<uint64_t, const MuonServiceSignature*> tokens;
  std::set<uint64_t> callbacks;
  bool cancelled = false;
};

struct NodeServiceEntry {
  std::weak_ptr<MuonNodeServiceHostImpl> owner;
  MuonNodeServiceSpec spec;
  uint64_t generation;
  MuonNodeProcess child;
  MuonServiceInterface interface;
  std::unique_ptr<MuonServiceIo> io;
  std::map<uint64_t, std::function<void()>> io_requests;
  uint64_t io_sequence = 0;
  std::string interface_json;
  std::map<uint64_t, std::shared_ptr<NodeServiceCall>> calls;
  std::map<uint64_t, std::function<void(yyjson_val*)>> requests;
  std::deque<std::vector<std::byte>> writes;
  size_t queued_bytes = 0;
  size_t event_bytes = 0;
  size_t events = 0;
  size_t tasks = 0;
  uint64_t sequence = 0;
  uint64_t callback_sequence = 0;
  bool writer = false;
  bool ready = false;
  bool stopping = false;
  bool stop_acknowledged = false;
  bool closed = false;
  std::string fault;
  std::function<void()> cancel_deadline;
  std::function<void(const std::string&)> started;
  std::vector<std::function<void()>> stopped;
};

struct MuonNodeServiceHostImpl {
  MuonNodeServicePlatform platform;
  std::map<std::pair<std::string, uint64_t>, std::shared_ptr<NodeServiceEntry>> entries;
  MuonServiceEventSink events;
  uint64_t next_call = 0;
  size_t queued = 0;
};

static void Finish(const std::shared_ptr<NodeServiceEntry>& entry);
static void Fault(const std::shared_ptr<NodeServiceEntry>& entry, const std::string& error);

static void Queue(const std::shared_ptr<MuonNodeServiceHostImpl>& owner, std::function<void()> task) {
  ++owner->queued;
  owner->platform.post([owner, task = std::move(task)] {
    --owner->queued;
    task();
  });
}

static std::string Quote(const std::string& value) {
  return EncodeMuonServiceValue({MUON_SERVICE_VALUE_STRING, {}, {}}, {value});
}

static std::string Json(yyjson_val* value) {
  if (!value) throw std::invalid_argument("Missing Node service JSON value");
  const auto text = std::unique_ptr<char, decltype(&std::free)>(yyjson_val_write(value, 0, nullptr), &std::free);
  if (!text) throw std::runtime_error("Unable to encode Node service JSON");
  return text.get();
}

static std::string Field(yyjson_val* object, const char* name) {
  const auto value = yyjson_obj_get(object, name);
  if (!yyjson_is_str(value)) throw std::invalid_argument("Missing Node service string");
  return {yyjson_get_str(value), yyjson_get_len(value)};
}

static uint64_t Identity(yyjson_val* object, const char* name) {
  const auto text = Field(object, name);
  uint64_t value = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !value || std::to_string(value) != text)
    throw std::invalid_argument("Invalid Node service identity");
  return value;
}

static void Complete(const std::shared_ptr<NodeServiceEntry>& entry,
    const std::shared_ptr<NodeServiceCall>& call, MuonServiceResult result) {
  if (!call->complete) return;
  auto complete = std::move(call->complete);
  call->complete = nullptr;
  Queue(entry->owner.lock(), [complete = std::move(complete), result = std::move(result)]() mutable {
    complete(std::move(result));
  });
}

static void Finish(const std::shared_ptr<NodeServiceEntry>& entry) {
  if (entry->closed || entry->tasks || (entry->child.process_started && !entry->child.process_exited)) return;
  if (!entry->stopping && entry->fault.empty()) return;
  entry->closed = true;
  if (entry->cancel_deadline) entry->cancel_deadline();
  entry->cancel_deadline = nullptr;
  CloseMuonNodeTransport(&entry->child);
  CloseMuonNodeProcessHandles(&entry->child);
  entry->writes.clear();
  entry->requests.clear();
  for (const auto& [id, call] : entry->calls) {
    (void)id;
    Complete(entry, call, {{}, "unavailable", entry->fault.empty() ? "Node service stopped" : entry->fault});
  }
  entry->calls.clear();
  const auto owner = entry->owner.lock();
  entry->io->Close([owner, entry] {
  owner->entries.erase({entry->spec.id, entry->generation});
  if (entry->started) {
    auto started = std::move(entry->started);
    entry->started = nullptr;
    started(entry->fault.empty() ? "Node service startup interrupted" : entry->fault);
  } else if (!entry->fault.empty()) {
    owner->platform.exited(entry->spec.id, entry->generation, entry->fault);
  }
  auto stopped = std::move(entry->stopped);
  entry->stopped.clear();
  for (const auto& complete : stopped) complete();
  });
}

static void Done(const std::shared_ptr<NodeServiceEntry>& entry) {
  // Finish on a later turn: all coroutine frames must have returned before the
  // host may destroy the dispatcher or start a replacement generation.
  Queue(entry->owner.lock(), [entry] { --entry->tasks; Finish(entry); });
}

static void Fault(const std::shared_ptr<NodeServiceEntry>& entry, const std::string& error) {
  if (entry->closed || !entry->fault.empty()) return;
  entry->fault = error.empty() ? "Node service failed" : error;
  const auto owner = entry->owner.lock();
  owner->platform.log(entry->spec.id, MUON_LOG_LEVEL_ERROR, entry->fault);
  if (entry->cancel_deadline) entry->cancel_deadline();
  entry->cancel_deadline = nullptr;
  entry->child.io_cancellation.cancel();
  TerminateMuonNodeProcess(&entry->child, true);
  Queue(owner, [entry] { Finish(entry); });
}

static void Deadline(const std::shared_ptr<NodeServiceEntry>& entry, const std::string& message,
    std::optional<std::chrono::milliseconds> remaining = std::nullopt) {
  if (entry->cancel_deadline) entry->cancel_deadline();
  const auto owner = entry->owner.lock();
  entry->cancel_deadline = owner->platform.delay(remaining.value_or(owner->platform.lifecycle_timeout),
      [entry, message] { Fault(entry, message); });
}

static cardio::promise<void> Write(const std::shared_ptr<NodeServiceEntry> entry) {
  try {
    while (!entry->writes.empty() && entry->fault.empty()) {
      const auto frame = std::move(entry->writes.front());
      entry->writes.pop_front();
      co_await WriteMuonNodeExact(&entry->child, frame);
      entry->queued_bytes -= frame.size();
    }
  } catch (const std::exception& error) { Fault(entry, error.what()); }
  entry->writer = false;
  Done(entry);
}

static void Send(const std::shared_ptr<NodeServiceEntry>& entry, const std::string& json) {
  if (entry->closed || !entry->fault.empty()) throw std::runtime_error("Node service is closed");
  auto frame = CreateMuonNodeFrame(json);
  if (entry->queued_bytes + frame.size() > kMuonServiceMaxQueuedBytes)
    throw std::runtime_error("Node service output queue is full");
  entry->queued_bytes += frame.size();
  entry->writes.push_back(std::move(frame));
  if (!entry->writer) {
    entry->writer = true;
    ++entry->tasks;
    cardio::fire_and_forget(Write(entry));
  }
}

static void Request(const std::shared_ptr<NodeServiceEntry>& entry, const std::string& command,
    const std::string& params, std::function<void(yyjson_val*)> complete) {
  if (entry->requests.size() >= 1024 || entry->sequence == UINT64_MAX)
    throw std::runtime_error("Node service request limit exceeded");
  const auto id = ++entry->sequence;
  entry->requests.emplace(id, std::move(complete));
  Send(entry, "{\"kind\":\"request\",\"id\":" + Quote(std::to_string(id)) +
      ",\"command\":" + Quote(command) + ",\"params\":" + params + "}");
}

static MuonServiceResult Result(const MuonServiceMethod& method, yyjson_val* value) {
  const auto status = Field(value, "status");
  if (status == "ok") return {DecodeMuonServiceValue(method.signature.returns,
      Json(yyjson_obj_get(value, "value"))), {}, {}};
  if (status != "error") throw std::invalid_argument("Unknown Node service result");
  const auto code = Field(value, "code");
  if (!IsMuonServiceFrameworkError(code) &&
      std::find(method.errors.begin(), method.errors.end(), code) == method.errors.end())
    throw std::invalid_argument("Undeclared Node service failure");
  return {{}, code, Field(value, "message")};
}

static std::string CallbackResult(const MuonServiceSignature& signature, const MuonServiceResult& result) {
  if (result.value && result.code.empty())
    return "{\"status\":\"ok\",\"value\":" + EncodeMuonServiceValue(signature.returns, *result.value) + "}";
  return "{\"status\":\"error\",\"code\":" + Quote(result.code.empty() ? "callback-failed" : result.code) +
      ",\"message\":" + Quote(result.message) + "}";
}

static void Callback(const std::shared_ptr<NodeServiceEntry>& entry, yyjson_val* message) {
  const auto id = Identity(message, "id");
  if (id <= entry->callback_sequence) throw std::invalid_argument("Duplicate Node service callback");
  entry->callback_sequence = id;
  const auto found = entry->calls.find(Identity(message, "callId"));
  if (found == entry->calls.end()) throw std::invalid_argument("Unknown Node service callback owner");
  const auto call = found->second;
  // The cancel frame and a callback may pass in flight. The child settles all
  // canceled callbacks itself; never dispatch these to a released page.
  if (call->cancelled || entry->stopping) return;
  const auto token = Identity(message, "token");
  const auto signature = call->tokens.find(token);
  if (signature == call->tokens.end() || !call->callback || call->callbacks.size() >= 128)
    throw std::invalid_argument("Unknown or excessive Node service callback token");
  auto arguments = DecodeMuonServiceArguments(signature->second->parameters, Json(yyjson_obj_get(message, "arguments")));
  call->callbacks.insert(id);
  const auto weak = std::weak_ptr<NodeServiceEntry>(entry);
  const auto call_id = call->id;
  const auto copied_signature = *signature->second;
  call->callback(token, copied_signature, std::move(arguments), [weak, call_id, id, copied_signature](MuonServiceResult result) {
    const auto current = weak.lock();
    if (!current || current->closed || !current->fault.empty()) return;
    Queue(current->owner.lock(), [current, call_id, id, copied_signature, result = std::move(result)] {
      const auto found = current->calls.find(call_id);
      if (current->closed || !current->fault.empty() || current->stopping || found == current->calls.end() ||
          found->second->cancelled || !found->second->callbacks.erase(id)) return;
      try {
        std::string json;
        try { json = CallbackResult(copied_signature, result); }
        catch (const std::exception& error) { json = CallbackResult(copied_signature, {{}, "invalid-result", error.what()}); }
        Send(current, "{\"kind\":\"callbackResult\",\"id\":" + Quote(std::to_string(id)) + ",\"result\":" + json + "}");
      } catch (const std::exception& error) { Fault(current, error.what()); }
    });
  });
}

static void RequestIo(const std::shared_ptr<NodeServiceEntry>& entry, yyjson_val* message) {
  const auto id = Identity(message, "id");
  if (id <= entry->io_sequence || entry->io_requests.size() >= 64) throw std::invalid_argument("Invalid Node service I/O identity");
  entry->io_sequence = id;
  const MuonServiceType binary{MUON_SERVICE_VALUE_BYTES, {}, {}};
  auto bytes = DecodeMuonServiceValue(binary, Json(yyjson_obj_get(message, "body")));
  entry->io_requests.emplace(id, entry->io->Run({Field(message, "operation"), Json(yyjson_obj_get(message, "parameters")),
      std::move(std::get<MuonServiceValue::Bytes>(bytes.data))}, [entry, id, binary](MuonServiceIoReply reply) {
    entry->io_requests.erase(id);
    if (entry->closed || !entry->fault.empty()) return;
    try {
      auto result = "{\"kind\":\"ioResult\",\"id\":" + Quote(std::to_string(id));
      if (reply.code.empty()) result += ",\"metadata\":" + reply.metadata + ",\"body\":" + EncodeMuonServiceValue(binary, {std::move(reply.body)});
      else result += ",\"error\":{\"code\":" + Quote(reply.code) + ",\"message\":" + Quote(reply.message) + "}";
      Send(entry, result + "}");
    } catch (const std::exception& error) { Fault(entry, error.what()); }
  }));
}

static void Message(const std::shared_ptr<NodeServiceEntry>& entry, const std::string& json) {
  const auto document = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>(
      yyjson_read(json.data(), json.size(), 0), &yyjson_doc_free);
  if (!document) throw std::invalid_argument("Invalid Node service frame");
  const auto message = yyjson_doc_get_root(document.get());
  const auto kind = Field(message, "kind");
  if (kind == "response") {
    const auto id = Identity(message, "id");
    const auto found = entry->requests.find(id);
    if (found == entry->requests.end()) throw std::invalid_argument("Unknown Node service response");
    auto complete = std::move(found->second);
    entry->requests.erase(found);
    if (!yyjson_is_true(yyjson_obj_get(message, "ok")))
      throw std::runtime_error("Node service request failed: " + Field(yyjson_obj_get(message, "error"), "message"));
    complete(yyjson_obj_get(message, "value"));
  } else if (kind == "io") RequestIo(entry, message);
  else if (kind == "ioCancel") {
    const auto found = entry->io_requests.find(Identity(message, "id"));
    if (found != entry->io_requests.end()) found->second();
  } else if (kind == "callback") Callback(entry, message);
  else if (kind == "event") {
    if (Field(message, "serviceId") != entry->spec.id || Identity(message, "generation") != entry->generation)
      throw std::invalid_argument("Wrong Node service event generation");
    if (entry->stopping) return;
    if (!entry->ready) return; // Startup events have no acquired subscribers yet.
    const auto name = Field(message, "event");
    const auto found = entry->interface.events.find(name);
    if (found == entry->interface.events.end()) throw std::invalid_argument("Unknown Node service event");
    auto value = DecodeMuonServiceValue(found->second, Json(yyjson_obj_get(message, "value")));
    if (++entry->events > 1024 || entry->event_bytes + json.size() > kMuonServiceMaxQueuedBytes)
      throw std::runtime_error("Node service event queue is full");
    entry->event_bytes += json.size();
    Queue(entry->owner.lock(), [entry, name, value = std::move(value), bytes = json.size()] {
      --entry->events;
      entry->event_bytes -= bytes;
      const auto owner = entry->owner.lock();
      if (entry->ready && !entry->closed && !entry->stopping && entry->fault.empty() && owner->events)
        owner->events(entry->spec.id, entry->generation, name, value);
    });
  } else throw std::invalid_argument("Unknown Node service message kind");
}

static cardio::promise<void> Read(const std::shared_ptr<NodeServiceEntry> entry) {
  try {
    while (entry->fault.empty()) {
      std::array<std::byte, 4> header{};
      co_await ReadMuonNodeExact(&entry->child, header);
      uint32_t size = 0;
      for (const auto byte : header) size = (size << 8) | std::to_integer<uint32_t>(byte);
      if (!size || size > 16 * 1024 * 1024) throw std::invalid_argument("Invalid Node service frame size");
      std::string json(size, '\0');
      co_await ReadMuonNodeExact(&entry->child, std::as_writable_bytes(std::span<char>(json)));
      Message(entry, json);
    }
  } catch (const std::exception& error) {
    if (!entry->stopping || !entry->stop_acknowledged) Fault(entry, error.what());
  }
  Done(entry);
}

static cardio::promise<void> Monitor(const std::shared_ptr<NodeServiceEntry> entry) {
  try {
    const auto result = co_await WaitMuonNodeProcess(&entry->child);
    entry->child.process_exited = true;
    if (!entry->stopping || !result.available || result.exit_code != 0)
      Fault(entry, "Node service terminated with exit code " + std::to_string(result.exit_code));
    // A normal exit can precede the read continuation. Let it drain the stop
    // response and EOF before deciding whether author cleanup was acknowledged.
    if (!entry->child.transport_connected) entry->child.io_cancellation.cancel();
  } catch (const std::exception& error) { Fault(entry, error.what()); }
  Done(entry);
}

static cardio::promise<void> StartChild(const std::shared_ptr<NodeServiceEntry> entry) {
  try {
    entry->child.session_token = CreateMuonNodeSessionToken();
    co_await StartMuonNodeProcess(&entry->child, true, [entry] {
      ++entry->tasks;
      cardio::fire_and_forget(Monitor(entry));
    });
    if (!entry->fault.empty()) throw std::runtime_error(entry->fault);
    ++entry->tasks;
    cardio::fire_and_forget(Read(entry));
    auto config = std::string("{");
    for (const auto& [key, value] : entry->spec.config) {
      if (config.size() > 1) config += ',';
      config += Quote(key) + ':' + Quote(value);
    }
    config += '}';
    Request(entry, "initialize", "{\"protocol\":\"node-service/1\",\"token\":" + Quote(entry->child.session_token) +
        ",\"projectRoot\":" + Quote(entry->spec.project) + ",\"entry\":" + Quote(entry->spec.entry) +
        ",\"id\":" + Quote(entry->spec.id) + ",\"generation\":" + Quote(std::to_string(entry->generation)) +
        ",\"startReason\":" + Quote(MuonServiceReasonName(entry->spec.start_reason)) +
        ",\"config\":" + config + ",\"dataDirectory\":" + Quote(entry->spec.data_directory) + ",\"io\":" + entry->spec.io.policy + "}",
        [entry](yyjson_val* value) {
          if (Field(value, "protocol") != "node-service/1") throw std::invalid_argument("Unsupported Node service protocol");
          entry->interface_json = Json(yyjson_obj_get(value, "interface"));
          entry->interface = ParseMuonServiceInterface(entry->interface_json);
          entry->ready = true;
          if (entry->cancel_deadline) entry->cancel_deadline();
          entry->cancel_deadline = nullptr;
          Queue(entry->owner.lock(), [entry] {
            if (!entry->started || !entry->fault.empty()) return;
            auto started = std::move(entry->started);
            entry->started = nullptr;
            started({});
          });
        });
    if (entry->spec.signal) {
      const auto cancel = [weak = std::weak_ptr<NodeServiceEntry>(entry)] {
        const auto current = weak.lock();
        if (!current || current->ready || current->closed || !current->fault.empty()) return;
        try { Request(current, "cancelStart", "{}", [](yyjson_val*) {}); }
        catch (const std::exception& error) { Fault(current, error.what()); }
      };
      if (!entry->spec.signal->Subscribe(cancel)) cancel();
    }
  } catch (const std::exception& error) { Fault(entry, error.what()); }
  Done(entry);
}

static void CollectTokens(const MuonServiceType& type, const MuonServiceValue& value,
    std::map<uint64_t, const MuonServiceSignature*>& tokens) {
  if (type.kind == MUON_SERVICE_VALUE_CALLBACK) {
    if (tokens.size() >= 1024 || !tokens.emplace(std::get<MuonServiceCallback>(value.data).token,
        type.callback.get()).second) throw std::invalid_argument("Duplicate or excessive service callback tokens");
  } else if (type.kind == MUON_SERVICE_VALUE_ARRAY) {
    for (const auto& item : std::get<MuonServiceValue::Array>(value.data)) CollectTokens(*type.element, item, tokens);
  }
}

static std::shared_ptr<NodeServiceEntry> Running(const std::shared_ptr<MuonNodeServiceHostImpl>& owner,
    const std::string& id, uint64_t generation) {
  const auto found = owner->entries.find({id, generation});
  if (found == owner->entries.end() || !found->second->ready || found->second->stopping ||
      found->second->closed || !found->second->fault.empty()) throw std::runtime_error("Node service generation is not running");
  return found->second;
}

MuonNodeServiceHost::MuonNodeServiceHost(MuonNodeServicePlatform platform)
    : impl_(std::make_shared<MuonNodeServiceHostImpl>()) { impl_->platform = std::move(platform); }
MuonNodeServiceHost::~MuonNodeServiceHost() { assert(IsIdle()); }
bool MuonNodeServiceHost::IsIdle() const { return impl_->entries.empty() && !impl_->queued; }

void MuonNodeServiceHost::Start(const MuonNodeServiceSpec& spec, uint64_t generation,
    std::function<void(const std::string&)> complete) {
  if (!generation || impl_->entries.contains({spec.id, generation})) throw std::logic_error("Duplicate Node service generation");
  const auto entry = std::make_shared<NodeServiceEntry>();
  entry->owner = impl_;
  entry->spec = spec;
  entry->io = std::make_unique<MuonServiceIo>(spec.data_directory, spec.io, impl_->platform.post, impl_->platform.delay);
  entry->generation = generation;
  entry->child.executable_path = spec.executable;
  entry->child.bridge_path = spec.bridge;
  entry->started = std::move(complete);
  impl_->entries.emplace(std::make_pair(spec.id, generation), entry);
  Queue(impl_, [entry] {
    Deadline(entry, "Node service startup deadline exceeded");
    ++entry->tasks;
    cardio::fire_and_forget(StartChild(entry));
  });
}

void MuonNodeServiceHost::Stop(const std::string& id, uint64_t generation, std::function<void()> complete, MuonServiceStopCause cause) {
  const auto found = impl_->entries.find({id, generation});
  if (found == impl_->entries.end()) { Queue(impl_, std::move(complete)); return; }
  const auto entry = found->second;
  entry->stopped.push_back(std::move(complete));
  if (entry->stopping || !entry->fault.empty()) return;
  entry->stopping = true;
  const auto deadline = std::chrono::steady_clock::now() + impl_->platform.lifecycle_timeout;
  cause.deadline = std::min(deadline, cause.deadline.value_or(deadline));
  Queue(impl_, [entry, cause = std::move(cause)] {
    if (entry->closed || !entry->fault.empty()) return;
    try {
      const auto remaining = MuonServiceRemainingMs(cause);
      Deadline(entry, "Node service stop deadline exceeded", std::chrono::milliseconds(remaining));
      for (const auto& [id, call] : entry->calls) {
        (void)id;
        call->cancelled = true;
        call->callbacks.clear();
        Complete(entry, call, {{}, "cancelled", "Node service stopped"});
      }
      Request(entry, "stop", "{\"reason\":" + Quote(MuonServiceReasonName(cause.reason)) +
          (cause.detail.empty() ? "" : ",\"detail\":" + Quote(cause.detail)) +
          ",\"remainingMs\":" + std::to_string(remaining) + "}", [entry](yyjson_val* value) {
        if (!yyjson_is_true(yyjson_obj_get(value, "stopped"))) throw std::invalid_argument("Invalid Node service stop response");
        entry->stop_acknowledged = true;
      });
    } catch (const std::exception& error) { Fault(entry, error.what()); }
  });
}

MuonServiceExecution MuonNodeServiceHost::Execution() {
  const auto owner = impl_;
  return {
    [owner](const std::string& id, uint64_t generation) { return std::cref(Running(owner, id, generation)->interface); },
    [owner](const std::string& id, uint64_t generation) { return Running(owner, id, generation)->interface_json; },
    [owner](const std::string& id, uint64_t generation, const std::string& method,
        std::vector<MuonServiceValue> arguments, MuonServiceCallbackHandler callback, MuonServiceCallCompletion complete) {
      const auto entry = Running(owner, id, generation);
      const auto& signature = entry->interface.methods.at(method);
      const auto json = EncodeMuonServiceArguments(signature.signature.parameters, arguments);
      if (entry->calls.size() >= 128 || owner->next_call == UINT64_MAX) throw std::runtime_error("Node service call limit exceeded");
      const auto call = std::make_shared<NodeServiceCall>();
      call->id = ++owner->next_call;
      call->method = &signature;
      call->callback = std::move(callback);
      call->complete = std::move(complete);
      for (size_t index = 0; index < arguments.size(); ++index) CollectTokens(signature.signature.parameters[index], arguments[index], call->tokens);
      entry->calls.emplace(call->id, call);
      Queue(owner, [entry, call, method, json] {
        if (entry->closed || !entry->fault.empty()) return;
        if (call->cancelled || entry->stopping) {
          Complete(entry, call, {{}, "cancelled", "Node service call canceled before dispatch"});
          entry->calls.erase(call->id);
          return;
        }
        try {
          Request(entry, "invoke", "{\"callId\":" + Quote(std::to_string(call->id)) + ",\"method\":" + Quote(method) +
              ",\"arguments\":" + json + "}", [entry, call](yyjson_val* result) {
            try {
              if (!call->callbacks.empty()) throw std::runtime_error("Service completed with callbacks outstanding");
              Complete(entry, call, Result(*call->method, result));
            } catch (const std::exception& error) { Complete(entry, call, {{}, "invalid-result", error.what()}); }
            entry->calls.erase(call->id);
          });
        } catch (const std::exception& error) { Fault(entry, error.what()); }
      });
      return call->id;
    },
    [owner](const std::string& id, uint64_t generation, uint64_t identity) {
      const auto found = owner->entries.find({id, generation});
      if (found == owner->entries.end()) return;
      const auto entry = found->second;
      const auto found_call = entry->calls.find(identity);
      if (found_call == entry->calls.end() || found_call->second->cancelled) return;
      const auto call = found_call->second;
      call->cancelled = true;
      call->callbacks.clear();
      Complete(entry, call, {{}, "cancelled", "Node service call canceled"});
      Queue(owner, [entry, identity] {
        if (entry->closed || !entry->fault.empty() || entry->stopping) return;
        try { Request(entry, "cancel", "{\"callId\":" + Quote(std::to_string(identity)) + "}", [](yyjson_val*) {}); }
        catch (const std::exception& error) { Fault(entry, error.what()); }
      });
    },
    [owner](MuonServiceEventSink sink) { owner->events = std::move(sink); }
  };
}
