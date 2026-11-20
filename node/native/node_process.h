/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once
#include <cardio.h>
#include <functional>
#include <span>
#include <string>
#include <vector>
#if defined(_WIN32)
#include "muon_windows_job_process.h"
#else
#include <sys/types.h>
#endif

/** One child and its transport, exclusively owned by a dispatcher-bound caller.
 * All pending I/O and the process monitor must drain before closing its handles.
 * The service host supplies lifecycle policy. */
struct MuonNodeProcess {
  std::string executable_path; /**< Absolute Node executable path. */
  std::string bridge_path; /**< Absolute bundled bridge entry. */
  std::string session_token; /**< Fresh random handshake secret. */
  cardio::cancellation_source io_cancellation; /**< Cancels reads and connection. */
  bool process_started = false; /**< OS child was successfully created. */
  bool process_exited = false; /**< Owner observed and reaped child termination. */
  // Shutdown RPCs are valid only after the child completed its IPC handshake.
  bool transport_connected = false; /**< Child authenticated as the pipe peer. */
#if defined(_WIN32)
  HANDLE pipe = INVALID_HANDLE_VALUE; /**< Private overlapped named pipe. */
  MuonWindowsJobProcess process{}; /**< Child job, killed when owner closes it. */
#else
  int socket_fd = -1; /**< Nonblocking inherited socketpair endpoint. */
  int process_fd = -1; /**< Readiness notification for child exit. */
  pid_t process_id = -1; /**< Direct child identity for waitpid. */
#endif
};

/** Observed child result, including termination signals as 128 + signal. */
struct MuonNodeProcessExit {
  int exit_code; /**< Numeric OS result, or -1 when unavailable. */
  bool available; /**< Whether the OS supplied a terminal result. */
};

/** @returns A 256-bit secret from the operating system random source. */
std::string CreateMuonNodeSessionToken();
/** Starts the child and establishes its private transport.
 * @param process Owned child state and immutable launch paths.
 * @param spawned Invoked after child creation, before connection waits; starts
 * the owner's process monitor so pre-connect exits cancel connection promptly.
 * @remarks The owner must clean up even if startup throws after spawning. */
cardio::promise<void> StartMuonNodeProcess(MuonNodeProcess* process,
    std::function<void()> spawned);
/** @returns Terminal result after asynchronous OS notification and reaping. */
cardio::promise<MuonNodeProcessExit> WaitMuonNodeProcess(MuonNodeProcess* process);
/** @returns Length-prefixed frame; rejects payloads over 16 MiB. */
std::vector<std::byte> CreateMuonNodeFrame(const std::string& json);
/** Fills a borrowed buffer or throws on EOF; buffer survives until completion. */
cardio::promise<void> ReadMuonNodeExact(MuonNodeProcess* process, std::span<std::byte> buffer);
/** Writes a borrowed buffer or throws on EOF; buffer survives until completion. */
cardio::promise<void> WriteMuonNodeExact(MuonNodeProcess* process, std::span<const std::byte> buffer);
/** Closes transport after I/O drainage, idempotently. */
void CloseMuonNodeTransport(MuonNodeProcess* process);
/** Releases handles after the process monitor completes, idempotently. */
void CloseMuonNodeProcessHandles(MuonNodeProcess* process);
/** Requests termination; force selects immediate termination instead of SIGTERM. */
void TerminateMuonNodeProcess(MuonNodeProcess* process, bool force);
