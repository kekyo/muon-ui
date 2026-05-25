#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

ARCH_LIST="amd64,armv7l,arm64"
JOBS=""
FORCE="false"
CONTAINER_ENGINE="${CONTAINER_ENGINE:-podman}"

usage() {
  cat <<'EOF'
Usage: ./prereq.sh [--arch amd64,armv7l,arm64] [--jobs N] [--force]

Builds Podman prerequisite images used by build_package.sh.
EOF
}

fail() {
  printf '%s\n' "$*" >&2
  exit 1
}

require_command() {
  command -v "$1" >/dev/null 2>&1 || fail "Missing required command: $1"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --arch)
      [[ $# -ge 2 ]] || fail "--arch requires a value."
      ARCH_LIST="$2"
      shift 2
      ;;
    --jobs)
      [[ $# -ge 2 ]] || fail "--jobs requires a value."
      JOBS="$2"
      shift 2
      ;;
    --force)
      FORCE="true"
      shift
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      fail "Unknown option: $1"
      ;;
  esac
done

if [[ -z "${JOBS}" ]]; then
  if command -v nproc >/dev/null 2>&1; then
    JOBS="$(nproc)"
  else
    JOBS="1"
  fi
fi

case "${JOBS}" in
  ''|*[!0-9]*)
    fail "--jobs must be a positive integer."
    ;;
esac
[[ "${JOBS}" -gt 0 ]] || fail "--jobs must be a positive integer."

require_command "${CONTAINER_ENGINE}"

target_name_for_arch() {
  case "$1" in
    amd64)
      printf '%s\n' "linux64"
      ;;
    armv7l)
      printf '%s\n' "linuxarm"
      ;;
    arm64)
      printf '%s\n' "linuxarm64"
      ;;
    *)
      fail "Unsupported architecture: $1"
      ;;
  esac
}

platform_for_arch() {
  case "$1" in
    amd64)
      printf '%s\n' "linux/amd64"
      ;;
    armv7l)
      printf '%s\n' "linux/arm/v7"
      ;;
    arm64)
      printf '%s\n' "linux/arm64"
      ;;
    *)
      fail "Unsupported architecture: $1"
      ;;
  esac
}

image_for_arch() {
  printf 'localhost/muon-pack-native-debian-bookworm-%s:latest\n' "$1"
}

base_image_for_arch() {
  case "$1" in
    amd64)
      printf '%s\n' "docker.io/amd64/debian:bookworm"
      ;;
    armv7l)
      printf '%s\n' "docker.io/arm32v7/debian:bookworm"
      ;;
    arm64)
      printf '%s\n' "docker.io/arm64v8/debian:bookworm"
      ;;
    *)
      fail "Unsupported architecture: $1"
      ;;
  esac
}

split_arch_list() {
  local value="$1"
  local -n output="$2"
  IFS=',' read -r -a output <<< "${value}"
  for arch in "${output[@]}"; do
    [[ -n "${arch}" ]] || fail "--arch must not contain empty entries."
    target_name_for_arch "${arch}" >/dev/null
  done
}

build_image() {
  local arch="$1"
  local target_name
  local platform
  local image
  local base_image
  local containerfile

  target_name="$(target_name_for_arch "${arch}")"
  platform="$(platform_for_arch "${arch}")"
  image="$(image_for_arch "${arch}")"
  base_image="$(base_image_for_arch "${arch}")"

  if [[ "${FORCE}" != "true" ]] &&
      "${CONTAINER_ENGINE}" image exists "${image}" >/dev/null 2>&1; then
    printf 'Prerequisite image already exists: %s (%s)\n' "${image}" "${target_name}"
    return
  fi

  containerfile="$(mktemp)"
  cat > "${containerfile}" <<'EOF'
ARG BASE_IMAGE
FROM ${BASE_IMAGE}
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    binutils \
    build-essential \
    bzip2 \
    ca-certificates \
    cmake \
    coreutils \
    curl \
    file \
    libasound2-dev \
    libatk-bridge2.0-dev \
    libdrm-dev \
    libffi-dev \
    libgbm-dev \
    libglib2.0-dev \
    libgtk-3-dev \
    libnss3-dev \
    libx11-dev \
    libxcb1-dev \
    libxcomposite-dev \
    libxdamage-dev \
    libxext-dev \
    libxfixes-dev \
    libxkbcommon-dev \
    libxrandr-dev \
    libxrender-dev \
    libxss-dev \
    libxtst-dev \
    make \
    ninja-build \
    patch \
    pkg-config \
    tar \
    xz-utils \
  && rm -rf /var/lib/apt/lists/*
EOF

  printf 'Building prerequisite image: %s (%s, %s)\n' \
    "${image}" "${target_name}" "${platform}"
  "${CONTAINER_ENGINE}" build \
    --platform "${platform}" \
    --build-arg "BASE_IMAGE=${base_image}" \
    -f "${containerfile}" \
    -t "${image}" \
    .
  rm -f "${containerfile}"
}

wait_for_slot() {
  while [[ "$(jobs -pr | wc -l)" -ge "${JOBS}" ]]; do
    wait -n
  done
}

declare -a ARCHES=()
split_arch_list "${ARCH_LIST}" ARCHES

for arch in "${ARCHES[@]}"; do
  wait_for_slot
  build_image "${arch}" &
done
wait
