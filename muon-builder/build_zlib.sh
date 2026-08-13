#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ "$#" -ne 4 ]]; then
  echo "Usage: $0 <target> <cc> <ar> <ranlib>" >&2
  exit 1
fi

TARGET_NAME="$1"
CC_VALUE="$2"
AR_VALUE="$3"
RANLIB_VALUE="$4"

command -v curl >/dev/null || { echo "curl is required" >&2; exit 1; }
command -v sha256sum >/dev/null || { echo "sha256sum is required" >&2; exit 1; }
command -v tar >/dev/null || { echo "tar is required" >&2; exit 1; }
command -v cmake >/dev/null || { echo "cmake is required" >&2; exit 1; }
command -v "${CC_VALUE}" >/dev/null || { echo "${CC_VALUE} is required" >&2; exit 1; }
command -v "${AR_VALUE}" >/dev/null || { echo "${AR_VALUE} is required" >&2; exit 1; }
command -v "${RANLIB_VALUE}" >/dev/null || { echo "${RANLIB_VALUE} is required" >&2; exit 1; }
CC_PATH="$(command -v "${CC_VALUE}")"
AR_PATH="$(command -v "${AR_VALUE}")"
RANLIB_PATH="$(command -v "${RANLIB_VALUE}")"

ZLIB_VERSION="1.3.2"
ZLIB_PACKAGE="zlib-${ZLIB_VERSION}.tar.gz"
ZLIB_URLS=(
  "https://github.com/madler/zlib/releases/download/v${ZLIB_VERSION}/${ZLIB_PACKAGE}"
  "https://zlib.net/fossils/${ZLIB_PACKAGE}"
)
ZLIB_SHA256="bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16"
DEPS_DIR="${SCRIPT_DIR}/.deps"
ARCHIVE="${DEPS_DIR}/${ZLIB_PACKAGE}"
SOURCE_DIR="${DEPS_DIR}/src/zlib-${ZLIB_VERSION}"
BUILD_DIR="${DEPS_DIR}/build/zlib-${TARGET_NAME}"
INSTALL_DIR="${BUILD_DIR}/install"
STAMP="${BUILD_DIR}/.built-recipe"
LOCK_DIR="${DEPS_DIR}/.locks/zlib-${ZLIB_VERSION}.lock"
TMP_ARCHIVE="${ARCHIVE}.$$"
TMP_SOURCE_DIR="${SOURCE_DIR}.tmp.$$"
LOCK_HELD=0

case "${TARGET_NAME}" in
  windows32|windows64)
    LIBRARY="${INSTALL_DIR}/lib/libzs.a"
    ;;
  *)
    LIBRARY="${INSTALL_DIR}/lib/libz.a"
    ;;
esac

RECIPE="zlib-${ZLIB_VERSION}|cmake-static-install-v1|${CC_PATH}|${AR_PATH}|${RANLIB_PATH}|${CFLAGS:-}"

cleanup() {
  rm -f "${TMP_ARCHIVE}"
  rm -rf "${TMP_SOURCE_DIR}"
  if [[ "${LOCK_HELD}" -eq 1 ]]; then
    rm -rf "${LOCK_DIR}"
  fi
}

acquire_lock() {
  mkdir -p "${DEPS_DIR}/.locks"
  while ! mkdir "${LOCK_DIR}" 2>/dev/null; do
    sleep 1
  done
  LOCK_HELD=1
}

release_lock() {
  if [[ "${LOCK_HELD}" -eq 1 ]]; then
    rm -rf "${LOCK_DIR}"
    LOCK_HELD=0
  fi
}

file_sha256() {
  sha256sum "$1" | awk '{print $1}'
}

download_archive() {
  local actual
  local url

  for url in "${ZLIB_URLS[@]}"; do
    rm -f "${TMP_ARCHIVE}"
    echo "Downloading ${ZLIB_PACKAGE} from ${url}" >&2
    if ! curl \
      -fL \
      --retry 2 \
      --retry-all-errors \
      --retry-delay 2 \
      --retry-max-time 120 \
      --connect-timeout 15 \
      --max-time 60 \
      -o "${TMP_ARCHIVE}" \
      "${url}"; then
      echo "Failed to download ${ZLIB_PACKAGE} from ${url}" >&2
      continue
    fi
    if actual="$(file_sha256 "${TMP_ARCHIVE}")" &&
       [[ "${actual}" == "${ZLIB_SHA256}" ]]; then
      mv "${TMP_ARCHIVE}" "${ARCHIVE}"
      return 0
    fi
    echo "zlib archive sha256 mismatch from ${url}: expected ${ZLIB_SHA256}, got ${actual:-unavailable}" >&2
  done

  echo "Failed to download a verified ${ZLIB_PACKAGE} archive" >&2
  return 1
}

mkdir -p "${DEPS_DIR}" "${DEPS_DIR}/src" "${DEPS_DIR}/build"
trap cleanup EXIT
acquire_lock

if [[ ! -f "${ARCHIVE}" ]]; then
  download_archive
elif [[ "$(file_sha256 "${ARCHIVE}")" != "${ZLIB_SHA256}" ]]; then
  echo "zlib archive sha256 mismatch; redownloading ${ZLIB_PACKAGE}" >&2
  rm -f "${ARCHIVE}"
  download_archive
fi

actual="$(file_sha256 "${ARCHIVE}")"
if [[ "${ZLIB_SHA256}" != "${actual}" ]]; then
  echo "zlib archive sha256 mismatch: expected ${ZLIB_SHA256}, got ${actual}" >&2
  exit 1
fi

if [[ ! -f "${SOURCE_DIR}/.extracted" ]]; then
  rm -rf "${TMP_SOURCE_DIR}"
  mkdir -p "${TMP_SOURCE_DIR}"
  tar -xzf "${ARCHIVE}" -C "${TMP_SOURCE_DIR}" --strip-components=1
  if [[ ! -f "${TMP_SOURCE_DIR}/zlib.h" ||
        ! -f "${TMP_SOURCE_DIR}/CMakeLists.txt" ||
        ! -f "${TMP_SOURCE_DIR}/LICENSE" ]]; then
    echo "zlib archive did not contain expected files" >&2
    exit 1
  fi
  rm -rf "${SOURCE_DIR}"
  mv "${TMP_SOURCE_DIR}" "${SOURCE_DIR}"
  touch "${SOURCE_DIR}/.extracted"
fi
release_lock

if [[ -f "${STAMP}" &&
      -f "${LIBRARY}" &&
      -f "${INSTALL_DIR}/include/zlib.h" &&
      -f "${INSTALL_DIR}/include/zconf.h" &&
      "$(cat "${STAMP}")" == "${RECIPE}" ]]; then
  exit 0
fi

rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"

cmake_args=(
  -S "${SOURCE_DIR}"
  -B "${BUILD_DIR}"
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_C_COMPILER="${CC_PATH}"
  -DCMAKE_AR="${AR_PATH}"
  -DCMAKE_RANLIB="${RANLIB_PATH}"
  -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}"
  -DCMAKE_INSTALL_LIBDIR=lib
  -DCMAKE_INSTALL_INCLUDEDIR=include
  -DZLIB_BUILD_TESTING=OFF
  -DZLIB_BUILD_SHARED=OFF
  -DZLIB_BUILD_STATIC=ON
  -DZLIB_INSTALL=ON
)

case "${TARGET_NAME}" in
  windows32|windows64)
    cmake_args+=(-DCMAKE_SYSTEM_NAME=Windows)
    ;;
esac

cmake "${cmake_args[@]}"
cmake --build "${BUILD_DIR}" --target zlibstatic --config Release -j
cmake --install "${BUILD_DIR}" --config Release --component Development

if [[ ! -f "${LIBRARY}" ||
      ! -f "${INSTALL_DIR}/include/zlib.h" ||
      ! -f "${INSTALL_DIR}/include/zconf.h" ]]; then
  echo "zlib static installation was not produced under ${INSTALL_DIR}" >&2
  exit 1
fi
printf '%s\n' "${RECIPE}" > "${STAMP}"
