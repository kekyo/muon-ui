#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="${SCRIPT_DIR}/.run/test-bootstrap-progress"
HARNESS="${OUT_DIR}/bootstrap_progress_harness.c"
WIN_FALLBACK_HARNESS="${OUT_DIR}/bootstrap_progress_windows_fallback_harness.c"
WIN_UI_HARNESS="${OUT_DIR}/bootstrap_progress_windows_ui_harness.c"
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

cat >"${WIN_FALLBACK_HARNESS}" <<'HARNESS_EOF'
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

cat >"${WIN_UI_HARNESS}" <<'HARNESS_EOF'
#define WIN32_LEAN_AND_MEAN
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <commctrl.h>

#define MUON_BOOTSTRAP_PROGRESS_TEST
#include "../../src/bootstrap_progress.c"

#define MUON_EXPECTED_PROGRESS_HEIGHT 6

typedef struct {
  HWND label;
  HWND determinate_bar;
  HWND marquee_bar;
  int bar_count;
} ProgressControls;

static BOOL CALLBACK collect_child_window(HWND child, LPARAM parameter) {
  ProgressControls *controls = (ProgressControls *)parameter;
  char class_name[64];
  if (GetClassNameA(child, class_name, sizeof(class_name)) == 0) {
    return TRUE;
  }
  if (lstrcmpiA(class_name, "STATIC") == 0) {
    controls->label = child;
    return TRUE;
  }
  if (lstrcmpiA(class_name, PROGRESS_CLASSA) == 0) {
    const LONG_PTR style = GetWindowLongPtrA(child, GWL_STYLE);
    controls->bar_count += 1;
    if ((style & PBS_MARQUEE) != 0) {
      controls->marquee_bar = child;
    } else {
      controls->determinate_bar = child;
    }
  }
  return TRUE;
}

static void collect_controls(HWND window, ProgressControls *controls) {
  memset(controls, 0, sizeof(*controls));
  EnumChildWindows(window, collect_child_window, (LPARAM)controls);
}

static int wait_for_progress_window(HWND *window) {
  for (int attempt = 0; attempt < 100; attempt += 1) {
    HWND candidate = FindWindowA("MuonBootstrapProgressWindow", "Muon");
    if (candidate != NULL && IsWindowVisible(candidate)) {
      *window = candidate;
      return 0;
    }
    Sleep(20);
  }
  fprintf(stderr, "bootstrap progress window was not shown\n");
  return 1;
}

static int wait_for_mode(HWND window, int expect_marquee,
                         ProgressControls *controls) {
  for (int attempt = 0; attempt < 100; attempt += 1) {
    collect_controls(window, controls);
    if (controls->label != NULL && controls->determinate_bar != NULL &&
        controls->marquee_bar != NULL && controls->bar_count == 2) {
      const int determinate_visible =
          IsWindowVisible(controls->determinate_bar) != 0;
      const int marquee_visible = IsWindowVisible(controls->marquee_bar) != 0;
      if ((!expect_marquee && determinate_visible && !marquee_visible) ||
          (expect_marquee && !determinate_visible && marquee_visible)) {
        return 0;
      }
    }
    Sleep(20);
  }
  fprintf(stderr, "progress controls did not reach the expected mode\n");
  fprintf(stderr, "label=%p determinate=%p marquee=%p count=%d\n",
          (void *)controls->label, (void *)controls->determinate_bar,
          (void *)controls->marquee_bar, controls->bar_count);
  return 1;
}

static int assert_progress_geometry(HWND determinate_bar, HWND marquee_bar) {
  RECT determinate;
  RECT marquee;
  if (!GetWindowRect(determinate_bar, &determinate) ||
      !GetWindowRect(marquee_bar, &marquee)) {
    fprintf(stderr, "failed to read progress bar geometry\n");
    return 1;
  }
  const LONG height = determinate.bottom - determinate.top;
  if (height != MUON_EXPECTED_PROGRESS_HEIGHT) {
    fprintf(stderr, "progress height: expected %d, got %ld\n",
            MUON_EXPECTED_PROGRESS_HEIGHT, height);
    return 1;
  }
  if (memcmp(&determinate, &marquee, sizeof(determinate)) != 0) {
    fprintf(stderr, "progress bars do not share the same geometry\n");
    return 1;
  }
  return 0;
}

static int assert_background(HWND window, HWND label) {
  HBRUSH class_brush = (HBRUSH)GetClassLongPtrA(window, GCLP_HBRBACKGROUND);
  if (class_brush != (HBRUSH)(COLOR_BTNFACE + 1) &&
      class_brush != GetSysColorBrush(COLOR_BTNFACE)) {
    fprintf(stderr, "window background does not use COLOR_BTNFACE\n");
    return 1;
  }
  HDC dc = GetDC(label);
  if (dc == NULL) {
    fprintf(stderr, "failed to read label device context\n");
    return 1;
  }
  const LRESULT brush =
      SendMessageA(window, WM_CTLCOLORSTATIC, (WPARAM)dc, (LPARAM)label);
  const COLORREF background = GetBkColor(dc);
  ReleaseDC(label, dc);
  if ((HBRUSH)brush != GetSysColorBrush(COLOR_BTNFACE)) {
    fprintf(stderr, "label background brush does not use COLOR_BTNFACE\n");
    return 1;
  }
  if (background != GetSysColor(COLOR_BTNFACE)) {
    fprintf(stderr, "label background color does not use COLOR_BTNFACE\n");
    return 1;
  }
  return 0;
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
  event.current = 25;
  event.total = 100;
  event.determinate = 1;
  muon_bootstrap_progress_update(&progress, &event);

  HWND window = NULL;
  ProgressControls controls;
  int failed = wait_for_progress_window(&window);
  if (!failed) {
    failed |= wait_for_mode(window, 0, &controls);
  }
  if (!failed) {
    failed |= assert_progress_geometry(controls.determinate_bar,
                                       controls.marquee_bar);
    failed |= assert_background(window, controls.label);
    const LRESULT position =
        SendMessageA(controls.determinate_bar, PBM_GETPOS, 0, 0);
    if (position <= 0) {
      fprintf(stderr, "determinate progress did not advance\n");
      failed = 1;
    }
  }

  event.phase = MUON_PREPARE_PROGRESS_PHASE_INSTALLING;
  event.status = "Installing CEF runtime...";
  event.current = 0;
  event.total = 0;
  event.determinate = 0;
  muon_bootstrap_progress_update(&progress, &event);
  if (!failed) {
    failed |= wait_for_mode(window, 1, &controls);
  }

  muon_bootstrap_progress_dispose(&progress);
  return failed;
}
HARNESS_EOF

wine_prefix="${OUT_DIR}/wineprefix"
rm -rf "${wine_prefix}"

x86_64-w64-mingw32-gcc -std=c99 -Wall -Wextra -pedantic \
  -I"${SCRIPT_DIR}/src" \
  -o "${OUT_DIR}/bootstrap_progress_windows64_fallback_harness.exe" \
  "${WIN_FALLBACK_HARNESS}" \
  -lcomctl32 -lgdi32
xvfb-run -a env WINEDEBUG=-all WINEPREFIX="${wine_prefix}" \
  wine "${OUT_DIR}/bootstrap_progress_windows64_fallback_harness.exe"
WINEPREFIX="${wine_prefix}" wineserver -w

x86_64-w64-mingw32-gcc -std=c99 -Wall -Wextra -pedantic \
  -I"${SCRIPT_DIR}/src" \
  -o "${OUT_DIR}/bootstrap_progress_windows64_ui_harness.exe" \
  "${WIN_UI_HARNESS}" \
  -lcomctl32 -lgdi32
xvfb-run -a env WINEDEBUG=-all WINEPREFIX="${wine_prefix}" \
  wine "${OUT_DIR}/bootstrap_progress_windows64_ui_harness.exe"
WINEPREFIX="${wine_prefix}" wineserver -w

i686-w64-mingw32-gcc -std=c99 -Wall -Wextra -pedantic \
  -I"${SCRIPT_DIR}/src" \
  -o "${OUT_DIR}/bootstrap_progress_windows32_fallback_harness.exe" \
  "${WIN_FALLBACK_HARNESS}" \
  -lcomctl32 -lgdi32
xvfb-run -a env WINEDEBUG=-all WINEPREFIX="${wine_prefix}" \
  wine "${OUT_DIR}/bootstrap_progress_windows32_fallback_harness.exe"
WINEPREFIX="${wine_prefix}" wineserver -w

i686-w64-mingw32-gcc -std=c99 -Wall -Wextra -pedantic \
  -I"${SCRIPT_DIR}/src" \
  -o "${OUT_DIR}/bootstrap_progress_windows32_ui_harness.exe" \
  "${WIN_UI_HARNESS}" \
  -lcomctl32 -lgdi32
xvfb-run -a env WINEDEBUG=-all WINEPREFIX="${wine_prefix}" \
  wine "${OUT_DIR}/bootstrap_progress_windows32_ui_harness.exe"
WINEPREFIX="${wine_prefix}" wineserver -w
