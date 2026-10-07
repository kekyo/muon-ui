# muon - Multi-platform GUI application framework that uses CEF as its backend
# Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
# Under MIT.
# https://github.com/kekyo/muon-ui

include_guard(GLOBAL)

function(muon_add_plugin_runtime_core target_name)
  get_filename_component(MUON_PLUGIN_RUNTIME_CORE_ROOT
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
  foreach(required_target IN ITEMS
      muon_cardio
      muon_rpc_core
      muon_sha2)
    if(NOT TARGET ${required_target})
      message(FATAL_ERROR
        "${required_target} must exist before adding ${target_name}.")
    endif()
  endforeach()
  if(NOT LIBFFI_TARGET)
    message(FATAL_ERROR
      "LIBFFI_TARGET must name an existing target before adding ${target_name}.")
  endif()
  if(NOT TARGET ${LIBFFI_TARGET})
    message(FATAL_ERROR
      "LIBFFI_TARGET must name an existing target before adding ${target_name}.")
  endif()
  if(NOT EXISTS "${TRA_FFIC_ROOT_RESOLVED}/include/tra_ffic.h")
    message(FATAL_ERROR
      "TRA_FFIC_ROOT_RESOLVED must contain include/tra_ffic.h.")
  endif()
  if(NOT EXISTS "${CARDIO_ROOT_RESOLVED}/include/cardio.h")
    message(FATAL_ERROR
      "CARDIO_ROOT_RESOLVED must contain include/cardio.h.")
  endif()

  add_library(${target_name} STATIC
    "${MUON_PLUGIN_RUNTIME_CORE_ROOT}/src/muon_sha256.cpp"
    "${MUON_PLUGIN_RUNTIME_CORE_ROOT}/src/plugins/muon_function_wrapper_lifecycle.cpp"
    "${MUON_PLUGIN_RUNTIME_CORE_ROOT}/src/plugins/muon_plugin_metadata.cpp"
    "${MUON_PLUGIN_RUNTIME_CORE_ROOT}/src/plugins/muon_plugin_runtime.cpp"
    "${MUON_PLUGIN_RUNTIME_CORE_ROOT}/src/plugins/muon_traffic_type_metadata.cpp"
    )
  set_target_properties(${target_name} PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    )
  target_compile_features(${target_name} PUBLIC cxx_std_20)
  target_include_directories(${target_name} PUBLIC
    "${MUON_PLUGIN_RUNTIME_CORE_ROOT}/include"
    "${MUON_PLUGIN_RUNTIME_CORE_ROOT}/src"
    "${TRA_FFIC_ROOT_RESOLVED}/include"
    "${CARDIO_ROOT_RESOLVED}/include"
    )
  target_link_libraries(${target_name} PUBLIC
    muon_cardio
    muon_rpc_core
    muon_sha2
    ${LIBFFI_TARGET}
    )
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target_name} PRIVATE
      -fexceptions
      )
  elseif(MSVC)
    target_compile_options(${target_name} PRIVATE
      /EHsc
      )
  endif()
  if(MUON_TRACK_FFI_CLOSURES)
    target_compile_definitions(${target_name} PRIVATE
      MUON_TRACK_FFI_CLOSURES
      TRA_FFIC_TRACK_CLOSURES
      )
  endif()
  if(MUON_BUILD_TESTS)
    target_compile_definitions(${target_name} PRIVATE
      MUON_TEST_BUILD
      )
  endif()
  if(COMMAND MUON_APPLY_SANITIZERS)
    MUON_APPLY_SANITIZERS(${target_name})
  endif()
endfunction()
