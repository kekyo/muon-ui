/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

/**
 * Result status for app storage resource lookups.
 */
enum class MuonAppStorageStatus {
  /**
   * Resource was found and read successfully.
   */
  kOk,

  /**
   * Resource path was valid but no file exists.
   */
  kNotFound,

  /**
   * Request was rejected before filesystem access.
   */
  kRejected,

  /**
   * Resource exists but could not be read.
   */
  kReadError,
};

/**
 * Logical app storage request.
 */
struct MuonAppStorageRequest {
  /**
   * URL host component. v1 accepts only "main".
   */
  std::string host;

  /**
   * URL path component, with or without the leading slash.
   */
  std::string path;
};

/**
 * Resource returned by app storage.
 */
struct MuonAppStorageResource {
  /**
   * Lookup result status.
   */
  MuonAppStorageStatus status = MuonAppStorageStatus::kNotFound;

  /**
   * Response MIME type for successful reads.
   */
  std::string mime_type;

  /**
   * Resource bytes for successful reads.
   */
  std::vector<uint8_t> data;
};

/**
 * Storage backend for asset:// resources.
 */
class MuonAppStorage {
 public:
  virtual ~MuonAppStorage() = default;

  /**
   * Reads a resource by logical host and path.
   *
   * @param request Logical resource request.
   * @return Resource bytes and MIME type when found.
   */
  virtual MuonAppStorageResource ReadResource(
      const MuonAppStorageRequest& request) = 0;
};

/**
 * Creates a filesystem-backed app storage rooted at an assets directory.
 *
 * @param asset_root Directory containing per-host app assets.
 * @return Storage implementation that serves files below the asset root.
 */
std::shared_ptr<MuonAppStorage> CreateMuonFileAppStorage(
    std::filesystem::path asset_root);

/**
 * Creates a ZIP-backed app storage rooted at an asset archive file.
 *
 * @param archive_path ZIP file containing per-host app assets.
 * @return Storage implementation that serves files from the archive.
 */
std::shared_ptr<MuonAppStorage> CreateMuonZipAppStorage(
    std::filesystem::path archive_path);

/**
 * Creates app storage from an optional muon.json asset.sourcePath path.
 *
 * @remarks Missing asset.sourcePath keeps the executable-directory/assets
 * default.
 * Explicit directories are served directly, and explicit regular files are
 * treated as ZIP archives.
 *
 * @param has_asset_from Whether asset.sourcePath was explicitly configured.
 * @param asset_from Configured asset.sourcePath path.
 * @param has_asset_signature Whether asset.signature was explicitly configured.
 * @param asset_signature Expected SHA-256 hexadecimal digest for ZIP storage.
 * @param has_asset_salt Whether asset.salt was explicitly configured.
 * @param asset_salt Bytes appended to the ZIP stream for SHA-256 comparison.
 * @param error_message Receives startup validation errors.
 * @return Storage implementation, or null when validation fails.
 */
std::shared_ptr<MuonAppStorage> CreateConfiguredMuonAppStorage(
    bool has_asset_from,
    const std::filesystem::path& asset_from,
    bool has_asset_signature,
    const std::string& asset_signature,
    bool has_asset_salt,
    const std::vector<uint8_t>& asset_salt,
    std::string* error_message);

/**
 * Creates the default filesystem-backed app storage for the running executable.
 *
 * @return Storage implementation rooted at executable-directory/assets.
 */
std::shared_ptr<MuonAppStorage> CreateDefaultMuonFileAppStorage();
