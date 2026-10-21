/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

/** One bounded HTTP request, with duplicate headers retained for policy checks. */
struct MuonServerHttpRequest {
  /** HTTP method. */
  std::string method;
  /** Request target as sent by the client. */
  std::string target;
  /** Initial headers; trailers cannot replace authentication headers. */
  std::vector<std::pair<std::string, std::string>> headers;
  /** Decoded request body, limited to 1 MiB. */
  std::vector<uint8_t> body;
  /** Whether HTTP requested a protocol upgrade. */
  bool upgrade = false;
};

/** Result of consuming an input fragment. */
struct MuonServerHttpParseResult {
  /** Bytes consumed from this fragment; upgrade bytes remain with the caller. */
  size_t consumed;
  /** Zero on success, otherwise the HTTP error to send (400, 413 or 431). */
  int error_status;
  /** Present only when exactly one complete request has been parsed. */
  std::shared_ptr<const MuonServerHttpRequest> request;
};

/** Incremental strict HTTP/1 request parser. It stops after the first message. */
struct MuonServerHttpParser {
  /** Consumes a borrowed input fragment, retaining only bounded request data. */
  std::function<MuonServerHttpParseResult(std::span<const uint8_t>)> consume;
  /** Checks that EOF does not truncate the request. */
  std::function<MuonServerHttpParseResult()> finish;
};

/** Creates a parser with 32 KiB headers, an 8 KiB target and a 1 MiB body limit. */
std::shared_ptr<MuonServerHttpParser> CreateMuonServerHttpParser();
