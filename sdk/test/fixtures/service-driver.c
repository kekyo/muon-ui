#include "muon_service_api.h"
#include <dlfcn.h>
#include <stdlib.h>

typedef struct start_result {
  muon_service_instance instance;
  int completed;
} start_result;

static void started(void* data, const muon_service_instance* instance,
                    const char* error) {
  if (error != NULL || instance == NULL) exit(20);
  start_result* result = (start_result*)data;
  result->instance = *instance;
  result->completed += 1;
}

static void stopped(void* data) { *(int*)data += 1; }

static void write_log(void* data, muon_log_level level, const char* message) {
  (void)data;
  (void)level;
  if (message == NULL) exit(21);
}

static void unexpected_task(void* data, muon_service_task task, void* task_data) {
  (void)data;
  (void)task;
  (void)task_data;
  exit(22);
}

int main(int argc, char** argv) {
  if (argc != 2) return 1;
  void* library = dlopen(argv[1], RTLD_NOW);
  if (library == NULL) return 2;
  muon_get_service_factory_func get_factory =
      (muon_get_service_factory_func)dlsym(library, "muon_get_service_factory");
  if (get_factory == NULL) return 3;
  const muon_service_factory* factory = get_factory();
  if (factory == NULL || factory->abi_version != MUON_SERVICE_ABI_VERSION)
    return 4;
  const muon_service_host host = {NULL, unexpected_task, write_log, NULL};
  const muon_service_context alpha = {&host, "alpha", 1, "/tmp", 0, NULL, MUON_SERVICE_START_ACQUIRE, NULL, NULL};
  const muon_service_context beta = {&host, "beta", 1, "/tmp", 0, NULL, MUON_SERVICE_START_ACQUIRE, NULL, NULL};
  start_result first = {0};
  start_result second = {0};
  factory->start(&alpha, started, &first);
  factory->start(&beta, started, &second);
  if (first.completed != 1 || second.completed != 1 ||
      first.instance.user_data == second.instance.user_data ||
      first.instance.stop == NULL || second.instance.stop == NULL) return 5;
  int stops = 0;
  const muon_service_stop_info info = {MUON_SERVICE_ABI_VERSION, sizeof(muon_service_stop_info), MUON_SERVICE_STOP_APP_EXIT, NULL, -1, NULL};
  first.instance.stop(first.instance.user_data, &info, stopped, &stops);
  if (stops != 1) return 6;
  second.instance.stop(second.instance.user_data, &info, stopped, &stops);
  if (stops != 2) return 7;
  dlclose(library);
  return 0;
}
