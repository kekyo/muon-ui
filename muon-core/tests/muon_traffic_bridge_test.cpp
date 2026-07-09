/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "muon_plugin_api.h"
#include "muon_cardio_post.h"
#include "plugins/muon_traffic_adapter.h"

#include <cardio.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static const tra_ffic_type kI32Type = {
    TRA_FFIC_TYPE_INT32,
    nullptr,
};

static const tra_ffic_type kI32FunctionArgs[] = {
    kI32Type,
};

static const tra_ffic_signature kI32FunctionSignature = {
    TRA_FFIC_SIGNATURE_ABI_COMPLETION,
    1,
    kI32FunctionArgs,
    &kI32Type,
    TRA_FFIC_ARGUMENT_PASSING_STACK,
};

static const tra_ffic_type kI32FunctionType = {
    TRA_FFIC_TYPE_FUNCTION,
    &kI32FunctionSignature,
};

static const tra_ffic_type kBufferViewType = {
    TRA_FFIC_TYPE_BUFFER_VIEW,
    nullptr,
};

static const tra_ffic_type kBufferViewFunctionArgs[] = {
    kBufferViewType,
};

static const tra_ffic_signature kBufferViewFunctionSignature = {
    TRA_FFIC_SIGNATURE_ABI_COMPLETION,
    1,
    kBufferViewFunctionArgs,
    &kBufferViewType,
    TRA_FFIC_ARGUMENT_PASSING_STACK,
};

static const tra_ffic_type kFunctionRoundtripArgs[] = {
    kI32FunctionType,
};

static const tra_ffic_signature kFunctionRoundtripSignature = {
    TRA_FFIC_SIGNATURE_ABI_COMPLETION,
    1,
    kFunctionRoundtripArgs,
    &kI32FunctionType,
    TRA_FFIC_ARGUMENT_PASSING_STACK,
};

struct TestSides {
  tra_ffic_task_queue queue = {};
  tra_ffic_side renderer_side = {};
  tra_ffic_side plugin_side = {};
  bool initialized = false;
};

struct CapturedResult {
  bool called = false;
  bool success = false;
  int32_t i32_value = 0;
  muon_native_function function_value = nullptr;
  void* buffer_pointer = nullptr;
  uintptr_t buffer_size = 0;
  std::vector<uint8_t> buffer_bytes;
  std::string error_message;
};

struct TrafficThreadObservation {
  std::mutex mutex;
  cardio::dispatcher* dispatcher = nullptr;
  cardio::dispatcher_group* group = nullptr;
  std::thread::id main_thread_id;
  bool notification_from_main_thread = false;
  bool notification_from_detached_thread = false;
  bool all_drains_on_main_thread = true;
  bool all_drains_on_expected_dispatcher = true;
  bool result_on_main_thread = false;
  bool result_on_expected_dispatcher = false;
  bool all_direct_tasks_on_main_thread = true;
  bool all_direct_tasks_on_expected_dispatcher = true;
  size_t notification_count = 0;
  size_t drain_post_count = 0;
  size_t drain_run_count = 0;
  size_t direct_task_run_count = 0;
  bool drain_posted = false;
};

struct ThreadedCapturedResult {
  CapturedResult captured;
  TrafficThreadObservation* observation = nullptr;
};

static bool Expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << "\n";
    return false;
  }
  return true;
}

static bool IsTrafficMainThread(
    const TrafficThreadObservation& observation) {
  return std::this_thread::get_id() == observation.main_thread_id;
}

static bool IsTrafficMainDispatcher(
    const TrafficThreadObservation& observation) {
  return cardio::unsafe_get_current_dispatcher() == observation.dispatcher;
}

static bool TrafficQueueHasTasks(tra_ffic_task_queue* queue) {
  if (queue == nullptr || !queue->initialized) {
    return false;
  }
  tra_ffic_mutex_lock(&queue->mutex);
  const auto has_tasks = queue->head != nullptr;
  tra_ffic_mutex_unlock(&queue->mutex);
  return has_tasks;
}

static void RequestTrafficDrainForTest(
    tra_ffic_task_queue* queue,
    TrafficThreadObservation* observation);

static void RequestTrafficDrainForTest(
    tra_ffic_task_queue* queue,
    TrafficThreadObservation* observation) {
  if (queue == nullptr || observation == nullptr ||
      observation->dispatcher == nullptr) {
    return;
  }

  auto should_post = false;
  {
    auto lock = std::lock_guard<std::mutex>(observation->mutex);
    if (!observation->drain_posted) {
      observation->drain_posted = true;
      observation->drain_post_count += 1u;
      should_post = true;
    }
  }
  if (!should_post) {
    return;
  }

  muon_internal::FireAndForgetOnDispatcher(
      observation->dispatcher, [queue, observation] {
    {
      auto lock = std::lock_guard<std::mutex>(observation->mutex);
      observation->drain_run_count += 1u;
      observation->all_drains_on_main_thread =
          observation->all_drains_on_main_thread &&
          IsTrafficMainThread(*observation);
      observation->all_drains_on_expected_dispatcher =
          observation->all_drains_on_expected_dispatcher &&
          IsTrafficMainDispatcher(*observation);
    }

    tra_ffic_task_drain_finalization(queue);

    {
      auto lock = std::lock_guard<std::mutex>(observation->mutex);
      observation->drain_posted = false;
    }
    if (TrafficQueueHasTasks(queue)) {
      RequestTrafficDrainForTest(queue, observation);
    }
  });
}

static void NotifyTrafficFinalizationForTest(
    tra_ffic_task_queue* queue,
    void* state) {
  auto* observation = static_cast<TrafficThreadObservation*>(state);
  if (observation == nullptr) {
    return;
  }
  {
    auto lock = std::lock_guard<std::mutex>(observation->mutex);
    observation->notification_count += 1u;
    if (IsTrafficMainThread(*observation)) {
      observation->notification_from_main_thread = true;
    } else {
      observation->notification_from_detached_thread = true;
    }
  }
  RequestTrafficDrainForTest(queue, observation);
}

static int ScheduleTrafficTask(void* schedule_data,
                               tra_ffic_task_function task,
                               void* task_data) {
  auto* queue = static_cast<tra_ffic_task_queue*>(schedule_data);
  if (queue == nullptr || task == nullptr) {
    return 0;
  }
  return tra_ffic_task_queue_schedule(queue, task, task_data);
}

static bool InitTestSides(TestSides* sides, tra_ffic_error* error) {
  if (!tra_ffic_task_queue_init(&sides->queue, nullptr, nullptr)) {
    return false;
  }
  if (!tra_ffic_side_init_pair(&sides->renderer_side, &sides->plugin_side,
                               ScheduleTrafficTask, &sides->queue, error)) {
    tra_ffic_task_queue_destroy(&sides->queue);
    return false;
  }
  sides->initialized = true;
  return true;
}

static bool InitThreadedTestSides(TestSides* sides,
                                  TrafficThreadObservation* observation,
                                  tra_ffic_error* error) {
  if (!tra_ffic_task_queue_init(
          &sides->queue, NotifyTrafficFinalizationForTest, observation)) {
    return false;
  }
  if (!tra_ffic_side_init_pair(
          &sides->renderer_side, &sides->plugin_side,
          tra_ffic_task_queue_schedule_callback, &sides->queue, error)) {
    tra_ffic_task_queue_destroy(&sides->queue);
    return false;
  }
  sides->initialized = true;
  return true;
}

static void DestroyTestSides(TestSides* sides) {
  if (!sides->initialized) {
    return;
  }
  tra_ffic_side_destroy(&sides->plugin_side);
  tra_ffic_side_destroy(&sides->renderer_side);
  tra_ffic_task_drain_finalization(&sides->queue);
  tra_ffic_task_queue_destroy(&sides->queue);
  sides->initialized = false;
}

static void CaptureResult(void* user_data, const tra_ffic_result* result) {
  auto* captured = static_cast<CapturedResult*>(user_data);
  captured->called = true;
  if (result == nullptr) {
    captured->success = false;
    captured->error_message = "missing result";
    return;
  }
  captured->success = result->success;
  if (!result->success) {
    captured->error_message = result->error_message;
    return;
  }
  if (result->value.kind == TRA_FFIC_TYPE_INT32) {
    captured->i32_value = result->value.as.int32_value;
  } else if (result->value.kind == TRA_FFIC_TYPE_FUNCTION) {
    captured->function_value = result->value.as.function_value;
  } else if (result->value.kind == TRA_FFIC_TYPE_BUFFER_VIEW) {
    const auto view = result->value.as.buffer_view_value;
    captured->buffer_pointer = view.data;
    captured->buffer_size = view.size;
    const auto* bytes = static_cast<const uint8_t*>(view.data);
    if (bytes != nullptr) {
      captured->buffer_bytes.assign(bytes, bytes + view.size);
    }
  }
}

static void CaptureThreadedResult(void* user_data,
                                  const tra_ffic_result* result) {
  auto* threaded = static_cast<ThreadedCapturedResult*>(user_data);
  if (threaded == nullptr) {
    return;
  }
  CaptureResult(&threaded->captured, result);
  auto* observation = threaded->observation;
  if (observation == nullptr) {
    return;
  }
  cardio::dispatcher_group* group = nullptr;
  {
    auto lock = std::lock_guard<std::mutex>(observation->mutex);
    observation->result_on_main_thread = IsTrafficMainThread(*observation);
    observation->result_on_expected_dispatcher =
        IsTrafficMainDispatcher(*observation);
    group = observation->group;
  }
  if (group != nullptr) {
    group->shutdown();
  }
}

static void AddOne(muon_completion_func completion, int32_t value) {
  const auto result = value + 1;
  completion(&result, nullptr);
}

static void AddOneAsync(muon_completion_func completion, int32_t value) {
  std::thread([completion, value] {
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    const auto result = value + 1;
    completion(&result, nullptr);
  }).detach();
}

static void RecordDirectTrafficTask(void* raw_state) {
  auto* observation = static_cast<TrafficThreadObservation*>(raw_state);
  if (observation == nullptr) {
    return;
  }
  auto lock = std::lock_guard<std::mutex>(observation->mutex);
  observation->direct_task_run_count += 1u;
  observation->all_direct_tasks_on_main_thread =
      observation->all_direct_tasks_on_main_thread &&
      IsTrafficMainThread(*observation);
  observation->all_direct_tasks_on_expected_dispatcher =
      observation->all_direct_tasks_on_expected_dispatcher &&
      IsTrafficMainDispatcher(*observation);
}

static void AddStateRawClosure(muon_completion_func completion,
                               void* state,
                               const tra_ffic_value* args,
                               uint32_t arg_count) {
  if (state == nullptr || args == nullptr || arg_count != 1 ||
      args[0].kind != TRA_FFIC_TYPE_INT32) {
    completion(nullptr, "invalid raw closure arguments");
    return;
  }
  const auto result =
      args[0].as.int32_value + *static_cast<int32_t*>(state);
  completion(&result, nullptr);
}

static void RoundtripFunctionRawClosure(muon_completion_func completion,
                                        void* state,
                                        const tra_ffic_value* args,
                                        uint32_t arg_count) {
  (void)state;
  if (args == nullptr || arg_count != 1 ||
      args[0].kind != TRA_FFIC_TYPE_FUNCTION ||
      args[0].as.function_value == nullptr) {
    completion(nullptr, "invalid function argument");
    return;
  }
  const auto function = args[0].as.function_value;
  completion(&function, nullptr);
}

static uint8_t g_buffer_result_storage[4] = {};

static void TransformBuffer(muon_completion_func completion,
                            muon_buffer_view value) {
  if (value.data == nullptr || value.size != sizeof(g_buffer_result_storage)) {
    completion(nullptr, "invalid buffer_view argument");
    return;
  }
  auto* bytes = static_cast<uint8_t*>(value.data);
  bytes[0] = static_cast<uint8_t>(bytes[0] + 10);
  for (auto index = size_t{0}; index < value.size; ++index) {
    g_buffer_result_storage[index] =
        static_cast<uint8_t>(bytes[value.size - 1 - index] ^ 0x55u);
  }
  muon_buffer_view result = {
      g_buffer_result_storage,
      static_cast<uintptr_t>(sizeof(g_buffer_result_storage)),
  };
  completion(&result, nullptr);
}

static bool CallFunction(tra_ffic_side* caller_side,
                         const tra_ffic_function_ref& function_ref,
                         const tra_ffic_value* args,
                         uint32_t arg_count,
                         CapturedResult* captured,
                         tra_ffic_task_queue* queue,
                         tra_ffic_error* error) {
  captured->called = false;
  captured->success = false;
  captured->i32_value = 0;
  captured->function_value = nullptr;
  captured->buffer_pointer = nullptr;
  captured->buffer_size = 0;
  captured->buffer_bytes.clear();
  captured->error_message.clear();
  if (!tra_ffic_call_with_result(caller_side, &function_ref, args, arg_count,
                                 CaptureResult, captured, error)) {
    return false;
  }
  tra_ffic_task_drain_finalization(queue);
  return true;
}

static bool RunPureFunctionBridgeTest(TestSides* sides) {
  tra_ffic_error error;
  muon_native_function function = nullptr;
  if (!Expect(tra_ffic_side_create_pure_function_impl(
                  &sides->plugin_side, &kI32FunctionSignature,
                  reinterpret_cast<tra_ffic_user_function>(&AddOne),
                  &function, &error),
              error.message)) {
    return false;
  }
  if (!Expect(tra_ffic_function_retain(function, &error), error.message)) {
    return false;
  }
  if (!Expect(tra_ffic_function_release(function, &error), error.message)) {
    return false;
  }

  tra_ffic_function_ref function_ref = {};
  if (!Expect(tra_ffic_function_ref_from_raw(function, &kI32FunctionSignature,
                                             &function_ref, &error),
              error.message)) {
    return false;
  }

  const auto arg = tra_ffic_value_int32(41);
  CapturedResult captured;
  if (!Expect(CallFunction(&sides->renderer_side, function_ref, &arg, 1,
                           &captured, &sides->queue, &error),
              error.message)) {
    return false;
  }
  return Expect(captured.called && captured.success &&
                    captured.i32_value == 42,
                "pure function bridge did not return 42");
}

static bool RunRawClosureBridgeTest(TestSides* sides) {
  tra_ffic_error error;
  auto base = int32_t{30};
  muon_native_function function = nullptr;
  if (!Expect(tra_ffic_side_create_raw_closure(
                  &sides->renderer_side, &kI32FunctionSignature,
                  AddStateRawClosure, &base, nullptr, &function, &error),
              error.message)) {
    return false;
  }

  tra_ffic_function_ref function_ref = {};
  if (!Expect(tra_ffic_function_ref_from_raw(function, &kI32FunctionSignature,
                                             &function_ref, &error),
              error.message)) {
    return false;
  }

  const auto arg = tra_ffic_value_int32(12);
  CapturedResult captured;
  if (!Expect(CallFunction(&sides->plugin_side, function_ref, &arg, 1,
                           &captured, &sides->queue, &error),
              error.message)) {
    return false;
  }
  return Expect(captured.called && captured.success &&
                    captured.i32_value == 42,
                "raw closure bridge did not return 42");
}

static bool RunFunctionArgumentBridgeTest(TestSides* sides) {
  tra_ffic_error error;
  muon_native_function add_one_function = nullptr;
  if (!Expect(tra_ffic_side_create_pure_function_impl(
                  &sides->plugin_side, &kI32FunctionSignature,
                  reinterpret_cast<tra_ffic_user_function>(&AddOne),
                  &add_one_function, &error),
              error.message)) {
    return false;
  }

  muon_native_function roundtrip_function = nullptr;
  if (!Expect(tra_ffic_side_create_raw_closure(
                  &sides->renderer_side, &kFunctionRoundtripSignature,
                  RoundtripFunctionRawClosure, nullptr, nullptr,
                  &roundtrip_function, &error),
              error.message)) {
    return false;
  }

  tra_ffic_function_ref roundtrip_ref = {};
  if (!Expect(tra_ffic_function_ref_from_raw(
                  roundtrip_function, &kFunctionRoundtripSignature,
                  &roundtrip_ref, &error),
              error.message)) {
    return false;
  }

  const auto arg = tra_ffic_value_function(add_one_function);
  CapturedResult captured;
  if (!Expect(CallFunction(&sides->plugin_side, roundtrip_ref, &arg, 1,
                           &captured, &sides->queue, &error),
              error.message)) {
    return false;
  }
  return Expect(captured.called && captured.success &&
                    captured.function_value == add_one_function,
                "function argument bridge changed the function pointer");
}

static bool RunBufferViewBridgeTest(TestSides* sides) {
  tra_ffic_error error;
  muon_native_function function = nullptr;
  if (!Expect(tra_ffic_side_create_pure_function_impl(
                  &sides->plugin_side, &kBufferViewFunctionSignature,
                  reinterpret_cast<tra_ffic_user_function>(&TransformBuffer),
                  &function, &error),
              error.message)) {
    return false;
  }

  tra_ffic_function_ref function_ref = {};
  if (!Expect(tra_ffic_function_ref_from_raw(
                  function, &kBufferViewFunctionSignature, &function_ref,
                  &error),
              error.message)) {
    return false;
  }

  uint8_t source_bytes[] = {1, 2, 3, 4};
  const auto arg =
      tra_ffic_value_buffer_view(source_bytes, sizeof(source_bytes));
  CapturedResult captured;
  if (!Expect(CallFunction(&sides->renderer_side, function_ref, &arg, 1,
                           &captured, &sides->queue, &error),
              error.message)) {
    return false;
  }
  const auto expected = std::vector<uint8_t>{
      static_cast<uint8_t>(4 ^ 0x55u),
      static_cast<uint8_t>(3 ^ 0x55u),
      static_cast<uint8_t>(2 ^ 0x55u),
      static_cast<uint8_t>(11 ^ 0x55u),
  };
  return Expect(source_bytes[0] == 11,
                "buffer_view argument mutation was not visible") &&
         Expect(captured.called && captured.success &&
                    captured.buffer_pointer == g_buffer_result_storage &&
                    captured.buffer_size == sizeof(g_buffer_result_storage) &&
                    captured.buffer_bytes == expected,
                "buffer_view bridge changed the returned bytes");
}

static cardio::promise<void> ShutdownTrafficTestAfterDelay(
    cardio::dispatcher_group* group) {
  co_await cardio::promises::delay(5000);
  group->shutdown();
}

static bool RunDetachedCompletionDrainsOnMainDispatcherTest() {
  auto group = cardio::dispatcher_group(
      cardio::exit_condition::exit_by_manual);
  auto host = cardio::dispatcher_host(group);
  auto observation = TrafficThreadObservation{};
  observation.dispatcher = cardio::unsafe_get_current_dispatcher();
  observation.group = &group;
  observation.main_thread_id = std::this_thread::get_id();

  auto timeout = ShutdownTrafficTestAfterDelay(&group);
  (void)timeout;

  tra_ffic_error error;
  TestSides sides;
  if (!Expect(InitThreadedTestSides(&sides, &observation, &error),
              error.message)) {
    return false;
  }

  auto passed = true;
  auto call_started = false;
  muon_native_function function = nullptr;
  passed = passed &&
           Expect(tra_ffic_side_create_pure_function_impl(
                      &sides.plugin_side, &kI32FunctionSignature,
                      reinterpret_cast<tra_ffic_user_function>(&AddOneAsync),
                      &function, &error),
                  error.message);

  tra_ffic_function_ref function_ref = {};
  if (passed) {
    passed = Expect(tra_ffic_function_ref_from_raw(
                        function, &kI32FunctionSignature, &function_ref,
                        &error),
                    error.message);
  }

  auto captured = ThreadedCapturedResult{};
  captured.observation = &observation;
  const auto arg = tra_ffic_value_int32(41);
  if (passed) {
    passed = Expect(tra_ffic_call_with_result(
                        &sides.renderer_side, &function_ref, &arg, 1,
                        CaptureThreadedResult, &captured, &error),
                    error.message);
    call_started = passed;
  }
  if (passed) {
    passed = Expect(tra_ffic_task_queue_schedule(
                        &sides.queue, RecordDirectTrafficTask,
                        &observation) != 0,
                    "failed to schedule first direct traffic task");
  }
  if (passed) {
    passed = Expect(tra_ffic_task_queue_schedule(
                        &sides.queue, RecordDirectTrafficTask,
                        &observation) != 0,
                    "failed to schedule second direct traffic task");
  }

  if (call_started) {
    host.park();
  }
  DestroyTestSides(&sides);
  if (!passed) {
    return false;
  }

  size_t notification_count = 0;
  size_t drain_post_count = 0;
  size_t drain_run_count = 0;
  size_t direct_task_run_count = 0;
  bool notification_from_main_thread = false;
  bool notification_from_detached_thread = false;
  bool all_drains_on_main_thread = false;
  bool all_drains_on_expected_dispatcher = false;
  bool result_on_main_thread = false;
  bool result_on_expected_dispatcher = false;
  bool all_direct_tasks_on_main_thread = false;
  bool all_direct_tasks_on_expected_dispatcher = false;
  {
    auto lock = std::lock_guard<std::mutex>(observation.mutex);
    notification_count = observation.notification_count;
    drain_post_count = observation.drain_post_count;
    drain_run_count = observation.drain_run_count;
    direct_task_run_count = observation.direct_task_run_count;
    notification_from_main_thread = observation.notification_from_main_thread;
    notification_from_detached_thread =
        observation.notification_from_detached_thread;
    all_drains_on_main_thread = observation.all_drains_on_main_thread;
    all_drains_on_expected_dispatcher =
        observation.all_drains_on_expected_dispatcher;
    result_on_main_thread = observation.result_on_main_thread;
    result_on_expected_dispatcher = observation.result_on_expected_dispatcher;
    all_direct_tasks_on_main_thread =
        observation.all_direct_tasks_on_main_thread;
    all_direct_tasks_on_expected_dispatcher =
        observation.all_direct_tasks_on_expected_dispatcher;
  }

  return Expect(captured.captured.called,
                "detached completion did not deliver a result") &&
         Expect(captured.captured.success, captured.captured.error_message) &&
         Expect(captured.captured.i32_value == 42,
                "detached completion returned wrong value") &&
         Expect(notification_from_main_thread,
                "traffic finalization was not notified from main thread") &&
         Expect(notification_from_detached_thread,
                "traffic finalization was not notified from detached thread") &&
         Expect(drain_run_count >= 1u,
                "traffic finalization drain did not run") &&
         Expect(drain_post_count < notification_count,
                "traffic finalization drain posts were not coalesced") &&
         Expect(direct_task_run_count == 2u,
                "direct traffic tasks did not both run") &&
         Expect(all_direct_tasks_on_main_thread,
                "direct traffic task did not run on main thread") &&
         Expect(all_direct_tasks_on_expected_dispatcher,
                "direct traffic task did not run on main dispatcher") &&
         Expect(all_drains_on_main_thread,
                "traffic finalization drain did not run on main thread") &&
         Expect(all_drains_on_expected_dispatcher,
                "traffic finalization drain did not run on main dispatcher") &&
         Expect(result_on_main_thread,
                "traffic result callback did not run on main thread") &&
         Expect(result_on_expected_dispatcher,
                "traffic result callback did not run on main dispatcher");
}

int main() {
  tra_ffic_error error;
  TestSides sides;
  if (!Expect(InitTestSides(&sides, &error), error.message)) {
    return 1;
  }

  const auto passed = RunPureFunctionBridgeTest(&sides) &&
                      RunRawClosureBridgeTest(&sides) &&
                      RunFunctionArgumentBridgeTest(&sides) &&
                      RunBufferViewBridgeTest(&sides) &&
                      RunDetachedCompletionDrainsOnMainDispatcherTest();
  DestroyTestSides(&sides);
  return passed ? 0 : 1;
}
