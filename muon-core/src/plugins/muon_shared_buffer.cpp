/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "plugins/muon_shared_buffer.h"

#include "include/cef_shared_process_message_builder.h"

#include <charconv>
#include <cstdint>
#include <cstring>
#include <limits>
#include <system_error>

static constexpr uint32_t kMuonSharedBufferMagic = 0x424e4c47u;
static constexpr uint32_t kMuonSharedBufferVersion = 2u;
static constexpr char kMuonSharedBufferKindKey[] = "kind";
static constexpr char kMuonSharedBufferKindValue[] = "shared_buffer";
static constexpr char kMuonSharedBufferIndexKey[] = "index";
static constexpr char kMuonSharedBufferOffsetKey[] = "offset";
static constexpr char kMuonSharedBufferSizeKey[] = "size";
static constexpr size_t kMuonSharedBufferHeaderSize =
    sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint64_t) +
    sizeof(uint64_t) + sizeof(uint64_t);
static constexpr size_t kMuonSharedBufferEntrySize =
    sizeof(uint64_t) + sizeof(uint64_t) + sizeof(uint64_t);

static bool AddMuonSize(size_t left, size_t right, size_t* result) {
  if (result == nullptr ||
      left > std::numeric_limits<size_t>::max() - right) {
    return false;
  }
  *result = left + right;
  return true;
}

static bool MultiplyMuonSize(size_t left, size_t right, size_t* result) {
  if (result == nullptr ||
      (left != 0 && right > std::numeric_limits<size_t>::max() / left)) {
    return false;
  }
  *result = left * right;
  return true;
}

static bool WriteMuonU32(uint8_t* memory,
                          size_t memory_size,
                          size_t* cursor,
                          uint32_t value) {
  if (cursor == nullptr || *cursor > memory_size ||
      memory_size - *cursor < sizeof(value)) {
    return false;
  }
  std::memcpy(memory + *cursor, &value, sizeof(value));
  *cursor += sizeof(value);
  return true;
}

static bool WriteMuonU64(uint8_t* memory,
                          size_t memory_size,
                          size_t* cursor,
                          uint64_t value) {
  if (cursor == nullptr || *cursor > memory_size ||
      memory_size - *cursor < sizeof(value)) {
    return false;
  }
  std::memcpy(memory + *cursor, &value, sizeof(value));
  *cursor += sizeof(value);
  return true;
}

static bool ReadMuonU32(const uint8_t* memory,
                         size_t memory_size,
                         size_t* cursor,
                         uint32_t* value) {
  if (cursor == nullptr || value == nullptr || *cursor > memory_size ||
      memory_size - *cursor < sizeof(*value)) {
    return false;
  }
  std::memcpy(value, memory + *cursor, sizeof(*value));
  *cursor += sizeof(*value);
  return true;
}

static bool ReadMuonU64(const uint8_t* memory,
                         size_t memory_size,
                         size_t* cursor,
                         uint64_t* value) {
  if (cursor == nullptr || value == nullptr || *cursor > memory_size ||
      memory_size - *cursor < sizeof(*value)) {
    return false;
  }
  std::memcpy(value, memory + *cursor, sizeof(*value));
  *cursor += sizeof(*value);
  return true;
}

static bool ParseMuonU64String(const std::string& source, uint64_t* value) {
  if (value == nullptr || source.empty()) {
    return false;
  }
  const auto begin = source.data();
  const auto end = begin + source.size();
  auto parsed = uint64_t{0};
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc() || result.ptr != end) {
    return false;
  }
  *value = parsed;
  return true;
}

static bool ValidateMuonSharedBufferEntryRange(
    const MuonSharedBufferPayload& payload,
    const MuonSharedBufferEntry& entry) {
  if (!payload.region || !payload.region->IsValid()) {
    return false;
  }
  const auto region_size = payload.region->Size();
  return entry.offset <= region_size && entry.size <= region_size &&
         entry.size <= region_size - entry.offset;
}

size_t GetMuonSharedBufferHeaderSize() {
  return kMuonSharedBufferHeaderSize;
}

size_t GetMuonSharedBufferEntrySize() {
  return kMuonSharedBufferEntrySize;
}

size_t GetMuonSharedBufferSingleEntryDataOffset() {
  return kMuonSharedBufferHeaderSize + kMuonSharedBufferEntrySize;
}

bool GetMuonSharedBufferSingleEntryPayloadSize(size_t data_size,
                                                size_t* payload_size) {
  return AddMuonSize(GetMuonSharedBufferSingleEntryDataOffset(), data_size,
                      payload_size);
}

bool WriteMuonSharedBufferPayloadHeader(
    void* memory,
    size_t memory_size,
    int call_id,
    int renderer_context_id,
    const std::vector<MuonSharedBufferEntry>& entries,
    std::string* error_message) {
  if (memory == nullptr || call_id < 0 || renderer_context_id < 0) {
    *error_message = "Shared buffer payload is invalid";
    return false;
  }
  auto entry_bytes = size_t{0};
  auto required_size = size_t{0};
  if (!MultiplyMuonSize(entries.size(), kMuonSharedBufferEntrySize,
                         &entry_bytes) ||
      !AddMuonSize(kMuonSharedBufferHeaderSize, entry_bytes,
                    &required_size) ||
      required_size > memory_size) {
    *error_message = "Shared buffer payload header is too large";
    return false;
  }

  auto* bytes = static_cast<uint8_t*>(memory);
  auto cursor = size_t{0};
  if (!WriteMuonU32(bytes, memory_size, &cursor, kMuonSharedBufferMagic) ||
      !WriteMuonU32(bytes, memory_size, &cursor,
                     kMuonSharedBufferVersion) ||
      !WriteMuonU64(bytes, memory_size, &cursor,
                     static_cast<uint64_t>(call_id)) ||
      !WriteMuonU64(bytes, memory_size, &cursor,
                     static_cast<uint64_t>(renderer_context_id)) ||
      !WriteMuonU64(bytes, memory_size, &cursor,
                     static_cast<uint64_t>(entries.size()))) {
    *error_message = "Failed to write shared buffer payload header";
    return false;
  }

  for (const auto& entry : entries) {
    if (entry.offset > memory_size || entry.size > memory_size ||
        entry.size > memory_size - entry.offset) {
      *error_message = "Shared buffer entry is out of range";
      return false;
    }
    if (!WriteMuonU64(bytes, memory_size, &cursor,
                       static_cast<uint64_t>(entry.value_index)) ||
        !WriteMuonU64(bytes, memory_size, &cursor,
                       static_cast<uint64_t>(entry.offset)) ||
        !WriteMuonU64(bytes, memory_size, &cursor,
                       static_cast<uint64_t>(entry.size))) {
      *error_message = "Failed to write shared buffer payload entry";
      return false;
    }
  }
  return true;
}

bool CreateMuonSharedBufferMessage(
    const std::string& message_name,
    int call_id,
    int renderer_context_id,
    const std::vector<MuonSharedBufferSource>& sources,
    MuonCreatedSharedBufferMessage* created_message,
    std::string* error_message) {
  if (created_message == nullptr || error_message == nullptr) {
    return false;
  }
  created_message->message = nullptr;
  created_message->entries.clear();
  if (sources.empty()) {
    *error_message = "Shared buffer payload has no entries";
    return false;
  }

  auto entry_bytes = size_t{0};
  auto total_size = size_t{0};
  if (!MultiplyMuonSize(sources.size(), kMuonSharedBufferEntrySize,
                         &entry_bytes) ||
      !AddMuonSize(kMuonSharedBufferHeaderSize, entry_bytes, &total_size)) {
    *error_message = "Shared buffer payload is too large";
    return false;
  }

  created_message->entries.reserve(sources.size());
  for (const auto& source : sources) {
    if (source.size > 0 && source.data == nullptr) {
      *error_message = "Shared buffer source data is null";
      return false;
    }
    MuonSharedBufferEntry entry;
    entry.value_index = source.value_index;
    entry.offset = total_size;
    entry.size = source.size;
    created_message->entries.push_back(entry);
    if (!AddMuonSize(total_size, source.size, &total_size)) {
      *error_message = "Shared buffer payload is too large";
      return false;
    }
  }

  const auto builder =
      CefSharedProcessMessageBuilder::Create(message_name, total_size);
  if (!builder || !builder->IsValid() || builder->Size() != total_size ||
      builder->Memory() == nullptr) {
    *error_message = "Failed to allocate shared buffer payload";
    return false;
  }

  if (!WriteMuonSharedBufferPayloadHeader(
          builder->Memory(), builder->Size(), call_id, renderer_context_id,
          created_message->entries, error_message)) {
    return false;
  }

  auto* memory = static_cast<uint8_t*>(builder->Memory());
  for (auto index = size_t{0}; index < sources.size(); ++index) {
    const auto& source = sources[index];
    const auto& entry = created_message->entries[index];
    if (entry.size > 0) {
      std::memcpy(memory + entry.offset, source.data, entry.size);
    }
  }

  created_message->message = builder->Build();
  if (!created_message->message) {
    *error_message = "Failed to build shared buffer payload";
    return false;
  }
  return true;
}

bool ReadMuonSharedBufferPayloadMetadata(
    CefRefPtr<CefProcessMessage> message,
    int* call_id,
    int* renderer_context_id,
    std::string* error_message) {
  if (call_id == nullptr || renderer_context_id == nullptr ||
      error_message == nullptr) {
    return false;
  }
  const auto region = message ? message->GetSharedMemoryRegion() : nullptr;
  if (!region || !region->IsValid() || region->Memory() == nullptr ||
      region->Size() < kMuonSharedBufferHeaderSize) {
    *error_message = "Shared buffer payload is missing";
    return false;
  }

  const auto* memory = static_cast<const uint8_t*>(region->Memory());
  const auto memory_size = region->Size();
  auto cursor = size_t{0};
  auto magic = uint32_t{0};
  auto version = uint32_t{0};
  auto raw_call_id = uint64_t{0};
  auto raw_context_id = uint64_t{0};
  auto entry_count = uint64_t{0};
  if (!ReadMuonU32(memory, memory_size, &cursor, &magic) ||
      !ReadMuonU32(memory, memory_size, &cursor, &version) ||
      !ReadMuonU64(memory, memory_size, &cursor, &raw_call_id) ||
      !ReadMuonU64(memory, memory_size, &cursor, &raw_context_id) ||
      !ReadMuonU64(memory, memory_size, &cursor, &entry_count) ||
      magic != kMuonSharedBufferMagic ||
      version != kMuonSharedBufferVersion ||
      raw_call_id > static_cast<uint64_t>(std::numeric_limits<int>::max()) ||
      raw_context_id >
          static_cast<uint64_t>(std::numeric_limits<int>::max())) {
    *error_message = "Shared buffer payload header is invalid";
    return false;
  }

  *call_id = static_cast<int>(raw_call_id);
  *renderer_context_id = static_cast<int>(raw_context_id);
  return true;
}

bool DecodeMuonSharedBufferPayload(
    CefRefPtr<CefProcessMessage> message,
    int* call_id,
    std::shared_ptr<MuonSharedBufferPayload>* payload,
    std::string* error_message) {
  if (call_id == nullptr || payload == nullptr || error_message == nullptr) {
    return false;
  }
  *payload = nullptr;
  const auto region = message ? message->GetSharedMemoryRegion() : nullptr;
  if (!region || !region->IsValid() || region->Memory() == nullptr ||
      region->Size() < kMuonSharedBufferHeaderSize) {
    *error_message = "Shared buffer payload is missing";
    return false;
  }

  const auto* memory = static_cast<const uint8_t*>(region->Memory());
  const auto memory_size = region->Size();
  auto cursor = size_t{0};
  auto magic = uint32_t{0};
  auto version = uint32_t{0};
  auto raw_call_id = uint64_t{0};
  auto raw_context_id = uint64_t{0};
  auto entry_count = uint64_t{0};
  if (!ReadMuonU32(memory, memory_size, &cursor, &magic) ||
      !ReadMuonU32(memory, memory_size, &cursor, &version) ||
      !ReadMuonU64(memory, memory_size, &cursor, &raw_call_id) ||
      !ReadMuonU64(memory, memory_size, &cursor, &raw_context_id) ||
      !ReadMuonU64(memory, memory_size, &cursor, &entry_count)) {
    *error_message = "Shared buffer payload header is invalid";
    return false;
  }
  if (magic != kMuonSharedBufferMagic ||
      version != kMuonSharedBufferVersion ||
      raw_call_id > static_cast<uint64_t>(std::numeric_limits<int>::max()) ||
      raw_context_id >
          static_cast<uint64_t>(std::numeric_limits<int>::max()) ||
      entry_count > static_cast<uint64_t>(
                        std::numeric_limits<size_t>::max())) {
    *error_message = "Shared buffer payload header is invalid";
    return false;
  }

  auto entry_bytes = size_t{0};
  auto required_size = size_t{0};
  if (!MultiplyMuonSize(static_cast<size_t>(entry_count),
                         kMuonSharedBufferEntrySize, &entry_bytes) ||
      !AddMuonSize(kMuonSharedBufferHeaderSize, entry_bytes,
                    &required_size) ||
      required_size > memory_size) {
    *error_message = "Shared buffer payload entries are invalid";
    return false;
  }

  auto decoded = std::make_shared<MuonSharedBufferPayload>();
  decoded->region = region;
  decoded->renderer_context_id = static_cast<int>(raw_context_id);
  decoded->entries.reserve(static_cast<size_t>(entry_count));
  for (auto index = size_t{0}; index < entry_count; ++index) {
    auto value_index = uint64_t{0};
    auto offset = uint64_t{0};
    auto size = uint64_t{0};
    if (!ReadMuonU64(memory, memory_size, &cursor, &value_index) ||
        !ReadMuonU64(memory, memory_size, &cursor, &offset) ||
        !ReadMuonU64(memory, memory_size, &cursor, &size) ||
        value_index > static_cast<uint64_t>(
                          std::numeric_limits<size_t>::max()) ||
        offset > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
        size > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
      *error_message = "Shared buffer payload entry is invalid";
      return false;
    }
    MuonSharedBufferEntry entry;
    entry.value_index = static_cast<size_t>(value_index);
    entry.offset = static_cast<size_t>(offset);
    entry.size = static_cast<size_t>(size);
    decoded->entries.push_back(entry);
    if (!ValidateMuonSharedBufferEntryRange(*decoded, entry)) {
      *error_message = "Shared buffer payload entry is out of range";
      return false;
    }
  }

  *call_id = static_cast<int>(raw_call_id);
  *payload = decoded;
  return true;
}

CefRefPtr<CefDictionaryValue> CreateMuonSharedBufferPlaceholder(
    const MuonSharedBufferEntry& entry) {
  auto dictionary = CefDictionaryValue::Create();
  dictionary->SetString(kMuonSharedBufferKindKey,
                        kMuonSharedBufferKindValue);
  dictionary->SetInt(kMuonSharedBufferIndexKey,
                     static_cast<int>(entry.value_index));
  dictionary->SetString(kMuonSharedBufferOffsetKey,
                        std::to_string(static_cast<uint64_t>(entry.offset)));
  dictionary->SetString(kMuonSharedBufferSizeKey,
                        std::to_string(static_cast<uint64_t>(entry.size)));
  return dictionary;
}

bool IsMuonSharedBufferPlaceholder(CefRefPtr<CefDictionaryValue> dictionary) {
  return dictionary &&
         dictionary->HasKey(kMuonSharedBufferKindKey) &&
         dictionary->GetString(kMuonSharedBufferKindKey).ToString() ==
             kMuonSharedBufferKindValue;
}

bool ReadMuonSharedBufferPlaceholder(CefRefPtr<CefDictionaryValue> dictionary,
                                      MuonSharedBufferEntry* entry) {
  if (entry == nullptr || !IsMuonSharedBufferPlaceholder(dictionary) ||
      !dictionary->HasKey(kMuonSharedBufferIndexKey) ||
      !dictionary->HasKey(kMuonSharedBufferOffsetKey) ||
      !dictionary->HasKey(kMuonSharedBufferSizeKey)) {
    return false;
  }
  const auto raw_index = dictionary->GetInt(kMuonSharedBufferIndexKey);
  if (raw_index < 0) {
    return false;
  }
  auto offset = uint64_t{0};
  auto size = uint64_t{0};
  if (!ParseMuonU64String(
          dictionary->GetString(kMuonSharedBufferOffsetKey).ToString(),
          &offset) ||
      !ParseMuonU64String(
          dictionary->GetString(kMuonSharedBufferSizeKey).ToString(),
          &size) ||
      offset > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
      size > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
    return false;
  }
  entry->value_index = static_cast<size_t>(raw_index);
  entry->offset = static_cast<size_t>(offset);
  entry->size = static_cast<size_t>(size);
  return true;
}

bool CefListValueHasMuonSharedBufferPlaceholders(
    CefRefPtr<CefListValue> values) {
  if (!values) {
    return false;
  }
  for (auto index = size_t{0}; index < values->GetSize(); ++index) {
    if (values->GetType(index) == VTYPE_DICTIONARY &&
        IsMuonSharedBufferPlaceholder(values->GetDictionary(index))) {
      return true;
    }
  }
  return false;
}

bool FindMuonSharedBufferEntry(const std::vector<MuonSharedBufferEntry>& entries,
                                size_t value_index,
                                MuonSharedBufferEntry* entry) {
  for (const auto& candidate : entries) {
    if (candidate.value_index == value_index) {
      if (entry != nullptr) {
        *entry = candidate;
      }
      return true;
    }
  }
  return false;
}

bool FindMuonSharedBufferEntry(const MuonSharedBufferPayload& payload,
                                size_t value_index,
                                MuonSharedBufferEntry* entry) {
  return FindMuonSharedBufferEntry(payload.entries, value_index, entry);
}

void* GetMuonSharedBufferEntryData(const MuonSharedBufferPayload& payload,
                                    const MuonSharedBufferEntry& entry) {
  if (entry.size == 0) {
    return nullptr;
  }
  if (!ValidateMuonSharedBufferEntryRange(payload, entry) ||
      payload.region->Memory() == nullptr) {
    return nullptr;
  }
  return static_cast<uint8_t*>(payload.region->Memory()) + entry.offset;
}
