#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

PACKAGE_TARGET="all"
ARCH_LIST="amd64,armv7l,arm64"
JOBS=""
CONTAINER_ENGINE="${CONTAINER_ENGINE:-podman}"
ARTIFACT_DIR="${ARTIFACT_DIR:-${SCRIPT_DIR}/artifacts}"
PACKAGE_LOG_DIR="${PACKAGE_LOG_DIR:-${SCRIPT_DIR}/.deps/package-logs}"
TRA_FFIC_ROOT_HOST="${TRA_FFIC_ROOT_HOST:-${SCRIPT_DIR}/deps/tra-ffic}"
CARDIO_ROOT_HOST="${CARDIO_ROOT_HOST:-${SCRIPT_DIR}/deps/cardio}"

usage() {
  cat <<'EOF'
Usage: ./build_package.sh [--target dist|npm|all] [--arch amd64,armv7l,arm64] [--jobs N]

Builds the multi-platform native/runtime dist tree for the muon npm package.
The npm target packs the already-built dist tree and verifies the tarball list.
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
    --target)
      [[ $# -ge 2 ]] || fail "--target requires a value."
      PACKAGE_TARGET="$2"
      shift 2
      ;;
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
    --help|-h)
      usage
      exit 0
      ;;
    *)
      fail "Unknown option: $1"
      ;;
  esac
done

case "${PACKAGE_TARGET}" in
  dist|npm|all)
    ;;
  *)
    fail "--target must be one of: dist, npm, all."
    ;;
esac

if [[ -z "${JOBS}" ]]; then
  if command -v nproc >/dev/null 2>&1; then
    JOBS="$(nproc)"
    if [[ "${JOBS}" -gt 2 ]]; then
      JOBS="2"
    fi
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

split_arch_list() {
  local value="$1"
  local -n output="$2"
  IFS=',' read -r -a output <<< "${value}"
  for arch in "${output[@]}"; do
    [[ -n "${arch}" ]] || fail "--arch must not contain empty entries."
    target_name_for_arch "${arch}" >/dev/null
  done
}

contains_arch() {
  local expected="$1"
  shift
  for arch in "$@"; do
    if [[ "${arch}" == "${expected}" ]]; then
      return 0
    fi
  done
  return 1
}

is_full_arch_matrix() {
  [[ "$#" -eq 3 ]] &&
    contains_arch amd64 "$@" &&
    contains_arch armv7l "$@" &&
    contains_arch arm64 "$@"
}

package_version() {
  local package_json="$1"
  node -e 'const fs = require("node:fs"); const packageJson = JSON.parse(fs.readFileSync(process.argv[1], "utf8")); process.stdout.write(packageJson.version);' "${package_json}"
}

git_commit_hash() {
  if git rev-parse HEAD >/dev/null 2>&1; then
    git rev-parse --short=20 HEAD
  else
    printf '%s\n' "unknown"
  fi
}

assert_prereq_image() {
  local arch="$1"
  local image
  image="$(image_for_arch "${arch}")"
  if ! "${CONTAINER_ENGINE}" image exists "${image}" >/dev/null 2>&1; then
    fail "Missing prerequisite image: ${image}. Run ./prereq.sh --arch ${arch} first."
  fi
}

assert_native_dependency_checkouts() {
  [[ -f "${TRA_FFIC_ROOT_HOST}/include/tra_ffic.h" ]] ||
    fail "Missing tra-ffic checkout: ${TRA_FFIC_ROOT_HOST}"
  [[ -f "${CARDIO_ROOT_HOST}/include/cardio.h" ]] ||
    fail "Missing cardio checkout: ${CARDIO_ROOT_HOST}"
}

build_linux_target() {
  local arch="$1"
  local target_name
  local platform
  local image
  local log_path
  local status
  target_name="$(target_name_for_arch "${arch}")"
  platform="$(platform_for_arch "${arch}")"
  image="$(image_for_arch "${arch}")"
  log_path="${PACKAGE_LOG_DIR}/${target_name}.log"

  assert_prereq_image "${arch}"
  mkdir -p "${PACKAGE_LOG_DIR}"
  printf 'Building Linux package target: %s (%s), log: %s\n' "${target_name}" "${platform}" "${log_path}"
  set +e
  "${CONTAINER_ENGINE}" run --rm \
    --platform "${platform}" \
    --userns=keep-id \
    --security-opt label=disable \
    -e "HOME=/tmp" \
    -e "MUON_PACKAGE_ARCH=${arch}" \
    -e "MUON_PACKAGE_TARGET=${target_name}" \
    -e "MUON_PREPARE_VERSION=${MUON_PREPARE_VERSION}" \
    -e "MUON_PREPARE_GIT_COMMIT_HASH=${MUON_PREPARE_GIT_COMMIT_HASH}" \
    -e "MUON_CORE_VERSION=${MUON_CORE_VERSION}" \
    -e "MUON_CORE_GIT_COMMIT_HASH=${MUON_CORE_GIT_COMMIT_HASH}" \
    -e "MUON_TRA_FFIC_ROOT=/workspace-deps/tra-ffic" \
    -e "MUON_CARDIO_ROOT=/workspace-deps/cardio" \
    -e "MUON_CACHE_DIR=/workspace/.deps/muon-cache" \
    -v "${SCRIPT_DIR}:/workspace:Z" \
    -v "${TRA_FFIC_ROOT_HOST}:/workspace-deps/tra-ffic:ro,Z" \
    -v "${CARDIO_ROOT_HOST}:/workspace-deps/cardio:ro,Z" \
    -w /workspace \
    "${image}" \
    bash scripts/build_linux_package_target_container.sh 2>&1 | tee "${log_path}"
  status="${PIPESTATUS[0]}"
  set -e
  if [[ "${status}" -ne 0 ]]; then
    printf 'Linux package target failed: %s (%s), exit code %s, log: %s\n' \
      "${target_name}" "${platform}" "${status}" "${log_path}" >&2
    return "${status}"
  fi
  printf 'Linux package target completed: %s (%s), log: %s\n' "${target_name}" "${platform}" "${log_path}"
}

LINUX_BUILD_FAILURES=0

wait_for_slot() {
  while [[ "$(jobs -pr | wc -l)" -ge "${JOBS}" ]]; do
    if ! wait -n; then
      LINUX_BUILD_FAILURES=1
    fi
  done
}

build_linux_targets() {
  local -a arches=("$@")
  LINUX_BUILD_FAILURES=0
  for arch in "${arches[@]}"; do
    wait_for_slot
    build_linux_target "${arch}" &
  done
  while [[ "$(jobs -pr | wc -l)" -gt 0 ]]; do
    if ! wait -n; then
      LINUX_BUILD_FAILURES=1
    fi
  done
  if [[ "${LINUX_BUILD_FAILURES}" -ne 0 ]]; then
    fail "One or more Linux package targets failed. See logs in ${PACKAGE_LOG_DIR}."
  fi
}

build_windows_targets() {
  printf 'Building Windows package targets\n'
  npm run build:target --workspace muon-prepare -- dist Release windows32
  npm run build:target --workspace muon-prepare -- dist Release windows64
  npm run build:target --workspace muon-core -- dist Release windows32
  npm run build:target --workspace muon-core -- dist Release windows64
}

stage_targets() {
  local -a arches=("$@")
  printf 'Building JavaScript package entries\n'
  npm run build:js --workspace muon-ui

  printf 'Staging package targets\n'
  if is_full_arch_matrix "${arches[@]}"; then
    (cd muon-ui && node scripts/stage-muon-prepare.mjs --all)
  else
    for arch in "${arches[@]}"; do
      local target_name
      target_name="$(target_name_for_arch "${arch}")"
      (cd muon-ui && node scripts/stage-muon-prepare.mjs --target "${target_name}" --dist)
    done
    (cd muon-ui && node scripts/stage-muon-prepare.mjs --target windows32 --dist)
    (cd muon-ui && node scripts/stage-muon-prepare.mjs --target windows64 --dist)
  fi
}

validate_readelf_header() {
  local path="$1"
  local expected_class="$2"
  local expected_machine="$3"
  local header
  header="$(readelf -h "${path}")"
  if ! grep -Eq "^[[:space:]]*Class:[[:space:]]+${expected_class}$" <<< "${header}"; then
    fail "Unexpected ELF class in ${path}; expected ${expected_class}."
  fi
  if ! grep -Eq "^[[:space:]]*Machine:[[:space:]]+${expected_machine}$" <<< "${header}"; then
    fail "Unexpected ELF machine in ${path}; expected ${expected_machine}."
  fi
}

validate_linux_artifacts() {
  local -a arches=("$@")
  require_command readelf

  for arch in "${arches[@]}"; do
    local target_name
    local expected_class
    local expected_machine
    target_name="$(target_name_for_arch "${arch}")"
    case "${arch}" in
      amd64)
        expected_class="ELF64"
        expected_machine="Advanced Micro Devices X86-64"
        ;;
      armv7l)
        expected_class="ELF32"
        expected_machine="ARM"
        ;;
      arm64)
        expected_class="ELF64"
        expected_machine="AArch64"
        ;;
      *)
        fail "Unsupported architecture: ${arch}"
        ;;
    esac

    validate_readelf_header \
      "muon-ui/dist/native/${target_name}/muon-prepare" \
      "${expected_class}" \
      "${expected_machine}"
    validate_readelf_header \
      "muon-ui/dist/native/${target_name}/muon-bootstrap" \
      "${expected_class}" \
      "${expected_machine}"
    validate_readelf_header \
      "muon-ui/dist/runtime/${target_name}/muon-core" \
      "${expected_class}" \
      "${expected_machine}"
    validate_readelf_header \
      "muon-ui/dist/runtime/${target_name}/libmuon-ui.so" \
      "${expected_class}" \
      "${expected_machine}"
    validate_readelf_header \
      "muon-ui/dist/runtime/${target_name}/libcardio.so" \
      "${expected_class}" \
      "${expected_machine}"
  done
}

verify_full_runtime() {
  local -a arches=("$@")
  if is_full_arch_matrix "${arches[@]}"; then
    npm run verify:package-runtime --workspace muon-ui
  else
    printf 'Skipping full package runtime verification for partial architecture set: %s\n' "${ARCH_LIST}"
  fi
}

build_dist() {
  local -a arches=("$@")
  require_command "${CONTAINER_ENGINE}"
  require_command npm
  require_command node

  export MUON_PREPARE_VERSION
  export MUON_PREPARE_GIT_COMMIT_HASH
  export MUON_CORE_VERSION
  export MUON_CORE_GIT_COMMIT_HASH
  MUON_PREPARE_VERSION="$(package_version "muon-prepare/package.json")"
  MUON_PREPARE_GIT_COMMIT_HASH="$(git_commit_hash)"
  MUON_CORE_VERSION="$(package_version "muon-core/package.json")"
  MUON_CORE_GIT_COMMIT_HASH="${MUON_PREPARE_GIT_COMMIT_HASH}"

  assert_native_dependency_checkouts

  printf 'Preparing shared native source dependencies\n'
  bash muon-core/build_yyjson.sh
  bash muon-core/build_miniz.sh

  build_linux_targets "${arches[@]}"
  build_windows_targets
  stage_targets "${arches[@]}"
  validate_linux_artifacts "${arches[@]}"
  verify_full_runtime "${arches[@]}"
}

get_dry_run_files() {
  npm pack --workspace muon-ui --dry-run --ignore-scripts --json |
    node -e 'let input = ""; process.stdin.setEncoding("utf8"); process.stdin.on("data", chunk => input += chunk); process.stdin.on("end", () => { const packs = JSON.parse(input); for (const file of packs[0].files) console.log(file.path); });'
}

get_tarball_files() {
  local tarball="$1"
  tar -tzf "${tarball}" | sed -n 's#^package/##p'
}

verify_package_file_list() {
  local source_name="$1"
  local files="$2"

  require_pack_file() {
    local file="$1"
    if ! grep -Fxq "${file}" <<< "${files}"; then
      fail "${source_name} is missing required file: ${file}"
    fi
  }

  require_pack_file_match() {
    local pattern="$1"
    if ! grep -Eq "${pattern}" <<< "${files}"; then
      fail "${source_name} is missing required file matching: ${pattern}"
    fi
  }

  reject_pack_file() {
    local file="$1"
    if grep -Fxq "${file}" <<< "${files}"; then
      fail "${source_name} includes unexpected file: ${file}"
    fi
  }

  require_pack_file "muon.d.ts"
  require_pack_file "vite.d.ts"
  require_pack_file "dist/cli.cjs"
  require_pack_file "dist/native/linux64/muon-prepare"
  require_pack_file "dist/native/linuxarm/muon-prepare"
  require_pack_file "dist/native/linuxarm64/muon-prepare"
  require_pack_file "dist/native/windows32/muon-prepare.exe"
  require_pack_file "dist/native/windows64/muon-prepare.exe"
  require_pack_file "dist/runtime/linux64/libcardio.so"
  require_pack_file "dist/runtime/linuxarm/libcardio.so"
  require_pack_file "dist/runtime/linuxarm64/libcardio.so"
  require_pack_file "dist/runtime/windows32/libcardio.dll"
  require_pack_file "dist/runtime/windows64/libcardio.dll"
  require_pack_file_match '^dist/runtime/windows32/libgcc_s_.*-1\.dll$'
  require_pack_file "dist/runtime/windows32/libstdc++-6.dll"
  require_pack_file_match '^dist/runtime/windows64/libgcc_s_.*-1\.dll$'
  require_pack_file "dist/runtime/windows64/libstdc++-6.dll"

  reject_pack_file "dist/cli.mjs"
  reject_pack_file "dist/vite.d.ts"
  reject_pack_file "dist/native/linux32/muon-prepare"
  reject_pack_file "dist/runtime/linux64/muon-runtime.json"
  reject_pack_file "dist/runtime/linuxarm/muon-runtime.json"
  reject_pack_file "dist/runtime/linuxarm64/muon-runtime.json"
  reject_pack_file "dist/runtime/windows32/muon-runtime.json"
  reject_pack_file "dist/runtime/windows64/muon-runtime.json"
  reject_pack_file "dist/runtime/linux32/muon-runtime.json"

  if grep -Eq '^dist/.*\.d\.ts$' <<< "${files}"; then
    fail "${source_name} includes dist declaration files."
  fi
  if grep -Eq '\.d\.ts\.map$' <<< "${files}"; then
    fail "${source_name} includes declaration maps."
  fi
  if grep -Eq '^dist/(native|runtime)/linux32(/|$)' <<< "${files}"; then
    fail "${source_name} includes unsupported linux32 files."
  fi
}

verify_pack_file_list() {
  verify_package_file_list "npm pack dry run" "$(get_dry_run_files)"
}

verify_tarball_file_list() {
  local tarball="$1"
  verify_package_file_list "screw-up tarball" "$(get_tarball_files "${tarball}")"
}

pack_with_screw_up() {
  local pack_output
  local package_file_name
  local package_file_path

  pack_output="$(
    npm exec --workspace muon-ui -- screw-up pack \
      --pack-destination "${ARTIFACT_DIR}" \
      "${SCRIPT_DIR}/muon-ui"
  )"
  printf '%s\n' "${pack_output}" >&2
  package_file_name="$(
    grep -Eo '[^[:space:]]+\.tgz' <<< "${pack_output}" |
      tail -n 1
  )"
  [[ -n "${package_file_name}" ]] ||
    fail "screw-up pack did not report a generated tarball."
  if [[ "${package_file_name}" = /* ]]; then
    package_file_path="${package_file_name}"
  else
    package_file_path="${ARTIFACT_DIR}/${package_file_name}"
  fi
  [[ -f "${package_file_path}" ]] ||
    fail "screw-up pack did not create the expected tarball: ${package_file_path}"
  printf '%s\n' "${package_file_path}"
}

pack_npm_package() {
  local tarball

  require_command npm
  require_command node
  require_command tar
  npm run verify:package-runtime --workspace muon-ui
  verify_pack_file_list
  mkdir -p "${ARTIFACT_DIR}"
  tarball="$(pack_with_screw_up)"
  verify_tarball_file_list "${tarball}"
}

declare -a ARCHES=()
split_arch_list "${ARCH_LIST}" ARCHES

case "${PACKAGE_TARGET}" in
  dist)
    build_dist "${ARCHES[@]}"
    ;;
  npm)
    pack_npm_package
    ;;
  all)
    build_dist "${ARCHES[@]}"
    pack_npm_package
    ;;
esac
