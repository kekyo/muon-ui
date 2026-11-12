/* Observes cross-process startup-lock cancellation and owner termination. */
#include "muon_service_process_lock.h"
#include <iostream>

static cardio::promise<void> Run(std::string key, std::string mode, int& code) {
  try {
    cardio::cancellation_source cancellation;
    std::shared_ptr<void> local;
    if (mode == "local") local = co_await AcquireMuonServiceProcessLock(key, {});
    auto pending = AcquireMuonServiceProcessLock(key, cancellation.get_cancellation());
    std::cout << "waiting" << std::endl;
    if (mode == "cancel" || mode == "local") {
      cancellation.cancel();
      bool canceled = false;
      try { co_await pending; }
      catch (const cardio::canceled_exception&) { canceled = true; }
      if (!canceled) throw std::runtime_error("Contended lock ignored cancellation");
      std::cout << "canceled" << std::endl;
      if (mode == "cancel") co_return;
      local.reset();
      pending = AcquireMuonServiceProcessLock(key, {});
    }
    auto lock = co_await pending;
    std::cout << "locked" << std::endl;
    if (mode == "hold") {
      cardio::promise_source<void> never;
      co_await never.get_promise();
    }
  } catch (const std::exception& error) {
    std::cerr << error.what() << std::endl;
    code = 1;
  }
}

#if defined(_WIN32)
int main(int argc, char** argv) {
  if (argc != 3) return 2;
  int code = 0;
  cardio::dispatcher_host_win32_auto dispatcher;
  cardio::fire_and_forget(Run(argv[1], argv[2], code));
  dispatcher.park();
  return code;
}
#else
static cardio::promise<void> RunLinux(std::string key, std::string mode,
    cardio::dispatcher_group_glib& group, int& code) {
  co_await Run(std::move(key), std::move(mode), code);
  group.shutdown();
}

int main(int argc, char** argv) {
  if (argc != 3) return 2;
  int code = 0;
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib dispatcher(group);
  cardio::fire_and_forget(RunLinux(argv[1], argv[2], group, code));
  dispatcher.park();
  return code;
}
#endif
