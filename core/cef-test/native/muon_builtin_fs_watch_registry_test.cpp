/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "plugins/builtin/muon_builtin_fs_watch_registry.h"

#include <iostream>
#include <string>

static constexpr int kContextAlpha = 100;
static constexpr int kContextBeta = 200;

static_assert(muon_internal::kMuonBuiltinFsWatchPerContextLimit == 16);
static_assert(muon_internal::kMuonBuiltinFsWatchGlobalLimit == 128);

static bool Expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << "\n";
    return false;
  }
  return true;
}

static bool RunPerContextQuotaTest() {
  auto registry = muon_internal::MuonBuiltinFsWatchRegistry({2, 4});
  auto error = std::string{};
  const auto first = registry.TryAcquire(kContextAlpha, &error);
  const auto second = registry.TryAcquire(kContextAlpha, &error);
  const auto rejected = registry.TryAcquire(kContextAlpha, &error);
  const auto counts = registry.GetCounts(kContextAlpha);

  return Expect(first.has_value(), "first watcher lease was rejected") &&
         Expect(second.has_value(), "second watcher lease was rejected") &&
         Expect(!rejected.has_value(),
                "third watcher exceeded the context quota") &&
         Expect(error == "Filesystem watcher limit exceeded",
                "context quota error message changed") &&
         Expect(counts.owner_count == 2 && counts.global_count == 2,
                "context quota rejection changed active counts");
}

static bool RunGlobalQuotaTest() {
  auto registry = muon_internal::MuonBuiltinFsWatchRegistry({4, 2});
  auto error = std::string{};
  const auto alpha = registry.TryAcquire(kContextAlpha, &error);
  const auto beta = registry.TryAcquire(kContextBeta, &error);
  const auto rejected = registry.TryAcquire(kContextBeta, &error);
  const auto alpha_counts = registry.GetCounts(kContextAlpha);
  const auto beta_counts = registry.GetCounts(kContextBeta);

  return Expect(alpha.has_value(), "global quota rejected alpha") &&
         Expect(beta.has_value(), "global quota rejected beta") &&
         Expect(!rejected.has_value(),
                "separate contexts exceeded the global quota") &&
         Expect(error == "Global filesystem watcher limit exceeded",
                "global quota error message changed") &&
         Expect(alpha_counts.owner_count == 1 &&
                    alpha_counts.global_count == 2,
                "global quota changed alpha counts") &&
         Expect(beta_counts.owner_count == 1 && beta_counts.global_count == 2,
                "global quota changed beta counts");
}

static bool RunReleaseAndReacquireTest() {
  auto registry = muon_internal::MuonBuiltinFsWatchRegistry({2, 4});
  auto error = std::string{};
  const auto first = registry.TryAcquire(kContextAlpha, &error);
  const auto second = registry.TryAcquire(kContextAlpha, &error);
  if (!first.has_value() || !second.has_value()) {
    return Expect(false, "release test setup could not acquire leases");
  }

  const auto released = registry.Release(kContextAlpha, first->token);
  const auto duplicate = registry.Release(kContextAlpha, first->token);
  const auto replacement = registry.TryAcquire(kContextAlpha, &error);
  const auto counts = registry.GetCounts(kContextAlpha);

  return Expect(released, "active watcher lease was not released") &&
         Expect(!duplicate, "duplicate watcher release was accepted") &&
         Expect(replacement.has_value(),
                "released watcher quota could not be reacquired") &&
         Expect(replacement->token != first->token,
                "reacquired watcher lease reused a token") &&
         Expect(counts.owner_count == 2 && counts.global_count == 2,
                "release and reacquire changed active counts");
}

static bool RunContextReleaseTest() {
  auto registry = muon_internal::MuonBuiltinFsWatchRegistry({4, 4});
  auto error = std::string{};
  const auto first = registry.TryAcquire(kContextAlpha, &error);
  const auto second = registry.TryAcquire(kContextAlpha, &error);
  const auto beta = registry.TryAcquire(kContextBeta, &error);
  if (!first.has_value() || !second.has_value() || !beta.has_value()) {
    return Expect(false, "context release setup could not acquire leases");
  }

  registry.ReleaseContext(kContextAlpha);
  const auto alpha_counts = registry.GetCounts(kContextAlpha);
  const auto beta_counts = registry.GetCounts(kContextBeta);

  return Expect(!registry.IsActive(kContextAlpha, first->token),
                "context release left the first lease active") &&
         Expect(!registry.IsActive(kContextAlpha, second->token),
                "context release left the second lease active") &&
         Expect(registry.IsActive(kContextBeta, beta->token),
                "context release removed another context lease") &&
         Expect(alpha_counts.owner_count == 0 &&
                    alpha_counts.global_count == 1,
                "context release changed alpha counts incorrectly") &&
         Expect(beta_counts.owner_count == 1 && beta_counts.global_count == 1,
                "context release changed beta counts incorrectly");
}

int main() {
  return RunPerContextQuotaTest() && RunGlobalQuotaTest() &&
                 RunReleaseAndReacquireTest() && RunContextReleaseTest()
             ? 0
             : 1;
}
