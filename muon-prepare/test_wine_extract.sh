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
  node
  tar
)

for command_name in "${required_commands[@]}"; do
  if ! command -v "${command_name}" >/dev/null; then
    echo "Skipping Wine extraction test: ${command_name} is not available."
    exit 0
  fi
done

bash "${SCRIPT_DIR}/build.sh" check Release windows64

temp_dir="$(mktemp -d)"
trap 'rm -rf "${temp_dir}"' EXIT

source_dir="${temp_dir}/source"
cache_dir="${temp_dir}/cache"
output_dir="${temp_dir}/output"
wine_prefix="${temp_dir}/wineprefix"
archive_root="${source_dir}/cef_binary_fake_windows64_minimal"
archive_path="${source_dir}/cef.tar.bz2"
catalog_path="${source_dir}/source-catalog.json"
executable="${SCRIPT_DIR}/.run/check-windows64-release/muon-prepare.exe"

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
    --target windows64 \
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
if (result.version !== "fake-cef" || result.target !== "windows64") {
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
