if(NOT INPUT)
  message(FATAL_ERROR "INPUT is required.")
endif()
if(NOT OUTPUT)
  message(FATAL_ERROR "OUTPUT is required.")
endif()
if(NOT EXISTS "${INPUT}")
  message(FATAL_ERROR "Default title bar icon PNG does not exist: ${INPUT}")
endif()

file(READ "${INPUT}" MUON_DEFAULT_TITLE_BAR_ICON_HEX HEX)
string(LENGTH "${MUON_DEFAULT_TITLE_BAR_ICON_HEX}" MUON_DEFAULT_TITLE_BAR_ICON_HEX_LENGTH)
math(EXPR MUON_DEFAULT_TITLE_BAR_ICON_SIZE
  "${MUON_DEFAULT_TITLE_BAR_ICON_HEX_LENGTH} / 2")
if(MUON_DEFAULT_TITLE_BAR_ICON_SIZE EQUAL 0)
  message(FATAL_ERROR "Default title bar icon PNG must not be empty: ${INPUT}")
endif()

set(MUON_DEFAULT_TITLE_BAR_ICON_BYTES "")
math(EXPR MUON_DEFAULT_TITLE_BAR_ICON_LAST_INDEX
  "${MUON_DEFAULT_TITLE_BAR_ICON_SIZE} - 1")
foreach(MUON_DEFAULT_TITLE_BAR_ICON_INDEX RANGE 0
    ${MUON_DEFAULT_TITLE_BAR_ICON_LAST_INDEX})
  math(EXPR MUON_DEFAULT_TITLE_BAR_ICON_HEX_OFFSET
    "${MUON_DEFAULT_TITLE_BAR_ICON_INDEX} * 2")
  string(SUBSTRING "${MUON_DEFAULT_TITLE_BAR_ICON_HEX}"
    ${MUON_DEFAULT_TITLE_BAR_ICON_HEX_OFFSET} 2
    MUON_DEFAULT_TITLE_BAR_ICON_BYTE_HEX)
  math(EXPR MUON_DEFAULT_TITLE_BAR_ICON_COLUMN
    "${MUON_DEFAULT_TITLE_BAR_ICON_INDEX} % 12")
  if(MUON_DEFAULT_TITLE_BAR_ICON_COLUMN EQUAL 0)
    string(APPEND MUON_DEFAULT_TITLE_BAR_ICON_BYTES "    ")
  else()
    string(APPEND MUON_DEFAULT_TITLE_BAR_ICON_BYTES ", ")
  endif()
  string(APPEND MUON_DEFAULT_TITLE_BAR_ICON_BYTES
    "0x${MUON_DEFAULT_TITLE_BAR_ICON_BYTE_HEX}")
  if(MUON_DEFAULT_TITLE_BAR_ICON_COLUMN EQUAL 11 OR
      MUON_DEFAULT_TITLE_BAR_ICON_INDEX EQUAL
      MUON_DEFAULT_TITLE_BAR_ICON_LAST_INDEX)
    string(APPEND MUON_DEFAULT_TITLE_BAR_ICON_BYTES ",\n")
  endif()
endforeach()

get_filename_component(MUON_DEFAULT_TITLE_BAR_ICON_OUTPUT_DIR
  "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${MUON_DEFAULT_TITLE_BAR_ICON_OUTPUT_DIR}")
file(WRITE "${OUTPUT}"
"/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

/* This file is auto-generated from images/muon-256.png. DO NOT EDIT manually. */

#pragma once

#include <array>
#include <cstdint>

namespace muon_internal {

inline constexpr std::array<std::uint8_t, ${MUON_DEFAULT_TITLE_BAR_ICON_SIZE}>
    kMuonDefaultTitleBarIconPng = {
${MUON_DEFAULT_TITLE_BAR_ICON_BYTES}};

}  // namespace muon_internal
")
