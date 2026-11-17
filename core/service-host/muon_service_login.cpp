/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "muon_service_login.h"
#include <algorithm>
#include <stdexcept>

static bool AnyEnabled(const std::map<std::string, bool>& values) {
  return std::any_of(values.begin(), values.end(), [](const auto& item) { return item.second; });
}

MuonServiceLoginPolicy::MuonServiceLoginPolicy(std::map<std::string, bool> preferences,
    std::function<cardio::promise<void>(std::string, bool)> save,
    std::function<cardio::promise<void>(bool)> registration,
    std::function<void(const std::string&)> diagnostic)
    : preferences_(std::move(preferences)), save_(std::move(save)),
      registration_(std::move(registration)), diagnostic_(std::move(diagnostic)) {
  if (!preferences_.contains("") || !save_ || !registration_ || !diagnostic_)
    throw std::invalid_argument("Login policy requires UI preference and platform operations");
}

bool MuonServiceLoginPolicy::UiEnabled() const { return preferences_.at(""); }

cardio::promise<void> MuonServiceLoginPolicy::Reconcile() {
  auto lock = std::move(co_await mutex_.lock());
  co_await registration_(AnyEnabled(preferences_));
}

cardio::promise<void> MuonServiceLoginPolicy::Configure(
    std::function<cardio::promise<void>(bool)> registration, bool refresh) {
  auto lock = std::move(co_await mutex_.lock());
  if (!registration) throw std::invalid_argument("Login registration operation is required");
  if (refresh) co_await registration(AnyEnabled(preferences_));
  registration_ = std::move(registration);
}

cardio::promise<void> MuonServiceLoginPolicy::Set(std::string id, bool enabled) {
  auto lock = std::move(co_await mutex_.lock());
  if (!preferences_.contains(id)) throw std::invalid_argument("Unknown login policy identity");
  const auto previously_enabled = AnyEnabled(preferences_);
  const auto newly_registered = enabled && !previously_enabled;
  if (newly_registered) co_await registration_(true);
  std::exception_ptr failure;
  try { co_await save_(id, enabled); }
  catch (...) { failure = std::current_exception(); }
  if (!failure) preferences_.at(id) = enabled;
  // The file is authoritative. An OS entry can outlive a failed removal, but its
  // command always reads these files before choosing whether to start UI/work.
  if ((!failure && !AnyEnabled(preferences_)) || (failure && newly_registered)) {
    try { co_await registration_(false); }
    catch (const std::exception& error) { diagnostic_(error.what()); }
  }
  if (failure) std::rethrow_exception(failure);
}
