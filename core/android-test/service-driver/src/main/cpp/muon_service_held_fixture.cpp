// Reuse the identical worker without changing the shared desktop fixture.
#define muon_get_service_factory muon_get_processing_service_factory
#include "muon_service_fixture.cpp"
#undef muon_get_service_factory
#include <fcntl.h>
#include <sys/stat.h>

struct HeldStart {
  const muon_service_context* context;
  muon_service_start_completion complete;
  void* data;
  int gate;
  muon_service_instance instance{};
  std::optional<cardio::promise<void>> waiting;
  std::string error;
};

static void FinishHeldStart(void* data) {
  auto* state = static_cast<HeldStart*>(data);
  const auto complete = state->complete;
  auto* completion_data = state->data;
  const auto instance = state->instance;
  const auto error = state->error;
  close(state->gate);
  // Destroy the completed gate coroutine before native completion can unload us.
  delete state;
  complete(completion_data, error.empty() ? &instance : nullptr,
           error.empty() ? nullptr : error.c_str());
}

static void FailedHeldWorkerStopped(void* data) {
  auto* state = static_cast<HeldStart*>(data);
  state->context->host->post(state->context->host->user_data, FinishHeldStart, state);
}

static cardio::promise<void> AwaitStartGate(HeldStart* state) {
  try {
    for (;;) {
      char value;
      const auto size = read(state->gate, &value, 1);
      if (size == 1) break;
      if (size < 0 && errno != EAGAIN && errno != EINTR)
        throw std::runtime_error("Cannot read native initialization gate");
      co_await cardio::from_fd(state->gate, cardio::fd_event::read);
    }
    state->context->host->post(state->context->host->user_data, FinishHeldStart, state);
  } catch (const std::exception& error) {
    state->error = error.what();
    state->instance.stop(state->instance.user_data, FailedHeldWorkerStopped, state);
  }
}

static void ProcessingStarted(void* data, const muon_service_instance* instance, const char* error) {
  auto* state = static_cast<HeldStart*>(data);
  if (!instance || error) {
    state->error = error ? error : "Missing processing instance";
    state->context->host->post(state->context->host->user_data, FinishHeldStart, state);
    return;
  }
  state->instance = *instance;
  state->waiting.emplace(AwaitStartGate(state));
}

static void StartHeld(const muon_service_context* context,
    muon_service_start_completion complete, void* data) {
  const auto path = std::string(context->data_directory) + "/start-gate";
  if (mkfifo(path.c_str(), 0600) != 0 && errno != EEXIST) {
    complete(data, nullptr, "Cannot create native initialization gate");
    return;
  }
  const auto gate = open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (gate < 0) { complete(data, nullptr, "Cannot open native initialization gate"); return; }
  auto* state = new HeldStart{context, complete, data, gate, {}, {}, {}};
  Start(context, ProcessingStarted, state);
}

static const muon_service_factory held_factory{MUON_SERVICE_ABI_VERSION, StartHeld, interface_json};
/** @return Factory whose completion is controlled by the observer gate. */
extern "C" MUON_PLUGIN_EXPORT const muon_service_factory* muon_get_service_factory() {
  return &held_factory;
}
