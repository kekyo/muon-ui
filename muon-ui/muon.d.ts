// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

export {};

declare global {
  /** Browser window object extended with the Muon host API. */
  interface Window {
    /** Host APIs exposed by Muon to pages that pass the plugin allow policy. */
    readonly muon: MuonApi;
  }

  /** Host APIs exposed by Muon to the main page. */
  interface MuonApi {
    /** Browser and native window operations for the current Muon window. */
    readonly browser: MuonBrowserApi;
    /** Bootstrap update controls exposed by Muon. */
    readonly bootstrap: MuonBootstrapApi;
    /** Environment information exposed by Muon. */
    readonly environments: MuonEnvironmentsApi;
    /** Child process execution operations exposed by Muon. */
    readonly executor: MuonExecutorApi;
    /** Filesystem operations exposed by the built-in Muon filesystem plugin. */
    readonly fs: MuonFsApi;
  }

  /** CEF version selection policy used by muon-bootstrap. */
  type MuonCefVersionPolicy =
    | "tested"
    | "same-major-latest"
    | "compat-latest"
    | "exact";

  /** Bootstrap settings used on the next muon-bootstrap startup. */
  interface MuonBootstrapSettings {
    /** CEF version selection policy. */
    readonly cefVersionPolicy?: MuonCefVersionPolicy;
    /** Exact CEF version used when cefVersionPolicy is exact. */
    readonly cefExactVersion?: string;
    /** Minimum seconds between automatic CEF catalog refresh attempts. */
    readonly catalogRefreshIntervalSeconds?: number;
  }

  /** Partial bootstrap settings update. */
  interface MuonBootstrapSettingsPatch {
    /** CEF version selection policy, or null to use defaultVersionPolicy. */
    readonly cefVersionPolicy?: MuonCefVersionPolicy | null;
    /** Exact CEF version used when cefVersionPolicy is exact, or null to clear. */
    readonly cefExactVersion?: string | null;
    /** Minimum seconds between automatic CEF catalog refresh attempts, or null to use the default. */
    readonly catalogRefreshIntervalSeconds?: number | null;
  }

  /** Bootstrap update controls exposed by Muon. */
  interface MuonBootstrapApi {
    /**
     * Return bootstrap settings used by the next muon-bootstrap startup.
     *
     * @returns A promise for the effective bootstrap settings.
     */
    readonly getSettings: () => Promise<MuonBootstrapSettings>;
    /**
     * Update bootstrap settings used by the next muon-bootstrap startup.
     *
     * @param settings - Partial settings to persist. Null values clear explicit settings.
     * @returns A promise that resolves when settings are written.
     */
    readonly setSettings: (
      settings?: MuonBootstrapSettingsPatch,
    ) => Promise<void>;
    /**
     * Request a CEF catalog refresh on the next muon-bootstrap startup.
     *
     * @returns A promise that resolves when the refresh request is persisted.
     */
    readonly triggerUpdate: () => Promise<void>;
  }

  /** Browser and native window operations for the current Muon window. */
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
     * Show or hide the current Muon custom title bar.
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
     * The icon is loaded from the configured Muon asset storage. Muon custom
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
     * Shut down the Muon process.
     *
     * @param exitCode - Process exit code. Omit to use `0`.
     * @returns A promise that resolves after the shutdown request is submitted.
     */
    readonly shutdown: (exitCode?: number) => Promise<void>;
    /**
     * Recycle the Muon process by requesting an automatic restart.
     *
     * @returns A promise that resolves after the recycle request is submitted.
     * @remarks The page context can be destroyed by process shutdown before
     * callers can observe promise settlement.
     */
    readonly recycle: () => Promise<void>;
  }

  /**
   * Filesystem operations exposed by the built-in Muon filesystem plugin.
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
     * safe integers. When `position` is past the end of the file, the returned
     * buffer is empty.
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
     * @remarks Muon currently watches by polling snapshots. Listener exceptions
     * and rejected promises are ignored. Aborting before the watcher is created
     * rejects; aborting after creation closes the watcher.
     */
    readonly watch: (
      path: string,
      listener: MuonFsWatchListener,
      options?: MuonFsOperationOptions,
    ) => Promise<MuonFsWatcher>;
    /** Native filesystem dialogs exposed by the built-in Muon filesystem plugin. */
    readonly dialogs: MuonFsDialogsApi;
  }

  /** Native filesystem dialogs exposed by the built-in Muon filesystem plugin. */
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
     * pending, Muon requests native cancellation when possible.
     */
    readonly signal?: AbortSignal;
  }

  /** Options for reading a binary file. */
  interface MuonFsReadFileOptions extends MuonFsOperationOptions {
    /**
     * Byte offset where reading starts.
     *
     * @remarks Must be a non-negative safe integer. Defaults to `0`.
     */
    readonly position?: number;
    /**
     * Maximum number of bytes to read.
     *
     * @remarks Must be a non-negative safe integer. Omit to read through the end
     * of the file.
     */
    readonly length?: number;
  }

  /** Options for writing a binary file. */
  interface MuonFsWriteFileOptions extends MuonFsOperationOptions {
    /**
     * Byte offset where writing starts.
     *
     * @remarks Must be a non-negative safe integer. Omit to replace the whole file.
     */
    readonly position?: number;
  }

  /** Access modes checked by Muon filesystem access. */
  type MuonFsAccessMode = "read" | "write" | "execute";

  /** Options for checking path accessibility. */
  interface MuonFsAccessOptions extends MuonFsOperationOptions {
    /**
     * Requested access modes.
     *
     * @remarks Omit to check only existence. Every listed mode must be allowed
     * for `access()` to return true.
     */
    readonly mode?: readonly MuonFsAccessMode[];
  }

  /** Options for reading a directory. */
  interface MuonFsReadDirectoryOptions extends MuonFsOperationOptions {
    /**
     * Return entries with metadata helpers instead of names.
     *
     * @remarks Use `true` to select the `MuonFsDirent[]` overload.
     */
    readonly withFileTypes?: boolean;
  }

  /** Options for creating a directory. */
  interface MuonFsMakeDirectoryOptions extends MuonFsOperationOptions {
    /**
     * Create parent directories as needed.
     *
     * @remarks Defaults to false.
     */
    readonly recursive?: boolean;
  }

  /** Options for removing a path. */
  interface MuonFsRemoveOptions extends MuonFsOperationOptions {
    /**
     * Remove directory trees recursively.
     *
     * @remarks Defaults to false.
     */
    readonly recursive?: boolean;
    /**
     * Do not fail when the path is missing.
     *
     * @remarks Defaults to false.
     */
    readonly force?: boolean;
  }

  /** Options for copying a regular file. */
  interface MuonFsCopyFileOptions extends MuonFsOperationOptions {
    /** Replace an existing destination file. Defaults to true. */
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
     * @remarks Defaults to false so GTK/GVfs locations can return URI values.
     */
    readonly localOnly?: boolean;
    /**
     * Allow creating folders from save and folder chooser dialogs.
     *
     * @remarks Applies only when supported by the GTK backend.
     */
    readonly createFolders?: boolean;
    /**
     * Additional GTK MIME type filters.
     *
     * @remarks Entries must be non-empty strings.
     */
    readonly mimeTypes?: readonly string[];
  }

  /** Win32-specific native dialog options. */
  interface MuonFsWin32DialogOptions {
    /** Force selections to filesystem-backed shell items. */
    readonly forceFilesystem?: boolean;
    /** Return shortcut/link items themselves instead of their targets. */
    readonly noDereferenceLinks?: boolean;
    /** Do not add selected locations to the recent documents list. */
    readonly dontAddToRecent?: boolean;
    /** Allow paths that do not pass normal shell validation. */
    readonly noValidate?: boolean;
    /** Restrict typed filenames to the configured file types. */
    readonly strictFileTypes?: boolean;
    /** Require selected paths to exist. */
    readonly pathMustExist?: boolean;
    /** Require selected files to exist. */
    readonly fileMustExist?: boolean;
  }

  /** Common options for native filesystem dialogs. */
  interface MuonFsDialogOptions extends MuonFsOperationOptions {
    /** Dialog title. */
    readonly title?: string;
    /**
     * Initial path or URI shown by the dialog.
     *
     * @remarks GTK accepts GVfs URI values when gtk.localOnly is false.
     */
    readonly defaultPath?: string;
    /** Accept button label. */
    readonly buttonLabel?: string;
    /**
     * Whether to disable the calling browser view while the dialog is open.
     *
     * @remarks Defaults to true.
     */
    readonly modal?: boolean;
    /** Show hidden files when supported by the backend. */
    readonly showHidden?: boolean;
    /**
     * File filters shown by the dialog.
     *
     * @remarks Every filter requires a non-empty name and at least one extension.
     */
    readonly filters?: readonly MuonFsDialogFilter[];
    /** GTK-specific dialog flags. */
    readonly gtk?: MuonFsGtkDialogOptions;
    /** Win32-specific dialog flags. */
    readonly win32?: MuonFsWin32DialogOptions;
  }

  /** Options for open and directory selection dialogs. */
  interface MuonFsOpenDialogOptions extends MuonFsDialogOptions {}

  /** Options for save file dialogs. */
  interface MuonFsSaveDialogOptions extends MuonFsDialogOptions {
    /** Initial file name shown by the save dialog. */
    readonly defaultName?: string;
    /** Ask before replacing an existing file. Defaults to true. */
    readonly confirmOverwrite?: boolean;
  }

  /** Filesystem entry type reported by Muon. */
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
   * @param event - Watch event emitted by Muon.
   * @returns Ignored by Muon. Rejected promises are ignored.
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

  /** Runtime metadata for the current Muon process. */
  interface MuonRuntimeInfo {
    /** Runtime package name. */
    readonly name: string;
    /** Native executable file name included in the runtime payload. */
    readonly executableName: string;
    /** Muon runtime target name. */
    readonly target: string;
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

  /** CEF build loaded by the current Muon process. */
  interface MuonCefRuntimeInfo {
    /** Runtime CEF version reported by libcef. */
    readonly version: string;
    /** Runtime CEF API version configured by libcef. */
    readonly apiVersion: number;
    /** Runtime platform API hash for the configured CEF API version. */
    readonly apiHash: string;
  }

  /** Environment information exposed by Muon. */
  interface MuonEnvironmentsApi {
    /**
     * Return the current process environment variables.
     *
     * @returns A promise for a key-value map of environment variables.
     */
    readonly getVariables: () => Promise<Record<string, string>>;
    /**
     * Return the command line captured when the current Muon process started.
     *
     * @returns A promise for the process command line, including `argv[0]` when available.
     */
    readonly getCommandLine: () => Promise<string[]>;
    /**
     * Return the native Muon process id.
     *
     * @returns A promise for the current process id.
     */
    readonly getProcessId: () => Promise<number>;
    /**
     * Return runtime metadata for the current Muon process.
     *
     * @returns A promise for build-time muon-core metadata and runtime CEF metadata.
     */
    readonly getRuntimeInfo: () => Promise<MuonRuntimeInfo>;
    /**
     * Return whether the current Muon application starts with the user session.
     *
     * @returns A promise for the autostart state, or `undefined` when unknown.
     * @remarks The active launch source selects the platform backend. Muon uses
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

  /** Child process execution operations exposed by Muon. */
  interface MuonExecutorApi {
    /**
     * Spawn a child process without invoking a shell.
     *
     * @param options - Process launch options.
     * @returns A promise for the completed child process result.
     * @remarks The promise resolves even when the child exits with a non-zero
     * exit code. It rejects only when the process cannot be launched or the
     * options are invalid.
     */
    readonly spawn: (
      options: MuonExecutorSpawnOptions,
    ) => Promise<MuonExecutorSpawnResult>;
  }

  /** Options for spawning a child process through Muon. */
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
     */
    readonly args?: readonly string[];
    /**
     * UTF-8 text written to the child process stdin.
     *
     * @remarks Omit or use an empty string to close stdin without writing data.
     */
    readonly stdin?: string;
    /**
     * Working directory used for the child process.
     *
     * @remarks Must not contain NUL. On POSIX, failure to change directory makes
     * the child exit with code `126`.
     */
    readonly cwd?: string;
    /**
     * Environment variable overrides for the child process.
     *
     * @remarks Entries are merged with the current process environment. Keys
     * must be non-empty and must not contain `=` or NUL; values must not contain
     * NUL.
     */
    readonly env?: Record<string, string>;
  }

  /** Completed child process result. */
  interface MuonExecutorSpawnResult {
    /** Child process id. */
    readonly processId: number;
    /** Child process exit code. */
    readonly exitCode: number;
    /** Captured UTF-8 stdout text. */
    readonly stdout: string;
    /** Captured UTF-8 stderr text. */
    readonly stderr: string;
  }
}
