#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OUTPUT_ROOT="${SCRIPT_DIR}"
cd "${PROJECT_ROOT}"

command -v cmake >/dev/null || { echo "cmake is required" >&2; exit 1; }
command -v ninja >/dev/null || { echo "ninja is required" >&2; exit 1; }

host_prepare_target() {
  local system
  local machine
  system="$(uname -s)"
  machine="$(uname -m)"
  case "${system}:${machine}" in
    Linux:x86_64|Linux:amd64)
      printf '%s\n' linux64
      ;;
    Linux:armv7l|Linux:armv7*|Linux:armhf)
      printf '%s\n' linuxarm
      ;;
    Linux:aarch64|Linux:arm64)
      printf '%s\n' linuxarm64
      ;;
    MINGW*:i686|MSYS*:i686|CYGWIN*:i686)
      printf '%s\n' windows32
      ;;
    MINGW*:x86_64|MSYS*:x86_64|CYGWIN*:x86_64)
      printf '%s\n' windows64
      ;;
    *)
      echo "Unsupported muon-prepare host: ${system} ${machine}" >&2
      return 1
      ;;
  esac
}

prepare_executable_name() {
  case "$1" in
    windows32|windows64)
      printf '%s\n' muon-prepare.exe
      ;;
    *)
      printf '%s\n' muon-prepare
      ;;
  esac
}

bootstrap_executable_name() {
  case "$1" in
    windows32|windows64)
      printf '%s\n' muon-bootstrap.exe
      ;;
    *)
      printf '%s\n' muon-bootstrap
      ;;
  esac
}

prepare_output_dir() {
  local usage="$1"
  local target_name="$2"
  local build_type="$3"
  local build_type_lower="${build_type,,}"
  case "${usage}" in
    dev|test|check)
      printf '%s\n' "${PROJECT_ROOT}/muon-prepare/.run/${usage}-${target_name}-${build_type_lower}"
      ;;
    dist)
      if [[ "${build_type}" == "Debug" ]]; then
        printf '%s\n' "${PROJECT_ROOT}/muon-prepare/dist-${target_name}-debug"
      else
        printf '%s\n' "${PROJECT_ROOT}/muon-prepare/dist-${target_name}"
      fi
      ;;
  esac
}

ensure_host_muon_prepare() {
  if [[ -n "${MUON_PREPARE_PATH:-}" ]]; then
    printf '%s\n' "${MUON_PREPARE_PATH}"
    return
  fi
  local host_target
  host_target="${MUON_HOST_PREPARE_TARGET:-$(host_prepare_target)}"
  local executable_name
  executable_name="$(prepare_executable_name "${host_target}")"
  bash "${PROJECT_ROOT}/muon-prepare/build.sh" dev Debug "${host_target}" >&2
  printf '%s\n' "${PROJECT_ROOT}/muon-prepare/.run/dev-${host_target}-debug/${executable_name}"
}

BUILD_USAGE="${1:-dev}"
USAGE="Usage: $0 [dev|test|check|dist] [Debug|Release] [linux64|linuxarm|linuxarm64|mingw32|mingw64|win32|win64|windows32|windows64]"
case "${BUILD_USAGE}" in
  dev|test|check|dist)
    ;;
  *)
    echo "${USAGE}" >&2
    exit 1
    ;;
esac

DEFAULT_BUILD_TYPE="Release"
case "${BUILD_USAGE}" in
  dev|test)
    DEFAULT_BUILD_TYPE="Debug"
    ;;
esac
BUILD_TYPE="${2:-${DEFAULT_BUILD_TYPE}}"
case "${BUILD_TYPE}" in
  Debug|Release)
    ;;
  *)
    echo "${USAGE}" >&2
    exit 1
    ;;
esac

TARGET="${3:-linux64}"
EXTRA_CMAKE_ARGS=("${@:4}")
TOOLCHAIN_ARGS=()
PROJECT_ARCH_ARGS=()
LIBFFI_CACHE_ARGS=()
LIBFFI_ARGS=()
YYJSON_VERSION="0.12.0"
YYJSON_ARGS=()
MINIZ_VERSION="3.1.1"
MINIZ_ARGS=()
case "${TARGET}" in
  linux64|amd64|x64)
    CEF_ARCH="linux64"
    TARGET_NAME="linux64"
    PROJECT_ARCH_ARGS=("-DPROJECT_ARCH=x86_64")
    ;;
  linuxarm|armv7l|armv7|armhf|arm)
    CEF_ARCH="linuxarm"
    TARGET_NAME="linuxarm"
    PROJECT_ARCH_ARGS=("-DPROJECT_ARCH=arm")
    ;;
  linuxarm64|arm64|aarch64)
    CEF_ARCH="linuxarm64"
    TARGET_NAME="linuxarm64"
    PROJECT_ARCH_ARGS=("-DPROJECT_ARCH=arm64")
    ;;
  linux32|i686|i386|ia32|x86)
    echo "Unsupported target: ${TARGET}. Linux 32-bit CEF builds are discontinued after CEF 101." >&2
    exit 1
    ;;
  mingw32|win32|windows32)
    CEF_ARCH="windows32"
    TARGET_NAME="windows32"
    TOOLCHAIN_ARGS=("-DCMAKE_TOOLCHAIN_FILE=${SCRIPT_DIR}/cmake/toolchains/mingw32.cmake")
    ;;
  mingw64|win64|windows64)
    CEF_ARCH="windows64"
    TARGET_NAME="windows64"
    TOOLCHAIN_ARGS=("-DCMAKE_TOOLCHAIN_FILE=${SCRIPT_DIR}/cmake/toolchains/mingw64.cmake")
    ;;
  *)
    echo "${USAGE}" >&2
    exit 1
    ;;
esac

case "${TARGET_NAME}" in
  windows32)
    command -v i686-w64-mingw32-g++ >/dev/null || { echo "i686-w64-mingw32-g++ is required" >&2; exit 1; }
    "${SCRIPT_DIR}/build_libffi_mingw32.sh" mingw32
    LIBFFI_CACHE_ARGS=("-U" "LIBFFI_*" "-U" "pkgcfg_lib_LIBFFI_*")
    LIBFFI_ARGS=("-DLIBFFI_ROOT=${OUTPUT_ROOT}/.deps/libffi-mingw32")
    ;;
  windows64)
    command -v x86_64-w64-mingw32-g++ >/dev/null || { echo "x86_64-w64-mingw32-g++ is required" >&2; exit 1; }
    "${SCRIPT_DIR}/build_libffi_mingw32.sh" mingw64
    LIBFFI_CACHE_ARGS=("-U" "LIBFFI_*" "-U" "pkgcfg_lib_LIBFFI_*")
    LIBFFI_ARGS=("-DLIBFFI_ROOT=${OUTPUT_ROOT}/.deps/libffi-mingw64")
    ;;
esac

bash "${SCRIPT_DIR}/build_yyjson.sh"
YYJSON_ARGS=("-DYYJSON_SOURCE_DIR=${OUTPUT_ROOT}/.deps/src/yyjson-${YYJSON_VERSION}/src")
bash "${SCRIPT_DIR}/build_miniz.sh"
MINIZ_ARGS=("-DMINIZ_SOURCE_DIR=${OUTPUT_ROOT}/.deps/src/miniz-${MINIZ_VERSION}")

CEF_VERSION="147.0.14+g76d2442+chromium-147.0.7727.138"
CEF_DIST_SUFFIX="${CEF_ARCH}_minimal"
CEF_PACKAGE="cef_binary_${CEF_VERSION}_${CEF_DIST_SUFFIX}.tar.bz2"
CEF_CACHE_DIR="${OUTPUT_ROOT}/.cef"
CEF_ROOT="${CEF_CACHE_DIR}/cef_binary_${CEF_VERSION}_${CEF_DIST_SUFFIX}"
HOST_MUON_PREPARE="$(ensure_host_muon_prepare)"
CEF_PREPARE_JSON_FILE="$(mktemp)"
CEF_PREPARE_METADATA_FILE="$(mktemp)"
"${HOST_MUON_PREPARE}" buildtime \
  --version "${CEF_VERSION}" \
  --target "${TARGET_NAME}" \
  --output-dir "${CEF_ROOT}" \
  --json >"${CEF_PREPARE_JSON_FILE}"
cmake \
  -DINPUT="${CEF_PREPARE_JSON_FILE}" \
  -DOUTPUT="${CEF_PREPARE_METADATA_FILE}" \
  -P "${SCRIPT_DIR}/cmake/extract_cef_prepare_metadata.cmake"
mapfile -t CEF_PREPARE_METADATA <"${CEF_PREPARE_METADATA_FILE}"
rm -f "${CEF_PREPARE_JSON_FILE}" "${CEF_PREPARE_METADATA_FILE}"
CEF_PACKAGE="${CEF_PREPARE_METADATA[0]}"
CEF_PACKAGE_URL="${CEF_PREPARE_METADATA[1]}"
expected="${CEF_PREPARE_METADATA[2]}"
CEF_ARCHIVE_SIZE="${CEF_PREPARE_METADATA[3]}"

BUILD_TYPE_LOWER="${BUILD_TYPE,,}"
BUILD_DIR="${OUTPUT_ROOT}/.build/${BUILD_USAGE}/${TARGET_NAME}/${BUILD_TYPE_LOWER}"
CONFIG_TEMPLATE="${SCRIPT_DIR}/config/muon.dev.json"
if [[ "${TARGET_NAME}" == linux* ]]; then
  CONFIG_TEMPLATE="${SCRIPT_DIR}/config/muon.dev.linux.json"
fi
CONFIG_COPY_MODE="always"
RUNTIME_INCLUDE_APP_FILES="ON"
BUILD_TESTS="OFF"
case "${BUILD_USAGE}" in
  dev)
    RUNTIME_DIR="${OUTPUT_ROOT}/.run/dev-${TARGET_NAME}-${BUILD_TYPE_LOWER}"
    CONFIG_COPY_MODE="if_missing"
    ;;
  test)
    RUNTIME_DIR="${OUTPUT_ROOT}/.run/test-${TARGET_NAME}-${BUILD_TYPE_LOWER}"
    BUILD_TESTS="ON"
    rm -rf "${RUNTIME_DIR}"
    ;;
  check)
    RUNTIME_DIR="${OUTPUT_ROOT}/.run/check-${TARGET_NAME}-${BUILD_TYPE_LOWER}"
    rm -rf "${RUNTIME_DIR}"
    ;;
  dist)
    CONFIG_COPY_MODE="none"
    RUNTIME_INCLUDE_APP_FILES="OFF"
    if [[ "${BUILD_TYPE}" == "Debug" ]]; then
      RUNTIME_DIR="${OUTPUT_ROOT}/dist-${TARGET_NAME}-debug"
    else
      RUNTIME_DIR="${OUTPUT_ROOT}/dist-${TARGET_NAME}"
    fi
    rm -rf "${RUNTIME_DIR}"
    ;;
esac

cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" -G Ninja \
  "${LIBFFI_CACHE_ARGS[@]}" \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
  -DCEF_ROOT="${CEF_ROOT}" \
  -DMUON_RUNTIME_DIR="${RUNTIME_DIR}" \
  -DMUON_CONFIG_TEMPLATE="${CONFIG_TEMPLATE}" \
  -DMUON_CONFIG_COPY_MODE="${CONFIG_COPY_MODE}" \
  -DMUON_RUNTIME_INCLUDE_APP_FILES="${RUNTIME_INCLUDE_APP_FILES}" \
  -DMUON_TARGET_NAME="${TARGET_NAME}" \
  -DMUON_CEF_VERSION="${CEF_VERSION}" \
  -DMUON_CEF_PACKAGE="${CEF_PACKAGE}" \
  -DMUON_CEF_PACKAGE_URL="${CEF_PACKAGE_URL}" \
  -DMUON_CEF_SHA1="${expected}" \
  -DMUON_CEF_SIZE="${CEF_ARCHIVE_SIZE}" \
  -DMUON_CORE_VERSION="${MUON_CORE_VERSION:-}" \
  -DMUON_CORE_GIT_COMMIT_HASH="${MUON_CORE_GIT_COMMIT_HASH:-}" \
  -DMUON_BUILD_TESTS="${BUILD_TESTS}" \
  -DMUON_ENABLE_SANITIZERS=OFF \
  -DMUON_TRACK_FFI_CLOSURES=OFF \
  "${LIBFFI_ARGS[@]}" \
  "${YYJSON_ARGS[@]}" \
  "${MINIZ_ARGS[@]}" \
  "${PROJECT_ARCH_ARGS[@]}" \
  "${TOOLCHAIN_ARGS[@]}" \
  "${EXTRA_CMAKE_ARGS[@]}"
cmake --build "${BUILD_DIR}" --parallel "$(nproc)"

MUON_RUNTIME_INFO_HEADER="${BUILD_DIR}/generated/muon_runtime_info_generated.h" \
  bash "${PROJECT_ROOT}/muon-prepare/build.sh" \
    "${BUILD_USAGE}" \
    "${BUILD_TYPE}" \
    "${TARGET_NAME}"

BOOTSTRAP_EXECUTABLE_NAME="$(bootstrap_executable_name "${TARGET_NAME}")"
PREPARE_OUTPUT_DIR="$(prepare_output_dir "${BUILD_USAGE}" "${TARGET_NAME}" "${BUILD_TYPE}")"
cp -f \
  "${PREPARE_OUTPUT_DIR}/${BOOTSTRAP_EXECUTABLE_NAME}" \
  "${RUNTIME_DIR}/${BOOTSTRAP_EXECUTABLE_NAME}"
