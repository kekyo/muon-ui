/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "services/muon_service_client.h"
#include "services/muon_service_limits.h"
#include <yyjson.h>
#include <cstdlib>
#include <deque>
#include <map>
#include <limits>
#include <optional>
#include <stdexcept>

struct MuonServiceClientImpl {
  std::function<cardio::promise<MuonServiceTransport>(cardio::cancellation)> connect;
  cardio::cancellation_source cancellation;
  std::optional<MuonServiceTransport> transport;
  // A cardio promise has one awaiting continuation. The manual condition gives
  // each concurrent closer its own independently awaited disposal promise.
  cardio::primitives::manually_conditional disposed{true};
  std::map<std::string, cardio::promise_source<std::string>> pending;
  std::deque<std::string> output;
  uint64_t sequence = 0;
  size_t output_bytes = 0;
  size_t active_tasks = 0;
  bool connection_started = false;
  bool connected = false;
  bool writer_running = false;
  bool closed = false;
  std::string failure;
};

static std::string Text(yyjson_val* object, const char* field) {
  auto* value = yyjson_obj_get(object, field);
  if (!yyjson_is_str(value)) throw std::runtime_error("Missing service protocol string");
  return {yyjson_get_str(value), yyjson_get_len(value)};
}

static std::string Json(yyjson_val* value) {
  if (!value) throw std::runtime_error("Missing service protocol value");
  auto size = size_t{};
  auto text = std::unique_ptr<char, decltype(&std::free)>(
      yyjson_val_write(value, 0, &size), std::free);
  if (!text) throw std::bad_alloc();
  return {text.get(), size};
}

static void Fail(const std::shared_ptr<MuonServiceClientImpl>& impl, const std::string& reason) {
  if (impl->closed) return;
  impl->closed = true;
  impl->failure = reason;
  auto pending = std::move(impl->pending);
  impl->pending.clear();
  impl->output.clear();
  impl->output_bytes = 0;
  impl->cancellation.cancel();
  if (impl->transport && impl->transport->close) impl->transport->close();
  const auto error = std::make_exception_ptr(std::runtime_error(reason));
  for (auto& [id, completion] : pending) {
    (void)id;
    completion.try_reject(error);
  }
}

static void BeginTask(const std::shared_ptr<MuonServiceClientImpl>& impl) {
  ++impl->active_tasks;
  impl->disposed.drop();
}

static void EndTask(const std::shared_ptr<MuonServiceClientImpl>& impl) {
  if (--impl->active_tasks == 0) impl->disposed.raise();
}

static void Receive(const std::shared_ptr<MuonServiceClientImpl>& impl, const std::string& frame) {
  auto doc = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>(
      yyjson_read(frame.data(), frame.size(), 0), yyjson_doc_free);
  if (!doc) throw std::runtime_error("Invalid service protocol JSON");
  auto* root = yyjson_doc_get_root(doc.get());
  // Invalidated references are already unusable at the host. The broker settles
  // held call/event requests; later operations observe the invalid generation.
  if (yyjson_obj_get(root, "event")) return;
  const auto id = Text(root, "requestId");
  auto* error = yyjson_obj_get(root, "error");
  const auto result = error ? Text(error, "message") : Json(yyjson_obj_get(root, "result"));
  const auto found = impl->pending.find(id);
  if (found == impl->pending.end()) throw std::runtime_error("Unexpected service response identity");
  auto completion = std::move(found->second);
  impl->pending.erase(found);
  if (error) {
    completion.try_reject(std::make_exception_ptr(std::runtime_error(result)));
  } else {
    completion.try_resolve(result);
  }
}

static cardio::promise<void> Read(std::shared_ptr<MuonServiceClientImpl> impl) {
  try {
    std::string input;
    while (!impl->closed) {
      auto chunk = co_await impl->transport->read();
      if (chunk.empty()) throw std::runtime_error("Service host disconnected");
      input += chunk;
      size_t end;
      while (!impl->closed && (end = input.find('\n')) != std::string::npos) {
        if (end > kMuonServiceMaxFrameBytes) throw std::runtime_error("Service response exceeds frame limit");
        Receive(impl, input.substr(0, end));
        input.erase(0, end + 1);
      }
      if (input.size() > kMuonServiceMaxFrameBytes) throw std::runtime_error("Service response exceeds frame limit");
    }
  } catch (const std::exception& error) {
    Fail(impl, error.what());
  }
  EndTask(impl);
}

static void StartWriter(const std::shared_ptr<MuonServiceClientImpl>& impl);

static cardio::promise<void> Connect(std::shared_ptr<MuonServiceClientImpl> impl) {
  try {
    impl->transport.emplace(co_await impl->connect(impl->cancellation.get_cancellation()));
    if (!impl->transport->read || !impl->transport->write || !impl->transport->close)
      throw std::runtime_error("Incomplete service transport");
    if (impl->closed) impl->transport->close();
    else {
      impl->connected = true;
      BeginTask(impl);
      cardio::fire_and_forget(Read(impl));
      StartWriter(impl);
    }
  } catch (const std::exception& error) {
    Fail(impl, error.what());
  }
  EndTask(impl);
}

static cardio::promise<void> Write(std::shared_ptr<MuonServiceClientImpl> impl) {
  try {
    while (!impl->closed && !impl->output.empty()) {
      auto frame = std::move(impl->output.front());
      impl->output.pop_front();
      impl->output_bytes -= frame.size();
      co_await impl->transport->write(std::move(frame));
    }
  } catch (const std::exception& error) {
    Fail(impl, error.what());
  }
  impl->writer_running = false;
  EndTask(impl);
}

static void StartWriter(const std::shared_ptr<MuonServiceClientImpl>& impl) {
  if (impl->closed || !impl->connected || impl->writer_running || impl->output.empty()) return;
  impl->writer_running = true;
  BeginTask(impl);
  cardio::fire_and_forget(Write(impl));
}

static cardio::promise<std::string> Call(std::shared_ptr<MuonServiceClientImpl> impl,
                                        std::string operation, std::string json) {
  if (operation != "acquire" && operation != "release" && operation != "getState" &&
      operation != "setAutostart" && operation != "watchState" && operation != "rpc" &&
      operation != "getUiAutostart" && operation != "setUiAutostart" && operation != "configureStartup")
    throw std::invalid_argument("Unsupported service client operation");
  auto doc = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>(
      yyjson_read(json.data(), json.size(), 0), yyjson_doc_free);
  if (!doc) throw std::invalid_argument("Invalid service request JSON");
  auto* root = yyjson_doc_get_root(doc.get());
  if (!yyjson_is_obj(root)) throw std::invalid_argument("Service request must be an object");
  auto payload = std::string{};
  if (operation == "configureStartup") {
    auto* startup = yyjson_obj_get(root, "startup");
    if (!yyjson_is_obj(startup)) throw std::invalid_argument("startup must be an object");
    payload = ",\"startup\":" + Json(startup);
  } else if (operation != "getUiAutostart" && operation != "setUiAutostart") {
    Text(root, "serviceId");
    payload = ",\"serviceId\":" + Json(yyjson_obj_get(root, "serviceId"));
  }
  if (operation == "release" || operation == "rpc") {
    Text(root, "refId");
    Text(root, "generation");
    payload += ",\"refId\":" + Json(yyjson_obj_get(root, "refId")) +
               ",\"generation\":" + Json(yyjson_obj_get(root, "generation"));
    if (operation == "rpc") {
      Text(root, "action");
      auto* arguments = yyjson_obj_get(root, "payload");
      if (!yyjson_is_obj(arguments)) throw std::invalid_argument("Service RPC payload must be an object");
      payload += ",\"action\":" + Json(yyjson_obj_get(root, "action")) + ",\"payload\":" + Json(arguments);
    }
  } else if (operation == "setAutostart" || operation == "setUiAutostart") {
    auto* enabled = yyjson_obj_get(root, "enabled");
    if (!yyjson_is_bool(enabled)) throw std::invalid_argument("enabled must be boolean");
    payload += ",\"enabled\":" + Json(enabled);
  } else if (operation == "watchState") {
    const auto action = Text(root, "operation");
    if (action != "acquire" && action != "next" && action != "release")
      throw std::invalid_argument("Unsupported service observation operation");
    payload += ",\"operation\":" + Json(yyjson_obj_get(root, "operation"));
    if (action != "acquire") {
      Text(root, "token");
      payload += ",\"token\":" + Json(yyjson_obj_get(root, "token"));
    }
  }
  if (impl->closed) throw std::runtime_error(impl->failure);
  if (impl->sequence == std::numeric_limits<uint64_t>::max())
    throw std::overflow_error("Service request identity exhausted");
  const auto id = std::to_string(++impl->sequence);
  auto frame = "{\"requestId\":\"" + id + "\",\"op\":\"" + operation + "\"" + payload + "}\n";
  if (frame.size() > kMuonServiceMaxFrameBytes || impl->output_bytes + frame.size() > kMuonServiceMaxQueuedBytes)
    throw std::runtime_error("Service request queue is full");
  auto response = impl->pending.emplace(id, cardio::promise_source<std::string>()).first->second.get_promise();
  impl->output_bytes += frame.size();
  impl->output.push_back(std::move(frame));
  if (!impl->connection_started) {
    impl->connection_started = true;
    BeginTask(impl);
    cardio::fire_and_forget(Connect(impl));
  }
  StartWriter(impl);
  co_return co_await response;
}

static cardio::promise<void> Close(std::shared_ptr<MuonServiceClientImpl> impl) {
  Fail(impl, "Service page session ended");
  co_await impl->disposed.wait();
  impl->transport.reset();
}

MuonServiceClient::MuonServiceClient(
    std::function<cardio::promise<MuonServiceTransport>(cardio::cancellation)> connect)
    : impl_(std::make_shared<MuonServiceClientImpl>()) {
  impl_->connect = std::move(connect);
}

MuonServiceClient::~MuonServiceClient() { Fail(impl_, "Service page session ended"); }

cardio::promise<std::string> MuonServiceClient::Call(std::string operation, std::string json) {
  return ::Call(impl_, std::move(operation), std::move(json));
}

cardio::promise<void> MuonServiceClient::Close() { return ::Close(impl_); }
