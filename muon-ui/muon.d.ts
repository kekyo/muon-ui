// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

/// <reference lib="esnext.disposable" />
/// <reference path="./muon-virtual-modules.d.ts" />

export {};

declare global {
  /** Browser window object extended with the muon host API. */
  interface Window {
    /** Host APIs exposed by muon to pages that pass the plugin allow policy. */
    readonly muon: MuonApi;
  }

  /** Host APIs exposed by muon to the main page. */
  interface MuonApi {
    /** Browser and native window operations for the current muon window. */
    readonly browser: MuonBrowserApi;
    /** Launcher update controls exposed by muon. */
    readonly launcher: MuonLauncherApi;
    /** Environment information exposed by muon. */
    readonly environments: MuonEnvironmentsApi;
    /** Child process execution operations exposed by muon. */
    readonly executor: MuonExecutorApi;
    /** Filesystem operations exposed by the built-in muon filesystem plugin. */
    readonly fs: MuonFsApi;
  }

  /**
   * JavaScript proxy for a native function returned by a muon plugin.
   *
   * @typeParam TArgs - Arguments accepted by the native function.
   * @typeParam TResult - Value produced by the native function.
   * @remarks Calls are asynchronous and return a promise. Release the proxy
   * deterministically when it is no longer needed. Calling a released proxy
   * returns a rejected promise, and passing it back to a plugin is a validation
   * error.
   */
  interface MuonPluginFunctionProxy<
    TArgs extends readonly unknown[],
    TResult,
  > extends Releaseable {
    /**
     * Invoke the native function.
     *
     * @param args - Arguments passed to the native function.
     * @returns A promise for the native function result.
     */
    (...args: TArgs): Promise<TResult>;
  }

  /** Synchronously releasable resource handle. */
  interface Releaseable extends Disposable {
    /**
     * Release the resource.
     *
     * @remarks Release is synchronous and idempotent. This method and
     * `[Symbol.dispose]` perform the same operation.
     */
    readonly release: () => void;
  }

  /** Asynchronously releasable resource handle. */
  interface AsyncReleaseable extends AsyncDisposable {
    /**
     * Release the resource.
     *
     * @returns A promise that resolves after the resource is released.
     */
    readonly release: () => Promise<void>;
  }

  /** CEF version selection policy used by muon-launcher. */
  type MuonCefVersionPolicy =
    | "tested"
    | "same-major-latest"
    | "compat-latest"
    | "exact";

  /** Launcher settings used on the next muon-launcher startup. */
  interface MuonLauncherSettings {
    /**
     * CEF version selection policy.
     *
     * @defaultValue The configured launcher default version policy, normally `"tested"`.
     */
    readonly cefVersionPolicy?: MuonCefVersionPolicy;
    /**
     * Exact CEF version used when cefVersionPolicy is exact.
     *
     * @defaultValue `""`
     */
    readonly cefExactVersion?: string;
    /**
     * Minimum seconds between automatic CEF catalog refresh attempts.
     *
     * @defaultValue `604800`
     */
    readonly catalogRefreshIntervalSeconds?: number;
  }

  /** Partial launcher settings update. */
  interface MuonLauncherSettingsPatch {
    /**
     * CEF version selection policy.
     *
     * @remarks Set `null` to restore the default version policy.
     * @defaultValue Omit to keep the current setting.
     */
    readonly cefVersionPolicy?: MuonCefVersionPolicy | null;
    /**
     * Exact CEF version used when cefVersionPolicy is exact.
     *
     * @remarks Set `null` to restore the default empty value.
     * @defaultValue Omit to keep the current setting.
     */
    readonly cefExactVersion?: string | null;
    /**
     * Minimum seconds between automatic CEF catalog refresh attempts.
     *
     * @remarks Set `null` to restore the default interval.
     * @defaultValue Omit to keep the current setting.
     */
    readonly catalogRefreshIntervalSeconds?: number | null;
  }

  /** Launcher update controls exposed by muon. */
  interface MuonLauncherApi {
    /**
     * Return launcher settings used by the next muon-launcher startup.
     *
     * @returns A promise for the effective launcher settings.
     */
    readonly getSettings: () => Promise<MuonLauncherSettings>;
    /**
     * Update launcher settings used by the next muon-launcher startup.
     *
     * @param settings - Partial settings to persist. Null values clear explicit settings.
     * @returns A promise that resolves when settings are written.
     */
    readonly setSettings: (
      settings?: MuonLauncherSettingsPatch,
    ) => Promise<void>;
    /**
     * Request a CEF catalog refresh on the next muon-launcher startup.
     *
     * @returns A promise that resolves when the refresh request is persisted.
     */
    readonly triggerUpdate: () => Promise<void>;
  }

  /** Top-level muon window bounds in DIP screen coordinates. */
  interface MuonWindowBounds {
    /** Left edge of the window in screen coordinates. */
    readonly x: number;
    /** Top edge of the window in screen coordinates. */
    readonly y: number;
    /** Width of the top-level window. */
    readonly width: number;
    /** Height of the top-level window. */
    readonly height: number;
  }

  /** Placement slot for muon browser context menu items. */
  type MuonBrowserContextMenuPlacement = "start" | "afterEdit" | "end";

  /** Visibility conditions for a muon browser context menu item. */
  interface MuonBrowserContextMenuWhen {
    /** Match editable targets when specified. */
    readonly editable?: boolean;
    /** Match text selection state when specified. */
    readonly selection?: boolean;
    /** Match link targets when specified. */
    readonly link?: boolean;
    /** Match image targets when specified. */
    readonly image?: boolean;
    /** Match copy availability when specified. */
    readonly canCopy?: boolean;
    /** Match paste availability when specified. */
    readonly canPaste?: boolean;
  }

  /** Command item inserted into the muon browser native context menu. */
  interface MuonBrowserContextMenuCommandItem {
    /**
     * Item type. Omit for normal command items.
     *
     * @defaultValue `"item"`
     */
    readonly type?: "item";
    /**
     * Application command id.
     *
     * @remarks Must be non-empty, must not contain control characters, and
     * must not start with `muon.` or `standard.`.
     */
    readonly id: string;
    /** User-visible command label. */
    readonly label: string;
    /**
     * Whether the command can be selected.
     *
     * @defaultValue `true`
     */
    readonly enabled?: boolean;
    /**
     * Placement slot relative to standard CEF menu groups.
     *
     * @defaultValue `"end"`
     */
    readonly placement?: MuonBrowserContextMenuPlacement;
    /** Optional visibility conditions. */
    readonly when?: MuonBrowserContextMenuWhen;
  }

  /** Separator inserted into the muon browser native context menu. */
  interface MuonBrowserContextMenuSeparatorItem {
    /** Separator item type. */
    readonly type: "separator";
    /**
     * Placement slot relative to standard CEF menu groups.
     *
     * @defaultValue `"end"`
     */
    readonly placement?: MuonBrowserContextMenuPlacement;
    /** Optional visibility conditions. */
    readonly when?: MuonBrowserContextMenuWhen;
  }

  /** Item inserted into the muon browser native context menu. */
  type MuonBrowserContextMenuItem =
    | MuonBrowserContextMenuCommandItem
    | MuonBrowserContextMenuSeparatorItem;

  /** Context captured when a muon browser context menu command is selected. */
  interface MuonBrowserContextMenuCommand {
    /** Application command id selected by the user. */
    readonly id: string;
    /** X coordinate in the page view where the menu was invoked. */
    readonly x: number;
    /** Y coordinate in the page view where the menu was invoked. */
    readonly y: number;
    /** Top-level page URL reported by CEF. */
    readonly pageUrl: string;
    /** Frame URL reported by CEF. */
    readonly frameUrl: string;
    /** Link URL at the invocation target, when present. */
    readonly linkUrl: string;
    /** Source URL for media at the invocation target, when present. */
    readonly sourceUrl: string;
    /** Selection text at the invocation target, when present. */
    readonly selectionText: string;
    /** Whether the target was editable. */
    readonly editable: boolean;
    /** Whether text was selected. */
    readonly selection: boolean;
    /** Whether the target was inside a link. */
    readonly link: boolean;
    /** Whether the target was an image. */
    readonly image: boolean;
    /** Whether copy was available. */
    readonly canCopy: boolean;
    /** Whether paste was available. */
    readonly canPaste: boolean;
  }

  /** Receives muon browser context menu command selections. */
  type MuonBrowserContextMenuHandler = (
    command: MuonBrowserContextMenuCommand,
  ) => void;

  /** Normal command item inserted into a muon browser system tray menu. */
  interface MuonBrowserTrayCommandMenuItem {
    /**
     * Item type. Omit for normal command items.
     *
     * @defaultValue `"item"`
     */
    readonly type?: "item";
    /**
     * Application command id.
     *
     * @remarks Must be non-empty, must not contain control characters, and
     * must not start with `muon.` or `standard.`.
     */
    readonly id: string;
    /** User-visible command label. */
    readonly label: string;
    /**
     * Whether the command can be selected.
     *
     * @defaultValue `true`
     */
    readonly enabled?: boolean;
  }

  /** Checkbox command item inserted into a muon browser system tray menu. */
  interface MuonBrowserTrayCheckboxMenuItem {
    /** Checkbox item type. */
    readonly type: "checkbox";
    /**
     * Application command id.
     *
     * @remarks Must be non-empty, must not contain control characters, and
     * must not start with `muon.` or `standard.`.
     */
    readonly id: string;
    /** User-visible command label. */
    readonly label: string;
    /**
     * Whether the command can be selected.
     *
     * @defaultValue `true`
     */
    readonly enabled?: boolean;
    /**
     * Whether the item is checked.
     *
     * @defaultValue `false`
     */
    readonly checked?: boolean;
  }

  /** Radio command item inserted into a muon browser system tray menu. */
  interface MuonBrowserTrayRadioMenuItem {
    /** Radio item type. */
    readonly type: "radio";
    /**
     * Application command id.
     *
     * @remarks Must be non-empty, must not contain control characters, and
     * must not start with `muon.` or `standard.`.
     */
    readonly id: string;
    /** User-visible command label. */
    readonly label: string;
    /**
     * Whether the command can be selected.
     *
     * @defaultValue `true`
     */
    readonly enabled?: boolean;
    /**
     * Whether the item is checked.
     *
     * @defaultValue `false`
     */
    readonly checked?: boolean;
  }

  /** Separator inserted into a muon browser system tray menu. */
  interface MuonBrowserTraySeparatorMenuItem {
    /** Separator item type. */
    readonly type: "separator";
  }

  /** Item inserted into a muon browser system tray menu. */
  type MuonBrowserTrayMenuItem =
    | MuonBrowserTrayCommandMenuItem
    | MuonBrowserTrayCheckboxMenuItem
    | MuonBrowserTrayRadioMenuItem
    | MuonBrowserTraySeparatorMenuItem;

  /** Options used when creating a muon browser system tray item. */
  interface MuonBrowserTrayOptions {
    /**
     * Browser-scoped tray id.
     *
     * @remarks When omitted, muon generates and returns a unique id.
     */
    readonly id?: string;
    /**
     * Tray icon asset path.
     *
     * @remarks Accepts `asset://main/...` URLs or `main`-relative asset paths.
     * Native tray icons accept PNG icons. Omit to follow the current title bar
     * icon.
     */
    readonly icon?: string;
    /** Tooltip text shown by the platform shell when supported. */
    readonly tooltip?: string | null;
    /** Initial tray menu items. */
    readonly menu?: readonly MuonBrowserTrayMenuItem[];
  }

  /** Primary or secondary activation event from a muon browser system tray item. */
  interface MuonBrowserTrayActivationEvent {
    /** Event type. */
    readonly type: "activate" | "secondaryActivate";
    /** Browser-scoped tray id. */
    readonly trayId: string;
    /** Screen X coordinate provided by the platform shell. */
    readonly x: number;
    /** Screen Y coordinate provided by the platform shell. */
    readonly y: number;
  }

  /** Menu command event from a muon browser system tray item. */
  interface MuonBrowserTrayMenuEvent {
    /** Event type. */
    readonly type: "menu";
    /** Browser-scoped tray id. */
    readonly trayId: string;
    /** Application command id selected by the user. */
    readonly id: string;
    /** Current checked state for checkbox and radio menu items. */
    readonly checked: boolean;
  }

  /** Event emitted by a muon browser system tray item. */
  type MuonBrowserTrayEvent =
    | MuonBrowserTrayActivationEvent
    | MuonBrowserTrayMenuEvent;

  /** Receives muon browser system tray activation and menu events. */
  type MuonBrowserTrayEventHandler = (event: MuonBrowserTrayEvent) => void;

  /** Browser and native window operations for the current muon window. */
  interface MuonBrowserApi {
    /**
     * Reload the current page.
     *
     * @returns A promise that resolves after the reload request is submitted.
     * @remarks The page context can be destroyed by navigation before callers
     * can observe promise settlement.
     */
    readonly reload: () => Promise<void>;
    /**
     * Reload the current page while ignoring cached data.
     *
     * @returns A promise that resolves after the hard reload request is submitted.
     * @remarks The page context can be destroyed by navigation before callers
     * can observe promise settlement.
     */
    readonly hardReload: () => Promise<void>;
    /**
     * Toggle fullscreen mode for the current browser window.
     *
     * @returns A promise that resolves when the fullscreen toggle is requested.
     */
    readonly toggleFullscreen: () => Promise<void>;
    /**
     * Enter fullscreen mode for the current browser window.
     *
     * @returns A promise that resolves when fullscreen mode is requested.
     */
    readonly enterFullscreen: () => Promise<void>;
    /**
     * Exit fullscreen mode for the current browser window.
     *
     * @returns A promise that resolves when leaving fullscreen is requested.
     */
    readonly exitFullscreen: () => Promise<void>;
    /**
     * Increase the current page zoom level.
     *
     * @returns A promise that resolves when the zoom change is requested.
     */
    readonly zoomIn: () => Promise<void>;
    /**
     * Decrease the current page zoom level.
     *
     * @returns A promise that resolves when the zoom change is requested.
     */
    readonly zoomOut: () => Promise<void>;
    /**
     * Reset the current page zoom level.
     *
     * @returns A promise that resolves when the zoom reset is requested.
     */
    readonly resetZoom: () => Promise<void>;
    /**
     * Show the current browser window.
     *
     * @returns A promise that resolves when showing the window is requested.
     */
    readonly show: () => Promise<void>;
    /**
     * Hide the current browser window.
     *
     * @returns A promise that resolves when hiding the window is requested.
     */
    readonly hide: () => Promise<void>;
    /**
     * Focus and activate the current browser window.
     *
     * @returns A promise that resolves when focusing the window is requested.
     */
    readonly focus: () => Promise<void>;
    /**
     * Deactivate the current browser window.
     *
     * @returns A promise that resolves when blurring the window is requested.
     */
    readonly blur: () => Promise<void>;
    /**
     * Minimize the current browser window.
     *
     * @returns A promise that resolves when minimizing the window is requested.
     */
    readonly minimize: () => Promise<void>;
    /**
     * Maximize the current browser window.
     *
     * @returns A promise that resolves when maximizing the window is requested.
     */
    readonly maximize: () => Promise<void>;
    /**
     * Restore the current browser window from minimized or maximized state.
     *
     * @returns A promise that resolves when restoring the window is requested.
     */
    readonly restore: () => Promise<void>;
    /**
     * Return bounds for the current top-level muon window.
     *
     * @returns A promise for window bounds in DIP screen coordinates.
     * @remarks The bounds describe the top-level window, not just the browser
     * content area, and therefore include muon custom title bars or native
     * window frames when present.
     */
    readonly getWindowBounds: () => Promise<MuonWindowBounds>;
    /**
     * Request new bounds for the current top-level muon window.
     *
     * @param bounds - New window bounds in DIP screen coordinates.
     * @returns A promise that resolves when the bounds change is requested.
     * @remarks `x`, `y`, `width`, and `height` must be safe integers in the
     * signed 32-bit range. `width` and `height` must be greater than zero.
     * Wayland compositors may ignore or adjust requested position and size.
     */
    readonly setWindowBounds: (bounds: MuonWindowBounds) => Promise<void>;
    /**
     * Replace the current browser window's custom native context menu items.
     *
     * @param items - Command and separator items to insert.
     * @param handler - Optional callback for command selections.
     * @returns A promise that resolves when the registration is stored.
     * @remarks The registration is browser-scoped and is replaced by the next
     * call. It is cleared on main-frame navigation, browser close, or
     * `clearContextMenuItems()`.
     */
    readonly setContextMenuItems: (
      items: readonly MuonBrowserContextMenuItem[],
      handler?: MuonBrowserContextMenuHandler,
    ) => Promise<void>;
    /**
     * Clear custom native context menu items for the current browser window.
     *
     * @returns A promise that resolves when the registration is cleared.
     */
    readonly clearContextMenuItems: () => Promise<void>;
    /**
     * Create a browser-owned system tray item.
     *
     * @param options - Tray icon, tooltip, menu, and optional id.
     * @param handler - Optional callback for activation and menu events.
     * @returns A promise for the normalized or generated tray id.
     * @remarks The tray item is owned by the current browser. It is removed on
     * main-frame navigation, browser close, or `removeTray()`. Hidden initial
     * windows can create tray items while the browser stays alive.
     */
    readonly createTray: (
      options: MuonBrowserTrayOptions,
      handler?: MuonBrowserTrayEventHandler,
    ) => Promise<string>;
    /**
     * Replace menu items for a browser-owned system tray item.
     *
     * @param id - Browser-scoped tray id.
     * @param items - Command and separator items.
     * @param handler - Optional callback replacing the current tray handler.
     * @returns A promise that resolves when the menu is updated.
     */
    readonly setTrayMenu: (
      id: string,
      items: readonly MuonBrowserTrayMenuItem[],
      handler?: MuonBrowserTrayEventHandler,
    ) => Promise<void>;
    /**
     * Replace the icon for a browser-owned system tray item.
     *
     * @param id - Browser-scoped tray id.
     * @param iconPath - PNG icon asset path.
     * @returns A promise that resolves when the icon is updated.
     */
    readonly setTrayIcon: (id: string, iconPath: string) => Promise<void>;
    /**
     * Replace or clear the tooltip for a browser-owned system tray item.
     *
     * @param id - Browser-scoped tray id.
     * @param tooltip - Tooltip text, or `null` to clear it.
     * @returns A promise that resolves when the tooltip is updated.
     */
    readonly setTrayTooltip: (
      id: string,
      tooltip: string | null,
    ) => Promise<void>;
    /**
     * Remove a browser-owned system tray item.
     *
     * @param id - Browser-scoped tray id.
     * @returns A promise that resolves when the tray item is removed.
     */
    readonly removeTray: (id: string) => Promise<void>;
    /**
     * Show or hide the current muon custom title bar.
     *
     * @param visible - Whether the title bar should be visible.
     * @returns A promise that resolves when the title bar visibility is requested.
     * @remarks Native title bars are not affected.
     */
    readonly setTitleBarVisibility: (visible: boolean) => Promise<void>;
    /**
     * Set or clear the current window title bar icon.
     *
     * @param path - Asset path for an icon, or `null` to clear the icon.
     * @returns A promise that resolves when the title bar icon is requested.
     * @remarks Accepts `asset://main/...` URLs or `main`-relative asset paths.
     * The icon is loaded from the configured muon asset storage. muon custom
     * title bars accept browser-displayable image formats. Native title bars
     * accept PNG icons only and reject other formats.
     */
    readonly setTitleBarIcon: (path: string | null) => Promise<void>;
    /**
     * Close the current browser window.
     *
     * @returns A promise that resolves after the close request is submitted.
     * @remarks Modal filesystem dialogs owned by this browser are aborted before
     * closing. The page context can be destroyed before callers can observe
     * promise settlement.
     */
    readonly close: () => Promise<void>;
    /**
     * Shut down the muon process.
     *
     * @param exitCode - Process exit code. Omit to use `0`.
     * @returns A promise that resolves after the shutdown request is submitted.
     */
    readonly shutdown: (exitCode?: number) => Promise<void>;
    /**
     * Recycle the muon process by requesting an automatic restart.
     *
     * @returns A promise that resolves after the recycle request is submitted.
     * @remarks The page context can be destroyed by process shutdown before
     * callers can observe promise settlement.
     */
    readonly recycle: () => Promise<void>;
  }

  /**
   * Filesystem operations exposed by the built-in muon filesystem plugin.
   *
   * @remarks Linux accepts local paths or GIO/GVfs URI values for filesystem
   * location arguments. Other platforms accept local filesystem paths.
   */
  interface MuonFsApi {
    /**
     * Read a file as binary data.
     *
     * @param path - Filesystem path, or a URI on Linux, to read.
     * @param options - Optional read range and abort signal.
     * @returns A promise for an `ArrayBuffer` containing the selected bytes.
     * @remarks `options.position` and `options.length` must be non-negative
     * safe integers. The native per-operation limit is configured by the
     * internal plugin's `fs.readFile.maxBytes` string value and defaults to
     * 67108864 bytes (64 MiB). An explicit `length` above the limit rejects
     * before source access. When `length` is omitted, all bytes from `position`
     * through the end of the file must fit within the limit; muon never
     * silently truncates the result. When `position` is past the end of the
     * file, the returned buffer is empty. The limit applies only to `readFile`,
     * not to `readTextFile` or an aggregate of concurrent operations.
     */
    readonly readFile: (
      path: string,
      options?: MuonFsReadFileOptions,
    ) => Promise<ArrayBuffer>;
    /**
     * Write binary data to a file.
     *
     * @param path - Filesystem path, or a URI on Linux, to write.
     * @param data - Binary data to write.
     * @param options - Optional write offset and abort signal.
     * @returns A promise that resolves when the write completes.
     * @remarks Omitting `options.position` replaces the whole file. Supplying
     * `options.position` writes at that byte offset and creates the file when it
     * does not exist.
     */
    readonly writeFile: (
      path: string,
      data: BufferSource,
      options?: MuonFsWriteFileOptions,
    ) => Promise<void>;
    /**
     * Read a UTF-8 text file.
     *
     * @param path - Filesystem path, or a URI on Linux, to read.
     * @param encoding - Text encoding. Only `"utf8"` and `"utf-8"` are supported.
     * @param options - Optional abort signal.
     * @returns A promise for the decoded text.
     * @remarks The file must contain valid UTF-8 text without NUL bytes.
     */
    readonly readTextFile: (
      path: string,
      encoding: "utf8" | "utf-8",
      options?: MuonFsOperationOptions,
    ) => Promise<string>;
    /**
     * Write a UTF-8 text file.
     *
     * @param path - Filesystem path, or a URI on Linux, to write.
     * @param data - Text to write.
     * @param encoding - Text encoding. Only `"utf8"` and `"utf-8"` are supported.
     * @param options - Optional abort signal.
     * @returns A promise that resolves when the write completes.
     * @remarks The whole file is replaced.
     */
    readonly writeTextFile: (
      path: string,
      data: string,
      encoding: "utf8" | "utf-8",
      options?: MuonFsOperationOptions,
    ) => Promise<void>;
    /**
     * Return metadata for a path, following symbolic links.
     *
     * @param path - Filesystem path, or a URI on Linux, to inspect.
     * @param options - Optional abort signal.
     * @returns A promise for filesystem metadata.
     * @remarks Rejects when the path does not exist.
     */
    readonly stat: (
      path: string,
      options?: MuonFsOperationOptions,
    ) => Promise<MuonFsStats>;
    /**
     * Return metadata for a path without following symbolic links.
     *
     * @param path - Filesystem path, or a URI on Linux, to inspect.
     * @param options - Optional abort signal.
     * @returns A promise for filesystem metadata.
     * @remarks A symbolic link is reported as type `"symlink"` instead of the
     * target entry type.
     */
    readonly lstat: (
      path: string,
      options?: MuonFsOperationOptions,
    ) => Promise<MuonFsStats>;
    /**
     * Return whether a path exists.
     *
     * @param path - Filesystem path, or a URI on Linux, to check.
     * @param options - Optional abort signal.
     * @returns A promise for `true` when the path exists and can be inspected.
     * @remarks Filesystem errors are reported as `false`.
     */
    readonly exists: (
      path: string,
      options?: MuonFsOperationOptions,
    ) => Promise<boolean>;
    /**
     * Return whether a path is accessible for the requested modes.
     *
     * @param path - Filesystem path, or a URI on Linux, to check.
     * @param options - Optional access modes and abort signal.
     * @returns A promise for `true` when all requested modes are allowed.
     * @remarks Omitting `options.mode` checks existence only. Permission checks
     * are based on filesystem permission bits.
     */
    readonly access: (
      path: string,
      options?: MuonFsAccessOptions,
    ) => Promise<boolean>;
    /**
     * Read directory entry names.
     *
     * @param path - Directory path, or a URI on Linux, to read.
     * @param options - Optional directory read options and abort signal.
     * @returns A promise for entry names relative to `path`.
     * @remarks This overload is selected when `withFileTypes` is omitted or false.
     */
    readdir(
      path: string,
      options?: MuonFsReadDirectoryOptions & { withFileTypes?: false },
    ): Promise<string[]>;
    /**
     * Read directory entries with metadata helpers.
     *
     * @param path - Directory path, or a URI on Linux, to read.
     * @param options - Directory read options with `withFileTypes: true`.
     * @returns A promise for directory entries with metadata helper methods.
     */
    readdir(
      path: string,
      options: MuonFsReadDirectoryOptions & { withFileTypes: true },
    ): Promise<MuonFsDirent[]>;
    /**
     * Create a directory.
     *
     * @param path - Directory path, or a URI on Linux, to create.
     * @param options - Optional recursive flag and abort signal.
     * @returns A promise that resolves when the directory is created.
     * @remarks With `recursive: false` or omitted, rejects if the path already
     * exists or a parent is missing.
     */
    readonly mkdir: (
      path: string,
      options?: MuonFsMakeDirectoryOptions,
    ) => Promise<void>;
    /**
     * Remove a file or directory.
     *
     * @param path - Filesystem path, or a URI on Linux, to remove.
     * @param options - Optional recursive, force, and abort options.
     * @returns A promise that resolves when removal completes.
     * @remarks Directories require `recursive: true`. `force: true` suppresses a
     * missing-path error.
     */
    readonly rm: (path: string, options?: MuonFsRemoveOptions) => Promise<void>;
    /**
     * Remove a file or symbolic link.
     *
     * @param path - File or symbolic link path, or a URI on Linux, to remove.
     * @param options - Optional abort signal.
     * @returns A promise that resolves when removal completes.
     * @remarks Rejects when `path` is a directory.
     */
    readonly unlink: (
      path: string,
      options?: MuonFsOperationOptions,
    ) => Promise<void>;
    /**
     * Remove an empty directory.
     *
     * @param path - Directory path, or a URI on Linux, to remove.
     * @param options - Optional abort signal.
     * @returns A promise that resolves when the directory is removed.
     * @remarks Rejects when `path` is not an empty directory.
     */
    readonly rmdir: (
      path: string,
      options?: MuonFsOperationOptions,
    ) => Promise<void>;
    /**
     * Rename or move a path.
     *
     * @param oldPath - Existing filesystem path, or a URI on Linux.
     * @param newPath - Destination filesystem path, or a URI on Linux.
     * @param options - Optional abort signal.
     * @returns A promise that resolves when the path is renamed.
     */
    readonly rename: (
      oldPath: string,
      newPath: string,
      options?: MuonFsOperationOptions,
    ) => Promise<void>;
    /**
     * Copy a regular file.
     *
     * @param source - Existing regular file path, or a URI on Linux.
     * @param destination - Destination file path, or a URI on Linux.
     * @param options - Optional overwrite flag and abort signal.
     * @returns A promise that resolves when the copy completes.
     * @remarks Rejects when `source` is not a regular file. Existing
     * destinations are replaced unless `options.overwrite` is false.
     */
    readonly copyFile: (
      source: string,
      destination: string,
      options?: MuonFsCopyFileOptions,
    ) => Promise<void>;
    /**
     * Append binary data to a file.
     *
     * @param path - Filesystem path, or a URI on Linux, to append to.
     * @param data - Binary data to append.
     * @param options - Optional abort signal.
     * @returns A promise that resolves when append completes.
     * @remarks Creates the file when it does not exist.
     */
    readonly appendFile: (
      path: string,
      data: BufferSource,
      options?: MuonFsOperationOptions,
    ) => Promise<void>;
    /**
     * Append UTF-8 text to a file.
     *
     * @param path - Filesystem path, or a URI on Linux, to append to.
     * @param data - Text to append.
     * @param encoding - Text encoding. Only `"utf8"` and `"utf-8"` are supported.
     * @param options - Optional abort signal.
     * @returns A promise that resolves when append completes.
     * @remarks Creates the file when it does not exist.
     */
    readonly appendTextFile: (
      path: string,
      data: string,
      encoding: "utf8" | "utf-8",
      options?: MuonFsOperationOptions,
    ) => Promise<void>;
    /** Truncate or extend a file. */
    readonly truncate: {
      /**
       * Truncate or extend a file to the requested byte length.
       *
       * @param path - File path, or a URI on Linux, to resize.
       * @param length - Target byte length. Omit to truncate to zero bytes.
       * @param options - Optional abort signal.
       * @returns A promise that resolves when the file size is updated.
       * @remarks `length` must be a non-negative safe integer.
       */
      (
        path: string,
        length?: number,
        options?: MuonFsOperationOptions,
      ): Promise<void>;
      /**
       * Truncate a file to zero bytes using operation options as the second argument.
       *
       * @param path - File path, or a URI on Linux, to resize.
       * @param options - Optional abort signal.
       * @returns A promise that resolves when the file is truncated to zero bytes.
       */
      (path: string, options: MuonFsOperationOptions): Promise<void>;
    };
    /**
     * Resolve a path to its canonical absolute path.
     *
     * @param path - Filesystem path, or a URI on Linux, to resolve.
     * @param options - Optional abort signal.
     * @returns A promise for the canonical absolute path.
     * @remarks Rejects when the path or a path component does not exist.
     */
    readonly realpath: (
      path: string,
      options?: MuonFsOperationOptions,
    ) => Promise<string>;
    /**
     * Read the target of a symbolic link.
     *
     * @param path - Symbolic link path, or a URI on Linux, to inspect.
     * @param options - Optional abort signal.
     * @returns A promise for the symbolic link target path.
     */
    readonly readlink: (
      path: string,
      options?: MuonFsOperationOptions,
    ) => Promise<string>;
    /** Create a symbolic link. */
    readonly symlink: {
      /**
       * Create a symbolic link.
       *
       * @param target - Link target path string.
       * @param path - Symbolic link path, or a URI on Linux, to create.
       * @param type - Symbolic link kind. Omit to create a file link.
       * @param options - Optional abort signal.
       * @returns A promise that resolves when the link is created.
       * @remarks `"dir"` and `"junction"` create a directory link. On Linux,
       * only `path` is interpreted as a path-or-URI location; `target` is the
       * symlink target string.
       */
      (
        target: string,
        path: string,
        type?: MuonFsSymlinkType,
        options?: MuonFsOperationOptions,
      ): Promise<void>;
      /**
       * Create a file symbolic link using operation options as the third argument.
       *
       * @param target - Link target path string.
       * @param path - Symbolic link path, or a URI on Linux, to create.
       * @param options - Optional abort signal.
       * @returns A promise that resolves when the link is created.
       * @remarks On Linux, only `path` is interpreted as a path-or-URI
       * location; `target` is the symlink target string.
       */
      (
        target: string,
        path: string,
        options: MuonFsOperationOptions,
      ): Promise<void>;
    };
    /**
     * Watch a path for changes until the returned watcher is closed.
     *
     * @param path - File or directory path, or a URI on Linux, to watch.
     * @param listener - Listener invoked for change, rename, and error events.
     * @param options - Optional abort signal.
     * @returns A promise for a watcher handle.
     * @remarks muon currently watches by polling snapshots. Listener exceptions
     * and rejected promises are ignored. Aborting before the watcher is created
     * rejects; aborting after creation closes the watcher.
     */
    readonly watch: (
      path: string,
      listener: MuonFsWatchListener,
      options?: MuonFsOperationOptions,
    ) => Promise<MuonFsWatcher>;
    /** Native filesystem dialogs exposed by the built-in muon filesystem plugin. */
    readonly dialogs: MuonFsDialogsApi;
  }

  /** Native filesystem dialogs exposed by the built-in muon filesystem plugin. */
  interface MuonFsDialogsApi {
    /**
     * Show a native file open dialog and return the selected local path or URI.
     *
     * @param options - Optional native dialog options and abort signal.
     * @returns A promise for the selected path or URI, or `null` when canceled.
     * @remarks GTK can return URI values when `options.gtk.localOnly` is false.
     */
    readonly selectFile: (
      options?: MuonFsOpenDialogOptions,
    ) => Promise<string | null>;
    /**
     * Show a native multi-file open dialog and return selected local paths or URIs.
     *
     * @param options - Optional native dialog options and abort signal.
     * @returns A promise for selected paths or URIs, or an empty array when canceled.
     * @remarks GTK can return URI values when `options.gtk.localOnly` is false.
     */
    readonly selectFiles: (
      options?: MuonFsOpenDialogOptions,
    ) => Promise<string[]>;
    /**
     * Show a native directory selection dialog and return the selected local path or URI.
     *
     * @param options - Optional native dialog options and abort signal.
     * @returns A promise for the selected path or URI, or `null` when canceled.
     * @remarks GTK can return URI values when `options.gtk.localOnly` is false.
     */
    readonly selectDirectory: (
      options?: MuonFsOpenDialogOptions,
    ) => Promise<string | null>;
    /**
     * Show a native multi-directory selection dialog and return selected local paths or URIs.
     *
     * @param options - Optional native dialog options and abort signal.
     * @returns A promise for selected paths or URIs, or an empty array when canceled.
     * @remarks GTK can return URI values when `options.gtk.localOnly` is false.
     */
    readonly selectDirectories: (
      options?: MuonFsOpenDialogOptions,
    ) => Promise<string[]>;
    /**
     * Show a native save dialog and return the selected local path or URI.
     *
     * @param options - Optional native dialog options and abort signal.
     * @returns A promise for the selected path or URI, or `null` when canceled.
     * @remarks The dialog only returns a path or URI; it does not create or
     * overwrite the file.
     */
    readonly selectSaveFile: (
      options?: MuonFsSaveDialogOptions,
    ) => Promise<string | null>;
  }

  /** Options shared by built-in filesystem operations. */
  interface MuonFsOperationOptions {
    /**
     * Signal used to abort the filesystem operation.
     *
     * @remarks If the signal is already aborted, the operation rejects with the
     * signal reason or an `AbortError`. If it aborts while the operation is
     * pending, muon requests native cancellation when possible.
     * @defaultValue No abort signal.
     */
    readonly signal?: AbortSignal;
  }

  /** Options for reading a binary file. */
  interface MuonFsReadFileOptions extends MuonFsOperationOptions {
    /**
     * Byte offset where reading starts.
     *
     * @remarks Must be a non-negative safe integer.
     * @defaultValue `0`
     */
    readonly position?: number;
    /**
     * Maximum number of bytes to read.
     *
     * @remarks Must be a non-negative safe integer and no greater than the
     * configured `fs.readFile.maxBytes` limit. Omit to request all bytes through
     * the end of the file; the operation rejects instead of truncating when the
     * remaining byte count exceeds that limit.
     * @defaultValue Reads through the end of the file when the result is within
     * the configured limit.
     */
    readonly length?: number;
  }

  /** Options for writing a binary file. */
  interface MuonFsWriteFileOptions extends MuonFsOperationOptions {
    /**
     * Byte offset where writing starts.
     *
     * @remarks Must be a non-negative safe integer. Omit to replace the whole file.
     * @defaultValue Replaces the whole file.
     */
    readonly position?: number;
  }

  /** Access modes checked by muon filesystem access. */
  type MuonFsAccessMode = "read" | "write" | "execute";

  /** Options for checking path accessibility. */
  interface MuonFsAccessOptions extends MuonFsOperationOptions {
    /**
     * Requested access modes.
     *
     * @remarks Omit to check only existence. Every listed mode must be allowed
     * for `access()` to return true.
     * @defaultValue Checks existence only.
     */
    readonly mode?: readonly MuonFsAccessMode[];
  }

  /** Options for reading a directory. */
  interface MuonFsReadDirectoryOptions extends MuonFsOperationOptions {
    /**
     * Return entries with metadata helpers instead of names.
     *
     * @remarks Use `true` to select the `MuonFsDirent[]` overload.
     * @defaultValue `false`
     */
    readonly withFileTypes?: boolean;
  }

  /** Options for creating a directory. */
  interface MuonFsMakeDirectoryOptions extends MuonFsOperationOptions {
    /**
     * Create parent directories as needed.
     *
     * @defaultValue `false`
     */
    readonly recursive?: boolean;
  }

  /** Options for removing a path. */
  interface MuonFsRemoveOptions extends MuonFsOperationOptions {
    /**
     * Remove directory trees recursively.
     *
     * @defaultValue `false`
     */
    readonly recursive?: boolean;
    /**
     * Do not fail when the path is missing.
     *
     * @defaultValue `false`
     */
    readonly force?: boolean;
  }

  /** Options for copying a regular file. */
  interface MuonFsCopyFileOptions extends MuonFsOperationOptions {
    /**
     * Replace an existing destination file.
     *
     * @defaultValue `true`
     */
    readonly overwrite?: boolean;
  }

  /** File type filter shown in native file dialogs. */
  interface MuonFsDialogFilter {
    /** Human-readable filter name. */
    readonly name: string;
    /**
     * File extensions accepted by this filter.
     *
     * @remarks Entries may be plain extensions such as "png", dotted
     * extensions such as ".png", wildcard patterns such as "*.png", or "*".
     */
    readonly extensions: readonly string[];
  }

  /** GTK-specific native dialog options. */
  interface MuonFsGtkDialogOptions {
    /**
     * Restrict selection to local files.
     *
     * @remarks GTK/GVfs locations can return URI values when this is false.
     * @defaultValue `false`
     */
    readonly localOnly?: boolean;
    /**
     * Allow creating folders from save and folder chooser dialogs.
     *
     * @remarks Applies only when supported by the GTK backend.
     * @defaultValue `true`
     */
    readonly createFolders?: boolean;
    /**
     * Additional GTK MIME type filters.
     *
     * @remarks Entries must be non-empty strings.
     * @defaultValue No additional MIME type filters.
     */
    readonly mimeTypes?: readonly string[];
  }

  /** Win32-specific native dialog options. */
  interface MuonFsWin32DialogOptions {
    /**
     * Force selections to filesystem-backed shell items.
     *
     * @defaultValue `true`
     */
    readonly forceFilesystem?: boolean;
    /**
     * Return shortcut/link items themselves instead of their targets.
     *
     * @defaultValue `false`
     */
    readonly noDereferenceLinks?: boolean;
    /**
     * Do not add selected locations to the recent documents list.
     *
     * @defaultValue `false`
     */
    readonly dontAddToRecent?: boolean;
    /**
     * Allow paths that do not pass normal shell validation.
     *
     * @defaultValue `false`
     */
    readonly noValidate?: boolean;
    /**
     * Restrict typed filenames to the configured file types.
     *
     * @defaultValue `false`
     */
    readonly strictFileTypes?: boolean;
    /**
     * Require selected paths to exist.
     *
     * @defaultValue `false`
     */
    readonly pathMustExist?: boolean;
    /**
     * Require selected files to exist.
     *
     * @defaultValue `false`
     */
    readonly fileMustExist?: boolean;
  }

  /** Common options for native filesystem dialogs. */
  interface MuonFsDialogOptions extends MuonFsOperationOptions {
    /**
     * Dialog title.
     *
     * @defaultValue The native backend default title.
     */
    readonly title?: string;
    /**
     * Initial path or URI shown by the dialog.
     *
     * @remarks GTK accepts GVfs URI values when gtk.localOnly is false.
     * @defaultValue No explicit initial path.
     */
    readonly defaultPath?: string;
    /**
     * Accept button label.
     *
     * @defaultValue The native backend default button label.
     */
    readonly buttonLabel?: string;
    /**
     * Whether to disable the calling browser view while the dialog is open.
     *
     * @defaultValue `true`
     */
    readonly modal?: boolean;
    /**
     * Show hidden files when supported by the backend.
     *
     * @defaultValue `false`
     */
    readonly showHidden?: boolean;
    /**
     * File filters shown by the dialog.
     *
     * @remarks Every filter requires a non-empty name and at least one extension.
     * @defaultValue No file filters.
     */
    readonly filters?: readonly MuonFsDialogFilter[];
    /**
     * GTK-specific dialog flags.
     *
     * @defaultValue GTK option defaults.
     */
    readonly gtk?: MuonFsGtkDialogOptions;
    /**
     * Win32-specific dialog flags.
     *
     * @defaultValue Win32 option defaults.
     */
    readonly win32?: MuonFsWin32DialogOptions;
  }

  /** Options for open and directory selection dialogs. */
  interface MuonFsOpenDialogOptions extends MuonFsDialogOptions {}

  /** Options for save file dialogs. */
  interface MuonFsSaveDialogOptions extends MuonFsDialogOptions {
    /**
     * Initial file name shown by the save dialog.
     *
     * @defaultValue No explicit initial file name.
     */
    readonly defaultName?: string;
    /**
     * Ask before replacing an existing file.
     *
     * @defaultValue `true`
     */
    readonly confirmOverwrite?: boolean;
  }

  /** Filesystem entry type reported by muon. */
  type MuonFsEntryType =
    | "file"
    | "directory"
    | "symlink"
    | "blockDevice"
    | "characterDevice"
    | "fifo"
    | "socket"
    | "other";

  /** Metadata returned for filesystem entries. */
  interface MuonFsStats {
    /** Entry type. */
    readonly type: MuonFsEntryType;
    /** Entry size in bytes for regular files, otherwise zero. */
    readonly size: number;
    /** Last modification time as milliseconds since the Unix epoch. */
    readonly mtimeMs: number;
    /** Whether no write permission bits are set. */
    readonly readonly: boolean;
    /**
     * Return whether this entry is a regular file.
     *
     * @returns `true` when `type` is `"file"`.
     */
    readonly isFile: () => boolean;
    /**
     * Return whether this entry is a directory.
     *
     * @returns `true` when `type` is `"directory"`.
     */
    readonly isDirectory: () => boolean;
    /**
     * Return whether this entry is a symbolic link.
     *
     * @returns `true` when `type` is `"symlink"`.
     */
    readonly isSymbolicLink: () => boolean;
  }

  /** Directory entry with metadata helpers. */
  interface MuonFsDirent extends MuonFsStats {
    /** Entry name relative to the listed directory. */
    readonly name: string;
  }

  /** Symbolic link creation mode. */
  type MuonFsSymlinkType = "file" | "dir" | "junction";

  /** Filesystem watch event delivered to a watch listener. */
  interface MuonFsWatchEvent {
    /** Event kind. */
    readonly eventType: "rename" | "change" | "error";
    /** Changed entry name, or null for the watched path itself. */
    readonly filename: string | null;
    /** Error message when eventType is error. */
    readonly message?: string;
  }

  /**
   * Listener invoked for filesystem watch events.
   *
   * @param event - Watch event emitted by muon.
   * @returns Ignored by muon. Rejected promises are ignored.
   */
  type MuonFsWatchListener = (event: MuonFsWatchEvent) => void | Promise<void>;

  /** Active filesystem watcher. */
  interface MuonFsWatcher {
    /**
     * Stop watching.
     *
     * @returns A promise that resolves after the watcher is closed.
     * @remarks Calling `close()` more than once is allowed.
     */
    readonly close: () => Promise<void>;
  }

  /** Runtime metadata for the current muon process. */
  interface MuonRuntimeInfo {
    /** Runtime package name. */
    readonly name: string;
    /** Native executable file name included in the runtime payload. */
    readonly executableName: string;
    /** muon runtime target name. */
    readonly target: string;
    /** Internal CEF target name used for catalog lookup. */
    readonly cefTarget: string;
    /** muon-core build identity. */
    readonly muonCore: MuonCoreRuntimeInfo;
    /** CEF build selected when muon-core was built. */
    readonly cefReference: MuonCefReferenceInfo;
    /** CEF build loaded by the current process. */
    readonly cefRuntime: MuonCefRuntimeInfo;
    /** Runtime payload entries copied with muon-core. */
    readonly corePayload: readonly string[];
  }

  /** muon-core build identity. */
  interface MuonCoreRuntimeInfo {
    /** muon-core package version embedded at build time. */
    readonly version: string;
    /** Git commit hash embedded at build time. */
    readonly gitCommitHash: string;
    /** Build date embedded at build time. */
    readonly buildDate: string;
    /** Git commit date embedded at build time. */
    readonly gitCommitDate: string;
  }

  /** CEF build selected when muon-core was built. */
  interface MuonCefReferenceInfo {
    /** CEF binary distribution version. */
    readonly version: string;
    /** CEF binary distribution kind. */
    readonly distribution: string;
    /** Stable CEF API version used by muon-core. */
    readonly apiVersion: number;
    /** Platform API hash for the selected CEF API version. */
    readonly apiHash: string;
    /** Downloadable CEF artifact metadata. */
    readonly artifact: MuonCefArtifactInfo;
  }

  /** CEF artifact metadata used by muon prepare. */
  interface MuonCefArtifactInfo {
    /** CEF archive file name. */
    readonly fileName: string;
    /** CEF archive download URL. */
    readonly url: string;
    /** Expected SHA-1 digest for the CEF archive. */
    readonly sha1: string;
    /** Expected CEF archive size in bytes. */
    readonly size: number;
  }

  /** CEF build loaded by the current muon process. */
  interface MuonCefRuntimeInfo {
    /** Runtime CEF version reported by libcef. */
    readonly version: string;
    /** Runtime CEF API version configured by libcef. */
    readonly apiVersion: number;
    /** Runtime platform API hash for the configured CEF API version. */
    readonly apiHash: string;
  }

  /** Environment information exposed by muon. */
  interface MuonEnvironmentsApi {
    /**
     * Return the current process environment variables.
     *
     * @returns A promise for a key-value map of environment variables.
     */
    readonly getVariables: () => Promise<Record<string, string>>;
    /**
     * Return the command line captured when the current muon process started.
     *
     * @returns A promise for the process command line, including `argv[0]` when available.
     */
    readonly getCommandLine: () => Promise<string[]>;
    /**
     * Return the native muon process id.
     *
     * @returns A promise for the current process id.
     */
    readonly getProcessId: () => Promise<number>;
    /**
     * Return runtime metadata for the current muon process.
     *
     * @returns A promise for build-time muon-core metadata and runtime CEF metadata.
     */
    readonly getRuntimeInfo: () => Promise<MuonRuntimeInfo>;
    /**
     * Return whether the current muon application starts with the user session.
     *
     * @returns A promise for the autostart state, or `undefined` when unknown.
     * @remarks The active launch source selects the platform backend. muon uses
     * XDG Autostart on POSIX desktop environments and the current user's Run
     * registry entry on Windows.
     */
    readonly getAutostart: () => Promise<boolean | undefined>;
    /**
     * Enable or disable startup with the user session.
     *
     * @param enabled - Whether autostart should be enabled.
     * @returns A promise that resolves when the platform backend updates the setting.
     */
    readonly setAutostart: (enabled: boolean) => Promise<void>;
  }

  /** Child process execution operations exposed by muon. */
  interface MuonExecutorApi {
    /**
     * Spawn a child process without invoking a shell.
     *
     * @param options - Process launch options.
     * @returns A promise for the started child process handle.
     * @remarks The returned process handle controls stdin, termination, and
     * completion observation. Use `wait()` to receive the exit result.
     */
    readonly spawn: (
      options: MuonExecutorSpawnOptions,
    ) => Promise<MuonExecutorProcess>;
    /**
     * Load a native dynamic library for ad-hoc FFI calls.
     *
     * @param path - Path or platform loader name for the library.
     * @returns A promise for the loaded library handle.
     * @remarks Calls made through the returned handle are executed on temporary
     * worker threads. Pointer values are opaque and are not owned by muon.
     */
    readonly loadLibrary: (path: string) => Promise<MuonAdhocLibrary>;
    /** Native `void` type descriptor. */
    readonly voidType: MuonAdhocType<void>;
    /** Native `bool` type descriptor. */
    readonly boolType: MuonAdhocType<boolean>;
    /** Native signed 8-bit integer type descriptor. */
    readonly int8Type: MuonAdhocType<number>;
    /** Native unsigned 8-bit integer type descriptor. */
    readonly uint8Type: MuonAdhocType<number>;
    /** Native signed 16-bit integer type descriptor. */
    readonly int16Type: MuonAdhocType<number>;
    /** Native unsigned 16-bit integer type descriptor. */
    readonly uint16Type: MuonAdhocType<number>;
    /** Native signed 32-bit integer type descriptor. */
    readonly int32Type: MuonAdhocType<number>;
    /** Native unsigned 32-bit integer type descriptor. */
    readonly uint32Type: MuonAdhocType<number>;
    /** Native signed 64-bit integer type descriptor. */
    readonly int64Type: MuonAdhocType<MuonAdhocIntegerValue>;
    /** Native unsigned 64-bit integer type descriptor. */
    readonly uint64Type: MuonAdhocType<MuonAdhocIntegerValue>;
    /** Native 32-bit floating-point type descriptor. */
    readonly float32Type: MuonAdhocType<number>;
    /** Native 64-bit floating-point type descriptor. */
    readonly float64Type: MuonAdhocType<number>;
    /** Native UTF-8 string pointer type descriptor. */
    readonly stringType: MuonAdhocType<string | null>;
    /** Native pointer type descriptor. */
    readonly pointerType: MuonAdhocType<MuonNativePointer | null>;
    /** Native mutable byte buffer view type descriptor. */
    readonly bufferViewType: MuonAdhocType<Uint8Array>;
    /** Native `size_t` type descriptor. */
    readonly usizeType: MuonAdhocType<MuonAdhocIntegerValue>;
  }

  /** Integer value accepted by ad-hoc native calls. */
  type MuonAdhocIntegerValue = number | bigint | string;

  /** Opaque native pointer value returned by ad-hoc native calls. */
  interface MuonNativePointer {
    /** Decimal string representation of the native address. */
    readonly value: string;
    /**
     * Return the decimal string representation of the native address.
     *
     * @returns The native address as a decimal string.
     */
    readonly toString: () => string;
  }

  /** Type descriptor used by ad-hoc native signatures. */
  interface MuonAdhocType<TValue = unknown> {
    /** Stable type name consumed by the muon executor runtime. */
    readonly name: string;
  }

  /** Function signature used by ad-hoc native calls. */
  interface MuonAdhocSignature {
    /** Native argument type descriptors. */
    readonly argTypes: readonly MuonAdhocType[];
    /** Native return type descriptor. */
    readonly returnType: MuonAdhocType;
  }

  /** Dynamic library handle used by ad-hoc native calls. */
  interface MuonAdhocLibrary extends AsyncReleaseable {
    /**
     * Resolve a native function from the loaded library.
     *
     * @param name - Native symbol name.
     * @param signature - Native function signature.
     * @returns A promise for an async JavaScript proxy function.
     * @remarks The proxy function does not own pointer return values.
     */
    readonly getFunction: <T extends (...args: any[]) => Promise<unknown>>(
      name: string,
      signature: MuonAdhocSignature,
    ) => Promise<T>;
    /**
     * Release the native library handle.
     *
     * @returns A promise that resolves after in-flight calls finish and the
     * library is unloaded.
     */
    readonly release: () => Promise<void>;
  }

  /** Options for spawning a child process through muon. */
  interface MuonExecutorSpawnOptions {
    /**
     * Executable path or executable name resolved through `PATH`.
     *
     * @remarks Required, non-empty, and must not contain NUL.
     */
    readonly command: string;
    /**
     * Command line arguments passed as separate values.
     *
     * @remarks Values are not interpreted by a shell and must not contain NUL.
     * @defaultValue `[]`
     */
    readonly args?: readonly string[];
    /**
     * Receives stdout chunks as the child process writes them.
     *
     * @remarks When specified, `wait()` omits `stdout` from its result. The
     * callback receives a fresh `Uint8Array` for each chunk.
     */
    readonly onStdout?: (chunk: Uint8Array) => void;
    /**
     * Receives stderr chunks as the child process writes them.
     *
     * @remarks When specified, `wait()` omits `stderr` from its result. The
     * callback receives a fresh `Uint8Array` for each chunk.
     */
    readonly onStderr?: (chunk: Uint8Array) => void;
    /**
     * Working directory used for the child process.
     *
     * @remarks Must not contain NUL. On POSIX, failure to change directory makes
     * the child exit with code `126`.
     * @defaultValue The current process working directory.
     */
    readonly cwd?: string;
    /**
     * Environment variable overrides for the child process.
     *
     * @remarks Entries are merged with the current process environment. Keys
     * must be non-empty and must not contain `=` or NUL; values must not contain
     * NUL.
     * @defaultValue No environment overrides.
     */
    readonly env?: Record<string, string>;
  }

  /** Started child process handle. */
  interface MuonExecutorProcess extends AsyncReleaseable {
    /** Child process id. */
    readonly processId: number;
    /**
     * Write bytes to the child process stdin.
     *
     * @param data - UTF-8 text or raw bytes to write.
     * @returns A promise that resolves after the bytes are written.
     * @remarks Calls are processed in call order. Strings are UTF-8 encoded.
     */
    readonly writeStdin: (data: string | BufferSource) => Promise<void>;
    /**
     * Close child process stdin after all pending writes are processed.
     *
     * @returns A promise that resolves when stdin is closed.
     */
    readonly closeStdin: () => Promise<void>;
    /**
     * Wait for the child process to exit.
     *
     * @returns A promise for the completed child process result.
     * @remarks The same promise is reused for repeated calls. It resolves even
     * when the child exits with a non-zero exit code.
     */
    readonly wait: () => Promise<MuonExecutorSpawnResult>;
    /**
     * Request process termination.
     *
     * @returns A promise that resolves when the termination request is issued.
     * @remarks POSIX uses `SIGTERM`; Windows uses `TerminateProcess(..., 1)`.
     */
    readonly kill: () => Promise<void>;
    /**
     * Release the native handle and terminate the process when it is still
     * running.
     *
     * @returns A promise that resolves after release is requested.
     */
    readonly release: () => Promise<void>;
  }

  /** Completed child process result. */
  interface MuonExecutorSpawnResult {
    /** Child process id. */
    readonly processId: number;
    /** Child process exit code. */
    readonly exitCode: number;
    /** Captured stdout bytes when `onStdout` was not specified. */
    readonly stdout?: Uint8Array;
    /** Captured stderr bytes when `onStderr` was not specified. */
    readonly stderr?: Uint8Array;
  }
}
