// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

#ifndef MUON_PREPARE_NODE_H
#define MUON_PREPARE_NODE_H

#include <stddef.h>

#include "prepare_progress.h"

/** File name used for the cached official Node.js release index. */
#define MUON_PREPARE_NODE_CATALOG_FILE_NAME "node-catalog.json"

/**
 * Official Node.js distribution metadata for one public muon target.
 */
typedef struct {
  /** Value required in the release index `files` array. */
  const char *catalog_file;
  /** Target fragment used in the official archive file name. */
  const char *archive_target;
  /** Official archive file suffix. */
  const char *archive_suffix;
  /** Node executable path relative to the installed runtime root. */
  const char *executable_relative_path;
} MuonNodeTargetInfo;

/**
 * One normalized AND set in a Node.js semantic-version requirement.
 */
typedef struct {
  /** Owned normalized semantic-version comparator strings. */
  char **comparators;
  /** Number of comparator strings. Zero represents a wildcard set. */
  size_t count;
} MuonNodeComparatorSet;

/**
 * Owned normalized Node.js runtime requirement decoded from JSON input or an
 * embedded launcher configuration.
 */
typedef struct {
  /** Non-zero when absence of a compatible Node.js runtime is fatal. */
  int required;
  /** Non-zero when `package.json` explicitly specified `engines.node`. */
  int engine_range_specified;
  /** Owned original `package.json` `engines.node` diagnostic text. */
  char *engine_range;
  /** Owned normalized comparator sets forming an outer OR expression. */
  MuonNodeComparatorSet *sets;
  /** Number of comparator sets. */
  size_t set_count;
} MuonNodeRuntimeRequirement;

/**
 * Validates an owned normalized Node.js runtime requirement.
 *
 * @param requirement Requirement to validate.
 * @return 0 when every field and normalized comparator is valid, or non-zero
 *     otherwise.
 */
int muon_prepare_validate_node_runtime_requirement(
    const MuonNodeRuntimeRequirement *requirement);

/**
 * Owned metadata for one selected official Node.js archive.
 */
typedef struct {
  /** Owned release version including the leading `v`. */
  char *version;
  /** Owned release-index target identifier. */
  char *catalog_file;
  /** Owned archive target identifier. */
  char *archive_target;
  /** Owned official archive file name. */
  char *file_name;
  /** Owned official archive download URL. */
  char *url;
  /** Owned lowercase SHA-256 digest populated before archive download. */
  char *sha256;
  /** Owned LTS codename, or `NULL` for a non-LTS release. */
  char *lts;
} MuonNodeArtifact;

/**
 * Resolves official Node.js distribution metadata for a public muon target.
 *
 * @param target Public muon target such as `linux-amd64`.
 * @param info Receives static target metadata on success.
 * @return 0 on success, or non-zero for an unsupported target.
 */
int muon_prepare_get_node_target_info(const char *target,
                                      MuonNodeTargetInfo *info);

/**
 * Strictly decodes a normalized Node.js runtime requirement JSON object.
 *
 * @param json JSON text supplied by `--node-runtime-requirement`.
 * @param requirement Receives owned decoded fields on success.
 * @return 0 on success, or non-zero when the JSON or comparator schema is
 *     invalid.
 */
int muon_prepare_parse_node_runtime_requirement(
    const char *json, MuonNodeRuntimeRequirement *requirement);

/**
 * Frees fields owned by a Node.js runtime requirement.
 *
 * @param requirement Requirement to release.
 */
void muon_prepare_free_node_runtime_requirement(
    MuonNodeRuntimeRequirement *requirement);

/**
 * Downloads and atomically replaces the cached official Node.js release index.
 *
 * @param cache_dir muon cache directory.
 * @param force Non-zero to refresh even when a cached index exists.
 * @param updated Receives non-zero when the cached index was replaced, or may
 *     be `NULL`.
 * @param progress_callback Optional progress callback.
 * @param progress_user_data Opaque progress callback state.
 * @return 0 on success, including fallback to an existing cached index, or
 *     non-zero when no usable index is available.
 */
int muon_prepare_ensure_node_catalog_cache_with_status_progress(
    const char *cache_dir, int force, int *updated,
    MuonPrepareProgressCallback progress_callback, void *progress_user_data);

/**
 * Selects the newest compatible official Node.js archive from the cached index.
 *
 * @param cache_dir muon cache directory.
 * @param target Public muon runtime target.
 * @param requirement Normalized runtime requirement.
 * @param artifact Receives owned selected release metadata.
 * @return 0 on success, or non-zero for invalid catalog data or when no
 *     compatible target release exists.
 *
 * When `engines.node` was omitted, only matching LTS releases are considered.
 * When it was specified, matching LTS releases are preferred and every
 * matching release is considered only if no LTS release matches. Within the
 * selected group, the greatest semantic version is selected.
 */
int muon_prepare_resolve_node_artifact(
    const char *cache_dir, const char *target,
    const MuonNodeRuntimeRequirement *requirement, MuonNodeArtifact *artifact);

/**
 * Ensures a selected Node.js archive and its official checksum are cached.
 *
 * @param cache_dir muon cache directory.
 * @param artifact Selected artifact. Its owned `sha256` field is replaced with
 *     the exact checksum selected from `SHASUMS256.txt`.
 * @param force Non-zero to redownload cached checksum and archive files.
 * @param archive_path Receives the owned cached archive path on success.
 * @param progress_callback Optional progress callback.
 * @param progress_user_data Opaque progress callback state.
 * @return 0 on success, or non-zero on download, checksum schema, or SHA-256
 *     verification failure.
 */
int muon_prepare_ensure_node_archive_cache_progress(
    const char *cache_dir, MuonNodeArtifact *artifact, int force,
    char **archive_path, MuonPrepareProgressCallback progress_callback,
    void *progress_user_data);

/**
 * Selectively installs an official Node.js archive into a runtime namespace.
 *
 * @param archive_path Verified official Node.js archive to read.
 * @param target Public muon runtime target.
 * @param artifact Selected official artifact metadata. Its target identifiers
 *     and file name must exactly match `target`.
 * @param runtime_root Application runtime root that receives
 *     `runtimes/node`.
 * @param file_count Receives the number of installed files on success, or zero
 *     on failure. May be `NULL`.
 * @param progress_callback Optional progress callback.
 * @param progress_user_data Opaque progress callback state.
 * @return 0 after atomically installing the Node executable and `LICENSE`, or
 *     non-zero when metadata or archive contents are invalid or installation
 *     fails.
 *
 * Archive paths are validated before entries are considered for extraction.
 * Only regular, non-link entries at the two official paths are accepted, and
 * each path must occur exactly once. Other safe entries are ignored. An
 * existing installed runtime remains active until the candidate is complete
 * and is restored if the atomic replacement cannot be committed.
 */
int muon_prepare_install_node_runtime_progress(
    const char *archive_path, const char *target,
    const MuonNodeArtifact *artifact, const char *runtime_root,
    size_t *file_count, MuonPrepareProgressCallback progress_callback,
    void *progress_user_data);

/**
 * Frees fields owned by Node.js artifact metadata.
 *
 * @param artifact Artifact metadata to release.
 */
void muon_prepare_free_node_artifact(MuonNodeArtifact *artifact);

#endif
