// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

#ifndef _WIN32
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#define _POSIX_C_SOURCE 200809L
#endif

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "common.h"
#include "prepare.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef MUON_RUNTIME_HELPER_SOURCE_ROOT
#define MUON_RUNTIME_HELPER_SOURCE_ROOT "/usr/lib"
#endif

#ifndef MUON_RUNTIME_HELPER_SYSTEM_ROOT
#define MUON_RUNTIME_HELPER_SYSTEM_ROOT "/var/lib/muon/apps"
#endif

#ifndef MUON_RUNTIME_HELPER_CEF_CACHE
#define MUON_RUNTIME_HELPER_CEF_CACHE "/var/cache/muon/cef"
#endif

#define MUON_RUNTIME_HELPER_NAME "muon-runtime-helper"
#define MUON_RUNTIME_HELPER_DIST_DIR "dist-muon"

extern char **environ;

typedef struct {
  char *package_name;
  char *target;
  char *source_runtime_dir;
  char *system_runtime_dir;
  char *cache_dir;
} MuonRuntimeHelperPlan;

static const char *path_file_name(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash == NULL ? path : slash + 1;
}

static int path_is_absolute(const char *path) {
  return path != NULL && path[0] == '/';
}

static int path_is_direct_child_of(const char *path, const char *parent) {
  const size_t parent_length = strlen(parent);
  return strncmp(path, parent, parent_length) == 0 &&
         path[parent_length] == '/' &&
         strchr(path + parent_length + 1, '/') == NULL;
}

static char *canonical_path(const char *path) {
  char buffer[PATH_MAX];
  if (realpath(path, buffer) == NULL) {
    muon_print_errno(path);
    return NULL;
  }
  return muon_duplicate_path_string(buffer);
}

static int component_is_safe_package_name(const char *value) {
  if (value == NULL || value[0] == '\0') {
    return 0;
  }
  const char first = value[0];
  if (first < 'a' || first > 'z') {
    return 0;
  }
  for (const char *cursor = value; *cursor != '\0'; cursor += 1) {
    const int digit = *cursor >= '0' && *cursor <= '9';
    const int lower = *cursor >= 'a' && *cursor <= 'z';
    if (!digit && !lower && *cursor != '+' && *cursor != '.' &&
        *cursor != '-') {
      return 0;
    }
  }
  return 1;
}

static int component_is_supported_linux_target(const char *value) {
  return strcmp(value, "linux-amd64") == 0 ||
         strcmp(value, "linux-armhf") == 0 ||
         strcmp(value, "linux-arm64") == 0;
}

static void helper_plan_init(MuonRuntimeHelperPlan *plan) {
  memset(plan, 0, sizeof(*plan));
}

static void helper_plan_free(MuonRuntimeHelperPlan *plan) {
  free(plan->package_name);
  free(plan->target);
  free(plan->source_runtime_dir);
  free(plan->system_runtime_dir);
  free(plan->cache_dir);
  helper_plan_init(plan);
}

static char *create_system_runtime_dir(const char *package_name,
                                       const char *target) {
  char *package_dir =
      muon_path_join(MUON_RUNTIME_HELPER_SYSTEM_ROOT, package_name);
  char *target_dir = package_dir == NULL ? NULL : muon_path_join(package_dir, target);
  char *runtime_dir = target_dir == NULL ? NULL : muon_path_join(target_dir, "runtime");
  free(package_dir);
  free(target_dir);
  return runtime_dir;
}

int muon_runtime_helper_plan_from_path(const char *helper_path,
                                       MuonRuntimeHelperPlan *plan) {
  helper_plan_init(plan);
  if (!path_is_absolute(helper_path)) {
    muon_print_error("muon-runtime-helper: helper path is not absolute.\n");
    return -1;
  }
  char *canonical_helper = canonical_path(helper_path);
  char *canonical_source_root = canonical_path(MUON_RUNTIME_HELPER_SOURCE_ROOT);
  char *source_runtime_dir =
      canonical_helper == NULL ? NULL : muon_parent_directory(canonical_helper);
  char *dist_dir =
      source_runtime_dir == NULL ? NULL : muon_parent_directory(source_runtime_dir);
  char *package_dir = dist_dir == NULL ? NULL : muon_parent_directory(dist_dir);
  char *source_root = package_dir == NULL ? NULL : muon_parent_directory(package_dir);
  if (canonical_helper == NULL || canonical_source_root == NULL ||
      source_runtime_dir == NULL || dist_dir == NULL || package_dir == NULL ||
      source_root == NULL) {
    free(canonical_helper);
    free(canonical_source_root);
    free(source_runtime_dir);
    free(dist_dir);
    free(package_dir);
    free(source_root);
    return -1;
  }
  const char *helper_name = path_file_name(canonical_helper);
  const char *target = path_file_name(source_runtime_dir);
  const char *dist_name = path_file_name(dist_dir);
  const char *package_name = path_file_name(package_dir);
  int result = 0;
  if (strcmp(helper_name, MUON_RUNTIME_HELPER_NAME) != 0 ||
      strcmp(dist_name, MUON_RUNTIME_HELPER_DIST_DIR) != 0 ||
      strcmp(source_root, canonical_source_root) != 0 ||
      !path_is_direct_child_of(package_dir, canonical_source_root) ||
      !component_is_safe_package_name(package_name) ||
      !component_is_supported_linux_target(target)) {
    muon_print_error("muon-runtime-helper: invalid helper placement: %s\n",
                     canonical_helper);
    result = -1;
  } else {
    plan->package_name = muon_duplicate_string(package_name);
    plan->target = muon_duplicate_string(target);
    plan->source_runtime_dir = muon_duplicate_string(source_runtime_dir);
    plan->system_runtime_dir = create_system_runtime_dir(package_name, target);
    plan->cache_dir = muon_duplicate_string(MUON_RUNTIME_HELPER_CEF_CACHE);
    if (plan->package_name == NULL || plan->target == NULL ||
        plan->source_runtime_dir == NULL || plan->system_runtime_dir == NULL ||
        plan->cache_dir == NULL) {
      helper_plan_free(plan);
      result = -1;
    }
  }
  free(canonical_helper);
  free(canonical_source_root);
  free(source_runtime_dir);
  free(dist_dir);
  free(package_dir);
  free(source_root);
  return result;
}

int muon_runtime_helper_validate_source_runtime(const char *path) {
  struct stat entry;
  if (stat(path, &entry) != 0) {
    muon_print_errno(path);
    return -1;
  }
  if (!S_ISDIR(entry.st_mode) || entry.st_uid != 0 ||
      (entry.st_mode & 022) != 0) {
    muon_print_error(
        "muon-runtime-helper: source runtime must be a root-owned, non-user-writable directory: %s\n",
        path);
    return -1;
  }
  return 0;
}

static int validate_helper_binary(const char *path) {
  struct stat entry;
  if (stat(path, &entry) != 0) {
    muon_print_errno(path);
    return -1;
  }
  if (!S_ISREG(entry.st_mode) || entry.st_uid != 0 ||
      (entry.st_mode & S_ISUID) == 0 || (entry.st_mode & 0111) == 0) {
    muon_print_error(
        "muon-runtime-helper: helper must be root-owned and setuid executable: %s\n",
        path);
    return -1;
  }
  return 0;
}

static int reset_environment(void) {
  environ = NULL;
  return setenv("PATH", "/usr/sbin:/usr/bin:/sbin:/bin", 1) != 0 ||
         setenv("HOME", "/var/lib/muon", 1) != 0 ||
         setenv("LC_ALL", "C", 1) != 0
             ? -1
             : 0;
}

static int normalize_runtime_tree(const char *path) {
  struct stat entry;
  if (lstat(path, &entry) != 0) {
    muon_print_errno(path);
    return -1;
  }
  if (S_ISDIR(entry.st_mode)) {
    DIR *directory = opendir(path);
    if (directory == NULL) {
      muon_print_errno(path);
      return -1;
    }
    struct dirent *child = NULL;
    while ((child = readdir(directory)) != NULL) {
      if (strcmp(child->d_name, ".") == 0 || strcmp(child->d_name, "..") == 0) {
        continue;
      }
      char *child_path = muon_path_join(path, child->d_name);
      if (child_path == NULL || normalize_runtime_tree(child_path) != 0) {
        free(child_path);
        closedir(directory);
        return -1;
      }
      free(child_path);
    }
    closedir(directory);
    if (chown(path, 0, 0) != 0 || chmod(path, 0755) != 0) {
      muon_print_errno(path);
      return -1;
    }
    return 0;
  }
  if (!S_ISREG(entry.st_mode)) {
    muon_print_error("muon-runtime-helper: unsupported runtime file type: %s\n",
                     path);
    return -1;
  }
  const int is_chrome_sandbox =
      strcmp(path_file_name(path), "chrome-sandbox") == 0;
  const mode_t mode = is_chrome_sandbox
                          ? 04755
                          : ((entry.st_mode & 0111) != 0 ? 0755 : 0644);
  if (chown(path, 0, 0) != 0 || chmod(path, mode) != 0) {
    muon_print_errno(path);
    return -1;
  }
  return 0;
}

static char *get_helper_path(const char *argv0) {
  (void)argv0;
  char buffer[PATH_MAX];
  const ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
  if (length < 0 || (size_t)length >= sizeof(buffer)) {
    muon_print_errno("/proc/self/exe");
    return NULL;
  }
  buffer[length] = '\0';
  return muon_duplicate_path_string(buffer);
}

int main(int argc, char **argv) {
  if (argc != 1) {
    muon_print_error("muon-runtime-helper: arguments are not accepted.\n");
    return 2;
  }
  if (geteuid() != 0) {
    muon_print_error("muon-runtime-helper: effective root is required.\n");
    return 1;
  }
  char *helper_path = get_helper_path(argv[0]);
  MuonRuntimeHelperPlan plan;
  if (helper_path == NULL ||
      muon_runtime_helper_plan_from_path(helper_path, &plan) != 0 ||
      validate_helper_binary(helper_path) != 0 ||
      muon_runtime_helper_validate_source_runtime(plan.source_runtime_dir) != 0 ||
      reset_environment() != 0) {
    free(helper_path);
    return 1;
  }
  const int prepare_result = muon_prepare_staged_with_progress(
      plan.source_runtime_dir, plan.system_runtime_dir, plan.target,
      plan.cache_dir, 0, 0, NULL, NULL);
  const int normalize_result =
      prepare_result == 0 ? normalize_runtime_tree(plan.system_runtime_dir) : -1;
  const int result = prepare_result == 0 && normalize_result == 0 ? 0 : 1;
  helper_plan_free(&plan);
  free(helper_path);
  return result;
}
