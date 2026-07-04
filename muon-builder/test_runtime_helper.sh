#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="${SCRIPT_DIR}/.run/test-runtime-helper"
SOURCE_ROOT="${OUT_DIR}/usr-lib"
SYSTEM_ROOT="${OUT_DIR}/var-lib-muon-apps"
HARNESS="${OUT_DIR}/runtime_helper_harness.c"
mkdir -p "${SOURCE_ROOT}/sample-app/dist-muon/linux-amd64"
mkdir -p "${SOURCE_ROOT}/bad-target/dist-muon/windows-amd64"
mkdir -p "${SOURCE_ROOT}/BadName/dist-muon/linux-amd64"
mkdir -p "${SYSTEM_ROOT}"
touch "${SOURCE_ROOT}/sample-app/dist-muon/linux-amd64/muon-runtime-helper"
touch "${SOURCE_ROOT}/bad-target/dist-muon/windows-amd64/muon-runtime-helper"
touch "${SOURCE_ROOT}/BadName/dist-muon/linux-amd64/muon-runtime-helper"

bash "${SCRIPT_DIR}/build_yyjson.sh"

cat >"${HARNESS}" <<'HARNESS_EOF'
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define main muon_runtime_helper_original_main
#include "../../src/muon_runtime_helper.c"
#undef main

void muon_print_error(const char *format, ...) {
  (void)format;
}

void muon_print_errno(const char *message) {
  (void)message;
}

void muon_log_message(const char *format, ...) {
  (void)format;
}

void muon_report_progress(MuonPrepareProgressCallback callback,
                          void *user_data,
                          MuonPrepareProgressPhase phase,
                          const char *status,
                          unsigned long long current,
                          unsigned long long total,
                          int determinate) {
  (void)callback;
  (void)user_data;
  (void)phase;
  (void)status;
  (void)current;
  (void)total;
  (void)determinate;
}

#if 0
void muon_print_error_unused(const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  vfprintf(stderr, format, arguments);
  va_end(arguments);
}

void muon_print_errno_unused(const char *message) {
  perror(message);
}
#endif

char *muon_duplicate_string(const char *value) {
  const size_t size = strlen(value) + 1;
  char *result = (char *)malloc(size);
  if (result == NULL) {
    return NULL;
  }
  memcpy(result, value, size);
  return result;
}

char *muon_duplicate_path_string(const char *value) {
  char *result = muon_duplicate_string(value);
  if (result == NULL) {
    return NULL;
  }
  for (char *cursor = result; *cursor != '\0'; cursor += 1) {
    if (*cursor == '\\') {
      *cursor = '/';
    }
  }
  return result;
}

char *muon_substring(const char *start, size_t length) {
  char *result = (char *)malloc(length + 1);
  if (result == NULL) {
    return NULL;
  }
  memcpy(result, start, length);
  result[length] = '\0';
  return result;
}

char *muon_path_join(const char *left, const char *right) {
  const int needs_separator = left[strlen(left) - 1] != '/';
  const size_t size = strlen(left) + strlen(right) + (needs_separator ? 2 : 1);
  char *result = (char *)malloc(size);
  if (result != NULL) {
    snprintf(result, size, needs_separator ? "%s/%s" : "%s%s", left, right);
  }
  return result;
}

char *muon_parent_directory(const char *path) {
  const char *slash = strrchr(path, '/');
  if (slash == NULL) {
    return muon_duplicate_string(".");
  }
  if (slash == path) {
    return muon_duplicate_string("/");
  }
  return muon_substring(path, (size_t)(slash - path));
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

static int expect_string(const char *name, const char *actual,
                         const char *expected) {
  if (actual != NULL && strcmp(actual, expected) == 0) {
    return 0;
  }
  fprintf(stderr, "%s: expected %s, got %s\n", name, expected,
          actual == NULL ? "(null)" : actual);
  return 1;
}

int main(int argc, char **argv) {
  if (argc != 6) {
    fprintf(stderr, "usage: harness valid bad-target bad-name source-root system-root\n");
    return 1;
  }
  int failed = 0;
  MuonRuntimeHelperPlan plan;
  if (muon_runtime_helper_plan_from_path(argv[1], &plan) != 0) {
    fprintf(stderr, "valid helper path was rejected\n");
    failed = 1;
  } else {
    failed |= expect_string("package", plan.package_name, "sample-app");
    failed |= expect_string("target", plan.target, "linux-amd64");
    failed |= expect_string("source", plan.source_runtime_dir,
                            argv[4]);
    char *expected_system =
        muon_path_join(argv[5], "sample-app/linux-amd64/runtime");
    failed |= expect_string("system", plan.system_runtime_dir,
                            expected_system);
    free(expected_system);
    helper_plan_free(&plan);
  }
  if (muon_runtime_helper_plan_from_path(argv[2], &plan) == 0) {
    fprintf(stderr, "unsupported target was accepted\n");
    helper_plan_free(&plan);
    failed = 1;
  }
  if (muon_runtime_helper_plan_from_path(argv[3], &plan) == 0) {
    fprintf(stderr, "unsafe package name was accepted\n");
    helper_plan_free(&plan);
    failed = 1;
  }
  if (muon_runtime_helper_validate_source_runtime(argv[4]) == 0) {
    fprintf(stderr, "user-owned source runtime was accepted\n");
    failed = 1;
  }
  return failed;
}
HARNESS_EOF

gcc -std=c99 -Wall -Wextra -pedantic \
  -I"${SCRIPT_DIR}/src" \
  -I"${SCRIPT_DIR}/.deps/src/yyjson-0.12.0/src" \
  -DMUON_RUNTIME_HELPER_SOURCE_ROOT=\"${SOURCE_ROOT}\" \
  -DMUON_RUNTIME_HELPER_SYSTEM_ROOT=\"${SYSTEM_ROOT}\" \
  -o "${OUT_DIR}/runtime_helper_harness" \
  "${HARNESS}"

"${OUT_DIR}/runtime_helper_harness" \
  "${SOURCE_ROOT}/sample-app/dist-muon/linux-amd64/muon-runtime-helper" \
  "${SOURCE_ROOT}/bad-target/dist-muon/windows-amd64/muon-runtime-helper" \
  "${SOURCE_ROOT}/BadName/dist-muon/linux-amd64/muon-runtime-helper" \
  "${SOURCE_ROOT}/sample-app/dist-muon/linux-amd64" \
  "${SYSTEM_ROOT}"
