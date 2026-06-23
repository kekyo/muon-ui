#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>

#define NET_WM_MOVERESIZE_MOVE 8

typedef struct {
  Atom net_active_window;
  Atom net_client_list;
  Atom net_supported;
  Atom net_supporting_wm_check;
  Atom net_wm_name;
  Atom net_wm_moveresize;
  Atom net_wm_state;
  Atom net_wm_state_fullscreen;
  Atom net_wm_state_maximized_horz;
  Atom net_wm_state_maximized_vert;
  Atom utf8_string;
  Atom wm_change_state;
  Atom wm_state;
} MuonAtoms;

typedef struct {
  Window window;
  int saved_x;
  int saved_y;
  unsigned int saved_width;
  unsigned int saved_height;
  int has_saved_bounds;
  int fullscreen;
  int maximized;
} MuonManagedWindow;

typedef struct {
  Display *display;
  Window root;
  Window wm_window;
  MuonAtoms atoms;
  MuonManagedWindow *windows;
  size_t window_count;
  size_t window_capacity;
} MuonWindowManager;

static int g_x_error_seen = 0;

static int HandleXError(Display *display, XErrorEvent *event) {
  (void)display;
  if (event->error_code == BadAccess) {
    g_x_error_seen = 1;
  }
  return 0;
}

static Atom InternAtom(Display *display, const char *name) {
  return XInternAtom(display, name, False);
}

static MuonAtoms CreateAtoms(Display *display) {
  MuonAtoms atoms;
  atoms.net_active_window = InternAtom(display, "_NET_ACTIVE_WINDOW");
  atoms.net_client_list = InternAtom(display, "_NET_CLIENT_LIST");
  atoms.net_supported = InternAtom(display, "_NET_SUPPORTED");
  atoms.net_supporting_wm_check =
      InternAtom(display, "_NET_SUPPORTING_WM_CHECK");
  atoms.net_wm_name = InternAtom(display, "_NET_WM_NAME");
  atoms.net_wm_moveresize = InternAtom(display, "_NET_WM_MOVERESIZE");
  atoms.net_wm_state = InternAtom(display, "_NET_WM_STATE");
  atoms.net_wm_state_fullscreen =
      InternAtom(display, "_NET_WM_STATE_FULLSCREEN");
  atoms.net_wm_state_maximized_horz =
      InternAtom(display, "_NET_WM_STATE_MAXIMIZED_HORZ");
  atoms.net_wm_state_maximized_vert =
      InternAtom(display, "_NET_WM_STATE_MAXIMIZED_VERT");
  atoms.utf8_string = InternAtom(display, "UTF8_STRING");
  atoms.wm_change_state = InternAtom(display, "WM_CHANGE_STATE");
  atoms.wm_state = InternAtom(display, "WM_STATE");
  return atoms;
}

static MuonManagedWindow *FindWindow(MuonWindowManager *manager,
                                     Window window) {
  for (size_t i = 0; i < manager->window_count; ++i) {
    if (manager->windows[i].window == window) {
      return &manager->windows[i];
    }
  }
  return NULL;
}

static void UpdateClientList(MuonWindowManager *manager) {
  Window *client_windows = NULL;
  if (manager->window_count > 0) {
    client_windows = malloc(sizeof(Window) * manager->window_count);
    if (client_windows == NULL) {
      return;
    }
  }

  for (size_t i = 0; i < manager->window_count; ++i) {
    client_windows[i] = manager->windows[i].window;
  }

  XChangeProperty(manager->display, manager->root,
                  manager->atoms.net_client_list, XA_WINDOW, 32,
                  PropModeReplace, (unsigned char *)client_windows,
                  (int)manager->window_count);
  if (manager->window_count > 0) {
    const Window active = manager->windows[manager->window_count - 1].window;
    XChangeProperty(manager->display, manager->root,
                    manager->atoms.net_active_window, XA_WINDOW, 32,
                    PropModeReplace, (const unsigned char *)&active, 1);
  } else {
    XDeleteProperty(manager->display, manager->root,
                    manager->atoms.net_active_window);
  }
  XFlush(manager->display);
  free(client_windows);
}

static int AddWindow(MuonWindowManager *manager, Window window) {
  if (FindWindow(manager, window) != NULL) {
    return 1;
  }

  if (manager->window_count == manager->window_capacity) {
    const size_t next_capacity =
        manager->window_capacity == 0 ? 8 : manager->window_capacity * 2;
    MuonManagedWindow *next_windows =
        realloc(manager->windows, sizeof(MuonManagedWindow) * next_capacity);
    if (next_windows == NULL) {
      return 0;
    }
    manager->windows = next_windows;
    manager->window_capacity = next_capacity;
  }

  MuonManagedWindow managed_window;
  memset(&managed_window, 0, sizeof(managed_window));
  managed_window.window = window;
  manager->windows[manager->window_count] = managed_window;
  manager->window_count += 1;
  UpdateClientList(manager);
  return 1;
}

static void RemoveWindow(MuonWindowManager *manager, Window window) {
  for (size_t i = 0; i < manager->window_count; ++i) {
    if (manager->windows[i].window != window) {
      continue;
    }
    if (i + 1 < manager->window_count) {
      memmove(&manager->windows[i], &manager->windows[i + 1],
              sizeof(MuonManagedWindow) * (manager->window_count - i - 1));
    }
    manager->window_count -= 1;
    UpdateClientList(manager);
    return;
  }
}

static void SaveBoundsIfNeeded(MuonWindowManager *manager,
                               MuonManagedWindow *window) {
  if (window->has_saved_bounds) {
    return;
  }

  XWindowAttributes attributes;
  if (XGetWindowAttributes(manager->display, window->window, &attributes) == 0) {
    return;
  }
  window->saved_x = attributes.x;
  window->saved_y = attributes.y;
  window->saved_width = (unsigned int)attributes.width;
  window->saved_height = (unsigned int)attributes.height;
  window->has_saved_bounds = 1;
}

static void UpdateWindowStateProperty(MuonWindowManager *manager,
                                      MuonManagedWindow *window) {
  Atom states[3];
  int state_count = 0;
  if (window->fullscreen) {
    states[state_count] = manager->atoms.net_wm_state_fullscreen;
    state_count += 1;
  }
  if (window->maximized) {
    states[state_count] = manager->atoms.net_wm_state_maximized_horz;
    state_count += 1;
    states[state_count] = manager->atoms.net_wm_state_maximized_vert;
    state_count += 1;
  }

  if (state_count == 0) {
    XDeleteProperty(manager->display, window->window,
                    manager->atoms.net_wm_state);
    return;
  }
  XChangeProperty(manager->display, window->window,
                  manager->atoms.net_wm_state, XA_ATOM, 32, PropModeReplace,
                  (unsigned char *)states, state_count);
}

static void ApplyWindowBounds(MuonWindowManager *manager,
                              MuonManagedWindow *window) {
  if (window->fullscreen || window->maximized) {
    SaveBoundsIfNeeded(manager, window);
    XMoveResizeWindow(manager->display, window->window, 0, 0,
                      (unsigned int)DisplayWidth(manager->display,
                                                 DefaultScreen(manager->display)),
                      (unsigned int)DisplayHeight(
                          manager->display, DefaultScreen(manager->display)));
  } else if (window->has_saved_bounds) {
    XMoveResizeWindow(manager->display, window->window, window->saved_x,
                      window->saved_y, window->saved_width,
                      window->saved_height);
    window->has_saved_bounds = 0;
  }

  UpdateWindowStateProperty(manager, window);
  XFlush(manager->display);
}

static void SetStateFromAction(int action, int *state) {
  if (action == 0) {
    *state = 0;
    return;
  }
  if (action == 1) {
    *state = 1;
    return;
  }
  if (action == 2) {
    *state = !*state;
  }
}

static void HandleWindowStateMessage(MuonWindowManager *manager,
                                     XClientMessageEvent *event) {
  MuonManagedWindow *window = FindWindow(manager, event->window);
  if (window == NULL) {
    if (!AddWindow(manager, event->window)) {
      return;
    }
    window = FindWindow(manager, event->window);
    if (window == NULL) {
      return;
    }
  }

  const int action = (int)event->data.l[0];
  for (int i = 1; i <= 2; ++i) {
    const Atom state_atom = (Atom)event->data.l[i];
    if (state_atom == manager->atoms.net_wm_state_fullscreen) {
      SetStateFromAction(action, &window->fullscreen);
    }
    if (state_atom == manager->atoms.net_wm_state_maximized_horz ||
        state_atom == manager->atoms.net_wm_state_maximized_vert) {
      SetStateFromAction(action, &window->maximized);
    }
  }

  ApplyWindowBounds(manager, window);
}

static void HandleConfigureRequest(MuonWindowManager *manager,
                                   XConfigureRequestEvent *event) {
  XWindowChanges changes;
  changes.x = event->x;
  changes.y = event->y;
  changes.width = event->width;
  changes.height = event->height;
  changes.border_width = event->border_width;
  changes.sibling = event->above;
  changes.stack_mode = event->detail;
  XConfigureWindow(manager->display, event->window,
                   (unsigned int)event->value_mask, &changes);
  XFlush(manager->display);
}

static void HandleMapRequest(MuonWindowManager *manager,
                             XMapRequestEvent *event) {
  XWindowAttributes attributes;
  if (XGetWindowAttributes(manager->display, event->window, &attributes) != 0 &&
      attributes.override_redirect) {
    XMapWindow(manager->display, event->window);
    return;
  }

  AddWindow(manager, event->window);
  XMapWindow(manager->display, event->window);
  XFlush(manager->display);
}

static unsigned int ButtonMaskFromButton(long button) {
  if (button == 1) {
    return Button1Mask;
  }
  if (button == 2) {
    return Button2Mask;
  }
  if (button == 3) {
    return Button3Mask;
  }
  if (button == 4) {
    return Button4Mask;
  }
  if (button == 5) {
    return Button5Mask;
  }
  return Button1Mask | Button2Mask | Button3Mask | Button4Mask | Button5Mask;
}

static int QueryPointerDragState(MuonWindowManager *manager, long button,
                                 int *root_x, int *root_y,
                                 int *button_pressed) {
  Window root_return;
  Window child_return;
  int window_x;
  int window_y;
  unsigned int mask = 0;
  if (XQueryPointer(manager->display, manager->root, &root_return,
                    &child_return, root_x, root_y, &window_x, &window_y,
                    &mask) == 0) {
    return 0;
  }
  *button_pressed = (mask & ButtonMaskFromButton(button)) != 0;
  return 1;
}

static void SleepMicroseconds(long microseconds) {
  struct timeval timeout;
  timeout.tv_sec = microseconds / 1000000;
  timeout.tv_usec = microseconds % 1000000;
  select(0, NULL, NULL, NULL, &timeout);
}

static int GrabPointerForMove(MuonWindowManager *manager) {
  for (int retry = 0; retry < 50; ++retry) {
    const int result =
        XGrabPointer(manager->display, manager->root, False,
                     PointerMotionMask | ButtonReleaseMask, GrabModeAsync,
                     GrabModeAsync, None, None, CurrentTime);
    if (result == GrabSuccess) {
      return 1;
    }
    SleepMicroseconds(1000);
  }
  return 0;
}

static void HandleWindowMoveResizeMessage(MuonWindowManager *manager,
                                          XClientMessageEvent *event) {
  if ((int)event->data.l[2] != NET_WM_MOVERESIZE_MOVE) {
    return;
  }

  MuonManagedWindow *window = FindWindow(manager, event->window);
  if (window == NULL) {
    if (!AddWindow(manager, event->window)) {
      return;
    }
    window = FindWindow(manager, event->window);
    if (window == NULL) {
      return;
    }
  }

  XWindowAttributes attributes;
  if (XGetWindowAttributes(manager->display, window->window, &attributes) ==
      0) {
    return;
  }
  const int pointer_grabbed = GrabPointerForMove(manager);

  const int start_root_x = (int)event->data.l[0];
  const int start_root_y = (int)event->data.l[1];
  const int start_window_x = attributes.x;
  const int start_window_y = attributes.y;
  const long button = event->data.l[3];

  for (;;) {
    while (XPending(manager->display) > 0) {
      XEvent next_event;
      XNextEvent(manager->display, &next_event);
      if (next_event.type == ButtonRelease) {
        if (button == 0 || next_event.xbutton.button == (unsigned int)button) {
          if (pointer_grabbed) {
            XUngrabPointer(manager->display, CurrentTime);
          }
          XFlush(manager->display);
          return;
        }
      } else if (next_event.type == DestroyNotify) {
        RemoveWindow(manager, next_event.xdestroywindow.window);
      } else if (next_event.type == UnmapNotify) {
        RemoveWindow(manager, next_event.xunmap.window);
      }
    }

    int current_root_x = start_root_x;
    int current_root_y = start_root_y;
    int button_pressed = 0;
    if (!QueryPointerDragState(manager, button, &current_root_x,
                               &current_root_y, &button_pressed) ||
        !button_pressed) {
      break;
    }

    XMoveWindow(manager->display, window->window,
                start_window_x + current_root_x - start_root_x,
                start_window_y + current_root_y - start_root_y);
    XFlush(manager->display);
    SleepMicroseconds(1000);
  }

  if (pointer_grabbed) {
    XUngrabPointer(manager->display, CurrentTime);
  }
  XFlush(manager->display);
}

static void ManageExistingWindows(MuonWindowManager *manager) {
  Window root_return;
  Window parent_return;
  Window *children = NULL;
  unsigned int child_count = 0;
  if (XQueryTree(manager->display, manager->root, &root_return, &parent_return,
                 &children, &child_count) == 0) {
    return;
  }

  for (unsigned int i = 0; i < child_count; ++i) {
    XWindowAttributes attributes;
    if (XGetWindowAttributes(manager->display, children[i], &attributes) == 0) {
      continue;
    }
    if (attributes.override_redirect || attributes.map_state == IsUnmapped) {
      continue;
    }
    AddWindow(manager, children[i]);
  }

  if (children != NULL) {
    XFree(children);
  }
}

static int InitializeWindowManager(MuonWindowManager *manager) {
  memset(manager, 0, sizeof(*manager));
  manager->display = XOpenDisplay(NULL);
  if (manager->display == NULL) {
    fprintf(stderr, "Unable to open X display\n");
    return 0;
  }

  manager->root = DefaultRootWindow(manager->display);
  manager->atoms = CreateAtoms(manager->display);

  XSetErrorHandler(HandleXError);
  XSelectInput(manager->display, manager->root,
               SubstructureRedirectMask | SubstructureNotifyMask |
                   PropertyChangeMask);
  XSync(manager->display, False);
  if (g_x_error_seen) {
    fprintf(stderr, "Unable to become the X window manager\n");
    return 0;
  }

  const Atom supported[] = {
      manager->atoms.net_active_window,
      manager->atoms.net_client_list,
      manager->atoms.net_supported,
      manager->atoms.net_supporting_wm_check,
      manager->atoms.net_wm_name,
      manager->atoms.net_wm_moveresize,
      manager->atoms.net_wm_state,
      manager->atoms.net_wm_state_fullscreen,
      manager->atoms.net_wm_state_maximized_horz,
      manager->atoms.net_wm_state_maximized_vert,
  };
  XChangeProperty(manager->display, manager->root, manager->atoms.net_supported,
                  XA_ATOM, 32, PropModeReplace, (const unsigned char *)supported,
                  (int)(sizeof(supported) / sizeof(supported[0])));

  manager->wm_window =
      XCreateSimpleWindow(manager->display, manager->root, 0, 0, 1, 1, 0, 0, 0);
  XChangeProperty(manager->display, manager->root,
                  manager->atoms.net_supporting_wm_check, XA_WINDOW, 32,
                  PropModeReplace, (const unsigned char *)&manager->wm_window,
                  1);
  XChangeProperty(manager->display, manager->wm_window,
                  manager->atoms.net_supporting_wm_check, XA_WINDOW, 32,
                  PropModeReplace, (const unsigned char *)&manager->wm_window,
                  1);
  {
    const char name[] = "muon-xvfb-window-manager";
    XChangeProperty(manager->display, manager->wm_window,
                    manager->atoms.net_wm_name, manager->atoms.utf8_string, 8,
                    PropModeReplace, (const unsigned char *)name,
                    (int)strlen(name));
  }

  ManageExistingWindows(manager);
  XFlush(manager->display);
  return 1;
}

static void DestroyWindowManager(MuonWindowManager *manager) {
  if (manager->display != NULL) {
    if (manager->wm_window != 0) {
      XDestroyWindow(manager->display, manager->wm_window);
    }
    XCloseDisplay(manager->display);
  }
  free(manager->windows);
}

int main(void) {
  MuonWindowManager manager;
  if (!InitializeWindowManager(&manager)) {
    DestroyWindowManager(&manager);
    return 1;
  }

  printf("ready\n");
  fflush(stdout);

  for (;;) {
    XEvent event;
    XNextEvent(manager.display, &event);
    switch (event.type) {
      case MapRequest:
        HandleMapRequest(&manager, &event.xmaprequest);
        break;
      case ConfigureRequest:
        HandleConfigureRequest(&manager, &event.xconfigurerequest);
        break;
      case ClientMessage:
        if (event.xclient.message_type == manager.atoms.net_wm_state) {
          HandleWindowStateMessage(&manager, &event.xclient);
        } else if (event.xclient.message_type ==
                   manager.atoms.net_wm_moveresize) {
          HandleWindowMoveResizeMessage(&manager, &event.xclient);
        }
        break;
      case DestroyNotify:
        RemoveWindow(&manager, event.xdestroywindow.window);
        break;
      case UnmapNotify:
        RemoveWindow(&manager, event.xunmap.window);
        break;
      default:
        break;
    }
  }

  DestroyWindowManager(&manager);
  return 0;
}
