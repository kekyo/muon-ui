/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "services/muon_native_service_library.h"

#include "muon_filesystem.h"

#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

std::shared_ptr<const muon_service_factory> LoadMuonNativeServiceFactory(
    const std::string& path, std::string& error) {
  error.clear();
  const auto native_path = muon_internal::FilesystemPathFromUtf8(path);
  if (!native_path.is_absolute()) {
    error = "native service library path must be absolute";
    return {};
  }
#if defined(_WIN32)
  const auto io_path = muon_internal::FilesystemPathForIo(
      native_path, muon_internal::FilesystemIoUsage::File);
  const auto handle = LoadLibraryExW(io_path.c_str(), nullptr,
      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  if (!handle) {
    error = "cannot load native service library: Windows error " +
            std::to_string(GetLastError());
    return {};
  }
  auto library = std::shared_ptr<void>(handle, [](void* value) {
    FreeLibrary(static_cast<HMODULE>(value));
  });
  const auto symbol = GetProcAddress(handle, "muon_get_service_factory");
#else
  dlerror();
  auto* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!handle) {
    const auto* diagnostic = dlerror();
    error = diagnostic ? diagnostic : "cannot load native service library";
    return {};
  }
  auto library = std::shared_ptr<void>(handle, [](void* value) { dlclose(value); });
  const auto symbol = dlsym(handle, "muon_get_service_factory");
#endif
  if (!symbol) {
    error = "native service library does not export muon_get_service_factory";
    return {};
  }
  // These platform loaders represent callable addresses with the same width
  // as function pointers. Copy avoids an incompatible function-pointer cast
  // from Win32's generic FARPROC declaration.
  auto get_factory = muon_get_service_factory_func{};
  static_assert(sizeof(get_factory) == sizeof(symbol));
  std::memcpy(&get_factory, &symbol, sizeof(get_factory));
  const auto* factory = get_factory();
  if (!factory || factory->abi_version != MUON_SERVICE_ABI_VERSION ||
      !factory->start) {
    error = "native service factory ABI is incompatible";
    return {};
  }
  return {std::move(library), factory};
}
