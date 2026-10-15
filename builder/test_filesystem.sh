#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="$(cd "${1:?Specify the built native output directory}" && pwd)"
TARGET="${2:-linux64}"
TEST_DIR="$(mktemp -d)"
trap 'rm -rf "${TEST_DIR}"' EXIT

"${CC:-gcc}" -std=c99 -Wall -Wextra -Werror -pedantic \
  -I"${SCRIPT_DIR}/src" -I"${SCRIPT_DIR}/../deps/sha2" \
  -I"${SCRIPT_DIR}/.deps/src/yyjson-0.12.0/src" \
  "${SCRIPT_DIR}/test/filesystem_test.c" \
  "${OUT_DIR}/libmuon-builder.a" \
  "${SCRIPT_DIR}/.deps/build/libarchive-${TARGET}/libarchive/libarchive.a" \
  "${SCRIPT_DIR}/.deps/build/bzip2-${TARGET}/libbz2.a" \
  "${SCRIPT_DIR}/.deps/build/zlib-${TARGET}/install/lib/libz.a" \
  -pthread -o "${TEST_DIR}/filesystem_test"
"${TEST_DIR}/filesystem_test" "${TEST_DIR}"
