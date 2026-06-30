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
      printf '%s\n' linux-amd64
      ;;
    Linux:armv7l|Linux:armv7*|Linux:armhf)
      printf '%s\n' linux-armhf
      ;;
    Linux:aarch64|Linux:arm64)
      printf '%s\n' linux-arm64
      ;;
    MINGW*:i686|MSYS*:i686|CYGWIN*:i686)
      printf '%s\n' windows-i686
      ;;
    MINGW*:x86_64|MSYS*:x86_64|CYGWIN*:x86_64)
      printf '%s\n' windows-amd64
      ;;
    *)
      echo "Unsupported muon-prepare host: ${system} ${machine}" >&2
      return 1
      ;;
  esac
}

prepare_executable_name() {
  case "$1" in
    windows-i686|windows-amd64)
      printf '%s\n' muon-prepare.exe
      ;;
    *)
      printf '%s\n' muon-prepare
      ;;
  esac
}

bootstrap_executable_name() {
  case "$1" in
    windows-i686|windows-amd64)
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

generate_core_version_header() {
  local output_path="$1"
  mkdir -p "$(dirname "${output_path}")"
  if [[ -n "${MUON_CORE_VERSION_HEADER:-}" ]]; then
    if [[ "${MUON_CORE_VERSION_HEADER}" != "${output_path}" ]]; then
      cp "${MUON_CORE_VERSION_HEADER}" "${output_path}"
    fi
    return
  fi
  command -v npm >/dev/null || { echo "npm is required" >&2; exit 1; }
  npm run generate:runtime-version-header --workspace muon-core -- \
    "${output_path}" >/dev/null
}

verify_windows_icon_resources() {
  local executable_path="$1"
  local label="$2"
  local dump_path
  local resources_path
  dump_path="$(mktemp)"
  resources_path="$(mktemp)"
  "${OBJDUMP}" -x "${executable_path}" >"${dump_path}"
  sed -n '/The \.rsrc Resource Directory section:/,/Sections:/p' \
    "${dump_path}" >"${resources_path}"
  if ! grep -Fq 'Entry: ID: 0x000003' "${resources_path}" ||
      ! grep -Fq 'Entry: ID: 0x00000e' "${resources_path}"; then
    echo "${label} is missing Windows icon resources: ${executable_path}" >&2
    rm -f "${dump_path}" "${resources_path}"
    return 1
  fi
  rm -f "${dump_path}" "${resources_path}"
}

read_c_string_define() {
  local header_path="$1"
  local define_name="$2"
  sed -n "s/^#define ${define_name} \"\\(.*\\)\"/\\1/p" "${header_path}" | head -n 1
}

normalize_windows_version() {
  node -e '
const version = process.argv[1] ?? "";
const match = /^(\d+)(?:\.(\d+))?(?:\.(\d+))?(?:\.(\d+))?/.exec(version);
if (match === null) {
  console.error(`Invalid Windows version: ${version}`);
  process.exit(1);
}
const values = [match[1], match[2], match[3], match[4]]
  .map((value) => value === undefined ? 0 : Number.parseInt(value, 10));
for (const value of values) {
  if (!Number.isInteger(value) || value < 0 || value > 65535) {
    console.error(`Invalid Windows version: ${version}`);
    process.exit(1);
  }
}
console.log(values.join("."));
' "$1"
}

resolve_cef_api_version() {
  if [[ -n "${MUON_CEF_API_VERSION:-}" ]]; then
    printf '%s\n' "${MUON_CEF_API_VERSION}"
    return
  fi
  sed -n 's/^#define CEF_API_VERSION_LAST CEF_API_VERSION_\([0-9][0-9]*\)$/\1/p' \
    "${CEF_ROOT}/include/cef_api_versions.h" | head -n 1
}

resolve_cef_api_hash() {
  local api_version="$1"
  sed -n "s/^#define CEF_API_HASH_${api_version} \"\\([^\"]*\\)\".*$/\\1/p" \
    "${CEF_ROOT}/include/cef_api_versions.h" | head -n 1
}

verify_windows_version_resource() {
  local executable_path="$1"
  local file_description="$2"
  local internal_name="$3"
  local original_filename="$4"
  local version
  local fixed_version
  local git_commit_hash

  version="$(read_c_string_define "${MUON_CORE_VERSION_HEADER_PATH}" MUON_CORE_VERSION)"
  fixed_version="$(normalize_windows_version "${version}")"
  git_commit_hash="$(read_c_string_define "${MUON_CORE_VERSION_HEADER_PATH}" MUON_CORE_GIT_COMMIT_HASH)"
  node "${PROJECT_ROOT}/muon-prepare/scripts/assert-windows-version.mjs" \
    "${executable_path}" \
    --file-version \
    "${fixed_version}" \
    --product-version \
    "${fixed_version}" \
    "ProductName=Muon" \
    "CompanyName=Kouji Matsui. (@kekyo@mi.kekyo.net)" \
    "FileDescription=${file_description}" \
    "FileVersion=${version}" \
    "ProductVersion=${version}" \
    "InternalName=${internal_name}" \
    "OriginalFilename=${original_filename}" \
    "PrivateBuild=${git_commit_hash}" \
    "Comments=https://muon-ui.com/ target=${TARGET_NAME}; cef=${CEF_VERSION}; cefTarget=${CEF_ARCH}; cefApi=${MUON_CEF_API_VERSION}" \
    "SpecialBuild=cefArtifact=${CEF_PACKAGE}; distribution=minimal; apiHash=${MUON_CEF_API_HASH}"
}

patch_windows_cef_popup_settings() {
  if [[ "${TARGET_NAME}" != windows* ]]; then
    return
  fi

  local life_span_source_path="${CEF_ROOT}/libcef_dll/cpptoc/life_span_handler_cpptoc.cc"
  local browser_view_delegate_source_path="${CEF_ROOT}/libcef_dll/cpptoc/views/browser_view_delegate_cpptoc.cc"

  node -e '
const fs = require("fs");
const patches = [
  {
    sourcePath: process.argv[1],
    marker: "Muon: tolerate CEF Windows popup settings without size",
  },
  {
    sourcePath: process.argv[2],
    marker: "Muon: tolerate CEF Windows browser view settings without size",
  },
];
const guardPattern = /  if \(!template_util::has_valid_size\(settings\)\) {\n    DCHECK\(false\) << "invalid settings->\[base\.\]size";\n    return(?: 0| NULL)?;\n  }/g;
for (const patch of patches) {
  if (!fs.existsSync(patch.sourcePath)) {
    continue;
  }
  let source = fs.readFileSync(patch.sourcePath, "utf8");
  const replacement = `  if (!template_util::has_valid_size(settings)) {
    // ${patch.marker}.
    const_cast<struct _cef_browser_settings_t*>(settings)->size = sizeof(*settings);
  }`;
  let replacementCount = 0;
  source = source.replace(guardPattern, () => {
    replacementCount += 1;
    return replacement;
  });
  if (replacementCount === 0 && !source.includes(patch.marker)) {
    throw new Error(`CEF settings guard was not found: ${patch.sourcePath}`);
  }
  fs.writeFileSync(patch.sourcePath, source);
}
' "${life_span_source_path}" "${browser_view_delegate_source_path}"
}

BUILD_USAGE="${1:-dev}"
USAGE="Usage: $0 [dev|test|check|dist] [Debug|Release] [linux-amd64|linux-armhf|linux-arm64|windows-i686|windows-amd64]"
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

TARGET="${3:-linux-amd64}"
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
  linux-amd64)
    CEF_ARCH="linux64"
    TARGET_NAME="linux-amd64"
    PROJECT_ARCH_ARGS=("-DPROJECT_ARCH=x86_64")
    ;;
  linux-armhf)
    CEF_ARCH="linuxarm"
    TARGET_NAME="linux-armhf"
    PROJECT_ARCH_ARGS=("-DPROJECT_ARCH=arm")
    ;;
  linux-arm64)
    CEF_ARCH="linuxarm64"
    TARGET_NAME="linux-arm64"
    PROJECT_ARCH_ARGS=("-DPROJECT_ARCH=arm64")
    ;;
  windows-i686)
    CEF_ARCH="windows32"
    TARGET_NAME="windows-i686"
    OBJDUMP="${OBJDUMP:-i686-w64-mingw32-objdump}"
    TOOLCHAIN_ARGS=("-DCMAKE_TOOLCHAIN_FILE=${SCRIPT_DIR}/cmake/toolchains/mingw32.cmake")
    ;;
  windows-amd64)
    CEF_ARCH="windows64"
    TARGET_NAME="windows-amd64"
    OBJDUMP="${OBJDUMP:-x86_64-w64-mingw32-objdump}"
    TOOLCHAIN_ARGS=("-DCMAKE_TOOLCHAIN_FILE=${SCRIPT_DIR}/cmake/toolchains/mingw64.cmake")
    ;;
  *)
    echo "${USAGE}" >&2
    exit 1
    ;;
esac

case "${TARGET_NAME}" in
  windows-i686)
    command -v i686-w64-mingw32-g++ >/dev/null || { echo "i686-w64-mingw32-g++ is required" >&2; exit 1; }
    command -v "${OBJDUMP}" >/dev/null || { echo "${OBJDUMP} is required" >&2; exit 1; }
    "${SCRIPT_DIR}/build_libffi_mingw32.sh" mingw32
    LIBFFI_CACHE_ARGS=("-U" "LIBFFI_*" "-U" "pkgcfg_lib_LIBFFI_*")
    LIBFFI_ARGS=("-DLIBFFI_ROOT=${OUTPUT_ROOT}/.deps/libffi-mingw32")
    ;;
  windows-amd64)
    command -v x86_64-w64-mingw32-g++ >/dev/null || { echo "x86_64-w64-mingw32-g++ is required" >&2; exit 1; }
    command -v "${OBJDUMP}" >/dev/null || { echo "${OBJDUMP} is required" >&2; exit 1; }
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
patch_windows_cef_popup_settings

BUILD_TYPE_LOWER="${BUILD_TYPE,,}"
BUILD_DIR="${OUTPUT_ROOT}/.build/${BUILD_USAGE}/${TARGET_NAME}/${BUILD_TYPE_LOWER}"
MUON_CORE_VERSION_HEADER_PATH="${BUILD_DIR}/generated/muon_core_version_generated.h"
generate_core_version_header "${MUON_CORE_VERSION_HEADER_PATH}"
MUON_CORE_VERSION_VALUE="$(read_c_string_define "${MUON_CORE_VERSION_HEADER_PATH}" MUON_CORE_VERSION)"
MUON_CORE_GIT_COMMIT_HASH_VALUE="$(read_c_string_define "${MUON_CORE_VERSION_HEADER_PATH}" MUON_CORE_GIT_COMMIT_HASH)"
MUON_CEF_API_VERSION="$(resolve_cef_api_version)"
MUON_CEF_API_HASH="$(resolve_cef_api_hash "${MUON_CEF_API_VERSION}")"
CONFIG_TEMPLATE="${SCRIPT_DIR}/config/muon.dev.json"
if [[ "${TARGET_NAME}" == linux-* ]]; then
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
  -DMUON_CEF_TARGET_NAME="${CEF_ARCH}" \
  -DMUON_CEF_VERSION="${CEF_VERSION}" \
  -DMUON_CEF_PACKAGE="${CEF_PACKAGE}" \
  -DMUON_CEF_PACKAGE_URL="${CEF_PACKAGE_URL}" \
  -DMUON_CEF_SHA1="${expected}" \
  -DMUON_CEF_SIZE="${CEF_ARCHIVE_SIZE}" \
  -DMUON_CEF_API_VERSION="${MUON_CEF_API_VERSION}" \
  -DMUON_CORE_VERSION_HEADER="${MUON_CORE_VERSION_HEADER_PATH}" \
  -DMUON_CORE_VERSION="${MUON_CORE_VERSION_VALUE}" \
  -DMUON_CORE_GIT_COMMIT_HASH="${MUON_CORE_GIT_COMMIT_HASH_VALUE}" \
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
  MUON_CORE_VERSION_HEADER="${MUON_CORE_VERSION_HEADER_PATH}" \
  MUON_WINDOWS_RESOURCE_VERSION="${MUON_CORE_VERSION_VALUE}" \
  MUON_WINDOWS_RESOURCE_GIT_COMMIT_HASH="${MUON_CORE_GIT_COMMIT_HASH_VALUE}" \
  MUON_WINDOWS_RESOURCE_TARGET="${TARGET_NAME}" \
  MUON_WINDOWS_RESOURCE_CEF_VERSION="${CEF_VERSION}" \
  MUON_WINDOWS_RESOURCE_CEF_TARGET="${CEF_ARCH}" \
  MUON_WINDOWS_RESOURCE_CEF_API_VERSION="${MUON_CEF_API_VERSION}" \
  MUON_WINDOWS_RESOURCE_CEF_ARTIFACT="${CEF_PACKAGE}" \
  MUON_WINDOWS_RESOURCE_CEF_DISTRIBUTION="minimal" \
  MUON_WINDOWS_RESOURCE_CEF_API_HASH="${MUON_CEF_API_HASH}" \
  bash "${PROJECT_ROOT}/muon-prepare/build.sh" \
    "${BUILD_USAGE}" \
    "${BUILD_TYPE}" \
    "${TARGET_NAME}"

BOOTSTRAP_EXECUTABLE_NAME="$(bootstrap_executable_name "${TARGET_NAME}")"
PREPARE_OUTPUT_DIR="$(prepare_output_dir "${BUILD_USAGE}" "${TARGET_NAME}" "${BUILD_TYPE}")"
cp -f \
  "${PREPARE_OUTPUT_DIR}/${BOOTSTRAP_EXECUTABLE_NAME}" \
  "${RUNTIME_DIR}/${BOOTSTRAP_EXECUTABLE_NAME}"

if [[ "${TARGET_NAME}" == windows-* ]]; then
  verify_windows_icon_resources "${RUNTIME_DIR}/muon-core.exe" "muon-core"
  verify_windows_version_resource \
    "${RUNTIME_DIR}/muon-core.exe" \
    "Muon Core Runtime" \
    "muon-core" \
    "muon-core.exe"
  verify_windows_icon_resources \
    "${RUNTIME_DIR}/${BOOTSTRAP_EXECUTABLE_NAME}" \
    "muon-bootstrap"
  verify_windows_version_resource \
    "${RUNTIME_DIR}/${BOOTSTRAP_EXECUTABLE_NAME}" \
    "Muon Bootstrap" \
    "muon-bootstrap" \
    "${BOOTSTRAP_EXECUTABLE_NAME}"
fi
