#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LLHTTP_VERSION="9.4.3"
LLHTTP_SHA256="1eb813c7437b31a87496a1cd3ed79f00746720f5e7e29c79b42c02cb69f36c39"
DEPS_DIR="${SCRIPT_DIR}/.deps"
ARCHIVE="${DEPS_DIR}/llhttp-${LLHTTP_VERSION}.tar.gz"
SOURCE_DIR="${DEPS_DIR}/src/llhttp-${LLHTTP_VERSION}"
LOCK_DIR="${DEPS_DIR}/.locks/llhttp-${LLHTTP_VERSION}.lock"
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
if [[ -f "${SOURCE_DIR}/.extracted" && -f "${SOURCE_DIR}/include/llhttp.h" &&
      -f "${SOURCE_DIR}/src/llhttp.c" && -f "${SOURCE_DIR}/src/api.c" &&
      -f "${SOURCE_DIR}/src/http.c" ]]; then exit 0; fi
if [[ ! -f "${ARCHIVE}" ]] || ! echo "${LLHTTP_SHA256}  ${ARCHIVE}" | sha256sum --check --status; then
  curl -fL "https://github.com/nodejs/llhttp/archive/refs/tags/release/v${LLHTTP_VERSION}.tar.gz" -o "${TMP_ARCHIVE}"
  echo "${LLHTTP_SHA256}  ${TMP_ARCHIVE}" | sha256sum --check --status
  mv "${TMP_ARCHIVE}" "${ARCHIVE}"
fi
mkdir -p "${TMP_SOURCE_DIR}"
tar -xzf "${ARCHIVE}" -C "${TMP_SOURCE_DIR}" --strip-components=1
for required in include/llhttp.h src/llhttp.c src/api.c src/http.c LICENSE LICENSE-MIT; do
  test -f "${TMP_SOURCE_DIR}/${required}"
done
rm -rf "${SOURCE_DIR}"
mv "${TMP_SOURCE_DIR}" "${SOURCE_DIR}"
touch "${SOURCE_DIR}/.extracted"
