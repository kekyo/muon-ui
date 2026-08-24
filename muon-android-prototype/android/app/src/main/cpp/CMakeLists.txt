cmake_minimum_required(VERSION 3.24)

project(muon_android_rpc LANGUAGES C CXX)

find_package(Threads REQUIRED)

get_filename_component(MUON_REPOSITORY_ROOT
  "${CMAKE_CURRENT_LIST_DIR}/../../../../../.." ABSOLUTE)
set(MUON_CORE_ROOT "${MUON_REPOSITORY_ROOT}/muon-core")
set(CARDIO_ROOT_RESOLVED "${MUON_REPOSITORY_ROOT}/deps/cardio")
set(TRA_FFIC_ROOT_RESOLVED "${MUON_REPOSITORY_ROOT}/deps/tra-ffic")
set(MUON_ANDROID_DEPENDENCY_ROOT
  "${CMAKE_CURRENT_LIST_DIR}/../../../../.native-dependencies/${ANDROID_ABI}")
set(MUON_ANDROID_PLUGIN_REGISTRY_ROOT
  "${CMAKE_CURRENT_LIST_DIR}/../../../../.generated/plugin-registry")
set(QUICKJS_ROOT
  "${CMAKE_CURRENT_LIST_DIR}/../../../../.native-dependencies/quickjs/quickjs-2026-06-04")

if(NOT EXISTS "${QUICKJS_ROOT}/quickjs.c")
  message(FATAL_ERROR
    "Prepare the verified QuickJS source before configuring CMake.")
endif()

add_library(quickjs_android STATIC
  "${QUICKJS_ROOT}/quickjs.c"
  "${QUICKJS_ROOT}/dtoa.c"
  "${QUICKJS_ROOT}/libregexp.c"
  "${QUICKJS_ROOT}/libunicode.c"
  "${QUICKJS_ROOT}/cutils.c"
  )
set_target_properties(quickjs_android PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  C_EXTENSIONS ON
  )
target_compile_features(quickjs_android PRIVATE c_std_11)
target_compile_definitions(quickjs_android PRIVATE
  _GNU_SOURCE
  CONFIG_VERSION="2026-06-04"
  )
target_compile_options(quickjs_android PRIVATE
  -fwrapv
  )
target_include_directories(quickjs_android SYSTEM PUBLIC
  "${QUICKJS_ROOT}"
  )
target_link_libraries(quickjs_android PUBLIC
  dl
  m
  )

add_library(muon_javascript_runtime SHARED
  muon_javascript_runtime_jni.cpp
  )
target_compile_features(muon_javascript_runtime PRIVATE cxx_std_20)
target_compile_options(muon_javascript_runtime PRIVATE
  -Wall
  -Wextra
  -Wpedantic
  -Werror
  -Wno-c99-extensions
  )
target_link_libraries(muon_javascript_runtime PRIVATE
  quickjs_android
  android
  log
  )
target_link_options(muon_javascript_runtime PRIVATE
  -Wl,-z,max-page-size=16384
  )

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

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
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
