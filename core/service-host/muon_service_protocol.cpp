/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "muon_service_protocol.h"
#include <yyjson.h>

#include <charconv>
#include <cstdlib>
#include <stdexcept>

static std::string Quote(const std::string& value) {
  const auto doc = std::unique_ptr<yyjson_mut_doc, decltype(&yyjson_mut_doc_free)>(
      yyjson_mut_doc_new(nullptr), &yyjson_mut_doc_free);
  if (!doc) throw std::bad_alloc();
  auto* root = yyjson_mut_strn(doc.get(), value.data(), value.size());
  if (!root) throw std::bad_alloc();
  yyjson_mut_doc_set_root(doc.get(), root);
  auto size = size_t{0};
  const auto result = std::unique_ptr<char, decltype(&std::free)>(
      yyjson_mut_write(doc.get(), 0, &size), &std::free);
  if (!result) throw std::bad_alloc();
  return {result.get(), size};
}

static std::string Text(yyjson_val* object, const char* name) {
  auto* value = yyjson_obj_get(object, name);
  if (!yyjson_is_str(value)) throw std::invalid_argument(std::string(name) + " must be a string");
  auto text = std::string(yyjson_get_str(value), yyjson_get_len(value));
  if (text.empty() || text.find('\0') != std::string::npos)
    throw std::invalid_argument(std::string(name) + " must be nonempty and contain no NUL");
  return text;
}

static uint64_t Identity(yyjson_val* object, const char* name) {
  const auto text = Text(object, name);
  auto value = uint64_t{0};
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
      value == 0 || text.front() == '0')
    throw std::invalid_argument(std::string(name) + " must be a positive uint64 string");
  return value;
}

static std::string Reference(const MuonServiceReference& value) {
  return "{\"serviceId\":" + Quote(value.service_id) + ",\"refId\":" +
      Quote(std::to_string(value.id)) + ",\"generation\":" +
      Quote(std::to_string(value.generation)) + "}";
}

static const char* StateName(MuonServiceState state) {
  switch (state) {
    case MuonServiceState::Stopped: return "stopped";
    case MuonServiceState::Starting: return "starting";
    case MuonServiceState::Running: return "running";
    case MuonServiceState::Stopping: return "stopping";
    case MuonServiceState::Failed: return "failed";
  }
  throw std::logic_error("unknown service state");
}

std::string SerializeMuonServiceSnapshot(const MuonServiceSnapshot& value) {
  return "{\"id\":" + Quote(value.id) + ",\"state\":" + Quote(StateName(value.state)) +
      ",\"generation\":" + Quote(std::to_string(value.generation)) +
      ",\"revision\":" + Quote(std::to_string(value.revision)) +
      ",\"autostart\":" + (value.autostart ? "true" : "false") +
      ",\"resident\":" + (value.resident ? "true" : "false") +
      ",\"clientReferences\":" + std::to_string(value.client_references) +
      ",\"error\":" + Quote(value.error) + "}";
}

static std::string Reply(const std::string& request, const std::string& result) {
  return "{\"requestId\":" + Quote(request) + ",\"result\":" + result + "}";
}

static std::string Failure(const std::string& request, const std::string& code,
                            const std::string& message) {
  return "{\"requestId\":" + Quote(request) + ",\"error\":{\"code\":" +
      Quote(code) + ",\"message\":" + Quote(message) + "}}";
}

MuonServiceProtocol::MuonServiceProtocol(MuonServiceController& controller,
    std::function<void(const std::string&, const std::string&)> send)
    : controller_(controller), send_(std::move(send)) {}

bool MuonServiceProtocol::OpenOwner(const std::string& owner) {
  return !owner.empty() && owners_.insert(owner).second;
}

void MuonServiceProtocol::CloseOwner(const std::string& owner) {
  if (owners_.erase(owner) == 0) return;
  for (auto it = pending_.begin(); it != pending_.end();) {
    if (it->second.owner == owner) it = pending_.erase(it);
    else ++it;
  }
  controller_.ReleaseOwner(owner);
}

bool MuonServiceProtocol::Receive(const std::string& owner, const std::string& json) {
  if (!owners_.contains(owner) || json.size() > 1024 * 1024) return false;
  const auto doc = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>(
      yyjson_read(json.data(), json.size(), 0), &yyjson_doc_free);
  auto* root = yyjson_doc_get_root(doc.get());
  if (!yyjson_is_obj(root)) return false;
  auto request = std::string{};
  try {
    request = Text(root, "requestId");
    if (request.size() > 128) return false;
    for (const auto& [id, pending] : pending_) {
      if (pending.owner == owner && pending.request == request) return false;
    }
    const auto op = Text(root, "op");
    if (op == "shutdown") {
      controller_.Shutdown("application exit");
      send_(owner, Reply(request, "null"));
    } else if (op == "acquire") {
      const auto service = Text(root, "serviceId");
      if (!controller_.GetSnapshot(service)) {
        send_(owner, Failure(request, "unknown-service", "service is not configured"));
      } else if (const auto reference = controller_.Acquire(service, owner)) {
        pending_.emplace(reference->id, MuonServicePendingRequest{owner, request});
      } else {
        send_(owner, Failure(request, "unavailable", "service acquisition is unavailable"));
      }
    } else if (op == "release") {
      const auto reference = MuonServiceReference{
          Identity(root, "refId"), Text(root, "serviceId"), owner, Identity(root, "generation")};
      controller_.Release(reference);
      send_(owner, Reply(request, "null"));
    } else if (op == "getState") {
      const auto value = controller_.GetSnapshot(Text(root, "serviceId"));
      send_(owner, value ? Reply(request, SerializeMuonServiceSnapshot(value.value()))
                         : Failure(request, "unknown-service", "service is not configured"));
    } else {
      send_(owner, Failure(request, "invalid-request", "unknown service operation"));
    }
  } catch (const std::invalid_argument& error) {
    if (request.empty()) return false;
    send_(owner, Failure(request, "invalid-request", error.what()));
  }
  return true;
}

void MuonServiceProtocol::Notify(const MuonServiceEvent& event) {
  if (!event.reference) return;
  const auto& reference = event.reference.value();
  if (!owners_.contains(reference.owner_id)) return;
  const auto found = pending_.find(reference.id);
  if (found != pending_.end() &&
      (event.kind == MuonServiceEventKind::Acquired ||
       event.kind == MuonServiceEventKind::AcquireFailed ||
       event.kind == MuonServiceEventKind::Invalidated)) {
    const auto request = found->second;
    pending_.erase(found);
    send_(request.owner, event.kind == MuonServiceEventKind::Acquired
        ? Reply(request.request, Reference(reference))
        : Failure(request.request, "unavailable", event.error));
  } else if (event.kind == MuonServiceEventKind::Invalidated) {
    send_(reference.owner_id, "{\"event\":\"invalidated\",\"reference\":" +
        Reference(reference) + ",\"error\":{\"code\":\"invalidated\",\"message\":" +
        Quote(event.error) + "}}");
  }
}
