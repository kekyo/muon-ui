if(NOT DEFINED INPUT OR INPUT STREQUAL "")
  message(FATAL_ERROR "INPUT is required.")
endif()
if(NOT DEFINED OUTPUT_DIR OR OUTPUT_DIR STREQUAL "")
  message(FATAL_ERROR "OUTPUT_DIR is required.")
endif()
if(NOT DEFINED OBJDUMP OR OBJDUMP STREQUAL "")
  message(FATAL_ERROR "OBJDUMP is required.")
endif()
if(NOT DEFINED CANDIDATES)
  message(FATAL_ERROR "CANDIDATES is required.")
endif()
string(REPLACE "|" ";" CANDIDATES "${CANDIDATES}")

execute_process(
  COMMAND "${OBJDUMP}" -p "${INPUT}"
  RESULT_VARIABLE OBJDUMP_RESULT
  OUTPUT_VARIABLE OBJDUMP_OUTPUT
  ERROR_VARIABLE OBJDUMP_ERROR
  )
if(NOT OBJDUMP_RESULT EQUAL 0)
  message(FATAL_ERROR
    "Failed to inspect ${INPUT}: ${OBJDUMP_ERROR}")
endif()

file(MAKE_DIRECTORY "${OUTPUT_DIR}")
string(REGEX MATCHALL "DLL Name: [^\r\n]+" IMPORT_LINES
  "${OBJDUMP_OUTPUT}")

foreach(IMPORT_LINE IN LISTS IMPORT_LINES)
  string(REGEX REPLACE "^.*DLL Name: " "" IMPORT_NAME
    "${IMPORT_LINE}")
  set(IS_MINGW_RUNTIME_IMPORT OFF)
  if(IMPORT_NAME MATCHES "^libgcc_s_.*-1\\.dll$" OR
      IMPORT_NAME STREQUAL "libstdc++-6.dll" OR
      IMPORT_NAME STREQUAL "libwinpthread-1.dll")
    set(IS_MINGW_RUNTIME_IMPORT ON)
  endif()

  if(IS_MINGW_RUNTIME_IMPORT)
    set(CANDIDATE_FOUND OFF)
    foreach(CANDIDATE IN LISTS CANDIDATES)
      string(FIND "${CANDIDATE}" "=" SEPARATOR_INDEX)
      if(SEPARATOR_INDEX LESS 1)
        message(FATAL_ERROR
          "Invalid MinGW runtime candidate: ${CANDIDATE}")
      endif()
      string(SUBSTRING "${CANDIDATE}" 0 ${SEPARATOR_INDEX}
        CANDIDATE_NAME)
      math(EXPR CANDIDATE_PATH_INDEX "${SEPARATOR_INDEX} + 1")
      string(SUBSTRING "${CANDIDATE}" ${CANDIDATE_PATH_INDEX} -1
        CANDIDATE_PATH)

      if(IMPORT_NAME STREQUAL CANDIDATE_NAME)
        if(NOT EXISTS "${CANDIDATE_PATH}")
          message(FATAL_ERROR
            "MinGW runtime candidate does not exist: ${CANDIDATE_PATH}")
        endif()
        file(COPY_FILE
          "${CANDIDATE_PATH}"
          "${OUTPUT_DIR}/${CANDIDATE_NAME}"
          ONLY_IF_DIFFERENT)
        set(CANDIDATE_FOUND ON)
      endif()
    endforeach()

    if(NOT CANDIDATE_FOUND)
      message(FATAL_ERROR
        "${INPUT} imports ${IMPORT_NAME}, but the compiler did not resolve a matching runtime DLL.")
    endif()
  endif()
endforeach()
