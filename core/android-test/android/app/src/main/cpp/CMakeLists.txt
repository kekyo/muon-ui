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

set(MUON_ANDROID_TEST_HOST ON)
add_subdirectory("${MUON_REPOSITORY_ROOT}/core/android/runtime/src/main/cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/muon-android-runtime")

include("${MUON_ANDROID_PLUGIN_REGISTRY_ROOT}/muon_android_plugins.cmake")
if(MUON_ANDROID_PLUGIN_TARGETS)
  add_dependencies(muon_android_rpc ${MUON_ANDROID_PLUGIN_TARGETS})
endif()
