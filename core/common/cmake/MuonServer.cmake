set(MUON_LLHTTP_SOURCE_DIR "${MUON_COMMON_ROOT}/../cef/.deps/src/llhttp-9.4.3")
if(NOT EXISTS "${MUON_LLHTTP_SOURCE_DIR}/include/llhttp.h")
  message(FATAL_ERROR "llhttp is required. Run build_llhttp.sh first.")
endif()
add_library(muon_llhttp STATIC
  "${MUON_LLHTTP_SOURCE_DIR}/src/api.c"
  "${MUON_LLHTTP_SOURCE_DIR}/src/http.c"
  "${MUON_LLHTTP_SOURCE_DIR}/src/llhttp.c")
set_target_properties(muon_llhttp PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(muon_llhttp PRIVATE c_std_99)
target_include_directories(muon_llhttp PUBLIC "${MUON_LLHTTP_SOURCE_DIR}/include")

set(MUON_WSLAY_SOURCE_DIR "${MUON_COMMON_ROOT}/../cef/.deps/src/wslay-1.1.1")
if(NOT EXISTS "${MUON_WSLAY_SOURCE_DIR}/lib/includes/wslay/wslay.h")
  message(FATAL_ERROR "Wslay is required. Run build_wslay.sh first.")
endif()
add_library(muon_wslay STATIC
  "${MUON_WSLAY_SOURCE_DIR}/lib/wslay_event.c"
  "${MUON_WSLAY_SOURCE_DIR}/lib/wslay_frame.c"
  "${MUON_WSLAY_SOURCE_DIR}/lib/wslay_net.c"
  "${MUON_WSLAY_SOURCE_DIR}/lib/wslay_queue.c"
  "${MUON_WSLAY_SOURCE_DIR}/lib/wslay_stack.c")
set_target_properties(muon_wslay PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(muon_wslay PRIVATE c_std_99)
target_include_directories(muon_wslay PUBLIC "${MUON_WSLAY_SOURCE_DIR}/lib/includes")
# The public header explicitly supports this definition instead of generated
# wslayver.h; upstream sources and their obsolete CMake files remain untouched.
target_compile_definitions(muon_wslay PUBLIC WSLAY_VERSION="1.1.1")
if(WIN32)
  target_compile_definitions(muon_wslay PRIVATE HAVE_WINSOCK2_H)
  target_link_libraries(muon_wslay PUBLIC ws2_32)
else()
  target_compile_definitions(muon_wslay PRIVATE HAVE_ARPA_INET_H HAVE_NETINET_IN_H)
endif()
if(CMAKE_C_BYTE_ORDER STREQUAL "BIG_ENDIAN")
  target_compile_definitions(muon_wslay PRIVATE WORDS_BIGENDIAN)
endif()

set(MUON_SERVER_SOURCES
  "${MUON_COMMON_ROOT}/src/server/muon_server_http.cpp"
  "${MUON_COMMON_ROOT}/src/server/muon_server_service.cpp"
  "${MUON_COMMON_ROOT}/src/server/muon_server_socket.cpp"
  "${MUON_COMMON_ROOT}/src/server/muon_server_websocket.cpp"
)
