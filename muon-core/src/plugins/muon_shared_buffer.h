/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once

#include "include/cef_process_message.h"
#include "include/cef_shared_memory_region.h"
#include "include/cef_values.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

/**
 * One byte range inside a shared buffer payload.
 */
struct MuonSharedBufferEntry {
  size_t value_index = 0;
  size_t offset = 0;
  size_t size = 0;
};

/**
 * Decoded shared buffer payload received from a process message.
 */
struct MuonSharedBufferPayload {
  CefRefPtr<CefSharedMemoryRegion> region;
  int renderer_context_id = 0;
  std::vector<MuonSharedBufferEntry> entries;
};

/**
 * Source byte range used to create a shared buffer payload.
 */
struct MuonSharedBufferSource {
  size_t value_index = 0;
  const void* data = nullptr;
  size_t size = 0;
};

/**
 * Shared-memory process message and its entry table.
 */
struct MuonCreatedSharedBufferMessage {
  CefRefPtr<CefProcessMessage> message;
  std::vector<MuonSharedBufferEntry> entries;
};

/**
 * Returns the fixed payload header size.
 */
size_t GetMuonSharedBufferHeaderSize();

/**
 * Returns the fixed payload entry size.
 */
size_t GetMuonSharedBufferEntrySize();

/**
 * Returns the byte offset where single-entry helper allocations expose data.
 */
size_t GetMuonSharedBufferSingleEntryDataOffset();

/**
 * Returns the total payload size for a single-entry helper allocation.
 */
bool GetMuonSharedBufferSingleEntryPayloadSize(size_t data_size,
                                                size_t* payload_size);

/**
 * Writes the fixed header and entry table into an existing shared region.
 */
bool WriteMuonSharedBufferPayloadHeader(
    void* memory,
    size_t memory_size,
    int call_id,
    int renderer_context_id,
    const std::vector<MuonSharedBufferEntry>& entries,
    std::string* error_message);

/**
 * Creates a shared-memory process message from source byte ranges.
 */
bool CreateMuonSharedBufferMessage(
    const std::string& message_name,
    int call_id,
    int renderer_context_id,
    const std::vector<MuonSharedBufferSource>& sources,
    MuonCreatedSharedBufferMessage* created_message,
    std::string* error_message);

/**
 * Reads call routing metadata from a shared-memory process message.
 */
bool ReadMuonSharedBufferPayloadMetadata(
    CefRefPtr<CefProcessMessage> message,
    int* call_id,
    int* renderer_context_id,
    std::string* error_message);

/**
 * Decodes and validates a shared-memory process message.
 */
bool DecodeMuonSharedBufferPayload(
    CefRefPtr<CefProcessMessage> message,
    int* call_id,
    std::shared_ptr<MuonSharedBufferPayload>* payload,
    std::string* error_message);

/**
 * Creates a metadata placeholder for one shared buffer entry.
 */
CefRefPtr<CefDictionaryValue> CreateMuonSharedBufferPlaceholder(
    const MuonSharedBufferEntry& entry);

/**
 * Reads a metadata placeholder for one shared buffer entry.
 */
bool ReadMuonSharedBufferPlaceholder(CefRefPtr<CefDictionaryValue> dictionary,
                                      MuonSharedBufferEntry* entry);

/**
 * Returns true when the dictionary is a shared buffer placeholder.
 */
bool IsMuonSharedBufferPlaceholder(CefRefPtr<CefDictionaryValue> dictionary);

/**
 * Returns true when a list contains at least one shared buffer placeholder.
 */
bool CefListValueHasMuonSharedBufferPlaceholders(
    CefRefPtr<CefListValue> values);

/**
 * Finds a decoded shared buffer entry by value index.
 */
bool FindMuonSharedBufferEntry(const std::vector<MuonSharedBufferEntry>& entries,
                                size_t value_index,
                                MuonSharedBufferEntry* entry);

/**
 * Finds a decoded shared buffer entry by value index.
 */
bool FindMuonSharedBufferEntry(const MuonSharedBufferPayload& payload,
                                size_t value_index,
                                MuonSharedBufferEntry* entry);

/**
 * Returns the byte pointer for an entry inside a decoded payload.
 */
void* GetMuonSharedBufferEntryData(const MuonSharedBufferPayload& payload,
                                    const MuonSharedBufferEntry& entry);
