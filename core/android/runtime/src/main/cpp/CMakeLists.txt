cmake_minimum_required(VERSION 3.24)
project(muon_android_rpc LANGUAGES C CXX)
find_package(Threads REQUIRED)

foreach(required MUON_REPOSITORY_ROOT MUON_ANDROID_DEPENDENCY_ROOT)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} must be supplied by the runtime builder")
  endif()
endforeach()
set(MUON_COMMON_ROOT "${MUON_REPOSITORY_ROOT}/core/common")
set(CARDIO_ROOT_RESOLVED "${MUON_REPOSITORY_ROOT}/deps/cardio")
set(TRA_FFIC_ROOT_RESOLVED "${MUON_REPOSITORY_ROOT}/deps/tra-ffic")
set(MUON_ANDROID_QUICKJS_ROOT "${MUON_ANDROID_DEPENDENCY_ROOT}/quickjs")
set(MUON_ANDROID_DEPENDENCY_ROOT "${MUON_ANDROID_DEPENDENCY_ROOT}/${ANDROID_ABI}")
file(READ "${MUON_COMMON_ROOT}/quickjs-source.json" MUON_QUICKJS_PROFILE)
string(JSON MUON_QUICKJS_VERSION GET "${MUON_QUICKJS_PROFILE}" version)
include("${MUON_COMMON_ROOT}/cmake/MuonQuickJs.cmake")
muon_add_quickjs(muon_service_quickjs
  "${MUON_ANDROID_QUICKJS_ROOT}/quickjs-${MUON_QUICKJS_VERSION}")
set(MUON_YYJSON_SOURCE "${MUON_REPOSITORY_ROOT}/builder/.deps/src/yyjson-0.12.0/src")
add_library(muon_service_json STATIC "${MUON_YYJSON_SOURCE}/yyjson.c")
set_target_properties(muon_service_json PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(muon_service_json PRIVATE c_std_99)
target_include_directories(muon_service_json PUBLIC "${MUON_YYJSON_SOURCE}")

add_subdirectory(
  "${MUON_COMMON_ROOT}/src/rpc"
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

include("${MUON_REPOSITORY_ROOT}/sdk/cmake/MuonRuntimeProfile.cmake")
muon_runtime_cardio_definitions(android MUON_CARDIO_CONFIG_DEFINES)
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
include("${MUON_COMMON_ROOT}/cmake/MuonPluginRuntimeCore.cmake")
muon_add_plugin_runtime_core(muon_plugin_runtime_core)

add_library(muon_android_rpc SHARED
  muon_android_service_client_jni.cpp
  muon_android_service_transport.cpp
  "${MUON_COMMON_ROOT}/src/services/muon_service_client.cpp"
  muon_android_dispatcher.cpp
  muon_android_utf.cpp
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
if(MUON_ANDROID_TEST_HOST)
  target_compile_definitions(muon_android_rpc PRIVATE MUON_ANDROID_TEST_HOST)
endif()
target_link_libraries(muon_android_rpc PRIVATE
  "$<LINK_LIBRARY:WHOLE_ARCHIVE,muon_plugin_runtime_core>"
  android
  dl
  log
  muon_rpc_core
  muon_service_json
  )
target_link_options(muon_android_rpc PRIVATE
  -Wl,-z,max-page-size=16384
  )

# The background process owns this host independently of the page plugin runtime.
add_library(muon_android_services SHARED
  muon_android_service_plugins.cpp
  muon_android_service_peer.cpp
  muon_android_service_transport.cpp
  muon_android_utf.cpp
  muon_android_services_jni.cpp
  "${MUON_COMMON_ROOT}/src/muon_filesystem.cpp"
  "${MUON_COMMON_ROOT}/src/services/muon_service_lifecycle.cpp"
  "${MUON_COMMON_ROOT}/src/services/muon_service_controller.cpp"
  "${MUON_COMMON_ROOT}/src/services/muon_service_broker.cpp"
  "${MUON_COMMON_ROOT}/src/services/muon_service_execution_router.cpp"
  "${MUON_COMMON_ROOT}/src/services/muon_quickjs_service_host.cpp"
  "${MUON_COMMON_ROOT}/src/services/muon_quickjs_service_plugins.cpp"
  "${MUON_COMMON_ROOT}/src/services/muon_service_plugin_pool.cpp"
  "${MUON_COMMON_ROOT}/src/services/muon_quickjs_plugin_bindings.cpp"
  "${MUON_COMMON_ROOT}/src/quickjs/muon_quickjs_engine.cpp"
  "${MUON_COMMON_ROOT}/src/quickjs/muon_quickjs_module.cpp"
  "${MUON_COMMON_ROOT}/src/services/muon_native_service_host.cpp"
  "${MUON_COMMON_ROOT}/src/services/muon_service_value.cpp"
  "${MUON_COMMON_ROOT}/src/services/muon_service_interface.cpp"
  "${MUON_COMMON_ROOT}/src/services/muon_native_service_library.cpp"
  )
target_compile_features(muon_android_services PRIVATE cxx_std_20)
target_compile_options(muon_android_services PRIVATE -Wall -Wextra -Wpedantic -Werror -Wno-c99-extensions)
target_include_directories(muon_android_services PRIVATE
  "${MUON_COMMON_ROOT}/src" "${MUON_COMMON_ROOT}/include"
  "${MUON_REPOSITORY_ROOT}/sdk/include")
target_link_libraries(muon_android_services PRIVATE muon_plugin_runtime_core muon_cardio muon_service_json muon_service_quickjs android dl log)
target_link_options(muon_android_services PRIVATE -Wl,-z,max-page-size=16384)
