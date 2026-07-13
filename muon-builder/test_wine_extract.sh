#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

required_commands=(
  wine
  winepath
  wineserver
  x86_64-w64-mingw32-gcc
  x86_64-w64-mingw32-ar
  x86_64-w64-mingw32-ranlib
  x86_64-w64-mingw32-objdump
  node
  tar
)

for command_name in "${required_commands[@]}"; do
  if ! command -v "${command_name}" >/dev/null; then
    echo "Skipping Wine extraction test: ${command_name} is not available."
    exit 0
  fi
done

MUON_BUILDER_VERSION=1.2.3-beta \
  MUON_BUILDER_GIT_COMMIT_HASH=builder-test-hash \
  bash "${SCRIPT_DIR}/build.sh" check Release windows-amd64

bootstrap_executable="${SCRIPT_DIR}/.run/check-windows-amd64-release/muon-bootstrap.exe"
bootstrap_dump="${SCRIPT_DIR}/.run/check-windows-amd64-release/muon-bootstrap-resources.txt"
x86_64-w64-mingw32-objdump -x "${bootstrap_executable}" > "${bootstrap_dump}"
sed -n '/The \.rsrc Resource Directory section:/,/Sections:/p' \
  "${bootstrap_dump}" > "${bootstrap_dump}.section"
if grep -Fq 'Entry: ID: 0x000003' "${bootstrap_dump}.section" ||
    grep -Fq 'Entry: ID: 0x00000e' "${bootstrap_dump}.section"; then
  echo "muon-bootstrap.exe contains deprecated windres icon resources." >&2
  exit 1
fi
if ! grep -Fq 'Entry: ID: 0x000010' "${bootstrap_dump}.section" ||
    ! grep -Fq 'Entry: ID: 0x000018' "${bootstrap_dump}.section"; then
  echo "muon-bootstrap.exe is missing Windows version or manifest resources." >&2
  exit 1
fi
node "${SCRIPT_DIR}/scripts/assert-windows-version.mjs" \
  "${bootstrap_executable}" \
  --file-version \
  1.2.3.0 \
  --product-version \
  1.2.3.0 \
  "ProductName=muon" \
  "CompanyName=Kouji Matsui. (@kekyo@mi.kekyo.net)" \
  "FileDescription=muon Bootstrap" \
  "FileVersion=1.2.3-beta" \
  "ProductVersion=1.2.3-beta" \
  "InternalName=muon-bootstrap" \
  "OriginalFilename=muon-bootstrap.exe" \
  "PrivateBuild=builder-test-hash" \
  "Comments=https://muon-ui.com/ target=windows-amd64; cef=; cefTarget=; cefApi=" \
  "SpecialBuild=cefArtifact=; distribution=; apiHash="

temp_dir="$(mktemp -d)"
trap 'rm -rf "${temp_dir}"' EXIT

slow_writer_src="${temp_dir}/slow-writer.c"
slow_writer_exe="${temp_dir}/slow-writer.exe"
progress_harness_src="${temp_dir}/process-progress-harness.c"
progress_harness_exe="${temp_dir}/process-progress-harness.exe"
progress_output="${temp_dir}/process-progress-output.bin"
source_dir="${temp_dir}/source"
cache_dir="${temp_dir}/cache"
output_dir="${temp_dir}/output"
wine_prefix="${temp_dir}/wineprefix"
archive_root="${source_dir}/cef_binary_fake_windows64_minimal"
archive_path="${source_dir}/cef.tar.bz2"
catalog_path="${source_dir}/source-catalog.json"
executable="${SCRIPT_DIR}/.run/check-windows-amd64-release/muon-builder.exe"
node "${SCRIPT_DIR}/scripts/assert-windows-version.mjs" \
  "${executable}" \
  --file-version \
  1.2.3.0 \
  --product-version \
  1.2.3.0 \
  "ProductName=muon" \
  "CompanyName=Kouji Matsui. (@kekyo@mi.kekyo.net)" \
  "FileDescription=muon Builder Tool" \
  "FileVersion=1.2.3-beta" \
  "ProductVersion=1.2.3-beta" \
  "InternalName=muon-builder" \
  "OriginalFilename=muon-builder.exe" \
  "PrivateBuild=builder-test-hash" \
  "Comments=https://muon-ui.com/ target=windows-amd64; cef=; cefTarget=; cefApi=" \
  "SpecialBuild=cefArtifact=; distribution=; apiHash="

cat > "${slow_writer_src}" <<'C_EOF'
#define WIN32_LEAN_AND_MEAN
#include <stdio.h>
#include <windows.h>

int main(int argc, char **argv) {
  if (argc != 2) {
    return 64;
  }
  FILE *file = fopen(argv[1], "wb");
  if (file == NULL) {
    return 1;
  }
  for (int index = 0; index < 4; index += 1) {
    if (fwrite("0123456789", 1, 10, file) != 10) {
      fclose(file);
      return 1;
    }
    fflush(file);
    Sleep(250);
  }
  fclose(file);
  return 0;
}
C_EOF

cat > "${progress_harness_src}" <<'C_EOF'
#include <stdio.h>

#include "common.h"

typedef struct {
  int seen_intermediate;
} ProgressState;

static void on_progress(const MuonPrepareProgress *progress,
                        void *user_data) {
  ProgressState *state = (ProgressState *)user_data;
  printf("%llu/%llu\n", progress->current, progress->total);
  if (progress->determinate && progress->current > 0 &&
      progress->current < progress->total) {
    state->seen_intermediate = 1;
  }
}

int main(int argc, char **argv) {
  if (argc != 3) {
    return 64;
  }
  char *child_argv[] = {argv[1], argv[2], NULL};
  ProgressState state = {0};
  const int result = muon_run_process_with_file_progress(
      child_argv, argv[2], 40, on_progress, &state,
      MUON_PREPARE_PROGRESS_PHASE_DOWNLOADING,
      "Downloading CEF runtime...");
  if (result != 0) {
    return 1;
  }
  if (!state.seen_intermediate) {
    fprintf(stderr, "missing intermediate file progress\n");
    return 2;
  }
  return 0;
}
C_EOF

x86_64-w64-mingw32-gcc -std=c99 -O0 -g -Wall -Wextra -pedantic \
  -o "${slow_writer_exe}" "${slow_writer_src}"

x86_64-w64-mingw32-gcc -std=c99 -O0 -g -Wall -Wextra -pedantic \
  -I"${SCRIPT_DIR}/src" \
  -I"${SCRIPT_DIR}/../deps/sha2" \
  -I"${SCRIPT_DIR}/.deps/src/yyjson-0.12.0/src" \
  -o "${progress_harness_exe}" \
  "${progress_harness_src}" \
  "${SCRIPT_DIR}/.run/check-windows-amd64-release/libmuon-builder.a" \
  "${SCRIPT_DIR}/.deps/build/libarchive-windows64/libarchive/libarchive.a" \
  "${SCRIPT_DIR}/.deps/build/bzip2-windows64/libbz2.a"

slow_writer_windows="$(winepath -w "${slow_writer_exe}")"
progress_output_windows="$(winepath -w "${progress_output}")"
WINEDEBUG=-all WINEPREFIX="${wine_prefix}" \
  wine "${progress_harness_exe}" \
    "${slow_writer_windows}" \
    "${progress_output_windows}" > "${temp_dir}/progress.log"
WINEPREFIX="${wine_prefix}" wineserver -w

mkdir -p "${archive_root}/Release" "${archive_root}/Resources/locales"
printf 'cef library\r\n' > "${archive_root}/Release/libcef.dll"
printf 'cef data\r\n' > "${archive_root}/Resources/icudtl.dat"
printf 'locale\r\n' > "${archive_root}/Resources/locales/en-US.pak"
tar -cjf "${archive_path}" -C "${source_dir}" \
  "cef_binary_fake_windows64_minimal"

node - "${archive_path}" "${catalog_path}" <<'NODE'
const { createHash } = require("node:crypto");
const { readFileSync, statSync, writeFileSync } = require("node:fs");

const [archivePath, catalogPath] = process.argv.slice(2);
const archive = readFileSync(archivePath);
const catalog = {
  windows64: {
    versions: [
      {
        cef_version: "fake-cef",
        channel: "stable",
        chromium_version: "fake-chromium",
        files: [
          {
            name: "cef.tar.bz2",
            sha1: createHash("sha1").update(archive).digest("hex"),
            size: statSync(archivePath).size,
            type: "minimal",
          },
        ],
      },
    ],
  },
};
writeFileSync(catalogPath, `${JSON.stringify(catalog, null, 2)}\n`);
NODE

mkdir -p "${cache_dir}" "${output_dir}"
catalog_windows="$(winepath -w "${catalog_path}")"
cache_windows="$(winepath -w "${cache_dir}")"
output_windows="$(winepath -w "${output_dir}")"

WINEDEBUG=-all WINEPREFIX="${wine_prefix}" MUON_CEF_CATALOG_URL="${catalog_windows}" \
  wine "${executable}" buildtime \
    --version fake-cef \
    --target windows-amd64 \
    --output-dir "${output_windows}" \
    --cache-dir "${cache_windows}" \
    --quiet \
    --json > "${temp_dir}/result.json"
WINEPREFIX="${wine_prefix}" wineserver -w

node - "${temp_dir}/result.json" "${output_dir}" <<'NODE'
const { accessSync, readFileSync } = require("node:fs");
const { join } = require("node:path");

const [resultPath, outputDir] = process.argv.slice(2);
const result = JSON.parse(readFileSync(resultPath, "utf8"));
if (result.version !== "fake-cef" || result.target !== "windows-amd64") {
  throw new Error("Wine extraction result JSON did not contain the expected target.");
}
for (const relativePath of [
  "Release/libcef.dll",
  "Resources/icudtl.dat",
  "Resources/locales/en-US.pak",
  ".muon-cef-ready.json",
]) {
  accessSync(join(outputDir, relativePath));
}
NODE
