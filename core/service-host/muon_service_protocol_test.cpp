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

int main() {
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
