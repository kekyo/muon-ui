/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#pragma once

#include <cardio.h>

#include <memory>
#include <type_traits>
#include <utility>

namespace muon_internal {

class MuonCurrentDispatcherScope final {
 public:
  explicit MuonCurrentDispatcherScope(cardio::dispatcher* dispatcher) noexcept
      : previous_(cardio::unsafe_get_current_dispatcher()) {
    if (previous_ != dispatcher) {
      cardio::set_current_dispatcher(dispatcher);
    }
  }

  ~MuonCurrentDispatcherScope() noexcept {
    if (cardio::unsafe_get_current_dispatcher() != previous_) {
      cardio::set_current_dispatcher(previous_);
    }
  }

  MuonCurrentDispatcherScope(const MuonCurrentDispatcherScope&) = delete;
  MuonCurrentDispatcherScope& operator=(
      const MuonCurrentDispatcherScope&) = delete;

 private:
  cardio::dispatcher* previous_ = nullptr;
};

template <typename Task>
cardio::promise<void> RunMuonPostedTaskAsync(
    cardio::promise<void> ready,
    std::shared_ptr<Task> task) {
  co_await ready;
  auto current_task = std::move(task);
  (*current_task)();
  current_task.reset();
}

template <typename Task>
void FireAndForgetOnDispatcher(cardio::dispatcher* dispatcher, Task&& task) {
  using TaskStorage = std::decay_t<Task>;
  auto dispatcher_scope = MuonCurrentDispatcherScope(dispatcher);
  (void)dispatcher_scope;
  auto ready_source = std::make_shared<cardio::promise_source<void>>();
  auto ready = ready_source->get_promise();
  auto posted_task =
      std::make_shared<TaskStorage>(std::forward<Task>(task));
  cardio::fire_and_forget(RunMuonPostedTaskAsync(
      std::move(ready), std::move(posted_task)));
  {
    // Keep the post semantics separate from cardio's inline continuation path.
    auto enqueue_scope = MuonCurrentDispatcherScope(nullptr);
    (void)enqueue_scope;
    ready_source->resolve();
  }
}

}  // namespace muon_internal
