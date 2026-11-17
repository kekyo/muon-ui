#include "services/muon_service_execution_router.h"
#include <cassert>
#include <iostream>
#include <stdexcept>

int main() {
  MuonServiceInterface native_interface;
  MuonServiceInterface script_interface;
  std::vector<MuonServiceEventSink> sinks(2);
  std::vector<std::string> invoked;
  std::vector<std::string> cancelled;
  auto execution = [&](size_t runtime, MuonServiceInterface& metadata) {
    return MuonServiceExecution{
      [&metadata](const std::string&, uint64_t generation) -> const MuonServiceInterface& {
        if (generation != 7) throw std::invalid_argument("stale generation");
        return metadata;
      },
      [runtime](const std::string&, uint64_t) { return std::to_string(runtime); },
      [&, runtime](const std::string& id, uint64_t generation, const std::string& method,
          std::vector<MuonServiceValue> arguments, MuonServiceCallbackHandler callback,
          MuonServiceCallCompletion complete) {
        assert(generation == 7 && method == "echo");
        invoked.push_back(id + std::to_string(runtime));
        callback(11, {}, {}, [complete, arguments](MuonServiceResult) {
          complete({arguments.at(0), {}, {}});
        });
        return uint64_t{1}; // IDs may overlap in different runtimes.
      },
      [&, runtime](const std::string& id, uint64_t generation, uint64_t call) {
        assert(generation == 7 && call == 1);
        cancelled.push_back(id + std::to_string(runtime));
      },
      [&, runtime](MuonServiceEventSink sink) { sinks.at(runtime) = std::move(sink); }
    };
  };
  auto route = RouteMuonServiceExecutions(
      {execution(0, native_interface), execution(1, script_interface)},
      [](const std::string& id) -> size_t {
        if (id == "native") return 0;
        if (id == "script") return 1;
        return 2;
      });
  assert(&route.describe("native", 7) == &native_interface);
  assert(&route.describe("script", 7) == &script_interface);
  assert(route.interface_json("script", 7) == "1");
  for (const auto id : {"native", "script"}) {
    bool completed = false;
    const auto call = route.invoke(id, 7, "echo", {{std::string("copied")}},
        [](uint64_t token, const MuonServiceSignature&, std::vector<MuonServiceValue>,
            MuonServiceCallCompletion done) { assert(token == 11); done({{}, "unused", {}}); },
        [&](MuonServiceResult result) {
          assert(std::get<std::string>(result.value->data) == "copied"); completed = true;
        });
    assert(completed && call == 1);
    route.cancel(id, 7, call);
  }
  assert((invoked == std::vector<std::string>{"native0", "script1"}));
  assert(cancelled == invoked);
  std::vector<std::string> events;
  route.set_event_sink([&](const std::string& id, uint64_t generation, const std::string& event,
      const MuonServiceValue&) { assert(generation == 7 && event == "progress"); events.push_back(id); });
  sinks[0]("native", 7, "progress", {});
  sinks[1]("script", 7, "progress", {});
  assert((events == std::vector<std::string>{"native", "script"}));
  route.set_event_sink({});
  assert(!sinks[0] && !sinks[1]);
  bool rejected = false;
  try { route.describe("unknown", 7); } catch (const std::out_of_range&) { rejected = true; }
  assert(rejected);
  rejected = false;
  try { route.describe("script", 6); } catch (const std::invalid_argument&) { rejected = true; }
  assert(rejected);
  std::cout << "Service execution routing preserves runtime, generation, callbacks, and events\n";
}
