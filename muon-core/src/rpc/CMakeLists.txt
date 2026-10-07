if(TARGET muon_rpc_core)
  return()
endif()

get_filename_component(MUON_RPC_CORE_ROOT
  "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

add_library(muon_rpc_core STATIC
  "${MUON_RPC_CORE_ROOT}/src/config/muon_glob.cpp"
  "${MUON_RPC_CORE_ROOT}/src/config/muon_glob.h"
  "${MUON_RPC_CORE_ROOT}/src/plugins/muon_plugin_policy.cpp"
  "${MUON_RPC_CORE_ROOT}/src/plugins/muon_plugin_policy.h"
  "${MUON_RPC_CORE_ROOT}/src/plugins/muon_type_metadata.cpp"
  "${MUON_RPC_CORE_ROOT}/src/plugins/muon_type_metadata.h"
  "${MUON_RPC_CORE_ROOT}/src/rpc/muon_rpc.cpp"
  "${MUON_RPC_CORE_ROOT}/src/rpc/muon_rpc.h"
  "${MUON_RPC_CORE_ROOT}/src/rpc/muon_rpc_host.cpp"
  "${MUON_RPC_CORE_ROOT}/src/rpc/muon_rpc_host.h"
  "${MUON_RPC_CORE_ROOT}/include/muon_plugin_api.h"
  )
set_target_properties(muon_rpc_core PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  )
target_compile_features(muon_rpc_core PUBLIC cxx_std_20)
target_include_directories(muon_rpc_core PUBLIC
  "${MUON_RPC_CORE_ROOT}/include"
  "${MUON_RPC_CORE_ROOT}/src"
  )
