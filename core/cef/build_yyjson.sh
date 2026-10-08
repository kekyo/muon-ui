#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

command -v curl >/dev/null || { echo "curl is required" >&2; exit 1; }
command -v sha256sum >/dev/null || { echo "sha256sum is required" >&2; exit 1; }
command -v tar >/dev/null || { echo "tar is required" >&2; exit 1; }

YYJSON_VERSION="0.12.0"
YYJSON_PACKAGE="yyjson-${YYJSON_VERSION}.tar.gz"
YYJSON_URL="https://github.com/ibireme/yyjson/archive/refs/tags/${YYJSON_VERSION}.tar.gz"
YYJSON_SHA256="b16246f617b2a136c78d73e5e2647c6f1de1313e46678062985bdcf1f40bb75d"
DEPS_DIR="${SCRIPT_DIR}/.deps"
ARCHIVE="${DEPS_DIR}/${YYJSON_PACKAGE}"
SOURCE_DIR="${DEPS_DIR}/src/yyjson-${YYJSON_VERSION}"
STAMP="${SOURCE_DIR}/.extracted"
LOCK_DIR="${DEPS_DIR}/.locks/yyjson-${YYJSON_VERSION}.lock"
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
  curl -fL -o "${TMP_ARCHIVE}" "${YYJSON_URL}"
  mv "${TMP_ARCHIVE}" "${ARCHIVE}"
}

mkdir -p "${DEPS_DIR}" "${DEPS_DIR}/src"
trap cleanup EXIT
acquire_lock

if [[ -f "${STAMP}" && -f "${SOURCE_DIR}/src/yyjson.h" && -f "${SOURCE_DIR}/src/yyjson.c" ]]; then
  release_lock
  exit 0
fi

if [[ ! -f "${ARCHIVE}" ]]; then
  download_archive
elif [[ "$(archive_sha256)" != "${YYJSON_SHA256}" ]]; then
  echo "yyjson archive sha256 mismatch; redownloading ${YYJSON_PACKAGE}" >&2
  rm -f "${ARCHIVE}"
  download_archive
fi

actual="$(archive_sha256)"
if [[ "${YYJSON_SHA256}" != "${actual}" ]]; then
  echo "yyjson archive sha256 mismatch: expected ${YYJSON_SHA256}, got ${actual}" >&2
  exit 1
fi

rm -rf "${TMP_SOURCE_DIR}"
mkdir -p "${TMP_SOURCE_DIR}"
tar -xzf "${ARCHIVE}" -C "${TMP_SOURCE_DIR}" --strip-components=1

if [[ ! -f "${TMP_SOURCE_DIR}/src/yyjson.h" || ! -f "${TMP_SOURCE_DIR}/src/yyjson.c" || ! -f "${TMP_SOURCE_DIR}/LICENSE" ]]; then
  echo "yyjson archive did not contain expected files" >&2
  exit 1
fi

rm -rf "${SOURCE_DIR}"
mv "${TMP_SOURCE_DIR}" "${SOURCE_DIR}"
touch "${STAMP}"
release_lock
