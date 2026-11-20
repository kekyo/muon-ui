/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "services/muon_service_execution_router.h"
#include <memory>
#include <stdexcept>

struct MuonServiceExecutionRoutes {
  std::vector<MuonServiceExecution> runtimes;
  std::function<size_t(const std::string&)> select;

  const MuonServiceExecution& For(const std::string& id) const {
    return runtimes.at(select(id));
  }
};

MuonServiceExecution RouteMuonServiceExecutions(std::vector<MuonServiceExecution> runtimes,
    std::function<size_t(const std::string&)> select) {
  if (!select || runtimes.empty()) throw std::invalid_argument("Missing service runtime routes");
  for (const auto& runtime : runtimes)
    if (!runtime.describe || !runtime.interface_json || !runtime.invoke || !runtime.cancel || !runtime.set_event_sink)
      throw std::invalid_argument("Incomplete service runtime operations");
  const auto routes = std::make_shared<const MuonServiceExecutionRoutes>(
      MuonServiceExecutionRoutes{std::move(runtimes), std::move(select)});
  return {
    [routes](const std::string& id, uint64_t generation) -> const MuonServiceInterface& {
      return routes->For(id).describe(id, generation);
    },
    [routes](const std::string& id, uint64_t generation) {
      return routes->For(id).interface_json(id, generation);
    },
    [routes](const std::string& id, uint64_t generation, const std::string& method,
        std::vector<MuonServiceValue> arguments, MuonServiceCallbackHandler callback,
        MuonServiceCallCompletion complete) {
      return routes->For(id).invoke(id, generation, method, std::move(arguments),
          std::move(callback), std::move(complete));
    },
    [routes](const std::string& id, uint64_t generation, uint64_t call) {
      routes->For(id).cancel(id, generation, call);
    },
    [routes](MuonServiceEventSink sink) {
      for (const auto& runtime : routes->runtimes) runtime.set_event_sink(sink);
    }
  };
}
