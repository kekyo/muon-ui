// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include <stdint.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#else
#include <pthread.h>
#include <time.h>
#include <xcb/xcb.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bootstrap_progress.h"

#define MUON_PROGRESS_WIDTH 360
#define MUON_PROGRESS_HEIGHT 110
#define MUON_PROGRESS_BAR_HEIGHT 6
#define MUON_PROGRESS_FRAME_MILLISECONDS 33
#define MUON_PROGRESS_FRAME_NANOSECONDS 33000000ULL
#define MUON_PROGRESS_PULSE_PERIOD_NANOSECONDS 1600000000ULL
#define MUON_PROGRESS_BACKGROUND_COLOR 0xc0c0c0

static unsigned long long progress_position(const MuonPrepareProgress *event,
                                            unsigned long long width) {
  if (!event->determinate || event->total == 0) {
    return 0;
  }
  if (event->current >= event->total) {
    return width;
  }
  return (event->current * width) / event->total;
}

#if !defined(_WIN32) || defined(MUON_BOOTSTRAP_PROGRESS_TEST)
static uint16_t pulse_position_from_elapsed(
    unsigned long long elapsed_nanoseconds, uint16_t range) {
  if (range == 0) {
    return 0;
  }
  const unsigned long long cycle =
      MUON_PROGRESS_PULSE_PERIOD_NANOSECONDS * 2ULL;
  const unsigned long long phase = elapsed_nanoseconds % cycle;
  const unsigned long long forward =
      phase <= MUON_PROGRESS_PULSE_PERIOD_NANOSECONDS
          ? phase
          : cycle - phase;
  return (uint16_t)((forward * range) /
                    MUON_PROGRESS_PULSE_PERIOD_NANOSECONDS);
}
#endif

#ifdef MUON_BOOTSTRAP_PROGRESS_TEST
unsigned short muon_bootstrap_progress_test_pulse_position(
    unsigned long long elapsed_nanoseconds, unsigned short range) {
  return pulse_position_from_elapsed(elapsed_nanoseconds, (uint16_t)range);
}
#endif

#ifdef _WIN32

typedef struct {
  HINSTANCE instance;
  HWND window;
  HWND status_label;
  HWND determinate_progress_bar;
  HWND marquee_progress_bar;
  HANDLE update_event;
  HANDLE thread;
  DWORD thread_id;
  CRITICAL_SECTION mutex;
  MuonPrepareProgress last_event;
  char status[256];
  int has_event;
  int stop_requested;
  int shown;
  int marquee_running;
} MuonBootstrapProgressBackend;

static LRESULT CALLBACK progress_window_proc(HWND window, UINT message,
                                             WPARAM wparam, LPARAM lparam) {
  switch (message) {
  case WM_CLOSE:
    return 0;
  case WM_CTLCOLORSTATIC:
    SetTextColor((HDC)wparam, GetSysColor(COLOR_WINDOWTEXT));
    SetBkColor((HDC)wparam, GetSysColor(COLOR_BTNFACE));
    return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
  default:
    break;
  }
  return DefWindowProcA(window, message, wparam, lparam);
}

static int register_progress_window_class(HINSTANCE instance) {
  WNDCLASSA window_class;
  memset(&window_class, 0, sizeof(window_class));
  window_class.lpfnWndProc = progress_window_proc;
  window_class.hInstance = instance;
  window_class.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
  window_class.lpszClassName = "MuonBootstrapProgressWindow";
  return RegisterClassA(&window_class) != 0 ||
         GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

static int ensure_window(MuonBootstrapProgressBackend *backend) {
  if (backend->shown) {
    return 1;
  }
  if (!register_progress_window_class(backend->instance)) {
    return 0;
  }
  INITCOMMONCONTROLSEX controls;
  controls.dwSize = sizeof(controls);
  controls.dwICC = ICC_PROGRESS_CLASS;
  const int has_progress_control = InitCommonControlsEx(&controls);
  const int x =
      (GetSystemMetrics(SM_CXSCREEN) - MUON_PROGRESS_WIDTH) / 2;
  const int y =
      (GetSystemMetrics(SM_CYSCREEN) - MUON_PROGRESS_HEIGHT) / 2;
  backend->window = CreateWindowExA(
      WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
      "MuonBootstrapProgressWindow", "muon", WS_CAPTION | WS_SYSMENU, x, y,
      MUON_PROGRESS_WIDTH, MUON_PROGRESS_HEIGHT, NULL, NULL, backend->instance,
      NULL);
  if (backend->window == NULL) {
    return 0;
  }
  backend->status_label = CreateWindowExA(
      0, "STATIC", "", WS_CHILD | WS_VISIBLE, 20, 18,
      MUON_PROGRESS_WIDTH - 40, 22, backend->window, NULL, backend->instance,
      NULL);
  if (has_progress_control) {
    backend->determinate_progress_bar = CreateWindowExA(
        0, PROGRESS_CLASSA, "", WS_CHILD | WS_VISIBLE | PBS_SMOOTH, 20, 52,
        MUON_PROGRESS_WIDTH - 40, MUON_PROGRESS_BAR_HEIGHT, backend->window,
        NULL, backend->instance, NULL);
    backend->marquee_progress_bar = CreateWindowExA(
        0, PROGRESS_CLASSA, "", WS_CHILD | PBS_MARQUEE, 20, 52,
        MUON_PROGRESS_WIDTH - 40, MUON_PROGRESS_BAR_HEIGHT, backend->window,
        NULL, backend->instance, NULL);
    if (backend->determinate_progress_bar == NULL ||
        backend->marquee_progress_bar == NULL) {
      if (backend->determinate_progress_bar != NULL) {
        DestroyWindow(backend->determinate_progress_bar);
      }
      if (backend->marquee_progress_bar != NULL) {
        DestroyWindow(backend->marquee_progress_bar);
      }
      backend->determinate_progress_bar = NULL;
      backend->marquee_progress_bar = NULL;
    }
  }
  if (backend->status_label == NULL) {
    DestroyWindow(backend->window);
    backend->window = NULL;
    backend->status_label = NULL;
    backend->determinate_progress_bar = NULL;
    backend->marquee_progress_bar = NULL;
    return 0;
  }
  if (backend->determinate_progress_bar != NULL) {
    SendMessageA(backend->determinate_progress_bar, PBM_SETRANGE, 0,
                 MAKELPARAM(0, 1000));
  }
  ShowWindow(backend->window, SW_SHOWNORMAL);
  UpdateWindow(backend->window);
  backend->shown = 1;
  return 1;
}

static void set_progress_mode(MuonBootstrapProgressBackend *backend,
                              int marquee) {
  if (backend->determinate_progress_bar == NULL ||
      backend->marquee_progress_bar == NULL) {
    return;
  }
  if (marquee) {
    ShowWindow(backend->determinate_progress_bar, SW_HIDE);
    ShowWindow(backend->marquee_progress_bar, SW_SHOW);
    if (!backend->marquee_running) {
      SendMessageA(backend->marquee_progress_bar, PBM_SETMARQUEE, TRUE,
                   MUON_PROGRESS_FRAME_MILLISECONDS);
      backend->marquee_running = 1;
    }
  } else {
    if (backend->marquee_running) {
      SendMessageA(backend->marquee_progress_bar, PBM_SETMARQUEE, FALSE, 0);
      backend->marquee_running = 0;
    }
    ShowWindow(backend->marquee_progress_bar, SW_HIDE);
    ShowWindow(backend->determinate_progress_bar, SW_SHOW);
  }
}

static void dispose_window(MuonBootstrapProgressBackend *backend) {
  if (backend->marquee_progress_bar != NULL && backend->marquee_running) {
    SendMessageA(backend->marquee_progress_bar, PBM_SETMARQUEE, FALSE, 0);
  }
  if (backend->window != NULL) {
    DestroyWindow(backend->window);
    backend->window = NULL;
    backend->status_label = NULL;
    backend->determinate_progress_bar = NULL;
    backend->marquee_progress_bar = NULL;
    backend->shown = 0;
    backend->marquee_running = 0;
  }
}

static void pump_messages(void) {
  MSG message;
  while (PeekMessageA(&message, NULL, 0, 0, PM_REMOVE)) {
    TranslateMessage(&message);
    DispatchMessageA(&message);
  }
}

static void update_window_controls(MuonBootstrapProgressBackend *backend,
                                   const MuonPrepareProgress *event,
                                   const char *status) {
  SetWindowTextA(backend->status_label, status);
  if (backend->determinate_progress_bar == NULL ||
      backend->marquee_progress_bar == NULL) {
    return;
  }
  if (event->determinate && event->total != 0) {
    const unsigned long long position = progress_position(event, 1000);
    set_progress_mode(backend, 0);
    SendMessageA(backend->determinate_progress_bar, PBM_SETPOS,
                 (WPARAM)position, 0);
  } else {
    set_progress_mode(backend, 1);
  }
}

static DWORD WINAPI progress_thread_main(LPVOID parameter) {
  MuonBootstrapProgressBackend *backend =
      (MuonBootstrapProgressBackend *)parameter;
  for (;;) {
    MsgWaitForMultipleObjects(1, &backend->update_event, FALSE,
                              MUON_PROGRESS_FRAME_MILLISECONDS, QS_ALLINPUT);
    pump_messages();

    MuonPrepareProgress event;
    char status[sizeof(backend->status)];
    int has_event = 0;
    int stop_requested = 0;
    EnterCriticalSection(&backend->mutex);
    stop_requested = backend->stop_requested;
    has_event = backend->has_event;
    if (has_event) {
      event = backend->last_event;
      snprintf(status, sizeof(status), "%s", backend->status);
      event.status = status;
    }
    LeaveCriticalSection(&backend->mutex);

    if (stop_requested) {
      break;
    }
    if (!has_event) {
      continue;
    }
    if (!backend->shown &&
        (event.phase == MUON_PREPARE_PROGRESS_PHASE_DONE ||
         event.phase == MUON_PREPARE_PROGRESS_PHASE_FAILED)) {
      continue;
    }
    if (!ensure_window(backend)) {
      continue;
    }
    update_window_controls(backend, &event, status);
  }
  dispose_window(backend);
  return 0;
}

void muon_bootstrap_progress_init(MuonBootstrapProgress *progress) {
  progress->backend = NULL;
  MuonBootstrapProgressBackend *backend =
      (MuonBootstrapProgressBackend *)calloc(1, sizeof(*backend));
  if (backend == NULL) {
    return;
  }
  backend->instance = GetModuleHandleA(NULL);
  InitializeCriticalSection(&backend->mutex);
  backend->update_event = CreateEventA(NULL, FALSE, FALSE, NULL);
  if (backend->update_event == NULL) {
    DeleteCriticalSection(&backend->mutex);
    free(backend);
    return;
  }
  backend->thread =
      CreateThread(NULL, 0, progress_thread_main, backend, 0,
                   &backend->thread_id);
  if (backend->thread == NULL) {
    CloseHandle(backend->update_event);
    DeleteCriticalSection(&backend->mutex);
    free(backend);
    return;
  }
  progress->backend = backend;
}

int muon_bootstrap_progress_is_available(const MuonBootstrapProgress *progress) {
  return progress->backend != NULL;
}

void muon_bootstrap_progress_update(MuonBootstrapProgress *progress,
                                    const MuonPrepareProgress *event) {
  MuonBootstrapProgressBackend *backend =
      (MuonBootstrapProgressBackend *)progress->backend;
  if (backend == NULL) {
    return;
  }
  EnterCriticalSection(&backend->mutex);
  backend->last_event = *event;
  snprintf(backend->status, sizeof(backend->status), "%s",
           event->status == NULL ? "" : event->status);
  backend->last_event.status = backend->status;
  backend->has_event = 1;
  LeaveCriticalSection(&backend->mutex);
  SetEvent(backend->update_event);
}

void muon_bootstrap_progress_fail(MuonBootstrapProgress *progress) {
  MuonPrepareProgress event;
  event.phase = MUON_PREPARE_PROGRESS_PHASE_FAILED;
  event.status = "Failed to prepare CEF.";
  event.current = 0;
  event.total = 0;
  event.determinate = 0;
  muon_bootstrap_progress_update(progress, &event);
}

void muon_bootstrap_progress_dispose(MuonBootstrapProgress *progress) {
  MuonBootstrapProgressBackend *backend =
      (MuonBootstrapProgressBackend *)progress->backend;
  if (backend == NULL) {
    return;
  }
  EnterCriticalSection(&backend->mutex);
  backend->stop_requested = 1;
  LeaveCriticalSection(&backend->mutex);
  SetEvent(backend->update_event);
  WaitForSingleObject(backend->thread, INFINITE);
  CloseHandle(backend->thread);
  CloseHandle(backend->update_event);
  DeleteCriticalSection(&backend->mutex);
  free(backend);
  progress->backend = NULL;
}

#else

typedef struct {
  xcb_connection_t *connection;
  xcb_screen_t *screen;
  xcb_window_t window;
  xcb_gcontext_t gc;
  xcb_atom_t wm_protocols;
  xcb_atom_t wm_delete_window;
  uint16_t width;
  uint16_t height;
  pthread_t thread;
  pthread_mutex_t mutex;
  pthread_cond_t condition;
  MuonPrepareProgress last_event;
  char status[256];
  int has_event;
  int stop_requested;
  int shown;
} MuonBootstrapProgressBackend;

static xcb_atom_t intern_atom(xcb_connection_t *connection, const char *name) {
  xcb_intern_atom_cookie_t cookie =
      xcb_intern_atom(connection, 0, (uint16_t)strlen(name), name);
  xcb_intern_atom_reply_t *reply =
      xcb_intern_atom_reply(connection, cookie, NULL);
  if (reply == NULL) {
    return XCB_ATOM_NONE;
  }
  const xcb_atom_t atom = reply->atom;
  free(reply);
  return atom;
}

static void set_foreground(MuonBootstrapProgressBackend *backend,
                           uint32_t color) {
  const uint32_t values[] = {color};
  xcb_change_gc(backend->connection, backend->gc, XCB_GC_FOREGROUND, values);
}

static void fill_rect(MuonBootstrapProgressBackend *backend, uint32_t color,
                      int16_t x, int16_t y, uint16_t width,
                      uint16_t height) {
  xcb_rectangle_t rectangle;
  rectangle.x = x;
  rectangle.y = y;
  rectangle.width = width;
  rectangle.height = height;
  set_foreground(backend, color);
  xcb_poly_fill_rectangle(backend->connection, backend->window, backend->gc, 1,
                          &rectangle);
}

static unsigned long long monotonic_nanoseconds(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return 0;
  }
  return ((unsigned long long)now.tv_sec * 1000000000ULL) +
         (unsigned long long)now.tv_nsec;
}

static void draw_progress(MuonBootstrapProgressBackend *backend,
                          const MuonPrepareProgress *event,
                          const char *status) {
  const uint16_t bar_x = 24;
  const uint16_t bar_y = 68;
  const uint16_t bar_width = backend->width - 48;
  const uint16_t bar_height = MUON_PROGRESS_BAR_HEIGHT;
  fill_rect(backend, MUON_PROGRESS_BACKGROUND_COLOR, 0, 0, backend->width,
            backend->height);
  fill_rect(backend, 0x606060, bar_x, bar_y, bar_width, bar_height);
  fill_rect(backend, 0xffffff, bar_x + 1, bar_y + 1, bar_width - 2,
            bar_height - 2);
  if (event->determinate && event->total != 0) {
    const uint16_t filled =
        (uint16_t)progress_position(event, bar_width - 2);
    if (filled > 0) {
      fill_rect(backend, 0x2f6fed, bar_x + 1, bar_y + 1, filled,
                bar_height - 2);
    }
  } else {
    const uint16_t segment = bar_width / 3;
    const uint16_t range = bar_width - segment - 2;
    const uint16_t x =
        pulse_position_from_elapsed(monotonic_nanoseconds(), range);
    fill_rect(backend, 0x2f6fed, bar_x + 1 + x, bar_y + 1, segment,
              bar_height - 2);
  }
  set_foreground(backend, backend->screen->black_pixel);
  const size_t status_size = strlen(status);
  const uint8_t text_size =
      status_size > 255 ? 255 : (uint8_t)status_size;
  xcb_image_text_8(backend->connection, text_size, backend->window,
                   backend->gc, 24, 36, status);
  xcb_flush(backend->connection);
}

static void pump_events(MuonBootstrapProgressBackend *backend) {
  xcb_generic_event_t *event = NULL;
  while ((event = xcb_poll_for_event(backend->connection)) != NULL) {
    const uint8_t type = event->response_type & 0x7f;
    if (type == XCB_CLIENT_MESSAGE) {
      const xcb_client_message_event_t *message =
          (const xcb_client_message_event_t *)event;
      if (message->data.data32[0] == backend->wm_delete_window) {
        free(event);
        continue;
      }
    }
    free(event);
  }
}

static int ensure_window(MuonBootstrapProgressBackend *backend) {
  if (backend->shown) {
    return 1;
  }
  backend->width = MUON_PROGRESS_WIDTH;
  backend->height = MUON_PROGRESS_HEIGHT;
  backend->window = xcb_generate_id(backend->connection);
  const int16_t x =
      backend->screen->width_in_pixels > backend->width
          ? (int16_t)((backend->screen->width_in_pixels - backend->width) / 2)
          : 0;
  const int16_t y =
      backend->screen->height_in_pixels > backend->height
          ? (int16_t)((backend->screen->height_in_pixels - backend->height) / 2)
          : 0;
  const uint32_t values[] = {
      MUON_PROGRESS_BACKGROUND_COLOR,
      XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY};
  xcb_create_window(backend->connection, backend->screen->root_depth,
                    backend->window, backend->screen->root, x, y,
                    backend->width, backend->height, 0,
                    XCB_WINDOW_CLASS_INPUT_OUTPUT,
                    backend->screen->root_visual,
                    XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, values);
  const char title[] = "muon";
  xcb_change_property(backend->connection, XCB_PROP_MODE_REPLACE,
                      backend->window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
                      (uint32_t)strlen(title), title);
  backend->wm_protocols = intern_atom(backend->connection, "WM_PROTOCOLS");
  backend->wm_delete_window =
      intern_atom(backend->connection, "WM_DELETE_WINDOW");
  if (backend->wm_protocols != XCB_ATOM_NONE &&
      backend->wm_delete_window != XCB_ATOM_NONE) {
    xcb_change_property(backend->connection, XCB_PROP_MODE_REPLACE,
                        backend->window, backend->wm_protocols, XCB_ATOM_ATOM,
                        32, 1, &backend->wm_delete_window);
  }
  backend->gc = xcb_generate_id(backend->connection);
  const uint32_t gc_values[] = {backend->screen->black_pixel,
                                MUON_PROGRESS_BACKGROUND_COLOR};
  xcb_create_gc(backend->connection, backend->gc, backend->window,
                XCB_GC_FOREGROUND | XCB_GC_BACKGROUND, gc_values);
  xcb_map_window(backend->connection, backend->window);
  xcb_flush(backend->connection);
  backend->shown = 1;
  return xcb_connection_has_error(backend->connection) == 0;
}

static void add_frame_delay(struct timespec *deadline) {
  deadline->tv_nsec += (long)MUON_PROGRESS_FRAME_NANOSECONDS;
  while (deadline->tv_nsec >= 1000000000L) {
    deadline->tv_sec += 1;
    deadline->tv_nsec -= 1000000000L;
  }
}

static void wait_for_next_frame(MuonBootstrapProgressBackend *backend) {
  struct timespec deadline;
  if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
    return;
  }
  add_frame_delay(&deadline);
  pthread_mutex_lock(&backend->mutex);
  if (!backend->stop_requested) {
    pthread_cond_timedwait(&backend->condition, &backend->mutex, &deadline);
  }
  pthread_mutex_unlock(&backend->mutex);
}

static void *progress_thread_main(void *parameter) {
  MuonBootstrapProgressBackend *backend =
      (MuonBootstrapProgressBackend *)parameter;
  for (;;) {
    MuonPrepareProgress event;
    char status[sizeof(backend->status)];
    int has_event = 0;
    int stop_requested = 0;

    pthread_mutex_lock(&backend->mutex);
    while (!backend->stop_requested && !backend->has_event) {
      pthread_cond_wait(&backend->condition, &backend->mutex);
    }
    stop_requested = backend->stop_requested;
    has_event = backend->has_event;
    if (has_event) {
      event = backend->last_event;
      snprintf(status, sizeof(status), "%s", backend->status);
      event.status = status;
    }
    pthread_mutex_unlock(&backend->mutex);

    if (stop_requested) {
      break;
    }
    if (!has_event) {
      wait_for_next_frame(backend);
      continue;
    }
    if (!backend->shown &&
        (event.phase == MUON_PREPARE_PROGRESS_PHASE_DONE ||
         event.phase == MUON_PREPARE_PROGRESS_PHASE_FAILED)) {
      wait_for_next_frame(backend);
      continue;
    }
    if (ensure_window(backend)) {
      pump_events(backend);
      draw_progress(backend, &event, status);
    }
    wait_for_next_frame(backend);
  }
  if (backend->shown) {
    xcb_destroy_window(backend->connection, backend->window);
  }
  if (backend->gc != 0) {
    xcb_free_gc(backend->connection, backend->gc);
  }
  xcb_flush(backend->connection);
  xcb_disconnect(backend->connection);
  return NULL;
}

void muon_bootstrap_progress_init(MuonBootstrapProgress *progress) {
  progress->backend = NULL;
  xcb_connection_t *connection = xcb_connect(NULL, NULL);
  if (connection == NULL || xcb_connection_has_error(connection) != 0) {
    if (connection != NULL) {
      xcb_disconnect(connection);
    }
    return;
  }
  const xcb_setup_t *setup = xcb_get_setup(connection);
  xcb_screen_iterator_t iterator = xcb_setup_roots_iterator(setup);
  if (iterator.data == NULL) {
    xcb_disconnect(connection);
    return;
  }
  MuonBootstrapProgressBackend *backend =
      (MuonBootstrapProgressBackend *)calloc(1, sizeof(*backend));
  if (backend == NULL) {
    xcb_disconnect(connection);
    return;
  }
  backend->connection = connection;
  backend->screen = iterator.data;
  if (pthread_mutex_init(&backend->mutex, NULL) != 0) {
    xcb_disconnect(connection);
    free(backend);
    return;
  }
  if (pthread_cond_init(&backend->condition, NULL) != 0) {
    pthread_mutex_destroy(&backend->mutex);
    xcb_disconnect(connection);
    free(backend);
    return;
  }
  if (pthread_create(&backend->thread, NULL, progress_thread_main, backend) !=
      0) {
    pthread_cond_destroy(&backend->condition);
    pthread_mutex_destroy(&backend->mutex);
    xcb_disconnect(connection);
    free(backend);
    return;
  }
  progress->backend = backend;
}

int muon_bootstrap_progress_is_available(const MuonBootstrapProgress *progress) {
  return progress->backend != NULL;
}

void muon_bootstrap_progress_update(MuonBootstrapProgress *progress,
                                    const MuonPrepareProgress *event) {
  MuonBootstrapProgressBackend *backend =
      (MuonBootstrapProgressBackend *)progress->backend;
  if (backend == NULL) {
    return;
  }
  pthread_mutex_lock(&backend->mutex);
  backend->last_event = *event;
  snprintf(backend->status, sizeof(backend->status), "%s",
           event->status == NULL ? "" : event->status);
  backend->last_event.status = backend->status;
  backend->has_event = 1;
  pthread_cond_signal(&backend->condition);
  pthread_mutex_unlock(&backend->mutex);
}

void muon_bootstrap_progress_fail(MuonBootstrapProgress *progress) {
  MuonPrepareProgress event;
  event.phase = MUON_PREPARE_PROGRESS_PHASE_FAILED;
  event.status = "Failed to prepare CEF.";
  event.current = 0;
  event.total = 0;
  event.determinate = 0;
  muon_bootstrap_progress_update(progress, &event);
}

void muon_bootstrap_progress_dispose(MuonBootstrapProgress *progress) {
  MuonBootstrapProgressBackend *backend =
      (MuonBootstrapProgressBackend *)progress->backend;
  if (backend == NULL) {
    return;
  }
  pthread_mutex_lock(&backend->mutex);
  backend->stop_requested = 1;
  pthread_cond_signal(&backend->condition);
  pthread_mutex_unlock(&backend->mutex);
  pthread_join(backend->thread, NULL);
  pthread_cond_destroy(&backend->condition);
  pthread_mutex_destroy(&backend->mutex);
  free(backend);
  progress->backend = NULL;
}

#endif
