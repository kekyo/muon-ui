/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "plugins/muon_traffic_cardio_operation.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

struct CompletionState {
  bool completed = false;
  bool succeeded = false;
  int result = 0;
  std::string error_message;
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

int main() {
  return RunValueCompletionTest() && RunVoidCompletionTest() &&
                 RunPreCanceledOperationTest() &&
                 RunRunningCancellationTest() && RunExceptionMessageTest() &&
                 RunEmptyExceptionMessageTest()
             ? 0
             : 1;
}
