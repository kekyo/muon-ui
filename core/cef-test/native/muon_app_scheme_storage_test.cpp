/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "app/muon_app_scheme.h"
#include "app/muon_app_storage.h"

#include "include/internal/cef_types.h"

#include "miniz.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

static bool Expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << "\n";
    return false;
  }
  return true;
}

static std::filesystem::path CreateTestDirectory() {
  std::error_code error;
  const auto base = std::filesystem::temp_directory_path(error);
  if (error) {
    return {};
  }
  const auto unique =
      std::to_string(std::chrono::steady_clock::now()
                         .time_since_epoch()
                         .count());
  const auto directory = base / ("muon-app-storage-" + unique);
  if (!std::filesystem::create_directories(directory, error) || error) {
    return {};
  }
  return directory;
}

static bool WriteFile(const std::filesystem::path& path,
                      const std::string& content) {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) {
    return false;
  }

  std::ofstream output(path, std::ios::binary);
  if (!output) {
    return false;
  }
  output << content;
  return static_cast<bool>(output);
}

static void AppendU16(std::vector<uint8_t>* data, uint16_t value) {
  data->push_back(static_cast<uint8_t>(value & 0xff));
  data->push_back(static_cast<uint8_t>((value >> 8) & 0xff));
}

static void AppendU32(std::vector<uint8_t>* data, uint32_t value) {
  AppendU16(data, static_cast<uint16_t>(value & 0xffff));
  AppendU16(data, static_cast<uint16_t>((value >> 16) & 0xffff));
}

static bool WriteBinaryFile(const std::filesystem::path& path,
                            const std::vector<uint8_t>& data) {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) {
    return false;
  }

  std::ofstream output(path, std::ios::binary);
  if (!output) {
    return false;
  }
  output.write(reinterpret_cast<const char*>(data.data()),
               static_cast<std::streamsize>(data.size()));
  return static_cast<bool>(output);
}

static bool WriteDeterministicStoredZipArchive(
    const std::filesystem::path& path) {
  const auto name = std::string{"main/index.html"};
  const auto content = std::string{"<title>signed zip</title>"};
  const auto crc =
      static_cast<uint32_t>(mz_crc32(MZ_CRC32_INIT,
                                     reinterpret_cast<const unsigned char*>(
                                         content.data()),
                                     content.size()));

  std::vector<uint8_t> data;
  AppendU32(&data, 0x04034b50);
  AppendU16(&data, 20);
  AppendU16(&data, 0);
  AppendU16(&data, 0);
  AppendU16(&data, 0);
  AppendU16(&data, 0);
  AppendU32(&data, crc);
  AppendU32(&data, static_cast<uint32_t>(content.size()));
  AppendU32(&data, static_cast<uint32_t>(content.size()));
  AppendU16(&data, static_cast<uint16_t>(name.size()));
  AppendU16(&data, 0);
  data.insert(data.end(), name.begin(), name.end());
  data.insert(data.end(), content.begin(), content.end());

  const auto central_directory_offset = data.size();
  AppendU32(&data, 0x02014b50);
  AppendU16(&data, 20);
  AppendU16(&data, 20);
  AppendU16(&data, 0);
  AppendU16(&data, 0);
  AppendU16(&data, 0);
  AppendU16(&data, 0);
  AppendU32(&data, crc);
  AppendU32(&data, static_cast<uint32_t>(content.size()));
  AppendU32(&data, static_cast<uint32_t>(content.size()));
  AppendU16(&data, static_cast<uint16_t>(name.size()));
  AppendU16(&data, 0);
  AppendU16(&data, 0);
  AppendU16(&data, 0);
  AppendU16(&data, 0);
  AppendU32(&data, 0);
  AppendU32(&data, 0);
  data.insert(data.end(), name.begin(), name.end());

  const auto central_directory_size =
      data.size() - central_directory_offset;
  AppendU32(&data, 0x06054b50);
  AppendU16(&data, 0);
  AppendU16(&data, 0);
  AppendU16(&data, 1);
  AppendU16(&data, 1);
  AppendU32(&data, static_cast<uint32_t>(central_directory_size));
  AppendU32(&data, static_cast<uint32_t>(central_directory_offset));
  AppendU16(&data, 0);
  return WriteBinaryFile(path, data);
}

static bool WriteZipArchive(const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) {
    return false;
  }

  auto archive = mz_zip_archive{};
  mz_zip_zero_struct(&archive);
  const auto path_string = path.string();
  if (!mz_zip_writer_init_file(&archive, path_string.c_str(), 0)) {
    return false;
  }

  auto succeeded = true;
  const auto index_content = std::string{"<title>muon zip</title>"};
  const auto plain_content = std::string{"stored asset"};
  succeeded =
      succeeded &&
      mz_zip_writer_add_mem(&archive, "main/index.html",
                            index_content.data(), index_content.size(),
                            6);
  succeeded =
      succeeded &&
      mz_zip_writer_add_mem(&archive, "main/plain.txt",
                            plain_content.data(), plain_content.size(),
                            0);
  succeeded = succeeded && mz_zip_writer_finalize_archive(&archive);
  succeeded = mz_zip_writer_end(&archive) && succeeded;
  return succeeded;
}

static bool RunSchemeOptionsTest() {
  const auto options = GetMuonAppSchemeOptions();
  return Expect((options & CEF_SCHEME_OPTION_STANDARD) != 0,
                "asset scheme is not standard") &&
         Expect((options & CEF_SCHEME_OPTION_SECURE) != 0,
                "asset scheme is not secure") &&
         Expect((options & CEF_SCHEME_OPTION_CORS_ENABLED) != 0,
                "asset scheme is not CORS-enabled") &&
         Expect((options & CEF_SCHEME_OPTION_FETCH_ENABLED) != 0,
                "asset scheme is not Fetch-enabled") &&
         Expect((options & CEF_SCHEME_OPTION_DISPLAY_ISOLATED) != 0,
                "asset scheme is not display-isolated") &&
         Expect((options & CEF_SCHEME_OPTION_LOCAL) == 0,
                "asset scheme must not be local") &&
         Expect((options & CEF_SCHEME_OPTION_CSP_BYPASSING) == 0,
                "asset scheme must not bypass CSP");
}

static bool RunConfiguredStorageDirectoryTest(
    const std::filesystem::path& test_directory) {
  const auto asset_root = test_directory / "configured-assets";
  const auto index_path = asset_root / "main" / "index.html";
  if (!Expect(WriteFile(index_path, "<title>configured</title>"),
              "failed to create configured app asset")) {
    return false;
  }

  std::string error_message;
  const auto storage = CreateConfiguredMuonAppStorage(
      true, asset_root, false, "", false, {}, &error_message);
  if (!Expect(static_cast<bool>(storage), error_message)) {
    return false;
  }

  const auto resource = storage->ReadResource({"main", "/index.html"});
  const std::string body(resource.data.begin(), resource.data.end());
  return Expect(resource.status == MuonAppStorageStatus::kOk,
                "configured directory asset was not served") &&
         Expect(body == "<title>configured</title>",
                "configured directory asset content changed");
}

static bool RunConfiguredStorageValidationTest(
    const std::filesystem::path& test_directory) {
  std::string error_message;
  const auto missing = CreateConfiguredMuonAppStorage(
      true, test_directory / "missing-assets", false, "", false, {},
      &error_message);
  if (!Expect(!missing, "missing asset.sourcePath path was accepted") ||
      !Expect(error_message.find("asset.sourcePath") != std::string::npos,
              "missing asset.sourcePath error message lacks context")) {
    return false;
  }

  const auto default_storage =
      CreateConfiguredMuonAppStorage(false, {}, false, "", false, {},
                                     &error_message);
  return Expect(static_cast<bool>(default_storage),
                "default asset storage was rejected");
}

static bool RunConfiguredStorageSignatureTest(
    const std::filesystem::path& test_directory) {
  const auto archive_path = test_directory / "signed-assets.zip";
  if (!Expect(WriteDeterministicStoredZipArchive(archive_path),
              "failed to create signed ZIP asset")) {
    return false;
  }

  std::string error_message;
  const std::vector<uint8_t> salt = {0xde, 0xad, 0xbe, 0xef};
  const auto storage = CreateConfiguredMuonAppStorage(
      true, archive_path, true,
      "d243085d80934d981fbb4cc8633a2494270728b06c800cc9ec0d53bf88081a35",
      true, salt,
      &error_message);
  if (!Expect(static_cast<bool>(storage), error_message)) {
    return false;
  }

  const auto empty_salt_storage = CreateConfiguredMuonAppStorage(
      true, archive_path, true,
      "8153533b173d9190c4d874ee94e35c4b1255309265b632ad41a4091977012c6b",
      true, {},
      &error_message);
  if (!Expect(static_cast<bool>(empty_salt_storage),
              "explicit empty asset.salt was rejected")) {
    return false;
  }

  const auto resource = storage->ReadResource({"main", "/index.html"});
  const std::string body(resource.data.begin(), resource.data.end());
  if (!Expect(resource.status == MuonAppStorageStatus::kOk,
              "signed ZIP asset was not served") ||
      !Expect(body == "<title>signed zip</title>",
              "signed ZIP asset content changed")) {
    return false;
  }

  const auto mismatch = CreateConfiguredMuonAppStorage(
      true, archive_path, true,
      "0243085d80934d981fbb4cc8633a2494270728b06c800cc9ec0d53bf88081a35",
      true, salt,
      &error_message);
  if (!Expect(!mismatch, "mismatched asset.signature was accepted") ||
      !Expect(error_message.find("asset.signature") != std::string::npos,
              "mismatched asset.signature error lacks context")) {
    return false;
  }

  const auto without_salt = CreateConfiguredMuonAppStorage(
      true, archive_path, true,
      "d243085d80934d981fbb4cc8633a2494270728b06c800cc9ec0d53bf88081a35",
      false, {},
      &error_message);
  if (!Expect(!without_salt,
              "asset.signature without asset.salt was accepted") ||
      !Expect(error_message.find("asset.salt") != std::string::npos,
              "missing asset.salt error lacks context")) {
    return false;
  }

  const auto without_from = CreateConfiguredMuonAppStorage(
      false, {}, true,
      "d243085d80934d981fbb4cc8633a2494270728b06c800cc9ec0d53bf88081a35",
      true, salt, &error_message);
  if (!Expect(!without_from,
              "asset.signature without asset.sourcePath was accepted")) {
    return false;
  }

  const auto asset_root = test_directory / "signature-directory";
  if (!Expect(WriteFile(asset_root / "main" / "index.html", "directory"),
              "failed to create directory asset")) {
    return false;
  }
  const auto directory = CreateConfiguredMuonAppStorage(
      true, asset_root, true,
      "d243085d80934d981fbb4cc8633a2494270728b06c800cc9ec0d53bf88081a35",
      true, salt,
      &error_message);
  if (!Expect(!directory,
              "asset.signature with directory asset.sourcePath was accepted")) {
    return false;
  }

  const auto broken_path = test_directory / "signed-broken.zip";
  if (!Expect(WriteFile(broken_path, "not a zip"),
              "failed to create signed broken ZIP asset")) {
    return false;
  }
  const auto broken = CreateConfiguredMuonAppStorage(
      true, broken_path, true,
      "005d2a3368995baf1694f06110947374d3dea2d4bc72b6c676f83073cf229584",
      true, salt,
      &error_message);
  return Expect(!broken, "signed non-ZIP asset.sourcePath was accepted") &&
         Expect(error_message.find("ZIP") != std::string::npos,
                "signed non-ZIP error did not mention ZIP");
}

static bool ExpectConfiguredStorageSignatureFormatFailure(
    const std::filesystem::path& archive_path,
    const std::string& signature,
    const std::vector<uint8_t>& salt) {
  std::string error_message;
  const auto storage = CreateConfiguredMuonAppStorage(
      true, archive_path, true, signature, true, salt, &error_message);
  return Expect(!storage, "invalid asset.signature format was accepted") &&
         Expect(error_message.find(
                    "64-character SHA-256 hex string") != std::string::npos,
                "invalid asset.signature format error lacks context");
}

static bool RunConfiguredStorageSignatureFormatTest(
    const std::filesystem::path& test_directory) {
  const auto archive_path = test_directory / "signature-format-assets.zip";
  if (!Expect(WriteDeterministicStoredZipArchive(archive_path),
              "failed to create signature format ZIP asset")) {
    return false;
  }

  const std::vector<uint8_t> salt = {0xde, 0xad, 0xbe, 0xef};
  return ExpectConfiguredStorageSignatureFormatFailure(
             archive_path, std::string(63, 'a'), salt) &&
         ExpectConfiguredStorageSignatureFormatFailure(
             archive_path, std::string(65, 'a'), salt) &&
         ExpectConfiguredStorageSignatureFormatFailure(
             archive_path, std::string(63, 'a') + "x", salt);
}

static bool RunZipStorageReadTest(
    const std::filesystem::path& test_directory) {
  const auto archive_path = test_directory / "assets.zip";
  if (!Expect(WriteZipArchive(archive_path), "failed to create ZIP asset")) {
    return false;
  }

  const auto storage = CreateMuonZipAppStorage(archive_path);
  const auto index = storage->ReadResource({"main", "/index.html"});
  const auto plain = storage->ReadResource({"main", "/plain.txt"});
  const std::string index_body(index.data.begin(), index.data.end());
  const std::string plain_body(plain.data.begin(), plain.data.end());
  return Expect(index.status == MuonAppStorageStatus::kOk,
                "ZIP index.html was not served") &&
         Expect(index.mime_type == "text/html",
                "ZIP index.html MIME type is not text/html") &&
         Expect(index_body == "<title>muon zip</title>",
                "ZIP index.html content changed while reading") &&
         Expect(plain.status == MuonAppStorageStatus::kOk,
                "stored ZIP asset was not served") &&
         Expect(plain.mime_type == "text/plain",
                "stored ZIP asset MIME type is not text/plain") &&
         Expect(plain_body == "stored asset",
                "stored ZIP asset content changed while reading");
}

static bool RunZipStorageRejectionTest(
    const std::filesystem::path& test_directory) {
  const auto archive_path = test_directory / "zip-rejection" / "assets.zip";
  if (!Expect(WriteZipArchive(archive_path), "failed to create ZIP asset")) {
    return false;
  }

  const auto storage = CreateMuonZipAppStorage(archive_path);
  const auto missing = storage->ReadResource({"main", "/missing.html"});
  const auto wrong_host = storage->ReadResource({"other", "/index.html"});
  const auto traversal = storage->ReadResource({"main", "/../secret.txt"});
  const auto encoded_traversal =
      storage->ReadResource({"main", "/%2e%2e/secret.txt"});
  const auto encoded_separator =
      storage->ReadResource({"main", "/safe%2fsecret.txt"});

  return Expect(missing.status == MuonAppStorageStatus::kNotFound,
                "missing ZIP asset was not reported as not found") &&
         Expect(wrong_host.status == MuonAppStorageStatus::kRejected,
                "wrong ZIP app host was not rejected") &&
         Expect(traversal.status == MuonAppStorageStatus::kRejected,
                "ZIP path traversal was not rejected") &&
         Expect(encoded_traversal.status == MuonAppStorageStatus::kRejected,
                "ZIP percent-encoded traversal was not rejected") &&
         Expect(encoded_separator.status == MuonAppStorageStatus::kRejected,
                "ZIP percent-encoded path separator was not rejected");
}

static bool RunZipStorageReadErrorTest(
    const std::filesystem::path& test_directory) {
  const auto archive_path = test_directory / "broken.zip";
  if (!Expect(WriteFile(archive_path, "not a zip"),
              "failed to create broken ZIP asset")) {
    return false;
  }

  const auto storage = CreateMuonZipAppStorage(archive_path);
  const auto resource = storage->ReadResource({"main", "/index.html"});
  return Expect(resource.status == MuonAppStorageStatus::kReadError,
                "broken ZIP was not reported as read error");
}

static bool RunStorageReadTest(const std::filesystem::path& test_directory) {
  const auto asset_root = test_directory / "assets";
  const auto index_path = asset_root / "main" / "index.html";
  if (!Expect(WriteFile(index_path, "<title>muon</title>"),
              "failed to create app asset")) {
    return false;
  }

  const auto storage = CreateMuonFileAppStorage(asset_root);
  const auto resource = storage->ReadResource({"main", "/index.html"});
  const std::string body(resource.data.begin(), resource.data.end());
  return Expect(resource.status == MuonAppStorageStatus::kOk,
                "index.html was not served") &&
         Expect(resource.mime_type == "text/html",
                "index.html MIME type is not text/html") &&
         Expect(body == "<title>muon</title>",
                "index.html content changed while reading");
}

static bool RunStorageRejectionTest(
    const std::filesystem::path& test_directory) {
  const auto asset_root = test_directory / "assets";
  const auto storage = CreateMuonFileAppStorage(asset_root);

  const auto missing = storage->ReadResource({"main", "/missing.html"});
  const auto wrong_host = storage->ReadResource({"other", "/index.html"});
  const auto traversal = storage->ReadResource({"main", "/../secret.txt"});
  const auto encoded_traversal =
      storage->ReadResource({"main", "/%2e%2e/secret.txt"});
  const auto encoded_separator =
      storage->ReadResource({"main", "/safe%2fsecret.txt"});

  return Expect(missing.status == MuonAppStorageStatus::kNotFound,
                "missing asset was not reported as not found") &&
         Expect(wrong_host.status == MuonAppStorageStatus::kRejected,
                "wrong app host was not rejected") &&
         Expect(traversal.status == MuonAppStorageStatus::kRejected,
                "path traversal was not rejected") &&
         Expect(encoded_traversal.status == MuonAppStorageStatus::kRejected,
                "percent-encoded traversal was not rejected") &&
         Expect(encoded_separator.status == MuonAppStorageStatus::kRejected,
                "percent-encoded path separator was not rejected");
}

static bool RunStorageHttpStatusTest() {
  return Expect(GetMuonAppStorageHttpStatus(MuonAppStorageStatus::kOk) == 200,
                "served asset did not map to HTTP 200") &&
         Expect(GetMuonAppStorageHttpStatus(MuonAppStorageStatus::kRejected) ==
                    403,
                "rejected asset did not map to HTTP 403") &&
         Expect(GetMuonAppStorageHttpStatus(MuonAppStorageStatus::kNotFound) ==
                    404,
                "missing asset did not map to HTTP 404") &&
         Expect(GetMuonAppStorageHttpStatus(MuonAppStorageStatus::kReadError) ==
                    500,
                "read error did not map to HTTP 500");
}

static bool RunSymlinkEscapeTest(const std::filesystem::path& test_directory) {
  const auto asset_root = test_directory / "assets";
  const auto outside_path = test_directory / "secret.html";
  const auto link_path = asset_root / "main" / "link.html";
  if (!Expect(WriteFile(outside_path, "secret"), "failed to create secret")) {
    return false;
  }

  std::error_code error;
  std::filesystem::create_directories(link_path.parent_path(), error);
  if (error) {
    return Expect(false, "failed to create symlink parent directory");
  }
  std::filesystem::create_symlink(outside_path, link_path, error);
  if (error) {
    return Expect(false, "failed to create symlink");
  }

  const auto storage = CreateMuonFileAppStorage(asset_root);
  const auto resource = storage->ReadResource({"main", "/link.html"});
  return Expect(resource.status == MuonAppStorageStatus::kRejected,
                "symlink escape was not rejected");
}

int main() {
  const auto test_directory = CreateTestDirectory();
  if (!Expect(!test_directory.empty(), "failed to create test directory")) {
    return 1;
  }

  const auto passed = RunSchemeOptionsTest() &&
                      RunStorageReadTest(test_directory) &&
                      RunConfiguredStorageDirectoryTest(test_directory) &&
                      RunConfiguredStorageValidationTest(test_directory) &&
                      RunConfiguredStorageSignatureTest(test_directory) &&
                      RunConfiguredStorageSignatureFormatTest(test_directory) &&
                      RunZipStorageReadTest(test_directory) &&
                      RunZipStorageRejectionTest(test_directory) &&
                      RunZipStorageReadErrorTest(test_directory) &&
                      RunStorageRejectionTest(test_directory) &&
                      RunStorageHttpStatusTest() &&
                      RunSymlinkEscapeTest(test_directory);

  std::error_code error;
  std::filesystem::remove_all(test_directory, error);
  return passed ? 0 : 1;
}
