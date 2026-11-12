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
};

/** Non-owning startup connection; destroying it never kills resident services. */
struct MuonServiceProcess {
  /** Closes and drains bootstrap I/O; repeated calls never wait for or kill the host. */
  std::function<cardio::promise<void>()> detach;
};

/**
 * Launches an independent host and waits for its ready output.
 * @param options Absolute executable/config/data paths and authenticated endpoint.
 * @param cancellation Cancels readiness, closes bootstrap input, and drains I/O.
 * @return Bootstrap connection to retain while the UI owns the initial launch.
 * @remarks The host reads --bootstrap stdin. EOF permits an unused host to exit;
 * resident references and page connections retain their own independent lives.
 * The host inherits no unrelated descriptors. stderr is inherited for diagnostics.
 */
cardio::promise<MuonServiceProcess> LaunchMuonServiceProcess(
    MuonServiceProcessOptions options, cardio::cancellation cancellation);
