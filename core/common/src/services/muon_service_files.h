/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 */
#pragma once
#include "services/muon_service_io.h"
#include <filesystem>
#include <stdexcept>

/** A stable SDK error produced by a file operation. */
struct MuonServiceIoError : std::runtime_error {
  std::string code; /**< Public framework code. */
  /** @param code Public code. @param message Diagnostic. */
  MuonServiceIoError(std::string code, const std::string& message) : std::runtime_error(message), code(std::move(code)) {}
};

/** @param directory Host data root. @param policy Serialized registry grants. @param request Owned request.
 * @param cancelled Worker-visible generation or operation cancellation. @return Metadata and bytes.
 * @remarks Called only from a platform I/O pool. */
MuonServiceIoReply RunMuonServiceFile(const std::string& directory, const std::string& policy,
    const MuonServiceIoRequest& request, const std::atomic_bool& cancelled);
