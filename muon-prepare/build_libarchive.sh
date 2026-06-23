#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ "$#" -ne 5 ]]; then
  echo "Usage: $0 <target> <cc> <ar> <ranlib> <bzip2-lib>" >&2
  exit 1
fi

TARGET_NAME="$1"
CC_VALUE="$2"
AR_VALUE="$3"
RANLIB_VALUE="$4"
BZIP2_LIBRARY="$5"

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

LIBARCHIVE_VERSION="3.8.7"
LIBARCHIVE_PACKAGE="libarchive-${LIBARCHIVE_VERSION}.tar.xz"
LIBARCHIVE_URL="https://github.com/libarchive/libarchive/releases/download/v${LIBARCHIVE_VERSION}/${LIBARCHIVE_PACKAGE}"
LIBARCHIVE_SHA256="d3a8ba457ae25c27c84fd2830a2efdcc5b1d40bf585d4eb0d35f47e99e5d4774"
BZIP2_VERSION="1.0.8"
DEPS_DIR="${SCRIPT_DIR}/.deps"
ARCHIVE="${DEPS_DIR}/${LIBARCHIVE_PACKAGE}"
SOURCE_DIR="${DEPS_DIR}/src/libarchive-${LIBARCHIVE_VERSION}"
BZIP2_SOURCE_DIR="${DEPS_DIR}/src/bzip2-${BZIP2_VERSION}"
BUILD_DIR="${DEPS_DIR}/build/libarchive-${TARGET_NAME}"
LIBRARY="${BUILD_DIR}/libarchive/libarchive.a"
STAMP="${BUILD_DIR}/.built"
LOCK_DIR="${DEPS_DIR}/.locks/libarchive-${LIBARCHIVE_VERSION}.lock"
TMP_ARCHIVE="${ARCHIVE}.$$"
TMP_SOURCE_DIR="${SOURCE_DIR}.tmp.$$"
LOCK_HELD=0

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

archive_sha256() {
  sha256sum "${ARCHIVE}" | awk '{print $1}'
}

download_archive() {
  rm -f "${TMP_ARCHIVE}"
  curl -fL -o "${TMP_ARCHIVE}" "${LIBARCHIVE_URL}"
  mv "${TMP_ARCHIVE}" "${ARCHIVE}"
}

mkdir -p "${DEPS_DIR}" "${DEPS_DIR}/src" "${DEPS_DIR}/build"
trap cleanup EXIT
acquire_lock

if [[ ! -f "${ARCHIVE}" ]]; then
  download_archive
elif [[ "$(archive_sha256)" != "${LIBARCHIVE_SHA256}" ]]; then
  echo "libarchive archive sha256 mismatch; redownloading ${LIBARCHIVE_PACKAGE}" >&2
  rm -f "${ARCHIVE}"
  download_archive
fi

actual="$(archive_sha256)"
if [[ "${LIBARCHIVE_SHA256}" != "${actual}" ]]; then
  echo "libarchive archive sha256 mismatch: expected ${LIBARCHIVE_SHA256}, got ${actual}" >&2
  exit 1
fi

if [[ ! -f "${SOURCE_DIR}/.extracted" ]]; then
  rm -rf "${TMP_SOURCE_DIR}"
  mkdir -p "${TMP_SOURCE_DIR}"
  tar -xJf "${ARCHIVE}" -C "${TMP_SOURCE_DIR}" --strip-components=1
  if [[ ! -f "${TMP_SOURCE_DIR}/libarchive/archive.h" || ! -f "${TMP_SOURCE_DIR}/COPYING" ]]; then
    echo "libarchive archive did not contain expected files" >&2
    exit 1
  fi
  rm -rf "${SOURCE_DIR}"
  mv "${TMP_SOURCE_DIR}" "${SOURCE_DIR}"
  touch "${SOURCE_DIR}/.extracted"
fi
release_lock

if [[ -f "${STAMP}" && -f "${LIBRARY}" ]]; then
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
  -DBUILD_SHARED_LIBS=OFF
  -DENABLE_BZip2=ON
  -DBZIP2_INCLUDE_DIR="${BZIP2_SOURCE_DIR}"
  -DBZIP2_LIBRARIES="${BZIP2_LIBRARY}"
  -DENABLE_ZLIB=OFF
  -DENABLE_LZMA=OFF
  -DENABLE_ZSTD=OFF
  -DENABLE_LZ4=OFF
  -DENABLE_LZO=OFF
  -DENABLE_LIBB2=OFF
  -DENABLE_OPENSSL=OFF
  -DENABLE_MBEDTLS=OFF
  -DENABLE_NETTLE=OFF
  -DENABLE_LIBXML2=OFF
  -DENABLE_EXPAT=OFF
  -DENABLE_WIN32_XMLLITE=OFF
  -DENABLE_PCREPOSIX=OFF
  -DENABLE_PCRE2POSIX=OFF
  -DENABLE_CNG=OFF
  -DENABLE_TAR=OFF
  -DENABLE_CPIO=OFF
  -DENABLE_CAT=OFF
  -DENABLE_UNZIP=OFF
  -DENABLE_XATTR=OFF
  -DENABLE_ACL=OFF
  -DENABLE_ICONV=OFF
  -DENABLE_TEST=OFF
  -DENABLE_INSTALL=OFF
  -DENABLE_WERROR=OFF
)

case "${TARGET_NAME}" in
  windows32|windows64)
    cmake_args+=(-DCMAKE_SYSTEM_NAME=Windows)
    ;;
esac

cmake "${cmake_args[@]}"
cmake --build "${BUILD_DIR}" --target archive_static --config Release -j

if [[ ! -f "${LIBRARY}" ]]; then
  echo "libarchive static library was not produced: ${LIBRARY}" >&2
  exit 1
fi
touch "${STAMP}"
