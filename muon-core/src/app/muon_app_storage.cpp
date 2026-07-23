/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "app/muon_app_storage.h"

#include "config/muon_paths.h"
#include "muon_sha256.h"

#include "include/cef_parser.h"

#include "miniz.h"

#include <cstddef>
#include <fstream>
#include <iterator>
#include <limits>
#include <system_error>
#include <utility>

static constexpr char kMuonMainAppHost[] = "main";

static bool IsHexDigit(char value) {
  return (value >= '0' && value <= '9') ||
         (value >= 'A' && value <= 'F') ||
         (value >= 'a' && value <= 'f');
}

static char ToLowerHexDigit(char value) {
  if (value >= 'A' && value <= 'F') {
    return static_cast<char>(value - 'A' + 'a');
  }
  return value;
}

static uint8_t DecodeHexNibble(char value) {
  if (value >= '0' && value <= '9') {
    return static_cast<uint8_t>(value - '0');
  }
  if (value >= 'A' && value <= 'F') {
    return static_cast<uint8_t>(value - 'A' + 10);
  }
  return static_cast<uint8_t>(value - 'a' + 10);
}

static uint8_t DecodeHexByte(char high, char low) {
  return static_cast<uint8_t>((DecodeHexNibble(high) << 4) |
                              DecodeHexNibble(low));
}

static bool DecodeAppPath(const std::string& path,
                          std::string* decoded_path) {
  if (decoded_path == nullptr) {
    return false;
  }

  decoded_path->clear();
  decoded_path->reserve(path.size());
  for (auto index = size_t{0}; index < path.size(); ++index) {
    const auto value = path[index];
    if (value == '\0' || value == '\\') {
      return false;
    }
    if (value != '%') {
      decoded_path->push_back(value);
      continue;
    }
    if (index + 2 >= path.size() || !IsHexDigit(path[index + 1]) ||
        !IsHexDigit(path[index + 2])) {
      return false;
    }
    const auto decoded =
        static_cast<char>(DecodeHexByte(path[index + 1], path[index + 2]));
    if (decoded == '\0' || decoded == '/' || decoded == '\\') {
      return false;
    }
    decoded_path->push_back(decoded);
    index += 2;
  }
  return true;
}

static bool SplitRelativeAppPath(const std::string& path,
                                 std::vector<std::string>* segments) {
  if (segments == nullptr) {
    return false;
  }

  segments->clear();
  std::string decoded_path;
  if (!DecodeAppPath(path, &decoded_path)) {
    return false;
  }

  auto start = size_t{0};
  while (start < decoded_path.size() && decoded_path[start] == '/') {
    ++start;
  }
  if (start == decoded_path.size()) {
    return false;
  }

  while (start <= decoded_path.size()) {
    const auto end = decoded_path.find('/', start);
    const auto segment =
        decoded_path.substr(start, end == std::string::npos
                                       ? std::string::npos
                                       : end - start);
    if (segment.empty() || segment == "." || segment == "..") {
      return false;
    }
    segments->push_back(segment);
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return !segments->empty();
}

static bool IsPathInsideDirectory(const std::filesystem::path& path,
                                  const std::filesystem::path& directory) {
  auto path_iterator = path.begin();
  auto directory_iterator = directory.begin();
  for (; directory_iterator != directory.end();
       ++path_iterator, ++directory_iterator) {
    if (path_iterator == path.end() || *path_iterator != *directory_iterator) {
      return false;
    }
  }
  return true;
}

static std::string GetMimeTypeForPath(const std::filesystem::path& path) {
  auto extension = path.extension().string();
  if (!extension.empty() && extension.front() == '.') {
    extension.erase(extension.begin());
  }

  auto mime_type = CefGetMimeType(extension).ToString();
  if (mime_type.empty()) {
    mime_type = "application/octet-stream";
  }
  return mime_type;
}

static bool GetValidAppRequestSegments(const MuonAppStorageRequest& request,
                                       std::vector<std::string>* segments) {
  if (request.host != kMuonMainAppHost) {
    return false;
  }
  return SplitRelativeAppPath(request.path, segments);
}

static std::string JoinZipEntryPath(const std::string& host,
                                    const std::vector<std::string>& segments) {
  auto entry_path = host;
  for (const auto& segment : segments) {
    entry_path.push_back('/');
    entry_path.append(segment);
  }
  return entry_path;
}

using muon_internal::CalculateFileSha256Hex;

static bool ReadBinaryFile(const std::filesystem::path& path,
                           std::vector<uint8_t>* data) {
  if (data == nullptr) {
    return false;
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return false;
  }
  data->assign((std::istreambuf_iterator<char>(input)),
               std::istreambuf_iterator<char>());
  return input.eof() || !input.fail();
}

class MuonZipReader final {
 public:
  explicit MuonZipReader(const std::vector<uint8_t>& archive_data) {
    mz_zip_zero_struct(&archive_);
    initialized_ = mz_zip_reader_init_mem(
        &archive_, archive_data.data(), archive_data.size(), 0) == MZ_TRUE;
  }

  ~MuonZipReader() {
    if (initialized_) {
      mz_zip_reader_end(&archive_);
    }
  }

  MuonZipReader(const MuonZipReader&) = delete;
  MuonZipReader& operator=(const MuonZipReader&) = delete;

  bool initialized() const { return initialized_; }

  mz_zip_archive* archive() { return &archive_; }

 private:
  mz_zip_archive archive_{};
  bool initialized_ = false;
};

static bool IsZipArchiveData(const std::vector<uint8_t>& archive_data) {
  MuonZipReader reader(archive_data);
  return reader.initialized();
}

static MuonAppStorageResource ExtractZipEntrySync(
    const std::filesystem::path& archive_path,
    const std::string& entry_path) {
  std::vector<uint8_t> archive_data;
  if (!ReadBinaryFile(archive_path, &archive_data)) {
    return {MuonAppStorageStatus::kReadError, "", {}};
  }

  MuonZipReader reader(archive_data);
  if (!reader.initialized()) {
    return {MuonAppStorageStatus::kReadError, "", {}};
  }

  const auto entry_index = mz_zip_reader_locate_file(
      reader.archive(), entry_path.c_str(), nullptr,
      MZ_ZIP_FLAG_CASE_SENSITIVE);
  if (entry_index < 0) {
    return {MuonAppStorageStatus::kNotFound, "", {}};
  }

  mz_zip_archive_file_stat stat;
  if (!mz_zip_reader_file_stat(reader.archive(),
                               static_cast<mz_uint>(entry_index), &stat)) {
    return {MuonAppStorageStatus::kReadError, "", {}};
  }
  if (stat.m_is_directory) {
    return {MuonAppStorageStatus::kNotFound, "", {}};
  }
  if (stat.m_is_encrypted || !stat.m_is_supported ||
      stat.m_uncomp_size >
          static_cast<mz_uint64>(std::numeric_limits<size_t>::max())) {
    return {MuonAppStorageStatus::kReadError, "", {}};
  }

  std::vector<uint8_t> data(static_cast<size_t>(stat.m_uncomp_size));
  void* const target = data.empty() ? nullptr : data.data();
  if (!mz_zip_reader_extract_to_mem(reader.archive(),
                                    static_cast<mz_uint>(entry_index), target,
                                    data.size(), 0)) {
    return {MuonAppStorageStatus::kReadError, "", {}};
  }

  return {MuonAppStorageStatus::kOk,
          GetMimeTypeForPath(std::filesystem::path(entry_path)),
          std::move(data)};
}

class MuonFileAppStorage final : public MuonAppStorage {
 public:
  explicit MuonFileAppStorage(std::filesystem::path asset_root)
      : asset_root_(std::move(asset_root)) {}

  MuonAppStorageResource ReadResource(
      const MuonAppStorageRequest& request) override {
    std::vector<std::string> segments;
    if (!GetValidAppRequestSegments(request, &segments)) {
      return {MuonAppStorageStatus::kRejected, "", {}};
    }

    auto candidate = asset_root_ / request.host;
    for (const auto& segment : segments) {
      candidate /= segment;
    }

    std::error_code error;
    if (!std::filesystem::exists(candidate, error) ||
        !std::filesystem::is_regular_file(candidate, error)) {
      return {MuonAppStorageStatus::kNotFound, "", {}};
    }

    const auto canonical_asset_root =
        std::filesystem::canonical(asset_root_ / request.host, error);
    if (error) {
      return {MuonAppStorageStatus::kNotFound, "", {}};
    }
    const auto canonical_candidate =
        std::filesystem::canonical(candidate, error);
    if (error ||
        !IsPathInsideDirectory(canonical_candidate, canonical_asset_root)) {
      return {MuonAppStorageStatus::kRejected, "", {}};
    }

    std::ifstream input(canonical_candidate, std::ios::binary);
    if (!input) {
      return {MuonAppStorageStatus::kReadError, "", {}};
    }
    std::vector<uint8_t> data(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    if (!input.eof() && input.fail()) {
      return {MuonAppStorageStatus::kReadError, "", {}};
    }

    return {MuonAppStorageStatus::kOk, GetMimeTypeForPath(canonical_candidate),
            std::move(data)};
  }

 private:
  std::filesystem::path asset_root_;
};

class MuonZipAppStorage final : public MuonAppStorage {
 public:
  explicit MuonZipAppStorage(std::filesystem::path archive_path)
      : archive_path_(std::move(archive_path)) {}

  MuonAppStorageResource ReadResource(
      const MuonAppStorageRequest& request) override {
    std::vector<std::string> segments;
    if (!GetValidAppRequestSegments(request, &segments)) {
      return {MuonAppStorageStatus::kRejected, "", {}};
    }

    return ExtractZipEntrySync(archive_path_,
                               JoinZipEntryPath(request.host, segments));
  }

 private:
  std::filesystem::path archive_path_;
};

std::shared_ptr<MuonAppStorage> CreateMuonFileAppStorage(
    std::filesystem::path asset_root) {
  return std::make_shared<MuonFileAppStorage>(std::move(asset_root));
}

std::shared_ptr<MuonAppStorage> CreateMuonZipAppStorage(
    std::filesystem::path archive_path) {
  return std::make_shared<MuonZipAppStorage>(std::move(archive_path));
}

std::shared_ptr<MuonAppStorage> CreateConfiguredMuonAppStorage(
    bool has_asset_from,
    const std::filesystem::path& asset_from,
    bool has_asset_signature,
    const std::string& asset_signature,
    bool has_asset_salt,
    const std::vector<uint8_t>& asset_salt,
    std::string* error_message) {
  if (error_message == nullptr) {
    return nullptr;
  }
  error_message->clear();
  if (has_asset_signature && asset_signature.size() != 64) {
    *error_message =
        "muon.json asset.signature must be a 64-character SHA-256 hex string";
    return nullptr;
  }
  auto normalized_asset_signature = asset_signature;
  if (has_asset_signature) {
    for (const auto character : asset_signature) {
      if (!IsHexDigit(character)) {
        *error_message =
            "muon.json asset.signature must be a 64-character SHA-256 hex "
            "string";
        return nullptr;
      }
    }
    for (auto& character : normalized_asset_signature) {
      character = ToLowerHexDigit(character);
    }
  }
  if (has_asset_signature && !has_asset_salt) {
    *error_message = "muon.json asset.signature requires asset.salt";
    return nullptr;
  }
  if (!has_asset_from) {
    if (has_asset_signature) {
      *error_message =
          "muon.json asset.signature requires asset.sourcePath to be a ZIP "
          "file";
      return nullptr;
    }
    return CreateDefaultMuonFileAppStorage();
  }

  std::error_code error;
  if (!std::filesystem::exists(asset_from, error) || error) {
    *error_message = "muon.json asset.sourcePath does not exist: " +
                     asset_from.string();
    return nullptr;
  }
  if (std::filesystem::is_directory(asset_from, error) && !error) {
    if (has_asset_signature) {
      *error_message =
          "muon.json asset.signature requires asset.sourcePath to be a ZIP "
          "file: " +
          asset_from.string();
      return nullptr;
    }
    return CreateMuonFileAppStorage(asset_from);
  }
  error.clear();
  if (std::filesystem::is_regular_file(asset_from, error) && !error) {
    if (has_asset_signature) {
      std::string actual_signature;
      if (!CalculateFileSha256Hex(asset_from, asset_salt,
                                  &actual_signature)) {
        *error_message = "Failed to read muon.json asset.sourcePath: " +
                         asset_from.string();
        return nullptr;
      }
      std::vector<uint8_t> archive_data;
      if (!ReadBinaryFile(asset_from, &archive_data)) {
        *error_message = "Failed to read muon.json asset.sourcePath: " +
                         asset_from.string();
        return nullptr;
      }
      if (actual_signature != normalized_asset_signature) {
        *error_message =
            "muon.json asset.signature does not match asset.sourcePath salted "
            "SHA-256: " +
            asset_from.string();
        return nullptr;
      }
      if (!IsZipArchiveData(archive_data)) {
        *error_message =
            "muon.json asset.signature requires asset.sourcePath to be a "
            "valid ZIP file: " +
            asset_from.string();
        return nullptr;
      }
    }
    return CreateMuonZipAppStorage(asset_from);
  }

  *error_message =
      "muon.json asset.sourcePath must be a directory or regular file: " +
      asset_from.string();
  return nullptr;
}

std::shared_ptr<MuonAppStorage> CreateDefaultMuonFileAppStorage() {
  return CreateMuonFileAppStorage(GetMuonExecutableDirectory() / "assets");
}
