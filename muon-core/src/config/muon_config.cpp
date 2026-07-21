/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "config/muon_config.h"

#include "config/muon_paths.h"
#include "config/muon_startup.h"
#include "muon_json_helpers.h"
#include "muon_string_helpers.h"

#include "yyjson.h"

#include <array>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

static constexpr char kMuonConfigJson5FileName[] = "muon.json5";
static constexpr char kMuonConfigJsoncFileName[] = "muon.jsonc";
static constexpr char kMuonConfigFileName[] = "muon.json";
static constexpr char kMuonConfigLauncherKey[] = "launcher";
static constexpr char kMuonConfigApplicationConfigKey[] = "config";
static constexpr char kMuonConfigAppIdKey[] = "appId";
static constexpr char kMuonConfigDefaultVersionPolicyKey[] =
    "defaultVersionPolicy";
static constexpr char kMuonConfigDesktopIdKey[] = "desktopId";
static constexpr const char* kMuonConfigSearchFileNames[] = {
    kMuonConfigJson5FileName,
    kMuonConfigJsoncFileName,
    kMuonConfigFileName,
};
static constexpr yyjson_read_flag kMuonConfigReadFlags = YYJSON_READ_JSON5;
static constexpr char kMuonConfigAssetKey[] = "asset";
static constexpr char kMuonConfigAssetSourcePathKey[] = "sourcePath";
static constexpr char kMuonConfigAssetSignatureKey[] = "signature";
static constexpr char kMuonConfigAssetSaltKey[] = "salt";
static constexpr char kMuonConfigBrowserKey[] = "browser";
static constexpr char kMuonConfigBrowserStartPageKey[] = "startPage";
static constexpr char kMuonConfigBrowserProfilePathKey[] = "profilePath";
static constexpr char kMuonConfigBrowserInitialWindowStateKey[] =
    "initialWindowState";
static constexpr char kMuonConfigBrowserInitialTitleBarVisibilityKey[] =
    "initialTitleBarVisibility";
static constexpr char kMuonConfigBrowserInitialTitleBarIconKey[] =
    "initialTitleBarIcon";
static constexpr char kMuonConfigBrowserBackgroundColorKey[] =
    "backgroundColor";
static constexpr char kMuonConfigBrowserTitleBarTypeKey[] = "titleBarType";
static constexpr char kMuonConfigBrowserContextMenuKey[] = "contextMenu";
static constexpr char kMuonConfigBrowserContextMenuModeKey[] = "mode";
static constexpr char kMuonConfigBrowserKeybindsKey[] = "keybind";
static constexpr char kMuonConfigBrowserDevToolsKey[] = "devtools";
static constexpr char kMuonConfigBrowserReloadKey[] = "reload";
static constexpr char kMuonConfigBrowserHardReloadKey[] = "hardReload";
static constexpr char kMuonConfigBrowserFullscreenKey[] = "fullscreen";
static constexpr char kMuonConfigBrowserZoomInKey[] = "zoomIn";
static constexpr char kMuonConfigBrowserZoomOutKey[] = "zoomOut";
static constexpr char kMuonConfigBrowserResetZoomKey[] = "resetZoom";
static constexpr char kMuonConfigBrowserRecycleKey[] = "recycle";
static constexpr char kMuonConfigBrowserPluginKey[] = "plugin";
static constexpr char kMuonConfigBrowserAllowUnsafeJavaScriptParentAccessKey[] =
    "allowUnsafeJavaScriptParentAccess";
static constexpr char kMuonConfigNetworkKey[] = "network";
static constexpr char kMuonConfigNetworkAllowKey[] = "allow";
static constexpr char kMuonConfigNetworkAuthorizedOriginKey[] =
    "authorizedOrigin";
static constexpr char kMuonConfigNetworkAuthorizedOriginSchemeKey[] = "scheme";
static constexpr char kMuonConfigNetworkAuthorizedOriginDomainKey[] = "domain";
static constexpr char kMuonConfigNetworkAuthorizedOriginPortKey[] = "port";
static constexpr char kMuonConfigNetworkLocalAccessKey[] = "localAccess";
static constexpr char kMuonConfigNetworkLoopbackOriginsKey[] =
    "loopbackOrigins";
static constexpr char kMuonConfigNetworkLocalNetworkOriginsKey[] =
    "localNetworkOrigins";
static constexpr char kMuonConfigPluginKey[] = "plugin";
static constexpr char kMuonConfigPluginPathKey[] = "path";
static constexpr char kMuonConfigPluginModeKey[] = "mode";
static constexpr char kMuonConfigPluginPagesKey[] = "pages";
static constexpr char kMuonConfigPluginCapabilitiesKey[] = "capabilities";
static constexpr char kMuonConfigPluginCapabilityIdKey[] = "id";
static constexpr char kMuonConfigPluginPluginsKey[] = "plugins";
static constexpr char kMuonConfigLegacyPluginsKey[] = "plugins";
static constexpr char kMuonConfigPluginEntryNameKey[] = "name";
static constexpr char kMuonConfigPluginEntrySignatureKey[] = "signature";
static constexpr char kMuonConfigPluginEntrySaltKey[] = "salt";
static constexpr char kMuonConfigPluginEntryAllowKey[] = "allow";
static constexpr char kMuonConfigPluginEntryImportsKey[] = "imports";
static constexpr char kMuonConfigPluginEntryConfigKey[] = "config";
static constexpr char kMuonInternalPluginName[] = "internal";
static constexpr char kMuonConfigLogKey[] = "log";
static constexpr char kMuonConfigLogLevelKey[] = "level";
static constexpr char kMuonConfigLogOutputKey[] = "output";
static constexpr char kMuonConfigLogOutputTypeKey[] = "type";
static constexpr char kMuonConfigLogOutputPathKey[] = "path";
static constexpr char kMuonConfigLogSourcesKey[] = "sources";
static constexpr char kMuonConfigLogSourceMuonKey[] = "muon";
static constexpr char kMuonConfigLogSourceCefKey[] = "cef";
static constexpr char kMuonConfigLogSourceConsoleKey[] = "console";
static constexpr char kMuonConfigLogSourcePluginKey[] = "plugin";
static constexpr char kMuonConfigCdpKey[] = "cdp";
static constexpr char kMuonConfigCdpEnableKey[] = "enable";
static constexpr char kMuonConfigCdpPortKey[] = "port";
static constexpr size_t kMuonEmbeddedConfigPayloadCapacity =
    kMuonEmbeddedConfigSlotSize;
static constexpr std::array<uint8_t, 32> kMuonEmbeddedConfigEmptySlotMarker = {
    0x6d, 0x75, 0x6f, 0x6e, 0x2d, 0x63, 0x6f, 0x72,
    0x65, 0x3a, 0x65, 0x6d, 0x62, 0x65, 0x64, 0x2d,
    0x63, 0x6f, 0x6e, 0x66, 0x69, 0x67, 0x3a, 0x73,
    0x6c, 0x6f, 0x74, 0x3a, 0x76, 0x31, 0x00, 0x5d};
static constexpr uint8_t kMuonEmbeddedTlvNullTag = 0;
static constexpr uint8_t kMuonEmbeddedTlvFalseTag = 1;
static constexpr uint8_t kMuonEmbeddedTlvTrueTag = 2;
static constexpr uint8_t kMuonEmbeddedTlvUintTag = 3;
static constexpr uint8_t kMuonEmbeddedTlvStringTag = 4;
static constexpr uint8_t kMuonEmbeddedTlvBinaryTag = 5;
static constexpr uint8_t kMuonEmbeddedTlvArrayTag = 6;
static constexpr uint8_t kMuonEmbeddedTlvObjectTag = 7;
static constexpr char kMuonDefaultProfileDirectoryName[] = "profile";
static constexpr char kMuonLauncherAppIdEnvironmentName[] =
    "MUON_LAUNCHER_APP_ID";
static constexpr char kMuonFallbackApplicationName[] = "muon";

using muon_internal::DecodeAsciiHexByte;
using muon_internal::IsAsciiHexDigit;
using muon_internal::MuonJsonDocument;
using muon_internal::MuonMutableJsonDocument;
using muon_internal::ReadJsonString;
using muon_internal::ToLowerAscii;
using muon_internal::TrimAscii;
static constexpr int kVirtualKeyBackspace = 0x08;
static constexpr int kVirtualKeyTab = 0x09;
static constexpr int kVirtualKeyEnter = 0x0D;
static constexpr int kVirtualKeyEscape = 0x1B;
static constexpr int kVirtualKeySpace = 0x20;
static constexpr int kVirtualKeyPageUp = 0x21;
static constexpr int kVirtualKeyPageDown = 0x22;
static constexpr int kVirtualKeyEnd = 0x23;
static constexpr int kVirtualKeyHome = 0x24;
static constexpr int kVirtualKeyLeft = 0x25;
static constexpr int kVirtualKeyUp = 0x26;
static constexpr int kVirtualKeyRight = 0x27;
static constexpr int kVirtualKeyDown = 0x28;
static constexpr int kVirtualKeyInsert = 0x2D;
static constexpr int kVirtualKeyDelete = 0x2E;
static constexpr int kVirtualKey0 = 0x30;
static constexpr int kVirtualKeyA = 0x41;
static constexpr int kVirtualKeyF1 = 0x70;
static constexpr int kVirtualKeyOemPlus = 0xBB;
static constexpr int kVirtualKeyOemMinus = 0xBD;

static constexpr uint8_t CalculateMuonEmbeddedConfigEmptySlotByte(
    size_t index) {
  if (index < kMuonEmbeddedConfigEmptySlotMarker.size()) {
    return kMuonEmbeddedConfigEmptySlotMarker[index];
  }
  const auto value =
      0xa5u ^
      (static_cast<uint32_t>(index) * 0x25u) ^
      ((static_cast<uint32_t>(index) >> 8) * 0x6du) ^
      ((static_cast<uint32_t>(index) >> 16) * 0x3bu) ^
      ((static_cast<uint32_t>(index) * 0x9e3779b1u) >> 24);
  return static_cast<uint8_t>(value & 0xffu);
}

static constexpr std::array<uint8_t, kMuonEmbeddedConfigSlotSize>
CreateEmptyMuonEmbeddedConfigSlotBytes() {
  std::array<uint8_t, kMuonEmbeddedConfigSlotSize> bytes{};
  for (auto index = size_t{0}; index < bytes.size(); ++index) {
    bytes[index] = CalculateMuonEmbeddedConfigEmptySlotByte(index);
  }
  return bytes;
}

#if defined(__GNUC__) || defined(__clang__)
#define MUON_EMBEDDED_CONFIG_USED __attribute__((used))
#else
#define MUON_EMBEDDED_CONFIG_USED
#endif

extern "C" {
MUON_EMBEDDED_CONFIG_USED alignas(16) const std::array<
    uint8_t,
    kMuonEmbeddedConfigSlotSize> kMuonEmbeddedConfigSlot =
    CreateEmptyMuonEmbeddedConfigSlotBytes();
}

struct MuonBrowserShortcutEntry {
  const char* key;
  MuonKeyboardShortcut* shortcut;
};

static bool ReadStringArray(yyjson_val* object,
                            const char* key,
                            const std::string& config_path,
                            std::vector<std::string>* target,
                            std::string* error_message);
static bool ReadAuthorizedOriginArray(yyjson_val* object,
                                      const char* key,
                                      const std::string& config_path,
                                      std::vector<MuonAuthorizedOriginConfig>*
                                          target,
                                      std::string* error_message);

struct MuonConfigPathResolution final {
  std::filesystem::path path;
  bool exists = false;
};

struct MuonConfigPathBases final {
  std::filesystem::path browser_profile;
  bool has_browser_profile = false;
  std::filesystem::path log_output_path;
  bool has_log_output_path = false;
  std::filesystem::path plugin_path;
  bool has_plugin_path = false;
  std::filesystem::path asset_from;
  bool has_asset_from = false;
};

struct MuonEmbeddedConfigSlotState final {
  bool embedded = false;
};

struct MuonEmbeddedTlvReader final {
  const uint8_t* bytes = nullptr;
  size_t size = 0;
  size_t offset = 0;
};

static bool InspectMuonConfigPath(const std::filesystem::path& path,
                                  bool* exists,
                                  std::string* error_message) {
  std::error_code error;
  *exists = std::filesystem::exists(path, error);
  if (error) {
    *error_message = "Failed to inspect muon config: " + path.string();
    return false;
  }
  return true;
}

static bool IsValidCefVersionPolicy(const std::string& value) {
  return value == "tested" || value == "same-major-latest" ||
         value == "compat-latest" || value == "exact";
}

static bool ResolveMuonConfigPath(const std::filesystem::path& path,
                                  MuonConfigPathResolution* resolution,
                                  std::string* error_message) {
  if (path.filename() != kMuonConfigFileName) {
    resolution->path = path;
    return InspectMuonConfigPath(path, &resolution->exists, error_message);
  }

  const auto directory = path.parent_path();
  for (const auto* file_name : kMuonConfigSearchFileNames) {
    const auto candidate = directory / file_name;
    auto exists = false;
    if (!InspectMuonConfigPath(candidate, &exists, error_message)) {
      return false;
    }
    if (exists) {
      resolution->path = candidate;
      resolution->exists = true;
      return true;
    }
  }

  resolution->path = directory / kMuonConfigJson5FileName;
  resolution->exists = false;
  return true;
}

static bool ReadTextFile(const std::filesystem::path& path,
                         std::string* content,
                         std::string* error_message) {
  if (content == nullptr || error_message == nullptr) {
    return false;
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    *error_message = "Failed to read muon config: " + path.string();
    return false;
  }
  content->assign((std::istreambuf_iterator<char>(input)),
                  std::istreambuf_iterator<char>());
  if (!input.eof() && input.fail()) {
    *error_message = "Failed to read muon config: " + path.string();
    return false;
  }
  return true;
}

static std::string FormatJsonParseError(const yyjson_read_err& error) {
  const auto message = error.msg == nullptr ? "parse failed" : error.msg;
  return "Invalid muon.json: " + std::string(message) + " at byte " +
         std::to_string(error.pos);
}

static bool IsEmptyMuonEmbeddedConfigSlot(const uint8_t* slot,
                                          size_t slot_size) {
  if (slot == nullptr || slot_size != kMuonEmbeddedConfigSlotSize) {
    return false;
  }
  for (auto index = size_t{0}; index < slot_size; ++index) {
    if (slot[index] != CalculateMuonEmbeddedConfigEmptySlotByte(index)) {
      return false;
    }
  }
  return true;
}

static bool InspectMuonEmbeddedConfigSlot(
    const uint8_t* slot,
    size_t slot_size,
    MuonEmbeddedConfigSlotState* state,
    std::string* error_message) {
  if (state == nullptr || error_message == nullptr) {
    return false;
  }
  *state = {};
  if (slot == nullptr || slot_size != kMuonEmbeddedConfigSlotSize) {
    *error_message = "Invalid embedded muon config slot size";
    return false;
  }
  if (IsEmptyMuonEmbeddedConfigSlot(slot, slot_size)) {
    return true;
  }

  state->embedded = true;
  return true;
}

static bool ReadEmbeddedVarUint(MuonEmbeddedTlvReader* reader,
                                uint64_t* value,
                                std::string* error_message) {
  if (reader == nullptr || value == nullptr || error_message == nullptr) {
    return false;
  }
  *value = 0;
  auto shift = 0u;
  while (reader->offset < reader->size) {
    const auto byte = reader->bytes[reader->offset++];
    if (shift >= 64 && (byte & 0x7fu) != 0) {
      *error_message = "Invalid embedded muon config integer";
      return false;
    }
    *value |= static_cast<uint64_t>(byte & 0x7fu) << shift;
    if ((byte & 0x80u) == 0) {
      return true;
    }
    shift += 7;
    if (shift > 63) {
      *error_message = "Invalid embedded muon config integer";
      return false;
    }
  }
  *error_message = "Unexpected end of embedded muon config";
  return false;
}

static bool ReadEmbeddedBytes(MuonEmbeddedTlvReader* reader,
                              size_t size,
                              const uint8_t** bytes,
                              std::string* error_message) {
  if (reader == nullptr || bytes == nullptr || error_message == nullptr) {
    return false;
  }
  if (size > reader->size || reader->offset > reader->size - size) {
    *error_message = "Unexpected end of embedded muon config";
    return false;
  }
  *bytes = reader->bytes + reader->offset;
  reader->offset += size;
  return true;
}

static bool ReadEmbeddedRawString(MuonEmbeddedTlvReader* reader,
                                  std::string* value,
                                  std::string* error_message) {
  uint64_t size = 0;
  if (!ReadEmbeddedVarUint(reader, &size, error_message)) {
    return false;
  }
  if (size > std::numeric_limits<size_t>::max()) {
    *error_message = "Embedded muon config string is too large";
    return false;
  }
  const uint8_t* bytes = nullptr;
  if (!ReadEmbeddedBytes(reader, static_cast<size_t>(size), &bytes,
                         error_message)) {
    return false;
  }
  value->assign(reinterpret_cast<const char*>(bytes), static_cast<size_t>(size));
  return true;
}

static bool IsEmbeddedPath(const std::vector<std::string>& path,
                           const char* first,
                           const char* second) {
  return path.size() == 2 && path[0] == first && path[1] == second;
}

static bool IsEmbeddedPluginEntryPath(const std::vector<std::string>& path,
                                      const char* key) {
  return path.size() == 3 && path[0] == kMuonConfigPluginKey &&
         path[1] == kMuonConfigPluginPluginsKey && path[2] == key;
}

static char EncodeLowerHexNibble(uint8_t value) {
  return static_cast<char>(value < 10 ? '0' + value : 'a' + (value - 10));
}

static std::string EncodeLowerHex(const uint8_t* bytes, size_t size) {
  std::string hex;
  hex.resize(size * 2);
  for (auto index = size_t{0}; index < size; ++index) {
    hex[index * 2] = EncodeLowerHexNibble(bytes[index] >> 4);
    hex[index * 2 + 1] = EncodeLowerHexNibble(bytes[index] & 0x0f);
  }
  return hex;
}

static bool CreateEmbeddedBinaryJsonValue(
    yyjson_mut_doc* document,
    const std::vector<std::string>& path,
    const uint8_t* bytes,
    size_t size,
    yyjson_mut_val** value,
    std::string* error_message) {
  std::string string_value;
  if (IsEmbeddedPath(path, kMuonConfigAssetKey,
                     kMuonConfigAssetSignatureKey)) {
    if (size != 32) {
      *error_message = "Embedded muon config asset.signature must be 32 bytes";
      return false;
    }
    string_value = EncodeLowerHex(bytes, size);
  } else if (IsEmbeddedPath(path, kMuonConfigAssetKey,
                            kMuonConfigAssetSaltKey)) {
    string_value = EncodeLowerHex(bytes, size);
  } else if (IsEmbeddedPluginEntryPath(path,
                                       kMuonConfigPluginEntrySignatureKey)) {
    if (size != 32) {
      *error_message =
          "Embedded muon config plugin.plugins[].signature must be 32 bytes";
      return false;
    }
    string_value = EncodeLowerHex(bytes, size);
  } else if (IsEmbeddedPluginEntryPath(path, kMuonConfigPluginEntrySaltKey)) {
    string_value = EncodeLowerHex(bytes, size);
  } else if (IsEmbeddedPath(path, kMuonConfigBrowserKey,
                            kMuonConfigBrowserBackgroundColorKey)) {
    if (size != 3) {
      *error_message =
          "Embedded muon config browser.backgroundColor must be 3 bytes";
      return false;
    }
    string_value = "#" + EncodeLowerHex(bytes, size);
  } else {
    *error_message = "Unsupported embedded muon config binary value";
    return false;
  }

  *value = yyjson_mut_strncpy(document, string_value.data(),
                              string_value.size());
  if (*value == nullptr) {
    *error_message = "Failed to decode embedded muon config";
    return false;
  }
  return true;
}

static bool DecodeEmbeddedTlvValue(MuonEmbeddedTlvReader* reader,
                                   yyjson_mut_doc* document,
                                   std::vector<std::string>* path,
                                   yyjson_mut_val** value,
                                   std::string* error_message);

static bool DecodeEmbeddedTlvArray(MuonEmbeddedTlvReader* reader,
                                   yyjson_mut_doc* document,
                                   std::vector<std::string>* path,
                                   yyjson_mut_val** value,
                                   std::string* error_message) {
  uint64_t count = 0;
  if (!ReadEmbeddedVarUint(reader, &count, error_message)) {
    return false;
  }
  const auto array = yyjson_mut_arr(document);
  if (array == nullptr) {
    *error_message = "Failed to decode embedded muon config";
    return false;
  }
  for (auto index = uint64_t{0}; index < count; ++index) {
    yyjson_mut_val* entry = nullptr;
    if (!DecodeEmbeddedTlvValue(reader, document, path, &entry,
                                error_message) ||
        !yyjson_mut_arr_append(array, entry)) {
      if (error_message->empty()) {
        *error_message = "Failed to decode embedded muon config";
      }
      return false;
    }
  }
  *value = array;
  return true;
}

static bool DecodeEmbeddedTlvObject(MuonEmbeddedTlvReader* reader,
                                    yyjson_mut_doc* document,
                                    std::vector<std::string>* path,
                                    yyjson_mut_val** value,
                                    std::string* error_message) {
  uint64_t count = 0;
  if (!ReadEmbeddedVarUint(reader, &count, error_message)) {
    return false;
  }
  const auto object = yyjson_mut_obj(document);
  if (object == nullptr) {
    *error_message = "Failed to decode embedded muon config";
    return false;
  }
  for (auto index = uint64_t{0}; index < count; ++index) {
    std::string key_string;
    if (!ReadEmbeddedRawString(reader, &key_string, error_message)) {
      return false;
    }
    const auto key =
        yyjson_mut_strncpy(document, key_string.data(), key_string.size());
    if (key == nullptr) {
      *error_message = "Failed to decode embedded muon config";
      return false;
    }

    path->push_back(key_string);
    yyjson_mut_val* entry = nullptr;
    const auto decoded =
        DecodeEmbeddedTlvValue(reader, document, path, &entry, error_message);
    path->pop_back();
    if (!decoded || !yyjson_mut_obj_put(object, key, entry)) {
      if (error_message->empty()) {
        *error_message = "Failed to decode embedded muon config";
      }
      return false;
    }
  }
  *value = object;
  return true;
}

static bool DecodeEmbeddedTlvValue(MuonEmbeddedTlvReader* reader,
                                   yyjson_mut_doc* document,
                                   std::vector<std::string>* path,
                                   yyjson_mut_val** value,
                                   std::string* error_message) {
  if (reader == nullptr || document == nullptr || path == nullptr ||
      value == nullptr || error_message == nullptr) {
    return false;
  }
  if (reader->offset >= reader->size) {
    *error_message = "Unexpected end of embedded muon config";
    return false;
  }

  const auto tag = reader->bytes[reader->offset++];
  if (tag == kMuonEmbeddedTlvNullTag) {
    *value = yyjson_mut_null(document);
  } else if (tag == kMuonEmbeddedTlvFalseTag ||
             tag == kMuonEmbeddedTlvTrueTag) {
    *value = yyjson_mut_bool(document, tag == kMuonEmbeddedTlvTrueTag);
  } else if (tag == kMuonEmbeddedTlvUintTag) {
    uint64_t number = 0;
    if (!ReadEmbeddedVarUint(reader, &number, error_message)) {
      return false;
    }
    *value = yyjson_mut_uint(document, number);
  } else if (tag == kMuonEmbeddedTlvStringTag) {
    std::string string_value;
    if (!ReadEmbeddedRawString(reader, &string_value, error_message)) {
      return false;
    }
    *value =
        yyjson_mut_strncpy(document, string_value.data(), string_value.size());
  } else if (tag == kMuonEmbeddedTlvBinaryTag) {
    uint64_t size = 0;
    if (!ReadEmbeddedVarUint(reader, &size, error_message)) {
      return false;
    }
    if (size > std::numeric_limits<size_t>::max()) {
      *error_message = "Embedded muon config binary value is too large";
      return false;
    }
    const uint8_t* bytes = nullptr;
    if (!ReadEmbeddedBytes(reader, static_cast<size_t>(size), &bytes,
                           error_message)) {
      return false;
    }
    return CreateEmbeddedBinaryJsonValue(document, *path, bytes,
                                         static_cast<size_t>(size), value,
                                         error_message);
  } else if (tag == kMuonEmbeddedTlvArrayTag) {
    return DecodeEmbeddedTlvArray(reader, document, path, value,
                                  error_message);
  } else if (tag == kMuonEmbeddedTlvObjectTag) {
    return DecodeEmbeddedTlvObject(reader, document, path, value,
                                   error_message);
  } else {
    *error_message = "Embedded muon config contains an unknown TLV tag";
    return false;
  }

  if (*value == nullptr) {
    *error_message = "Failed to decode embedded muon config";
    return false;
  }
  return true;
}

static bool ResolveCurrentDirectory(std::filesystem::path* current_directory,
                                    std::string* error_message) {
  if (current_directory == nullptr || error_message == nullptr) {
    return false;
  }
  std::error_code error;
  *current_directory = std::filesystem::current_path(error);
  if (error) {
    *error_message = "Failed to resolve current directory";
    return false;
  }
  return true;
}

static bool GetEnvironmentPath(const char* name, std::filesystem::path* path) {
  const auto* value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') {
    return false;
  }
  *path = std::filesystem::path(value);
  return true;
}

static std::string GetStartupApplicationName() {
  const auto command_line = GetMuonStartupCommandLine();
  if (!command_line.empty()) {
    const auto stem = std::filesystem::path(command_line[0]).stem().string();
    if (!stem.empty()) {
      return stem;
    }
  }

  std::filesystem::path executable_path;
  if (GetMuonExecutablePath(&executable_path)) {
    const auto stem = executable_path.stem().string();
    if (!stem.empty()) {
      return stem;
    }
  }
  return kMuonFallbackApplicationName;
}

static std::string GetStartupAppId() {
  const auto* launcher_app_id =
      std::getenv(kMuonLauncherAppIdEnvironmentName);
  if (launcher_app_id != nullptr) {
    const auto app_id = TrimAscii(launcher_app_id);
    if (!app_id.empty()) {
      return app_id;
    }
  }
  return GetStartupApplicationName();
}

static bool IsAppIdCharacter(char value) {
  const auto is_digit = value >= '0' && value <= '9';
  const auto is_lower = value >= 'a' && value <= 'z';
  const auto is_upper = value >= 'A' && value <= 'Z';
  return is_digit || is_lower || is_upper || value == '.' || value == '_' ||
         value == '-';
}

static std::string SanitizeAppId(const std::string& value) {
  std::string result = value;
  for (auto& character : result) {
    if (!IsAppIdCharacter(character)) {
      character = '.';
    }
  }

  auto start = size_t{0};
  while (start < result.size() && result[start] == '.') {
    start += 1;
  }
  auto end = result.size();
  while (end > start && result[end - 1] == '.') {
    end -= 1;
  }
  if (end == start) {
    return "muon-app";
  }
  return result.substr(start, end - start);
}

static bool GetDefaultStateHomeDirectory(std::filesystem::path* directory,
                                         std::string* error_message) {
#if defined(_WIN32)
  if (GetEnvironmentPath("LOCALAPPDATA", directory)) {
    return true;
  }
  std::filesystem::path user_profile;
  if (GetEnvironmentPath("USERPROFILE", &user_profile)) {
    *directory = user_profile / "AppData" / "Local";
    return true;
  }
  *error_message = "LOCALAPPDATA and USERPROFILE are unavailable";
  return false;
#else
  if (GetEnvironmentPath("XDG_STATE_HOME", directory)) {
    return true;
  }
  std::filesystem::path home;
  if (!GetEnvironmentPath("HOME", &home)) {
    *error_message = "XDG_STATE_HOME and HOME are unavailable";
    return false;
  }
  *directory = home / ".local" / "state";
  return true;
#endif
}

static bool ResolveDefaultBrowserProfilePath(
    const std::string& app_id,
    std::filesystem::path* profile,
    std::string* error_message) {
  const auto launch_source = GetMuonStartupLaunchSource();
  if (launch_source == kMuonLaunchSourceNone ||
      launch_source == kMuonLaunchSourceNormal) {
    std::filesystem::path state_home;
    if (!GetDefaultStateHomeDirectory(&state_home, error_message)) {
      return false;
    }
    *profile = (state_home / SanitizeAppId(app_id) /
                kMuonDefaultProfileDirectoryName)
                   .lexically_normal();
    return true;
  }

  *error_message =
      "Unsupported --muon-launch-from for browser.profilePath: " +
      launch_source;
  return false;
}

static bool ResolveCommandLineConfigPath(
    const std::filesystem::path& path,
    std::filesystem::path* resolved_path,
    std::string* error_message) {
  if (resolved_path == nullptr || error_message == nullptr) {
    return false;
  }
  if (path.empty()) {
    *error_message = "muon -c path must not be empty";
    return false;
  }
  if (path.is_absolute()) {
    *resolved_path = path.lexically_normal();
    return true;
  }

  std::filesystem::path current_directory;
  if (!ResolveCurrentDirectory(&current_directory, error_message)) {
    return false;
  }
  *resolved_path = (current_directory / path).lexically_normal();
  return true;
}

static bool GetMuonConfigBaseDirectory(
    const std::filesystem::path& config_path,
    std::filesystem::path* base_directory,
    std::string* error_message) {
  if (base_directory == nullptr || error_message == nullptr) {
    return false;
  }

  const auto directory = config_path.parent_path();
  if (directory.is_absolute()) {
    *base_directory = directory.lexically_normal();
    return true;
  }

  std::filesystem::path current_directory;
  if (!ResolveCurrentDirectory(&current_directory, error_message)) {
    return false;
  }
  if (directory.empty()) {
    *base_directory = current_directory.lexically_normal();
    return true;
  }
  *base_directory = (current_directory / directory).lexically_normal();
  return true;
}

static std::filesystem::path ResolveConfigRelativePath(
    const std::filesystem::path& base_directory,
    const std::filesystem::path& path) {
  if (path.is_absolute()) {
    return path.lexically_normal();
  }
  return (base_directory / path).lexically_normal();
}

static yyjson_val* GetObjectValue(yyjson_val* object, const char* key) {
  if (!yyjson_is_obj(object)) {
    return nullptr;
  }
  return yyjson_obj_get(object, key);
}

static bool HasBrowserProfilePath(yyjson_val* root) {
  const auto browser = GetObjectValue(root, kMuonConfigBrowserKey);
  return GetObjectValue(browser, kMuonConfigBrowserProfilePathKey) != nullptr;
}

static bool HasPluginPath(yyjson_val* root) {
  const auto plugin = GetObjectValue(root, kMuonConfigPluginKey);
  return GetObjectValue(plugin, kMuonConfigPluginPathKey) != nullptr;
}

static bool HasAssetFromPath(yyjson_val* root) {
  const auto asset = GetObjectValue(root, kMuonConfigAssetKey);
  return GetObjectValue(asset, kMuonConfigAssetSourcePathKey) != nullptr;
}

static bool HasLogOutputPath(yyjson_val* root) {
  const auto log = GetObjectValue(root, kMuonConfigLogKey);
  const auto output = GetObjectValue(log, kMuonConfigLogOutputKey);
  return GetObjectValue(output, kMuonConfigLogOutputPathKey) != nullptr;
}

static void UpdateConfigPathBases(
    yyjson_val* root,
    const std::filesystem::path& base_directory,
    MuonConfigPathBases* path_bases) {
  if (path_bases == nullptr) {
    return;
  }
  if (HasBrowserProfilePath(root)) {
    path_bases->browser_profile = base_directory;
    path_bases->has_browser_profile = true;
  }
  if (HasLogOutputPath(root)) {
    path_bases->log_output_path = base_directory;
    path_bases->has_log_output_path = true;
  }
  if (HasPluginPath(root)) {
    path_bases->plugin_path = base_directory;
    path_bases->has_plugin_path = true;
  }
  if (HasAssetFromPath(root)) {
    path_bases->asset_from = base_directory;
    path_bases->has_asset_from = true;
  }
}

static void ResolveConfigPathsFromBases(yyjson_val* root,
                                        const MuonConfigPathBases& path_bases,
                                        MuonConfig* config) {
  if (config == nullptr) {
    return;
  }
  if (HasBrowserProfilePath(root) && path_bases.has_browser_profile) {
    config->browser.profile = ResolveConfigRelativePath(
        path_bases.browser_profile, config->browser.profile);
  }
  if (HasLogOutputPath(root) && path_bases.has_log_output_path) {
    config->log.output.path = ResolveConfigRelativePath(
        path_bases.log_output_path, config->log.output.path);
  }
  if (HasPluginPath(root) && path_bases.has_plugin_path) {
    config->plugin.path =
        ResolveConfigRelativePath(path_bases.plugin_path, config->plugin.path);
  }
  if (HasAssetFromPath(root) && path_bases.has_asset_from) {
    config->asset.from =
        ResolveConfigRelativePath(path_bases.asset_from, config->asset.from);
  }
}

static bool ResolveDefaultConfigPathsFromBases(
    yyjson_val* root,
    MuonConfig* config,
    std::string* error_message) {
  if (config == nullptr || error_message == nullptr) {
    return false;
  }
  if (!HasBrowserProfilePath(root)) {
    return ResolveDefaultBrowserProfilePath(config->app_id,
                                            &config->browser.profile,
                                            error_message);
  }
  return true;
}

static bool ParseMuonLogLevel(const std::string& raw_level,
                              MuonLogLevel* level) {
  if (level == nullptr) {
    return false;
  }
  const auto normalized = ToLowerAscii(TrimAscii(raw_level));
  if (normalized == "debug") {
    *level = kMuonLogLevelDebug;
    return true;
  }
  if (normalized == "info") {
    *level = kMuonLogLevelInfo;
    return true;
  }
  if (normalized == "warning" || normalized == "warn") {
    *level = kMuonLogLevelWarning;
    return true;
  }
  if (normalized == "error") {
    *level = kMuonLogLevelError;
    return true;
  }
  if (normalized == "fatal") {
    *level = kMuonLogLevelFatal;
    return true;
  }
  if (normalized == "off") {
    *level = kMuonLogLevelOff;
    return true;
  }
  return false;
}

static bool ParseMuonLogOutputType(const std::string& raw_type,
                                   MuonLogOutputType* type,
                                   std::string* error_message) {
  if (type == nullptr || error_message == nullptr) {
    return false;
  }
  const auto normalized = ToLowerAscii(TrimAscii(raw_type));
  if (normalized == "stdout") {
    *type = kMuonLogOutputStdout;
    return true;
  }
  if (normalized == "stderr") {
    *type = kMuonLogOutputStderr;
    return true;
  }
  if (normalized == "file") {
    *type = kMuonLogOutputFile;
    return true;
  }
  if (normalized == "debug") {
#if defined(_WIN32)
    *type = kMuonLogOutputDebug;
    return true;
#else
    *error_message = "muon.json log.output.type debug is only supported on win32";
    return false;
#endif
  }
  if (normalized == "eventlog") {
#if defined(_WIN32)
    *type = kMuonLogOutputEventLog;
    return true;
#else
    *error_message =
        "muon.json log.output.type eventlog is only supported on win32";
    return false;
#endif
  }
  if (normalized == "syslog") {
#if defined(_WIN32)
    *error_message =
        "muon.json log.output.type syslog is only supported on posix";
    return false;
#else
    *type = kMuonLogOutputSyslog;
    return true;
#endif
  }
  *error_message = "muon.json log.output.type is unknown: " + raw_type;
  return false;
}

static void SetAllMuonLogSourceLevels(MuonLogConfig* config,
                                      MuonLogLevel level) {
  config->muon = level;
  config->cef = level;
  config->console = level;
  config->plugin = level;
}

static bool SetMuonLogSourceLevel(MuonLogConfig* config,
                                  const std::string& source,
                                  MuonLogLevel level,
                                  std::string* error_message) {
  if (source == kMuonConfigLogSourceMuonKey) {
    config->muon = level;
    return true;
  }
  if (source == kMuonConfigLogSourceCefKey) {
    config->cef = level;
    return true;
  }
  if (source == kMuonConfigLogSourceConsoleKey) {
    config->console = level;
    return true;
  }
  if (source == kMuonConfigLogSourcePluginKey) {
    config->plugin = level;
    return true;
  }
  *error_message = "muon.json log.sources has unknown source: " + source;
  return false;
}

static bool ReadShortcutModifier(const std::string& token,
                                 uint32_t* modifier) {
  if (modifier == nullptr) {
    return false;
  }
  if (token == "shift") {
    *modifier = kMuonShortcutModifierShift;
    return true;
  }
  if (token == "ctrl" || token == "control") {
    *modifier = kMuonShortcutModifierControl;
    return true;
  }
  if (token == "alt") {
    *modifier = kMuonShortcutModifierAlt;
    return true;
  }
  if (token == "meta" || token == "cmd" || token == "command" ||
      token == "super") {
    *modifier = kMuonShortcutModifierMeta;
    return true;
  }
  return false;
}

static bool ReadFunctionKeyCode(const std::string& token, int* key_code) {
  if (key_code == nullptr || token.size() < 2 || token[0] != 'f') {
    return false;
  }

  auto number = int{0};
  for (auto index = size_t{1}; index < token.size(); ++index) {
    const auto character = token[index];
    if (character < '0' || character > '9') {
      return false;
    }
    number = number * 10 + character - '0';
  }
  if (number < 1 || number > 24) {
    return false;
  }

  *key_code = kVirtualKeyF1 + number - 1;
  return true;
}

static bool ReadShortcutKeyCode(const std::string& token,
                                int* key_code,
                                bool* accepts_shift_variant) {
  if (key_code == nullptr || accepts_shift_variant == nullptr ||
      token.empty()) {
    return false;
  }
  *accepts_shift_variant = false;
  if (token.size() == 1) {
    const auto character = token[0];
    if (character >= 'a' && character <= 'z') {
      *key_code = kVirtualKeyA + character - 'a';
      return true;
    }
    if (character >= '0' && character <= '9') {
      *key_code = kVirtualKey0 + character - '0';
      return true;
    }
  }
  if (ReadFunctionKeyCode(token, key_code)) {
    return true;
  }
  if (token == "plus") {
    *key_code = kVirtualKeyOemPlus;
    *accepts_shift_variant = true;
    return true;
  }
  if (token == "equal") {
    *key_code = kVirtualKeyOemPlus;
    return true;
  }
  if (token == "minus") {
    *key_code = kVirtualKeyOemMinus;
    return true;
  }
  if (token == "backspace") {
    *key_code = kVirtualKeyBackspace;
    return true;
  }
  if (token == "tab") {
    *key_code = kVirtualKeyTab;
    return true;
  }
  if (token == "enter" || token == "return") {
    *key_code = kVirtualKeyEnter;
    return true;
  }
  if (token == "escape" || token == "esc") {
    *key_code = kVirtualKeyEscape;
    return true;
  }
  if (token == "space") {
    *key_code = kVirtualKeySpace;
    return true;
  }
  if (token == "insert") {
    *key_code = kVirtualKeyInsert;
    return true;
  }
  if (token == "delete" || token == "del") {
    *key_code = kVirtualKeyDelete;
    return true;
  }
  if (token == "home") {
    *key_code = kVirtualKeyHome;
    return true;
  }
  if (token == "end") {
    *key_code = kVirtualKeyEnd;
    return true;
  }
  if (token == "pageup") {
    *key_code = kVirtualKeyPageUp;
    return true;
  }
  if (token == "pagedown") {
    *key_code = kVirtualKeyPageDown;
    return true;
  }
  if (token == "left") {
    *key_code = kVirtualKeyLeft;
    return true;
  }
  if (token == "right") {
    *key_code = kVirtualKeyRight;
    return true;
  }
  if (token == "up") {
    *key_code = kVirtualKeyUp;
    return true;
  }
  if (token == "down") {
    *key_code = kVirtualKeyDown;
    return true;
  }
  return false;
}

static bool ParseMuonKeyboardShortcut(
    const std::string& raw_shortcut,
    const std::string& config_path,
    MuonKeyboardShortcut* shortcut,
    std::string* error_message) {
  if (shortcut == nullptr || error_message == nullptr) {
    return false;
  }

  *shortcut = MuonKeyboardShortcut();
  const auto shortcut_text = TrimAscii(raw_shortcut);
  if (shortcut_text.empty()) {
    *error_message = "muon.json " + config_path +
                     " shortcut must not be empty";
    return false;
  }

  auto token_begin = size_t{0};
  auto has_key = false;
  while (token_begin <= shortcut_text.size()) {
    const auto token_end = shortcut_text.find('+', token_begin);
    const auto token_size =
        token_end == std::string::npos ? std::string::npos
                                       : token_end - token_begin;
    const auto token = ToLowerAscii(
        TrimAscii(shortcut_text.substr(token_begin, token_size)));
    if (token.empty()) {
      *error_message =
          "muon.json " + config_path + " shortcut has an empty part";
      return false;
    }

    auto modifier = uint32_t{0};
    if (ReadShortcutModifier(token, &modifier)) {
      if ((shortcut->modifiers & modifier) != 0) {
        *error_message = "muon.json " + config_path +
                         " shortcut has a duplicate modifier: " + token;
        return false;
      }
      shortcut->modifiers |= modifier;
    } else {
      auto key_code = int{0};
      auto accepts_shift_variant = false;
      if (!ReadShortcutKeyCode(token, &key_code, &accepts_shift_variant)) {
        *error_message = "muon.json " + config_path +
                         " shortcut has an unsupported key: " + token;
        return false;
      }
      if (has_key) {
        *error_message =
            "muon.json " + config_path + " shortcut has multiple keys";
        return false;
      }
      shortcut->windows_key_code = key_code;
      shortcut->accepts_shift_variant = accepts_shift_variant;
      has_key = true;
    }

    if (token_end == std::string::npos) {
      break;
    }
    token_begin = token_end + 1;
  }

  if (!has_key) {
    *error_message =
        "muon.json " + config_path + " shortcut must include a key";
    return false;
  }
  shortcut->enabled = true;
  return true;
}

static bool ReadBrowserShortcut(yyjson_val* keybind,
                                const char* key,
                                MuonKeyboardShortcut* shortcut,
                                std::string* error_message) {
  const auto value = yyjson_obj_get(keybind, key);
  if (value == nullptr) {
    return true;
  }
  const auto config_path = std::string("browser.keybind.") + key;
  if (!yyjson_is_str(value)) {
    *error_message = "muon.json " + config_path + " must be a string";
    return false;
  }

  return ParseMuonKeyboardShortcut(
      std::string(yyjson_get_str(value), yyjson_get_len(value)), config_path,
      shortcut, error_message);
}

static bool ReadBrowserStartPageConfig(yyjson_val* browser,
                                       MuonConfig* config,
                                       std::string* error_message) {
  const auto start_page =
      yyjson_obj_get(browser, kMuonConfigBrowserStartPageKey);
  if (start_page == nullptr) {
    return true;
  }
  if (!yyjson_is_str(start_page)) {
    *error_message = "muon.json browser.startPage must be a string";
    return false;
  }
  config->browser.start_page = ReadJsonString(start_page);
  if (config->browser.start_page.empty()) {
    *error_message = "muon.json browser.startPage must not be empty";
    return false;
  }
  return true;
}

static bool ReadBrowserProfileConfig(yyjson_val* browser,
                                     MuonConfig* config,
                                     std::string* error_message) {
  const auto profile =
      yyjson_obj_get(browser, kMuonConfigBrowserProfilePathKey);
  if (profile == nullptr) {
    return true;
  }
  if (!yyjson_is_str(profile)) {
    *error_message = "muon.json browser.profilePath must be a string";
    return false;
  }
  const auto profile_path = ReadJsonString(profile);
  if (profile_path.empty()) {
    *error_message = "muon.json browser.profilePath must not be empty";
    return false;
  }
  config->browser.profile = profile_path;
  return true;
}

static bool ParseBrowserInitialWindowState(
    const std::string& raw_state,
    MuonBrowserInitialWindowState* state) {
  if (state == nullptr) {
    return false;
  }
  if (raw_state == "normal") {
    *state = kMuonBrowserInitialWindowStateNormal;
    return true;
  }
  if (raw_state == "hidden") {
    *state = kMuonBrowserInitialWindowStateHidden;
    return true;
  }
  if (raw_state == "minimized") {
    *state = kMuonBrowserInitialWindowStateMinimized;
    return true;
  }
  if (raw_state == "maximized") {
    *state = kMuonBrowserInitialWindowStateMaximized;
    return true;
  }
  if (raw_state == "fullscreen") {
    *state = kMuonBrowserInitialWindowStateFullscreen;
    return true;
  }
  return false;
}

static bool ReadBrowserInitialWindowStateConfig(
    yyjson_val* browser,
    MuonConfig* config,
    std::string* error_message) {
  const auto state =
      yyjson_obj_get(browser, kMuonConfigBrowserInitialWindowStateKey);
  if (state == nullptr) {
    return true;
  }
  if (!yyjson_is_str(state)) {
    *error_message =
        "muon.json browser.initialWindowState must be a string";
    return false;
  }
  const auto raw_state = ReadJsonString(state);
  if (raw_state.empty()) {
    *error_message =
        "muon.json browser.initialWindowState must not be empty";
    return false;
  }
  if (!ParseBrowserInitialWindowState(
          raw_state, &config->browser.initial_window_state)) {
    *error_message =
        "muon.json browser.initialWindowState has unknown value: " + raw_state;
    return false;
  }
  return true;
}

static bool ReadBrowserInitialTitleBarVisibilityConfig(
    yyjson_val* browser,
    MuonConfig* config,
    std::string* error_message) {
  const auto visibility =
      yyjson_obj_get(browser, kMuonConfigBrowserInitialTitleBarVisibilityKey);
  if (visibility == nullptr) {
    return true;
  }
  if (!yyjson_is_bool(visibility)) {
    *error_message =
        "muon.json browser.initialTitleBarVisibility must be a boolean";
    return false;
  }
  config->browser.initial_title_bar_visibility = yyjson_get_bool(visibility);
  return true;
}

static bool ReadBrowserInitialTitleBarIconConfig(
    yyjson_val* browser,
    MuonConfig* config,
    std::string* error_message) {
  const auto icon =
      yyjson_obj_get(browser, kMuonConfigBrowserInitialTitleBarIconKey);
  if (icon == nullptr) {
    return true;
  }
  if (!yyjson_is_str(icon)) {
    *error_message =
        "muon.json browser.initialTitleBarIcon must be a string";
    return false;
  }
  config->browser.initial_title_bar_icon = ReadJsonString(icon);
  if (config->browser.initial_title_bar_icon.empty()) {
    *error_message =
        "muon.json browser.initialTitleBarIcon must not be empty";
    return false;
  }
  config->browser.has_initial_title_bar_icon = true;
  return true;
}

static bool IsHexDigit(char value) {
  return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
         (value >= 'A' && value <= 'F');
}

static uint8_t HexDigitValue(char value) {
  if (value >= '0' && value <= '9') {
    return static_cast<uint8_t>(value - '0');
  }
  if (value >= 'a' && value <= 'f') {
    return static_cast<uint8_t>(value - 'a' + 10);
  }
  return static_cast<uint8_t>(value - 'A' + 10);
}

static uint8_t ParseHexByte(char high, char low) {
  return static_cast<uint8_t>((HexDigitValue(high) << 4) |
                              HexDigitValue(low));
}

static bool ParseBrowserBackgroundColor(
    const std::string& raw_color,
    MuonBrowserBackgroundColorConfig* background_color) {
  if (background_color == nullptr) {
    return false;
  }
  if (raw_color == "system") {
    *background_color = {};
    background_color->mode = kMuonBrowserBackgroundColorSystem;
    return true;
  }

  const auto hex_begin = raw_color.starts_with("#") ? size_t{1} : size_t{0};
  if (raw_color.size() - hex_begin != 6) {
    return false;
  }
  for (auto index = hex_begin; index < raw_color.size(); ++index) {
    if (!IsHexDigit(raw_color[index])) {
      return false;
    }
  }

  background_color->mode = kMuonBrowserBackgroundColorRgb;
  background_color->red = ParseHexByte(raw_color[hex_begin],
                                       raw_color[hex_begin + 1]);
  background_color->green = ParseHexByte(raw_color[hex_begin + 2],
                                         raw_color[hex_begin + 3]);
  background_color->blue = ParseHexByte(raw_color[hex_begin + 4],
                                        raw_color[hex_begin + 5]);
  return true;
}

static bool ReadBrowserBackgroundColorConfig(
    yyjson_val* browser,
    MuonConfig* config,
    std::string* error_message) {
  const auto background_color =
      yyjson_obj_get(browser, kMuonConfigBrowserBackgroundColorKey);
  if (background_color == nullptr) {
    return true;
  }
  if (!yyjson_is_str(background_color)) {
    *error_message = "muon.json browser.backgroundColor must be a string";
    return false;
  }
  const auto raw_color = ReadJsonString(background_color);
  if (raw_color.empty()) {
    *error_message = "muon.json browser.backgroundColor must not be empty";
    return false;
  }
  if (!ParseBrowserBackgroundColor(raw_color,
                                   &config->browser.background_color)) {
    *error_message =
        "muon.json browser.backgroundColor has unknown value: " + raw_color;
    return false;
  }
  return true;
}

static bool ParseBrowserTitleBar(
    const std::string& raw_title_bar,
    MuonBrowserTitleBarMode* title_bar) {
  if (title_bar == nullptr) {
    return false;
  }
  if (raw_title_bar == "native") {
    *title_bar = kMuonBrowserTitleBarNative;
    return true;
  }
  if (raw_title_bar == "muon") {
    *title_bar = kMuonBrowserTitleBarMuon;
    return true;
  }
  return false;
}

static bool ReadBrowserTitleBarConfig(yyjson_val* browser,
                                      MuonConfig* config,
                                      std::string* error_message) {
  const auto title_bar =
      yyjson_obj_get(browser, kMuonConfigBrowserTitleBarTypeKey);
  if (title_bar == nullptr) {
    return true;
  }
  if (!yyjson_is_str(title_bar)) {
    *error_message = "muon.json browser.titleBarType must be a string";
    return false;
  }
  const auto raw_title_bar = ReadJsonString(title_bar);
  if (raw_title_bar.empty()) {
    *error_message = "muon.json browser.titleBarType must not be empty";
    return false;
  }
  if (!ParseBrowserTitleBar(raw_title_bar, &config->browser.title_bar)) {
    *error_message =
        "muon.json browser.titleBarType has unknown value: " + raw_title_bar;
    return false;
  }
  return true;
}

static bool ParseBrowserContextMenuMode(
    const std::string& raw_mode,
    MuonBrowserContextMenuMode* mode) {
  if (mode == nullptr) {
    return false;
  }
  if (raw_mode == "standard") {
    *mode = kMuonBrowserContextMenuModeStandard;
    return true;
  }
  if (raw_mode == "disabled") {
    *mode = kMuonBrowserContextMenuModeDisabled;
    return true;
  }
  if (raw_mode == "custom") {
    *mode = kMuonBrowserContextMenuModeCustom;
    return true;
  }
  return false;
}

static bool ReadBrowserContextMenuConfig(yyjson_val* browser,
                                         MuonConfig* config,
                                         std::string* error_message) {
  const auto context_menu =
      yyjson_obj_get(browser, kMuonConfigBrowserContextMenuKey);
  if (context_menu == nullptr) {
    return true;
  }
  if (!yyjson_is_obj(context_menu)) {
    *error_message = "muon.json browser.contextMenu must be an object";
    return false;
  }

  const auto mode =
      yyjson_obj_get(context_menu, kMuonConfigBrowserContextMenuModeKey);
  if (mode == nullptr) {
    return true;
  }
  if (!yyjson_is_str(mode)) {
    *error_message =
        "muon.json browser.contextMenu.mode must be a string";
    return false;
  }
  const auto raw_mode = ReadJsonString(mode);
  if (raw_mode.empty()) {
    *error_message =
        "muon.json browser.contextMenu.mode must not be empty";
    return false;
  }
  if (!ParseBrowserContextMenuMode(raw_mode,
                                   &config->browser.context_menu_mode)) {
    *error_message =
        "muon.json browser.contextMenu.mode has unknown value: " + raw_mode;
    return false;
  }
  return true;
}

static bool ShortcutAcceptsModifiers(const MuonKeyboardShortcut& shortcut,
                                     uint32_t modifiers) {
  if (shortcut.modifiers == modifiers) {
    return true;
  }
  if (!shortcut.accepts_shift_variant ||
      (shortcut.modifiers & kMuonShortcutModifierShift) != 0) {
    return false;
  }
  return (shortcut.modifiers | kMuonShortcutModifierShift) == modifiers;
}

static bool ShortcutsOverlap(const MuonKeyboardShortcut& left,
                             const MuonKeyboardShortcut& right) {
  if (!left.enabled || !right.enabled ||
      left.windows_key_code != right.windows_key_code) {
    return false;
  }
  if (ShortcutAcceptsModifiers(left, right.modifiers) ||
      ShortcutAcceptsModifiers(right, left.modifiers)) {
    return true;
  }
  if (right.accepts_shift_variant &&
      (right.modifiers & kMuonShortcutModifierShift) == 0 &&
      ShortcutAcceptsModifiers(left,
                               right.modifiers | kMuonShortcutModifierShift)) {
    return true;
  }
  if (left.accepts_shift_variant &&
      (left.modifiers & kMuonShortcutModifierShift) == 0 &&
      ShortcutAcceptsModifiers(right,
                               left.modifiers | kMuonShortcutModifierShift)) {
    return true;
  }
  return false;
}

static bool ValidateBrowserShortcutAssignments(
    const MuonBrowserShortcutEntry* entries,
    size_t entry_count,
    std::string* error_message) {
  if (entries == nullptr || error_message == nullptr) {
    return false;
  }
  for (auto left_index = size_t{0}; left_index < entry_count; ++left_index) {
    for (auto right_index = left_index + 1; right_index < entry_count;
         ++right_index) {
      if (ShortcutsOverlap(*entries[left_index].shortcut,
                           *entries[right_index].shortcut)) {
        *error_message =
            "muon.json browser.keybind." +
            std::string(entries[left_index].key) +
            " and browser.keybind." + entries[right_index].key +
            " must not use overlapping shortcuts";
        return false;
      }
    }
  }
  return true;
}

static bool ReadBrowserKeybindsConfig(yyjson_val* browser,
                                      MuonConfig* config,
                                      std::string* error_message) {
  const auto keybind = yyjson_obj_get(browser, kMuonConfigBrowserKeybindsKey);
  if (keybind == nullptr) {
    return true;
  }
  if (!yyjson_is_obj(keybind)) {
    *error_message = "muon.json browser.keybind must be an object";
    return false;
  }

  MuonBrowserShortcutEntry entries[] = {
      {kMuonConfigBrowserDevToolsKey, &config->browser.devtools},
      {kMuonConfigBrowserReloadKey, &config->browser.reload},
      {kMuonConfigBrowserHardReloadKey, &config->browser.hard_reload},
      {kMuonConfigBrowserFullscreenKey, &config->browser.fullscreen},
      {kMuonConfigBrowserZoomInKey, &config->browser.zoom_in},
      {kMuonConfigBrowserZoomOutKey, &config->browser.zoom_out},
      {kMuonConfigBrowserResetZoomKey, &config->browser.reset_zoom},
      {kMuonConfigBrowserRecycleKey, &config->browser.recycle},
  };
  constexpr size_t entry_count = sizeof(entries) / sizeof(entries[0]);
  for (auto index = size_t{0}; index < entry_count; ++index) {
    if (!ReadBrowserShortcut(keybind, entries[index].key,
                             entries[index].shortcut, error_message)) {
      return false;
    }
  }
  return ValidateBrowserShortcutAssignments(entries, entry_count,
                                            error_message);
}

static bool ParseBrowserPluginMode(const std::string& raw_mode,
                                   MuonBrowserPluginMode* mode) {
  if (mode == nullptr) {
    return false;
  }
  if (raw_mode == "simple") {
    *mode = kMuonBrowserPluginModeSimple;
    return true;
  }
  if (raw_mode == "validate") {
    *mode = kMuonBrowserPluginModeValidate;
    return true;
  }
  return false;
}

static bool ReadPluginModeConfig(yyjson_val* plugin,
                                 MuonConfig* config,
                                 std::string* error_message) {
  const auto mode = yyjson_obj_get(plugin, kMuonConfigPluginModeKey);
  if (mode == nullptr) {
    return true;
  }
  if (!yyjson_is_str(mode)) {
    *error_message = "muon.json plugin.mode must be a string";
    return false;
  }
  const auto raw_mode = TrimAscii(ReadJsonString(mode));
  if (raw_mode.empty()) {
    *error_message = "muon.json plugin.mode must not be empty";
    return false;
  }
  if (!ParseBrowserPluginMode(raw_mode, &config->browser.plugin.mode)) {
    *error_message = "muon.json plugin.mode has unknown value: " + raw_mode;
    return false;
  }
  return true;
}

static bool ReadPluginCapabilitiesConfig(yyjson_val* plugin,
                                         MuonConfig* config,
                                         std::string* error_message) {
  const auto capabilities =
      yyjson_obj_get(plugin, kMuonConfigPluginCapabilitiesKey);
  if (capabilities == nullptr) {
    return true;
  }
  if (!yyjson_is_arr(capabilities)) {
    *error_message = "muon.json plugin.capabilities must be an array";
    return false;
  }

  config->browser.plugin.capabilities.clear();
  const auto capability_count = yyjson_arr_size(capabilities);
  for (auto index = size_t{0}; index < capability_count; ++index) {
    const auto capability = yyjson_arr_get(capabilities, index);
    if (!yyjson_is_obj(capability)) {
      *error_message =
          "muon.json plugin.capabilities entries must be objects";
      return false;
    }

    const auto id =
        yyjson_obj_get(capability, kMuonConfigPluginCapabilityIdKey);
    if (!yyjson_is_str(id)) {
      *error_message = "muon.json plugin.capabilities.id must be a string";
      return false;
    }

    MuonBrowserPluginCapabilityConfig capability_config;
    capability_config.id = ReadJsonString(id);
    if (capability_config.id.empty()) {
      *error_message =
          "muon.json plugin.capabilities.id must not be empty";
      return false;
    }
    if (!ReadStringArray(capability, kMuonConfigPluginEntryAllowKey,
                         "plugin.capabilities.allow",
                         &capability_config.allow, error_message)) {
      return false;
    }
    config->browser.plugin.capabilities.push_back(
        std::move(capability_config));
  }
  return true;
}

static bool RejectLegacyBrowserPluginConfig(yyjson_val* browser,
                                           std::string* error_message) {
  const auto plugin = yyjson_obj_get(browser, kMuonConfigBrowserPluginKey);
  if (plugin == nullptr) {
    return true;
  }
  *error_message =
      "muon.json browser.plugin is no longer supported; use plugin.mode and "
      "plugin.pages instead";
  return false;
}

static bool ReadBrowserConfig(yyjson_val* root,
                              MuonConfig* config,
                              std::string* error_message) {
  const auto browser = yyjson_obj_get(root, kMuonConfigBrowserKey);
  if (browser == nullptr) {
    return true;
  }
  if (!yyjson_is_obj(browser)) {
    *error_message = "muon.json browser must be an object";
    return false;
  }
  if (!ReadBrowserStartPageConfig(browser, config, error_message) ||
      !ReadBrowserProfileConfig(browser, config, error_message) ||
      !ReadBrowserInitialWindowStateConfig(browser, config, error_message) ||
      !ReadBrowserInitialTitleBarVisibilityConfig(
          browser, config, error_message) ||
      !ReadBrowserInitialTitleBarIconConfig(browser, config, error_message) ||
      !ReadBrowserBackgroundColorConfig(browser, config, error_message) ||
      !ReadBrowserTitleBarConfig(browser, config, error_message) ||
      !ReadBrowserContextMenuConfig(browser, config, error_message) ||
      !ReadBrowserKeybindsConfig(browser, config, error_message)) {
    return false;
  }
  return ReadStringArray(
             browser, kMuonConfigBrowserAllowUnsafeJavaScriptParentAccessKey,
             "browser.allowUnsafeJavaScriptParentAccess",
             &config->browser.allow_unsafe_javascript_parent_access,
             error_message) &&
         RejectLegacyBrowserPluginConfig(browser, error_message);
}

static bool ReadStringArray(yyjson_val* object,
                            const char* key,
                            const std::string& config_path,
                            std::vector<std::string>* target,
                            std::string* error_message) {
  const auto allow = yyjson_obj_get(object, key);
  if (allow == nullptr) {
    return true;
  }
  if (!yyjson_is_arr(allow)) {
    *error_message = "muon.json " + config_path + " must be an array";
    return false;
  }

  target->clear();
  const auto allow_size = yyjson_arr_size(allow);
  for (auto index = size_t{0}; index < allow_size; ++index) {
    const auto pattern = yyjson_arr_get(allow, index);
    if (!yyjson_is_str(pattern)) {
      *error_message =
          "muon.json " + config_path + " entries must be strings";
      return false;
    }
    target->emplace_back(yyjson_get_str(pattern), yyjson_get_len(pattern));
  }
  return true;
}

static bool ReadRequiredStringArray(yyjson_val* object,
                                    const char* key,
                                    const std::string& config_path,
                                    std::vector<std::string>* target,
                                    std::string* error_message) {
  if (yyjson_obj_get(object, key) == nullptr) {
    *error_message = "muon.json " + config_path + " is required";
    return false;
  }
  return ReadStringArray(object, key, config_path, target, error_message);
}

static bool IsMuonOriginSchemeCharacterAllowed(char value, bool first) {
  const auto character = static_cast<unsigned char>(value);
  if (first) {
    return std::isalpha(character) != 0;
  }
  return std::isalnum(character) != 0 || value == '+' || value == '-' ||
         value == '.';
}

static bool ValidateMuonOriginScheme(const std::string& scheme,
                                     const std::string& config_path,
                                     std::string* error_message) {
  if (scheme.empty()) {
    *error_message = "muon.json " + config_path + " must not be empty";
    return false;
  }
  for (auto index = size_t{0}; index < scheme.size(); ++index) {
    if (!IsMuonOriginSchemeCharacterAllowed(scheme[index], index == 0)) {
      *error_message =
          "muon.json " + config_path + " must be a URL scheme";
      return false;
    }
  }
  return true;
}

static bool ValidateMuonOriginDomain(const std::string& domain,
                                     const std::string& config_path,
                                     std::string* error_message) {
  if (domain.empty()) {
    *error_message = "muon.json " + config_path + " must not be empty";
    return false;
  }
  for (const auto character : domain) {
    if (character == ':' || character == '/' || character == '?' ||
        character == '#' || character == '*') {
      *error_message =
          "muon.json " + config_path +
          " must be a domain without separators or wildcards";
      return false;
    }
  }
  return true;
}

static bool ReadRequiredStringValue(yyjson_val* object,
                                    const char* key,
                                    const std::string& config_path,
                                    std::string* target,
                                    std::string* error_message) {
  const auto value = yyjson_obj_get(object, key);
  if (value == nullptr) {
    *error_message = "muon.json " + config_path + " is required";
    return false;
  }
  if (!yyjson_is_str(value)) {
    *error_message = "muon.json " + config_path + " must be a string";
    return false;
  }
  *target = ReadJsonString(value);
  return true;
}

static bool ReadOptionalPortValue(yyjson_val* object,
                                  const char* key,
                                  const std::string& config_path,
                                  int* target,
                                  std::string* error_message) {
  const auto value = yyjson_obj_get(object, key);
  if (value == nullptr) {
    *target = 0;
    return true;
  }
  if (!yyjson_is_uint(value)) {
    *error_message =
        "muon.json " + config_path + " must be an integer from 1 to 65535";
    return false;
  }
  const auto port = yyjson_get_uint(value);
  if (port == 0 || port > 65535) {
    *error_message =
        "muon.json " + config_path + " must be an integer from 1 to 65535";
    return false;
  }
  *target = static_cast<int>(port);
  return true;
}

static bool ReadAuthorizedOriginArray(
    yyjson_val* object,
    const char* key,
    const std::string& config_path,
    std::vector<MuonAuthorizedOriginConfig>* target,
    std::string* error_message) {
  const auto origins = yyjson_obj_get(object, key);
  if (origins == nullptr) {
    return true;
  }
  if (!yyjson_is_arr(origins)) {
    *error_message = "muon.json " + config_path + " must be an array";
    return false;
  }

  target->clear();
  const auto origin_count = yyjson_arr_size(origins);
  for (auto index = size_t{0}; index < origin_count; ++index) {
    const auto entry = yyjson_arr_get(origins, index);
    const auto entry_path = config_path + "[" + std::to_string(index) + "]";
    if (!yyjson_is_obj(entry)) {
      *error_message = "muon.json " + entry_path + " must be an object";
      return false;
    }

    MuonAuthorizedOriginConfig origin_config;
    if (!ReadRequiredStringValue(
            entry, kMuonConfigNetworkAuthorizedOriginSchemeKey,
            entry_path + ".scheme", &origin_config.scheme, error_message) ||
        !ReadRequiredStringValue(
            entry, kMuonConfigNetworkAuthorizedOriginDomainKey,
            entry_path + ".domain", &origin_config.domain, error_message) ||
        !ReadOptionalPortValue(
            entry, kMuonConfigNetworkAuthorizedOriginPortKey,
            entry_path + ".port", &origin_config.port, error_message)) {
      return false;
    }
    origin_config.scheme = ToLowerAscii(origin_config.scheme);
    origin_config.domain = ToLowerAscii(origin_config.domain);
    if (!ValidateMuonOriginScheme(origin_config.scheme,
                                  entry_path + ".scheme", error_message) ||
        !ValidateMuonOriginDomain(origin_config.domain,
                                  entry_path + ".domain", error_message)) {
      return false;
    }
    target->push_back(std::move(origin_config));
  }
  return true;
}

static bool IsMuonPluginNameCharacterAllowed(char value) {
  const auto character = static_cast<unsigned char>(value);
  if (character < 0x20 || character == 0x7F) {
    return false;
  }
  return value != '/' && value != '\\' && value != ':';
}

static bool EndsWithAscii(const std::string& value, const char* suffix) {
  const auto suffix_size = std::char_traits<char>::length(suffix);
  return value.size() >= suffix_size &&
         value.compare(value.size() - suffix_size, suffix_size, suffix) == 0;
}

static bool ValidateMuonPluginEntryName(const std::string& name,
                                        std::string* error_message) {
  if (name.empty()) {
    *error_message = "muon.json plugin.plugins entry name must not be empty";
    return false;
  }
  if (name == "." || name == "..") {
    *error_message =
        "muon.json plugin.plugins entry name must be a file name stem: " +
        name;
    return false;
  }
  for (const auto character : name) {
    if (!IsMuonPluginNameCharacterAllowed(character)) {
      *error_message =
          "muon.json plugin.plugins entry name must be a file name stem: " +
          name;
      return false;
    }
  }
  if (name != kMuonInternalPluginName &&
      (EndsWithAscii(name, ".so") || EndsWithAscii(name, ".dll"))) {
    *error_message =
        "muon.json plugin.plugins entry name must omit the library "
        "extension: " +
        name;
    return false;
  }
  return true;
}

static bool ReadLogLevelValue(yyjson_val* object,
                              const char* key,
                              const std::string& config_path,
                              MuonLogLevel* target,
                              std::string* error_message) {
  const auto value = yyjson_obj_get(object, key);
  if (value == nullptr) {
    return true;
  }
  if (!yyjson_is_str(value)) {
    *error_message = "muon.json " + config_path + " must be a string";
    return false;
  }
  const auto raw_level = ReadJsonString(value);
  if (!ParseMuonLogLevel(raw_level, target)) {
    *error_message = "muon.json " + config_path +
                     " has unknown level: " + raw_level;
    return false;
  }
  return true;
}

static bool ReadLogOutputConfig(yyjson_val* log,
                                MuonConfig* config,
                                std::string* error_message) {
  const auto output = yyjson_obj_get(log, kMuonConfigLogOutputKey);
  if (output == nullptr) {
    return true;
  }
  if (!yyjson_is_obj(output)) {
    *error_message = "muon.json log.output must be an object";
    return false;
  }

  const auto type_value = yyjson_obj_get(output, kMuonConfigLogOutputTypeKey);
  if (type_value != nullptr) {
    if (!yyjson_is_str(type_value)) {
      *error_message = "muon.json log.output.type must be a string";
      return false;
    }
    if (!ParseMuonLogOutputType(ReadJsonString(type_value),
                                &config->log.output.type, error_message)) {
      return false;
    }
  }

  const auto path_value = yyjson_obj_get(output, kMuonConfigLogOutputPathKey);
  if (path_value != nullptr) {
    if (!yyjson_is_str(path_value)) {
      *error_message = "muon.json log.output.path must be a string";
      return false;
    }
    config->log.output.path = ReadJsonString(path_value);
  }

  if (config->log.output.type == kMuonLogOutputFile) {
    if (config->log.output.path.empty()) {
      *error_message = "muon.json log.output.path is required for file output";
      return false;
    }
    return true;
  }
  if (path_value != nullptr) {
    *error_message =
        "muon.json log.output.path is only valid for file output";
    return false;
  }
  return true;
}

static bool ReadLogSourcesConfig(yyjson_val* log,
                                 MuonConfig* config,
                                 bool level_was_set,
                                 std::string* error_message) {
  const auto sources = yyjson_obj_get(log, kMuonConfigLogSourcesKey);
  if (sources == nullptr) {
    return true;
  }
  if (!yyjson_is_obj(sources)) {
    *error_message = "muon.json log.sources must be an object";
    return false;
  }
  if (!level_was_set) {
    SetAllMuonLogSourceLevels(&config->log, config->log.level);
  }

  size_t index;
  size_t max;
  yyjson_val* key;
  yyjson_val* value;
  yyjson_obj_foreach(sources, index, max, key, value) {
    const auto source = ReadJsonString(key);
    if (!yyjson_is_str(value)) {
      *error_message =
          "muon.json log.sources." + source + " must be a string";
      return false;
    }
    MuonLogLevel level;
    const auto raw_level = ReadJsonString(value);
    if (!ParseMuonLogLevel(raw_level, &level)) {
      *error_message = "muon.json log.sources." + source +
                       " has unknown level: " + raw_level;
      return false;
    }
    if (!SetMuonLogSourceLevel(&config->log, source, level,
                               error_message)) {
      return false;
    }
  }
  return true;
}

static bool ReadLogConfig(yyjson_val* root,
                          MuonConfig* config,
                          std::string* error_message) {
  const auto log = yyjson_obj_get(root, kMuonConfigLogKey);
  if (log == nullptr) {
    return true;
  }
  if (!yyjson_is_obj(log)) {
    *error_message = "muon.json log must be an object";
    return false;
  }

  const auto level_was_set =
      yyjson_obj_get(log, kMuonConfigLogLevelKey) != nullptr;
  if (!ReadLogLevelValue(log, kMuonConfigLogLevelKey, "log.level",
                         &config->log.level, error_message)) {
    return false;
  }
  if (level_was_set) {
    SetAllMuonLogSourceLevels(&config->log, config->log.level);
  }
  return ReadLogOutputConfig(log, config, error_message) &&
         ReadLogSourcesConfig(log, config, level_was_set, error_message);
}

static bool ReadSha256SignatureString(yyjson_val* value,
                                      const std::string& config_path,
                                      std::string* signature,
                                      std::string* error_message) {
  if (!yyjson_is_str(value)) {
    *error_message = "muon.json " + config_path + " must be a string";
    return false;
  }
  auto normalized_signature = ToLowerAscii(ReadJsonString(value));
  if (normalized_signature.size() != 64) {
    *error_message =
        "muon.json " + config_path +
        " must be a 64-character SHA-256 hex string";
    return false;
  }
  for (const auto character : normalized_signature) {
    if (!IsAsciiHexDigit(character)) {
      *error_message =
          "muon.json " + config_path +
          " must be a 64-character SHA-256 hex string";
      return false;
    }
  }
  *signature = std::move(normalized_signature);
  return true;
}

static bool ReadHexByteString(yyjson_val* value,
                              const std::string& config_path,
                              std::vector<uint8_t>* bytes,
                              std::string* error_message) {
  if (!yyjson_is_str(value)) {
    *error_message = "muon.json " + config_path + " must be a string";
    return false;
  }
  const auto hex = ReadJsonString(value);
  if (hex.size() % 2 != 0) {
    *error_message =
        "muon.json " + config_path + " must be a hexadecimal byte string";
    return false;
  }
  std::vector<uint8_t> decoded;
  decoded.reserve(hex.size() / 2);
  for (auto index = size_t{0}; index < hex.size(); index += 2) {
    if (!IsAsciiHexDigit(hex[index]) ||
        !IsAsciiHexDigit(hex[index + 1])) {
      *error_message =
          "muon.json " + config_path + " must be a hexadecimal byte string";
      return false;
    }
    decoded.push_back(DecodeAsciiHexByte(hex[index], hex[index + 1]));
  }
  *bytes = std::move(decoded);
  return true;
}

static bool ReadAssetConfig(yyjson_val* root,
                            MuonConfig* config,
                            std::string* error_message) {
  const auto asset = yyjson_obj_get(root, kMuonConfigAssetKey);
  if (asset == nullptr) {
    return true;
  }
  if (!yyjson_is_obj(asset)) {
    *error_message = "muon.json asset must be an object";
    return false;
  }

  const auto from = yyjson_obj_get(asset, kMuonConfigAssetSourcePathKey);
  if (from != nullptr) {
    if (!yyjson_is_str(from)) {
      *error_message = "muon.json asset.sourcePath must be a string";
      return false;
    }
    config->asset.from = ReadJsonString(from);
    if (config->asset.from.empty()) {
      *error_message = "muon.json asset.sourcePath must not be empty";
      return false;
    }
    config->asset.has_from = true;
  }

  const auto signature = yyjson_obj_get(asset, kMuonConfigAssetSignatureKey);
  if (signature != nullptr) {
    if (!ReadSha256SignatureString(signature, "asset.signature",
                                   &config->asset.signature, error_message)) {
      return false;
    }
    config->asset.has_signature = true;
  }

  const auto salt = yyjson_obj_get(asset, kMuonConfigAssetSaltKey);
  if (salt == nullptr) {
    return true;
  }
  if (!ReadHexByteString(salt, "asset.salt", &config->asset.salt,
                         error_message)) {
    return false;
  }
  config->asset.has_salt = true;
  return true;
}

static bool ReadNetworkConfig(yyjson_val* root,
                              MuonConfig* config,
                              std::string* error_message) {
  const auto network = yyjson_obj_get(root, kMuonConfigNetworkKey);
  if (network == nullptr) {
    return true;
  }
  if (!yyjson_is_obj(network)) {
    *error_message = "muon.json network must be an object";
    return false;
  }
  if (!ReadStringArray(network, kMuonConfigNetworkAllowKey, "network.allow",
                       &config->network.allow, error_message) ||
      !ReadAuthorizedOriginArray(network,
                                 kMuonConfigNetworkAuthorizedOriginKey,
                                 "network.authorizedOrigin",
                                 &config->network.authorized_origin,
                                 error_message)) {
    return false;
  }

  const auto local_access =
      yyjson_obj_get(network, kMuonConfigNetworkLocalAccessKey);
  if (local_access == nullptr) {
    return true;
  }
  if (!yyjson_is_obj(local_access)) {
    *error_message = "muon.json network.localAccess must be an object";
    return false;
  }
  return ReadAuthorizedOriginArray(
             local_access, kMuonConfigNetworkLoopbackOriginsKey,
             "network.localAccess.loopbackOrigins",
             &config->network.local_access.loopback_origins, error_message) &&
         ReadAuthorizedOriginArray(
             local_access, kMuonConfigNetworkLocalNetworkOriginsKey,
             "network.localAccess.localNetworkOrigins",
             &config->network.local_access.local_network_origins,
             error_message);
}

static bool ReadDebuggerConfig(yyjson_val* root,
                               MuonConfig* config,
                               std::string* error_message) {
  const auto cdp = yyjson_obj_get(root, kMuonConfigCdpKey);
  if (cdp == nullptr) {
    return true;
  }
  if (!yyjson_is_obj(cdp)) {
    *error_message = "muon.json cdp must be an object";
    return false;
  }

  const auto enable = yyjson_obj_get(cdp, kMuonConfigCdpEnableKey);
  if (enable != nullptr) {
    if (!yyjson_is_bool(enable)) {
      *error_message = "muon.json cdp.enable must be a boolean";
      return false;
    }
    config->cdp.enable = yyjson_get_bool(enable);
  }

  const auto port = yyjson_obj_get(cdp, kMuonConfigCdpPortKey);
  if (port != nullptr) {
    if (!yyjson_is_uint(port)) {
      *error_message =
          "muon.json cdp.port must be an integer from 1024 to 65535";
      return false;
    }
    const auto port_number = yyjson_get_uint(port);
    if (port_number < 1024 || port_number > 65535) {
      *error_message =
          "muon.json cdp.port must be an integer from 1024 to 65535";
      return false;
    }
    config->cdp.port = static_cast<int>(port_number);
  }
  return true;
}

static bool ContainsNulByte(const std::string& value) {
  return value.find('\0') != std::string::npos;
}

static bool ReadStringConfig(
    yyjson_val* parent,
    const char* config_key,
    const std::string& config_value_path,
    std::vector<MuonStringConfigEntry>* target,
    std::string* error_message) {
  if (target == nullptr || error_message == nullptr) {
    return false;
  }
  target->clear();
  const auto config_value = yyjson_obj_get(parent, config_key);
  if (config_value == nullptr) {
    return true;
  }
  if (!yyjson_is_obj(config_value)) {
    *error_message = "muon.json " + config_value_path + " must be an object";
    return false;
  }

  std::set<std::string> keys;
  size_t index = 0;
  size_t max = 0;
  yyjson_val* key = nullptr;
  yyjson_val* value = nullptr;
  yyjson_obj_foreach(config_value, index, max, key, value) {
    std::string key_string(yyjson_get_str(key), yyjson_get_len(key));
    if (key_string.empty()) {
      *error_message =
          "muon.json " + config_value_path + " key must not be empty";
      return false;
    }
    if (ContainsNulByte(key_string)) {
      *error_message =
          "muon.json " + config_value_path + " key must not contain NUL";
      return false;
    }
    if (keys.find(key_string) != keys.end()) {
      *error_message =
          "muon.json " + config_value_path +
          " has duplicate key: " + key_string;
      return false;
    }
    keys.insert(key_string);

    const auto value_path = config_value_path + "." + key_string;
    if (!yyjson_is_str(value)) {
      *error_message = "muon.json " + value_path + " must be a string";
      return false;
    }
    std::string value_string(yyjson_get_str(value), yyjson_get_len(value));
    if (ContainsNulByte(value_string)) {
      *error_message = "muon.json " + value_path + " must not contain NUL";
      return false;
    }
    target->push_back({std::move(key_string), std::move(value_string)});
  }
  return true;
}

static bool ReadApplicationConfig(yyjson_val* root,
                                  MuonConfig* config,
                                  std::string* error_message) {
  return ReadStringConfig(root, kMuonConfigApplicationConfigKey, "config",
                          &config->config, error_message);
}

static bool ReadPluginConfig(yyjson_val* root,
                             MuonConfig* config,
                             std::string* error_message) {
  if (yyjson_obj_get(root, kMuonConfigLegacyPluginsKey) != nullptr) {
    *error_message =
        "muon.json plugins is no longer supported; use plugin.plugins instead";
    return false;
  }

  const auto plugin = yyjson_obj_get(root, kMuonConfigPluginKey);
  if (plugin == nullptr) {
    return true;
  }
  if (!yyjson_is_obj(plugin)) {
    *error_message = "muon.json plugin must be an object";
    return false;
  }

  if (!ReadPluginModeConfig(plugin, config, error_message) ||
      !ReadStringArray(plugin, kMuonConfigPluginPagesKey, "plugin.pages",
                       &config->browser.plugin.allow, error_message) ||
      !ReadPluginCapabilitiesConfig(plugin, config, error_message)) {
    return false;
  }

  const auto plugin_path = yyjson_obj_get(plugin, kMuonConfigPluginPathKey);
  if (plugin_path != nullptr) {
    if (!yyjson_is_str(plugin_path)) {
      *error_message = "muon.json plugin.path must be a string";
      return false;
    }
    config->plugin.path = ReadJsonString(plugin_path);
    if (config->plugin.path.empty()) {
      *error_message = "muon.json plugin.path must not be empty";
      return false;
    }
  }

  const auto plugins = yyjson_obj_get(plugin, kMuonConfigPluginPluginsKey);
  if (plugins == nullptr) {
    return true;
  }
  if (!yyjson_is_arr(plugins)) {
    *error_message = "muon.json plugin.plugins must be an array";
    return false;
  }

  config->plugin.plugins.clear();
  std::set<std::string> plugin_names;
  const auto plugin_count = yyjson_arr_size(plugins);
  for (auto index = size_t{0}; index < plugin_count; ++index) {
    const auto entry = yyjson_arr_get(plugins, index);
    const auto config_path =
        "plugin.plugins[" + std::to_string(index) + "]";
    if (!yyjson_is_obj(entry)) {
      *error_message = "muon.json " + config_path + " must be an object";
      return false;
    }

    const auto name_value =
        yyjson_obj_get(entry, kMuonConfigPluginEntryNameKey);
    if (name_value == nullptr) {
      *error_message = "muon.json " + config_path + ".name is required";
      return false;
    }
    if (!yyjson_is_str(name_value)) {
      *error_message = "muon.json " + config_path + ".name must be a string";
      return false;
    }

    MuonPluginEntryConfig plugin_config;
    plugin_config.name = ReadJsonString(name_value);
    if (!ValidateMuonPluginEntryName(plugin_config.name, error_message)) {
      return false;
    }
    if (plugin_names.find(plugin_config.name) != plugin_names.end()) {
      *error_message =
          "muon.json plugin.plugins has duplicate plugin entry: " +
          plugin_config.name;
      return false;
    }
    plugin_names.insert(plugin_config.name);

    const auto signature_value =
        yyjson_obj_get(entry, kMuonConfigPluginEntrySignatureKey);
    if (signature_value != nullptr) {
      if (plugin_config.name == kMuonInternalPluginName) {
        *error_message =
            "muon.json " + config_path +
            ".signature is not supported for internal plugins";
        return false;
      }
      if (!ReadSha256SignatureString(signature_value,
                                     config_path + ".signature",
                                     &plugin_config.signature,
                                     error_message)) {
        return false;
      }
      plugin_config.has_signature = true;
    }

    const auto salt_value =
        yyjson_obj_get(entry, kMuonConfigPluginEntrySaltKey);
    if (salt_value != nullptr) {
      if (plugin_config.name == kMuonInternalPluginName) {
        *error_message =
            "muon.json " + config_path +
            ".salt is not supported for internal plugins";
        return false;
      }
      if (!ReadHexByteString(salt_value, config_path + ".salt",
                             &plugin_config.salt, error_message)) {
        return false;
      }
      plugin_config.has_salt = true;
    }
    if (plugin_config.has_signature && !plugin_config.has_salt) {
      *error_message =
          "muon.json " + config_path + ".signature requires " + config_path +
          ".salt";
      return false;
    }

    if (yyjson_obj_get(entry, kMuonConfigPluginEntryAllowKey) == nullptr &&
        yyjson_obj_get(entry, kMuonConfigPluginEntryImportsKey) != nullptr) {
      *error_message =
          "muon.json " + config_path +
          ".allow is required in runtime config; validate imports require "
          "Vite capability generation";
      return false;
    }
    if (!ReadRequiredStringArray(entry, kMuonConfigPluginEntryAllowKey,
                                 config_path + ".allow",
                                 &plugin_config.allow, error_message)) {
      return false;
    }
    if (!ReadStringConfig(entry, kMuonConfigPluginEntryConfigKey,
                          config_path + ".config", &plugin_config.config,
                          error_message)) {
      return false;
    }
    config->plugin.plugins.push_back(std::move(plugin_config));
  }
  return true;
}

static bool ReadMuonConfigDocument(const std::filesystem::path& path,
                                   bool missing_is_success,
                                   MuonJsonDocument* document,
                                   std::filesystem::path* base_directory,
                                   std::string* error_message) {
  if (document == nullptr || base_directory == nullptr ||
      error_message == nullptr) {
    return false;
  }

  MuonConfigPathResolution config_path;
  if (!ResolveMuonConfigPath(path, &config_path, error_message)) {
    return false;
  }
  if (!config_path.exists) {
    if (missing_is_success) {
      return true;
    }
    *error_message = "muon config does not exist: " + path.string();
    return false;
  }

  std::error_code error;
  if (!std::filesystem::is_regular_file(config_path.path, error) || error) {
    *error_message =
        "muon config is not a regular file: " + config_path.path.string();
    return false;
  }

  std::string json;
  if (!ReadTextFile(config_path.path, &json, error_message)) {
    return false;
  }

  yyjson_read_err read_error;
  document->value =
      yyjson_read_opts(json.data(), json.size(), kMuonConfigReadFlags, nullptr,
                       &read_error);
  if (document->value == nullptr) {
    *error_message = FormatJsonParseError(read_error);
    return false;
  }
  return GetMuonConfigBaseDirectory(config_path.path, base_directory,
                                    error_message);
}

static bool PutMergedJsonValue(yyjson_mut_doc* target_document,
                               yyjson_mut_val* target_object,
                               yyjson_val* source_key,
                               yyjson_val* source_value,
                               std::string* error_message) {
  const auto key = yyjson_get_str(source_key);
  const auto key_size = yyjson_get_len(source_key);
  const auto copied_key =
      yyjson_mut_strncpy(target_document, key, key_size);
  const auto copied_value =
      yyjson_val_mut_copy(target_document, source_value);
  if (copied_key == nullptr || copied_value == nullptr) {
    *error_message = "Failed to merge muon config JSON";
    return false;
  }
  if (!yyjson_mut_obj_put(target_object, copied_key, copied_value)) {
    *error_message = "Failed to merge muon config JSON";
    return false;
  }
  return true;
}

static bool AppendUniqueMergedArrayValue(yyjson_mut_doc* target_document,
                                         yyjson_mut_val* target_array,
                                         yyjson_val* source_value,
                                         std::string* error_message) {
  const auto copied_value = yyjson_val_mut_copy(target_document, source_value);
  if (copied_value == nullptr) {
    *error_message = "Failed to merge muon config JSON";
    return false;
  }

  size_t index;
  size_t max;
  yyjson_mut_val* value;
  yyjson_mut_arr_foreach(target_array, index, max, value) {
    if (yyjson_mut_equals(value, copied_value)) {
      return true;
    }
  }

  if (!yyjson_mut_arr_append(target_array, copied_value)) {
    *error_message = "Failed to merge muon config JSON";
    return false;
  }
  return true;
}

static bool MergeJsonArray(yyjson_mut_doc* target_document,
                           yyjson_mut_val* target_array,
                           yyjson_val* source_array,
                           std::string* error_message) {
  size_t index;
  size_t max;
  yyjson_val* value;
  yyjson_arr_foreach(source_array, index, max, value) {
    if (!AppendUniqueMergedArrayValue(target_document, target_array, value,
                                      error_message)) {
      return false;
    }
  }
  return true;
}

static bool MergeJsonObject(yyjson_mut_doc* target_document,
                            yyjson_mut_val* target_object,
                            yyjson_val* source_object,
                            const std::string& config_path,
                            std::string* error_message) {
  size_t index;
  size_t max;
  yyjson_val* key;
  yyjson_val* value;
  yyjson_obj_foreach(source_object, index, max, key, value) {
    const auto key_string = yyjson_get_str(key);
    const auto key_size = yyjson_get_len(key);
    const auto next_config_path =
        config_path.empty() ? std::string(key_string)
                            : config_path + "." + key_string;
    const auto target_value =
        yyjson_mut_obj_getn(target_object, key_string, key_size);
    if (yyjson_mut_is_obj(target_value) && yyjson_is_obj(value)) {
      if (!MergeJsonObject(target_document, target_value, value,
                           next_config_path, error_message)) {
        return false;
      }
      continue;
    }
    if (yyjson_mut_is_arr(target_value) && yyjson_is_arr(value)) {
      if (next_config_path == "plugin.pages" ||
          next_config_path == "plugin.plugins" ||
          next_config_path == "plugin.capabilities") {
        if (!PutMergedJsonValue(target_document, target_object, key, value,
                                error_message)) {
          return false;
        }
        continue;
      }
      if (!MergeJsonArray(target_document, target_value, value,
                          error_message)) {
        return false;
      }
      continue;
    }
    if (!PutMergedJsonValue(target_document, target_object, key, value,
                            error_message)) {
      return false;
    }
  }
  return true;
}

static bool ReadLauncherConfig(yyjson_val* root,
                                MuonConfig* config,
                                std::string* error_message) {
  config->default_version_policy = "tested";
  config->desktop_id = "muon";
  config->app_id = SanitizeAppId(GetStartupAppId());
  const auto launcher = yyjson_obj_get(root, kMuonConfigLauncherKey);
  if (launcher == nullptr) {
    return true;
  }
  if (!yyjson_is_obj(launcher)) {
    *error_message = "muon.json launcher must be an object";
    return false;
  }
  const auto desktop_id_value =
      yyjson_obj_get(launcher, kMuonConfigDesktopIdKey);
  if (desktop_id_value != nullptr) {
    if (!yyjson_is_str(desktop_id_value)) {
      *error_message = "muon.json launcher.desktopId must be a string";
      return false;
    }
    const auto desktop_id = TrimAscii(yyjson_get_str(desktop_id_value));
    if (desktop_id.empty()) {
      *error_message = "muon.json launcher.desktopId must not be empty";
      return false;
    }
    config->desktop_id = desktop_id;
  }

  const auto app_id_value = yyjson_obj_get(launcher, kMuonConfigAppIdKey);
  if (app_id_value != nullptr) {
    if (!yyjson_is_str(app_id_value)) {
      *error_message = "muon.json launcher.appId must be a string";
      return false;
    }
    const auto app_id = TrimAscii(yyjson_get_str(app_id_value));
    if (!app_id.empty()) {
      config->app_id = SanitizeAppId(app_id);
    }
  }

  const auto value =
      yyjson_obj_get(launcher, kMuonConfigDefaultVersionPolicyKey);
  if (value == nullptr) {
    return true;
  }
  if (!yyjson_is_str(value)) {
    *error_message =
        "muon.json launcher.defaultVersionPolicy must be a string";
    return false;
  }
  const std::string policy(yyjson_get_str(value));
  if (!IsValidCefVersionPolicy(policy)) {
    *error_message =
        "muon.json launcher.defaultVersionPolicy has unknown value: " +
        policy;
    return false;
  }
  config->default_version_policy = policy;
  return true;
}

static bool ReadMuonConfigRoot(yyjson_val* root,
                               MuonConfig* config,
                               std::string* error_message) {
  if (!yyjson_is_obj(root)) {
    *error_message = "muon.json root must be an object";
    return false;
  }
  return ReadLauncherConfig(root, config, error_message) &&
         ReadApplicationConfig(root, config, error_message) &&
         ReadAssetConfig(root, config, error_message) &&
         ReadLogConfig(root, config, error_message) &&
         ReadBrowserConfig(root, config, error_message) &&
         ReadNetworkConfig(root, config, error_message) &&
         ReadDebuggerConfig(root, config, error_message) &&
         ReadPluginConfig(root, config, error_message);
}

static bool LoadMuonConfigFromEmbeddedPayload(
    const uint8_t* payload,
    size_t payload_size,
    const std::filesystem::path& embedded_base_directory,
    MuonConfig* config,
    std::string* error_message) {
  if (payload == nullptr || config == nullptr || error_message == nullptr) {
    return false;
  }

  *config = MuonConfig();
  error_message->clear();

  MuonMutableJsonDocument mutable_document{yyjson_mut_doc_new(nullptr)};
  if (mutable_document.value == nullptr) {
    *error_message = "Failed to initialize embedded muon config JSON";
    return false;
  }

  MuonEmbeddedTlvReader reader{payload, payload_size, 0};
  std::vector<std::string> path;
  yyjson_mut_val* mutable_root = nullptr;
  if (!DecodeEmbeddedTlvValue(&reader, mutable_document.value, &path,
                              &mutable_root, error_message)) {
    return false;
  }
  yyjson_mut_doc_set_root(mutable_document.value, mutable_root);

  MuonJsonDocument immutable_document{
      yyjson_mut_doc_imut_copy(mutable_document.value, nullptr)};
  if (immutable_document.value == nullptr) {
    *error_message = "Failed to finalize embedded muon config JSON";
    return false;
  }

  const auto root = yyjson_doc_get_root(immutable_document.value);
  if (!ReadMuonConfigRoot(root, config, error_message)) {
    return false;
  }

  MuonConfigPathBases path_bases;
  UpdateConfigPathBases(root, embedded_base_directory, &path_bases);
  ResolveConfigPathsFromBases(root, path_bases, config);
  return ResolveDefaultConfigPathsFromBases(root, config, error_message);
}

static bool LoadMuonConfigPathSequence(
    const std::vector<std::filesystem::path>& paths,
    bool missing_is_success,
    MuonConfig* config,
    std::string* error_message) {
  if (config == nullptr || error_message == nullptr) {
    return false;
  }

  *config = MuonConfig();
  error_message->clear();

  MuonMutableJsonDocument merged_document{yyjson_mut_doc_new(nullptr)};
  if (merged_document.value == nullptr) {
    *error_message = "Failed to initialize muon config JSON";
    return false;
  }
  const auto merged_root = yyjson_mut_obj(merged_document.value);
  if (merged_root == nullptr) {
    *error_message = "Failed to initialize muon config JSON";
    return false;
  }
  yyjson_mut_doc_set_root(merged_document.value, merged_root);

  MuonConfigPathBases path_bases;
  for (const auto& path : paths) {
    MuonJsonDocument document;
    std::filesystem::path base_directory;
    if (!ReadMuonConfigDocument(path, missing_is_success, &document,
                                &base_directory, error_message)) {
      return false;
    }
    if (document.value == nullptr) {
      continue;
    }
    const auto root = yyjson_doc_get_root(document.value);
    if (!yyjson_is_obj(root)) {
      *error_message = "muon.json root must be an object";
      return false;
    }
    UpdateConfigPathBases(root, base_directory, &path_bases);
    if (!MergeJsonObject(merged_document.value, merged_root, root, "",
                         error_message)) {
      return false;
    }
  }

  MuonJsonDocument immutable_document{
      yyjson_mut_doc_imut_copy(merged_document.value, nullptr)};
  if (immutable_document.value == nullptr) {
    *error_message = "Failed to finalize muon config JSON";
    return false;
  }

  const auto root = yyjson_doc_get_root(immutable_document.value);
  if (!ReadMuonConfigRoot(root, config, error_message)) {
    return false;
  }
  ResolveConfigPathsFromBases(root, path_bases, config);
  return ResolveDefaultConfigPathsFromBases(root, config, error_message);
}

std::filesystem::path GetDefaultMuonConfigPath() {
  return GetMuonExecutableDirectory() / kMuonConfigFileName;
}

bool LoadMuonConfig(const std::filesystem::path& path,
                    MuonConfig* config,
                    std::string* error_message) {
  return LoadMuonConfigPathSequence({path}, true, config, error_message);
}

bool GetMuonConfigPathsFromCommandLine(
    const std::vector<std::string>& command_line,
    std::vector<std::filesystem::path>* config_paths,
    std::string* error_message) {
  if (config_paths == nullptr || error_message == nullptr) {
    return false;
  }

  config_paths->clear();
  error_message->clear();
  for (auto index = size_t{1}; index < command_line.size(); ++index) {
    if (command_line[index] != "-c") {
      continue;
    }
    if (index + 1 >= command_line.size()) {
      *error_message = "muon -c requires a path";
      return false;
    }

    std::filesystem::path config_path;
    if (!ResolveCommandLineConfigPath(command_line[index + 1], &config_path,
                                      error_message)) {
      return false;
    }
    config_paths->push_back(std::move(config_path));
    ++index;
  }
  return true;
}

bool LoadMuonConfigFiles(const std::vector<std::filesystem::path>& paths,
                         MuonConfig* config,
                         std::string* error_message) {
  return LoadMuonConfigPathSequence(paths, false, config, error_message);
}

bool LoadDefaultMuonConfig(MuonConfig* config, std::string* error_message) {
  return LoadMuonConfig(GetDefaultMuonConfigPath(), config, error_message);
}

uint8_t GetMuonEmbeddedConfigEmptySlotByte(size_t index) {
  return CalculateMuonEmbeddedConfigEmptySlotByte(index);
}

bool CreateMuonEmbeddedConfigSlot(const std::vector<uint8_t>& payload,
                                  std::vector<uint8_t>* slot,
                                  std::string* error_message) {
  if (slot == nullptr || error_message == nullptr) {
    return false;
  }
  error_message->clear();
  if (payload.size() > kMuonEmbeddedConfigPayloadCapacity) {
    *error_message = "Embedded muon config payload exceeds slot capacity";
    return false;
  }

  slot->resize(kMuonEmbeddedConfigSlotSize);
  for (auto index = size_t{0}; index < slot->size(); ++index) {
    (*slot)[index] = CalculateMuonEmbeddedConfigEmptySlotByte(index);
  }
  for (auto index = size_t{0}; index < payload.size(); ++index) {
    (*slot)[index] = payload[index];
  }
  return true;
}

bool LoadMuonStartupConfigFromEmbeddedSlot(
    const std::vector<std::string>& command_line,
    const uint8_t* slot,
    size_t slot_size,
    const std::filesystem::path& embedded_base_directory,
    MuonConfig* config,
    std::vector<std::filesystem::path>* config_paths,
    bool* embedded,
    std::string* error_message) {
  if (config == nullptr || config_paths == nullptr || embedded == nullptr ||
      error_message == nullptr) {
    return false;
  }
  config_paths->clear();
  *embedded = false;
  error_message->clear();

  MuonEmbeddedConfigSlotState slot_state;
  if (!InspectMuonEmbeddedConfigSlot(slot, slot_size, &slot_state,
                                     error_message)) {
    return false;
  }
  if (slot_state.embedded) {
    *embedded = true;
    return LoadMuonConfigFromEmbeddedPayload(
        slot, kMuonEmbeddedConfigPayloadCapacity, embedded_base_directory,
        config, error_message);
  }

  if (!GetMuonConfigPathsFromCommandLine(command_line, config_paths,
                                         error_message)) {
    return false;
  }
  return config_paths->empty()
             ? LoadDefaultMuonConfig(config, error_message)
             : LoadMuonConfigFiles(*config_paths, config, error_message);
}

bool LoadMuonStartupConfig(
    const std::vector<std::string>& command_line,
    MuonConfig* config,
    std::vector<std::filesystem::path>* config_paths,
    bool* embedded,
    std::string* error_message) {
  return LoadMuonStartupConfigFromEmbeddedSlot(
      command_line, kMuonEmbeddedConfigSlot.data(),
      kMuonEmbeddedConfigSlot.size(), GetMuonExecutableDirectory(), config,
      config_paths, embedded, error_message);
}
