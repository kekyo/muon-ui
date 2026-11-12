/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "services/muon_service_broker.h"
#include <yyjson.h>

#include <charconv>
#include <cstdlib>
#include <deque>
#include <limits>
#include <stdexcept>

using BrokerReply = std::function<void(const std::string&)>;
static constexpr size_t kQueuedBytes = 16 * 1024 * 1024;
static constexpr auto kDone = "{\"done\":true}";
struct BrokerReference;

struct BrokerCallback {
  uint64_t id;
  MuonServiceSignature signature;
  std::string json;
  MuonServiceCallCompletion complete;
  bool delivered = false;
};

struct BrokerCall {
  uint64_t id;
  uint64_t native_id = 0;
  std::weak_ptr<BrokerReference> reference;
  MuonServiceType result_type;
  std::optional<std::string> result;
  size_t result_bytes = 0;
  BrokerReply result_wait;
  BrokerReply callback_wait;
  std::map<uint64_t, std::shared_ptr<BrokerCallback>> callbacks;
  std::deque<uint64_t> callback_queue;
  uint64_t next_callback = 0;
};

struct BrokerSubscription {
  uint64_t id;
  std::weak_ptr<BrokerReference> reference;
  std::string name;
  MuonServiceType type;
  std::deque<std::string> queue;
  BrokerReply wait;
  std::string failure;
  bool closed = false;
};

struct BrokerReference {
  MuonServiceReference identity;
  std::map<uint64_t, std::shared_ptr<BrokerCall>> calls;
  std::map<uint64_t, std::shared_ptr<BrokerSubscription>> subscriptions;
  uint64_t next_call = 0;
  uint64_t next_subscription = 0;
  bool closed = false;
};

struct MuonServiceBrokerImpl {
  MuonServiceController& controller;
  MuonServiceExecution host;
  std::map<uint64_t, std::shared_ptr<BrokerReference>> references;
  size_t calls = 0;
  size_t subscriptions = 0;
  size_t bytes = 0;
  bool closed = false;
};

static void Require(bool condition, const char* message) {
  if (!condition) throw std::invalid_argument(message);
}

static std::string Text(yyjson_val* value) {
  Require(yyjson_is_str(value), "Expected a service RPC string");
  return {yyjson_get_str(value), yyjson_get_len(value)};
}

static std::string Json(yyjson_val* value) {
  Require(value != nullptr, "Missing service RPC payload");
  auto size = size_t{};
  const auto text = std::unique_ptr<char, decltype(&std::free)>(yyjson_val_write(value, 0, &size), &std::free);
  if (!text) throw std::bad_alloc();
  return {text.get(), size};
}

static std::string Quote(const std::string& text) {
  return EncodeMuonServiceValue({MUON_SERVICE_VALUE_STRING, {}, {}}, {text});
}

static uint64_t Identity(yyjson_val* root, const char* field) {
  const auto text = Text(yyjson_obj_get(root, field));
  auto value = uint64_t{};
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  Require(value != 0 && result.ec == std::errc{} && result.ptr == text.data() + text.size() &&
      std::to_string(value) == text, "Expected a positive service RPC identity");
  return value;
}

static std::string Error(const std::string& code, const std::string& message) {
  return "{\"status\":\"error\",\"code\":" + Quote(code) + ",\"message\":" + Quote(message) + "}";
}

static BrokerReply Take(BrokerReply& callback) {
  auto result = std::move(callback);
  callback = nullptr; // libc++ can retain small std::function targets after move.
  return result;
}

static std::shared_ptr<BrokerReference> Reference(const std::shared_ptr<MuonServiceBrokerImpl>& impl,
                                                 const MuonServiceReference& identity) {
  Require(!impl->closed && impl->controller.IsReady(identity), "Service reference is not callable");
  auto& reference = impl->references[identity.id];
  if (!reference) {
    reference = std::make_shared<BrokerReference>();
    reference->identity = identity;
  }
  Require(!reference->closed && reference->identity == identity, "Service reference owner does not match");
  return reference;
}

static void ClearCallbacks(const std::shared_ptr<MuonServiceBrokerImpl>& impl,
                            const std::shared_ptr<BrokerCall>& call) {
  auto callbacks = std::move(call->callbacks);
  call->callbacks.clear();
  call->callback_queue.clear();
  for (const auto& [id, callback] : callbacks) impl->bytes -= callback->json.size();
  for (const auto& [id, callback] : callbacks)
    callback->complete({{}, "cancelled", "Parent service call ended"});
}

static void EraseCall(const std::shared_ptr<MuonServiceBrokerImpl>& impl,
                       const std::shared_ptr<BrokerReference>& reference,
                       const std::shared_ptr<BrokerCall>& call) {
  if (!reference->calls.erase(call->id)) return;
  --impl->calls;
  impl->bytes -= call->result_bytes;
  ClearCallbacks(impl, call);
}

static void FinishCall(const std::shared_ptr<MuonServiceBrokerImpl>& impl,
                        const std::shared_ptr<BrokerCall>& call, std::string json) {
  const auto reference = call->reference.lock();
  if (!reference || reference->closed || call->result) return;
  ClearCallbacks(impl, call);
  auto result_wait = Take(call->result_wait);
  auto callback_wait = Take(call->callback_wait);
  if (result_wait) {
    EraseCall(impl, reference, call);
  } else {
    if (json.size() > kQueuedBytes - impl->bytes) {
      json = Error("resource-exhausted", "Unconsumed service results exceeded the queue budget");
      // At most 128 small terminal diagnostics use the fixed record budget;
      // they must remain deliverable when the bulk-data budget is exhausted.
    } else call->result_bytes = json.size();
    impl->bytes += call->result_bytes;
    call->result = json;
  }
  if (callback_wait) callback_wait(kDone);
  if (result_wait) result_wait(json);
}

static void NextCallback(const std::shared_ptr<MuonServiceBrokerImpl>& impl,
                          const std::shared_ptr<BrokerCall>& call, BrokerReply reply) {
  Require(!call->callback_wait, "A callback receive is already pending");
  if (call->result) { reply(kDone); return; }
  if (call->callback_queue.empty()) { call->callback_wait = std::move(reply); return; }
  const auto id = call->callback_queue.front();
  call->callback_queue.pop_front();
  const auto callback = call->callbacks.at(id);
  callback->delivered = true;
  impl->bytes -= callback->json.size();
  auto json = std::move(callback->json);
  callback->json.clear();
  reply(json);
}

static void ReceiveCallback(const std::shared_ptr<MuonServiceBrokerImpl>& impl,
    const std::shared_ptr<BrokerCall>& call, uint64_t token,
    const MuonServiceSignature& signature, std::vector<MuonServiceValue> arguments,
    MuonServiceCallCompletion complete) {
  const auto reference = call->reference.lock();
  if (!reference || reference->closed || call->result || !reference->calls.contains(call->id) ||
      !impl->controller.IsReady(reference->identity)) {
    complete({{}, "cancelled", "Service reference ended"});
    return;
  }
  if (call->callbacks.size() >= 32 || call->next_callback == std::numeric_limits<uint64_t>::max()) {
    complete({{}, "resource-exhausted", "Service callback capacity exceeded"});
    return;
  }
  const auto id = ++call->next_callback;
  auto json = "{\"done\":false,\"callbackId\":" + Quote(std::to_string(id)) +
      ",\"token\":" + Quote(std::to_string(token)) + ",\"arguments\":" +
      EncodeMuonServiceArguments(signature.parameters, arguments) + "}";
  if (json.size() > kQueuedBytes - impl->bytes) {
    complete({{}, "resource-exhausted", "Service callback queue exceeded its budget"});
    return;
  }
  impl->bytes += json.size();
  call->callbacks.emplace(id, std::make_shared<BrokerCallback>(
      BrokerCallback{id, signature, std::move(json), std::move(complete), false}));
  call->callback_queue.push_back(id);
  if (auto waiting = Take(call->callback_wait)) NextCallback(impl, call, std::move(waiting));
}

static void ClearEvents(const std::shared_ptr<MuonServiceBrokerImpl>& impl,
                         const std::shared_ptr<BrokerSubscription>& subscription) {
  for (const auto& event : subscription->queue) impl->bytes -= event.size();
  subscription->queue.clear();
}

static void EraseSubscription(const std::shared_ptr<MuonServiceBrokerImpl>& impl,
    const std::shared_ptr<BrokerReference>& reference, const std::shared_ptr<BrokerSubscription>& subscription) {
  if (!reference->subscriptions.erase(subscription->id)) return;
  --impl->subscriptions;
  subscription->closed = true;
  ClearEvents(impl, subscription);
}

static void NextEvent(const std::shared_ptr<MuonServiceBrokerImpl>& impl,
    const std::shared_ptr<BrokerReference>& reference, const std::shared_ptr<BrokerSubscription>& subscription,
    BrokerReply reply) {
  Require(!subscription->wait, "An event receive is already pending");
  if (!subscription->failure.empty()) {
    const auto failure = subscription->failure;
    EraseSubscription(impl, reference, subscription);
    reply(failure);
  } else if (!subscription->queue.empty()) {
    auto json = std::move(subscription->queue.front());
    subscription->queue.pop_front();
    impl->bytes -= json.size();
    reply(json);
  } else subscription->wait = std::move(reply);
}

static void Event(const std::shared_ptr<MuonServiceBrokerImpl>& impl, const std::string& service,
                   uint64_t generation, const std::string& name, const MuonServiceValue& value) {
  auto targets = std::vector<std::shared_ptr<BrokerSubscription>>{};
  for (const auto& [id, reference] : impl->references) {
    if (reference->identity.service_id == service && reference->identity.generation == generation &&
        impl->controller.IsReady(reference->identity)) {
      for (const auto& [token, subscription] : reference->subscriptions)
        if (subscription->name == name) targets.push_back(subscription);
    }
  }
  for (const auto& subscription : targets) {
    const auto reference = subscription->reference.lock();
    if (!reference || reference->closed || subscription->closed || !subscription->failure.empty() ||
        !impl->controller.IsReady(reference->identity)) continue;
    auto json = "{\"done\":false,\"value\":" + EncodeMuonServiceValue(subscription->type, value) + "}";
    if (auto waiting = Take(subscription->wait)) {
      waiting(json);
    } else if (subscription->queue.size() >= 64 || json.size() > kQueuedBytes - impl->bytes) {
      ClearEvents(impl, subscription);
      subscription->failure = "{\"done\":true,\"error\":{\"code\":\"resource-exhausted\","
          "\"message\":\"Service event consumer exceeded its queue budget\"}}";
    } else {
      impl->bytes += json.size();
      subscription->queue.push_back(std::move(json));
    }
  }
}

static void DropReference(const std::shared_ptr<MuonServiceBrokerImpl>& impl,
                           std::shared_ptr<BrokerReference> reference) {
  if (reference->closed) return;
  reference->closed = true;
  impl->references.erase(reference->identity.id);
  auto calls = std::vector<std::shared_ptr<BrokerCall>>{};
  for (const auto& [id, call] : reference->calls) calls.push_back(call);
  auto subscriptions = std::vector<std::shared_ptr<BrokerSubscription>>{};
  for (const auto& [id, subscription] : reference->subscriptions) subscriptions.push_back(subscription);
  auto replies = std::vector<std::pair<BrokerReply, std::string>>{};
  for (const auto& call : calls) {
    if (auto waiting = Take(call->result_wait)) replies.emplace_back(std::move(waiting), Error("cancelled", "Service reference was released"));
    if (auto waiting = Take(call->callback_wait)) replies.emplace_back(std::move(waiting), kDone);
    EraseCall(impl, reference, call);
    impl->host.cancel(reference->identity.service_id, reference->identity.generation, call->native_id);
  }
  for (const auto& subscription : subscriptions) {
    if (auto waiting = Take(subscription->wait)) replies.emplace_back(std::move(waiting), kDone);
    EraseSubscription(impl, reference, subscription);
  }
  for (const auto& [reply, json] : replies) reply(json);
}

MuonServiceBroker::MuonServiceBroker(MuonServiceController& controller, MuonServiceExecution host)
    : impl_(std::make_shared<MuonServiceBrokerImpl>(MuonServiceBrokerImpl{controller, std::move(host), {}, 0, 0, 0, false})) {
  const auto weak = std::weak_ptr<MuonServiceBrokerImpl>(impl_);
  impl_->host.set_event_sink([weak](const std::string& id, uint64_t generation,
      const std::string& name, const MuonServiceValue& value) {
    if (const auto impl = weak.lock()) Event(impl, id, generation, name, value);
  });
}

MuonServiceBroker::~MuonServiceBroker() {
  Shutdown();
  impl_->host.set_event_sink({});
}

void MuonServiceBroker::Release(const MuonServiceReference& reference) {
  impl_->controller.Release(reference);
  const auto found = impl_->references.find(reference.id);
  if (found != impl_->references.end() && found->second->identity == reference) DropReference(impl_, found->second);
}

void MuonServiceBroker::CloseOwner(const std::string& owner) {
  impl_->controller.ReleaseOwner(owner);
  auto references = std::vector<std::shared_ptr<BrokerReference>>{};
  for (const auto& [id, reference] : impl_->references)
    if (reference->identity.owner_id == owner) references.push_back(reference);
  for (const auto& reference : references) DropReference(impl_, reference);
}

void MuonServiceBroker::Shutdown() {
  if (impl_->closed) return;
  impl_->closed = true;
  auto references = std::vector<std::shared_ptr<BrokerReference>>{};
  for (const auto& [id, reference] : impl_->references) references.push_back(reference);
  for (const auto& reference : references) DropReference(impl_, reference);
}

static void Begin(const std::shared_ptr<MuonServiceBrokerImpl>& impl,
    const std::shared_ptr<BrokerReference>& reference, yyjson_val* root, const BrokerReply& reply) {
  Require(impl->calls < 128 && reference->next_call != std::numeric_limits<uint64_t>::max(),
      "Service call capacity exceeded");
  const auto method = Text(yyjson_obj_get(root, "method"));
  const auto& identity = reference->identity;
  const auto& interface = impl->host.interface(identity.service_id, identity.generation);
  const auto found = interface.methods.find(method);
  Require(found != interface.methods.end(), "Unknown service method");
  auto arguments = DecodeMuonServiceArguments(found->second.signature.parameters,
      Json(yyjson_obj_get(root, "arguments")));
  const auto call = std::make_shared<BrokerCall>();
  call->id = ++reference->next_call;
  call->reference = reference;
  call->result_type = found->second.signature.returns;
  reference->calls.emplace(call->id, call);
  ++impl->calls;
  const auto weak_impl = std::weak_ptr<MuonServiceBrokerImpl>(impl);
  const auto weak_call = std::weak_ptr<BrokerCall>(call);
  try {
    call->native_id = impl->host.invoke(identity.service_id, identity.generation, method, std::move(arguments),
      [weak_impl, weak_call](uint64_t token, const MuonServiceSignature& signature,
          std::vector<MuonServiceValue> values, MuonServiceCallCompletion complete) {
        const auto owner = weak_impl.lock();
        const auto pending = weak_call.lock();
        if (!owner || !pending) { complete({{}, "cancelled", "Service call ended"}); return; }
        ReceiveCallback(owner, pending, token, signature, std::move(values), std::move(complete));
      },
      [weak_impl, weak_call](MuonServiceResult result) {
        const auto owner = weak_impl.lock();
        const auto pending = weak_call.lock();
        if (!owner || !pending) return;
        const auto json = result.value ? "{\"status\":\"ok\",\"value\":" +
            EncodeMuonServiceValue(pending->result_type, *result.value) + "}" : Error(result.code, result.message);
        FinishCall(owner, pending, json);
      });
  } catch (...) {
    EraseCall(impl, reference, call);
    throw;
  }
  reply("{\"callId\":" + Quote(std::to_string(call->id)) + "}");
}

static void CallbackResult(const std::shared_ptr<MuonServiceBrokerImpl>& impl,
    const std::shared_ptr<BrokerCall>& call, yyjson_val* root, const BrokerReply& reply) {
  const auto id = Identity(root, "callbackId");
  Require(id <= call->next_callback, "Unknown service callback identity");
  const auto found = call->callbacks.find(id);
  if (found == call->callbacks.end()) { reply("null"); return; }
  const auto callback = found->second;
  Require(callback->delivered, "Service callback has not been received");
  auto result = MuonServiceResult{{}, "invalid-result", "Callback result does not match its interface"};
  try {
    auto* value = yyjson_obj_get(root, "result");
    const auto status = Text(yyjson_obj_get(value, "status"));
    if (status == "ok") {
      result = {DecodeMuonServiceValue(callback->signature.returns, Json(yyjson_obj_get(value, "value"))), {}, {}};
    } else if (status == "error") {
      result = {{}, Text(yyjson_obj_get(value, "code")), Text(yyjson_obj_get(value, "message"))};
    }
  } catch (const std::invalid_argument&) {}
  impl->bytes -= callback->json.size();
  call->callbacks.erase(id);
  callback->complete(std::move(result));
  reply("null");
}

void MuonServiceBroker::Request(const MuonServiceReference& identity, const std::string& action,
    const std::string& payload, std::function<void(const std::string&)> reply) {
  const auto impl = impl_;
  const auto reference = Reference(impl, identity);
  Require(static_cast<bool>(reply) && !payload.empty() && payload.size() <= 8 * 1024 * 1024,
      "Invalid service RPC payload");
  const auto doc = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>(
      yyjson_read(payload.data(), payload.size(), 0), &yyjson_doc_free);
  auto* root = yyjson_doc_get_root(doc.get());
  Require(yyjson_is_obj(root), "Service RPC payload must be an object");
  if (action == "describe") {
    const auto schema = impl->host.interface_json(identity.service_id, identity.generation);
    const auto definition = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>(
        yyjson_read(schema.data(), schema.size(), 0), &yyjson_doc_free);
    // Author-formatted metadata can contain line breaks. Keep every response a
    // compact JSON value for the shared newline-framed transport.
    reply(Json(yyjson_doc_get_root(definition.get())));
    return;
  }
  if (action == "begin") { Begin(impl, reference, root, reply); return; }
  if (action == "result" || action == "cancel" || action == "callbackNext" || action == "callbackResult") {
    const auto id = Identity(root, "callId");
    Require(id <= reference->next_call, "Unknown service call identity");
    const auto found = reference->calls.find(id);
    if (found == reference->calls.end()) {
      reply(action == "result" ? Error("expired", "Service result was already collected") :
          action == "callbackNext" ? kDone : "null");
      return;
    }
    const auto call = found->second;
    if (action == "result") {
      Require(!call->result_wait, "A service result receive is already pending");
      if (call->result) {
        const auto result = *call->result;
        EraseCall(impl, reference, call);
        reply(result);
      } else call->result_wait = std::move(reply);
    } else if (action == "cancel") {
      impl->host.cancel(identity.service_id, identity.generation, call->native_id);
      reply("null");
    } else if (action == "callbackNext") NextCallback(impl, call, std::move(reply));
    else CallbackResult(impl, call, root, reply);
    return;
  }
  if (action == "subscribe") {
    Require(impl->subscriptions < 256 && reference->next_subscription != std::numeric_limits<uint64_t>::max(),
        "Service subscription capacity exceeded");
    const auto name = Text(yyjson_obj_get(root, "event"));
    const auto& interface = impl->host.interface(identity.service_id, identity.generation);
    const auto event = interface.events.find(name);
    Require(event != interface.events.end(), "Unknown service event");
    const auto subscription = std::make_shared<BrokerSubscription>();
    subscription->id = ++reference->next_subscription;
    subscription->reference = reference;
    subscription->name = name;
    subscription->type = event->second;
    reference->subscriptions.emplace(subscription->id, subscription);
    ++impl->subscriptions;
    reply("{\"subscriptionId\":" + Quote(std::to_string(subscription->id)) + "}");
    return;
  }
  if (action == "eventNext" || action == "unsubscribe") {
    const auto id = Identity(root, "subscriptionId");
    Require(id <= reference->next_subscription, "Unknown service subscription identity");
    const auto found = reference->subscriptions.find(id);
    if (found == reference->subscriptions.end()) { reply(action == "eventNext" ? kDone : "null"); return; }
    const auto subscription = found->second;
    if (action == "unsubscribe") {
      auto waiting = Take(subscription->wait);
      EraseSubscription(impl, reference, subscription);
      if (waiting) waiting(kDone);
      reply("null");
    } else NextEvent(impl, reference, subscription, std::move(reply));
    return;
  }
  throw std::invalid_argument("Unknown service RPC operation");
}
