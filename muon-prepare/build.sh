#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

BUILD_USAGE="${1:-dev}"
USAGE="Usage: $0 [dev|test|check|dist] [Debug|Release] [linux64|linuxarm|linuxarm64|mingw32|mingw64|win32|win64|windows32|windows64]"
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

TARGET="${3:-linux64}"
case "${TARGET}" in
  linux64|amd64|x64)
    TARGET_NAME="linux64"
    CC="${CC:-gcc}"
    AR="${AR:-ar}"
    RANLIB="${RANLIB:-ranlib}"
    EXECUTABLE_NAME="muon-prepare"
    BOOTSTRAP_EXECUTABLE_NAME="muon-bootstrap"
    LDFLAGS_VALUE="${LDFLAGS:--static}"
    BOOTSTRAP_LDFLAGS_VALUE="${BOOTSTRAP_LDFLAGS:-}"
    ;;
  linuxarm|armv7l|armv7|armhf|arm)
    TARGET_NAME="linuxarm"
    CC="${CC:-gcc}"
    AR="${AR:-ar}"
    RANLIB="${RANLIB:-ranlib}"
    EXECUTABLE_NAME="muon-prepare"
    BOOTSTRAP_EXECUTABLE_NAME="muon-bootstrap"
    LDFLAGS_VALUE="${LDFLAGS:--static}"
    BOOTSTRAP_LDFLAGS_VALUE="${BOOTSTRAP_LDFLAGS:-}"
    ;;
  linuxarm64|arm64|aarch64)
    TARGET_NAME="linuxarm64"
    CC="${CC:-gcc}"
    AR="${AR:-ar}"
    RANLIB="${RANLIB:-ranlib}"
    EXECUTABLE_NAME="muon-prepare"
    BOOTSTRAP_EXECUTABLE_NAME="muon-bootstrap"
    LDFLAGS_VALUE="${LDFLAGS:--static}"
    BOOTSTRAP_LDFLAGS_VALUE="${BOOTSTRAP_LDFLAGS:-}"
    ;;
  linux32|i686|i386|ia32|x86)
    echo "Unsupported target: ${TARGET}. Linux 32-bit CEF builds are discontinued after CEF 101." >&2
    exit 1
    ;;
  mingw32|win32|windows32)
    TARGET_NAME="windows32"
    CC="${CC:-i686-w64-mingw32-gcc}"
    AR="${AR:-i686-w64-mingw32-ar}"
    RANLIB="${RANLIB:-i686-w64-mingw32-ranlib}"
    EXECUTABLE_NAME="muon-prepare.exe"
    BOOTSTRAP_EXECUTABLE_NAME="muon-bootstrap.exe"
    LDFLAGS_VALUE="${LDFLAGS:--static -static-libgcc}"
    BOOTSTRAP_LDFLAGS_VALUE="${BOOTSTRAP_LDFLAGS:-${LDFLAGS_VALUE} -mwindows}"
    ;;
  mingw64|win64|windows64)
    TARGET_NAME="windows64"
    CC="${CC:-x86_64-w64-mingw32-gcc}"
    AR="${AR:-x86_64-w64-mingw32-ar}"
    RANLIB="${RANLIB:-x86_64-w64-mingw32-ranlib}"
    EXECUTABLE_NAME="muon-prepare.exe"
    BOOTSTRAP_EXECUTABLE_NAME="muon-bootstrap.exe"
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
BOOTSTRAP_CPPFLAGS_EXTRA=""
BOOTSTRAP_LDLIBS_EXTRA=""
case "${TARGET_NAME}" in
  linux*)
    command -v pkg-config >/dev/null || { echo "pkg-config is required" >&2; exit 1; }
    BOOTSTRAP_CPPFLAGS_EXTRA="$(pkg-config --cflags xcb) -pthread"
    BOOTSTRAP_LDLIBS_EXTRA="$(pkg-config --libs xcb) -pthread"
    ;;
  windows*)
    BOOTSTRAP_LDLIBS_EXTRA="-lcomctl32"
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

YYJSON_VERSION="0.12.0"
BZIP2_VERSION="1.0.8"
LIBARCHIVE_VERSION="3.8.7"
bash "${SCRIPT_DIR}/build_yyjson.sh"
YYJSON_SOURCE_DIR="${SCRIPT_DIR}/.deps/src/yyjson-${YYJSON_VERSION}/src"
BZIP2_SOURCE_DIR="${SCRIPT_DIR}/.deps/src/bzip2-${BZIP2_VERSION}"
BZIP2_BUILD_DIR="${SCRIPT_DIR}/.deps/build/bzip2-${TARGET_NAME}"
BZIP2_LIB="${BZIP2_BUILD_DIR}/libbz2.a"
bash "${SCRIPT_DIR}/build_bzip2.sh" "${TARGET_NAME}" "${CC}" "${AR}" "${RANLIB}"
LIBARCHIVE_SOURCE_DIR="${SCRIPT_DIR}/.deps/src/libarchive-${LIBARCHIVE_VERSION}"
LIBARCHIVE_BUILD_DIR="${SCRIPT_DIR}/.deps/build/libarchive-${TARGET_NAME}"
LIBARCHIVE_INCLUDE_DIR="${LIBARCHIVE_SOURCE_DIR}/libarchive"
LIBARCHIVE_LIB="${LIBARCHIVE_BUILD_DIR}/libarchive/libarchive.a"
bash "${SCRIPT_DIR}/build_libarchive.sh" "${TARGET_NAME}" "${CC}" "${AR}" "${RANLIB}" "${BZIP2_LIB}"

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
if [[ -n "${MUON_PREPARE_VERSION:-}" && -n "${MUON_PREPARE_GIT_COMMIT_HASH:-}" ]]; then
  cat > "${VERSION_HEADER}" <<EOF
#ifndef MUON_PREPARE_VERSION_H
#define MUON_PREPARE_VERSION_H

#define MUON_PREPARE_VERSION "${MUON_PREPARE_VERSION}"
#define MUON_PREPARE_GIT_COMMIT_HASH "${MUON_PREPARE_GIT_COMMIT_HASH}"

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
#ifndef MUON_PREPARE_VERSION_H
#define MUON_PREPARE_VERSION_H

#define MUON_PREPARE_VERSION "{version}"
#define MUON_PREPARE_GIT_COMMIT_HASH "{git.commit.hash}"

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

make -j -C "${SCRIPT_DIR}" -B \
  CC="${CC}" \
  AR="${AR}" \
  OUT_DIR="${OUT_DIR}" \
  YYJSON_SOURCE_DIR="${YYJSON_SOURCE_DIR}" \
  PREPARE_TARGET="${OUT_DIR}/${EXECUTABLE_NAME}" \
  BOOTSTRAP_TARGET="${OUT_DIR}/${BOOTSTRAP_EXECUTABLE_NAME}" \
  VERSION_HEADER="${VERSION_HEADER}" \
  RUNTIME_INFO_HEADER="${RUNTIME_INFO_HEADER}" \
  CPPFLAGS="${CPPFLAGS_VALUE}" \
  BOOTSTRAP_CPPFLAGS="${BOOTSTRAP_CPPFLAGS_VALUE}" \
  CFLAGS="${CFLAGS_VALUE}" \
  LDFLAGS="${LDFLAGS_VALUE}" \
  BOOTSTRAP_LDFLAGS="${BOOTSTRAP_LDFLAGS_VALUE}" \
  LDLIBS="${LDLIBS_VALUE}" \
  BOOTSTRAP_LDLIBS="${BOOTSTRAP_LDLIBS_VALUE}"
