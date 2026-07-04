#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_USAGE="${1:-dev}"
USAGE="Usage: $0 [dev|test|check|dist] [Debug|Release] [linux-amd64|linux-armhf|linux-arm64|windows-i686|windows-amd64]"
case "${BUILD_USAGE}" in
  dev|test|check|dist)
    ;;
  *)
    echo "${USAGE}" >&2
    exit 1
    ;;
esac

DEFAULT_BUILD_TYPE="Release"
case "${BUILD_USAGE}" in
  dev|test)
    DEFAULT_BUILD_TYPE="Debug"
    ;;
esac
BUILD_TYPE="${2:-${DEFAULT_BUILD_TYPE}}"
case "${BUILD_TYPE}" in
  Debug|Release)
    ;;
  *)
    echo "${USAGE}" >&2
    exit 1
    ;;
esac

TARGET="${3:-linux-amd64}"
case "${TARGET}" in
  linux-amd64)
    TARGET_NAME="linux-amd64"
    CEF_TARGET_NAME="linux64"
    CC="${CC:-gcc}"
    AR="${AR:-ar}"
    RANLIB="${RANLIB:-ranlib}"
    EXECUTABLE_NAME="muon-builder"
    BOOTSTRAP_EXECUTABLE_NAME="muon-bootstrap"
    RUNTIME_HELPER_EXECUTABLE_NAME="muon-runtime-helper"
    LDFLAGS_VALUE="${LDFLAGS:--static}"
    BOOTSTRAP_LDFLAGS_VALUE="${BOOTSTRAP_LDFLAGS:-}"
    ;;
  linux-armhf)
    TARGET_NAME="linux-armhf"
    CEF_TARGET_NAME="linuxarm"
    CC="${CC:-gcc}"
    AR="${AR:-ar}"
    RANLIB="${RANLIB:-ranlib}"
    EXECUTABLE_NAME="muon-builder"
    BOOTSTRAP_EXECUTABLE_NAME="muon-bootstrap"
    RUNTIME_HELPER_EXECUTABLE_NAME="muon-runtime-helper"
    LDFLAGS_VALUE="${LDFLAGS:--static}"
    BOOTSTRAP_LDFLAGS_VALUE="${BOOTSTRAP_LDFLAGS:-}"
    ;;
  linux-arm64)
    TARGET_NAME="linux-arm64"
    CEF_TARGET_NAME="linuxarm64"
    CC="${CC:-gcc}"
    AR="${AR:-ar}"
    RANLIB="${RANLIB:-ranlib}"
    EXECUTABLE_NAME="muon-builder"
    BOOTSTRAP_EXECUTABLE_NAME="muon-bootstrap"
    RUNTIME_HELPER_EXECUTABLE_NAME="muon-runtime-helper"
    LDFLAGS_VALUE="${LDFLAGS:--static}"
    BOOTSTRAP_LDFLAGS_VALUE="${BOOTSTRAP_LDFLAGS:-}"
    ;;
  windows-i686)
    TARGET_NAME="windows-i686"
    CEF_TARGET_NAME="windows32"
    CC="${CC:-i686-w64-mingw32-gcc}"
    AR="${AR:-i686-w64-mingw32-ar}"
    RANLIB="${RANLIB:-i686-w64-mingw32-ranlib}"
    WINDRES="${WINDRES:-i686-w64-mingw32-windres}"
    EXECUTABLE_NAME="muon-builder.exe"
    BOOTSTRAP_EXECUTABLE_NAME="muon-bootstrap.exe"
    RUNTIME_HELPER_EXECUTABLE_NAME=""
    LDFLAGS_VALUE="${LDFLAGS:--static -static-libgcc}"
    BOOTSTRAP_LDFLAGS_VALUE="${BOOTSTRAP_LDFLAGS:-${LDFLAGS_VALUE} -mwindows}"
    ;;
  windows-amd64)
    TARGET_NAME="windows-amd64"
    CEF_TARGET_NAME="windows64"
    CC="${CC:-x86_64-w64-mingw32-gcc}"
    AR="${AR:-x86_64-w64-mingw32-ar}"
    RANLIB="${RANLIB:-x86_64-w64-mingw32-ranlib}"
    WINDRES="${WINDRES:-x86_64-w64-mingw32-windres}"
    EXECUTABLE_NAME="muon-builder.exe"
    BOOTSTRAP_EXECUTABLE_NAME="muon-bootstrap.exe"
    RUNTIME_HELPER_EXECUTABLE_NAME=""
    LDFLAGS_VALUE="${LDFLAGS:--static -static-libgcc}"
    BOOTSTRAP_LDFLAGS_VALUE="${BOOTSTRAP_LDFLAGS:-${LDFLAGS_VALUE} -mwindows}"
    ;;
  *)
    echo "${USAGE}" >&2
    exit 1
    ;;
esac

command -v "${CC}" >/dev/null || { echo "${CC} is required" >&2; exit 1; }
command -v "${AR}" >/dev/null || { echo "${AR} is required" >&2; exit 1; }
command -v "${RANLIB}" >/dev/null || { echo "${RANLIB} is required" >&2; exit 1; }
if [[ "${TARGET_NAME}" == windows-* ]]; then
  command -v "${WINDRES}" >/dev/null || { echo "${WINDRES} is required" >&2; exit 1; }
fi
BOOTSTRAP_CPPFLAGS_EXTRA=""
BOOTSTRAP_LDLIBS_EXTRA=""
case "${TARGET_NAME}" in
  linux-*)
    command -v pkg-config >/dev/null || { echo "pkg-config is required" >&2; exit 1; }
    BOOTSTRAP_CPPFLAGS_EXTRA="$(pkg-config --cflags xcb) -pthread"
    BOOTSTRAP_LDLIBS_EXTRA="$(pkg-config --libs xcb) -pthread"
    ;;
  windows-*)
    BOOTSTRAP_LDLIBS_EXTRA="-lcomctl32 -lgdi32"
    ;;
esac

case "${BUILD_TYPE}" in
  Debug)
    CFLAGS_VALUE="${CFLAGS:--std=c99 -O0 -g -Wall -Wextra -pedantic}"
    ;;
  Release)
    CFLAGS_VALUE="${CFLAGS:--std=c99 -Os -DNDEBUG -Wall -Wextra -pedantic}"
    ;;
esac

read_c_string_define() {
  local header_path="$1"
  local define_name="$2"
  sed -n "s/^#define ${define_name} \"\\(.*\\)\"/\\1/p" "${header_path}" | head -n 1
}

normalize_windows_version() {
  node -e '
const version = process.argv[1] ?? "";
const match = /^(\d+)(?:\.(\d+))?(?:\.(\d+))?(?:\.(\d+))?/.exec(version);
if (match === null) {
  console.error(`Invalid Windows version: ${version}`);
  process.exit(1);
}
const values = [match[1], match[2], match[3], match[4]]
  .map((value) => value === undefined ? 0 : Number.parseInt(value, 10));
for (const value of values) {
  if (!Number.isInteger(value) || value < 0 || value > 65535) {
    console.error(`Invalid Windows version: ${version}`);
    process.exit(1);
  }
}
console.log(values.join("."));
' "$1"
}

escape_rc_string() {
  local value="$1"
  value="${value//\\/\\\\}"
  value="${value//\"/\\\"}"
  printf '%s' "${value}"
}

write_windows_version_resource_script() {
  local output_path="$1"
  local icon_path="$2"
  local manifest_path="$3"
  local file_description="$4"
  local internal_name="$5"
  local original_filename="$6"
  local version="$7"
  local fixed_version="$8"
  local git_commit_hash="$9"
  local comments="${10}"
  local special_build="${11}"
  local fixed_version_comma="${fixed_version//./,}"

  {
    printf '#include <windows.h>\n\n'
    if [[ -n "${icon_path}" ]]; then
      printf '1 ICON "%s"\n\n' "$(escape_rc_string "${icon_path}")"
    fi
    if [[ -n "${manifest_path}" ]]; then
      printf 'CREATEPROCESS_MANIFEST_RESOURCE_ID RT_MANIFEST "%s"\n\n' \
        "$(escape_rc_string "${manifest_path}")"
    fi
    cat <<EOF
1 VERSIONINFO
FILEVERSION ${fixed_version_comma}
PRODUCTVERSION ${fixed_version_comma}
FILEFLAGSMASK 0x3fL
FILEFLAGS 0x28L
FILEOS 0x40004L
FILETYPE 0x1L
FILESUBTYPE 0x0L
BEGIN
  BLOCK "StringFileInfo"
  BEGIN
    BLOCK "040904b0"
    BEGIN
      VALUE "CompanyName", "Kouji Matsui. (@kekyo@mi.kekyo.net)\0"
      VALUE "FileDescription", "$(escape_rc_string "${file_description}")\0"
      VALUE "FileVersion", "$(escape_rc_string "${version}")\0"
      VALUE "InternalName", "$(escape_rc_string "${internal_name}")\0"
      VALUE "LegalCopyright", "Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)\0"
      VALUE "OriginalFilename", "$(escape_rc_string "${original_filename}")\0"
      VALUE "ProductName", "Muon\0"
      VALUE "ProductVersion", "$(escape_rc_string "${version}")\0"
      VALUE "Comments", "$(escape_rc_string "${comments}")\0"
      VALUE "PrivateBuild", "$(escape_rc_string "${git_commit_hash}")\0"
      VALUE "SpecialBuild", "$(escape_rc_string "${special_build}")\0"
    END
  END
  BLOCK "VarFileInfo"
  BEGIN
    VALUE "Translation", 0x0409, 1200
  END
END
EOF
  } > "${output_path}"
}

YYJSON_VERSION="0.12.0"
BZIP2_VERSION="1.0.8"
LIBARCHIVE_VERSION="3.8.7"
bash "${SCRIPT_DIR}/build_yyjson.sh"
YYJSON_SOURCE_DIR="${SCRIPT_DIR}/.deps/src/yyjson-${YYJSON_VERSION}/src"
BZIP2_SOURCE_DIR="${SCRIPT_DIR}/.deps/src/bzip2-${BZIP2_VERSION}"
BZIP2_BUILD_DIR="${SCRIPT_DIR}/.deps/build/bzip2-${CEF_TARGET_NAME}"
BZIP2_LIB="${BZIP2_BUILD_DIR}/libbz2.a"
bash "${SCRIPT_DIR}/build_bzip2.sh" "${CEF_TARGET_NAME}" "${CC}" "${AR}" "${RANLIB}"
LIBARCHIVE_SOURCE_DIR="${SCRIPT_DIR}/.deps/src/libarchive-${LIBARCHIVE_VERSION}"
LIBARCHIVE_BUILD_DIR="${SCRIPT_DIR}/.deps/build/libarchive-${CEF_TARGET_NAME}"
LIBARCHIVE_INCLUDE_DIR="${LIBARCHIVE_SOURCE_DIR}/libarchive"
LIBARCHIVE_LIB="${LIBARCHIVE_BUILD_DIR}/libarchive/libarchive.a"
bash "${SCRIPT_DIR}/build_libarchive.sh" "${CEF_TARGET_NAME}" "${CC}" "${AR}" "${RANLIB}" "${BZIP2_LIB}"

BUILD_TYPE_LOWER="${BUILD_TYPE,,}"
case "${BUILD_USAGE}" in
  dev)
    OUT_DIR="${SCRIPT_DIR}/.run/dev-${TARGET_NAME}-${BUILD_TYPE_LOWER}"
    ;;
  test)
    OUT_DIR="${SCRIPT_DIR}/.run/test-${TARGET_NAME}-${BUILD_TYPE_LOWER}"
    rm -rf "${OUT_DIR}"
    ;;
  check)
    OUT_DIR="${SCRIPT_DIR}/.run/check-${TARGET_NAME}-${BUILD_TYPE_LOWER}"
    rm -rf "${OUT_DIR}"
    ;;
  dist)
    if [[ "${BUILD_TYPE}" == "Debug" ]]; then
      OUT_DIR="${SCRIPT_DIR}/dist-${TARGET_NAME}-debug"
    else
      OUT_DIR="${SCRIPT_DIR}/dist-${TARGET_NAME}"
    fi
    rm -rf "${OUT_DIR}"
    ;;
esac

VERSION_DIR="${OUT_DIR}/generated"
VERSION_TEMPLATE="${VERSION_DIR}/version.h.in"
VERSION_HEADER="${VERSION_DIR}/version.h"
RUNTIME_INFO_HEADER="${VERSION_DIR}/muon_runtime_info_generated.h"
mkdir -p "${VERSION_DIR}"
if [[ -n "${MUON_BUILDER_VERSION:-}" && -n "${MUON_BUILDER_GIT_COMMIT_HASH:-}" ]]; then
  cat > "${VERSION_HEADER}" <<EOF
#ifndef MUON_BUILDER_VERSION_H
#define MUON_BUILDER_VERSION_H

#define MUON_BUILDER_VERSION "${MUON_BUILDER_VERSION}"
#define MUON_BUILDER_GIT_COMMIT_HASH "${MUON_BUILDER_GIT_COMMIT_HASH}"

#endif
EOF
else
  if [[ -f "${SCRIPT_DIR}/node_modules/screw-up/dist/main.mjs" ]]; then
    SCREW_UP_CLI="${SCRIPT_DIR}/node_modules/screw-up/dist/main.mjs"
  elif [[ -f "${SCRIPT_DIR}/../node_modules/screw-up/dist/main.mjs" ]]; then
    SCREW_UP_CLI="${SCRIPT_DIR}/../node_modules/screw-up/dist/main.mjs"
  else
    echo "screw-up is required" >&2
    exit 1
  fi
  cat > "${VERSION_TEMPLATE}" <<'EOF'
#ifndef MUON_BUILDER_VERSION_H
#define MUON_BUILDER_VERSION_H

#define MUON_BUILDER_VERSION "{version}"
#define MUON_BUILDER_GIT_COMMIT_HASH "{git.commit.hash}"

#endif
EOF
  (cd "${SCRIPT_DIR}" && node "${SCREW_UP_CLI}" format --input "${VERSION_TEMPLATE}" "${VERSION_HEADER}" >/dev/null)
fi
if [[ -n "${MUON_RUNTIME_INFO_HEADER:-}" ]]; then
  cp "${MUON_RUNTIME_INFO_HEADER}" "${RUNTIME_INFO_HEADER}"
  if [[ -n "${MUON_CORE_VERSION_HEADER:-}" ]]; then
    cp "${MUON_CORE_VERSION_HEADER}" "${VERSION_DIR}/muon_core_version_generated.h"
  fi
else
  cp "${SCRIPT_DIR}/src/muon_runtime_info_fallback.h" "${RUNTIME_INFO_HEADER}"
fi

PREPARE_WINDOWS_RESOURCE_OBJECTS_VALUE=""
BOOTSTRAP_RESOURCE_OBJECTS_VALUE=""
if [[ "${TARGET_NAME}" == windows-* ]]; then
  WINDOWS_RESOURCE_VERSION="${MUON_WINDOWS_RESOURCE_VERSION:-}"
  WINDOWS_RESOURCE_GIT_COMMIT_HASH="${MUON_WINDOWS_RESOURCE_GIT_COMMIT_HASH:-}"
  if [[ -z "${WINDOWS_RESOURCE_VERSION}" ]]; then
    if [[ -n "${MUON_CORE_VERSION_HEADER:-}" && -f "${MUON_CORE_VERSION_HEADER}" ]]; then
      WINDOWS_RESOURCE_VERSION="$(read_c_string_define "${MUON_CORE_VERSION_HEADER}" MUON_CORE_VERSION)"
      WINDOWS_RESOURCE_GIT_COMMIT_HASH="$(read_c_string_define "${MUON_CORE_VERSION_HEADER}" MUON_CORE_GIT_COMMIT_HASH)"
    else
      WINDOWS_RESOURCE_VERSION="$(read_c_string_define "${VERSION_HEADER}" MUON_BUILDER_VERSION)"
      WINDOWS_RESOURCE_GIT_COMMIT_HASH="$(read_c_string_define "${VERSION_HEADER}" MUON_BUILDER_GIT_COMMIT_HASH)"
    fi
  fi
  if [[ -z "${WINDOWS_RESOURCE_GIT_COMMIT_HASH}" ]]; then
    WINDOWS_RESOURCE_GIT_COMMIT_HASH="unknown"
  fi
  WINDOWS_RESOURCE_FIXED_VERSION="$(normalize_windows_version "${WINDOWS_RESOURCE_VERSION}")"
  WINDOWS_RESOURCE_TARGET="${MUON_WINDOWS_RESOURCE_TARGET:-${TARGET_NAME}}"
  WINDOWS_RESOURCE_CEF_VERSION="${MUON_WINDOWS_RESOURCE_CEF_VERSION:-}"
  WINDOWS_RESOURCE_CEF_TARGET="${MUON_WINDOWS_RESOURCE_CEF_TARGET:-}"
  WINDOWS_RESOURCE_CEF_API_VERSION="${MUON_WINDOWS_RESOURCE_CEF_API_VERSION:-}"
  WINDOWS_RESOURCE_CEF_ARTIFACT="${MUON_WINDOWS_RESOURCE_CEF_ARTIFACT:-}"
  WINDOWS_RESOURCE_CEF_DISTRIBUTION="${MUON_WINDOWS_RESOURCE_CEF_DISTRIBUTION:-}"
  WINDOWS_RESOURCE_CEF_API_HASH="${MUON_WINDOWS_RESOURCE_CEF_API_HASH:-}"
  WINDOWS_RESOURCE_COMMENTS="https://muon-ui.com/ target=${WINDOWS_RESOURCE_TARGET}; cef=${WINDOWS_RESOURCE_CEF_VERSION}; cefTarget=${WINDOWS_RESOURCE_CEF_TARGET}; cefApi=${WINDOWS_RESOURCE_CEF_API_VERSION}"
  WINDOWS_RESOURCE_SPECIAL_BUILD="cefArtifact=${WINDOWS_RESOURCE_CEF_ARTIFACT}; distribution=${WINDOWS_RESOURCE_CEF_DISTRIBUTION}; apiHash=${WINDOWS_RESOURCE_CEF_API_HASH}"

  PREPARE_RESOURCE_SCRIPT="${OUT_DIR}/muon_prepare.rc"
  PREPARE_WINDOWS_RESOURCE_OBJECTS_VALUE="${OUT_DIR}/muon_prepare_windows_resource.o"
  write_windows_version_resource_script \
    "${PREPARE_RESOURCE_SCRIPT}" \
    "" \
    "" \
    "Muon Builder Tool" \
    "muon-builder" \
    "${EXECUTABLE_NAME}" \
    "${WINDOWS_RESOURCE_VERSION}" \
    "${WINDOWS_RESOURCE_FIXED_VERSION}" \
    "${WINDOWS_RESOURCE_GIT_COMMIT_HASH}" \
    "${WINDOWS_RESOURCE_COMMENTS}" \
    "${WINDOWS_RESOURCE_SPECIAL_BUILD}"
  "${WINDRES}" -I "${SCRIPT_DIR}/src" -I "${PROJECT_ROOT}/images" \
    "${PREPARE_RESOURCE_SCRIPT}" \
    "${PREPARE_WINDOWS_RESOURCE_OBJECTS_VALUE}"

  BOOTSTRAP_RESOURCE_SCRIPT="${OUT_DIR}/muon_bootstrap.rc"
  BOOTSTRAP_RESOURCE_OBJECTS_VALUE="${OUT_DIR}/muon_bootstrap_resource.o"
  write_windows_version_resource_script \
    "${BOOTSTRAP_RESOURCE_SCRIPT}" \
    "" \
    "muon_bootstrap.manifest" \
    "Muon Bootstrap" \
    "muon-bootstrap" \
    "${BOOTSTRAP_EXECUTABLE_NAME}" \
    "${WINDOWS_RESOURCE_VERSION}" \
    "${WINDOWS_RESOURCE_FIXED_VERSION}" \
    "${WINDOWS_RESOURCE_GIT_COMMIT_HASH}" \
    "${WINDOWS_RESOURCE_COMMENTS}" \
    "${WINDOWS_RESOURCE_SPECIAL_BUILD}"
  "${WINDRES}" -I "${SCRIPT_DIR}/src" -I "${PROJECT_ROOT}/images" \
    "${BOOTSTRAP_RESOURCE_SCRIPT}" \
    "${BOOTSTRAP_RESOURCE_OBJECTS_VALUE}"
fi

CPPFLAGS_VALUE="-I${VERSION_DIR} -I${YYJSON_SOURCE_DIR} -I${LIBARCHIVE_INCLUDE_DIR} -I${BZIP2_SOURCE_DIR} -DLIBARCHIVE_STATIC -DMUON_PREPARE_TARGET_NAME=\\\"${TARGET_NAME}\\\""
if [[ -n "${CPPFLAGS:-}" ]]; then
  CPPFLAGS_VALUE="${CPPFLAGS_VALUE} ${CPPFLAGS}"
fi
BOOTSTRAP_CPPFLAGS_VALUE="${BOOTSTRAP_CPPFLAGS_EXTRA}"
if [[ -n "${BOOTSTRAP_CPPFLAGS:-}" ]]; then
  BOOTSTRAP_CPPFLAGS_VALUE="${BOOTSTRAP_CPPFLAGS_VALUE} ${BOOTSTRAP_CPPFLAGS}"
fi

LDLIBS_VALUE="${LIBARCHIVE_LIB} ${BZIP2_LIB}"
if [[ -n "${LDLIBS:-}" ]]; then
  LDLIBS_VALUE="${LDLIBS_VALUE} ${LDLIBS}"
fi
BOOTSTRAP_LDLIBS_VALUE="${BOOTSTRAP_LDLIBS_EXTRA}"
if [[ -n "${BOOTSTRAP_LDLIBS:-}" ]]; then
  BOOTSTRAP_LDLIBS_VALUE="${BOOTSTRAP_LDLIBS_VALUE} ${BOOTSTRAP_LDLIBS}"
fi

if [[ -n "${RUNTIME_HELPER_EXECUTABLE_NAME}" ]]; then
  RUNTIME_HELPER_TARGET_VALUE="${OUT_DIR}/${RUNTIME_HELPER_EXECUTABLE_NAME}"
else
  RUNTIME_HELPER_TARGET_VALUE=""
fi

make -j -C "${SCRIPT_DIR}" -B \
  CC="${CC}" \
  AR="${AR}" \
  OUT_DIR="${OUT_DIR}" \
  YYJSON_SOURCE_DIR="${YYJSON_SOURCE_DIR}" \
  PREPARE_TARGET="${OUT_DIR}/${EXECUTABLE_NAME}" \
  BOOTSTRAP_TARGET="${OUT_DIR}/${BOOTSTRAP_EXECUTABLE_NAME}" \
  RUNTIME_HELPER_TARGET="${RUNTIME_HELPER_TARGET_VALUE}" \
  VERSION_HEADER="${VERSION_HEADER}" \
  RUNTIME_INFO_HEADER="${RUNTIME_INFO_HEADER}" \
  CPPFLAGS="${CPPFLAGS_VALUE}" \
  BOOTSTRAP_CPPFLAGS="${BOOTSTRAP_CPPFLAGS_VALUE}" \
  CFLAGS="${CFLAGS_VALUE}" \
  LDFLAGS="${LDFLAGS_VALUE}" \
  BOOTSTRAP_LDFLAGS="${BOOTSTRAP_LDFLAGS_VALUE}" \
  PREPARE_WINDOWS_RESOURCE_OBJECTS="${PREPARE_WINDOWS_RESOURCE_OBJECTS_VALUE}" \
  BOOTSTRAP_RESOURCE_OBJECTS="${BOOTSTRAP_RESOURCE_OBJECTS_VALUE}" \
  LDLIBS="${LDLIBS_VALUE}" \
  BOOTSTRAP_LDLIBS="${BOOTSTRAP_LDLIBS_VALUE}"
