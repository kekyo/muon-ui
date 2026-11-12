/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "muon_service_login.h"
#include <cassert>
#include <iostream>

static cardio::promise<void> Test(bool& passed) {
  std::map<std::string, bool> disk{{"", false}, {"worker", true}, {"second", false}};
  bool registered = false;
  bool fail_save = false;
  bool fail_registration = false;
  bool hold_save = false;
  cardio::promise_source<void> finish_save;
  cardio::primitives::manually_conditional saving{false};
  std::vector<std::string> saved;
  std::vector<std::string> errors;
  MuonServiceLoginPolicy policy(disk,
      [&](std::string id, bool value) -> cardio::promise<void> {
        if (fail_save) throw std::runtime_error("disk failed");
        if (hold_save) { saving.raise(); co_await finish_save.get_promise(); }
        disk.at(id) = value;
        saved.push_back(id);
      },
      [&](bool enabled) -> cardio::promise<void> {
        if (fail_registration) throw std::runtime_error("registration failed");
        registered = enabled;
        co_return;
      }, [&](const std::string& error) { errors.push_back(error); });
  co_await policy.Reconcile();
  assert(registered && !policy.UiEnabled());
  co_await policy.Set("", false);
  assert(registered && !policy.UiEnabled());
  co_await policy.Set("worker", false);
  assert(!registered);

  hold_save = true;
  auto ui = policy.Set("", true);
  co_await saving.wait();
  auto worker = policy.Set("second", true);
  assert(!policy.UiEnabled() && !disk.at("second"));
  hold_save = false;
  finish_save.resolve();
  co_await ui;
  co_await worker;
  assert(policy.UiEnabled() && registered && disk.at("second"));
  assert(saved[saved.size() - 2].empty() && saved.back() == "second");
  co_await policy.Set("", false);
  assert(!policy.UiEnabled() && registered);

  fail_save = true;
  bool rejected = false;
  try { co_await policy.Set("second", false); }
  catch (const std::runtime_error&) { rejected = true; }
  assert(rejected && disk.at("second") && registered);
  fail_save = false;
  // Failure to remove a redundant OS entry cannot undo a committed preference.
  // The login command still evaluates the saved OFF values without starting UI.
  fail_registration = true;
  co_await policy.Set("second", false);
  assert(!disk.at("second") && !policy.UiEnabled() && !errors.empty());
  fail_registration = false;
  co_await policy.Reconcile();
  assert(!registered);
  int relocated = 0;
  co_await policy.Configure([&](bool enabled) -> cardio::promise<void> {
    ++relocated;
    registered = enabled;
    co_return;
  }, true);
  assert(relocated == 1 && !registered);
  co_await policy.Set("", true);
  assert(relocated == 2 && registered);
  co_await policy.Set("", false);
  co_await policy.Configure([&](bool enabled) -> cardio::promise<void> {
    if (fail_registration) throw std::runtime_error("registration failed");
    registered = enabled;
    co_return;
  }, false);
  fail_registration = true;
  const auto saves_before = saved.size();
  rejected = false;
  try { co_await policy.Set("", true); }
  catch (const std::runtime_error&) { rejected = true; }
  assert(rejected && !policy.UiEnabled() && !disk.at("") && saved.size() == saves_before);
  fail_registration = false;
  fail_save = true;
  rejected = false;
  try { co_await policy.Set("", true); }
  catch (const std::runtime_error&) { rejected = true; }
  assert(rejected && !registered && !policy.UiEnabled());
  std::cout << "service login policy: passed\n";
  passed = true;
}

int main() {
  cardio::dispatcher_host dispatcher;
  bool passed = false;
  auto test = Test(passed);
  dispatcher.park();
  return passed ? 0 : 1;
}
