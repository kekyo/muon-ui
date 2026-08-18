cmake_minimum_required(VERSION 3.22)

project(muon_android_rpc LANGUAGES C CXX)

add_subdirectory(
  "${CMAKE_CURRENT_LIST_DIR}/../../../../../../muon-core/src/rpc"
  "${CMAKE_CURRENT_BINARY_DIR}/muon-rpc-core"
  )

add_library(muon_android_rpc SHARED
  muon_android_rpc_jni.cpp
  )
target_compile_features(muon_android_rpc PRIVATE cxx_std_20)
target_compile_options(muon_android_rpc PRIVATE
  -Wall
  -Wextra
  -Wpedantic
  -Werror
  )
target_link_libraries(muon_android_rpc PRIVATE
  muon_rpc_core
  )
