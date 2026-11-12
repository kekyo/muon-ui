/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "muon_service_protocol.h"
#include <yyjson.h>

#include <deque>
#include <iostream>
#include <stdexcept>

static void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

static void PolicyAndObservation() {
  auto queue = std::deque<std::function<void()>>{};
  const auto pump = [&] {
    while (!queue.empty()) {
      auto task = std::move(queue.front());
      queue.pop_front();
      task();
    }
  };
  auto saves = std::deque<std::function<void(const std::string&)>>{};
  auto stop = std::function<void()>{};
  auto starts = 0;
  auto messages = std::map<std::string, std::string>{};
  auto* protocol = static_cast<MuonServiceProtocol*>(nullptr);
  auto controller = MuonServiceController({{"worker", false, std::nullopt}}, {
      [&](const MuonServiceSnapshot&, auto complete) { ++starts; complete(""); },
      [&](const MuonServiceSnapshot&, auto complete) { stop = std::move(complete); },
      [&](const std::string&, bool, auto complete) { saves.push_back(std::move(complete)); },
      [&](const MuonServiceEvent& event) { protocol->Notify(event); },
      [&](std::function<void()> task) { queue.push_back(std::move(task)); }});
  auto wire = MuonServiceProtocol(controller, [&](const auto& owner, const auto& json) {
    const auto doc = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>(
        yyjson_read(json.data(), json.size(), 0), &yyjson_doc_free);
    auto* request = yyjson_obj_get(yyjson_doc_get_root(doc.get()), "requestId");
    Require(yyjson_is_str(request), "management replies need their request identity");
    messages.emplace(owner + ":" + yyjson_get_str(request), json);
  });
  protocol = &wire;
  wire.OpenOwner("ui");
  wire.OpenOwner("other");
  controller.Activate();
  wire.Receive("ui", R"({"requestId":"watch","op":"watchState","operation":"acquire","serviceId":"worker"})");
  const auto doc = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>(
      yyjson_read(messages.at("ui:watch").data(), messages.at("ui:watch").size(), 0), &yyjson_doc_free);
  auto* result = yyjson_obj_get(yyjson_doc_get_root(doc.get()), "result");
  Require(yyjson_is_obj(result), "a non-owning watch must return the initial state");
  const auto token = std::string(yyjson_get_str(yyjson_obj_get(result, "token")));
  const auto watching = "\",\"op\":\"watchState\",\"serviceId\":\"worker\",\"token\":\"" + token;
  wire.Receive("other", "{\"requestId\":\"forged" + watching + "\",\"operation\":\"next\"}");
  Require(messages.at("other:forged").find("invalid-watch") != std::string::npos,
      "another page must not consume a known observer token");
  wire.Receive("other", "{\"requestId\":\"forgedClose" + watching + "\",\"operation\":\"release\"}");
  wire.Receive("ui", "{\"requestId\":\"first" + watching + "\",\"operation\":\"next\"}");
  pump();
  Require(starts == 0 && controller.GetSnapshot("worker")->client_references == 0 &&
      !messages.contains("ui:first"), "watching a stopped service must await change without starting it");
  wire.Receive("ui", R"({"requestId":"bad","op":"setAutostart","serviceId":"worker","enabled":"false"})");
  Require(messages.at("ui:bad").find("invalid-request") != std::string::npos && saves.empty(),
      "a string boolean must not change policy or touch storage");
  wire.Receive("ui", R"({"requestId":"on","op":"setAutostart","serviceId":"worker","enabled":true})");
  pump();
  Require(saves.size() == 1 && !messages.contains("ui:on") && starts == 0,
      "autostart must wait for durable storage before replying or starting");
  Require(!wire.Receive("ui", R"({"requestId":"on","op":"getState","serviceId":"worker"})"),
      "setting requests participate in duplicate correlation detection");
  auto saved = std::move(saves.front());
  saves.pop_front();
  saved("");
  pump();
  Require(messages.contains("ui:on") && messages.contains("ui:first") && starts == 1,
      "successful persistence must notify observers and create one resident reference");
  wire.Receive("other", R"({"requestId":"borrow","op":"acquire","serviceId":"worker"})");
  pump();
  wire.Receive("ui", R"({"requestId":"off","op":"setAutostart","serviceId":"worker","enabled":false})");
  pump();
  saved = std::move(saves.front());
  saves.pop_front();
  saved("disk full");
  pump();
  Require(messages.at("ui:off").find("disk full") != std::string::npos &&
      controller.GetSnapshot("worker")->resident, "failed persistence must retain the previous policy");
  wire.Receive("ui", R"({"requestId":"off2","op":"setAutostart","serviceId":"worker","enabled":false})");
  pump();
  saved = std::move(saves.front());
  saves.pop_front();
  saved("");
  pump();
  Require(messages.contains("ui:off2") && !controller.GetSnapshot("worker")->resident &&
      controller.GetSnapshot("worker")->client_references == 1 && !stop,
      "turning off must preserve another page's active use");
  wire.CloseOwner("other");
  pump();
  Require(stop && controller.GetSnapshot("worker")->state == MuonServiceState::Stopping,
      "watching must not retain the runtime after the final reference disappears");
  wire.Receive("ui", "{\"requestId\":\"latest" + watching + "\",\"operation\":\"next\"}");
  Require(messages.at("ui:latest").find("stopping") != std::string::npos,
      "slow observers must receive the latest coalesced state");
  wire.Receive("ui", "{\"requestId\":\"wait" + watching + "\",\"operation\":\"next\"}");
  wire.Receive("ui", "{\"requestId\":\"close" + watching + "\",\"operation\":\"release\"}");
  Require(messages.at("ui:wait").find("\"state\":null") != std::string::npos &&
      messages.contains("ui:close"), "closing a watch must settle its pending next request");
  wire.Receive("ui", "{\"requestId\":\"close2" + watching + "\",\"operation\":\"release\"}");
  stop();
  pump();
  Require(controller.IsQuiescent(), "observer cleanup must leave the stopped controller quiescent");
  wire.Receive("ui", R"({"requestId":"detachedOn","op":"setAutostart","serviceId":"worker","enabled":true})");
  pump();
  wire.CloseOwner("ui");
  saved = std::move(saves.front());
  saves.pop_front();
  saved("");
  pump();
  Require(controller.GetSnapshot("worker")->resident && starts == 2 && !messages.contains("ui:detachedOn"),
      "a page disappearing during persistence must not cancel its committed preference or receive a reply");
  controller.Shutdown("test complete");
  pump();
  stop();
  pump();
  Require(controller.IsQuiescent(), "policy test must drain its resident generation");
}

static void Communication() {
  auto queue = std::deque<std::function<void()>>{};
  const auto pump = [&] {
    while (!queue.empty()) {
      auto task = std::move(queue.front());
      queue.pop_front();
      task();
    }
  };
  auto* protocol = static_cast<MuonServiceProtocol*>(nullptr);
  auto controller = MuonServiceController({{"worker", true, std::nullopt}}, {
    [](const MuonServiceSnapshot&, auto complete) { complete(""); },
    [](const MuonServiceSnapshot&, auto complete) { complete(); },
    [](const std::string&, bool, auto complete) { complete(""); },
    [&](const MuonServiceEvent& event) { protocol->Notify(event); },
    [&](std::function<void()> task) { queue.push_back(std::move(task)); }});
  auto messages = std::map<std::string, std::string>{};
  auto waiting = std::function<void(const std::string&)>{};
  auto late = std::function<void(const std::string&)>{};
  auto released = 0;
  auto disconnected = 0;
  auto stopped = 0;
  auto dispatched = 0;
  auto wire = MuonServiceProtocol(controller, [&](const std::string& owner, const std::string& json) {
    const auto doc = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>(
        yyjson_read(json.data(), json.size(), 0), &yyjson_doc_free);
    auto* request = yyjson_obj_get(yyjson_doc_get_root(doc.get()), "requestId");
    if (request) messages.emplace(owner + ":" + yyjson_get_str(request), json);
  }, {}, MuonServiceCommunication{
    [&](const MuonServiceReference& reference, const std::string& action,
        const std::string& payload, std::function<void(const std::string&)> reply) {
      Require(reference.owner_id == "page" && controller.IsReady(reference), "RPC must receive its native authenticated owner");
      ++dispatched;
      if (action == "describe") reply(payload);
      else waiting = std::move(reply);
    },
    [&](const MuonServiceReference& reference) {
      ++released;
      controller.Release(reference);
      if (waiting) {
        late = std::move(waiting);
        waiting = nullptr;
        late("{\"status\":\"error\",\"code\":\"cancelled\",\"message\":\"released\"}");
      }
    },
    [&](const std::string& owner) { ++disconnected; controller.ReleaseOwner(owner); },
    [&] { ++stopped; }});
  protocol = &wire;
  wire.OpenOwner("page");
  wire.OpenOwner("intruder");
  controller.Activate();
  pump();
  const auto reference = controller.Acquire("worker", "page").value();
  pump();
  const auto fields = "\"serviceId\":\"worker\",\"refId\":\"" + std::to_string(reference.id) +
      "\",\"generation\":\"" + std::to_string(reference.generation) + "\"";
  const auto held = "{\"requestId\":\"held\",\"op\":\"rpc\",\"owner\":\"forged\"," + fields +
      ",\"action\":\"result\",\"payload\":{\"callId\":\"1\"}}";
  Require(wire.Receive("intruder", held) && dispatched == 0, "RPC must reject another owner's reference before dispatch");
  Require(wire.Receive("page", held) && waiting && !messages.contains("page:held"), "RPC replies may remain pending");
  Require(!wire.Receive("page", "{\"requestId\":\"held\",\"op\":\"getState\",\"serviceId\":\"worker\"}"),
      "RPC correlation participates in all duplicate request checks");
  auto large = std::string{};
  for (size_t index = 0; index < 1024 * 1024; ++index) large += "\\u0000";
  Require(wire.Receive("page", "{\"requestId\":\"large\",\"op\":\"rpc\"," + fields +
      ",\"action\":\"describe\",\"payload\":{\"value\":\"" + large + "\"}}"), "Encoded 1 MiB values must fit the frame budget");
  Require(messages.at("page:large").find(large) != std::string::npos, "Large RPC replies must be preserved");
  wire.Receive("page", "{\"requestId\":\"release\",\"op\":\"release\"," + fields + "}");
  Require(released == 1 && messages.at("page:held").find("cancelled") != std::string::npos,
      "Release must settle calls owned by exactly that reference");
  late("null");
  const auto remaining = controller.Acquire("worker", "page").value();
  pump();
  Require(controller.IsReady(remaining), "Another reference remains usable");
  wire.CloseOwner("page");
  late("null");
  Require(disconnected == 1 && !controller.IsReady(remaining), "Disconnect must reclaim RPC and lifetime owners together");
  wire.Receive("intruder", "{\"requestId\":\"exit\",\"op\":\"shutdown\"}");
  pump();
  Require(stopped == 1 && controller.IsQuiescent(), "Shutdown must close communication admission");
}

int main() {
  Communication();
  PolicyAndObservation();
  auto queue = std::deque<std::function<void()>>{};
  const auto pump = [&] {
    while (!queue.empty()) {
      auto task = std::move(queue.front());
      queue.pop_front();
      task();
    }
  };
  auto starts = 0;
  auto stops = 0;
  auto start = std::function<void(const std::string&)>{};
  auto messages = std::vector<std::pair<std::string, std::string>>{};
  auto* protocol = static_cast<MuonServiceProtocol*>(nullptr);
  auto controller = MuonServiceController({{"worker", false, std::nullopt}}, {
      [&](const MuonServiceSnapshot&, auto complete) { ++starts; start = std::move(complete); },
      [&](const MuonServiceSnapshot&, auto complete) { ++stops; complete(); },
      [](const std::string&, bool, auto complete) { complete("not available"); },
      [&](const MuonServiceEvent& event) { protocol->Notify(event); },
      [&](std::function<void()> task) { queue.push_back(std::move(task)); }});
  auto wire = MuonServiceProtocol(controller, [&](const auto& owner, const auto& json) {
    messages.emplace_back(owner, json);
  });
  protocol = &wire;
  Require(wire.OpenOwner("one") && wire.OpenOwner("two"), "connections must register distinct owners");
  Require(wire.Receive("one", R"({"requestId":"a","op":"acquire","serviceId":"worker"})") &&
              wire.Receive("two", R"({"requestId":"a","op":"acquire","serviceId":"worker"})"),
          "different connections may use the same request correlation identity");
  pump();
  Require(starts == 1 && messages.empty(), "acquisition must await startup");
  wire.CloseOwner("one");
  start("");
  pump();
  Require(messages.size() == 1 && messages[0].first == "two", "a closed page must not receive its late acquisition");
  const auto doc = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>(
      yyjson_read(messages[0].second.data(), messages[0].second.size(), 0), &yyjson_doc_free);
  const auto result = yyjson_obj_get(yyjson_doc_get_root(doc.get()), "result");
  const auto ref = std::string(yyjson_get_str(yyjson_obj_get(result, "refId")));
  const auto generation = std::string(yyjson_get_str(yyjson_obj_get(result, "generation")));
  const auto release = std::string(R"({"requestId":"r","op":"release","serviceId":"worker","refId":")") +
      ref + R"(","generation":")" + generation + "\"}";
  wire.OpenOwner("intruder");
  Require(wire.Receive("intruder", release), "a forged release must be harmless");
  pump();
  Require(stops == 0 && controller.GetSnapshot("worker")->client_references == 1,
          "JSON must never choose another connection's owner identity");
  wire.Receive("two", R"({"requestId":"s","op":"getState","serviceId":"worker"})");
  Require(controller.GetSnapshot("worker")->client_references == 1,
          "observation must not reserve another reference");
  wire.Receive("two", release);
  wire.Receive("two", release);
  pump();
  Require(stops == 1 && controller.IsQuiescent(), "duplicate release must stop one generation exactly once");
  Require(!wire.Receive("one", "{}") && !wire.Receive("two", "not json"),
          "closed connections and malformed messages must be rejected");
  Require(wire.Receive("two", R"({"requestId":"x","op":"acquire","serviceId":"worker"})"), "new generation must be reservable");
  Require(!wire.Receive("two", R"({"requestId":"x","op":"acquire","serviceId":"worker"})"),
          "duplicate pending request identity must not create another reservation");
  wire.CloseOwner("two");
  controller.Shutdown("test complete");
  pump();
  Require(controller.IsQuiescent(), "cancelled undispatched startup must settle without leaking references");
  std::cout << "service wire protocol: passed\n";
  return 0;
}
