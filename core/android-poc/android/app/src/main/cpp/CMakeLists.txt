cmake_minimum_required(VERSION 3.24)

project(muon_android_rpc LANGUAGES C CXX)

find_package(Threads REQUIRED)

get_filename_component(MUON_REPOSITORY_ROOT
  "${CMAKE_CURRENT_LIST_DIR}/../../../../../../.." ABSOLUTE)
set(MUON_COMMON_ROOT "${MUON_REPOSITORY_ROOT}/core/common")
set(CARDIO_ROOT_RESOLVED "${MUON_REPOSITORY_ROOT}/deps/cardio")
set(TRA_FFIC_ROOT_RESOLVED "${MUON_REPOSITORY_ROOT}/deps/tra-ffic")
set(MUON_ANDROID_DEPENDENCY_ROOT
  "${CMAKE_CURRENT_LIST_DIR}/../../../../.native-dependencies")
set(MUON_ANDROID_PLUGIN_REGISTRY_ROOT
  "${CMAKE_CURRENT_LIST_DIR}/../../../../.generated/plugin-registry")
set(QUICKJS_ROOT
  "${CMAKE_CURRENT_LIST_DIR}/../../../../.native-dependencies/quickjs/quickjs-2026-06-04")

include("${MUON_COMMON_ROOT}/cmake/MuonQuickJs.cmake")
muon_add_quickjs(quickjs_android "${QUICKJS_ROOT}")

add_library(muon_javascript_runtime SHARED
  muon_javascript_runtime_jni.cpp
  "${MUON_COMMON_ROOT}/src/quickjs/muon_quickjs_module.cpp"
  "${MUON_COMMON_ROOT}/src/quickjs/muon_quickjs_engine.cpp"
  )
target_include_directories(muon_javascript_runtime PRIVATE
  "${MUON_COMMON_ROOT}/src"
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


set(MUON_ANDROID_TEST_HOST OFF)
add_subdirectory("${MUON_REPOSITORY_ROOT}/core/android/runtime/src/main/cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/muon-android-runtime")

include("${MUON_ANDROID_PLUGIN_REGISTRY_ROOT}/muon_android_plugins.cmake")
if(MUON_ANDROID_PLUGIN_TARGETS)
  add_dependencies(muon_android_rpc ${MUON_ANDROID_PLUGIN_TARGETS})
endif()
