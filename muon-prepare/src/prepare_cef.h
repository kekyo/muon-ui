// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

#ifndef MUON_PREPARE_CEF_H
#define MUON_PREPARE_CEF_H

#include <stddef.h>

#include "prepare_progress.h"

/**
 * CEF artifact metadata resolved from the cached catalog.
 */
typedef struct {
  /** CEF version requested by the caller. */
  char *version;
  /** Muon target name such as linux64, windows32, or windows64. */
  char *target;
  /** CEF distribution name. Currently always minimal. */
  char *distribution;
  /** Archive file name in the artifact cache. */
  char *file_name;
  /** Download URL for the archive. */
  char *url;
  /** Expected SHA1 digest for the archive. */
  char *sha1;
  /** Expected archive size in bytes. */
  unsigned long long size;
} MuonCefArtifact;

/**
 * CEF reference metadata embedded in the Muon runtime.
 */
typedef struct {
  /** Tested CEF binary distribution version. */
  const char *version;
  /** Muon target name such as linux64, windows32, or windows64. */
  const char *target;
  /** Tested CEF binary distribution kind. */
  const char *distribution;
  /** Stable CEF C API version used by muon-core. */
  int api_version;
  /** Platform API hash for the configured CEF C API version. */
  const char *api_hash;
  /** Tested CEF archive file name. */
  const char *artifact_file_name;
  /** Tested CEF archive URL. */
  const char *artifact_url;
  /** Tested CEF archive SHA-1 digest. */
  const char *artifact_sha1;
  /** Tested CEF archive size in bytes. */
  unsigned long long artifact_size;
} MuonCefReference;

/**
 * Sets quiet mode for CEF preparation diagnostics.
 *
 * @param quiet Non-zero to suppress diagnostic output.
 */
void muon_prepare_set_cef_quiet(int quiet);

/**
 * Frees strings owned by a CEF artifact metadata object.
 *
 * @param artifact Artifact metadata to release.
 */
void muon_prepare_free_cef_artifact(MuonCefArtifact *artifact);

/**
 * Downloads and atomically replaces the cached CEF catalog when possible.
 *
 * @param cache_dir Muon cache directory.
 * @param force Non-zero to refresh even when a cached catalog exists.
 * @return 0 on success, or non-zero on failure.
 *
 * Existing catalog data is used as a fallback when refresh fails.
 */
int muon_prepare_ensure_catalog_cache(const char *cache_dir, int force);
int muon_prepare_ensure_catalog_cache_with_status(const char *cache_dir,
                                                  int force,
                                                  int *updated);
int muon_prepare_ensure_catalog_cache_with_status_progress(
    const char *cache_dir, int force, int *updated,
    MuonPrepareProgressCallback progress_callback, void *progress_user_data);

/**
 * Resolves metadata for a CEF artifact from the cached catalog.
 *
 * @param cache_dir Muon cache directory.
 * @param version Requested CEF version.
 * @param target Requested target platform.
 * @param distribution Requested distribution name.
 * @param artifact Destination metadata object.
 * @return 0 on success, or non-zero on failure.
 */
int muon_prepare_resolve_cef_artifact(const char *cache_dir,
                                      const char *version,
                                      const char *target,
                                      const char *distribution,
                                      MuonCefArtifact *artifact);

/**
 * Resolves and verifies a CEF archive according to a version policy.
 *
 * @param cache_dir Muon cache directory.
 * @param reference Runtime embedded CEF reference metadata.
 * @param policy Version policy: tested, same-major-latest, compat-latest, or exact.
 * @param exact_version CEF version used by the exact policy.
 * @param force Non-zero to redownload selected archives.
 * @param artifact Receives selected artifact metadata.
 * @param archive_path Receives the cached archive path.
 * @return 0 on success, or non-zero on failure.
 */
int muon_prepare_ensure_cef_archive_cache_for_policy(
    const char *cache_dir, const MuonCefReference *reference,
    const char *policy, const char *exact_version, int force,
    MuonCefArtifact *artifact, char **archive_path);
int muon_prepare_ensure_cef_archive_cache_for_policy_progress(
    const char *cache_dir, const MuonCefReference *reference,
    const char *policy, const char *exact_version, int force,
    MuonCefArtifact *artifact, char **archive_path,
    MuonPrepareProgressCallback progress_callback, void *progress_user_data);

/**
 * Ensures the CEF archive exists in the artifact cache and is verified.
 *
 * @param cache_dir Muon cache directory.
 * @param artifact Artifact metadata resolved from the catalog.
 * @param force Non-zero to redownload the archive.
 * @param archive_path Receives the cached archive path.
 * @return 0 on success, or non-zero on failure.
 */
int muon_prepare_ensure_cef_artifact_cache(
    const char *cache_dir, const MuonCefArtifact *artifact, int force,
    char **archive_path);
int muon_prepare_ensure_cef_artifact_cache_progress(
    const char *cache_dir, const MuonCefArtifact *artifact, int force,
    char **archive_path, MuonPrepareProgressCallback progress_callback,
    void *progress_user_data);

/**
 * Extracts a full CEF distribution root from an archive.
 *
 * @param archive_path CEF archive path.
 * @param output_dir Destination directory for the extracted CEF root.
 * @param force Non-zero to replace an existing ready destination.
 * @param file_count Receives the number of CEF payload files.
 * @return 0 on success, or non-zero on failure.
 *
 * Existing output is reused only when its ready marker matches the archive
 * fingerprint.
 */
int muon_prepare_extract_cef_archive_full(const char *archive_path,
                                          const char *output_dir, int force,
                                          size_t *file_count);

#endif
