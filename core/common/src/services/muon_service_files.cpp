/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 */
#include "services/muon_service_files.h"
#include "services/muon_service_interface.h"
#include <yyjson.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <random>
#include <set>
#ifdef _WIN32
#include <windows.h>
#include "muon_filesystem.h"
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static constexpr size_t limit = 1024 * 1024;
static void Denied() { throw MuonServiceIoError("permission-denied", "Service file access denied"); }
static void Check(const std::atomic_bool& cancelled) {
  if (cancelled.load()) throw MuonServiceIoError("cancelled", "Service file I/O cancelled");
}
static std::string Text(yyjson_val* value) {
  if (!yyjson_is_str(value)) throw MuonServiceIoError("invalid-input", "Expected a string");
  return {yyjson_get_str(value), yyjson_get_len(value)};
}
static std::string Quote(const std::string& value) {
  return EncodeMuonServiceValue({MUON_SERVICE_VALUE_STRING, {}, {}}, {value});
}
static auto Document(const std::string& json) {
  auto doc = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>(yyjson_read(json.data(), json.size(), 0), yyjson_doc_free);
  if (!doc || !yyjson_is_obj(yyjson_doc_get_root(doc.get()))) throw MuonServiceIoError("invalid-input", "Invalid service I/O metadata");
  return doc;
}
static std::vector<std::string> Parts(const std::string& path, bool allow_root) {
  if (path.empty() && allow_root) return {};
  if (path.size() > 4096) Denied();
  std::vector<std::string> parts;
  size_t begin = 0;
  for (;;) {
    const auto end = path.find('/', begin);
    auto name = path.substr(begin, end == std::string::npos ? end : end - begin);
    if (name.empty() || name.back() == '.' || name.back() == ' ' || name.starts_with(".muon-tmp-") ||
        name.find_first_of("\\:\"<>|?*") != std::string::npos ||
        std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 32; })) Denied();
    auto stem = name.substr(0, name.find('.'));
    for (auto& ch : stem) if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" ||
        (stem.size() == 4 && (stem.starts_with("COM") || stem.starts_with("LPT")) && stem[3] >= '1' && stem[3] <= '9')) Denied();
    parts.push_back(std::move(name));
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  return parts;
}
static MuonServiceIoReply Listing(const std::set<std::string>& names) {
  MuonServiceIoReply result;
  result.metadata = "[";
  for (const auto& name : names) {
    if (name.starts_with(".muon-tmp-")) continue;
    if (result.metadata.size() > 1) result.metadata += ',';
    result.metadata += Quote(name);
    if (result.metadata.size() > limit) throw MuonServiceIoError("size-limit", "Service directory exceeds 1 MiB");
  }
  result.metadata += ']';
  return result;
}

static MuonServiceIoReply ReadResult(std::vector<uint8_t> bytes, bool text) {
  if (bytes.size() > limit) throw MuonServiceIoError("size-limit", "Service file exceeds 1 MiB");
  MuonServiceIoReply result;
  if (text) result.metadata = Quote(std::string(bytes.begin(), bytes.end()));
  else result.body = std::move(bytes);
  return result;
}

static MuonServiceIoReply Asset(yyjson_val* assets, const std::string& path, const std::string& operation) {
  if (!yyjson_is_obj(assets)) Denied();
  if (operation == "fs.list") {
    const auto prefix = path.empty() ? "" : path + "/";
    std::set<std::string> names;
    auto iter = yyjson_obj_iter_with(assets);
    while (auto* key = yyjson_obj_iter_next(&iter)) {
      const auto name = Text(key);
      if (name.starts_with(prefix)) names.insert(name.substr(prefix.size(), name.find('/', prefix.size()) - prefix.size()));
    }
    if (names.empty() && !path.empty()) throw MuonServiceIoError("not-found", "Unknown asset directory");
    return Listing(names);
  }
  if (operation != "fs.read" && operation != "fs.readText") Denied();
  const auto source = yyjson_obj_getn(assets, path.data(), path.size());
  if (!source) throw MuonServiceIoError("not-found", "Unknown service asset");
  const auto encoded = std::unique_ptr<char, decltype(&std::free)>(yyjson_val_write(source, 0, nullptr), &std::free);
  if (!encoded) throw std::bad_alloc();
  auto value = DecodeMuonServiceValue({MUON_SERVICE_VALUE_BYTES, {}, {}}, encoded.get());
  return ReadResult(std::move(std::get<MuonServiceValue::Bytes>(value.data)), operation == "fs.readText");
}

#ifdef _WIN32
using File = HANDLE;
static constexpr auto fileFlags = FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS;
static void OsError() {
  const auto error = GetLastError();
  if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) throw MuonServiceIoError("not-found", "Unknown service file");
  if (error == ERROR_ACCESS_DENIED || error == ERROR_CANT_ACCESS_FILE) Denied();
  throw std::system_error(static_cast<int>(error), std::system_category(), "Service file operation");
}
static void CloseFile(File value) { CloseHandle(value); }
static std::filesystem::path Native(const std::string& value) {
  return std::filesystem::path(std::u8string(value.begin(), value.end()));
}
struct Directory {
  std::vector<HANDLE> held;
  std::filesystem::path path;
  ~Directory() { for (const auto handle : held) CloseHandle(handle); }
  void Pin() {
    const auto handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, fileFlags, nullptr);
    if (handle == INVALID_HANDLE_VALUE) OsError();
    held.push_back(handle);
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info)) OsError();
    if ((info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != FILE_ATTRIBUTE_DIRECTORY) Denied();
  }
  void Open(const std::string& root, const std::vector<std::string>& parts, size_t count) {
    path = muon_internal::FilesystemPathForIo(Native(root), muon_internal::FilesystemIoUsage::File);
    Pin();
    for (size_t i = 0; i < count; ++i) { path /= Native(parts[i]); Pin(); }
  }
  void CheckEntry(const std::string& name, bool missing) const {
    const auto target = path / Native(name);
    const auto attr = GetFileAttributesW(target.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
      if (missing && GetLastError() == ERROR_FILE_NOT_FOUND) return;
      OsError();
    }
    if (attr & FILE_ATTRIBUTE_REPARSE_POINT) Denied();
  }
  File Read(const std::string& name) const {
    const auto file = CreateFileW((path / Native(name)).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) OsError();
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file, &info) || GetFileType(file) != FILE_TYPE_DISK ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) || info.nNumberOfLinks != 1) {
      CloseFile(file); Denied();
    }
    return file;
  }
  File Temporary(const std::string& name) const {
    auto file = CreateFileW((path / Native(name)).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) OsError();
    return file;
  }
  void Remove(const std::string& name) const { DeleteFileW((path / Native(name)).c_str()); }
  void Move(const std::string& name, const Directory& target, const std::string& to) const {
    if (!MoveFileExW((path / Native(name)).c_str(), (target.path / Native(to)).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) OsError();
  }
  void Make(const std::string& name) const { if (!CreateDirectoryW((path / Native(name)).c_str(), nullptr)) OsError(); }
  std::set<std::string> List() const {
    std::set<std::string> result;
    for (const auto& item : std::filesystem::directory_iterator(path)) {
      const auto name = item.path().filename().u8string();
      result.emplace(name.begin(), name.end());
    }
    return result;
  }
};
static size_t ReadChunk(File file, uint8_t* bytes, size_t size) {
  DWORD read = 0;
  if (!ReadFile(file, bytes, static_cast<DWORD>(size), &read, nullptr)) OsError();
  return read;
}
static size_t WriteChunk(File file, const uint8_t* bytes, size_t size) {
  DWORD written = 0;
  if (!WriteFile(file, bytes, static_cast<DWORD>(size), &written, nullptr)) OsError();
  return written;
}
static void Flush(File file) { if (!FlushFileBuffers(file)) OsError(); }
#else
using File = int;
static void OsError() {
  if (errno == ENOENT) throw MuonServiceIoError("not-found", "Unknown service file");
  if (errno == ELOOP || errno == EACCES || errno == EPERM || errno == ENOTDIR) Denied();
  throw std::system_error(errno, std::generic_category(), "Service file operation");
}
static void CloseFile(File value) { close(value); }
struct Directory {
  int fd = -1;
  ~Directory() { if (fd >= 0) close(fd); }
  void Open(const std::string& root, const std::vector<std::string>& parts, size_t count) {
    constexpr int flags = O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW;
    fd = open(root.c_str(), flags);
    if (fd < 0) OsError();
    for (size_t i = 0; i < count; ++i) {
      const auto next = openat(fd, parts[i].c_str(), flags);
      if (next < 0) OsError();
      close(fd); fd = next;
    }
  }
  void CheckEntry(const std::string& name, bool missing) const {
    struct stat info{};
    if (fstatat(fd, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
      if (missing && errno == ENOENT) return;
      OsError();
    }
    if (!S_ISDIR(info.st_mode) && (!S_ISREG(info.st_mode) || info.st_nlink != 1)) Denied();
  }
  File Read(const std::string& name) const {
    const auto file = openat(fd, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (file < 0) OsError();
    struct stat info{};
    if (fstat(file, &info) != 0 || !S_ISREG(info.st_mode) || info.st_nlink != 1) { close(file); Denied(); }
    return file;
  }
  File Temporary(const std::string& name) const {
    const auto file = openat(fd, name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (file < 0) OsError();
    return file;
  }
  void Remove(const std::string& name) const { unlinkat(fd, name.c_str(), 0); }
  void Move(const std::string& name, const Directory& target, const std::string& to) const {
    if (renameat(fd, name.c_str(), target.fd, to.c_str()) != 0) OsError();
  }
  void Make(const std::string& name) const { if (mkdirat(fd, name.c_str(), 0700) != 0) OsError(); }
  std::set<std::string> List() const {
    auto copy = dup(fd);
    if (copy < 0) OsError();
    DIR* entries = fdopendir(copy);
    if (!entries) { close(copy); OsError(); }
    struct Owner { DIR* directory; ~Owner() { closedir(directory); } } owner{entries};
    std::set<std::string> names;
    for (;;) {
      errno = 0;
      auto* entry = readdir(entries);
      if (!entry) { if (errno) OsError(); break; }
      const std::string name(entry->d_name);
      if (name != "." && name != "..") names.insert(name);
    }
    return names;
  }
};
static size_t ReadChunk(File file, uint8_t* bytes, size_t size) {
  ssize_t count;
  do { count = read(file, bytes, size); } while (count < 0 && errno == EINTR);
  if (count < 0) OsError();
  return static_cast<size_t>(count);
}
static size_t WriteChunk(File file, const uint8_t* bytes, size_t size) {
  ssize_t count;
  do { count = write(file, bytes, size); } while (count < 0 && errno == EINTR);
  if (count < 0) OsError();
  return static_cast<size_t>(count);
}
static void Flush(File file) { if (fsync(file) != 0) OsError(); }
#endif

struct OwnedServiceFile {
  File file;
  ~OwnedServiceFile() { CloseFile(file); }
};

MuonServiceIoReply RunMuonServiceFile(const std::string& directory, const std::string& policy,
    const MuonServiceIoRequest& request, const std::atomic_bool& cancelled) {
  Check(cancelled);
  const auto grants = Document(policy);
  const auto arguments = Document(request.parameters);
  const auto options = yyjson_doc_get_root(arguments.get());
  const auto permissions = yyjson_obj_get(yyjson_doc_get_root(grants.get()), "fs");
  if (!yyjson_is_obj(permissions)) Denied();
  const auto area = Text(yyjson_obj_get(options, "area"));
  const auto path = Text(yyjson_obj_get(options, "path"));
  const auto& op = request.operation;
  const auto parts = Parts(path, op == "fs.list");
  if (area == "assets") return Asset(yyjson_obj_get(permissions, "assets"), path, op);
  auto root = directory;
  auto* grant = yyjson_obj_get(permissions, "data");
  if (area != "data") {
    const auto other = yyjson_obj_getn(yyjson_obj_get(permissions, "directories"), area.data(), area.size());
    if (!yyjson_is_obj(other)) Denied();
    root = Text(yyjson_obj_get(other, "path"));
    grant = yyjson_obj_get(other, "access");
  }
  const auto reading = op == "fs.read" || op == "fs.readText";
  if (!yyjson_equals_str(grant, "read-write") && !(yyjson_equals_str(grant, "read") && (reading || op == "fs.list"))) Denied();
  Directory parent;
  parent.Open(root, parts, op == "fs.list" ? parts.size() : parts.size() - 1);
  Check(cancelled);
  if (op == "fs.list") return Listing(parent.List());
  const auto& name = parts.back();
  parent.CheckEntry(name, !reading && op != "fs.rename");
  if (reading) {
    OwnedServiceFile file{parent.Read(name)};
    std::array<uint8_t, 16384> chunk;
    std::vector<uint8_t> bytes;
    for (;;) {
      Check(cancelled);
      const auto count = ReadChunk(file.file, chunk.data(), chunk.size());
      if (!count) break;
      if (count > limit - bytes.size()) throw MuonServiceIoError("size-limit", "Service file exceeds 1 MiB");
      bytes.insert(bytes.end(), chunk.data(), chunk.data() + count);
    }
    return ReadResult(std::move(bytes), op == "fs.readText");
  }
  if (op == "fs.mkdir") { parent.Make(name); return {}; }
  if (op == "fs.rename") {
    const auto target = Parts(Text(yyjson_obj_get(options, "destination")), false);
    Directory to;
    to.Open(root, target, target.size() - 1);
    to.CheckEntry(target.back(), true);
    Check(cancelled);
    parent.Move(name, to, target.back());
    return {};
  }
  if (op != "fs.write" && op != "fs.writeText") Denied();
  const auto text = op == "fs.writeText" ? Text(yyjson_obj_get(options, "value")) : std::string{};
  const auto size = op == "fs.writeText" ? text.size() : request.body.size();
  if (size > limit) throw MuonServiceIoError("size-limit", "Service file exceeds 1 MiB");
  const auto* bytes = op == "fs.writeText" ? reinterpret_cast<const uint8_t*>(text.data()) : request.body.data();
  std::random_device random;
  const auto temporary = ".muon-tmp-" + std::to_string(random()) + "-" + std::to_string(random());
  auto opened = parent.Temporary(temporary);
  try {
    {
      OwnedServiceFile file{opened};
      for (size_t offset = 0; offset < size;) {
        Check(cancelled);
        const auto count = WriteChunk(file.file, bytes + offset, std::min<size_t>(16384, size - offset));
        if (!count) throw MuonServiceIoError("io-error", "Short service file write");
        offset += count;
      }
      Flush(file.file);
    }
    Check(cancelled);
    parent.Move(temporary, parent, name);
  } catch (...) { parent.Remove(temporary); throw; }
  return {};
}
