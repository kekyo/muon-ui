/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "services/muon_native_service_host.h"
#include "services/muon_native_service_library.h"

#include <deque>
#include <iostream>
#include <stdexcept>

int main() {
  auto error = std::string{};
  if (LoadMuonNativeServiceFactory("/nonexistent/service-library", error) ||
      error.empty()) return 1;
  if (LoadMuonNativeServiceFactory("relative-library.so", error) ||
      error.empty()) return 1;
  auto queue = std::deque<std::function<void()>>{};
  auto logs = std::vector<std::string>{};
  auto host = MuonNativeServiceHost({
      &LoadMuonNativeServiceFactory,
      [&](std::function<void()> task) { queue.push_back(std::move(task)); },
      [&](const std::string& id, muon_log_level, const std::string& message) {
        logs.push_back(id + ":" + message);
      }});
  auto started = 0;
  auto stopped = 0;
  for (const auto* id : {"alpha", "beta"}) {
    host.Start({id, MUON_SERVICE_FIXTURE_PATH, "/private", {}}, 1,
               [&, id](const std::string& failure) {
      if (!failure.empty()) throw std::runtime_error(failure);
      ++started;
      host.Stop(id, 1, [&] { ++stopped; });
    });
  }
  while (!queue.empty()) {
    auto task = std::move(queue.front());
    queue.pop_front();
    task();
  }
  if (!host.IsIdle() || started != 2 || stopped != 2 ||
      logs != std::vector<std::string>{"alpha:alpha", "beta:beta",
                                      "alpha:stopped", "beta:stopped"}) return 1;
  std::cout << "native service shared library: passed\n";
  return 0;
}
