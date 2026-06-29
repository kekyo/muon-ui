// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import type { Plugin } from "vite";

/**
 * Windows PE and NSIS resource metadata options.
 */
export interface MuonWindowsResourceOptions {
  /**
   * Windows icon file path.
   *
   * @remarks Only `.ico` files are supported. Relative paths are resolved from
   * the source that supplied the option.
   * @defaultValue Uses `windows.resource.iconPath`, then the packaged Muon
   * bootstrap icon when available.
   */
  iconPath?: string;

  /**
   * Product name written to the Windows version resource.
   *
   * @defaultValue Uses `windows.resource.productName`, `project.json`,
   * `package.json`, then the Muon launcher name.
   */
  productName?: string;

  /**
   * File description written to the Windows version resource.
   *
   * @defaultValue Uses `windows.resource.fileDescription`, `project.json`,
   * `package.json.description`, then the product name.
   */
  fileDescription?: string;

  /**
   * Company name written to the Windows version resource.
   *
   * @defaultValue Uses `windows.resource.companyName`, `project.json`,
   * `package.json.author`, then `"Unknown"`.
   */
  companyName?: string;

  /**
   * Product and file version string.
   *
   * @remarks PE fixed version fields are normalized to four numeric parts, so
   * `1.2.3` becomes `1.2.3.0`. String fields keep the original value.
   * @defaultValue Uses `windows.resource.version`, `project.json.version`,
   * `package.json.version`, then `"0.0.0"`.
   */
  version?: string;

  /**
   * Legal copyright written to the Windows version resource.
   *
   * @defaultValue Uses `windows.resource.copyright`, `project.json`, then
   * `package.json.copyright` when available.
   */
  copyright?: string;

  /**
   * Windows resource language identifier.
   *
   * @defaultValue `1033` (`en-US`).
   */
  language?: number;

  /**
   * Windows version resource code page.
   *
   * @defaultValue `1200` (`UTF-16LE`).
   */
  codePage?: number;
}

/**
 * Options for generating Muon app distributions after Vite build.
 */
export interface MuonViteBuildOptions {
  /**
   * Public target identifiers to build.
   *
   * @defaultValue Uses every supported target unless `allTargets` is `false`,
   * then the host target is used.
   */
  targets?: readonly string[];

  /**
   * Build every supported target from the installed package.
   *
   * @remarks Set false to build only the host target when `targets` is omitted.
   * @defaultValue `true` when `targets` is omitted.
   */
  allTargets?: boolean;

  /**
   * File name used for the app launcher.
   *
   * @remarks The .exe suffix is added automatically for Windows targets.
   * @defaultValue The sanitized package name, or `"muon-app"` when unavailable.
   */
  appName?: string;

  /**
   * Stable application identifier used for portable runtime state.
   *
   * @defaultValue The sanitized package name, or `"muon-app"` when unavailable.
   */
  appId?: string;

  /**
   * Parent directory that receives dist-muon-linux-amd64/ style outputs.
   *
   * @defaultValue The Vite project root.
   */
  outputRoot?: string;

  /**
   * Muon config path to embed.
   *
   * @defaultValue Auto-detects `muon.json5`, `muon.jsonc`, then `muon.json`;
   * uses an empty config when none exists.
   */
  configPath?: string;

  /**
   * Windows PE and NSIS resource metadata.
   *
   * @defaultValue Uses CLI options, `muon.json` `windows.resource`,
   * `project.json`, `package.json`, then Muon defaults.
   */
  windowsResource?: MuonWindowsResourceOptions;

  /**
   * Directory containing package runtime/ and native/ folders.
   *
   * @defaultValue The installed muon package dist directory.
   */
  packageDirectory?: string;

  /**
   * Asset salt override for deterministic tests.
   *
   * @remarks Production builds should omit this option.
   * @defaultValue A random 16-byte salt.
   */
  assetSalt?: Uint8Array;
}

/**
 * Options for the Muon Vite development plugin.
 */
export interface MuonVitePluginOptions {
  /**
   * Directory containing muon-core runtime files such as muon-core and plugins.
   *
   * @remarks Relative paths are resolved from the Vite project root. When omitted,
   * the packaged runtime at dist/runtime/<public-target> is used.
   * @defaultValue The packaged runtime at `dist/runtime/<public-target>`.
   */
  muonPath?: string;

  /**
   * Directory containing CEF files, or a CEF archive root with Release/Resources.
   *
   * @remarks Relative paths are resolved from the Vite project root. When omitted,
   * muon-prepare downloads and caches the tested CEF artifact from muonPath.
   * @defaultValue The tested CEF artifact downloaded and cached by muon-prepare.
   */
  cefPath?: string;

  /**
   * Runtime staging directory used for development startup.
   *
   * @remarks Relative paths are resolved from the Vite project root.
   * @defaultValue `.muon/<public-target>`.
   */
  stagePath?: string;

  /**
   * Launch Muon automatically during Vite dev startup.
   *
   * @remarks This is independent from Vite's server.open browser startup
   * option. Vite build ignores this option.
   * @defaultValue `true`
   */
  open?: boolean;

  /**
   * Enable the Muon debugger defaults during Vite dev startup.
   *
   * @remarks When enabled, the generated development config enables CDP and
   * binds DevTools to F12. Vite build ignores this option.
   * @defaultValue `true`
   */
  enableDebugger?: boolean;

  /**
   * Build app distributions from Vite output.
   *
   * @remarks Set false to disable the build hook while keeping the development
   * bridge enabled.
   * @defaultValue `true` during Vite build.
   */
  build?: boolean | MuonViteBuildOptions;
}

/**
 * Creates a Vite plugin that launches Muon during Vite dev startup.
 *
 * @param options Muon plugin options used for development startup and build.
 * @returns Vite plugin instance.
 * @defaultValue `options` defaults to `{}`.
 */
declare const muon: (options?: MuonVitePluginOptions) => Plugin;
export default muon;
