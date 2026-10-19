// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#include "launcher_progress.h"

static int visible;

static void CALLBACK window_shown(HWINEVENTHOOK hook, DWORD event,
    HWND window, LONG object, LONG child, DWORD thread, DWORD time) {
  (void)hook; (void)event; (void)child; (void)thread; (void)time;
  wchar_t name[128];
  if (object == OBJID_WINDOW && window != NULL && IsWindowVisible(window) &&
      GetClassNameW(window, name, 128) != 0 &&
      !wcscmp(name, L"MuonLauncherProgressWindow")) {
    visible = 1;
    PostQuitMessage(0);
  }
}

int wmain(void) {
  // Observe the actual show event. A fast preparation must not make this test
  // pass by disposing the progress thread before it creates its window.
  HWINEVENTHOOK hook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW,
      NULL, window_shown, GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);
  if (hook == NULL) return 2;
  MuonLauncherProgress progress;
  muon_launcher_progress_init(&progress);
  if (!muon_launcher_progress_is_available(&progress)) {
    UnhookWinEvent(hook);
    return 3;
  }
  MuonPrepareProgress event = {0};
  event.phase = MUON_PREPARE_PROGRESS_PHASE_INSTALLING;
  event.status = "Preparing runtime";
  event.determinate = 1;
  event.current = 1;
  event.total = 2;
  muon_launcher_progress_update(&progress, &event);
  MSG message;
  while (GetMessageW(&message, NULL, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  muon_launcher_progress_dispose(&progress);
  UnhookWinEvent(hook);
  if (!visible) return 4;
  puts("progress window shown");
  return 0;
}
