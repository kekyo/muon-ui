/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "services/muon_service_process.h"
#include <array>
#include <filesystem>
#include <memory>

struct MuonServiceBootstrap {
  std::shared_ptr<GSubprocess> process;
  bool closing = false;
  cardio::primitives::manually_conditional closed{false};
  std::exception_ptr close_error;
};

static cardio::promise<void> Detach(std::shared_ptr<MuonServiceBootstrap> bootstrap) {
  if (!bootstrap->closing) {
    bootstrap->closing = true;
    try {
      co_await cardio::gio::close(g_subprocess_get_stdin_pipe(bootstrap->process.get()));
    } catch (...) {
      bootstrap->close_error = std::current_exception();
    }
    bootstrap->closed.raise();
  } else {
    co_await bootstrap->closed.wait();
  }
  if (bootstrap->close_error) std::rethrow_exception(bootstrap->close_error);
}

cardio::promise<MuonServiceProcess> LaunchMuonServiceProcess(
    MuonServiceProcessOptions options, cardio::cancellation cancellation) {
  for (const auto* path : {&options.executable, &options.registry, &options.data_root}) {
    if (!std::filesystem::path(*path).is_absolute() || path->find('\0') != std::string::npos)
      throw std::invalid_argument("Service process paths must be absolute and contain no NUL");
  }
  if (cancellation.is_cancellation_requested()) throw cardio::canceled_exception();
  const char* arguments[] = {options.executable.c_str(), "--config", options.registry.c_str(),
      "--endpoint", options.endpoint.c_str(), "--data-root", options.data_root.c_str(),
      "--bootstrap", "stdin", nullptr};
  GError* error = nullptr;
  auto bootstrap = std::make_shared<MuonServiceBootstrap>();
  bootstrap->process = std::shared_ptr<GSubprocess>(g_subprocess_newv(arguments,
      static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE),
      &error), g_object_unref);
  if (!bootstrap->process) {
    const auto message = std::string(error ? error->message : "Cannot launch service host");
    if (error) g_error_free(error);
    throw std::runtime_error(message);
  }
  auto* output = g_subprocess_get_stdout_pipe(bootstrap->process.get());
  std::exception_ptr failure;
  try {
    std::string input;
    bool ready = false;
    while (!ready) {
      std::array<std::byte, 4096> bytes;
      const auto count = co_await cardio::gio::read(output, bytes, cancellation);
      if (count == 0) throw std::runtime_error("Service host exited before readiness");
      input.append(reinterpret_cast<const char*>(bytes.data()), count);
      if (input.size() > 1024 * 1024) throw std::runtime_error("Service host readiness output is too large");
      size_t end;
      while ((end = input.find('\n')) != std::string::npos) {
        if (input.substr(0, end) == "{\"ready\":true}") ready = true;
        input.erase(0, end + 1);
      }
    }
    if (cancellation.is_cancellation_requested()) throw cardio::canceled_exception();
  } catch (...) { failure = std::current_exception(); }
  // The host ignores SIGPIPE after readiness. Keeping an unread stdout pipe
  // would eventually suspend an otherwise independent resident service.
  try { co_await cardio::gio::close(output); }
  catch (...) { if (!failure) failure = std::current_exception(); }
  if (failure) {
    try { co_await Detach(bootstrap); } catch (...) {}
    std::rethrow_exception(failure);
  }
  co_return MuonServiceProcess{[bootstrap] { return Detach(bootstrap); }};
}
