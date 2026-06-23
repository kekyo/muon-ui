// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

#ifndef MUON_BOOTSTRAP_PROGRESS_H
#define MUON_BOOTSTRAP_PROGRESS_H

#include "prepare_progress.h"

typedef struct {
  void *backend;
} MuonBootstrapProgress;

void muon_bootstrap_progress_init(MuonBootstrapProgress *progress);
int muon_bootstrap_progress_is_available(const MuonBootstrapProgress *progress);
void muon_bootstrap_progress_update(MuonBootstrapProgress *progress,
                                    const MuonPrepareProgress *event);
void muon_bootstrap_progress_fail(MuonBootstrapProgress *progress);
void muon_bootstrap_progress_dispose(MuonBootstrapProgress *progress);

#endif
