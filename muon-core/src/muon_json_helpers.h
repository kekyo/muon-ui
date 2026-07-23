/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once

#include "yyjson.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace muon_internal {

/**
 * RAII owner for an immutable yyjson document.
 */
class MuonJsonDocument final {
 public:
  /**
   * Creates an empty document owner.
   */
  MuonJsonDocument();

  /**
   * Takes ownership of an immutable yyjson document.
   */
  explicit MuonJsonDocument(yyjson_doc* source);

  /**
   * Releases the owned yyjson document.
   */
  ~MuonJsonDocument();

  MuonJsonDocument(const MuonJsonDocument&) = delete;
  MuonJsonDocument& operator=(const MuonJsonDocument&) = delete;

  /**
   * Moves document ownership from another owner.
   */
  MuonJsonDocument(MuonJsonDocument&& other) noexcept;

  /**
   * Replaces this owner with another document owner.
   */
  MuonJsonDocument& operator=(MuonJsonDocument&& other) noexcept;

  /**
   * Returns the owned document pointer.
   */
  yyjson_doc* get() const;

  /**
   * Replaces the owned document pointer.
   */
  void reset(yyjson_doc* source);

  /**
   * Owned yyjson document pointer.
   *
   * @remarks Kept public for existing internal call sites that inspect yyjson
   * ownership directly.
   */
  yyjson_doc* value = nullptr;
};

/**
 * RAII owner for a mutable yyjson document.
 */
class MuonMutableJsonDocument final {
 public:
  /**
   * Creates an empty mutable document owner.
   */
  MuonMutableJsonDocument();

  /**
   * Takes ownership of a mutable yyjson document.
   */
  explicit MuonMutableJsonDocument(yyjson_mut_doc* source);

  /**
   * Releases the owned mutable yyjson document.
   */
  ~MuonMutableJsonDocument();

  MuonMutableJsonDocument(const MuonMutableJsonDocument&) = delete;
  MuonMutableJsonDocument& operator=(const MuonMutableJsonDocument&) = delete;

  /**
   * Moves mutable document ownership from another owner.
   */
  MuonMutableJsonDocument(MuonMutableJsonDocument&& other) noexcept;

  /**
   * Replaces this owner with another mutable document owner.
   */
  MuonMutableJsonDocument& operator=(
      MuonMutableJsonDocument&& other) noexcept;

  /**
   * Returns the owned mutable document pointer.
   */
  yyjson_mut_doc* get() const;

  /**
   * Replaces the owned mutable document pointer.
   */
  void reset(yyjson_mut_doc* source);

  /**
   * Owned mutable yyjson document pointer.
   *
   * @remarks Kept public for existing internal call sites that inspect yyjson
   * ownership directly.
   */
  yyjson_mut_doc* value = nullptr;
};

/**
 * Copies a yyjson string value while preserving embedded NUL bytes.
 */
std::string ReadJsonString(yyjson_val* value);

/**
 * Appends a JSON string literal to the target.
 */
void AppendJsonString(std::string* target, std::string_view value);

/**
 * Creates a JSON array containing string values.
 */
std::string CreateJsonStringArray(const std::vector<std::string>& values);

/**
 * Creates a JSON object containing string key-value entries.
 */
std::string CreateJsonStringObject(
    const std::vector<std::pair<std::string, std::string>>& entries);

/**
 * Parses options JSON whose root must be an object.
 */
bool ParseJsonObjectOptions(const char* options_json,
                            MuonJsonDocument* document,
                            yyjson_val** root,
                            std::string* error_message);

}  // namespace muon_internal
