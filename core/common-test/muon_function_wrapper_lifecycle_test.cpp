/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "plugins/muon_function_wrapper_lifecycle.h"

#include <cstddef>
#include <iostream>
#include <string>

static constexpr size_t kTestOwnerLimit = 2;
static constexpr size_t kTestGlobalLimit = 2;
static constexpr char kOwnerAlpha[] = "browser:1/frame:10/context:100";
static constexpr char kOwnerBeta[] = "browser:1/frame:20/context:200";

static_assert(kMuonFunctionWrapperOwnerLimit == 256);
static_assert(kMuonFunctionWrapperGlobalLimit == 4096);
static_assert(kMuonRendererFunctionReferenceLimit == 256);

struct FakeBackend {
  size_t acquire_count = 0;
  bool acquire_result = true;

  bool Acquire() {
    ++acquire_count;
    return acquire_result;
  }
};

static bool Expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << "\n";
    return false;
  }
  return true;
}

static bool ExpectCounts(
    const MuonFunctionWrapperLifecycle& lifecycle,
    const std::string& owner_id,
    size_t expected_sources,
    size_t expected_proxy_leases,
    size_t expected_total,
    const std::string& label) {
  const auto owner_counts = lifecycle.GetOwnerCounts(owner_id);
  const auto global_counts = lifecycle.GetGlobalCounts();
  return Expect(owner_counts.renderer_source_count == expected_sources,
                label + ": wrong owner source count") &&
         Expect(owner_counts.plugin_proxy_lease_count ==
                    expected_proxy_leases,
                label + ": wrong owner proxy lease count") &&
         Expect(owner_counts.total_count == expected_total,
                label + ": wrong owner total count") &&
         Expect(global_counts.renderer_source_count == expected_sources,
                label + ": wrong global source count") &&
         Expect(global_counts.plugin_proxy_lease_count ==
                    expected_proxy_leases,
                label + ": wrong global proxy lease count") &&
         Expect(global_counts.total_count == expected_total,
                label + ": wrong global total count");
}

static bool RunRendererSourceQuotaTest() {
  auto lifecycle = MuonFunctionWrapperLifecycle(
      {kTestOwnerLimit, kTestGlobalLimit});
  auto backend = FakeBackend{};
  const auto first = lifecycle.TryAcquire(
      kOwnerAlpha,
      MuonFunctionWrapperKind::kRendererSource,
      [&backend]() { return backend.Acquire(); });
  const auto second = lifecycle.TryAcquire(
      kOwnerAlpha,
      MuonFunctionWrapperKind::kRendererSource,
      [&backend]() { return backend.Acquire(); });
  const auto rejected = lifecycle.TryAcquire(
      kOwnerAlpha,
      MuonFunctionWrapperKind::kRendererSource,
      [&backend]() { return backend.Acquire(); });

  return Expect(first.has_value(), "first renderer source was rejected") &&
         Expect(second.has_value(), "second renderer source was rejected") &&
         Expect(first->owner_id == kOwnerAlpha &&
                    first->kind ==
                        MuonFunctionWrapperKind::kRendererSource,
                "renderer source lease lost its owner or kind") &&
         Expect(!rejected.has_value(),
                "third renderer source exceeded the owner quota") &&
         Expect(backend.acquire_count == 2,
                "renderer source quota rejection called the backend") &&
         ExpectCounts(lifecycle, kOwnerAlpha, 2, 0, 2,
                      "renderer source quota rejection");
}

static bool RunPluginProxyQuotaTest() {
  auto lifecycle = MuonFunctionWrapperLifecycle(
      {kTestOwnerLimit, kTestGlobalLimit});
  auto backend = FakeBackend{};
  const auto first = lifecycle.TryAcquire(
      kOwnerAlpha,
      MuonFunctionWrapperKind::kPluginProxyLease,
      [&backend]() { return backend.Acquire(); });
  const auto second = lifecycle.TryAcquire(
      kOwnerAlpha,
      MuonFunctionWrapperKind::kPluginProxyLease,
      [&backend]() { return backend.Acquire(); });
  const auto rejected = lifecycle.TryAcquire(
      kOwnerAlpha,
      MuonFunctionWrapperKind::kPluginProxyLease,
      [&backend]() { return backend.Acquire(); });

  return Expect(first.has_value(), "first plugin proxy was rejected") &&
         Expect(second.has_value(), "second plugin proxy was rejected") &&
         Expect(!rejected.has_value(),
                "third plugin proxy exceeded the owner quota") &&
         Expect(backend.acquire_count == 2,
                "plugin proxy quota rejection called the backend") &&
         ExpectCounts(lifecycle, kOwnerAlpha, 0, 2, 2,
                      "plugin proxy quota rejection");
}

static bool RunCombinedOwnerQuotaTest() {
  auto lifecycle = MuonFunctionWrapperLifecycle(
      {kTestOwnerLimit, kTestGlobalLimit});
  auto backend = FakeBackend{};
  const auto source = lifecycle.TryAcquire(
      kOwnerAlpha,
      MuonFunctionWrapperKind::kRendererSource,
      [&backend]() { return backend.Acquire(); });
  const auto proxy = lifecycle.TryAcquire(
      kOwnerAlpha,
      MuonFunctionWrapperKind::kPluginProxyLease,
      [&backend]() { return backend.Acquire(); });
  const auto rejected = lifecycle.TryAcquire(
      kOwnerAlpha,
      MuonFunctionWrapperKind::kRendererSource,
      [&backend]() { return backend.Acquire(); });

  return Expect(source.has_value(), "combined quota rejected the source") &&
         Expect(proxy.has_value(), "combined quota rejected the proxy") &&
         Expect(!rejected.has_value(),
                "combined source and proxy exceeded the owner quota") &&
         Expect(backend.acquire_count == 2,
                "combined owner quota rejection called the backend") &&
         ExpectCounts(lifecycle, kOwnerAlpha, 1, 1, 2,
                      "combined owner quota rejection");
}

static bool RunGlobalQuotaAcrossOwnersTest() {
  auto lifecycle = MuonFunctionWrapperLifecycle(
      {kTestOwnerLimit, kTestGlobalLimit});
  auto backend = FakeBackend{};
  const auto alpha = lifecycle.TryAcquire(
      kOwnerAlpha,
      MuonFunctionWrapperKind::kRendererSource,
      [&backend]() { return backend.Acquire(); });
  const auto beta = lifecycle.TryAcquire(
      kOwnerBeta,
      MuonFunctionWrapperKind::kPluginProxyLease,
      [&backend]() { return backend.Acquire(); });
  const auto rejected = lifecycle.TryAcquire(
      kOwnerBeta,
      MuonFunctionWrapperKind::kRendererSource,
      [&backend]() { return backend.Acquire(); });
  const auto alpha_counts = lifecycle.GetOwnerCounts(kOwnerAlpha);
  const auto beta_counts = lifecycle.GetOwnerCounts(kOwnerBeta);
  const auto global_counts = lifecycle.GetGlobalCounts();

  return Expect(alpha.has_value(), "global quota rejected owner alpha") &&
         Expect(beta.has_value(), "global quota rejected owner beta") &&
         Expect(!rejected.has_value(),
                "separate owners exceeded the global quota") &&
         Expect(backend.acquire_count == 2,
                "global quota rejection called the backend") &&
         Expect(alpha_counts.renderer_source_count == 1 &&
                    alpha_counts.total_count == 1,
                "owner alpha count changed after global rejection") &&
         Expect(beta_counts.plugin_proxy_lease_count == 1 &&
                    beta_counts.total_count == 1,
                "owner beta count changed after global rejection") &&
         Expect(global_counts.renderer_source_count == 1 &&
                    global_counts.plugin_proxy_lease_count == 1 &&
                    global_counts.total_count == 2,
                "global count changed after global rejection");
}

static bool RunBackendFailureRollbackTest() {
  auto lifecycle = MuonFunctionWrapperLifecycle(
      {kTestOwnerLimit, kTestGlobalLimit});
  auto backend = FakeBackend{0, false};
  const auto failed = lifecycle.TryAcquire(
      kOwnerAlpha,
      MuonFunctionWrapperKind::kRendererSource,
      [&backend]() { return backend.Acquire(); });
  const auto counts_after_failure = lifecycle.GetGlobalCounts();

  backend.acquire_result = true;
  const auto first = lifecycle.TryAcquire(
      kOwnerAlpha,
      MuonFunctionWrapperKind::kRendererSource,
      [&backend]() { return backend.Acquire(); });
  const auto second = lifecycle.TryAcquire(
      kOwnerAlpha,
      MuonFunctionWrapperKind::kPluginProxyLease,
      [&backend]() { return backend.Acquire(); });
  const auto rejected = lifecycle.TryAcquire(
      kOwnerAlpha,
      MuonFunctionWrapperKind::kPluginProxyLease,
      [&backend]() { return backend.Acquire(); });

  return Expect(!failed.has_value(),
                "backend failure returned an active lease") &&
         Expect(counts_after_failure.total_count == 0,
                "backend failure did not roll back the reservation") &&
         Expect(first.has_value() && second.has_value(),
                "backend failure consumed quota") &&
         Expect(!rejected.has_value(),
                "quota allowed a third lease after rollback") &&
         Expect(backend.acquire_count == 3,
                "backend failure or quota rejection called backend wrongly") &&
         ExpectCounts(lifecycle, kOwnerAlpha, 1, 1, 2,
                      "backend failure rollback");
}

static bool RunReleaseAndReacquireTest() {
  auto lifecycle = MuonFunctionWrapperLifecycle(
      {kTestOwnerLimit, kTestGlobalLimit});
  auto backend = FakeBackend{};
  const auto first = lifecycle.TryAcquire(
      kOwnerAlpha,
      MuonFunctionWrapperKind::kRendererSource,
      [&backend]() { return backend.Acquire(); });
  const auto second = lifecycle.TryAcquire(
      kOwnerAlpha,
      MuonFunctionWrapperKind::kPluginProxyLease,
      [&backend]() { return backend.Acquire(); });
  if (!first.has_value() || !second.has_value()) {
    return Expect(false, "release test setup could not acquire leases");
  }

  const auto released = lifecycle.Release(*first);
  const auto replacement = lifecycle.TryAcquire(
      kOwnerAlpha,
      MuonFunctionWrapperKind::kRendererSource,
      [&backend]() { return backend.Acquire(); });

  return Expect(released, "active lease release was rejected") &&
         Expect(replacement.has_value(),
                "released quota could not be reacquired") &&
         Expect(replacement->id != first->id,
                "reacquired lease reused an active identity") &&
         Expect(backend.acquire_count == 3,
                "reacquiring released quota did not call the backend once") &&
         ExpectCounts(lifecycle, kOwnerAlpha, 1, 1, 2,
                      "release and reacquire");
}

static bool RunDuplicateReleaseTest() {
  auto lifecycle = MuonFunctionWrapperLifecycle(
      {kTestOwnerLimit, kTestGlobalLimit});
  auto backend = FakeBackend{};
  const auto lease = lifecycle.TryAcquire(
      kOwnerAlpha,
      MuonFunctionWrapperKind::kRendererSource,
      [&backend]() { return backend.Acquire(); });
  if (!lease.has_value()) {
    return Expect(false, "duplicate release setup could not acquire a lease");
  }

  const auto first_release = lifecycle.Release(*lease);
  const auto second_release = lifecycle.Release(*lease);
  const auto counts_after_release = lifecycle.GetGlobalCounts();
  const auto replacement = lifecycle.TryAcquire(
      kOwnerAlpha,
      MuonFunctionWrapperKind::kPluginProxyLease,
      [&backend]() { return backend.Acquire(); });

  return Expect(first_release, "first release was rejected") &&
         Expect(!second_release, "duplicate release was accepted") &&
         Expect(counts_after_release.renderer_source_count == 0 &&
                    counts_after_release.plugin_proxy_lease_count == 0 &&
                    counts_after_release.total_count == 0,
                "duplicate release underflowed lifecycle counts") &&
         Expect(replacement.has_value(),
                "duplicate release corrupted later acquisition") &&
         ExpectCounts(lifecycle, kOwnerAlpha, 0, 1, 1,
                      "duplicate release");
}

int main() {
  return RunRendererSourceQuotaTest() && RunPluginProxyQuotaTest() &&
                 RunCombinedOwnerQuotaTest() &&
                 RunGlobalQuotaAcrossOwnersTest() &&
                 RunBackendFailureRollbackTest() &&
                 RunReleaseAndReacquireTest() &&
                 RunDuplicateReleaseTest()
             ? 0
             : 1;
}
