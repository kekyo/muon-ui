/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "muon_plugin_api.h"

#include <cardio.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <unistd.h>

static std::atomic_bool dispatcher_available_at_init = false;
static std::thread::id dispatcher_init_thread;

static constexpr uint32_t dispatcher_init_bit = 1U << 0;
static constexpr uint32_t dispatcher_call_bit = 1U << 1;
static constexpr uint32_t dispatcher_thread_bit = 1U << 2;
static constexpr uint32_t direct_bit = 1U << 3;
static constexpr uint32_t timer_bit = 1U << 4;
static constexpr uint32_t fd_bit = 1U << 5;
static constexpr uint32_t worker_bit = 1U << 6;
static constexpr uint32_t all_event_bits =
    direct_bit | timer_bit | fd_bit | worker_bit;

static cardio::dispatcher* try_get_current_dispatcher() noexcept {
  try {
    return &cardio::get_current_dispatcher();
  } catch (...) {
    return nullptr;
  }
}

static void append_stop_marker(const char* text) {
  const auto* path = std::getenv("MUON_TEST_PLUGIN_STOP_MARKER");
  if (path == nullptr || path[0] == '\0') {
    return;
  }
  auto* file = std::fopen(path, "ab");
  if (file == nullptr) {
    return;
  }
  std::fputs(text, file);
  std::fclose(file);
}

struct CardioPluginUnloadMarker {
  ~CardioPluginUnloadMarker() { append_stop_marker("unloaded\n"); }
};

static CardioPluginUnloadMarker unload_marker;

static cardio::promise<void> complete_stop_after_delay(
    muon_plugin_stop_completion completion,
    void* user_data) {
  co_await cardio::promises::delay(25);
  append_stop_marker("stop-completed\n");
  completion(user_data);
}

static void stop_cardio_plugin(muon_plugin_stop_completion completion,
                               void* user_data) {
  append_stop_marker("stop-started\n");
  if (try_get_current_dispatcher() == nullptr) {
    append_stop_marker("stop-completed\n");
    completion(user_data);
    return;
  }
  cardio::fire_and_forget(
      complete_stop_after_delay(completion, user_data));
}

extern "C" void dispatcher_available_at_init_call(muon_completion_func comp) {
  const auto result = dispatcher_available_at_init.load();
  comp(&result, nullptr);
}

extern "C" void dispatcher_available(muon_completion_func comp) {
  const auto result = try_get_current_dispatcher() != nullptr;
  comp(&result, nullptr);
}

struct DispatcherProbeState {
  muon_completion_func completion = nullptr;
  std::thread::id owner_thread;
  int pipe_fds[2]{-1, -1};
  std::thread worker;
  uint32_t mask = 0;
  bool completed = false;

  ~DispatcherProbeState() {
    if (worker.joinable()) {
      worker.join();
    }
    if (pipe_fds[0] >= 0) {
      (void)::close(pipe_fds[0]);
    }
    if (pipe_fds[1] >= 0) {
      (void)::close(pipe_fds[1]);
    }
  }

  void complete_event(uint32_t bit) {
    if (std::this_thread::get_id() != owner_thread) {
      mask &= ~dispatcher_thread_bit;
    }
    mask |= bit;
    if (!completed && (mask & all_event_bits) == all_event_bits) {
      completed = true;
      const auto result = mask;
      completion(&result, nullptr);
    }
  }
};

static cardio::promise<void> complete_probe_timer(
    std::shared_ptr<DispatcherProbeState> state) {
  co_await cardio::promises::delay(25);
  state->complete_event(timer_bit);
}

static cardio::promise<void> complete_probe_fd(
    std::shared_ptr<DispatcherProbeState> state) {
  const auto events = co_await cardio::from_fd(
      state->pipe_fds[0], cardio::fd_event::read);
  if ((events & cardio::fd_event::read) == cardio::fd_event::none) {
    state->mask &= ~dispatcher_call_bit;
  }
  state->complete_event(fd_bit);
}

static cardio::promise<void> complete_probe_worker(
    std::shared_ptr<DispatcherProbeState> state,
    cardio::promise<void> ready) {
  co_await ready;
  state->complete_event(worker_bit);
}

extern "C" void dispatcher_probe(muon_completion_func completion) {
  auto state = std::make_shared<DispatcherProbeState>();
  state->completion = completion;
  state->owner_thread = std::this_thread::get_id();
  if (dispatcher_available_at_init.load()) {
    state->mask |= dispatcher_init_bit;
  }
  if (try_get_current_dispatcher() != nullptr) {
    state->mask |= dispatcher_call_bit;
  }
  if (state->owner_thread == dispatcher_init_thread) {
    state->mask |= dispatcher_thread_bit;
  }
  if (::pipe(state->pipe_fds) != 0) {
    const auto result = state->mask;
    completion(&result, nullptr);
    return;
  }

  auto worker_ready =
      std::make_shared<cardio::promise_source<void>>();
  auto worker_promise = worker_ready->get_promise();
  cardio::fire_and_forget(complete_probe_timer(state));
  cardio::fire_and_forget(complete_probe_fd(state));
  cardio::fire_and_forget(
      complete_probe_worker(state, std::move(worker_promise)));
  const auto write_fd = state->pipe_fds[1];
  state->worker = std::thread([worker_ready, write_fd] {
    (void)worker_ready->try_resolve();
    const auto value = char{'x'};
    auto result = ssize_t{};
    do {
      result = ::write(write_fd, &value, 1);
    } while (result == -1 && errno == EINTR);
  });
  state->complete_event(direct_bit);
}

static const muon_type_descriptor type_bool = {
    MUON_TYPE_BOOL,
    nullptr,
};

static const muon_type_descriptor type_u32 = {
    MUON_TYPE_U32,
    nullptr,
};

static const muon_plugin_function_metadata cardio_functions[] = {
    {
        "dispatcherAvailableAtInit",
        reinterpret_cast<muon_native_function>(
            &dispatcher_available_at_init_call),
        {0, nullptr, &type_bool},
        nullptr,
    },
    {
        "dispatcherAvailable",
        reinterpret_cast<muon_native_function>(&dispatcher_available),
        {0, nullptr, &type_bool},
        nullptr,
    },
    {
        "dispatcherProbe",
        reinterpret_cast<muon_native_function>(&dispatcher_probe),
        {0, nullptr, &type_u32},
        nullptr,
    },
};

static const muon_plugin_function_metadata* const cardio_functions_pointers[] = {
    &cardio_functions[0],
    &cardio_functions[1],
    &cardio_functions[2],
    nullptr,
};

static const muon_plugin_namespace cardio_namespaces[] = {
    {
        "muon.test.cardio",
        nullptr,
        cardio_functions_pointers,
    },
};

static const muon_plugin_namespace* const cardio_namespaces_pointers[] = {
    &cardio_namespaces[0],
    nullptr,
};

static const muon_plugin_metadata cardio_metadata = {
    cardio_namespaces_pointers,
    &stop_cardio_plugin,
    nullptr,
};

extern "C" const muon_plugin_metadata* muon_init_plugin(
    const muon_plugin_init_context* context) {
  (void)context;
  dispatcher_available_at_init.store(
      try_get_current_dispatcher() != nullptr);
  dispatcher_init_thread = std::this_thread::get_id();
  return &cardio_metadata;
}
