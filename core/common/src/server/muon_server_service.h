/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#pragma once

#include <cardio.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

/** Owner-scoped listeners with independently releasable page handler bindings.
 * All operations run on the dispatcher that creates the service.
 */
struct MuonServerService {
  /** Executes a command with binary data and the host-authenticated page owner.
   * Returns a binary envelope: big-endian uint32 JSON length, JSON, raw body.
   */
  std::function<cardio::promise<std::vector<uint8_t>>(
      std::string, std::vector<uint8_t>, std::string)> dispatch;
  /** Releases this owner's handlers and asynchronously closes its listeners. */
  std::function<void(const std::string&)> release_owner;
  /** Stops accepting commands and awaits every listener and connection. */
  std::function<cardio::promise<void>()> stop;
};

/** Creates a server service on the current cardio dispatcher. @param valid_origin Verifies each serialized origin. */
std::shared_ptr<MuonServerService> CreateMuonServerService(std::function<bool(const std::string&)> valid_origin);
