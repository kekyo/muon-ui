// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

declare module "muon:browser" {
  /** Reload the current page. */
  export const reload: () => Promise<void>;
  /** Reload the current page while ignoring cached data. */
  export const hardReload: () => Promise<void>;
  /** Toggle fullscreen mode for the current browser window. */
  export const toggleFullscreen: () => Promise<void>;
  /** Enter fullscreen mode for the current browser window. */
  export const enterFullscreen: () => Promise<void>;
  /** Exit fullscreen mode for the current browser window. */
  export const exitFullscreen: () => Promise<void>;
  /** Increase the current page zoom level. */
  export const zoomIn: () => Promise<void>;
  /** Decrease the current page zoom level. */
  export const zoomOut: () => Promise<void>;
  /** Reset the current page zoom level. */
  export const resetZoom: () => Promise<void>;
  /** Show the current browser window. */
  export const show: () => Promise<void>;
  /** Hide the current browser window. */
  export const hide: () => Promise<void>;
  /** Focus and activate the current browser window. */
  export const focus: () => Promise<void>;
  /** Deactivate the current browser window. */
  export const blur: () => Promise<void>;
  /** Minimize the current browser window. */
  export const minimize: () => Promise<void>;
  /** Maximize the current browser window. */
  export const maximize: () => Promise<void>;
  /** Restore the current browser window from minimized or maximized state. */
  export const restore: () => Promise<void>;
  /** Return bounds for the current top-level muon window. */
  export const getWindowBounds: () => Promise<MuonWindowBounds>;
  /** Request new bounds for the current top-level muon window. */
  export const setWindowBounds: (bounds: MuonWindowBounds) => Promise<void>;
  /** Replace the current browser window's custom native context menu items. */
  export const setContextMenuItems: (
    items: readonly MuonBrowserContextMenuItem[],
    handler?: MuonBrowserContextMenuHandler,
  ) => Promise<void>;
  /** Clear custom native context menu items for the current browser window. */
  export const clearContextMenuItems: () => Promise<void>;
  /** Create a browser-owned system tray item. */
  export const createTray: (
    options: MuonBrowserTrayOptions,
    handler?: MuonBrowserTrayEventHandler,
  ) => Promise<string>;
  /** Replace menu items for a browser-owned system tray item. */
  export const setTrayMenu: (
    id: string,
    items: readonly MuonBrowserTrayMenuItem[],
    handler?: MuonBrowserTrayEventHandler,
  ) => Promise<void>;
  /** Replace the icon for a browser-owned system tray item. */
  export const setTrayIcon: (id: string, iconPath: string) => Promise<void>;
  /** Replace or clear the tooltip for a browser-owned system tray item. */
  export const setTrayTooltip: (
    id: string,
    tooltip: string | null,
  ) => Promise<void>;
  /** Remove a browser-owned system tray item. */
  export const removeTray: (id: string) => Promise<void>;
  /** Show or hide the current muon custom title bar. */
  export const setTitleBarVisibility: (visible: boolean) => Promise<void>;
  /** Set or clear the current window title bar icon. */
  export const setTitleBarIcon: (path: string | null) => Promise<void>;
  /** Close the current browser window. */
  export const close: () => Promise<void>;
  /** Shut down the muon process. */
  export const shutdown: (exitCode?: number) => Promise<void>;
  /** Recycle the muon process by requesting an automatic restart. */
  export const recycle: () => Promise<void>;
}

declare module "muon:executor" {
  /**
   * Spawn a child process without invoking a shell.
   *
   * @param options - Process launch options.
   * @returns A promise for the started child process handle.
   */
  export const spawn: (
    options: MuonExecutorSpawnOptions,
  ) => Promise<MuonExecutorProcess>;
}
