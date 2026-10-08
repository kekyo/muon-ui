// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

#ifndef MUON_RUNTIME_INFO_GENERATED_H
#define MUON_RUNTIME_INFO_GENERATED_H

#include <stddef.h>

#define MUON_RUNTIME_INFO_AVAILABLE 0

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
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    NULL,
    {NULL, NULL, NULL, 0},
    NULL,
    0,
};

#endif
