/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "plugins/muon_traffic_cardio_operation.h"

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

struct CompletionState {
  bool completed = false;
  bool succeeded = false;
  int result = 0;
  std::string error_message;
};

struct MoveOnlyResult {
  explicit MoveOnlyResult(int source_value)
      : value(source_value) {}

  MoveOnlyResult(const MoveOnlyResult&) = delete;
  MoveOnlyResult& operator=(const MoveOnlyResult&) = delete;

  MoveOnlyResult(MoveOnlyResult&& other) noexcept
      : value(other.value) {
    other.value = 0;
  }

  MoveOnlyResult& operator=(MoveOnlyResult&& other) noexcept {
    if (this != &other) {
      value = other.value;
      other.value = 0;
    }
    return *this;
  }

  int value = 0;
};

struct StartNewHeartbeatProbe {
  std::mutex mutex;
  std::condition_variable condition;
  bool worker_started = false;
  bool release_worker = false;
};

static bool Expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << "\n";
    return false;
  }
  return true;
}

static void ResolveCompletion(CompletionState* state, int result) {
  state->completed = true;
  state->succeeded = true;
  state->result = result;
}

static void ResolveVoidCompletion(CompletionState* state) {
  state->completed = true;
  state->succeeded = true;
}

static void RejectCompletion(CompletionState* state,
                             std::string error_message) {
  state->completed = true;
  state->succeeded = false;
  state->error_message = std::move(error_message);
}

static cardio::promise<int> ReturnValueAsync(
    cardio::cancellation cancellation) {
  cancellation.throw_if_cancellation_requested();
  co_return 42;
}

static cardio::promise<MoveOnlyResult> ReturnMoveOnlyAsync(
    cardio::cancellation cancellation) {
  cancellation.throw_if_cancellation_requested();
  co_return MoveOnlyResult(57);
}

static cardio::promise<void> ReturnVoidAsync(
    cardio::cancellation cancellation) {
  cancellation.throw_if_cancellation_requested();
  co_return;
}

static cardio::promise<int> WaitForCancellationAsync(
    cardio::cancellation cancellation) {
  co_await cardio::promises::delay(5000, cancellation);
  co_return 13;
}

static cardio::promise<int> ThrowRuntimeErrorAsync(
    cardio::cancellation cancellation) {
  (void)cancellation;
  throw std::runtime_error("specific failure");
  co_return 0;
}

static cardio::promise<int> ThrowRuntimeErrorAfterSuspendAsync(
    cardio::cancellation cancellation) {
  auto start_gate = cardio::resolved();
  co_await start_gate;
  cancellation.throw_if_cancellation_requested();
  throw std::runtime_error("suspended failure");
  co_return 0;
}

static cardio::promise<int> ThrowEmptyRuntimeErrorAsync(
    cardio::cancellation cancellation) {
  (void)cancellation;
  throw std::runtime_error("");
  co_return 0;
}

static cardio::promise<void> CancelAfterDelay(
    std::shared_ptr<muon_internal::MuonTrafficCardioCancellation> cancellation,
    std::string message) {
  co_await cardio::promises::delay(1);
  cancellation->Cancel(std::move(message));
}

static cardio::promise<void> ShutdownAfterDelay(
    cardio::dispatcher_group* group) {
  co_await cardio::promises::delay(5000);
  group->shutdown();
}

static cardio::promise<void> ResolveCompletionGateAfterProbe(
    std::shared_ptr<cardio::promise_source<void>> completion_gate,
    cardio::promise<void>* operation,
    bool* operation_ready_before_completion,
    cardio::dispatcher_group* group) {
  co_await cardio::promises::delay(1);
  *operation_ready_before_completion = operation->is_ready();
  completion_gate->resolve();
  co_await cardio::promises::delay(1);
  group->shutdown();
}

static cardio::promise<void> ObserveStartNewContinuationDispatcherAsync(
    cardio::dispatcher* expected_dispatcher,
    bool* worker_has_dispatcher,
    bool* worker_uses_independent_dispatcher,
    bool* continuation_on_expected_dispatcher,
    cardio::dispatcher_group* group) {
  const auto worker_result = co_await cardio::promises::start_new(
      [expected_dispatcher] {
        auto* worker_dispatcher = cardio::unsafe_get_current_dispatcher();
        return std::pair<bool, bool>{
            worker_dispatcher != nullptr,
            worker_dispatcher != expected_dispatcher};
      });
  *worker_has_dispatcher = worker_result.first;
  *worker_uses_independent_dispatcher = worker_result.second;
  *continuation_on_expected_dispatcher =
      cardio::unsafe_get_current_dispatcher() == expected_dispatcher;
  group->shutdown();
}

static cardio::promise<void> ReleaseStartNewHeartbeatProbeAfterDelay(
    StartNewHeartbeatProbe* probe,
    cardio::dispatcher_group* group) {
  co_await cardio::promises::delay(5000);
  {
    auto lock = std::lock_guard<std::mutex>(probe->mutex);
    probe->release_worker = true;
  }
  probe->condition.notify_all();
  group->shutdown();
}

static cardio::promise<void> ObserveStartNewHeartbeatAsync(
    StartNewHeartbeatProbe* probe,
    int* heartbeat_count,
    bool* worker_completed,
    cardio::dispatcher_group* group) {
  auto worker = cardio::promises::start_new([probe] {
    {
      auto lock = std::lock_guard<std::mutex>(probe->mutex);
      probe->worker_started = true;
    }
    probe->condition.notify_all();

    auto lock = std::unique_lock<std::mutex>(probe->mutex);
    return probe->condition.wait_for(
        lock,
        std::chrono::seconds(5),
        [probe] { return probe->release_worker; });
  });

  while (true) {
    {
      auto lock = std::lock_guard<std::mutex>(probe->mutex);
      if (probe->worker_started) {
        break;
      }
    }
    co_await cardio::promises::delay(1);
  }

  while (*heartbeat_count < 3 && !worker.is_ready()) {
    ++*heartbeat_count;
    co_await cardio::promises::delay(1);
  }

  {
    auto lock = std::lock_guard<std::mutex>(probe->mutex);
    probe->release_worker = true;
  }
  probe->condition.notify_all();
  *worker_completed = co_await worker;
  group->shutdown();
}

static bool RunValueCompletionTest() {
  auto group = cardio::dispatcher_group(
      cardio::exit_condition::exit_by_manual);
  auto host = cardio::dispatcher_host(group);
  auto cancellation =
      std::make_shared<muon_internal::MuonTrafficCardioCancellation>();
  auto state = CompletionState{};
  auto timeout = ShutdownAfterDelay(&group);
  auto operation = muon_internal::RunMuonTrafficCardioOperation<int>(
      cancellation,
      "generic failure",
      [](cardio::cancellation cancellation_signal) {
        return ReturnValueAsync(cancellation_signal);
      },
      [&group, &state](int result) {
        ResolveCompletion(&state, result);
        group.shutdown();
      },
      [&group, &state](std::string error) {
        RejectCompletion(&state, std::move(error));
        group.shutdown();
      });
  (void)timeout;

  host.park();

  return Expect(operation.is_ready(), "value operation did not finish") &&
         Expect(state.completed, "value operation did not complete") &&
         Expect(state.succeeded, state.error_message) &&
         Expect(state.result == 42, "value operation returned wrong value");
}

static bool RunVoidCompletionTest() {
  auto group = cardio::dispatcher_group(
      cardio::exit_condition::exit_by_manual);
  auto host = cardio::dispatcher_host(group);
  auto cancellation =
      std::make_shared<muon_internal::MuonTrafficCardioCancellation>();
  auto state = CompletionState{};
  auto timeout = ShutdownAfterDelay(&group);
  auto operation = muon_internal::RunMuonTrafficCardioOperation<void>(
      cancellation,
      "generic failure",
      [](cardio::cancellation cancellation_signal) {
        return ReturnVoidAsync(cancellation_signal);
      },
      [&group, &state]() {
        ResolveVoidCompletion(&state);
        group.shutdown();
      },
      [&group, &state](std::string error) {
        RejectCompletion(&state, std::move(error));
        group.shutdown();
      });
  (void)timeout;

  host.park();

  return Expect(operation.is_ready(), "void operation did not finish") &&
         Expect(state.completed, "void operation did not complete") &&
         Expect(state.succeeded, state.error_message);
}

static bool RunMoveOnlyCompletionTest() {
  auto group = cardio::dispatcher_group(
      cardio::exit_condition::exit_by_manual);
  auto host = cardio::dispatcher_host(group);
  auto cancellation =
      std::make_shared<muon_internal::MuonTrafficCardioCancellation>();
  auto state = CompletionState{};
  auto timeout = ShutdownAfterDelay(&group);
  auto operation =
      muon_internal::RunMuonTrafficCardioOperation<MoveOnlyResult>(
          cancellation,
          "generic failure",
          [](cardio::cancellation cancellation_signal) {
            return ReturnMoveOnlyAsync(cancellation_signal);
          },
          [&group, &state](MoveOnlyResult result) {
            ResolveCompletion(&state, result.value);
            group.shutdown();
          },
          [&group, &state](std::string error) {
            RejectCompletion(&state, std::move(error));
            group.shutdown();
          });
  (void)timeout;

  host.park();

  return Expect(operation.is_ready(),
                "move-only operation did not finish") &&
         Expect(state.completed, "move-only operation did not complete") &&
         Expect(state.succeeded, state.error_message) &&
         Expect(state.result == 57,
                "move-only operation returned wrong value");
}

static bool RunAsyncCompletionWaitTest() {
  auto group = cardio::dispatcher_group(
      cardio::exit_condition::exit_by_manual);
  auto host = cardio::dispatcher_host(group);
  auto cancellation =
      std::make_shared<muon_internal::MuonTrafficCardioCancellation>();
  auto state = CompletionState{};
  auto completion_gate =
      std::make_shared<cardio::promise_source<void>>();
  auto completion_requested = false;
  auto operation_ready_before_completion = false;
  auto timeout = ShutdownAfterDelay(&group);
  auto operation = muon_internal::RunMuonTrafficCardioOperation<int>(
      cancellation,
      "generic failure",
      [](cardio::cancellation cancellation_signal) {
        return ReturnValueAsync(cancellation_signal);
      },
      [&state, completion_gate, &completion_requested](int result) {
        ResolveCompletion(&state, result);
        completion_requested = true;
        return completion_gate->get_promise();
      },
      [&group, &state](std::string error) {
        RejectCompletion(&state, std::move(error));
        group.shutdown();
      });
  auto gate = ResolveCompletionGateAfterProbe(
      completion_gate, &operation, &operation_ready_before_completion, &group);
  (void)timeout;
  (void)gate;

  host.park();

  return Expect(completion_requested,
                "async completion was not requested") &&
         Expect(!operation_ready_before_completion,
                "operation finished before async completion") &&
         Expect(operation.is_ready(), "async completion operation did not finish") &&
         Expect(state.completed, "async completion did not complete") &&
         Expect(state.succeeded, state.error_message) &&
         Expect(state.result == 42,
                "async completion operation returned wrong value");
}

static bool RunPreCanceledOperationTest() {
  auto group = cardio::dispatcher_group(
      cardio::exit_condition::exit_by_manual);
  auto host = cardio::dispatcher_host(group);
  auto cancellation =
      std::make_shared<muon_internal::MuonTrafficCardioCancellation>();
  auto state = CompletionState{};
  auto body_started = false;
  cancellation->Cancel("pre-canceled");
  auto timeout = ShutdownAfterDelay(&group);
  auto operation = muon_internal::RunMuonTrafficCardioOperation<int>(
      cancellation,
      "generic failure",
      [&body_started](cardio::cancellation cancellation_signal) {
        body_started = true;
        return ReturnValueAsync(cancellation_signal);
      },
      [&group, &state](int result) {
        ResolveCompletion(&state, result);
        group.shutdown();
      },
      [&group, &state](std::string error) {
        RejectCompletion(&state, std::move(error));
        group.shutdown();
      });
  (void)timeout;

  host.park();

  return Expect(operation.is_ready(), "pre-canceled operation did not finish") &&
         Expect(state.completed, "pre-canceled operation did not complete") &&
         Expect(!state.succeeded, "pre-canceled operation succeeded") &&
         Expect(state.error_message == "pre-canceled",
                "pre-canceled operation returned wrong error") &&
         Expect(!body_started, "pre-canceled operation body started");
}

static bool RunRunningCancellationTest() {
  auto group = cardio::dispatcher_group(
      cardio::exit_condition::exit_by_manual);
  auto host = cardio::dispatcher_host(group);
  auto cancellation =
      std::make_shared<muon_internal::MuonTrafficCardioCancellation>();
  auto state = CompletionState{};
  auto canceler = CancelAfterDelay(cancellation, "running abort");
  auto timeout = ShutdownAfterDelay(&group);
  auto operation = muon_internal::RunMuonTrafficCardioOperation<int>(
      cancellation,
      "generic failure",
      [](cardio::cancellation cancellation_signal) {
        return WaitForCancellationAsync(cancellation_signal);
      },
      [&group, &state](int result) {
        ResolveCompletion(&state, result);
        group.shutdown();
      },
      [&group, &state](std::string error) {
        RejectCompletion(&state, std::move(error));
        group.shutdown();
      });
  (void)canceler;
  (void)timeout;

  host.park();

  return Expect(operation.is_ready(), "running cancel operation did not finish") &&
         Expect(state.completed, "running cancel operation did not complete") &&
         Expect(!state.succeeded, "running cancel operation succeeded") &&
         Expect(state.error_message == "running abort",
                "running cancel operation returned wrong error");
}

static bool RunExceptionMessageTest() {
  auto group = cardio::dispatcher_group(
      cardio::exit_condition::exit_by_manual);
  auto host = cardio::dispatcher_host(group);
  auto cancellation =
      std::make_shared<muon_internal::MuonTrafficCardioCancellation>();
  auto state = CompletionState{};
  auto timeout = ShutdownAfterDelay(&group);
  auto operation = muon_internal::RunMuonTrafficCardioOperation<int>(
      cancellation,
      "generic failure",
      [](cardio::cancellation cancellation_signal) {
        return ThrowRuntimeErrorAsync(cancellation_signal);
      },
      [&group, &state](int result) {
        ResolveCompletion(&state, result);
        group.shutdown();
      },
      [&group, &state](std::string error) {
        RejectCompletion(&state, std::move(error));
        group.shutdown();
      });
  (void)timeout;

  host.park();

  return Expect(operation.is_ready(), "exception operation did not finish") &&
         Expect(state.completed, "exception operation did not complete") &&
         Expect(!state.succeeded, "exception operation succeeded") &&
         Expect(state.error_message == "specific failure",
                "exception operation returned wrong error");
}

static bool RunSuspendedExceptionMessageTest() {
  auto group = cardio::dispatcher_group(
      cardio::exit_condition::exit_by_manual);
  auto host = cardio::dispatcher_host(group);
  auto cancellation =
      std::make_shared<muon_internal::MuonTrafficCardioCancellation>();
  auto state = CompletionState{};
  auto timeout = ShutdownAfterDelay(&group);
  auto operation = muon_internal::RunMuonTrafficCardioOperation<int>(
      cancellation,
      "generic failure",
      [](cardio::cancellation cancellation_signal) {
        return ThrowRuntimeErrorAfterSuspendAsync(cancellation_signal);
      },
      [&group, &state](int result) {
        ResolveCompletion(&state, result);
        group.shutdown();
      },
      [&group, &state](std::string error) {
        RejectCompletion(&state, std::move(error));
        group.shutdown();
      });
  (void)timeout;

  host.park();

  return Expect(operation.is_ready(),
                "suspended exception operation did not finish") &&
         Expect(state.completed,
                "suspended exception operation did not complete") &&
         Expect(!state.succeeded, "suspended exception operation succeeded") &&
         Expect(state.error_message == "suspended failure",
                "suspended exception operation returned wrong error");
}

static bool RunEmptyExceptionMessageTest() {
  auto group = cardio::dispatcher_group(
      cardio::exit_condition::exit_by_manual);
  auto host = cardio::dispatcher_host(group);
  auto cancellation =
      std::make_shared<muon_internal::MuonTrafficCardioCancellation>();
  auto state = CompletionState{};
  auto timeout = ShutdownAfterDelay(&group);
  auto operation = muon_internal::RunMuonTrafficCardioOperation<int>(
      cancellation,
      "generic failure",
      [](cardio::cancellation cancellation_signal) {
        return ThrowEmptyRuntimeErrorAsync(cancellation_signal);
      },
      [&group, &state](int result) {
        ResolveCompletion(&state, result);
        group.shutdown();
      },
      [&group, &state](std::string error) {
        RejectCompletion(&state, std::move(error));
        group.shutdown();
      });
  (void)timeout;

  host.park();

  return Expect(operation.is_ready(), "empty exception operation did not finish") &&
         Expect(state.completed, "empty exception operation did not complete") &&
         Expect(!state.succeeded, "empty exception operation succeeded") &&
         Expect(state.error_message == "generic failure",
                "empty exception operation returned wrong error");
}

static bool RunStartNewContinuationDispatcherTest() {
  auto group = cardio::dispatcher_group(
      cardio::exit_condition::exit_by_manual);
  auto host = cardio::dispatcher_host(group);
  auto* dispatcher = cardio::unsafe_get_current_dispatcher();
  auto worker_has_dispatcher = false;
  auto worker_uses_independent_dispatcher = false;
  auto continuation_on_expected_dispatcher = false;
  auto timeout = ShutdownAfterDelay(&group);
  auto operation = ObserveStartNewContinuationDispatcherAsync(
      dispatcher,
      &worker_has_dispatcher,
      &worker_uses_independent_dispatcher,
      &continuation_on_expected_dispatcher,
      &group);
  (void)timeout;

  host.park();

  return Expect(operation.is_ready(),
                "start_new dispatcher operation did not finish") &&
         Expect(worker_has_dispatcher,
                "start_new worker did not have a dispatcher") &&
         Expect(worker_uses_independent_dispatcher,
                "start_new worker reused the caller dispatcher") &&
         Expect(continuation_on_expected_dispatcher,
                "start_new continuation did not resume on the caller dispatcher");
}

static bool RunStartNewHeartbeatTest() {
  auto group = cardio::dispatcher_group(
      cardio::exit_condition::exit_by_manual);
  auto host = cardio::dispatcher_host(group);
  auto probe = StartNewHeartbeatProbe{};
  auto heartbeat_count = 0;
  auto worker_completed = false;
  auto timeout = ReleaseStartNewHeartbeatProbeAfterDelay(&probe, &group);
  auto operation = ObserveStartNewHeartbeatAsync(
      &probe, &heartbeat_count, &worker_completed, &group);
  (void)timeout;

  host.park();

  return Expect(operation.is_ready(),
                "start_new heartbeat operation did not finish") &&
         Expect(heartbeat_count >= 3,
                "dispatcher did not tick while start_new worker was blocked") &&
         Expect(worker_completed,
                "start_new worker was not released by the dispatcher");
}

int main() {
  return RunValueCompletionTest() && RunVoidCompletionTest() &&
                 RunMoveOnlyCompletionTest() &&
                 RunAsyncCompletionWaitTest() && RunPreCanceledOperationTest() &&
                 RunRunningCancellationTest() && RunExceptionMessageTest() &&
                 RunSuspendedExceptionMessageTest() &&
                 RunEmptyExceptionMessageTest() &&
                 RunStartNewContinuationDispatcherTest() &&
                 RunStartNewHeartbeatTest()
             ? 0
             : 1;
}
