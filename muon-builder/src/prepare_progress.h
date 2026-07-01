// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

#ifndef MUON_PREPARE_PROGRESS_H
#define MUON_PREPARE_PROGRESS_H

/**
 * High-level phase for a Muon runtime preparation progress event.
 */
typedef enum {
  /** The prepare helper is checking update metadata. */
  MUON_PREPARE_PROGRESS_PHASE_CHECKING = 0,
  /** The CEF runtime archive or catalog is being downloaded. */
  MUON_PREPARE_PROGRESS_PHASE_DOWNLOADING = 1,
  /** The downloaded CEF archive is being verified. */
  MUON_PREPARE_PROGRESS_PHASE_VERIFYING = 2,
  /** CEF runtime files are being installed into the runtime directory. */
  MUON_PREPARE_PROGRESS_PHASE_INSTALLING = 3,
  /** Preparation has finished and Muon is about to start. */
  MUON_PREPARE_PROGRESS_PHASE_FINALIZING = 4,
  /** Preparation has completed successfully. */
  MUON_PREPARE_PROGRESS_PHASE_DONE = 5,
  /** Preparation has failed. */
  MUON_PREPARE_PROGRESS_PHASE_FAILED = 6
} MuonPrepareProgressPhase;

/**
 * Progress event emitted while preparing a Muon runtime.
 */
typedef struct {
  /** Current high-level preparation phase. */
  MuonPrepareProgressPhase phase;
  /** Human-readable single-line status text. Valid only during callback. */
  const char *status;
  /** Current progress value when determinate is non-zero. */
  unsigned long long current;
  /** Maximum progress value when determinate is non-zero. */
  unsigned long long total;
  /** Non-zero when current and total describe bounded progress. */
  int determinate;
} MuonPrepareProgress;

/**
 * Receives a preparation progress event.
 *
 * @param progress Event information. The pointer and status string are valid
 * only for the duration of the callback.
 * @param user_data Opaque user data supplied by the caller.
 */
typedef void (*MuonPrepareProgressCallback)(
    const MuonPrepareProgress *progress, void *user_data);

#endif
