#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

command -v curl >/dev/null || { echo "curl is required" >&2; exit 1; }
command -v sha256sum >/dev/null || { echo "sha256sum is required" >&2; exit 1; }
command -v tar >/dev/null || { echo "tar is required" >&2; exit 1; }

MINIZ_VERSION="3.1.1"
MINIZ_PACKAGE="miniz-${MINIZ_VERSION}.tar.gz"
MINIZ_URL="https://github.com/richgel999/miniz/archive/refs/tags/${MINIZ_VERSION}.tar.gz"
MINIZ_SHA256="8bb29c7bd6f22356e5583e794bed4a0b3e6dfcbcadb49974fc9270ccca1e5557"
DEPS_DIR="${SCRIPT_DIR}/.deps"
ARCHIVE="${DEPS_DIR}/${MINIZ_PACKAGE}"
SOURCE_DIR="${DEPS_DIR}/src/miniz-${MINIZ_VERSION}"
STAMP="${SOURCE_DIR}/.extracted"
LOCK_DIR="${DEPS_DIR}/.locks/miniz-${MINIZ_VERSION}.lock"
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
  curl -fL -o "${TMP_ARCHIVE}" "${MINIZ_URL}"
  mv "${TMP_ARCHIVE}" "${ARCHIVE}"
}

mkdir -p "${DEPS_DIR}" "${DEPS_DIR}/src"
trap cleanup EXIT
acquire_lock

if [[ -f "${STAMP}" && -f "${SOURCE_DIR}/miniz.c" && -f "${SOURCE_DIR}/miniz.h" ]]; then
  release_lock
  exit 0
fi

if [[ ! -f "${ARCHIVE}" ]]; then
  download_archive
elif [[ "$(archive_sha256)" != "${MINIZ_SHA256}" ]]; then
  echo "miniz archive sha256 mismatch; redownloading ${MINIZ_PACKAGE}" >&2
  rm -f "${ARCHIVE}"
  download_archive
fi

actual="$(archive_sha256)"
if [[ "${MINIZ_SHA256}" != "${actual}" ]]; then
  echo "miniz archive sha256 mismatch: expected ${MINIZ_SHA256}, got ${actual}" >&2
  exit 1
fi

rm -rf "${TMP_SOURCE_DIR}"
mkdir -p "${TMP_SOURCE_DIR}"
tar -xzf "${ARCHIVE}" -C "${TMP_SOURCE_DIR}" --strip-components=1

if [[ ! -f "${TMP_SOURCE_DIR}/miniz.c" || ! -f "${TMP_SOURCE_DIR}/miniz.h" || ! -f "${TMP_SOURCE_DIR}/LICENSE" ]]; then
  echo "miniz archive did not contain expected files" >&2
  exit 1
fi

rm -rf "${SOURCE_DIR}"
mv "${TMP_SOURCE_DIR}" "${SOURCE_DIR}"
touch "${STAMP}"
release_lock
