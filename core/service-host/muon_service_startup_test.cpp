/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "muon_service_startup.h"
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <unistd.h>

static cardio::promise<void> Test(const std::filesystem::path& root, bool& passed) {
  const auto metadata = std::string("{\"executable\":\"") + (root / "app").string() +
      "\",\"source\":\"normal\",\"id\":\"service-startup-consumer\",\"refresh\":true,"
      "\"arguments\":[\"--muon-autostart\",\"--muon-launch-from=normal\"]}";
  const std::vector<MuonServiceDefinition> definitions{{"worker", true, std::nullopt}};
  auto policy = std::move(co_await CreateMuonServiceLoginPolicy(root / "data", definitions, metadata));
  const auto entry = root / "xdg/autostart/service-startup-consumer.desktop";
  assert(std::filesystem::exists(entry));
  assert(!policy->UiEnabled());
  co_await policy->Set("worker", false);
  assert(!std::filesystem::exists(entry));
  co_await ConfigureMuonServiceLoginPolicy(*policy, metadata);
  co_await policy->Set("", true);
  assert(std::filesystem::exists(entry));
  co_await policy->Set("", false);
  assert(!std::filesystem::exists(entry));
  passed = true;
  std::cout << "service startup registration: passed\n";
}

static cardio::promise<void> Run(const std::filesystem::path& root, bool& passed,
    cardio::dispatcher_group_glib& group) {
  try { co_await Test(root, passed); }
  catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
  group.shutdown();
}

int main() {
  auto pattern = std::string("/tmp/muon-startup-test-XXXXXX");
  if (!mkdtemp(pattern.data())) return 1;
  const auto root = std::filesystem::path(pattern);
  std::filesystem::create_directories(root / "data/.autostart");
  setenv("XDG_CONFIG_HOME", (root / "xdg").c_str(), 1);
  setenv("XDG_CONFIG_DIRS", (root / "system-xdg").c_str(), 1);
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib dispatcher(group);
  bool passed = false;
  auto test = Run(root, passed, group);
  dispatcher.park();
  std::filesystem::remove_all(root);
  return passed ? 0 : 1;
}
