#include "muon_plugin_api.h"
#include <cardio.h>
#include <cstdio>
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

static int32_t result = -1;
static bool stopped = false;
static void completed(const void* value, const char* error) {
  result = error == nullptr ? *static_cast<const int32_t*>(value) : -1;
}
static void stop_completed(void*) { stopped = true; }

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  cardio::dispatcher_host dispatcher;
#if defined(_WIN32)
  auto* library = LoadLibraryExA(argv[1], nullptr,
      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  if (library == nullptr) { std::fprintf(stderr, "LoadLibrary: %lu\n", GetLastError()); return 10; }
  const auto initialize = reinterpret_cast<muon_init_plugin_func>(GetProcAddress(library, "muon_init_plugin"));
#else
  auto* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (library == nullptr) { std::fprintf(stderr, "%s\n", dlerror()); return 10; }
  const auto initialize = reinterpret_cast<muon_init_plugin_func>(dlsym(library, "muon_init_plugin"));
#endif
  if (initialize == nullptr) return 11;
  const auto* metadata = initialize(nullptr);
  if (metadata == nullptr || metadata->namespaces == nullptr) return 12;
  const auto* function = metadata->namespaces[0]->functions[0];
  reinterpret_cast<void (*)(muon_completion_func, int32_t)>(function->native_func)(completed, 41);
  dispatcher.park();
  if (result != 42) return 13;
  if (metadata->stop != nullptr) {
    metadata->stop(stop_completed, nullptr);
    dispatcher.park();
    if (!stopped) return 14;
  }
#if defined(_WIN32)
  FreeLibrary(library);
#else
  dlclose(library);
#endif
  std::puts("plugin function and shutdown passed");
  return 0;
}
