cmake_minimum_required(VERSION 3.24)
project(muon_android_rpc LANGUAGES C CXX)
find_package(Threads REQUIRED)

foreach(required MUON_REPOSITORY_ROOT MUON_ANDROID_DEPENDENCY_ROOT MUON_ANDROID_PLUGIN_REGISTRY_ROOT)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} must be supplied by the runtime builder")
  endif()
endforeach()
set(MUON_CORE_ROOT "${MUON_REPOSITORY_ROOT}/muon-core")
set(CARDIO_ROOT_RESOLVED "${MUON_REPOSITORY_ROOT}/deps/cardio")
set(TRA_FFIC_ROOT_RESOLVED "${MUON_REPOSITORY_ROOT}/deps/tra-ffic")
set(MUON_ANDROID_DEPENDENCY_ROOT "${MUON_ANDROID_DEPENDENCY_ROOT}/${ANDROID_ABI}")

add_subdirectory(
  "${MUON_CORE_ROOT}/src/rpc"
  "${CMAKE_CURRENT_BINARY_DIR}/muon-rpc-core"
  )

add_library(muon_sha2 STATIC
  "${MUON_REPOSITORY_ROOT}/deps/sha2/sha2.c"
  "${MUON_REPOSITORY_ROOT}/deps/sha2/sha2.h"
  )
set_target_properties(muon_sha2 PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  )
target_compile_features(muon_sha2 PRIVATE c_std_99)
target_include_directories(muon_sha2 PUBLIC
  "${MUON_REPOSITORY_ROOT}/deps/sha2"
  )

add_library(libffi_android STATIC IMPORTED GLOBAL)
set_target_properties(libffi_android PROPERTIES
  IMPORTED_LOCATION
    "${MUON_ANDROID_DEPENDENCY_ROOT}/install/lib/libffi.a"
  INTERFACE_INCLUDE_DIRECTORIES
    "${MUON_ANDROID_DEPENDENCY_ROOT}/install/include"
  )
set(LIBFFI_TARGET libffi_android)

set(MUON_CARDIO_CONFIG_DEFINES
  CARDIO_HAS_POSIX_FD=1
  CARDIO_SHARED_LIB=1
  CARDIO_WITH_LINUX_IO_URING=0
  )
add_library(cardio_shared SHARED
  "${CARDIO_ROOT_RESOLVED}/samples/libcardio/libcardio.cpp"
  "${CARDIO_ROOT_RESOLVED}/include/cardio.h"
  )
set_target_properties(cardio_shared PROPERTIES
  OUTPUT_NAME "cardio"
  )
target_compile_features(cardio_shared PRIVATE cxx_std_20)
target_compile_definitions(cardio_shared PRIVATE
  CARDIO_BUILD_SHARED_LIB=1
  ${MUON_CARDIO_CONFIG_DEFINES}
  )
target_include_directories(cardio_shared PRIVATE
  "${CARDIO_ROOT_RESOLVED}/include"
  )
target_link_libraries(cardio_shared PRIVATE
  Threads::Threads
  android
  )
target_link_options(cardio_shared PRIVATE
  -Wl,-z,max-page-size=16384
  )

add_library(muon_cardio INTERFACE)
target_compile_definitions(muon_cardio INTERFACE
  ${MUON_CARDIO_CONFIG_DEFINES}
  )
target_include_directories(muon_cardio INTERFACE
  "${CARDIO_ROOT_RESOLVED}/include"
  )
target_link_libraries(muon_cardio INTERFACE
  cardio_shared
  Threads::Threads
  android
  )

if(MUON_ANDROID_TEST_HOST AND CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(MUON_BUILD_TESTS ON)
  set(MUON_TRACK_FFI_CLOSURES ON)
else()
  set(MUON_BUILD_TESTS OFF)
  set(MUON_TRACK_FFI_CLOSURES OFF)
endif()
include("${MUON_CORE_ROOT}/cmake/MuonPluginRuntimeCore.cmake")
muon_add_plugin_runtime_core(muon_plugin_runtime_core)

if(NOT EXISTS
    "${MUON_ANDROID_PLUGIN_REGISTRY_ROOT}/muon_android_plugins.cmake")
  message(FATAL_ERROR
    "Generate the Android plugin registry before configuring CMake.")
endif()
include(
  "${MUON_ANDROID_PLUGIN_REGISTRY_ROOT}/muon_android_plugins.cmake")

add_library(muon_android_rpc SHARED
  "${MUON_ANDROID_PLUGIN_REGISTRY_ROOT}/muon_android_plugin_registry.cpp"
  muon_android_process_runtime.cpp
  muon_android_rpc_jni.cpp
  )
target_compile_features(muon_android_rpc PRIVATE cxx_std_20)
target_compile_options(muon_android_rpc PRIVATE
  -Wall
  -Wextra
  -Wpedantic
  -Werror
  )
if(MUON_BUILD_TESTS)
  target_compile_definitions(muon_android_rpc PRIVATE
    MUON_TEST_BUILD
    )
endif()
target_include_directories(muon_android_rpc PRIVATE
  "${MUON_ANDROID_PLUGIN_REGISTRY_ROOT}"
  )
target_link_libraries(muon_android_rpc PRIVATE
  "$<LINK_LIBRARY:WHOLE_ARCHIVE,muon_plugin_runtime_core>"
  android
  dl
  log
  muon_rpc_core
  )
target_link_options(muon_android_rpc PRIVATE
  -Wl,-z,max-page-size=16384
  )
if(MUON_ANDROID_PLUGIN_TARGETS)
  add_dependencies(muon_android_rpc ${MUON_ANDROID_PLUGIN_TARGETS})
endif()
