#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="${SCRIPT_DIR}/.run/test-bootstrap-progress"
HARNESS="${OUT_DIR}/bootstrap_progress_harness.c"
BOOTSTRAP_STATE_HARNESS="${OUT_DIR}/bootstrap_state_directory_harness.c"
WIN_FALLBACK_HARNESS="${OUT_DIR}/bootstrap_progress_windows_fallback_harness.c"
WIN_UI_HARNESS="${OUT_DIR}/bootstrap_progress_windows_ui_harness.c"
mkdir -p "${OUT_DIR}"

bash "${SCRIPT_DIR}/build_yyjson.sh"

cat >"${HARNESS}" <<'HARNESS_EOF'
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <string.h>

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

static int assert_string(const char *name, const char *actual,
                         const char *expected) {
  if (strcmp(actual, expected) != 0) {
    fprintf(stderr, "%s: expected %s, got %s\n", name, expected, actual);
    return 1;
  }
  return 0;
}

static int assert_progress_text(const char *name,
                                MuonPrepareProgressPhase phase,
                                const char *status,
                                unsigned long long current,
                                unsigned long long total,
                                int determinate,
                                const char *expected) {
  MuonPrepareProgress event;
  char text[256];
  event.phase = phase;
  event.status = status;
  event.current = current;
  event.total = total;
  event.determinate = determinate;
  muon_bootstrap_progress_test_format_event(&event, text, sizeof(text));
  return assert_string(name, text, expected);
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
  failed |= assert_progress_text(
      "download percentage", MUON_PREPARE_PROGRESS_PHASE_DOWNLOADING,
      "Downloading CEF runtime...", 42, 100, 1,
      "Downloading CEF runtime... 42%");
  failed |= assert_progress_text(
      "download percentage clamp", MUON_PREPARE_PROGRESS_PHASE_DOWNLOADING,
      "Downloading CEF runtime...", 250, 100, 1,
      "Downloading CEF runtime... 100%");
  failed |= assert_progress_text(
      "extract file count", MUON_PREPARE_PROGRESS_PHASE_INSTALLING,
      "Extracting CEF runtime...", 123, 0, 0,
      "Extracting CEF runtime... 123 files");
  failed |= assert_progress_text(
      "install file count", MUON_PREPARE_PROGRESS_PHASE_INSTALLING,
      "Installing CEF runtime...", 228, 0, 0,
      "Installing CEF runtime... 228 files");
  failed |= assert_progress_text(
      "plain status", MUON_PREPARE_PROGRESS_PHASE_FINALIZING,
      "Starting muon...", 0, 0, 0, "Starting muon...");
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

cat >"${BOOTSTRAP_STATE_HARNESS}" <<'HARNESS_EOF'
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define main muon_bootstrap_original_main
#include "../../src/muon_bootstrap.c"
#undef main

char *muon_substring(const char *start, size_t length) {
  char *result = (char *)malloc(length + 1);
  if (result == NULL) {
    return NULL;
  }
  memcpy(result, start, length);
  result[length] = '\0';
  return result;
}

char *muon_duplicate_path_string(const char *value) {
  char *result = muon_substring(value, strlen(value));
  normalize_path_separators(result);
  return result;
}

char *muon_path_join(const char *left, const char *right) {
  if (left == NULL || left[0] == '\0') {
    return muon_substring(right, strlen(right));
  }
  if (right == NULL || right[0] == '\0') {
    return muon_substring(left, strlen(left));
  }
  const int needs_separator = !is_path_separator(left[strlen(left) - 1]);
  const size_t size = strlen(left) + strlen(right) + (needs_separator ? 2 : 1);
  char *result = (char *)malloc(size);
  if (result == NULL) {
    return NULL;
  }
  snprintf(result, size, needs_separator ? "%s/%s" : "%s%s", left, right);
  return result;
}

char *muon_path_join3(const char *first, const char *second,
                      const char *third) {
  char *left = muon_path_join(first, second);
  if (left == NULL) {
    return NULL;
  }
  char *result = muon_path_join(left, third);
  free(left);
  return result;
}

void muon_free_string_array(char **values, size_t count) {
  if (values == NULL) {
    return;
  }
  for (size_t index = 0; index < count; index += 1) {
    free(values[index]);
  }
  free(values);
}

int muon_path_exists(const char *path) {
#ifdef _WIN32
  (void)path;
  return 0;
#else
  return access(path, F_OK) == 0;
#endif
}

char *muon_read_text_file(const char *path) {
  (void)path;
  return NULL;
}

int muon_write_text_file(const char *path, const char *content) {
  (void)path;
  (void)content;
  return -1;
}

yyjson_doc *muon_json_read_file(const char *path) {
#ifdef _WIN32
  (void)path;
  return NULL;
#else
  return yyjson_read_file(path, 0, NULL, NULL);
#endif
}

int muon_bootstrap_get_embedded_app_id(char **app_id) {
  *app_id = NULL;
  return 0;
}

void muon_bootstrap_progress_init(MuonBootstrapProgress *progress) {
  progress->backend = NULL;
}

int muon_bootstrap_progress_is_available(const MuonBootstrapProgress *progress) {
  (void)progress;
  return 0;
}

void muon_bootstrap_progress_update(MuonBootstrapProgress *progress,
                                    const MuonPrepareProgress *event) {
  (void)progress;
  (void)event;
}

void muon_bootstrap_progress_fail(MuonBootstrapProgress *progress) {
  (void)progress;
}

void muon_bootstrap_progress_dispose(MuonBootstrapProgress *progress) {
  (void)progress;
}

int muon_prepare_staged_with_progress(
    const char *muon_path, const char *stage_dir, const char *target,
    const char *cache_dir, int force, int quiet,
    MuonPrepareProgressCallback progress_callback, void *progress_user_data) {
  (void)muon_path;
  (void)stage_dir;
  (void)target;
  (void)cache_dir;
  (void)force;
  (void)quiet;
  (void)progress_callback;
  (void)progress_user_data;
  return 1;
}

static int set_environment(const char *name, const char *value) {
#ifdef _WIN32
  return _putenv_s(name, value);
#else
  return setenv(name, value, 1);
#endif
}

#ifndef _WIN32
static int clear_environment(const char *name) {
  return unsetenv(name);
}
#endif

static int assert_stage_dir(const char *name, const char *app_id,
                            const char *target, const char *expected) {
  char *actual = create_state_runtime_dir(app_id, target);
  if (actual == NULL) {
    fprintf(stderr, "%s: failed to create stage directory\n", name);
    return 1;
  }
  const int failed = strcmp(actual, expected) != 0;
  if (failed) {
    fprintf(stderr, "%s: expected %s, got %s\n", name, expected, actual);
  }
  free(actual);
  return failed;
}

int main(void) {
  int failed = 0;
#ifdef _WIN32
  failed |= set_environment("LOCALAPPDATA", "C:\\Users\\alice\\AppData\\Local");
  failed |= assert_stage_dir(
      "LOCALAPPDATA", "com.example.app", "windows-amd64",
      "C:/Users/alice/AppData/Local/com.example.app/runtime");
#else
  failed |= set_environment("XDG_STATE_HOME", "/tmp/muon-state");
  failed |= assert_stage_dir("XDG_STATE_HOME", "com.example.app", "linux-amd64",
                             "/tmp/muon-state/com.example.app/runtime");
  failed |= clear_environment("XDG_STATE_HOME");
	  failed |= set_environment("HOME", "/home/alice");
	  failed |= assert_stage_dir("HOME", "com.example.app", "linux-arm64",
	                             "/home/alice/.local/state/com.example.app/runtime");
#endif
  if (!should_prepare_staged_runtime("/tmp/source", "/tmp/state")) {
    fprintf(stderr, "different runtime directories should be staged\n");
    failed = 1;
  }
  if (should_prepare_staged_runtime("/tmp/state", "/tmp/state")) {
    fprintf(stderr, "state runtime directory should not be staged again\n");
    failed = 1;
  }
#ifndef _WIN32
  const char *install_test_dir = getenv("MUON_BOOTSTRAP_INSTALL_TEST_DIR");
  if (install_test_dir == NULL || install_test_dir[0] == '\0') {
    fprintf(stderr, "MUON_BOOTSTRAP_INSTALL_TEST_DIR was not set\n");
    failed = 1;
  } else {
    MuonInstallConfig config;
    if (read_install_config(install_test_dir, &config) != 0) {
      fprintf(stderr, "system-setuid install config was rejected\n");
      failed = 1;
    } else {
      if (!config.is_deb || !config.is_system_setuid) {
        fprintf(stderr, "system-setuid install config mode was not parsed\n");
        failed = 1;
      }
      if (strcmp(config.launcher_path, "/usr/bin/sample-app") != 0 ||
          strcmp(config.system_runtime_path,
                 "/var/lib/muon/apps/sample-app/linux-amd64/runtime") != 0 ||
          strcmp(config.privileged_prepare_path,
                 "/usr/lib/sample-app/dist-muon/linux-amd64/muon-runtime-helper") !=
              0) {
        fprintf(stderr, "system-setuid install config paths were not parsed\n");
        failed = 1;
      }
      install_config_free(&config);
    }
  }
#endif
	  return failed;
	}
HARNESS_EOF

install_config_dir="${OUT_DIR}/install-config"
mkdir -p "${install_config_dir}"
cat >"${install_config_dir}/muon-install.json" <<'INSTALL_CONFIG_EOF'
{
  "type": "deb",
  "packageName": "sample-app",
  "launcherPath": "/usr/bin/sample-app",
  "runtimeMode": "system-setuid",
  "systemRuntimePath": "/var/lib/muon/apps/sample-app/linux-amd64/runtime",
  "privilegedPreparePath": "/usr/lib/sample-app/dist-muon/linux-amd64/muon-runtime-helper"
}
INSTALL_CONFIG_EOF

gcc -std=c99 -Wall -Wextra -pedantic \
	  -I"${SCRIPT_DIR}/src" \
	  -I"${SCRIPT_DIR}/.deps/src/yyjson-0.12.0/src" \
	  -o "${OUT_DIR}/bootstrap_state_directory_harness" \
	  "${BOOTSTRAP_STATE_HARNESS}" \
	  "${SCRIPT_DIR}/.deps/src/yyjson-0.12.0/src/yyjson.c"
MUON_BOOTSTRAP_INSTALL_TEST_DIR="${install_config_dir}" \
  "${OUT_DIR}/bootstrap_state_directory_harness"

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
    HWND window = FindWindowA("MuonBootstrapProgressWindow", "muon");
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
    HWND candidate = FindWindowA("MuonBootstrapProgressWindow", "muon");
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

static int assert_close_button_hidden(HWND window) {
  const LONG_PTR style = GetWindowLongPtrA(window, GWL_STYLE);
  if ((style & WS_SYSMENU) != 0) {
    fprintf(stderr, "progress window still exposes the close system menu\n");
    return 1;
  }
  return 0;
}

static int wait_for_label_text(HWND label, const char *expected) {
  char actual[256];
  for (int attempt = 0; attempt < 100; attempt += 1) {
    actual[0] = '\0';
    GetWindowTextA(label, actual, sizeof(actual));
    if (strcmp(actual, expected) == 0) {
      return 0;
    }
    Sleep(20);
  }
  fprintf(stderr, "progress label: expected %s, got %s\n", expected, actual);
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
    failed |= assert_close_button_hidden(window);
    failed |= assert_progress_geometry(controls.determinate_bar,
                                       controls.marquee_bar);
    failed |= assert_background(window, controls.label);
    failed |= wait_for_label_text(controls.label,
                                  "Downloading CEF runtime... 25%");
    const LRESULT position =
        SendMessageA(controls.determinate_bar, PBM_GETPOS, 0, 0);
    if (position <= 0) {
      fprintf(stderr, "determinate progress did not advance\n");
      failed = 1;
    }
  }

  event.phase = MUON_PREPARE_PROGRESS_PHASE_INSTALLING;
  event.status = "Installing CEF runtime...";
  event.current = 42;
  event.total = 0;
  event.determinate = 0;
  muon_bootstrap_progress_update(&progress, &event);
  if (!failed) {
    failed |= wait_for_mode(window, 1, &controls);
    failed |= wait_for_label_text(controls.label,
                                  "Installing CEF runtime... 42 files");
  }

  muon_bootstrap_progress_dispose(&progress);
  return failed;
}
HARNESS_EOF

wine_prefix="${OUT_DIR}/wineprefix"
rm -rf "${wine_prefix}"

x86_64-w64-mingw32-gcc -std=c99 -Wall -Wextra -pedantic \
  -I"${SCRIPT_DIR}/src" \
  -I"${SCRIPT_DIR}/.deps/src/yyjson-0.12.0/src" \
  -o "${OUT_DIR}/bootstrap_state_directory_windows64_harness.exe" \
  "${BOOTSTRAP_STATE_HARNESS}"
xvfb-run -a env WINEDEBUG=-all WINEPREFIX="${wine_prefix}" \
  wine "${OUT_DIR}/bootstrap_state_directory_windows64_harness.exe"
WINEPREFIX="${wine_prefix}" wineserver -w

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
