/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#pragma once
#include <cardio.h>
#include <functional>
#include <string>

/** Absolute paths and the private endpoint for an independent service host. */
struct MuonServiceProcessOptions {
  std::string executable; /**< Host executable, without shell interpretation. */
  std::string registry; /**< Application service registry JSON. */
  std::string endpoint; /**< Unix socket path or Windows named pipe name. */
  std::string data_root; /**< Private persistent service data directory. */
  std::string startup{}; /**< Trusted login command metadata, empty for standalone drivers. */
};

/** Non-owning startup connection; destroying it never kills resident services. */
struct MuonServiceProcess {
  /** Closes and drains startup or joined transport I/O; repeated calls never kill the host. */
  std::function<cardio::promise<void>()> detach;
};

/**
 * Joins an existing authenticated host or launches one and waits for readiness.
 * @param options Absolute executable/config/data paths and authenticated endpoint.
 * @param cancellation Cancels lock/readiness waits and drains startup I/O.
 * @return Non-owning connection retained until the caller establishes its session.
 * @remarks Concurrent launches for the same endpoint are serialized. An existing
 * host is joined through an authenticated transport; a new host receives a
 * --bootstrap stdin pipe. Closing either connection permits an unused host to
 * exit. Resident references and page connections retain their independent lives.
 * A spawned host inherits no unrelated descriptors; stderr carries diagnostics.
 */
cardio::promise<MuonServiceProcess> LaunchMuonServiceProcess(
    MuonServiceProcessOptions options, cardio::cancellation cancellation);
