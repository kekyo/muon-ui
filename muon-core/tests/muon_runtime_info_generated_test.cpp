/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "muon_runtime_info_generated.h"

#include <cstring>

#ifndef EXPECTED_EXECUTABLE_NAME
#define EXPECTED_EXECUTABLE_NAME ""
#endif

#ifndef EXPECTED_TARGET_NAME
#define EXPECTED_TARGET_NAME ""
#endif

#ifndef EXPECTED_CEF_TARGET_NAME
#define EXPECTED_CEF_TARGET_NAME ""
#endif

#ifndef EXPECTED_MUON_CORE_VERSION
#ifdef MUON_CORE_VERSION
#define EXPECTED_MUON_CORE_VERSION MUON_CORE_VERSION
#else
#define EXPECTED_MUON_CORE_VERSION ""
#endif
#endif

#ifndef EXPECTED_MUON_CORE_GIT_COMMIT_HASH
#ifdef MUON_CORE_GIT_COMMIT_HASH
#define EXPECTED_MUON_CORE_GIT_COMMIT_HASH MUON_CORE_GIT_COMMIT_HASH
#else
#define EXPECTED_MUON_CORE_GIT_COMMIT_HASH ""
#endif
#endif

#ifndef EXPECTED_CEF_VERSION
#define EXPECTED_CEF_VERSION ""
#endif

#ifndef EXPECTED_CEF_API_VERSION
#define EXPECTED_CEF_API_VERSION 0
#endif

static int StringEquals(const char* left, const char* right) {
  return left != nullptr && right != nullptr && std::strcmp(left, right) == 0;
}

int main() {
  if (MUON_RUNTIME_INFO_AVAILABLE != 1) {
    return 1;
  }
  if (!StringEquals(kMuonRuntimeInfo.name, "muon-core")) {
    return 2;
  }
  if (!StringEquals(kMuonRuntimeInfo.executable_name,
                    EXPECTED_EXECUTABLE_NAME)) {
    return 3;
  }
  if (!StringEquals(kMuonRuntimeInfo.target, EXPECTED_TARGET_NAME)) {
    return 4;
  }
  if (!StringEquals(kMuonRuntimeInfo.cef_target, EXPECTED_CEF_TARGET_NAME)) {
    return 14;
  }
  if (!StringEquals(kMuonRuntimeInfo.muon_core_version,
                    EXPECTED_MUON_CORE_VERSION)) {
    return 5;
  }
  if (!StringEquals(kMuonRuntimeInfo.muon_core_git_commit_hash,
                    EXPECTED_MUON_CORE_GIT_COMMIT_HASH)) {
    return 6;
  }
  if (kMuonRuntimeInfo.muon_core_build_date == nullptr ||
      kMuonRuntimeInfo.muon_core_build_date[0] == '\0') {
    return 15;
  }
  if (kMuonRuntimeInfo.muon_core_git_commit_date == nullptr ||
      kMuonRuntimeInfo.muon_core_git_commit_date[0] == '\0') {
    return 16;
  }
  if (!StringEquals(kMuonRuntimeInfo.cef_reference_version,
                    EXPECTED_CEF_VERSION)) {
    return 7;
  }
  if (kMuonRuntimeInfo.cef_reference_api_version !=
      EXPECTED_CEF_API_VERSION) {
    return 8;
  }
  if (kMuonRuntimeInfo.cef_reference_api_hash == nullptr ||
      kMuonRuntimeInfo.cef_reference_api_hash[0] == '\0') {
    return 9;
  }
  if (kMuonRuntimeInfo.cef_reference_artifact.file_name == nullptr ||
      kMuonRuntimeInfo.cef_reference_artifact.file_name[0] == '\0' ||
      kMuonRuntimeInfo.cef_reference_artifact.url == nullptr ||
      kMuonRuntimeInfo.cef_reference_artifact.url[0] == '\0' ||
      kMuonRuntimeInfo.cef_reference_artifact.sha1 == nullptr ||
      kMuonRuntimeInfo.cef_reference_artifact.sha1[0] == '\0' ||
      kMuonRuntimeInfo.cef_reference_artifact.size == 0) {
    return 10;
  }
  for (size_t index = 0; index < kMuonRuntimeInfo.core_payload_count;
       index += 1) {
    const char* item = kMuonRuntimeInfo.core_payload[index];
    if (item == nullptr || item[0] == '\0') {
      return 11;
    }
    if (std::strcmp(item, "muon-runtime.json") == 0) {
      return 12;
    }
  }
  return 0;
}
