#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

command -v curl >/dev/null || { echo "curl is required" >&2; exit 1; }
command -v make >/dev/null || { echo "make is required" >&2; exit 1; }
command -v tar >/dev/null || { echo "tar is required" >&2; exit 1; }

TARGET="${1:-mingw32}"
case "${TARGET}" in
  mingw32|win32|windows32)
    TARGET_NAME="mingw32"
    HOST_TRIPLET="i686-w64-mingw32"
    ;;
  mingw64|win64|windows64)
    TARGET_NAME="mingw64"
    HOST_TRIPLET="x86_64-w64-mingw32"
    ;;
  *)
    echo "Usage: $0 [mingw32|mingw64|win32|win64|windows32|windows64]" >&2
    exit 1
    ;;
esac

command -v "${HOST_TRIPLET}-gcc" >/dev/null || {
  echo "${HOST_TRIPLET}-gcc is required" >&2
  exit 1
}
command -v "${HOST_TRIPLET}-g++" >/dev/null || {
  echo "${HOST_TRIPLET}-g++ is required" >&2
  exit 1
}

LIBFFI_VERSION="3.4.6"
LIBFFI_PACKAGE="libffi-${LIBFFI_VERSION}.tar.gz"
LIBFFI_URL="https://github.com/libffi/libffi/releases/download/v${LIBFFI_VERSION}/${LIBFFI_PACKAGE}"
DEPS_DIR="${SCRIPT_DIR}/.deps"
ARCHIVE="${DEPS_DIR}/${LIBFFI_PACKAGE}"
SOURCE_DIR="${DEPS_DIR}/src/libffi-${LIBFFI_VERSION}-${TARGET_NAME}"
PREFIX="${DEPS_DIR}/libffi-${TARGET_NAME}"
STAMP="${PREFIX}/.built-${LIBFFI_VERSION}"

mkdir -p "${DEPS_DIR}" "${DEPS_DIR}/src"

if [[ -f "${STAMP}" && -f "${PREFIX}/include/ffi.h" && -f "${PREFIX}/lib/libffi.a" ]]; then
  exit 0
fi

if [[ ! -f "${ARCHIVE}" ]]; then
  curl -fL -o "${ARCHIVE}" "${LIBFFI_URL}"
fi

rm -rf "${SOURCE_DIR}" "${PREFIX}"
mkdir -p "${SOURCE_DIR}"
tar -xzf "${ARCHIVE}" -C "${SOURCE_DIR}" --strip-components=1

pushd "${SOURCE_DIR}" >/dev/null
./configure \
  --host="${HOST_TRIPLET}" \
  --prefix="${PREFIX}" \
  --disable-shared \
  --enable-static \
  --disable-docs
make -j"$(nproc)"
make install
popd >/dev/null

touch "${STAMP}"
