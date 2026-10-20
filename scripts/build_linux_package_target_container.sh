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
require_env MUON_BUILDER_VERSION
require_env MUON_BUILDER_GIT_COMMIT_HASH
require_env MUON_CORE_VERSION_HEADER
require_env MUON_NOTIFICATION_RENDERER_HEADER
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
require_command node

cd /workspace
[[ -f "${MUON_NOTIFICATION_RENDERER_HEADER}" ]] ||
  fail "Missing host-generated notification renderer: ${MUON_NOTIFICATION_RENDERER_HEADER}"

export MUON_BUILDER_VERSION
export MUON_BUILDER_GIT_COMMIT_HASH
export MUON_CORE_VERSION_HEADER

bash builder/build.sh dist Release "${MUON_PACKAGE_TARGET}"
rm -rf "core/cef/.build/dist/${MUON_PACKAGE_TARGET}"
bash core/cef/build.sh dist Release "${MUON_PACKAGE_TARGET}" \
  "-DMUON_NOTIFICATION_RENDERER_HEADER=${MUON_NOTIFICATION_RENDERER_HEADER}" \
  "-DTRA_FFIC_ROOT=${MUON_TRA_FFIC_ROOT}" \
  "-DCARDIO_ROOT=${MUON_CARDIO_ROOT}"

file "builder/dist-${MUON_PACKAGE_TARGET}/muon-builder"
readelf -h "builder/dist-${MUON_PACKAGE_TARGET}/muon-builder" >/dev/null
file "core/cef/dist-${MUON_PACKAGE_TARGET}/muon-core"
readelf -h "core/cef/dist-${MUON_PACKAGE_TARGET}/muon-core" >/dev/null

node ui/.build/linux-dependencies/linux-dependencies.mjs \
  --target "${MUON_PACKAGE_TARGET}" \
  --cmake-cache "core/cef/.build/dist/${MUON_PACKAGE_TARGET}/release/CMakeCache.txt" \
  --runtime "core/cef/dist-${MUON_PACKAGE_TARGET}" \
  --native "builder/dist-${MUON_PACKAGE_TARGET}" \
  --bundle-baseline /opt/muon-baseline/source.json \
  --output "core/cef/dist-${MUON_PACKAGE_TARGET}/linux-dependencies.json"
