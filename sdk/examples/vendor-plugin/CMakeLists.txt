cmake_minimum_required(VERSION 3.24)
project(vendor_counter LANGUAGES C)
find_package(MuonPlugin CONFIG REQUIRED )
add_library(vendor_counter SHARED plugin.c)
target_link_libraries(vendor_counter PRIVATE Muon::Plugin)
set_target_properties(vendor_counter PROPERTIES
  C_STANDARD 99 C_STANDARD_REQUIRED YES
  CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES
  PREFIX "" OUTPUT_NAME "${MUON_PLUGIN_OUTPUT_NAME}"
  LIBRARY_OUTPUT_DIRECTORY "${MUON_PLUGIN_OUTPUT_DIRECTORY}"
  RUNTIME_OUTPUT_DIRECTORY "${MUON_PLUGIN_OUTPUT_DIRECTORY}"
  ARCHIVE_OUTPUT_DIRECTORY "${MUON_PLUGIN_OUTPUT_DIRECTORY}/link"
  BUILD_WITH_INSTALL_RPATH YES INSTALL_RPATH "$ORIGIN")
# Keep the external source tree unchanged and put its outputs in the build tree.
if(NOT IS_DIRECTORY "${VENDOR_SOURCE_DIR}")
  message(FATAL_ERROR "Set VENDOR_SOURCE_DIR to the vendor/submodule checkout.")
endif()
add_subdirectory("${VENDOR_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/vendor")
set_target_properties(example_vendor PROPERTIES
  PREFIX "lib" OUTPUT_NAME "muon_example_vendor"
  LIBRARY_OUTPUT_DIRECTORY "${MUON_PLUGIN_OUTPUT_DIRECTORY}"
  RUNTIME_OUTPUT_DIRECTORY "${MUON_PLUGIN_OUTPUT_DIRECTORY}")
target_link_libraries(vendor_counter PRIVATE example_vendor)
