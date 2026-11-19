/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 */
#include "muon_service_io_desktop.h"
#include "muon_service_http_desktop.h"
#include "services/muon_service_server.h"
#include "muon_service_policy.h"
#include <cstdlib>

static cardio::promise<void> FileWork(std::function<void()> work, std::function<void(std::string)> completed) {
  std::string error;
  try { co_await RunMuonServiceFileWork(std::move(work)); }
  catch (const std::exception& failure) { error = failure.what(); }
  completed(std::move(error));
}

MuonServiceIoSettings CreateDesktopMuonServiceIo(yyjson_val* policy) {
  MuonServiceIoSettings settings;
  if (policy) {
    if (!yyjson_is_obj(policy)) throw std::invalid_argument("Invalid service I/O permissions");
    const auto encoded = std::unique_ptr<char, decltype(&std::free)>(yyjson_val_write(policy, 0, nullptr), &std::free);
    if (!encoded) throw std::bad_alloc();
    settings.policy = encoded.get();
  }
  settings.http = RunDesktopMuonServiceHttp;
  settings.server = CreateMuonServiceListeners;
  settings.submit = [](std::function<void()> work, std::function<void(std::string)> completed) {
    cardio::fire_and_forget(FileWork(std::move(work), std::move(completed)));
  };
  return settings;
}
