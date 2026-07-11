cmake_minimum_required(VERSION 3.21)

project(muon LANGUAGES C CXX)

option(MUON_ENABLE_SANITIZERS
  "Build local Linux targets with AddressSanitizer, LeakSanitizer, and UBSan."
  OFF)
option(MUON_TRACK_FFI_CLOSURES
  "Track libffi closure allocation and free counts in the muon runtime."
  OFF)
option(MUON_BUILD_TESTS
  "Build muon test plugins and local executable tests."
  ON)
option(MUON_RUNTIME_INCLUDE_APP_FILES
  "Copy development app config and assets into the runtime payload."
  ON)
set(MUON_CONFIG_TEMPLATE "" CACHE FILEPATH
  "Path to the muon.json template copied into the runtime directory.")
set(MUON_CONFIG_COPY_MODE "always" CACHE STRING
  "How to copy MUON_CONFIG_TEMPLATE into the runtime directory.")
set_property(CACHE MUON_CONFIG_COPY_MODE PROPERTY STRINGS always if_missing none)
set(MUON_CEF_API_VERSION "" CACHE STRING
  "CEF C API version used by muon-core and libcef_dll_wrapper.")
set(MUON_CEF_VERSION "" CACHE STRING
  "CEF version used to generate muon runtime metadata.")
set(MUON_CEF_PACKAGE "" CACHE STRING
  "CEF binary package file name used to generate muon runtime metadata.")
set(MUON_CEF_PACKAGE_URL "" CACHE STRING
  "CEF binary package URL used to generate muon runtime metadata.")
set(MUON_CEF_SHA1 "" CACHE STRING
  "CEF binary package SHA1 used to generate muon runtime metadata.")
set(MUON_CEF_SIZE "0" CACHE STRING
  "CEF binary package size used to generate muon runtime metadata.")
set(MUON_TARGET_NAME "" CACHE STRING
  "Public muon runtime target name used to generate runtime metadata.")
set(MUON_CEF_TARGET_NAME "" CACHE STRING
  "Internal CEF target name used to generate runtime metadata.")
set(MUON_CORE_VERSION "" CACHE STRING
  "muon-core version used to generate runtime metadata.")
set(MUON_CORE_GIT_COMMIT_HASH "" CACHE STRING
  "muon-core git commit hash used to generate runtime metadata.")
set(MUON_CORE_BUILD_DATE "" CACHE STRING
  "muon-core build date used to generate runtime metadata.")
set(MUON_CORE_GIT_COMMIT_DATE "" CACHE STRING
  "muon-core git commit date used to generate runtime metadata.")
set(MUON_CORE_VERSION_HEADER "" CACHE FILEPATH
  "Generated muon-core version header used to generate runtime metadata.")
set(TRA_FFIC_ROOT "../deps/tra-ffic" CACHE PATH
  "Path to the tra-ffic checkout.")
set(CARDIO_ROOT "../deps/cardio" CACHE PATH
  "Path to the cardio checkout.")

set(CMAKE_CONFIGURATION_TYPES Debug Release)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release)

if(NOT CEF_ROOT)
  message(FATAL_ERROR "Set CEF_ROOT to the extracted CEF binary distribution.")
endif()
get_filename_component(TRA_FFIC_ROOT_RESOLVED
  "${TRA_FFIC_ROOT}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
get_filename_component(CARDIO_ROOT_RESOLVED
  "${CARDIO_ROOT}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
if(NOT EXISTS "${TRA_FFIC_ROOT_RESOLVED}/include/tra_ffic.h")
  message(FATAL_ERROR
    "tra-ffic is required. Set TRA_FFIC_ROOT to a checkout containing include/tra_ffic.h.")
endif()
if(NOT EXISTS "${CARDIO_ROOT_RESOLVED}/include/cardio.h")
  message(FATAL_ERROR
    "cardio is required. Set CARDIO_ROOT to a checkout containing include/cardio.h.")
endif()

set(APP_NAME muon-core)
if(NOT MUON_CORE_VERSION)
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/package.json"
    MUON_CORE_PACKAGE_JSON)
  string(JSON MUON_CORE_VERSION GET
    "${MUON_CORE_PACKAGE_JSON}" version)
endif()
if(NOT MUON_CORE_GIT_COMMIT_HASH)
  execute_process(
    COMMAND git rev-parse --short=20 HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RESULT_VARIABLE MUON_CORE_GIT_RESULT
    OUTPUT_VARIABLE MUON_CORE_GIT_OUTPUT
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE
    )
  if(MUON_CORE_GIT_RESULT EQUAL 0 AND NOT MUON_CORE_GIT_OUTPUT STREQUAL "")
    set(MUON_CORE_GIT_COMMIT_HASH "${MUON_CORE_GIT_OUTPUT}")
  else()
    set(MUON_CORE_GIT_COMMIT_HASH "unknown")
  endif()
endif()
if(NOT MUON_CORE_BUILD_DATE)
  string(TIMESTAMP MUON_CORE_BUILD_DATE "%Y-%m-%dT%H:%M:%SZ" UTC)
endif()
if(NOT MUON_CORE_GIT_COMMIT_DATE)
  execute_process(
    COMMAND git show -s --format=%cI HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RESULT_VARIABLE MUON_CORE_GIT_DATE_RESULT
    OUTPUT_VARIABLE MUON_CORE_GIT_DATE_OUTPUT
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE
    )
  if(MUON_CORE_GIT_DATE_RESULT EQUAL 0 AND
      NOT MUON_CORE_GIT_DATE_OUTPUT STREQUAL "")
    set(MUON_CORE_GIT_COMMIT_DATE "${MUON_CORE_GIT_DATE_OUTPUT}")
  else()
    set(MUON_CORE_GIT_COMMIT_DATE "unknown")
  endif()
endif()
if(NOT MUON_RUNTIME_DIR)
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(MUON_RUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.run/dev-debug" CACHE PATH
      "Runtime output directory")
  else()
    set(MUON_RUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.run/dev-release" CACHE PATH
      "Runtime output directory")
  endif()
endif()
if(NOT MUON_CONFIG_TEMPLATE)
  if(WIN32)
    set(MUON_CONFIG_TEMPLATE "${CMAKE_CURRENT_SOURCE_DIR}/config/muon.dev.json"
      CACHE FILEPATH "Path to the muon.json template copied into the runtime directory." FORCE)
  else()
    set(MUON_CONFIG_TEMPLATE "${CMAKE_CURRENT_SOURCE_DIR}/config/muon.dev.linux.json"
      CACHE FILEPATH "Path to the muon.json template copied into the runtime directory." FORCE)
  endif()
endif()
if(NOT MUON_CONFIG_COPY_MODE MATCHES "^(always|if_missing|none)$")
  message(FATAL_ERROR
    "MUON_CONFIG_COPY_MODE must be one of: always, if_missing, none.")
endif()
if(NOT MUON_CONFIG_COPY_MODE STREQUAL "none" AND
    NOT EXISTS "${MUON_CONFIG_TEMPLATE}")
  message(FATAL_ERROR
    "MUON_CONFIG_TEMPLATE does not exist: ${MUON_CONFIG_TEMPLATE}")
endif()
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CEF_ROOT}/cmake")

if(MINGW)
  # CEF enables ATL by default for Windows, but MinGW builds do not use VC ATL.
  set(USE_ATL OFF CACHE BOOL "Enable or disable use of ATL." FORCE)
endif()

find_package(CEF REQUIRED)
find_package(Threads REQUIRED)

if(NOT MUON_CEF_API_VERSION)
  file(STRINGS "${CEF_ROOT}/include/cef_api_versions.h"
    MUON_CEF_API_VERSION_LAST_LINES
    REGEX "^#define CEF_API_VERSION_LAST CEF_API_VERSION_[0-9]+$")
  list(GET MUON_CEF_API_VERSION_LAST_LINES 0
    MUON_CEF_API_VERSION_LAST_LINE)
  string(REGEX REPLACE ".*CEF_API_VERSION_([0-9]+).*" "\\1"
    MUON_CEF_API_VERSION "${MUON_CEF_API_VERSION_LAST_LINE}")
  if(NOT MUON_CEF_API_VERSION MATCHES "^[0-9]+$")
    message(FATAL_ERROR
      "Unable to resolve CEF_API_VERSION_LAST from ${CEF_ROOT}/include/cef_api_versions.h")
  endif()
  set(MUON_CEF_API_VERSION "${MUON_CEF_API_VERSION}" CACHE STRING
    "CEF C API version used by muon-core and libcef_dll_wrapper." FORCE)
endif()

add_compile_definitions(CEF_API_VERSION=${MUON_CEF_API_VERSION})

if(NOT MUON_TARGET_NAME)
  if(WIN32)
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
      set(MUON_TARGET_NAME "windows-i686")
    else()
      set(MUON_TARGET_NAME "windows-amd64")
    endif()
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
    set(MUON_TARGET_NAME "linux-amd64")
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(MUON_TARGET_NAME "linux-arm64")
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
    set(MUON_TARGET_NAME "linux-armhf")
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "i.86|x86")
    message(FATAL_ERROR
      "Linux 32-bit CEF builds are discontinued after CEF 101 and are not supported.")
  else()
    set(MUON_TARGET_NAME "linux-amd64")
  endif()
endif()
if(NOT MUON_CEF_TARGET_NAME)
  if(MUON_TARGET_NAME STREQUAL "linux-amd64")
    set(MUON_CEF_TARGET_NAME "linux64")
  elseif(MUON_TARGET_NAME STREQUAL "linux-armhf")
    set(MUON_CEF_TARGET_NAME "linuxarm")
  elseif(MUON_TARGET_NAME STREQUAL "linux-arm64")
    set(MUON_CEF_TARGET_NAME "linuxarm64")
  elseif(MUON_TARGET_NAME STREQUAL "windows-i686")
    set(MUON_CEF_TARGET_NAME "windows32")
  elseif(MUON_TARGET_NAME STREQUAL "windows-amd64")
    set(MUON_CEF_TARGET_NAME "windows64")
  else()
    message(FATAL_ERROR
      "Unsupported MUON_TARGET_NAME: ${MUON_TARGET_NAME}")
  endif()
endif()
if(NOT MUON_CEF_VERSION)
  get_filename_component(MUON_CEF_ROOT_NAME "${CEF_ROOT}" NAME)
  if(MUON_CEF_ROOT_NAME MATCHES "^cef_binary_(.+)_([^_]+.*)$")
    set(MUON_CEF_VERSION "${CMAKE_MATCH_1}")
  endif()
endif()
if(NOT MUON_CEF_PACKAGE AND MUON_CEF_VERSION)
  set(MUON_CEF_PACKAGE "cef_binary_${MUON_CEF_VERSION}_${MUON_CEF_TARGET_NAME}_minimal.tar.bz2")
endif()
if(NOT MUON_CEF_PACKAGE_URL AND MUON_CEF_PACKAGE)
  string(REPLACE "+" "%2B" MUON_CEF_PACKAGE_URL_ESCAPED "${MUON_CEF_PACKAGE}")
  set(MUON_CEF_PACKAGE_URL
    "https://cef-builds.spotifycdn.com/${MUON_CEF_PACKAGE_URL_ESCAPED}")
endif()
file(STRINGS "${CEF_ROOT}/include/cef_api_versions.h" MUON_CEF_API_HASH_LINES
  REGEX "CEF_API_HASH_${MUON_CEF_API_VERSION}")
if(WIN32)
  list(GET MUON_CEF_API_HASH_LINES 0 MUON_CEF_API_HASH_LINE)
elseif(APPLE)
  list(GET MUON_CEF_API_HASH_LINES 1 MUON_CEF_API_HASH_LINE)
else()
  list(GET MUON_CEF_API_HASH_LINES 2 MUON_CEF_API_HASH_LINE)
endif()
string(REGEX REPLACE ".*\"([^\"]+)\".*" "\\1" MUON_CEF_API_HASH
  "${MUON_CEF_API_HASH_LINE}")

set(MUON_CARDIO_GLIB_TARGETS)
if(NOT WIN32)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(MUON_GLIB REQUIRED IMPORTED_TARGET glib-2.0)
  pkg_check_modules(MUON_GIO REQUIRED IMPORTED_TARGET "gio-2.0>=2.74")
  pkg_check_modules(MUON_GTK3 REQUIRED IMPORTED_TARGET "gtk+-3.0")
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    pkg_check_modules(MUON_X11 REQUIRED IMPORTED_TARGET x11)
    pkg_check_modules(MUON_XI REQUIRED IMPORTED_TARGET xi)
  endif()
  set(MUON_CARDIO_GLIB_TARGETS
    PkgConfig::MUON_GLIB
    PkgConfig::MUON_GIO
    )
endif()

set(MUON_NATIVE_WHEEL_TARGETS)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(MUON_NATIVE_WHEEL_TARGETS
    PkgConfig::MUON_X11
    PkgConfig::MUON_XI
    )
endif()

if(MUON_ENABLE_SANITIZERS)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
      NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR
      "MUON_ENABLE_SANITIZERS is supported only on Linux with GCC or Clang.")
  endif()
endif()

function(MUON_APPLY_SANITIZERS TARGET_NAME)
  if(MUON_ENABLE_SANITIZERS)
    get_target_property(MUON_SANITIZER_TARGET_TYPE ${TARGET_NAME} TYPE)
    target_compile_options(${TARGET_NAME} PRIVATE
      -fsanitize=address,leak,undefined
      -fno-omit-frame-pointer
      )
    target_link_options(${TARGET_NAME} PRIVATE
      -fsanitize=address,leak,undefined
      -fno-omit-frame-pointer
      )
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
        MUON_SANITIZER_TARGET_TYPE STREQUAL "EXECUTABLE")
      target_link_options(${TARGET_NAME} PRIVATE
        -static-libasan
        -static-libubsan
        )
    endif()
  endif()
endfunction()

if(LIBFFI_ROOT)
  unset(LIBFFI_INCLUDE_DIR CACHE)
  unset(LIBFFI_LIBRARY CACHE)
  find_path(LIBFFI_INCLUDE_DIR ffi.h
    HINTS
      "${LIBFFI_ROOT}/include"
      "${LIBFFI_ROOT}/lib/libffi/include"
    NO_DEFAULT_PATH
    NO_CMAKE_FIND_ROOT_PATH
    )
  find_library(LIBFFI_LIBRARY
    NAMES ffi libffi
    HINTS "${LIBFFI_ROOT}/lib"
    NO_DEFAULT_PATH
    NO_CMAKE_FIND_ROOT_PATH
    )
  if(NOT LIBFFI_INCLUDE_DIR OR NOT LIBFFI_LIBRARY)
    message(FATAL_ERROR
      "libffi is required. Install libffi development files or set LIBFFI_ROOT.")
  endif()
  add_library(libffi_lib UNKNOWN IMPORTED)
  set_target_properties(libffi_lib PROPERTIES
    IMPORTED_LOCATION "${LIBFFI_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${LIBFFI_INCLUDE_DIR}"
    )
  set(LIBFFI_TARGET libffi_lib)
else()
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(LIBFFI QUIET IMPORTED_TARGET libffi)
  endif()
  if(LIBFFI_FOUND)
    set(LIBFFI_TARGET PkgConfig::LIBFFI)
  else()
    find_path(LIBFFI_INCLUDE_DIR ffi.h)
    find_library(LIBFFI_LIBRARY NAMES ffi libffi)
    if(NOT LIBFFI_INCLUDE_DIR OR NOT LIBFFI_LIBRARY)
      message(FATAL_ERROR
        "libffi is required. Install libffi development files or set LIBFFI_ROOT.")
    endif()
    add_library(libffi_lib UNKNOWN IMPORTED)
    set_target_properties(libffi_lib PROPERTIES
      IMPORTED_LOCATION "${LIBFFI_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${LIBFFI_INCLUDE_DIR}"
      )
    set(LIBFFI_TARGET libffi_lib)
  endif()
endif()

if(MINGW)
  if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(MINGW_ARCH_FLAGS -m32)
    set(MINGW_STACK_FLAG -Wl,--stack,0x80000)
  else()
    set(MINGW_ARCH_FLAGS -m64)
    set(MINGW_STACK_FLAG -Wl,--stack,0x800000)
  endif()

  set(CEF_COMPILER_FLAGS
    ${MINGW_ARCH_FLAGS}
    -include cstdint
    -fno-strict-aliasing
    -pipe
    -Wall
    -Wextra
    -Wno-missing-field-initializers
    -Wno-unused-parameter
    -Wno-deprecated-declarations
    -Wno-comment
    -Wno-narrowing
    -Wno-literal-suffix
    -Wno-attributes
    -Wno-array-bounds
    -Wno-stringop-overflow
    -Wno-unknown-pragmas
    -Wno-cast-function-type
    )
  if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    list(APPEND CEF_COMPILER_FLAGS
      -mms-bitfields
      )
  endif()
  set(CEF_C_COMPILER_FLAGS
    -std=c99
    )
  set(CEF_CXX_COMPILER_FLAGS
    -std=c++20
    -Wsign-compare
    )
  set(CEF_COMPILER_FLAGS_DEBUG
    -O0
    -g3
    )
  set(CEF_C_COMPILER_FLAGS_DEBUG)
  set(CEF_CXX_COMPILER_FLAGS_DEBUG)
  set(CEF_COMPILER_FLAGS_RELEASE
    -O3
    -fdata-sections
    -fno-ident
    -U_FORTIFY_SOURCE
    -D_FORTIFY_SOURCE=2
    )
  # i686-w64-mingw32 emits section type conflicts for CEF generated scoped
  # callback templates when function sections are enabled.
  if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    list(APPEND CEF_COMPILER_FLAGS_RELEASE
      -ffunction-sections
      )
  endif()
  set(CEF_C_COMPILER_FLAGS_RELEASE)
  set(CEF_CXX_COMPILER_FLAGS_RELEASE)
  set(CEF_COMPILER_DEFINES
    __STDC_CONSTANT_MACROS
    __STDC_FORMAT_MACROS
    WIN32
    _WIN32
    _WINDOWS
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    NTDDI_VERSION=0x0A00000A
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    )
  set(CEF_COMPILER_DEFINES_DEBUG)
  set(CEF_COMPILER_DEFINES_RELEASE
    NDEBUG
    _NDEBUG
    )
  set(CEF_LINKER_FLAGS)
  set(CEF_LINKER_FLAGS_DEBUG)
  set(CEF_LINKER_FLAGS_RELEASE)
  set(CEF_EXE_LINKER_FLAGS
    ${MINGW_ARCH_FLAGS}
    -mwindows
    -static-libgcc
    -static-libstdc++
    -Wl,--gc-sections
    ${MINGW_STACK_FLAG}
    )
  set(CEF_EXE_LINKER_FLAGS_DEBUG)
  set(CEF_EXE_LINKER_FLAGS_RELEASE)
  set(CEF_SHARED_LINKER_FLAGS)
  set(CEF_SHARED_LINKER_FLAGS_DEBUG)
  set(CEF_SHARED_LINKER_FLAGS_RELEASE)
  set(CEF_STANDARD_LIBS
    comctl32
    crypt32
    delayimp
    gdi32
    rpcrt4
    shlwapi
    user32
    wintrust
    ws2_32
    )
  list(APPEND CEF_BINARY_FILES
    bootstrap.exe
    bootstrapc.exe
    )

  set(MUON_MINGW_RUNTIME_DLL_NAMES
    libgcc_s_seh-1.dll
    libgcc_s_dw2-1.dll
    libgcc_s_sjlj-1.dll
    libstdc++-6.dll
    libwinpthread-1.dll
    )
  set(MUON_MINGW_RUNTIME_DLLS)
  foreach(MUON_MINGW_RUNTIME_DLL_NAME IN LISTS MUON_MINGW_RUNTIME_DLL_NAMES)
    execute_process(
      COMMAND "${CMAKE_CXX_COMPILER}"
        "-print-file-name=${MUON_MINGW_RUNTIME_DLL_NAME}"
      RESULT_VARIABLE MUON_MINGW_RUNTIME_DLL_RESULT
      OUTPUT_VARIABLE MUON_MINGW_RUNTIME_DLL_PATH
      ERROR_QUIET
      OUTPUT_STRIP_TRAILING_WHITESPACE
      )
    if(MUON_MINGW_RUNTIME_DLL_RESULT EQUAL 0 AND
        IS_ABSOLUTE "${MUON_MINGW_RUNTIME_DLL_PATH}" AND
        EXISTS "${MUON_MINGW_RUNTIME_DLL_PATH}")
      get_filename_component(MUON_MINGW_RUNTIME_DLL_PATH
        "${MUON_MINGW_RUNTIME_DLL_PATH}" ABSOLUTE)
      list(APPEND MUON_MINGW_RUNTIME_DLLS
        "${MUON_MINGW_RUNTIME_DLL_NAME}=${MUON_MINGW_RUNTIME_DLL_PATH}")
    endif()
  endforeach()
  list(JOIN MUON_MINGW_RUNTIME_DLLS "|" MUON_MINGW_RUNTIME_DLLS_ARG)
endif()

# CEF defaults disable C++ exceptions, but muon builds with exceptions enabled.
foreach(MUON_CEF_FLAG_LIST IN ITEMS
    CEF_COMPILER_FLAGS
    CEF_CXX_COMPILER_FLAGS
    CEF_COMPILER_FLAGS_DEBUG
    CEF_CXX_COMPILER_FLAGS_DEBUG
    CEF_COMPILER_FLAGS_RELEASE
    CEF_CXX_COMPILER_FLAGS_RELEASE)
  if(DEFINED ${MUON_CEF_FLAG_LIST})
    list(REMOVE_ITEM ${MUON_CEF_FLAG_LIST}
      -fno-exceptions
      )
  endif()
endforeach()
foreach(MUON_CEF_DEFINE_LIST IN ITEMS
    CEF_COMPILER_DEFINES
    CEF_COMPILER_DEFINES_DEBUG
    CEF_COMPILER_DEFINES_RELEASE)
  if(DEFINED ${MUON_CEF_DEFINE_LIST})
    list(REMOVE_ITEM ${MUON_CEF_DEFINE_LIST}
      _HAS_EXCEPTIONS=0
      )
  endif()
endforeach()

set(CEF_LIB_DEBUG_EFFECTIVE "${CEF_LIB_DEBUG}")
set(CEF_BINARY_DIR_EFFECTIVE "${CEF_BINARY_DIR}")
if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT EXISTS "${CEF_LIB_DEBUG}")
  set(CEF_LIB_DEBUG_EFFECTIVE "${CEF_LIB_RELEASE}")
  set(CEF_BINARY_DIR_EFFECTIVE "${CEF_BINARY_DIR_RELEASE}")
endif()

ADD_LOGICAL_TARGET(libcef_lib "${CEF_LIB_DEBUG_EFFECTIVE}" "${CEF_LIB_RELEASE}")
add_subdirectory("${CEF_LIBCEF_DLL_WRAPPER_PATH}" libcef_dll_wrapper)
target_compile_definitions(libcef_dll_wrapper PUBLIC
  CEF_API_VERSION=${MUON_CEF_API_VERSION}
  )
if(MINGW)
  get_target_property(LIBCEF_DLL_WRAPPER_SOURCES libcef_dll_wrapper SOURCES)
  foreach(WRAPPER_EXCLUDE_SOURCE IN ITEMS
      wrapper/cef_certificate_util_win.cc
      wrapper/cef_scoped_library_loader_win.cc
      )
    list(FILTER LIBCEF_DLL_WRAPPER_SOURCES EXCLUDE REGEX
      "(^|/)${WRAPPER_EXCLUDE_SOURCE}$")
  endforeach()
  set_target_properties(libcef_dll_wrapper PROPERTIES
    SOURCES "${LIBCEF_DLL_WRAPPER_SOURCES}"
    )
endif()

if(NOT YYJSON_SOURCE_DIR)
  set(YYJSON_SOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/.deps/src/yyjson-0.12.0/src"
    CACHE PATH "Path to yyjson src directory containing yyjson.c and yyjson.h.")
endif()
if(NOT EXISTS "${YYJSON_SOURCE_DIR}/yyjson.c" OR
    NOT EXISTS "${YYJSON_SOURCE_DIR}/yyjson.h")
  message(FATAL_ERROR
    "yyjson source is required. Run build.sh to download yyjson or set YYJSON_SOURCE_DIR.")
endif()

add_library(yyjson STATIC
  "${YYJSON_SOURCE_DIR}/yyjson.c"
  "${YYJSON_SOURCE_DIR}/yyjson.h"
  )
set_target_properties(yyjson PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  )
target_compile_features(yyjson PRIVATE c_std_99)
target_include_directories(yyjson PUBLIC
  "${YYJSON_SOURCE_DIR}"
  )
MUON_APPLY_SANITIZERS(yyjson)

if(NOT MINIZ_SOURCE_DIR)
  set(MINIZ_SOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/.deps/src/miniz-3.1.1"
    CACHE PATH "Path to miniz source directory containing miniz.c and miniz.h.")
endif()
if(NOT EXISTS "${MINIZ_SOURCE_DIR}/miniz.c" OR
    NOT EXISTS "${MINIZ_SOURCE_DIR}/miniz_zip.c" OR
    NOT EXISTS "${MINIZ_SOURCE_DIR}/miniz_tinfl.c" OR
    NOT EXISTS "${MINIZ_SOURCE_DIR}/miniz_tdef.c" OR
    NOT EXISTS "${MINIZ_SOURCE_DIR}/miniz.h")
  message(FATAL_ERROR
    "miniz source is required. Run build.sh to download miniz or set MINIZ_SOURCE_DIR.")
endif()
set(MINIZ_GENERATED_INCLUDE_DIR
  "${CMAKE_CURRENT_BINARY_DIR}/miniz-generated")
file(MAKE_DIRECTORY "${MINIZ_GENERATED_INCLUDE_DIR}")
file(WRITE "${MINIZ_GENERATED_INCLUDE_DIR}/miniz_export.h"
  "#ifndef MINIZ_EXPORT\n#define MINIZ_EXPORT\n#endif\n")

add_library(miniz STATIC
  "${MINIZ_SOURCE_DIR}/miniz.c"
  "${MINIZ_SOURCE_DIR}/miniz_zip.c"
  "${MINIZ_SOURCE_DIR}/miniz_tinfl.c"
  "${MINIZ_SOURCE_DIR}/miniz_tdef.c"
  "${MINIZ_SOURCE_DIR}/miniz.h"
  "${MINIZ_SOURCE_DIR}/miniz_zip.h"
  "${MINIZ_SOURCE_DIR}/miniz_tinfl.h"
  "${MINIZ_SOURCE_DIR}/miniz_tdef.h"
  "${MINIZ_SOURCE_DIR}/miniz_common.h"
  )
set_target_properties(miniz PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  )
target_compile_features(miniz PRIVATE c_std_99)
target_compile_definitions(miniz PUBLIC
  MINIZ_NO_ZLIB_COMPATIBLE_NAMES
  )
target_include_directories(miniz PUBLIC
  "${MINIZ_GENERATED_INCLUDE_DIR}"
  "${MINIZ_SOURCE_DIR}"
  )
MUON_APPLY_SANITIZERS(miniz)

set(MUON_CARDIO_SHARED_SOURCE
  "${CARDIO_ROOT_RESOLVED}/samples/libcardio/libcardio.cpp")
if(NOT EXISTS "${MUON_CARDIO_SHARED_SOURCE}")
  message(FATAL_ERROR
    "cardio shared runtime source is required. Update CARDIO_ROOT to a checkout containing samples/libcardio/libcardio.cpp.")
endif()
string(TOLOWER "${CMAKE_BUILD_TYPE}" MUON_BUILD_TYPE_LOWER)
set(MUON_CARDIO_OUTPUT_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/.deps/libcardio/${MUON_TARGET_NAME}/${MUON_BUILD_TYPE_LOWER}")
set(MUON_CARDIO_RUNTIME_LIBRARY_NAME
  "libcardio${CMAKE_SHARED_LIBRARY_SUFFIX}")
set(MUON_CARDIO_CONFIG_DEFINES
  CARDIO_WITH_LINUX_IO_URING=0
  CARDIO_WITH_SUPPLEMENTAL=1
  )
if(NOT WIN32)
  list(APPEND MUON_CARDIO_CONFIG_DEFINES
    CARDIO_HAS_POSIX_FD=1
    )
endif()
if(MUON_CARDIO_GLIB_TARGETS)
  list(APPEND MUON_CARDIO_CONFIG_DEFINES
    CARDIO_WITH_GLIB=1
    CARDIO_WITH_GIO=1
    )
endif()

add_library(cardio_shared SHARED
  "${MUON_CARDIO_SHARED_SOURCE}"
  "${CARDIO_ROOT_RESOLVED}/include/cardio.h"
  )
set_target_properties(cardio_shared PROPERTIES
  OUTPUT_NAME "cardio"
  PREFIX "lib"
  LIBRARY_OUTPUT_DIRECTORY "${MUON_CARDIO_OUTPUT_DIR}"
  RUNTIME_OUTPUT_DIRECTORY "${MUON_CARDIO_OUTPUT_DIR}"
  ARCHIVE_OUTPUT_DIRECTORY "${MUON_CARDIO_OUTPUT_DIR}"
  )
target_compile_features(cardio_shared PRIVATE cxx_std_20)
target_compile_definitions(cardio_shared PRIVATE
  CARDIO_SHARED_LIB=1
  CARDIO_BUILD_SHARED_LIB=1
  ${MUON_CARDIO_CONFIG_DEFINES}
  )
target_include_directories(cardio_shared PRIVATE
  "${CARDIO_ROOT_RESOLVED}/include"
  )
target_link_libraries(cardio_shared PRIVATE
  Threads::Threads
  ${MUON_CARDIO_GLIB_TARGETS}
  )
if(MINGW)
  target_link_options(cardio_shared PRIVATE
    -static-libgcc
    -static-libstdc++
    )
endif()
MUON_APPLY_SANITIZERS(cardio_shared)

add_library(muon_cardio INTERFACE)
target_compile_definitions(muon_cardio INTERFACE
  CARDIO_SHARED_LIB=1
  ${MUON_CARDIO_CONFIG_DEFINES}
  )
target_include_directories(muon_cardio INTERFACE
  "${CARDIO_ROOT_RESOLVED}/include"
  )
target_link_libraries(muon_cardio INTERFACE
  cardio_shared
  Threads::Threads
  ${MUON_CARDIO_GLIB_TARGETS}
  )

set(APP_SOURCES
  src/main.cpp
  src/muon_json_helpers.cpp
  src/muon_sha1.cpp
  src/muon_string_helpers.cpp
  src/app/muon_app.cpp
  src/app/muon_app_scheme.cpp
  src/app/muon_app_storage.cpp
  src/browser/muon_browser_background_color.cpp
  src/browser/muon_browser_view_delegate.cpp
  src/browser/muon_builtin_browser.cpp
  src/browser/muon_client.cpp
  src/browser/muon_context_menu.cpp
  src/browser/muon_default_title_bar_icon.cpp
  src/browser/muon_icon.cpp
  src/browser/muon_native_wheel_forwarder.cpp
  src/browser/muon_tray.cpp
  src/browser/muon_tray_service.cpp
  src/browser/muon_title_bar.cpp
  src/browser/muon_title_bar_loader.cpp
  src/browser/muon_window_delegate.cpp
  src/browser/muon_window_state.cpp
  src/browser/muon_window_title.cpp
  src/browser/show_dev_tools_task.cpp
  src/config/muon_autostart.cpp
  src/config/muon_cef_settings.cpp
  src/config/muon_config.cpp
  src/config/muon_glob.cpp
  src/config/muon_linux_display_backend.cpp
  src/config/muon_paths.cpp
  src/config/muon_startup.cpp
  src/log/muon_log.cpp
  src/log/muon_close_debug_log.cpp
  src/network/muon_network_policy.cpp
  src/network/muon_network_request_handler.cpp
  src/plugins/builtin/muon_builtin.cpp
  src/plugins/builtin/muon_builtin_bootstrap.cpp
  src/plugins/builtin/muon_builtin_environment_helpers.cpp
  src/plugins/builtin/muon_builtin_environments.cpp
  src/plugins/builtin/muon_builtin_executor.cpp
  src/plugins/builtin/muon_builtin_fs.cpp
  src/plugins/builtin/muon_builtin_fs_dialogs_plugin.cpp
  src/plugins/builtin/muon_builtin_fs_helpers.cpp
  src/plugins/muon_function_wrapper_lifecycle.cpp
  src/plugins/muon_plugin_metadata.cpp
  src/plugins/muon_plugin_policy.cpp
  src/plugins/muon_plugin_runtime.cpp
  src/plugins/muon_shared_buffer.cpp
  src/plugins/muon_traffic_cardio_operation.cpp
  src/plugins/muon_type_metadata.cpp
  src/plugins/muon_v8_handler.cpp
  )
set(APP_HEADERS
  src/muon_json_helpers.h
  src/muon_sha1.h
  src/muon_string_helpers.h
  src/app/muon_app.h
  src/app/muon_app_scheme.h
  src/app/muon_app_storage.h
  src/browser/muon_browser_background_color.h
  src/browser/muon_browser_view_delegate.h
  src/browser/muon_builtin_browser.h
  src/browser/muon_client.h
  src/browser/muon_context_menu.h
  src/browser/muon_default_title_bar_icon.h
  src/browser/muon_icon.h
  src/browser/muon_native_wheel_forwarder.h
  src/browser/muon_tray.h
  src/browser/muon_title_bar.h
  src/browser/muon_title_bar_loader.h
  src/browser/muon_window_delegate.h
  src/browser/muon_window_state.h
  src/browser/muon_window_title.h
  src/browser/show_dev_tools_task.h
  src/config/muon_autostart.h
  src/config/muon_cef_settings.h
  src/config/muon_config.h
  src/config/muon_glob.h
  src/config/muon_linux_display_backend.h
  src/config/muon_paths.h
  src/config/muon_startup.h
  src/log/muon_log.h
  src/log/muon_close_debug_log.h
  src/network/muon_network_policy.h
  src/network/muon_network_request_handler.h
  src/plugins/builtin/muon_builtin.h
  src/plugins/builtin/muon_builtin_bootstrap.h
  src/plugins/builtin/muon_builtin_completion.h
  src/plugins/builtin/muon_builtin_environment_helpers.h
  src/plugins/builtin/muon_builtin_environments.h
  src/plugins/builtin/muon_builtin_executor.h
  src/plugins/builtin/muon_builtin_fs.h
  src/plugins/builtin/muon_builtin_fs_dialogs_plugin.h
  src/plugins/builtin/muon_builtin_fs_helpers.h
  src/plugins/muon_js_bridge.h
  src/plugins/muon_function_wrapper_lifecycle.h
  src/plugins/muon_plugin_metadata.h
  src/plugins/muon_plugin_policy.h
  src/plugins/muon_plugin_runtime.h
  src/plugins/muon_plugin_value.h
  src/plugins/muon_shared_buffer.h
  src/plugins/muon_traffic_adapter.h
  src/plugins/muon_traffic_cardio_operation.h
  src/plugins/muon_type_metadata.h
  src/plugins/muon_v8_handler.h
  src/ui/muon_ui_fs_dialogs.h
  src/ui/muon_ui_title_bar.h
  include/muon_plugin_api.h
  )

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  set_source_files_properties(
    src/plugins/builtin/muon_builtin_fs.cpp
    src/plugins/builtin/muon_builtin_fs_dialogs.cpp
    src/plugins/builtin/muon_builtin_fs_dialogs_plugin.cpp
    src/plugins/builtin/muon_builtin_fs_helpers.cpp
    src/plugins/muon_function_wrapper_lifecycle.cpp
    src/plugins/muon_traffic_cardio_operation.cpp
    tests/muon_traffic_cardio_operation_test.cpp
    PROPERTIES
    COMPILE_OPTIONS -fexceptions
    )
elseif(MSVC)
  set_source_files_properties(
    src/plugins/builtin/muon_builtin_fs.cpp
    src/plugins/builtin/muon_builtin_fs_dialogs.cpp
    src/plugins/builtin/muon_builtin_fs_dialogs_plugin.cpp
    src/plugins/builtin/muon_builtin_fs_helpers.cpp
    src/plugins/muon_function_wrapper_lifecycle.cpp
    src/plugins/muon_traffic_cardio_operation.cpp
    PROPERTIES
    COMPILE_OPTIONS /EHsc
    )
endif()

add_library(muon_ui SHARED
  src/muon_json_helpers.cpp
  src/muon_json_helpers.h
  src/plugins/builtin/muon_builtin_fs_dialogs.cpp
  src/plugins/builtin/muon_builtin_fs_dialogs.h
  src/plugins/builtin/muon_builtin_fs_helpers.cpp
  src/plugins/builtin/muon_builtin_fs_helpers.h
  src/ui/muon_ui_fs_dialogs.h
  src/ui/muon_ui_title_bar.cpp
  src/ui/muon_ui_title_bar.h
  )
set_target_properties(muon_ui PROPERTIES
  OUTPUT_NAME "muon-ui"
  PREFIX "lib"
  LIBRARY_OUTPUT_DIRECTORY "${MUON_RUNTIME_DIR}"
  RUNTIME_OUTPUT_DIRECTORY "${MUON_RUNTIME_DIR}"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
  )
target_compile_features(muon_ui PRIVATE cxx_std_20)
target_compile_definitions(muon_ui PRIVATE
  MUON_UI_BUILDING_LIBRARY
  )
target_include_directories(muon_ui PRIVATE
  include
  src
  )
target_link_libraries(muon_ui PRIVATE
  muon_cardio
  yyjson
  )
if(WIN32)
  target_link_libraries(muon_ui PRIVATE
    ole32
    shell32
    uuid
    )
else()
  target_link_libraries(muon_ui PRIVATE
    PkgConfig::MUON_GTK3
    )
endif()
MUON_APPLY_SANITIZERS(muon_ui)

set(APP_EXECUTABLE_NAME "${APP_NAME}${CMAKE_EXECUTABLE_SUFFIX}")
set(MUON_LICENSE_FILE_NAME "CREDITS.md")
set(MUON_LEGACY_LICENSE_FILE_NAME "THIRD_PARTY_NOTICES.md")
set(MUON_CORE_PAYLOAD_ITEMS
  "${APP_EXECUTABLE_NAME}"
  "libmuon-ui${CMAKE_SHARED_LIBRARY_SUFFIX}"
  "${MUON_CARDIO_RUNTIME_LIBRARY_NAME}"
  )
if(MUON_RUNTIME_INCLUDE_APP_FILES)
  list(APPEND MUON_CORE_PAYLOAD_ITEMS
    "muon.json"
    "assets"
    )
endif()
list(APPEND MUON_CORE_PAYLOAD_ITEMS
  "${MUON_LICENSE_FILE_NAME}"
  )
list(LENGTH MUON_CORE_PAYLOAD_ITEMS MUON_CORE_PAYLOAD_COUNT)
math(EXPR MUON_CORE_PAYLOAD_LAST_INDEX "${MUON_CORE_PAYLOAD_COUNT} - 1")
set(MUON_CORE_PAYLOAD_C "")
foreach(MUON_CORE_PAYLOAD_INDEX RANGE 0 ${MUON_CORE_PAYLOAD_LAST_INDEX})
  list(GET MUON_CORE_PAYLOAD_ITEMS ${MUON_CORE_PAYLOAD_INDEX}
    MUON_CORE_PAYLOAD_ITEM)
  string(APPEND MUON_CORE_PAYLOAD_C "    \"${MUON_CORE_PAYLOAD_ITEM}\"")
  if(NOT MUON_CORE_PAYLOAD_INDEX EQUAL MUON_CORE_PAYLOAD_LAST_INDEX)
    string(APPEND MUON_CORE_PAYLOAD_C ",\n")
  endif()
endforeach()
set(MUON_GENERATED_INCLUDE_DIR
  "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(MUON_CORE_VERSION_HEADER_PATH
  "${MUON_GENERATED_INCLUDE_DIR}/muon_core_version_generated.h")
set(MUON_RUNTIME_INFO_HEADER_PATH
  "${MUON_GENERATED_INCLUDE_DIR}/muon_runtime_info_generated.h")
set(MUON_DEFAULT_TITLE_BAR_ICON_PNG_PATH
  "${CMAKE_CURRENT_SOURCE_DIR}/../images/muon-256.png")
set(MUON_DEFAULT_TITLE_BAR_ICON_HEADER_PATH
  "${MUON_GENERATED_INCLUDE_DIR}/browser/muon_default_title_bar_icon_generated.h")
file(MAKE_DIRECTORY "${MUON_GENERATED_INCLUDE_DIR}")

function(muon_normalize_windows_version input output_dot output_comma)
  if(NOT "${input}" MATCHES
      "^([0-9]+)(\\.([0-9]+))?(\\.([0-9]+))?(\\.([0-9]+))?")
    message(FATAL_ERROR
      "Windows resource version must start with numeric parts: ${input}")
  endif()
  set(value_0 "${CMAKE_MATCH_1}")
  set(value_1 "${CMAKE_MATCH_3}")
  set(value_2 "${CMAKE_MATCH_5}")
  set(value_3 "${CMAKE_MATCH_7}")
  set(normalized_values)
  foreach(value IN ITEMS
      "${value_0}"
      "${value_1}"
      "${value_2}"
      "${value_3}")
    if("${value}" STREQUAL "")
      set(value 0)
    endif()
    if(value LESS 0 OR value GREATER 65535)
      message(FATAL_ERROR
        "Windows resource version part must be from 0 to 65535: ${input}")
    endif()
    list(APPEND normalized_values "${value}")
  endforeach()
  list(JOIN normalized_values "." normalized_dot)
  list(JOIN normalized_values "," normalized_comma)
  set(${output_dot} "${normalized_dot}" PARENT_SCOPE)
  set(${output_comma} "${normalized_comma}" PARENT_SCOPE)
endfunction()

function(muon_escape_windows_resource_string input output)
  string(REPLACE "\\" "\\\\" escaped "${input}")
  string(REPLACE "\"" "\\\"" escaped "${escaped}")
  set(${output} "${escaped}" PARENT_SCOPE)
endfunction()

add_custom_command(
  OUTPUT "${MUON_DEFAULT_TITLE_BAR_ICON_HEADER_PATH}"
  COMMAND ${CMAKE_COMMAND}
    "-DINPUT=${MUON_DEFAULT_TITLE_BAR_ICON_PNG_PATH}"
    "-DOUTPUT=${MUON_DEFAULT_TITLE_BAR_ICON_HEADER_PATH}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/generate_default_title_bar_icon_header.cmake"
  DEPENDS
    "${MUON_DEFAULT_TITLE_BAR_ICON_PNG_PATH}"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/generate_default_title_bar_icon_header.cmake"
  VERBATIM
  )
if(WIN32)
  muon_normalize_windows_version("${MUON_CORE_VERSION}"
    MUON_WINDOWS_VERSION_DOT
    MUON_WINDOWS_VERSION_COMMA)
  set(MUON_WINDOWS_COMMENTS
    "https://muon-ui.com/ target=${MUON_TARGET_NAME}; cef=${MUON_CEF_VERSION}; cefTarget=${MUON_CEF_TARGET_NAME}; cefApi=${MUON_CEF_API_VERSION}")
  set(MUON_WINDOWS_SPECIAL_BUILD
    "cefArtifact=${MUON_CEF_PACKAGE}; distribution=minimal; apiHash=${MUON_CEF_API_HASH}")
  muon_escape_windows_resource_string(
    "Kouji Matsui. (@kekyo@mi.kekyo.net)" MUON_WINDOWS_COMPANY_NAME)
  muon_escape_windows_resource_string(
    "Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)" MUON_WINDOWS_LEGAL_COPYRIGHT)
  muon_escape_windows_resource_string(
    "${MUON_CORE_VERSION}" MUON_WINDOWS_STRING_VERSION)
  muon_escape_windows_resource_string(
    "${MUON_CORE_GIT_COMMIT_HASH}" MUON_WINDOWS_GIT_COMMIT_HASH)
  muon_escape_windows_resource_string(
    "${MUON_WINDOWS_COMMENTS}" MUON_WINDOWS_COMMENTS)
  muon_escape_windows_resource_string(
    "${MUON_WINDOWS_SPECIAL_BUILD}" MUON_WINDOWS_SPECIAL_BUILD)
  set(MUON_CORE_WINDOWS_RESOURCE_PATH
    "${MUON_GENERATED_INCLUDE_DIR}/muon_core.rc")
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/muon_core.rc.in"
    "${MUON_CORE_WINDOWS_RESOURCE_PATH}"
    @ONLY
    )
  list(APPEND APP_SOURCES
    "${MUON_CORE_WINDOWS_RESOURCE_PATH}"
    )
endif()
if(MUON_CORE_VERSION_HEADER)
  get_filename_component(MUON_CORE_VERSION_HEADER_ABSOLUTE
    "${MUON_CORE_VERSION_HEADER}" ABSOLUTE)
  if(NOT "${MUON_CORE_VERSION_HEADER_ABSOLUTE}" STREQUAL
      "${MUON_CORE_VERSION_HEADER_PATH}")
    configure_file(
      "${MUON_CORE_VERSION_HEADER_ABSOLUTE}"
      "${MUON_CORE_VERSION_HEADER_PATH}"
      COPYONLY
      )
  endif()
else()
  file(WRITE "${MUON_CORE_VERSION_HEADER_PATH}"
"#ifndef MUON_CORE_VERSION_GENERATED_H
#define MUON_CORE_VERSION_GENERATED_H

#define MUON_CORE_VERSION \"${MUON_CORE_VERSION}\"
#define MUON_CORE_GIT_COMMIT_HASH \"${MUON_CORE_GIT_COMMIT_HASH}\"
#define MUON_CORE_BUILD_DATE \"${MUON_CORE_BUILD_DATE}\"
#define MUON_CORE_GIT_COMMIT_DATE \"${MUON_CORE_GIT_COMMIT_DATE}\"

#endif
")
endif()
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/muon_runtime_info_generated.h.in"
  "${MUON_RUNTIME_INFO_HEADER_PATH}"
  @ONLY
  )
list(APPEND APP_HEADERS
  "${MUON_DEFAULT_TITLE_BAR_ICON_HEADER_PATH}"
  "${MUON_CORE_VERSION_HEADER_PATH}"
  "${MUON_RUNTIME_INFO_HEADER_PATH}"
  )
add_executable(${APP_NAME} ${APP_SOURCES} ${APP_HEADERS})
SET_EXECUTABLE_TARGET_PROPERTIES(${APP_NAME})
target_compile_features(${APP_NAME} PRIVATE cxx_std_20)
set_target_properties(${APP_NAME} PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${MUON_RUNTIME_DIR}"
  )
target_link_libraries(${APP_NAME} PRIVATE
  muon_ui
  muon_cardio
  libcef_dll_wrapper
  libcef_lib
  yyjson
  miniz
  ${LIBFFI_TARGET}
  ${MUON_NATIVE_WHEEL_TARGETS}
  ${CEF_STANDARD_LIBS}
  ${CMAKE_DL_LIBS}
  )
if(WIN32)
  target_link_libraries(${APP_NAME} PRIVATE
    advapi32
    comctl32
    ole32
    shell32
    uuid
    )
endif()
target_include_directories(${APP_NAME} PRIVATE
  "${MUON_GENERATED_INCLUDE_DIR}"
  include
  src
  "${TRA_FFIC_ROOT_RESOLVED}/include"
  )
if(MUON_TRACK_FFI_CLOSURES)
  target_compile_definitions(${APP_NAME} PRIVATE
    MUON_TRACK_FFI_CLOSURES
    TRA_FFIC_TRACK_CLOSURES
    )
endif()
if(MUON_BUILD_TESTS)
  target_compile_definitions(${APP_NAME} PRIVATE MUON_TEST_BUILD)
endif()
MUON_APPLY_SANITIZERS(${APP_NAME})

if(MUON_BUILD_TESTS)
  set(TEST_PLUGIN_OUTPUT_DIR "${MUON_RUNTIME_DIR}/test-plugins")
  foreach(TEST_PLUGIN_TARGET IN ITEMS
      muon_test_plugin_alpha
      muon_test_plugin_beta
      muon_test_plugin_builtin_browser_conflict
      muon_test_plugin_builtin_fs_dialogs_conflict
      muon_test_plugin_cardio
      muon_test_plugin_duplicate_namespace
      muon_test_plugin_duplicate_namespace_in_plugin
      muon_test_plugin_function_path_namespace_conflict
      muon_test_plugin_function_lifetime
      muon_test_plugin_helpers
      muon_test_plugin_invalid_namespace
      muon_test_plugin_load_marker
      muon_test_plugin_multi_namespace
      muon_test_plugin_namespace_path_conflict_in_plugin
      muon_test_plugin_recursive_functions
      muon_test_plugin_same_name_alpha
      muon_test_plugin_same_name_beta
      muon_test_plugin_single_namespace
      muon_test_plugin_types
      )
    add_library(${TEST_PLUGIN_TARGET} MODULE
      test_plugins/${TEST_PLUGIN_TARGET}.cpp
      include/muon_plugin_api.h
      )
    set_target_properties(${TEST_PLUGIN_TARGET} PROPERTIES
      PREFIX ""
      LIBRARY_OUTPUT_DIRECTORY "${TEST_PLUGIN_OUTPUT_DIR}"
      RUNTIME_OUTPUT_DIRECTORY "${TEST_PLUGIN_OUTPUT_DIR}"
      )
    target_include_directories(${TEST_PLUGIN_TARGET} PRIVATE
      include
      )
    if(TEST_PLUGIN_TARGET STREQUAL "muon_test_plugin_cardio" OR
        TEST_PLUGIN_TARGET STREQUAL "muon_test_plugin_function_lifetime" OR
        TEST_PLUGIN_TARGET STREQUAL "muon_test_plugin_types")
      target_include_directories(${TEST_PLUGIN_TARGET} PRIVATE
        src
        )
      target_link_libraries(${TEST_PLUGIN_TARGET} PRIVATE
        muon_cardio
        )
    endif()
    target_compile_features(${TEST_PLUGIN_TARGET} PRIVATE cxx_std_20)
    MUON_APPLY_SANITIZERS(${TEST_PLUGIN_TARGET})
  endforeach()
endif()

if(MUON_BUILD_TESTS AND NOT WIN32)
  add_executable(muon_plugin_api_header_test
    tests/muon_plugin_api_header_test.c
    )
  target_include_directories(muon_plugin_api_header_test PRIVATE
    include
    )
  target_compile_features(muon_plugin_api_header_test PRIVATE c_std_99)
  MUON_APPLY_SANITIZERS(muon_plugin_api_header_test)

  add_executable(muon_traffic_type_metadata_test
    tests/muon_traffic_type_metadata_test.cpp
    src/plugins/muon_type_metadata.cpp
    )
  add_executable(muon_traffic_bridge_test
    tests/muon_traffic_bridge_test.cpp
    )
  foreach(MUON_FFI_TEST_TARGET IN ITEMS
      muon_traffic_type_metadata_test
      muon_traffic_bridge_test
      )
    target_compile_features(${MUON_FFI_TEST_TARGET} PRIVATE cxx_std_20)
    target_include_directories(${MUON_FFI_TEST_TARGET} PRIVATE
      include
      src
      "${TRA_FFIC_ROOT_RESOLVED}/include"
      )
    target_link_libraries(${MUON_FFI_TEST_TARGET} PRIVATE
      ${LIBFFI_TARGET}
      )
    if(MUON_TRACK_FFI_CLOSURES)
      target_compile_definitions(${MUON_FFI_TEST_TARGET} PRIVATE
        MUON_TRACK_FFI_CLOSURES
        TRA_FFIC_TRACK_CLOSURES)
    endif()
    MUON_APPLY_SANITIZERS(${MUON_FFI_TEST_TARGET})
  endforeach()
  target_link_libraries(muon_traffic_bridge_test PRIVATE
    muon_cardio
    )
endif()

set(MUON_RUNTIME_COPY_COMMANDS
  COMMAND ${CMAKE_COMMAND} -E make_directory "${MUON_RUNTIME_DIR}"
  COMMAND ${CMAKE_COMMAND} -E rm -f
    "${MUON_RUNTIME_DIR}/${MUON_LEGACY_LICENSE_FILE_NAME}"
  )
foreach(MUON_RUNTIME_CEF_FILE ${CEF_BINARY_FILES})
  set(MUON_RUNTIME_CEF_SOURCE
    "${CEF_BINARY_DIR_EFFECTIVE}/${MUON_RUNTIME_CEF_FILE}")
  get_filename_component(MUON_RUNTIME_CEF_NAME
    "${MUON_RUNTIME_CEF_FILE}" NAME)
  set(MUON_RUNTIME_CEF_TARGET
    "${MUON_RUNTIME_DIR}/${MUON_RUNTIME_CEF_NAME}")
  if(IS_DIRECTORY "${MUON_RUNTIME_CEF_SOURCE}")
    list(APPEND MUON_RUNTIME_COPY_COMMANDS
      COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${MUON_RUNTIME_CEF_SOURCE}"
        "${MUON_RUNTIME_CEF_TARGET}"
      )
  else()
    list(APPEND MUON_RUNTIME_COPY_COMMANDS
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${MUON_RUNTIME_CEF_SOURCE}"
        "${MUON_RUNTIME_CEF_TARGET}"
      )
  endif()
endforeach()
foreach(MUON_RUNTIME_CEF_FILE ${CEF_RESOURCE_FILES})
  set(MUON_RUNTIME_CEF_SOURCE
    "${CEF_RESOURCE_DIR}/${MUON_RUNTIME_CEF_FILE}")
  get_filename_component(MUON_RUNTIME_CEF_NAME
    "${MUON_RUNTIME_CEF_FILE}" NAME)
  set(MUON_RUNTIME_CEF_TARGET
    "${MUON_RUNTIME_DIR}/${MUON_RUNTIME_CEF_NAME}")
  if(IS_DIRECTORY "${MUON_RUNTIME_CEF_SOURCE}")
    list(APPEND MUON_RUNTIME_COPY_COMMANDS
      COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${MUON_RUNTIME_CEF_SOURCE}"
        "${MUON_RUNTIME_CEF_TARGET}"
      )
  else()
    list(APPEND MUON_RUNTIME_COPY_COMMANDS
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${MUON_RUNTIME_CEF_SOURCE}"
        "${MUON_RUNTIME_CEF_TARGET}"
      )
  endif()
endforeach()
set(MUON_RUNTIME_COPY_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/../${MUON_LICENSE_FILE_NAME}"
  "${MUON_RUNTIME_INFO_HEADER_PATH}"
  )
if(MUON_RUNTIME_INCLUDE_APP_FILES)
  list(APPEND MUON_RUNTIME_COPY_COMMANDS
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${CMAKE_CURRENT_SOURCE_DIR}/assets"
      "${MUON_RUNTIME_DIR}/assets"
    )
  if(MUON_CONFIG_COPY_MODE STREQUAL "always")
    list(APPEND MUON_RUNTIME_COPY_COMMANDS
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${MUON_CONFIG_TEMPLATE}"
        "${MUON_RUNTIME_DIR}/muon.json"
      )
  elseif(MUON_CONFIG_COPY_MODE STREQUAL "if_missing")
    list(APPEND MUON_RUNTIME_COPY_COMMANDS
      COMMAND ${CMAKE_COMMAND}
        "-DINPUT=${MUON_CONFIG_TEMPLATE}"
        "-DOUTPUT=${MUON_RUNTIME_DIR}/muon.json"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/copy_if_missing.cmake"
      )
  endif()
  if(MUON_RUNTIME_DIR MATCHES "/\\.run/")
    list(APPEND MUON_RUNTIME_COPY_COMMANDS
      COMMAND ${CMAKE_COMMAND}
        "-DOUTPUT=${MUON_RUNTIME_DIR}/muon.json"
        "-DKEYBIND_NAME=devtools"
        "-DKEYBIND_VALUE=f12"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ensure_browser_keybind_config.cmake"
      COMMAND ${CMAKE_COMMAND}
        "-DOUTPUT=${MUON_RUNTIME_DIR}/muon.json"
        "-DKEYBIND_NAME=recycle"
        "-DKEYBIND_VALUE=ctrl+f12"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ensure_browser_keybind_config.cmake"
      )
  endif()
  file(GLOB_RECURSE MUON_ASSET_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/*")
  list(APPEND MUON_RUNTIME_COPY_DEPENDS
    ${MUON_ASSET_FILES}
    )
endif()
if(MINGW)
  list(APPEND MUON_RUNTIME_COPY_COMMANDS
    COMMAND ${CMAKE_COMMAND}
      "-DINPUT=$<TARGET_FILE:muon_ui>"
      "-DOUTPUT_DIR=${MUON_RUNTIME_DIR}"
      "-DOBJDUMP=${CMAKE_OBJDUMP}"
      "-DCANDIDATES=${MUON_MINGW_RUNTIME_DLLS_ARG}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/copy_mingw_runtime_imports.cmake"
    )
endif()
list(APPEND MUON_RUNTIME_COPY_COMMANDS
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "$<TARGET_FILE:cardio_shared>"
    "${MUON_RUNTIME_DIR}/${MUON_CARDIO_RUNTIME_LIBRARY_NAME}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/../${MUON_LICENSE_FILE_NAME}"
    "${MUON_RUNTIME_DIR}/${MUON_LICENSE_FILE_NAME}"
  )
add_custom_target(muon_runtime_files ALL
  ${MUON_RUNTIME_COPY_COMMANDS}
  DEPENDS
    ${MUON_RUNTIME_COPY_DEPENDS}
  VERBATIM
  )
add_dependencies(muon_runtime_files ${APP_NAME})
add_dependencies(muon_runtime_files muon_ui)
add_dependencies(muon_runtime_files cardio_shared)
if(MUON_RUNTIME_INCLUDE_APP_FILES AND NOT MUON_CONFIG_COPY_MODE STREQUAL "none")
  add_custom_target(muon_runtime_config_template
    DEPENDS "${MUON_CONFIG_TEMPLATE}"
    )
  add_dependencies(muon_runtime_files muon_runtime_config_template)
endif()

enable_testing()
if(CMAKE_GENERATOR STREQUAL "Ninja")
  add_test(
    NAME cef_exceptions_enabled
    COMMAND bash -c
      "test -f '${CMAKE_CURRENT_BINARY_DIR}/build.ninja' && ! grep -Fq -- '-fno-exceptions' '${CMAKE_CURRENT_BINARY_DIR}/build.ninja' && ! grep -Fq -- '_HAS_EXCEPTIONS=0' '${CMAKE_CURRENT_BINARY_DIR}/build.ninja'"
    )
endif()
if(WIN32)
  add_test(
    NAME runtime_contains_windows_runtime
    COMMAND bash -c
      "test -f '${MUON_RUNTIME_DIR}/libcef.dll' && test -f '${MUON_RUNTIME_DIR}/bootstrap.exe' && test -f '${MUON_RUNTIME_DIR}/bootstrapc.exe' && test -f '${MUON_RUNTIME_DIR}/resources.pak' && test -d '${MUON_RUNTIME_DIR}/locales'"
    )
  add_test(
    NAME runtime_contains_muon
    COMMAND bash -c
      "test -f '${MUON_RUNTIME_DIR}/${APP_EXECUTABLE_NAME}' && file '${MUON_RUNTIME_DIR}/${APP_EXECUTABLE_NAME}' | grep -Eq 'PE32|PE32[+]'"
    )
  add_test(
    NAME runtime_contains_cardio
    COMMAND test -f "${MUON_RUNTIME_DIR}/${MUON_CARDIO_RUNTIME_LIBRARY_NAME}"
    )
  add_test(
    NAME runtime_muon_links_cardio
    COMMAND bash -c
      "'${CMAKE_OBJDUMP}' -p '${MUON_RUNTIME_DIR}/${APP_EXECUTABLE_NAME}' > '${CMAKE_CURRENT_BINARY_DIR}/muon-runtime-imports.txt' && grep -Fq 'DLL Name: ${MUON_CARDIO_RUNTIME_LIBRARY_NAME}' '${CMAKE_CURRENT_BINARY_DIR}/muon-runtime-imports.txt'"
    )
  add_test(
    NAME runtime_cardio_uses_static_mingw_runtime
    COMMAND bash -c
      "'${CMAKE_OBJDUMP}' -p '${MUON_RUNTIME_DIR}/${MUON_CARDIO_RUNTIME_LIBRARY_NAME}' > '${CMAKE_CURRENT_BINARY_DIR}/cardio-runtime-imports.txt' && ! grep -Eq 'DLL Name: (libgcc_s_.*-1|libstdc\\+\\+-6)\\.dll' '${CMAKE_CURRENT_BINARY_DIR}/cardio-runtime-imports.txt'"
    )
  add_test(
    NAME runtime_contains_muon_ui_mingw_runtime
    COMMAND bash -c
      "'${CMAKE_OBJDUMP}' -p '${MUON_RUNTIME_DIR}/libmuon-ui${CMAKE_SHARED_LIBRARY_SUFFIX}' > '${CMAKE_CURRENT_BINARY_DIR}/muon-ui-runtime-imports.txt' && while IFS= read -r dll; do test -f '${MUON_RUNTIME_DIR}/'\"$dll\"; done < <(sed -n 's/.*DLL Name: \\(libgcc_s_.*-1\\.dll\\|libstdc++-6\\.dll\\|libwinpthread-1\\.dll\\).*/\\1/p' '${CMAKE_CURRENT_BINARY_DIR}/muon-ui-runtime-imports.txt')"
    )
else()
  add_test(
    NAME runtime_contains_chrome_sandbox
    COMMAND test -x "${MUON_RUNTIME_DIR}/chrome-sandbox"
    )
  add_test(
    NAME runtime_contains_muon
    COMMAND test -x "${MUON_RUNTIME_DIR}/${APP_EXECUTABLE_NAME}"
    )
  add_test(
    NAME runtime_contains_muon_ui
    COMMAND test -f "${MUON_RUNTIME_DIR}/libmuon-ui.so"
    )
  add_test(
    NAME runtime_contains_cardio
    COMMAND test -f "${MUON_RUNTIME_DIR}/${MUON_CARDIO_RUNTIME_LIBRARY_NAME}"
    )
  add_test(
    NAME runtime_omits_standard_fs_dialogs_plugin
    COMMAND test ! -e "${MUON_RUNTIME_DIR}/plugins/muon_fs_dialogs_gtk3.so"
    )
  add_test(
    NAME runtime_muon_omits_direct_gtk3_link
    COMMAND bash -c
      "readelf -d '${MUON_RUNTIME_DIR}/${APP_EXECUTABLE_NAME}' > '${CMAKE_CURRENT_BINARY_DIR}/muon-runtime-dynamic.txt' && ! grep -Eq 'Shared library: \\[libgtk-3|Shared library: \\[libgdk-3' '${CMAKE_CURRENT_BINARY_DIR}/muon-runtime-dynamic.txt'"
    )
  add_test(
    NAME runtime_muon_links_muon_ui
    COMMAND bash -c
      "readelf -d '${MUON_RUNTIME_DIR}/${APP_EXECUTABLE_NAME}' > '${CMAKE_CURRENT_BINARY_DIR}/muon-runtime-dynamic.txt' && grep -Fq 'Shared library: [libmuon-ui.so]' '${CMAKE_CURRENT_BINARY_DIR}/muon-runtime-dynamic.txt'"
    )
  add_test(
    NAME runtime_muon_links_cardio
    COMMAND bash -c
      "readelf -d '${MUON_RUNTIME_DIR}/${APP_EXECUTABLE_NAME}' > '${CMAKE_CURRENT_BINARY_DIR}/muon-runtime-cardio-dynamic.txt' && grep -Fq 'Shared library: [${MUON_CARDIO_RUNTIME_LIBRARY_NAME}]' '${CMAKE_CURRENT_BINARY_DIR}/muon-runtime-cardio-dynamic.txt'"
    )
endif()
if(MUON_RUNTIME_INCLUDE_APP_FILES)
  add_test(
    NAME runtime_contains_app_assets
    COMMAND bash -c
      "test -f '${MUON_RUNTIME_DIR}/assets/main/index.html' && test -f '${MUON_RUNTIME_DIR}/muon.json' && test -f '${MUON_RUNTIME_DIR}/${MUON_LICENSE_FILE_NAME}' && test ! -e '${MUON_RUNTIME_DIR}/${MUON_LEGACY_LICENSE_FILE_NAME}'"
    )
else()
  add_test(
    NAME runtime_omits_app_assets
    COMMAND bash -c
      "test ! -e '${MUON_RUNTIME_DIR}/assets' && test ! -e '${MUON_RUNTIME_DIR}/muon.json' && test -f '${MUON_RUNTIME_DIR}/${MUON_LICENSE_FILE_NAME}' && test ! -e '${MUON_RUNTIME_DIR}/${MUON_LEGACY_LICENSE_FILE_NAME}'"
    )
endif()
if(MUON_BUILD_TESTS)
  add_executable(muon_runtime_info_generated_test
    tests/muon_runtime_info_generated_test.cpp
    "${MUON_RUNTIME_INFO_HEADER_PATH}"
    )
  target_include_directories(muon_runtime_info_generated_test PRIVATE
    "${MUON_GENERATED_INCLUDE_DIR}"
    )
  target_compile_features(muon_runtime_info_generated_test PRIVATE cxx_std_20)
  target_compile_definitions(muon_runtime_info_generated_test PRIVATE
    EXPECTED_EXECUTABLE_NAME="${APP_EXECUTABLE_NAME}"
    EXPECTED_TARGET_NAME="${MUON_TARGET_NAME}"
    EXPECTED_CEF_TARGET_NAME="${MUON_CEF_TARGET_NAME}"
    EXPECTED_CEF_VERSION="${MUON_CEF_VERSION}"
    EXPECTED_CEF_API_VERSION=${MUON_CEF_API_VERSION}
    )
  MUON_APPLY_SANITIZERS(muon_runtime_info_generated_test)
  add_test(
    NAME runtime_info_generated_header
    COMMAND muon_runtime_info_generated_test
    )
endif()
if(MUON_BUILD_TESTS AND NOT WIN32)
  add_test(
    NAME runtime_contains_test_plugins
    COMMAND bash -c
      "test -f '${TEST_PLUGIN_OUTPUT_DIR}/muon_test_plugin_alpha.so' && test -f '${TEST_PLUGIN_OUTPUT_DIR}/muon_test_plugin_beta.so' && test -f '${TEST_PLUGIN_OUTPUT_DIR}/muon_test_plugin_builtin_browser_conflict.so' && test -f '${TEST_PLUGIN_OUTPUT_DIR}/muon_test_plugin_builtin_fs_dialogs_conflict.so' && test -f '${TEST_PLUGIN_OUTPUT_DIR}/muon_test_plugin_cardio.so' && test -f '${TEST_PLUGIN_OUTPUT_DIR}/muon_test_plugin_duplicate_namespace.so' && test -f '${TEST_PLUGIN_OUTPUT_DIR}/muon_test_plugin_duplicate_namespace_in_plugin.so' && test -f '${TEST_PLUGIN_OUTPUT_DIR}/muon_test_plugin_function_path_namespace_conflict.so' && test -f '${TEST_PLUGIN_OUTPUT_DIR}/muon_test_plugin_function_lifetime.so' && test -f '${TEST_PLUGIN_OUTPUT_DIR}/muon_test_plugin_helpers.so' && test -f '${TEST_PLUGIN_OUTPUT_DIR}/muon_test_plugin_invalid_namespace.so' && test -f '${TEST_PLUGIN_OUTPUT_DIR}/muon_test_plugin_load_marker.so' && test -f '${TEST_PLUGIN_OUTPUT_DIR}/muon_test_plugin_multi_namespace.so' && test -f '${TEST_PLUGIN_OUTPUT_DIR}/muon_test_plugin_namespace_path_conflict_in_plugin.so' && test -f '${TEST_PLUGIN_OUTPUT_DIR}/muon_test_plugin_recursive_functions.so' && test -f '${TEST_PLUGIN_OUTPUT_DIR}/muon_test_plugin_same_name_alpha.so' && test -f '${TEST_PLUGIN_OUTPUT_DIR}/muon_test_plugin_same_name_beta.so' && test -f '${TEST_PLUGIN_OUTPUT_DIR}/muon_test_plugin_single_namespace.so' && test -f '${TEST_PLUGIN_OUTPUT_DIR}/muon_test_plugin_types.so'"
    )
elseif(NOT MUON_BUILD_TESTS)
  add_test(
    NAME runtime_omits_test_plugins
    COMMAND bash -c
      "test ! -e '${MUON_RUNTIME_DIR}/test-plugins'"
    )
endif()
if(MUON_RUNTIME_INCLUDE_APP_FILES AND MUON_CONFIG_COPY_MODE STREQUAL "always")
  add_test(
    NAME runtime_config_matches_template
    COMMAND ${CMAKE_COMMAND} -E compare_files
      "${MUON_CONFIG_TEMPLATE}"
      "${MUON_RUNTIME_DIR}/muon.json"
    )
endif()
if(MUON_BUILD_TESTS AND NOT WIN32)
  add_executable(muon_app_scheme_storage_test
    tests/muon_app_scheme_storage_test.cpp
    src/muon_sha1.cpp
    src/app/muon_app_scheme.cpp
    src/app/muon_app_storage.cpp
    src/config/muon_paths.cpp
    )
  SET_EXECUTABLE_TARGET_PROPERTIES(muon_app_scheme_storage_test)
  target_compile_features(muon_app_scheme_storage_test PRIVATE cxx_std_20)
  target_include_directories(muon_app_scheme_storage_test PRIVATE
    include
    src
    )
  target_link_libraries(muon_app_scheme_storage_test PRIVATE
    muon_cardio
    libcef_dll_wrapper
    libcef_lib
    miniz
    ${CEF_STANDARD_LIBS}
    ${CMAKE_DL_LIBS}
    )
  MUON_APPLY_SANITIZERS(muon_app_scheme_storage_test)
  add_executable(muon_builtin_browser_test
    tests/muon_builtin_browser_test.cpp
    src/muon_json_helpers.cpp
    src/muon_string_helpers.cpp
    src/browser/muon_builtin_browser.cpp
    src/browser/muon_context_menu.cpp
    src/browser/muon_icon.cpp
    src/browser/muon_tray.cpp
    src/log/muon_close_debug_log.cpp
    src/browser/muon_native_wheel_forwarder.cpp
    src/browser/muon_title_bar.cpp
    src/config/muon_linux_display_backend.cpp
    src/config/muon_startup.cpp
    src/plugins/muon_type_metadata.cpp
    src/browser/muon_window_delegate.cpp
    src/browser/muon_window_state.cpp
    src/browser/muon_window_title.cpp
    )
  SET_EXECUTABLE_TARGET_PROPERTIES(muon_builtin_browser_test)
  target_compile_features(muon_builtin_browser_test PRIVATE cxx_std_20)
  target_include_directories(muon_builtin_browser_test PRIVATE
    include
    src
    "${TRA_FFIC_ROOT_RESOLVED}/include"
    )
  target_link_libraries(muon_builtin_browser_test PRIVATE
    libcef_dll_wrapper
    libcef_lib
    yyjson
    ${LIBFFI_TARGET}
    ${MUON_NATIVE_WHEEL_TARGETS}
    ${CEF_STANDARD_LIBS}
    ${CMAKE_DL_LIBS}
    )
  MUON_APPLY_SANITIZERS(muon_builtin_browser_test)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(muon_tray_linux_dbus_test
      tests/muon_tray_linux_dbus_test.cpp
      src/muon_json_helpers.cpp
      src/muon_string_helpers.cpp
      src/muon_sha1.cpp
      src/app/muon_app_scheme.cpp
      src/app/muon_app_storage.cpp
      src/browser/muon_tray.cpp
      src/browser/muon_tray_service.cpp
      src/browser/muon_icon.cpp
      src/browser/muon_title_bar.cpp
      src/config/muon_linux_display_backend.cpp
      src/config/muon_paths.cpp
      src/log/muon_close_debug_log.cpp
      )
    SET_EXECUTABLE_TARGET_PROPERTIES(muon_tray_linux_dbus_test)
    target_compile_features(muon_tray_linux_dbus_test PRIVATE cxx_std_20)
    target_include_directories(muon_tray_linux_dbus_test PRIVATE
      include
      src
      "${TRA_FFIC_ROOT_RESOLVED}/include"
      )
    target_link_libraries(muon_tray_linux_dbus_test PRIVATE
      muon_cardio
      libcef_dll_wrapper
      libcef_lib
      yyjson
      miniz
      ${LIBFFI_TARGET}
      ${MUON_NATIVE_WHEEL_TARGETS}
      ${MUON_CARDIO_GLIB_TARGETS}
      ${CEF_STANDARD_LIBS}
      ${CMAKE_DL_LIBS}
      )
    MUON_APPLY_SANITIZERS(muon_tray_linux_dbus_test)
  endif()
  add_executable(muon_default_title_bar_icon_generated_test
    tests/muon_default_title_bar_icon_generated_test.cpp
    "${MUON_DEFAULT_TITLE_BAR_ICON_HEADER_PATH}"
    )
  target_compile_features(muon_default_title_bar_icon_generated_test PRIVATE
    cxx_std_20)
  target_include_directories(muon_default_title_bar_icon_generated_test PRIVATE
    "${MUON_GENERATED_INCLUDE_DIR}"
    )
  target_compile_definitions(muon_default_title_bar_icon_generated_test PRIVATE
    EXPECTED_DEFAULT_TITLE_BAR_ICON_PNG="${MUON_DEFAULT_TITLE_BAR_ICON_PNG_PATH}"
    )
  MUON_APPLY_SANITIZERS(muon_default_title_bar_icon_generated_test)
  add_test(
    NAME default_title_bar_icon_generated
    COMMAND muon_default_title_bar_icon_generated_test
    )
  add_executable(muon_ui_title_bar_manifest_test
    tests/muon_ui_title_bar_manifest_test.cpp
    )
  SET_EXECUTABLE_TARGET_PROPERTIES(muon_ui_title_bar_manifest_test)
  target_compile_features(muon_ui_title_bar_manifest_test PRIVATE cxx_std_20)
  set_target_properties(muon_ui_title_bar_manifest_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${MUON_RUNTIME_DIR}"
    )
  target_include_directories(muon_ui_title_bar_manifest_test PRIVATE
    include
    src
    )
  target_link_libraries(muon_ui_title_bar_manifest_test PRIVATE
    muon_ui
    yyjson
    )
  MUON_APPLY_SANITIZERS(muon_ui_title_bar_manifest_test)
  add_executable(muon_icon_test
    tests/muon_icon_test.cpp
    src/browser/muon_icon.cpp
    )
  SET_EXECUTABLE_TARGET_PROPERTIES(muon_icon_test)
  target_compile_features(muon_icon_test PRIVATE cxx_std_20)
  target_include_directories(muon_icon_test PRIVATE
    src
    )
  MUON_APPLY_SANITIZERS(muon_icon_test)
  add_executable(muon_internal_helpers_test
    tests/muon_internal_helpers_test.cpp
    src/muon_json_helpers.cpp
    src/muon_string_helpers.cpp
    src/plugins/builtin/muon_builtin_environment_helpers.cpp
    src/plugins/builtin/muon_builtin_fs_helpers.cpp
    src/plugins/muon_shared_buffer.cpp
    )
  SET_EXECUTABLE_TARGET_PROPERTIES(muon_internal_helpers_test)
  target_compile_features(muon_internal_helpers_test PRIVATE cxx_std_20)
  target_include_directories(muon_internal_helpers_test PRIVATE
    include
    src
    )
  target_link_libraries(muon_internal_helpers_test PRIVATE
    libcef_dll_wrapper
    libcef_lib
    muon_cardio
    yyjson
    ${CEF_STANDARD_LIBS}
    ${CMAKE_DL_LIBS}
    )
  MUON_APPLY_SANITIZERS(muon_internal_helpers_test)
  add_executable(muon_config_network_test
    tests/muon_config_network_test.cpp
    src/muon_json_helpers.cpp
    src/muon_string_helpers.cpp
    src/browser/muon_browser_background_color.cpp
    src/config/muon_config.cpp
    src/config/muon_glob.cpp
    src/network/muon_network_policy.cpp
    src/plugins/muon_plugin_policy.cpp
    src/config/muon_paths.cpp
    src/config/muon_startup.cpp
    )
  SET_EXECUTABLE_TARGET_PROPERTIES(muon_config_network_test)
  target_compile_features(muon_config_network_test PRIVATE cxx_std_20)
  target_compile_definitions(muon_config_network_test PRIVATE
    MUON_CORE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
  target_include_directories(muon_config_network_test PRIVATE
    include
    src
    )
  target_link_libraries(muon_config_network_test PRIVATE
    libcef_dll_wrapper
    libcef_lib
    yyjson
    ${MUON_CARDIO_GLIB_TARGETS}
    ${CEF_STANDARD_LIBS}
    ${CMAKE_DL_LIBS}
  )
  MUON_APPLY_SANITIZERS(muon_config_network_test)
  add_executable(muon_cef_settings_test
    tests/muon_cef_settings_test.cpp
    src/config/muon_cef_settings.cpp
    )
  SET_EXECUTABLE_TARGET_PROPERTIES(muon_cef_settings_test)
  target_compile_features(muon_cef_settings_test PRIVATE cxx_std_20)
  target_include_directories(muon_cef_settings_test PRIVATE
    include
    src
    )
  target_link_libraries(muon_cef_settings_test PRIVATE
    libcef_dll_wrapper
    libcef_lib
    ${CEF_STANDARD_LIBS}
    ${CMAKE_DL_LIBS}
    )
  MUON_APPLY_SANITIZERS(muon_cef_settings_test)
  add_executable(muon_autostart_test
    tests/muon_autostart_test.cpp
    src/muon_string_helpers.cpp
    src/config/muon_autostart.cpp
    src/config/muon_paths.cpp
    src/config/muon_startup.cpp
    )
  SET_EXECUTABLE_TARGET_PROPERTIES(muon_autostart_test)
  target_compile_features(muon_autostart_test PRIVATE cxx_std_20)
  target_include_directories(muon_autostart_test PRIVATE
    include
    src
    )
  MUON_APPLY_SANITIZERS(muon_autostart_test)
  add_executable(muon_log_test
    tests/muon_log_test.cpp
    src/log/muon_log.cpp
    )
  SET_EXECUTABLE_TARGET_PROPERTIES(muon_log_test)
  target_compile_features(muon_log_test PRIVATE cxx_std_20)
  target_include_directories(muon_log_test PRIVATE
    include
    src
    )
  target_link_libraries(muon_log_test PRIVATE
    muon_cardio
    )
  MUON_APPLY_SANITIZERS(muon_log_test)
  add_executable(muon_traffic_cardio_operation_test
    tests/muon_traffic_cardio_operation_test.cpp
    src/plugins/muon_traffic_cardio_operation.cpp
    )
  SET_EXECUTABLE_TARGET_PROPERTIES(muon_traffic_cardio_operation_test)
  target_compile_features(muon_traffic_cardio_operation_test PRIVATE cxx_std_20)
  target_include_directories(muon_traffic_cardio_operation_test PRIVATE
    include
    src
    )
  target_link_libraries(muon_traffic_cardio_operation_test PRIVATE
    muon_cardio
    )
  MUON_APPLY_SANITIZERS(muon_traffic_cardio_operation_test)
  add_executable(muon_function_wrapper_lifecycle_test
    tests/muon_function_wrapper_lifecycle_test.cpp
    src/plugins/muon_function_wrapper_lifecycle.cpp
    )
  SET_EXECUTABLE_TARGET_PROPERTIES(muon_function_wrapper_lifecycle_test)
  target_compile_features(muon_function_wrapper_lifecycle_test PRIVATE
    cxx_std_20)
  target_include_directories(muon_function_wrapper_lifecycle_test PRIVATE
    src
    )
  target_link_libraries(muon_function_wrapper_lifecycle_test PRIVATE
    Threads::Threads
    )
  MUON_APPLY_SANITIZERS(muon_function_wrapper_lifecycle_test)
  add_executable(muon_plugin_metadata_test
    tests/muon_plugin_metadata_test.cpp
    src/plugins/muon_plugin_metadata.cpp
    src/plugins/muon_type_metadata.cpp
    )
  SET_EXECUTABLE_TARGET_PROPERTIES(muon_plugin_metadata_test)
  target_compile_features(muon_plugin_metadata_test PRIVATE cxx_std_20)
  target_include_directories(muon_plugin_metadata_test PRIVATE
    include
    src
    "${TRA_FFIC_ROOT_RESOLVED}/include"
    )
  target_link_libraries(muon_plugin_metadata_test PRIVATE
    libcef_dll_wrapper
    libcef_lib
    ${LIBFFI_TARGET}
    ${CEF_STANDARD_LIBS}
    ${CMAKE_DL_LIBS}
    )
  MUON_APPLY_SANITIZERS(muon_plugin_metadata_test)
  add_test(
    NAME traffic_type_metadata
    COMMAND muon_traffic_type_metadata_test
    )
  add_test(
    NAME app_scheme_storage
    COMMAND muon_app_scheme_storage_test
    )
  add_test(
    NAME builtin_browser
    COMMAND muon_builtin_browser_test
    )
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_test(
      NAME tray_linux_dbus
      COMMAND muon_tray_linux_dbus_test
      )
  endif()
  add_test(
    NAME muon_ui_title_bar_manifest
    COMMAND "${MUON_RUNTIME_DIR}/muon_ui_title_bar_manifest_test"
    )
  add_test(
    NAME internal_helpers
    COMMAND muon_internal_helpers_test
    )
  add_test(
    NAME icon
    COMMAND muon_icon_test
    )
  add_test(
    NAME cef_settings
    COMMAND muon_cef_settings_test
    )
  add_test(
    NAME config_network
    COMMAND muon_config_network_test
    )
  add_test(
    NAME autostart
    COMMAND muon_autostart_test
    )
  add_test(
    NAME log
    COMMAND muon_log_test
    )
  add_test(
    NAME traffic_cardio_operation
    COMMAND muon_traffic_cardio_operation_test
    )
  add_test(
    NAME function_wrapper_lifecycle
    COMMAND muon_function_wrapper_lifecycle_test
    )
  add_test(
    NAME plugin_metadata
    COMMAND muon_plugin_metadata_test "${MUON_RUNTIME_DIR}"
    )
  set_tests_properties(plugin_metadata PROPERTIES
    WORKING_DIRECTORY "${MUON_RUNTIME_DIR}"
    )
  add_test(
    NAME plugin_api_header
    COMMAND muon_plugin_api_header_test
    )
  add_test(
    NAME traffic_bridge
    COMMAND muon_traffic_bridge_test
    )
  if(MUON_ENABLE_SANITIZERS)
    add_test(
      NAME plugin_api_header_sanitizer
      COMMAND muon_plugin_api_header_test
      )
    add_test(
      NAME traffic_type_metadata_sanitizer
      COMMAND muon_traffic_type_metadata_test
      )
    add_test(
      NAME traffic_bridge_sanitizer
      COMMAND muon_traffic_bridge_test
      )
  endif()
endif()

PRINT_CEF_CONFIG()
