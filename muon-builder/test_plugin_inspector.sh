#!/usr/bin/env bash
set -euo pipefail

if [[ "$#" -ne 1 ]]; then
  echo "usage: test_plugin_inspector.sh inspector-path" >&2
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INSPECTOR_PATH="$1"
OUT_DIR="${SCRIPT_DIR}/.run/test-plugin-inspector"
PLUGIN_DIR="${OUT_DIR}/plugins"
CC_VALUE="${CC:-gcc}"

rm -rf "${OUT_DIR}"
mkdir -p "${PLUGIN_DIR}"

"${CC_VALUE}" -std=c99 -fPIC -shared -Wall -Wextra -pedantic \
  -I"${SCRIPT_DIR}/../muon-core/include" \
  -o "${PLUGIN_DIR}/valid-plugin.so" \
  "${SCRIPT_DIR}/test/plugin_inspector_valid.c"

"${CC_VALUE}" -std=c99 -fPIC -shared -Wall -Wextra -pedantic \
  -I"${SCRIPT_DIR}/../muon-core/include" \
  -o "${PLUGIN_DIR}/invalid-plugin.so" \
  "${SCRIPT_DIR}/test/plugin_inspector_invalid.c"

cat >"${OUT_DIR}/valid-input.json" <<JSON
{
  "path": "${PLUGIN_DIR}",
  "plugins": [
    {
      "name": "valid-plugin",
      "config": {
        "mode": "metadata"
      }
    }
  ]
}
JSON

"${INSPECTOR_PATH}" "${OUT_DIR}/valid-input.json" >"${OUT_DIR}/valid-output.json"

node - "${OUT_DIR}/valid-output.json" <<'NODE'
const { readFileSync } = require("node:fs");
const outputPath = process.argv[2];
const output = JSON.parse(readFileSync(outputPath, "utf8"));
const plugin = output.plugins?.find((entry) => entry.name === "valid-plugin");
if (plugin === undefined) {
  throw new Error("valid plugin output was not found.");
}
const functions = plugin.functions ?? [];
for (const expected of ["test.namespace.alpha", "test.namespace.beta"]) {
  if (!functions.includes(expected)) {
    throw new Error(`Expected function was missing: ${expected}`);
  }
}
if (functions.includes("test.namespace.nativeBeta")) {
  throw new Error("filter_name was not used for public function output.");
}
NODE

expect_failure() {
  local name="$1"
  local input_path="$2"
  if "${INSPECTOR_PATH}" "${input_path}" \
      >"${OUT_DIR}/${name}-stdout.txt" \
      2>"${OUT_DIR}/${name}-stderr.txt"; then
    echo "expected inspector failure: ${name}" >&2
    exit 1
  fi
}

cat >"${OUT_DIR}/missing-input.json" <<JSON
{
  "path": "${PLUGIN_DIR}",
  "plugins": [
    {
      "name": "missing-plugin"
    }
  ]
}
JSON
expect_failure "missing-plugin" "${OUT_DIR}/missing-input.json"
grep -q "plugin file not found" "${OUT_DIR}/missing-plugin-stderr.txt"

cat >"${OUT_DIR}/signature-mismatch-input.json" <<JSON
{
  "path": "${PLUGIN_DIR}",
  "plugins": [
    {
      "name": "valid-plugin",
      "signature": "0000000000000000000000000000000000000000000000000000000000000000",
      "salt": "deadbeef",
      "config": {
        "mode": "metadata"
      }
    }
  ]
}
JSON
expect_failure "signature-mismatch" "${OUT_DIR}/signature-mismatch-input.json"
grep -q "plugin signature mismatch" \
  "${OUT_DIR}/signature-mismatch-stderr.txt"

cat >"${OUT_DIR}/invalid-metadata-input.json" <<JSON
{
  "path": "${PLUGIN_DIR}",
  "plugins": [
    {
      "name": "invalid-plugin"
    }
  ]
}
JSON
expect_failure "invalid-metadata" "${OUT_DIR}/invalid-metadata-input.json"
grep -q "invalid plugin namespace" \
  "${OUT_DIR}/invalid-metadata-stderr.txt"
