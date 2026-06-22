#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONTAINER_ENGINE="${CONTAINER_ENGINE:-podman}"

run_dist_ctest() {
  local target="$1"
  local test_dir="muon-core/.build/dist/${target}/release"
  local test_file="${test_dir}/CTestTestfile.cmake"

  if [[ "${target}" == "linux64" ]] &&
      [[ -f "${test_file}" ]] &&
      grep -Fq '/workspace/' "${test_file}"; then
    "${CONTAINER_ENGINE}" run --rm \
      --platform linux/amd64 \
      --userns=keep-id \
      --security-opt label=disable \
      -e "HOME=/tmp" \
      -v "${SCRIPT_DIR}:/workspace:Z" \
      -w /workspace \
      localhost/muon-pack-native-debian-bookworm-amd64:latest \
      ctest --test-dir "${test_dir}" --output-on-failure
  else
    ctest --test-dir "${test_dir}" --output-on-failure
  fi
}

npm install
npm run test
npm run build:dist --workspace muon-ui

run_dist_ctest linux64

run_dist_ctest windows32

run_dist_ctest windows64
