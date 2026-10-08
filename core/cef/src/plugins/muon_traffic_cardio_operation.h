/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once

#include <cardio.h>

#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace muon_internal {

/**
 * Cancellation state shared by a tra-ffic entrypoint and a cardio operation.
 *
 * @remarks
 * The state records the native-facing cancellation message separately from the
 * cardio cancellation signal so canceled_exception can be translated back to
 * the AbortSignal or shutdown error that caused it.
 */
class MuonTrafficCardioCancellation final {
 public:
  MuonTrafficCardioCancellation();
  ~MuonTrafficCardioCancellation();

  MuonTrafficCardioCancellation(const MuonTrafficCardioCancellation&) = delete;
  MuonTrafficCardioCancellation& operator=(
      const MuonTrafficCardioCancellation&) = delete;

  /**
   * Returns the cardio cancellation signal associated with this state.
   *
   * @return Signal passed to the operation body.
   */
  cardio::cancellation GetCancellation() const noexcept;

  /**
   * Requests cancellation while preserving the first cancellation message.
   *
   * @param error_message Message reported if the operation observes
   *   cancellation.
   */
  void Cancel(std::string error_message);

  /**
   * Requests cancellation and replaces any earlier cancellation message.
   *
   * @param error_message Message reported if the operation observes
   *   cancellation.
   */
  void ForceCancel(std::string error_message);

  /**
   * Returns the recorded cancellation message.
   *
   * @param fallback Message returned when no cancellation message was recorded.
   * @return Recorded message or fallback.
   */
  std::string CancellationMessage(std::string fallback) const;

 private:
  void CancelImpl(std::string error_message, bool overwrite);

  cardio::cancellation_source cancellation_source_;
  std::string cancellation_error_;
};

/**
 * Converts an operation exception to a native completion error message.
 *
 * @param cancellation Cancellation state associated with the operation.
 * @param generic_error Error used for non-standard or empty-message failures.
 * @param exception Operation exception.
 * @return Message to pass to the native completion error path.
 */
std::string MuonTrafficCardioOperationExceptionMessage(
    const std::shared_ptr<MuonTrafficCardioCancellation>& cancellation,
    std::string generic_error,
    std::exception_ptr exception);

/**
 * Invokes a native completion adapter and waits for its optional async tail.
 *
 * @remarks
 * Some adapters post completion back to the UI dispatcher. Returning a promise
 * keeps the operation alive until that posted callback has actually run.
 */
template <typename Completion, typename... Args>
cardio::promise<void> InvokeMuonTrafficCompletion(Completion& completion,
                                                 Args&&... args) {
  if constexpr (std::is_void_v<std::invoke_result_t<Completion&, Args...>>) {
    completion(std::forward<Args>(args)...);
    co_return;
  } else {
    auto completion_promise = completion(std::forward<Args>(args)...);
    co_await completion_promise;
  }
}

/**
 * Bridges a cardio operation to native completion callbacks.
 *
 * @tparam Result Coroutine result type.
 * @tparam Start Callable receiving cardio::cancellation and returning
 *   cardio::promise<Result>.
 * @tparam Complete Callable receiving Result, or no argument for void.
 * @tparam Fail Callable receiving the native error message.
 * @param cancellation Cancellation state used by the operation.
 * @param generic_error Error used for non-standard or empty-message failures.
 * @param start Callable that creates the operation body.
 * @param complete Native success completion conversion.
 * @param fail Native failure completion conversion.
 * @return Promise that completes after native completion has been called.
 */
template <typename Result, typename Start, typename Complete, typename Fail>
cardio::promise<void> RunMuonTrafficCardioOperation(
    std::shared_ptr<MuonTrafficCardioCancellation> cancellation,
    std::string generic_error,
    Start start,
    Complete complete,
    Fail fail) {
  auto failure_message = std::optional<std::string>{};
  try {
    if (!cancellation) {
      co_await InvokeMuonTrafficCompletion(fail, std::move(generic_error));
      co_return;
    }
    auto start_gate = cardio::resolved();
    co_await start_gate;
    auto cancellation_signal = cancellation->GetCancellation();
    cancellation_signal.throw_if_cancellation_requested();
    auto body = start(cancellation_signal);
    if constexpr (std::is_void_v<Result>) {
      co_await body;
      co_await InvokeMuonTrafficCompletion(complete);
    } else {
      auto result = std::move(co_await body);
      co_await InvokeMuonTrafficCompletion(complete, std::move(result));
    }
    co_return;
  } catch (...) {
    failure_message = MuonTrafficCardioOperationExceptionMessage(
        cancellation, std::move(generic_error), std::current_exception());
  }
  if (failure_message) {
    co_await InvokeMuonTrafficCompletion(fail, std::move(*failure_message));
  }
}

}  // namespace muon_internal
