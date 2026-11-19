/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once

#include "services/muon_service_value.h"

#include <map>

struct MuonServiceSignature;

/** Validated runtime type shared by native and JavaScript service adapters. */
struct MuonServiceType {
  muon_service_value_kind kind; /**< Portable value kind; no pointers. */
  std::shared_ptr<const MuonServiceType> element; /**< Array element, otherwise null. */
  std::shared_ptr<const MuonServiceSignature> callback; /**< Callback signature, otherwise null. */
};

/** Ordered input and result description of a method or invocation callback. */
struct MuonServiceSignature {
  std::vector<MuonServiceType> parameters; /**< At most 32 arguments. */
  MuonServiceType returns; /**< Result type, including void. */
};

/** Public method signature and its declared application failures. */
struct MuonServiceMethod {
  MuonServiceSignature signature; /**< Arguments and result. */
  std::vector<std::string> errors; /**< Unique application error codes. */
};

/** Detached interface metadata for one service implementation. */
struct MuonServiceInterface {
  std::map<std::string, MuonServiceMethod> methods; /**< Named callable methods. */
  std::map<std::string, MuonServiceType> events; /**< Named subscription payloads. */
};

/**
 * Validates version 1 interface JSON and copies all metadata.
 * @param json UTF-8 interface with schemaVersion, methods, and events.
 * @return Owned type descriptions, independent of the source text.
 * @throws std::invalid_argument For unsupported or excessive metadata.
 * @remarks Callbacks are limited to method inputs. Their signatures cannot
 *     themselves contain callbacks. Methods and events are limited to 256 each,
 *     argument lists to 32, error lists to 64, and array nesting to 16 levels.
 */
MuonServiceInterface ParseMuonServiceInterface(const std::string& json);

/**
 * Decodes one value against a validated type, preserving integer widths.
 * @param type Validated interface type.
 * @param json JSON wire value; 64-bit integers and floats use decimal strings.
 * @return Owned data, with callback tokens still scoped by the caller.
 * @throws std::invalid_argument For type mismatch, malformed data or limits.
 */
MuonServiceValue DecodeMuonServiceValue(const MuonServiceType& type,
                                      const std::string& json);

/**
 * Encodes an owned value against a validated interface type.
 * @param type Validated interface type.
 * @param value Owned data to check and encode.
 * @return JSON wire value with lossless numeric and binary representations.
 * @throws std::invalid_argument For type mismatch, malformed data or limits.
 */
std::string EncodeMuonServiceValue(const MuonServiceType& type,
                                   const MuonServiceValue& value);

/**
 * Decodes a complete argument list with a shared payload budget.
 * @param types Validated ordered parameter descriptions.
 * @param json JSON array containing exactly one value per parameter.
 * @return Owned arguments, limited together to 1 MiB and 65536 value nodes.
 * @throws std::invalid_argument For a mismatch, malformed data or limits.
 */
std::vector<MuonServiceValue> DecodeMuonServiceArguments(
    const std::vector<MuonServiceType>& types, const std::string& json);

/**
 * Encodes a complete argument list with a shared payload budget.
 * @param types Validated ordered parameter descriptions.
 * @param values Exactly one owned value per parameter.
 * @return JSON argument array.
 * @throws std::invalid_argument For a mismatch, malformed data or limits.
 */
std::string EncodeMuonServiceArguments(const std::vector<MuonServiceType>& types,
                                      const std::vector<MuonServiceValue>& values);

/** @param code Service failure identifier. @return Whether any method may report this framework error. */
bool IsMuonServiceFrameworkError(const std::string& code);
