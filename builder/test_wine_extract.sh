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

temp_dir="$(mktemp -d)"
trap 'rm -rf "${temp_dir}"' EXIT

runtime_info_header="${temp_dir}/muon_runtime_info_generated.h"
cat > "${runtime_info_header}" <<'C_EOF'
// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

#ifndef MUON_RUNTIME_INFO_GENERATED_H
#define MUON_RUNTIME_INFO_GENERATED_H

#include <stddef.h>

#define MUON_RUNTIME_INFO_AVAILABLE 1

typedef struct {
  const char *file_name;
  const char *url;
  const char *sha1;
  unsigned long long size;
} MuonRuntimeCefArtifactInfo;

typedef struct {
  const char *name;
  const char *executable_name;
  const char *target;
  const char *cef_target;
  const char *muon_core_version;
  const char *muon_core_git_commit_hash;
  const char *muon_core_build_date;
  const char *muon_core_git_commit_date;
  const char *cef_reference_version;
  const char *cef_reference_distribution;
  int cef_reference_api_version;
  const char *cef_reference_api_hash;
  MuonRuntimeCefArtifactInfo cef_reference_artifact;
  const char *const *core_payload;
  size_t core_payload_count;
} MuonRuntimeInfo;

static const MuonRuntimeInfo kMuonRuntimeInfo = {
    "muon-core",
    "muon-core.exe",
    "windows-amd64",
    "windows64",
    "fake-muon-core",
    "fake-core-git-commit",
    "2026-07-24T00:00:00+09:00",
    "2026-07-01T00:00:00+09:00",
    "fake-cef",
    "minimal",
    14700,
    "fake-api-hash",
    {
        "cef_binary_fake_windows64_minimal.tar.bz2",
        "unused",
        "0000000000000000000000000000000000000000",
        1,
    },
    NULL,
    0,
};

#endif
C_EOF

MUON_BUILDER_VERSION=1.2.3-beta \
  MUON_BUILDER_GIT_COMMIT_HASH=builder-test-hash \
  MUON_RUNTIME_INFO_HEADER="${runtime_info_header}" \
  bash "${SCRIPT_DIR}/build.sh" check Release windows-amd64

launcher_executable="${SCRIPT_DIR}/.run/check-windows-amd64-release/muon-launcher.exe"
launcher_dump="${SCRIPT_DIR}/.run/check-windows-amd64-release/muon-launcher-resources.txt"
x86_64-w64-mingw32-objdump -x "${launcher_executable}" > "${launcher_dump}"
sed -n '/The \.rsrc Resource Directory section:/,/Sections:/p' \
  "${launcher_dump}" > "${launcher_dump}.section"
if grep -Fq 'Entry: ID: 0x000003' "${launcher_dump}.section" ||
    grep -Fq 'Entry: ID: 0x00000e' "${launcher_dump}.section"; then
  echo "muon-launcher.exe contains deprecated windres icon resources." >&2
  exit 1
fi
if ! grep -Fq 'Entry: ID: 0x000010' "${launcher_dump}.section" ||
    ! grep -Fq 'Entry: ID: 0x000018' "${launcher_dump}.section"; then
  echo "muon-launcher.exe is missing Windows version or manifest resources." >&2
  exit 1
fi
node "${SCRIPT_DIR}/scripts/assert-windows-version.mjs" \
  "${launcher_executable}" \
  --file-version \
  1.2.3.0 \
  --product-version \
  1.2.3.0 \
  "ProductName=muon" \
  "CompanyName=Kouji Matsui. (@kekyo@mi.kekyo.net)" \
  "FileDescription=muon Launcher" \
  "FileVersion=1.2.3-beta" \
  "ProductVersion=1.2.3-beta" \
  "InternalName=muon-launcher" \
  "OriginalFilename=muon-launcher.exe" \
  "PrivateBuild=builder-test-hash" \
  "Comments=https://muon-ui.com/ target=windows-amd64; cef=; cefTarget=; cefApi=" \
  "SpecialBuild=cefArtifact=; distribution=; apiHash="

slow_writer_src="${temp_dir}/slow-writer.c"
slow_writer_exe="${temp_dir}/slow-writer.exe"
progress_harness_src="${temp_dir}/process-progress-harness.c"
progress_harness_exe="${temp_dir}/process-progress-harness.exe"
progress_output="${temp_dir}/process-progress-output.bin"
node_harness_src="${temp_dir}/node-runtime-harness.c"
node_harness_exe="${temp_dir}/node-runtime-harness.exe"
remove_harness_src="${temp_dir}/remove-reparse-harness.c"
remove_harness_exe="${temp_dir}/remove-reparse-harness.exe"
node_archive_path="${temp_dir}/node-v24.4.0-win-x64.zip"
node_runtime_root="${temp_dir}/node-runtime-root"
node_dist_dir="${temp_dir}/node-dist"
reparse_target_dir="${temp_dir}/reparse-target"
reparse_link_path="${temp_dir}/reparse-link"
reparse_sentinel_path="${reparse_target_dir}/sentinel.txt"
runtime_source_dir="${temp_dir}/runtime-source"
runtime_stage_dir="${temp_dir}/runtime-stage"
runtime_cache_dir="${temp_dir}/runtime-cache"
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
#include <string.h>
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

cat > "${node_harness_src}" <<'C_EOF'
#include <stdio.h>

#include "prepare_node.h"

int main(int argc, char **argv) {
  if (argc != 3) {
    return 64;
  }
  MuonNodeArtifact artifact = {
      .version = "v24.4.0",
      .catalog_file = "win-x64-zip",
      .archive_target = "win-x64",
      .file_name = "node-v24.4.0-win-x64.zip",
      .url = "",
      .sha256 = "",
      .lts = "Krypton",
  };
  size_t file_count = 0;
  const int result = muon_prepare_install_node_runtime_progress(
      argv[1], "windows-amd64", &artifact, argv[2], &file_count, NULL, NULL);
  if (result != 0) {
    return 1;
  }
  if (file_count != 2) {
    fprintf(stderr, "unexpected Node runtime file count: %llu\n",
            (unsigned long long)file_count);
    return 2;
  }
  return 0;
}
C_EOF

cat > "${remove_harness_src}" <<'C_EOF'
#define WIN32_LEAN_AND_MEAN
#include <stdio.h>
#include <windows.h>

#include "common.h"

typedef DWORD(WINAPI *GetFileAttributesAImport)(LPCSTR path);
typedef BOOL(WINAPI *RemoveDirectoryAImport)(LPCSTR path);

static const char *reparse_path;
static int reparse_removed;

extern GetFileAttributesAImport __real___imp_GetFileAttributesA;
extern RemoveDirectoryAImport __real___imp_RemoveDirectoryA;

static DWORD WINAPI get_file_attributes_with_reparse_point(LPCSTR path) {
  if (reparse_removed && strcmp(path, reparse_path) == 0) {
    SetLastError(ERROR_FILE_NOT_FOUND);
    return INVALID_FILE_ATTRIBUTES;
  }
  const DWORD attributes = __real___imp_GetFileAttributesA(path);
  if (attributes != INVALID_FILE_ATTRIBUTES &&
      strcmp(path, reparse_path) == 0) {
    return attributes | FILE_ATTRIBUTE_DIRECTORY |
           FILE_ATTRIBUTE_REPARSE_POINT;
  }
  return attributes;
}

static BOOL WINAPI remove_reparse_point_directory(LPCSTR path) {
  if (strcmp(path, reparse_path) == 0) {
    reparse_removed = 1;
    return TRUE;
  }
  return __real___imp_RemoveDirectoryA(path);
}

GetFileAttributesAImport __wrap___imp_GetFileAttributesA =
    get_file_attributes_with_reparse_point;
RemoveDirectoryAImport __wrap___imp_RemoveDirectoryA =
    remove_reparse_point_directory;

int main(int argc, char **argv) {
  if (argc != 4) {
    return 64;
  }
  reparse_path = argv[1];
  const DWORD link_attributes = GetFileAttributesA(argv[1]);
  if (link_attributes == INVALID_FILE_ATTRIBUTES ||
      (link_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
      (link_attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
    fprintf(stderr, "created path is not a directory reparse point\n");
    return 1;
  }
  if (muon_remove_recursive(argv[1]) != 0) {
    fprintf(stderr, "recursive removal followed the directory reparse point\n");
    return 2;
  }
  if (GetFileAttributesA(argv[1]) != INVALID_FILE_ATTRIBUTES) {
    fprintf(stderr, "directory reparse point was not removed\n");
    return 3;
  }
  if (GetFileAttributesA(argv[2]) == INVALID_FILE_ATTRIBUTES ||
      GetFileAttributesA(argv[3]) == INVALID_FILE_ATTRIBUTES) {
    fprintf(stderr, "directory reparse point target was modified\n");
    return 4;
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
  "${SCRIPT_DIR}/.deps/build/bzip2-windows64/libbz2.a" \
  "${SCRIPT_DIR}/.deps/build/zlib-windows64/install/lib/libzs.a"

x86_64-w64-mingw32-gcc -std=c99 -O0 -g -Wall -Wextra -pedantic \
  -I"${SCRIPT_DIR}/src" \
  -I"${SCRIPT_DIR}/../deps/sha2" \
  -I"${SCRIPT_DIR}/.deps/src/yyjson-0.12.0/src" \
  -o "${node_harness_exe}" \
  "${node_harness_src}" \
  "${SCRIPT_DIR}/.run/check-windows-amd64-release/libmuon-builder.a" \
  "${SCRIPT_DIR}/.deps/build/libarchive-windows64/libarchive/libarchive.a" \
  "${SCRIPT_DIR}/.deps/build/bzip2-windows64/libbz2.a" \
  "${SCRIPT_DIR}/.deps/build/zlib-windows64/install/lib/libzs.a"

x86_64-w64-mingw32-gcc -std=c99 -O0 -g -Wall -Wextra -pedantic \
  -I"${SCRIPT_DIR}/src" \
  -I"${SCRIPT_DIR}/../deps/sha2" \
  -I"${SCRIPT_DIR}/.deps/src/yyjson-0.12.0/src" \
  -o "${remove_harness_exe}" \
  "${remove_harness_src}" \
  "${SCRIPT_DIR}/.run/check-windows-amd64-release/libmuon-builder.a" \
  "${SCRIPT_DIR}/.deps/build/libarchive-windows64/libarchive/libarchive.a" \
  "${SCRIPT_DIR}/.deps/build/bzip2-windows64/libbz2.a" \
  "${SCRIPT_DIR}/.deps/build/zlib-windows64/install/lib/libzs.a" \
  -Wl,--wrap=__imp_GetFileAttributesA \
  -Wl,--wrap=__imp_RemoveDirectoryA

slow_writer_windows="$(winepath -w "${slow_writer_exe}")"
progress_output_windows="$(winepath -w "${progress_output}")"
WINEDEBUG=-all WINEPREFIX="${wine_prefix}" \
  wine "${progress_harness_exe}" \
    "${slow_writer_windows}" \
    "${progress_output_windows}" > "${temp_dir}/progress.log"
WINEPREFIX="${wine_prefix}" wineserver -w

node - \
  "${reparse_target_dir}" \
  "${reparse_link_path}" \
  "${reparse_sentinel_path}" <<'NODE'
const { mkdirSync, symlinkSync, writeFileSync } = require("node:fs");

const [targetPath, linkPath, sentinelPath] = process.argv.slice(2);
mkdirSync(targetPath, { recursive: true });
writeFileSync(sentinelPath, "must remain\r\n");
symlinkSync(targetPath, linkPath, "dir");
NODE
reparse_link_windows="$(winepath -w "${reparse_link_path}")"
reparse_target_windows="$(winepath -w "${reparse_target_dir}")"
reparse_sentinel_windows="$(winepath -w "${reparse_sentinel_path}")"
WINEDEBUG=-all WINEPREFIX="${wine_prefix}" \
  wine "${remove_harness_exe}" \
    "${reparse_link_windows}" \
    "${reparse_target_windows}" \
    "${reparse_sentinel_windows}"
WINEPREFIX="${wine_prefix}" wineserver -w

node - "${node_archive_path}" <<'NODE'
const AdmZip = require("adm-zip");
const { writeFileSync } = require("node:fs");

const [archivePath] = process.argv.slice(2);
const archive = new AdmZip();
const root = "node-v24.4.0-win-x64";
archive.addFile(
  `${root}/node.exe`,
  Buffer.from("fake windows Node executable\n".repeat(64)),
);
archive.addFile(`${root}/LICENSE`, Buffer.from("fake Node license\n"));
archive.addFile(`${root}/README.md`, Buffer.from("not installed\n"));
writeFileSync(archivePath, archive.toBuffer());
NODE

mkdir -p "${node_runtime_root}"
node_archive_windows="$(winepath -w "${node_archive_path}")"
node_runtime_root_windows="$(winepath -w "${node_runtime_root}")"
WINEDEBUG=-all WINEPREFIX="${wine_prefix}" \
  wine "${node_harness_exe}" \
    "${node_archive_windows}" \
    "${node_runtime_root_windows}"
WINEPREFIX="${wine_prefix}" wineserver -w

node - "${node_runtime_root}" <<'NODE'
const { readFileSync, readdirSync } = require("node:fs");
const { join, relative } = require("node:path");

const [runtimeRoot] = process.argv.slice(2);
const listFiles = (directory) => {
  const files = [];
  for (const entry of readdirSync(directory, { withFileTypes: true })) {
    const path = join(directory, entry.name);
    if (entry.isDirectory()) {
      files.push(...listFiles(path));
    } else if (entry.isFile()) {
      files.push(relative(runtimeRoot, path).replaceAll("\\", "/"));
    }
  }
  return files.sort();
};

const expectedNode = "fake windows Node executable\n".repeat(64);
if (
  readFileSync(join(runtimeRoot, "runtimes/node/bin/node.exe"), "utf8") !==
  expectedNode
) {
  throw new Error("Wine Node extraction did not preserve node.exe content.");
}
if (
  readFileSync(join(runtimeRoot, "runtimes/node/LICENSE"), "utf8") !==
  "fake Node license\n"
) {
  throw new Error("Wine Node extraction did not preserve LICENSE content.");
}
const files = listFiles(runtimeRoot);
const expectedFiles = [
  "runtimes/node/LICENSE",
  "runtimes/node/bin/node.exe",
];
if (JSON.stringify(files) !== JSON.stringify(expectedFiles)) {
  throw new Error(
    `Wine Node extraction produced unexpected files: ${JSON.stringify(files)}`,
  );
}
NODE

mkdir -p "${archive_root}/Release" "${archive_root}/Resources/locales"
printf 'cef library\r\n' > "${archive_root}/Release/libcef.dll"
printf 'cef data\r\n' > "${archive_root}/Resources/icudtl.dat"
printf 'locale\r\n' > "${archive_root}/Resources/locales/en-US.pak"

node - "${node_archive_path}" "${node_dist_dir}" <<'NODE'
const { createHash } = require("node:crypto");
const {
  copyFileSync,
  mkdirSync,
  readFileSync,
  writeFileSync,
} = require("node:fs");
const { basename, join } = require("node:path");

const [archivePath, distDir] = process.argv.slice(2);
const version = "v24.4.0";
const fileName = basename(archivePath);
const versionDir = join(distDir, version);
mkdirSync(versionDir, { recursive: true });
copyFileSync(archivePath, join(versionDir, fileName));
const sha256 = createHash("sha256")
  .update(readFileSync(archivePath))
  .digest("hex");
writeFileSync(
  join(distDir, "index.json"),
  `${JSON.stringify([
    {
      version,
      lts: "Krypton",
      files: ["win-x64-zip"],
    },
  ])}\n`,
);
writeFileSync(
  join(versionDir, "SHASUMS256.txt"),
  `${sha256}  ${fileName}\n`,
);
writeFileSync(join(distDir, "expected-sha256.txt"), `${sha256}\n`);
NODE

mkdir -p "${runtime_source_dir}" "${runtime_cache_dir}"
printf 'fake muon core\r\n' > "${runtime_source_dir}/muon-core.exe"
node_dist_windows="$(winepath -w "${node_dist_dir}")"
runtime_source_windows="$(winepath -w "${runtime_source_dir}")"
runtime_stage_windows="$(winepath -w "${runtime_stage_dir}")"
runtime_cache_windows="$(winepath -w "${runtime_cache_dir}")"
cef_source_windows="$(winepath -w "${archive_root}")"
node_requirement='{"required":true,"engineRangeSpecified":false,"engineRange":"*","comparatorSets":[[]]}'
runtime_wine_log="${temp_dir}/runtime-wine.log"

if ! WINEDEBUG=+seh WINEPREFIX="${wine_prefix}" \
  MUON_NODE_DIST_URL="${node_dist_windows}" \
  wine "${executable}" runtime \
    --muon-path "${runtime_source_windows}" \
    --cef-path "${cef_source_windows}" \
    --stage-dir "${runtime_stage_windows}" \
    --target windows-amd64 \
    --cache-dir "${runtime_cache_windows}" \
    --node-runtime-requirement "${node_requirement}" \
    --json > "${temp_dir}/runtime-result.json" 2> "${runtime_wine_log}"; then
  cat "${runtime_wine_log}" >&2
  exit 1
fi
WINEPREFIX="${wine_prefix}" wineserver -w

node - "${runtime_stage_dir}" "${node_dist_dir}/expected-sha256.txt" <<'NODE'
const { accessSync, readFileSync } = require("node:fs");
const { join } = require("node:path");

const [runtimeDir, expectedShaPath] = process.argv.slice(2);
for (const relativePath of [
  "libcef.dll",
  "runtimes/node/bin/node.exe",
  "runtimes/node/LICENSE",
  ".muon-runtime-ready.json",
]) {
  accessSync(join(runtimeDir, relativePath));
}
const marker = JSON.parse(
  readFileSync(join(runtimeDir, ".muon-runtime-ready.json"), "utf8"),
);
const expectedSha256 = readFileSync(expectedShaPath, "utf8").trim();
if (marker.nodeFingerprint !== expectedSha256) {
  throw new Error("Wine runtime marker did not contain the Node SHA-256.");
}
NODE

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
