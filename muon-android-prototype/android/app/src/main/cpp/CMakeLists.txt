cmake_minimum_required(VERSION 3.24)

project(muon_android_rpc LANGUAGES C CXX)

find_package(Threads REQUIRED)

get_filename_component(MUON_REPOSITORY_ROOT
  "${CMAKE_CURRENT_LIST_DIR}/../../../../../.." ABSOLUTE)
set(MUON_CORE_ROOT "${MUON_REPOSITORY_ROOT}/muon-core")
set(CARDIO_ROOT_RESOLVED "${MUON_REPOSITORY_ROOT}/deps/cardio")
set(TRA_FFIC_ROOT_RESOLVED "${MUON_REPOSITORY_ROOT}/deps/tra-ffic")
set(MUON_ANDROID_DEPENDENCY_ROOT
  "${CMAKE_CURRENT_LIST_DIR}/../../../../.native-dependencies")
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


set(MUON_ANDROID_TEST_HOST ON)
add_subdirectory("${MUON_REPOSITORY_ROOT}/muon-android/runtime/src/main/cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/muon-android-runtime")

include("${MUON_ANDROID_PLUGIN_REGISTRY_ROOT}/muon_android_plugins.cmake")
if(MUON_ANDROID_PLUGIN_TARGETS)
  add_dependencies(muon_android_rpc ${MUON_ANDROID_PLUGIN_TARGETS})
endif()
