// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

#ifndef MUON_LAUNCHER_PROGRESS_H
#define MUON_LAUNCHER_PROGRESS_H

#include "prepare_progress.h"

typedef struct {
  void *backend;
} MuonLauncherProgress;

void muon_launcher_progress_init(MuonLauncherProgress *progress);
int muon_launcher_progress_is_available(const MuonLauncherProgress *progress);
void muon_launcher_progress_update(MuonLauncherProgress *progress,
                                    const MuonPrepareProgress *event);
void muon_launcher_progress_fail(MuonLauncherProgress *progress);
void muon_launcher_progress_dispose(MuonLauncherProgress *progress);

#endif
