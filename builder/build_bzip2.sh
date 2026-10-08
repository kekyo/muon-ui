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
command -v "${CC_VALUE}" >/dev/null || { echo "${CC_VALUE} is required" >&2; exit 1; }
command -v "${AR_VALUE}" >/dev/null || { echo "${AR_VALUE} is required" >&2; exit 1; }
command -v "${RANLIB_VALUE}" >/dev/null || { echo "${RANLIB_VALUE} is required" >&2; exit 1; }

BZIP2_VERSION="1.0.8"
BZIP2_PACKAGE="bzip2-${BZIP2_VERSION}.tar.gz"
BZIP2_URL="https://sourceware.org/pub/bzip2/${BZIP2_PACKAGE}"
BZIP2_SHA256="ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269"
DEPS_DIR="${SCRIPT_DIR}/.deps"
ARCHIVE="${DEPS_DIR}/${BZIP2_PACKAGE}"
SOURCE_DIR="${DEPS_DIR}/src/bzip2-${BZIP2_VERSION}"
BUILD_DIR="${DEPS_DIR}/build/bzip2-${TARGET_NAME}"
STAMP="${BUILD_DIR}/.built"
LIBRARY="${BUILD_DIR}/libbz2.a"
LOCK_DIR="${DEPS_DIR}/.locks/bzip2-${BZIP2_VERSION}.lock"
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
  curl -fL -o "${TMP_ARCHIVE}" "${BZIP2_URL}"
  mv "${TMP_ARCHIVE}" "${ARCHIVE}"
}

mkdir -p "${DEPS_DIR}" "${DEPS_DIR}/src" "${BUILD_DIR}"
trap cleanup EXIT
acquire_lock

if [[ ! -f "${ARCHIVE}" ]]; then
  download_archive
elif [[ "$(archive_sha256)" != "${BZIP2_SHA256}" ]]; then
  echo "bzip2 archive sha256 mismatch; redownloading ${BZIP2_PACKAGE}" >&2
  rm -f "${ARCHIVE}"
  download_archive
fi

actual="$(archive_sha256)"
if [[ "${BZIP2_SHA256}" != "${actual}" ]]; then
  echo "bzip2 archive sha256 mismatch: expected ${BZIP2_SHA256}, got ${actual}" >&2
  exit 1
fi

if [[ ! -f "${SOURCE_DIR}/.extracted" ]]; then
  rm -rf "${TMP_SOURCE_DIR}"
  mkdir -p "${TMP_SOURCE_DIR}"
  tar -xzf "${ARCHIVE}" -C "${TMP_SOURCE_DIR}" --strip-components=1
  if [[ ! -f "${TMP_SOURCE_DIR}/bzlib.h" || ! -f "${TMP_SOURCE_DIR}/bzlib.c" || ! -f "${TMP_SOURCE_DIR}/LICENSE" ]]; then
    echo "bzip2 archive did not contain expected files" >&2
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
CFLAGS_VALUE="${CFLAGS:--O2 -DNDEBUG -D_FILE_OFFSET_BITS=64 -Wall -Winline}"
sources=(
  blocksort
  huffman
  crctable
  randtable
  compress
  decompress
  bzlib
)
objects=()
for source in "${sources[@]}"; do
  object="${BUILD_DIR}/${source}.o"
  "${CC_VALUE}" ${CFLAGS_VALUE} -c "${SOURCE_DIR}/${source}.c" -o "${object}"
  objects+=("${object}")
done
"${AR_VALUE}" cq "${LIBRARY}" "${objects[@]}"
"${RANLIB_VALUE}" "${LIBRARY}"
touch "${STAMP}"
