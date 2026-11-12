# Builds unchanged QuickJS sources prepared by prepare-quickjs-source.mjs.
function(muon_add_quickjs target source_root)
  file(READ "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../quickjs-source.json" profile)
  string(JSON version GET "${profile}" version)
  if(NOT EXISTS "${source_root}/quickjs.c")
    message(FATAL_ERROR "Prepare verified QuickJS ${version} sources before configuring CMake")
  endif()
  file(READ "${source_root}/VERSION" source_version)
  string(STRIP "${source_version}" source_version)
  if(NOT source_version STREQUAL version)
    message(FATAL_ERROR "QuickJS source version does not match the common profile")
  endif()
  add_library(${target} STATIC
    "${source_root}/quickjs.c"
    "${source_root}/dtoa.c"
    "${source_root}/libregexp.c"
    "${source_root}/libunicode.c"
    "${source_root}/cutils.c")
  set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON C_EXTENSIONS ON)
  target_compile_features(${target} PRIVATE c_std_11)
  target_compile_definitions(${target} PRIVATE _GNU_SOURCE CONFIG_VERSION="${version}")
  target_compile_options(${target} PRIVATE -fwrapv)
  target_include_directories(${target} SYSTEM PUBLIC "${source_root}")
  if(UNIX)
    target_link_libraries(${target} PUBLIC ${CMAKE_DL_LIBS} m)
  elseif(WIN32)
    target_link_libraries(${target} PUBLIC winpthread)
  endif()
endfunction()
