#!/usr/bin/env bash
set -euo pipefail

fail() {
  printf '%s\n' "$*" >&2
  exit 1
}

require_env() {
  local name=$1
  local value
  value="${!name:-}"
  [[ -n "${value}" ]] || fail "Missing required environment variable: ${name}"
}

require_command() {
  command -v "$1" >/dev/null 2>&1 || fail "Missing required command: $1"
}

expected_dpkg_architecture() {
  case "$1" in
    amd64)
      printf '%s\n' 'amd64'
      ;;
    armv7l)
      printf '%s\n' 'armhf'
      ;;
    arm64)
      printf '%s\n' 'arm64'
      ;;
    *)
      fail "Unsupported MUON_PACKAGE_ARCH: $1"
      ;;
  esac
}

require_env MUON_PACKAGE_ARCH
require_env MUON_PACKAGE_TARGET
require_env MUON_PREPARE_VERSION
require_env MUON_PREPARE_GIT_COMMIT_HASH
require_env MUON_CORE_VERSION_HEADER
require_env MUON_TRA_FFIC_ROOT
require_env MUON_CARDIO_ROOT

actual_dpkg_architecture="$(dpkg --print-architecture)"
expected_dpkg_architecture="$(expected_dpkg_architecture "${MUON_PACKAGE_ARCH}")"
if [[ "${actual_dpkg_architecture}" != "${expected_dpkg_architecture}" ]]; then
  fail "Container architecture mismatch: expected ${expected_dpkg_architecture} for ${MUON_PACKAGE_ARCH}, got ${actual_dpkg_architecture}."
fi

require_command bash
require_command cmake
require_command file
require_command gcc
require_command g++
require_command make
require_command ninja
require_command pkg-config
require_command readelf

cd /workspace

export MUON_PREPARE_VERSION
export MUON_PREPARE_GIT_COMMIT_HASH
export MUON_CORE_VERSION_HEADER

bash muon-prepare/build.sh dist Release "${MUON_PACKAGE_TARGET}"
rm -rf "muon-core/.build/dist/${MUON_PACKAGE_TARGET}"
bash muon-core/build.sh dist Release "${MUON_PACKAGE_TARGET}" \
  "-DTRA_FFIC_ROOT=${MUON_TRA_FFIC_ROOT}" \
  "-DCARDIO_ROOT=${MUON_CARDIO_ROOT}"

file "muon-prepare/dist-${MUON_PACKAGE_TARGET}/muon-prepare"
readelf -h "muon-prepare/dist-${MUON_PACKAGE_TARGET}/muon-prepare" >/dev/null
file "muon-core/dist-${MUON_PACKAGE_TARGET}/muon-core"
readelf -h "muon-core/dist-${MUON_PACKAGE_TARGET}/muon-core" >/dev/null
