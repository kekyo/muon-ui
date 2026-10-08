#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OUT_DIR="${SCRIPT_DIR}/.run/test-sha256"
SHA2_DIR="${PROJECT_ROOT}/deps/sha2"

mkdir -p "${OUT_DIR}"

"${CC:-gcc}" -std=c99 -Wall -Wextra -pedantic \
  -I"${SHA2_DIR}" \
  -o "${OUT_DIR}/sha256_test" \
  "${SCRIPT_DIR}/test/sha256_test.c" \
  "${SHA2_DIR}/sha2.c"

"${OUT_DIR}/sha256_test"
