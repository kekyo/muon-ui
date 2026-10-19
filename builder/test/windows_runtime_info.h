// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

#ifndef MUON_WINDOWS_TEST_RUNTIME_INFO_H
#define MUON_WINDOWS_TEST_RUNTIME_INFO_H

#include <stddef.h>

// Offline metadata for the real builder CLI. Tests supply local CEF payloads;
// an accidental download must fail instead of reaching an external service.
#define MUON_RUNTIME_INFO_AVAILABLE 1

#ifndef MUON_TEST_CEF_SHA1
#define MUON_TEST_CEF_SHA1 "0000000000000000000000000000000000000000"
#define MUON_TEST_CEF_SIZE 1
#endif

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
  .name = "muon-core",
  .executable_name = "muon-core.exe",
  .target = MUON_PREPARE_TARGET_NAME,
#ifdef _WIN64
  .cef_target = "windows64",
#else
  .cef_target = "windows32",
#endif
  .muon_core_version = "test-core",
  .muon_core_git_commit_hash = "test-commit",
  .muon_core_build_date = "2026-10-19T00:00:00Z",
  .muon_core_git_commit_date = "2026-10-19T00:00:00Z",
  .cef_reference_version = "test-cef",
  .cef_reference_distribution = "minimal",
  .cef_reference_api_version = 14700,
  .cef_reference_api_hash = "test-api",
  .cef_reference_artifact = {"test-cef.tar.bz2", "unused", MUON_TEST_CEF_SHA1, MUON_TEST_CEF_SIZE},
  .core_payload = NULL,
  .core_payload_count = 0,
};

#endif
