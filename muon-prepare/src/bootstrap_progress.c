// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#else
#include <stdint.h>
#include <xcb/xcb.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bootstrap_progress.h"

#define MUON_PROGRESS_WIDTH 360
#define MUON_PROGRESS_HEIGHT 110

#ifdef _WIN32

typedef struct {
  HINSTANCE instance;
  HWND window;
  HWND status_label;
  HWND progress_bar;
  int shown;
  int pulse;
} MuonBootstrapProgressBackend;

static LRESULT CALLBACK progress_window_proc(HWND window, UINT message,
                                             WPARAM wparam, LPARAM lparam) {
  if (message == WM_CLOSE) {
    return 0;
  }
  return DefWindowProcA(window, message, wparam, lparam);
}

static int register_progress_window_class(HINSTANCE instance) {
  WNDCLASSA window_class;
  memset(&window_class, 0, sizeof(window_class));
  window_class.lpfnWndProc = progress_window_proc;
  window_class.hInstance = instance;
  window_class.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
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
  controls.dwICC = ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES;
  if (!InitCommonControlsEx(&controls)) {
    return 0;
  }
  const int x =
      (GetSystemMetrics(SM_CXSCREEN) - MUON_PROGRESS_WIDTH) / 2;
  const int y =
      (GetSystemMetrics(SM_CYSCREEN) - MUON_PROGRESS_HEIGHT) / 2;
  backend->window = CreateWindowExA(
      WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
      "MuonBootstrapProgressWindow", "Muon", WS_CAPTION | WS_SYSMENU, x, y,
      MUON_PROGRESS_WIDTH, MUON_PROGRESS_HEIGHT, NULL, NULL, backend->instance,
      NULL);
  if (backend->window == NULL) {
    return 0;
  }
  backend->status_label = CreateWindowExA(
      0, "STATIC", "", WS_CHILD | WS_VISIBLE, 20, 18,
      MUON_PROGRESS_WIDTH - 40, 22, backend->window, NULL, backend->instance,
      NULL);
  backend->progress_bar = CreateWindowExA(
      0, PROGRESS_CLASSA, "", WS_CHILD | WS_VISIBLE | PBS_SMOOTH, 20, 52,
      MUON_PROGRESS_WIDTH - 40, 22, backend->window, NULL, backend->instance,
      NULL);
  if (backend->status_label == NULL || backend->progress_bar == NULL) {
    DestroyWindow(backend->window);
    backend->window = NULL;
    backend->status_label = NULL;
    backend->progress_bar = NULL;
    return 0;
  }
  SendMessageA(backend->progress_bar, PBM_SETRANGE, 0, MAKELPARAM(0, 1000));
  ShowWindow(backend->window, SW_SHOWNORMAL);
  UpdateWindow(backend->window);
  backend->shown = 1;
  return 1;
}

static void pump_messages(void) {
  MSG message;
  while (PeekMessageA(&message, NULL, 0, 0, PM_REMOVE)) {
    TranslateMessage(&message);
    DispatchMessageA(&message);
  }
}

void muon_bootstrap_progress_init(MuonBootstrapProgress *progress) {
  progress->backend = calloc(1, sizeof(MuonBootstrapProgressBackend));
  if (progress->backend != NULL) {
    ((MuonBootstrapProgressBackend *)progress->backend)->instance =
        GetModuleHandleA(NULL);
  }
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
  if (!backend->shown &&
      (event->phase == MUON_PREPARE_PROGRESS_PHASE_DONE ||
       event->phase == MUON_PREPARE_PROGRESS_PHASE_FAILED)) {
    return;
  }
  if (!ensure_window(backend)) {
    return;
  }
  SetWindowTextA(backend->status_label,
                 event->status == NULL ? "" : event->status);
  if (event->determinate && event->total != 0) {
    const unsigned long long position =
        event->current >= event->total
            ? 1000
            : (event->current * 1000ULL) / event->total;
    SendMessageA(backend->progress_bar, PBM_SETPOS, (WPARAM)position, 0);
  } else {
    backend->pulse = (backend->pulse + 65) % 1000;
    SendMessageA(backend->progress_bar, PBM_SETPOS, (WPARAM)backend->pulse, 0);
  }
  pump_messages();
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
  if (backend->window != NULL) {
    DestroyWindow(backend->window);
  }
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
  int shown;
  int pulse;
  MuonPrepareProgress last_event;
  char status[256];
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

static void draw_progress(MuonBootstrapProgressBackend *backend) {
  const uint16_t bar_x = 24;
  const uint16_t bar_y = 68;
  const uint16_t bar_width = backend->width - 48;
  const uint16_t bar_height = 6;
  fill_rect(backend, backend->screen->white_pixel, 0, 0, backend->width,
            backend->height);
  fill_rect(backend, 0x606060, bar_x, bar_y, bar_width, bar_height);
  fill_rect(backend, 0xffffff, bar_x + 1, bar_y + 1, bar_width - 2,
            bar_height - 2);
  if (backend->last_event.determinate && backend->last_event.total != 0) {
    const uint16_t filled =
        (uint16_t)progress_position(&backend->last_event, bar_width - 2);
    if (filled > 0) {
      fill_rect(backend, 0x2f6fed, bar_x + 1, bar_y + 1, filled,
                bar_height - 2);
    }
  } else {
    const uint16_t segment = bar_width / 3;
    const uint16_t range = bar_width - segment - 2;
    const uint16_t x =
        range == 0 ? 0 : (uint16_t)((backend->pulse % 1000) * range / 1000);
    fill_rect(backend, 0x2f6fed, bar_x + 1 + x, bar_y + 1, segment,
              bar_height - 2);
  }
  set_foreground(backend, backend->screen->black_pixel);
  const size_t status_size = strlen(backend->status);
  const uint8_t text_size =
      status_size > 255 ? 255 : (uint8_t)status_size;
  xcb_image_text_8(backend->connection, text_size, backend->window,
                   backend->gc, 24, 36, backend->status);
  xcb_flush(backend->connection);
}

static void pump_events(MuonBootstrapProgressBackend *backend) {
  xcb_generic_event_t *event = NULL;
  while ((event = xcb_poll_for_event(backend->connection)) != NULL) {
    const uint8_t type = event->response_type & 0x7f;
    if (type == XCB_EXPOSE) {
      draw_progress(backend);
    } else if (type == XCB_CLIENT_MESSAGE) {
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
      backend->screen->white_pixel,
      XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY};
  xcb_create_window(backend->connection, backend->screen->root_depth,
                    backend->window, backend->screen->root, x, y,
                    backend->width, backend->height, 0,
                    XCB_WINDOW_CLASS_INPUT_OUTPUT,
                    backend->screen->root_visual,
                    XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, values);
  const char title[] = "Muon";
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
                                backend->screen->white_pixel};
  xcb_create_gc(backend->connection, backend->gc, backend->window,
                XCB_GC_FOREGROUND | XCB_GC_BACKGROUND, gc_values);
  xcb_map_window(backend->connection, backend->window);
  xcb_flush(backend->connection);
  backend->shown = 1;
  return xcb_connection_has_error(backend->connection) == 0;
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
  if (!backend->shown &&
      (event->phase == MUON_PREPARE_PROGRESS_PHASE_DONE ||
       event->phase == MUON_PREPARE_PROGRESS_PHASE_FAILED)) {
    return;
  }
  if (!ensure_window(backend)) {
    return;
  }
  backend->last_event = *event;
  backend->pulse = (backend->pulse + 55) % 1000;
  snprintf(backend->status, sizeof(backend->status), "%s",
           event->status == NULL ? "" : event->status);
  pump_events(backend);
  draw_progress(backend);
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
  if (backend->shown) {
    xcb_destroy_window(backend->connection, backend->window);
  }
  if (backend->gc != 0) {
    xcb_free_gc(backend->connection, backend->gc);
  }
  xcb_flush(backend->connection);
  xcb_disconnect(backend->connection);
  free(backend);
  progress->backend = NULL;
}

#endif
