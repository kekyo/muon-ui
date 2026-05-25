export const createXvfbCommandEnvironment = (environment) => {
  const commandEnvironment = {
    ...environment,
    CLUTTER_BACKEND: "x11",
    ELECTRON_OZONE_PLATFORM_HINT: "x11",
    GDK_BACKEND: "x11",
    GIO_USE_VFS: "local",
    GNOME_ACCESSIBILITY: "1",
    GTK_MODULES: environment.GTK_MODULES ?? "gail:atk-bridge",
    GTK_USE_PORTAL: "0",
    LIBGL_ALWAYS_SOFTWARE: "1",
    MOZ_ENABLE_WAYLAND: "0",
    MUON_TEST_XVFB_WINDOW_MANAGER: "1",
    OZONE_PLATFORM: "x11",
    QT_QPA_PLATFORM: "xcb",
    SDL_VIDEODRIVER: "x11",
    XDG_SESSION_TYPE: "x11",
  };

  delete commandEnvironment.WAYLAND_DISPLAY;
  delete commandEnvironment.WAYLAND_SOCKET;
  delete commandEnvironment.AT_SPI_BUS_ADDRESS;
  delete commandEnvironment.GNOME_KEYRING_CONTROL;
  delete commandEnvironment.NO_AT_BRIDGE;
  delete commandEnvironment.SSH_AUTH_SOCK;

  return commandEnvironment;
};
