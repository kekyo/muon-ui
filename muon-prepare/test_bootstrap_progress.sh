#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="${SCRIPT_DIR}/.run/test-bootstrap-progress"
HARNESS="${OUT_DIR}/bootstrap_progress_harness.c"
WIN_HARNESS="${OUT_DIR}/bootstrap_progress_windows_harness.c"
mkdir -p "${OUT_DIR}"

cat >"${HARNESS}" <<'HARNESS_EOF'
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>

#define MUON_BOOTSTRAP_PROGRESS_TEST
#include "../../src/bootstrap_progress.c"

static int assert_equal_uint16(const char *name, unsigned short actual,
                               unsigned short expected) {
  if (actual != expected) {
    fprintf(stderr, "%s: expected %u, got %u\n", name, expected, actual);
    return 1;
  }
  return 0;
}

static int assert_in_range(const char *name, unsigned short actual,
                           unsigned short minimum, unsigned short maximum) {
  if (actual < minimum || actual > maximum) {
    fprintf(stderr, "%s: expected %u..%u, got %u\n", name, minimum, maximum,
            actual);
    return 1;
  }
  return 0;
}

int main(void) {
  const unsigned short range = 210;
  const unsigned short first =
      muon_bootstrap_progress_test_pulse_position(1000000000ULL, range);
  const unsigned short burst =
      muon_bootstrap_progress_test_pulse_position(1000000000ULL, range);
  const unsigned short later =
      muon_bootstrap_progress_test_pulse_position(1200000000ULL, range);
  int failed = 0;
  failed |= assert_equal_uint16("same timestamp", burst, first);
  failed |= assert_in_range("first", first, 0, range);
  failed |= assert_in_range("later", later, 0, range);
  if (later == first) {
    fprintf(stderr, "later timestamp: expected a different pulse position\n");
    failed = 1;
  }
  return failed;
}
HARNESS_EOF

gcc -std=c99 -Wall -Wextra -pedantic \
  -I"${SCRIPT_DIR}/src" \
  -o "${OUT_DIR}/bootstrap_progress_harness" \
  "${HARNESS}" \
  $(pkg-config --cflags --libs xcb) \
  -pthread
"${OUT_DIR}/bootstrap_progress_harness"

cat >"${WIN_HARNESS}" <<'HARNESS_EOF'
#define WIN32_LEAN_AND_MEAN
#include <stdio.h>
#include <windows.h>
#include <commctrl.h>

static BOOL WINAPI muon_test_init_common_controls_ex(
    const INITCOMMONCONTROLSEX *controls) {
  (void)controls;
  return FALSE;
}

#define InitCommonControlsEx muon_test_init_common_controls_ex
#define MUON_BOOTSTRAP_PROGRESS_TEST
#include "../../src/bootstrap_progress.c"
#undef InitCommonControlsEx

static int wait_for_progress_window(void) {
  for (int attempt = 0; attempt < 100; attempt += 1) {
    HWND window = FindWindowA("MuonBootstrapProgressWindow", "Muon");
    if (window != NULL && IsWindowVisible(window)) {
      return 0;
    }
    Sleep(20);
  }
  fprintf(stderr, "bootstrap progress window was not shown\n");
  return 1;
}

int main(void) {
  MuonBootstrapProgress progress;
  muon_bootstrap_progress_init(&progress);
  if (!muon_bootstrap_progress_is_available(&progress)) {
    fprintf(stderr, "bootstrap progress backend is unavailable\n");
    return 1;
  }
  MuonPrepareProgress event;
  event.phase = MUON_PREPARE_PROGRESS_PHASE_DOWNLOADING;
  event.status = "Downloading CEF runtime...";
  event.current = 1;
  event.total = 100;
  event.determinate = 1;
  muon_bootstrap_progress_update(&progress, &event);
  const int result = wait_for_progress_window();
  muon_bootstrap_progress_dispose(&progress);
  return result;
}
HARNESS_EOF

wine_prefix="${OUT_DIR}/wineprefix"
rm -rf "${wine_prefix}"

x86_64-w64-mingw32-gcc -std=c99 -Wall -Wextra -pedantic \
  -I"${SCRIPT_DIR}/src" \
  -o "${OUT_DIR}/bootstrap_progress_windows64_harness.exe" \
  "${WIN_HARNESS}" \
  -lcomctl32
xvfb-run -a env WINEDEBUG=-all WINEPREFIX="${wine_prefix}" \
  wine "${OUT_DIR}/bootstrap_progress_windows64_harness.exe"
WINEPREFIX="${wine_prefix}" wineserver -w

i686-w64-mingw32-gcc -std=c99 -Wall -Wextra -pedantic \
  -I"${SCRIPT_DIR}/src" \
  -o "${OUT_DIR}/bootstrap_progress_windows32_harness.exe" \
  "${WIN_HARNESS}" \
  -lcomctl32
xvfb-run -a env WINEDEBUG=-all WINEPREFIX="${wine_prefix}" \
  wine "${OUT_DIR}/bootstrap_progress_windows32_harness.exe"
WINEPREFIX="${wine_prefix}" wineserver -w
