#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WSLAY_VERSION="1.1.1"
WSLAY_SHA256="7b9f4b9df09adaa6e07ec309b68ab376c0db2cfd916613023b52a47adfda224a"
DEPS_DIR="${SCRIPT_DIR}/.deps"
ARCHIVE="${DEPS_DIR}/wslay-${WSLAY_VERSION}.tar.gz"
SOURCE_DIR="${DEPS_DIR}/src/wslay-${WSLAY_VERSION}"
LOCK_DIR="${DEPS_DIR}/.locks/wslay-${WSLAY_VERSION}.lock"
TMP_ARCHIVE="${ARCHIVE}.$$"
TMP_SOURCE_DIR="${SOURCE_DIR}.tmp.$$"
LOCK_HELD=0
cleanup() {
  rm -f "${TMP_ARCHIVE}"
  rm -rf "${TMP_SOURCE_DIR}"
  if [[ "${LOCK_HELD}" -eq 1 ]]; then rm -rf "${LOCK_DIR}"; fi
}
mkdir -p "${DEPS_DIR}/src" "${DEPS_DIR}/.locks"
trap cleanup EXIT
while ! mkdir "${LOCK_DIR}" 2>/dev/null; do sleep 1; done
LOCK_HELD=1
REQUIRED=(COPYING lib/includes/wslay/wslay.h lib/wslay_event.c lib/wslay_frame.c
  lib/wslay_net.c lib/wslay_queue.c lib/wslay_stack.c)
if [[ -f "${SOURCE_DIR}/.extracted" ]]; then
  WSLAY_COMPLETE=1
  for required in "${REQUIRED[@]}"; do
    if [[ ! -f "${SOURCE_DIR}/${required}" ]]; then WSLAY_COMPLETE=0; break; fi
  done
  if [[ "${WSLAY_COMPLETE}" -eq 1 ]]; then exit 0; fi
fi
if [[ ! -f "${ARCHIVE}" ]] || ! echo "${WSLAY_SHA256}  ${ARCHIVE}" | sha256sum --check --status; then
  curl -fL "https://github.com/tatsuhiro-t/wslay/archive/refs/tags/release-${WSLAY_VERSION}.tar.gz" -o "${TMP_ARCHIVE}"
  echo "${WSLAY_SHA256}  ${TMP_ARCHIVE}" | sha256sum --check --status
  mv "${TMP_ARCHIVE}" "${ARCHIVE}"
fi
mkdir -p "${TMP_SOURCE_DIR}"
tar -xzf "${ARCHIVE}" -C "${TMP_SOURCE_DIR}" --strip-components=1
for required in "${REQUIRED[@]}"; do test -f "${TMP_SOURCE_DIR}/${required}"; done
rm -rf "${SOURCE_DIR}"
mv "${TMP_SOURCE_DIR}" "${SOURCE_DIR}"
touch "${SOURCE_DIR}/.extracted"
