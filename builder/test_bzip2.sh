#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_DIR="$(mktemp -d "${TMPDIR:-/tmp}/muon-bzip2-test.XXXXXX")"
trap 'rm -rf "${TEST_DIR}"' EXIT

export MUON_TEST_BZIP2_CC="$(command -v "${CC:-gcc}")"
cp "${SCRIPT_DIR}/build_bzip2.sh" "${TEST_DIR}/build_bzip2.sh"
mkdir -p "${TEST_DIR}/.deps"
if [[ -f "${SCRIPT_DIR}/.deps/bzip2-1.0.8.tar.gz" ]]; then
  cp "${SCRIPT_DIR}/.deps/bzip2-1.0.8.tar.gz" "${TEST_DIR}/.deps/"
fi

# A small inlining budget reproduces the optimizer decisions of the baseline
# compiler. Dependency builds must remain warning-free with that budget.
cat > "${TEST_DIR}/cc" <<'EOF'
#!/usr/bin/env bash
exec "${MUON_TEST_BZIP2_CC}" -Werror --param=max-inline-insns-single=1 "$@"
EOF
chmod +x "${TEST_DIR}/cc"
unset CFLAGS
bash "${TEST_DIR}/build_bzip2.sh" test "${TEST_DIR}/cc" "${AR:-ar}" "${RANLIB:-ranlib}"

# Exercise the resulting static library through the unmodified upstream CLI.
SOURCE_DIR="${TEST_DIR}/.deps/src/bzip2-1.0.8"
"${MUON_TEST_BZIP2_CC}" -D_FILE_OFFSET_BITS=64 \
  "${SOURCE_DIR}/bzip2.c" "${TEST_DIR}/.deps/build/bzip2-test/libbz2.a" \
  -o "${TEST_DIR}/bzip2"
"${TEST_DIR}/bzip2" -c < "${SOURCE_DIR}/sample1.ref" > "${TEST_DIR}/sample.bz2"
"${TEST_DIR}/bzip2" -dc < "${TEST_DIR}/sample.bz2" > "${TEST_DIR}/sample.ref"
cmp "${SOURCE_DIR}/sample1.ref" "${TEST_DIR}/sample.ref"
echo "bzip2 warning-free build and compression round trip passed"
