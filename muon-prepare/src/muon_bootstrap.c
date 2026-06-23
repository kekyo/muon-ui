// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <direct.h>
#include <process.h>
#include <windows.h>
#else
#include <limits.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "bootstrap_progress.h"
#include "prepare.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MUON_RECYCLE_EXIT_CODE 88

static int is_path_separator(char value) {
  return value == '/' || value == '\\';
}

static void normalize_path_separators(char *path) {
  if (path == NULL) {
    return;
  }
  for (char *cursor = path; *cursor != '\0'; cursor += 1) {
    if (*cursor == '\\') {
      *cursor = '/';
    }
  }
}

static char *duplicate_string(const char *value) {
  const size_t size = strlen(value) + 1;
  char *result = (char *)malloc(size);
  if (result == NULL) {
    return NULL;
  }
  memcpy(result, value, size);
  return result;
}

static char *path_join(const char *left, const char *right) {
  const int needs_separator = !is_path_separator(left[strlen(left) - 1]);
  const size_t size = strlen(left) + strlen(right) + (needs_separator ? 2 : 1);
  char *result = (char *)malloc(size);
  if (result == NULL) {
    return NULL;
  }
  snprintf(result, size, needs_separator ? "%s/%s" : "%s%s", left, right);
  return result;
}

static char *parent_directory(const char *path) {
  const char *slash = strrchr(path, '/');
  if (slash == NULL) {
    return duplicate_string(".");
  }
  if (slash == path) {
    return duplicate_string("/");
  }
#ifdef _WIN32
  if (slash == path + 2 && ((path[0] >= 'A' && path[0] <= 'Z') ||
                            (path[0] >= 'a' && path[0] <= 'z')) &&
      path[1] == ':') {
    char *root = (char *)malloc(4);
    if (root == NULL) {
      return NULL;
    }
    memcpy(root, path, 3);
    root[3] = '\0';
    return root;
  }
#endif
  const size_t length = (size_t)(slash - path);
  char *result = (char *)malloc(length + 1);
  if (result == NULL) {
    return NULL;
  }
  memcpy(result, path, length);
  result[length] = '\0';
  return result;
}

static char *get_bootstrap_path(const char *argv0) {
#ifdef _WIN32
  (void)argv0;
  char buffer[PATH_MAX];
  const DWORD length = GetModuleFileNameA(NULL, buffer, (DWORD)sizeof(buffer));
  if (length == 0 || length >= sizeof(buffer)) {
    return NULL;
  }
  buffer[length] = '\0';
  normalize_path_separators(buffer);
  return duplicate_string(buffer);
#else
  char buffer[PATH_MAX];
  const ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
  if (length >= 0) {
    buffer[length] = '\0';
    normalize_path_separators(buffer);
    return duplicate_string(buffer);
  }
  char *fallback = duplicate_string(argv0);
  normalize_path_separators(fallback);
  return fallback;
#endif
}

static const char *get_default_target(void) {
  return MUON_PREPARE_TARGET_NAME;
}

static const char *get_core_executable_name(void) {
#ifdef _WIN32
  return "muon-core.exe";
#else
  return "muon-core";
#endif
}

static char **create_core_argv(const char *core_path, int argc, char **argv) {
  char **core_argv = (char **)calloc((size_t)argc + 1, sizeof(char *));
  if (core_argv == NULL) {
    return NULL;
  }
  core_argv[0] = (char *)core_path;
  for (int index = 1; index < argc; index += 1) {
    core_argv[index] = argv[index];
  }
  core_argv[argc] = NULL;
  return core_argv;
}

static int launch_core(const char *runtime_dir, const char *core_path, int argc,
                       char **argv) {
  char **core_argv = create_core_argv(core_path, argc, argv);
  if (core_argv == NULL) {
    fprintf(stderr, "muon-bootstrap: failed to allocate arguments.\n");
    return 1;
  }
#ifdef _WIN32
  if (_chdir(runtime_dir) != 0) {
    perror(runtime_dir);
    free(core_argv);
    return 1;
  }
  const intptr_t status =
      _spawnv(_P_WAIT, core_path, (const char *const *)core_argv);
  if (status < 0) {
    perror(core_path);
    free(core_argv);
    return 127;
  }
  free(core_argv);
  return (int)status;
#else
  if (chdir(runtime_dir) != 0) {
    perror(runtime_dir);
    free(core_argv);
    return 1;
  }
  const pid_t child = fork();
  if (child < 0) {
    perror("fork");
    free(core_argv);
    return 1;
  }
  if (child == 0) {
    execv(core_path, core_argv);
    const int error_code = errno;
    perror(core_path);
    _exit(error_code == ENOENT ? 127 : 126);
  }

  free(core_argv);
  int status = 0;
  while (waitpid(child, &status, 0) < 0) {
    if (errno != EINTR) {
      perror("waitpid");
      return 1;
    }
  }
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  if (WIFSIGNALED(status)) {
    return 128 + WTERMSIG(status);
  }
  return 1;
#endif
}

static void on_prepare_progress(const MuonPrepareProgress *progress,
                                void *user_data) {
  muon_bootstrap_progress_update((MuonBootstrapProgress *)user_data, progress);
}

int main(int argc, char **argv) {
  char *bootstrap_path = get_bootstrap_path(argv[0]);
  char *runtime_dir =
      bootstrap_path == NULL ? NULL : parent_directory(bootstrap_path);
  char *core_path =
      runtime_dir == NULL ? NULL : path_join(runtime_dir, get_core_executable_name());
  if (bootstrap_path == NULL || runtime_dir == NULL || core_path == NULL) {
    fprintf(stderr, "muon-bootstrap: failed to resolve runtime directory.\n");
    free(bootstrap_path);
    free(runtime_dir);
    free(core_path);
    return 1;
  }
  const char *cache_dir = getenv("MUON_CACHE_DIR");
  int exit_code = 0;
  do {
    MuonBootstrapProgress progress;
    muon_bootstrap_progress_init(&progress);
    const int has_progress = muon_bootstrap_progress_is_available(&progress);
    if (muon_prepare_in_place_with_progress(
            runtime_dir, get_default_target(), cache_dir, 0, has_progress,
            has_progress ? on_prepare_progress : NULL,
            has_progress ? &progress : NULL) != 0) {
      if (has_progress) {
        muon_bootstrap_progress_fail(&progress);
      }
      muon_bootstrap_progress_dispose(&progress);
      free(bootstrap_path);
      free(runtime_dir);
      free(core_path);
      return 1;
    }
    muon_bootstrap_progress_dispose(&progress);
    exit_code = launch_core(runtime_dir, core_path, argc, argv);
  } while (exit_code == MUON_RECYCLE_EXIT_CODE);
  free(bootstrap_path);
  free(runtime_dir);
  free(core_path);
  return exit_code;
}
