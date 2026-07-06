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
   * Windows-specific static application icon PNG file path.
   *
   * @remarks Only `.png` files are accepted as app inputs. muon generates the
   * required Windows `.ico` file automatically when updating PE resources or
   * creating NSIS installers. Relative paths are resolved from the source that
   * supplied the option. This overrides top-level `iconPath` on Windows when
   * updating PE resources or creating NSIS installers. Use top-level
   * `iconPath` for a shared static application icon.
   * @defaultValue Uses top-level `iconPath`, then the packaged `muon-256.png`
   * icon.
   */
  iconPath?: string;

  /**
   * Product name written to the Windows version resource.
   *
   * @defaultValue Uses `windows.resource.productName`, `project.json`,
   * `package.json`, then the muon launcher name.
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
 * Linux desktop entry metadata options.
 */
export interface MuonLinuxDesktopOptions {
  /**
   * Desktop entry identifier without the `.desktop` suffix.
   *
   * @defaultValue Uses the resolved muon `appId`.
   */
  desktopId?: string;
  /**
   * Application display name.
   *
   * @defaultValue Uses `linux.desktop.name`, then `package.json` name.
   */
  name?: string;
  /**
   * Desktop entry comment.
   *
   * @defaultValue Uses `linux.desktop.comment`, then `package.json`
   * description.
   */
  comment?: string;
  /**
   * Linux-specific static application icon PNG file path.
   *
   * @remarks Only `.png` files are accepted as app inputs. Relative paths are
   * resolved from the source that supplied the option. This overrides
   * top-level `iconPath` for Linux desktop entries. Use top-level `iconPath`
   * for a shared static application icon.
   * @defaultValue Uses top-level `iconPath`, then the packaged `muon-256.png`
   * icon.
   */
  iconPath?: string;
  /**
   * Desktop menu categories.
   *
   * @defaultValue `["Utility"]`.
   */
  categories?: readonly string[];
  /**
   * Whether desktop startup notification is requested.
   *
   * @defaultValue `true`.
   */
  startupNotify?: boolean;
}

/**
 * Options for generating muon app distributions after Vite build.
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
   * Stable base application identifier used for portable runtime state.
   *
   * @remarks Windows target distributions embed `<appId>.<arch>` as their
   * runtime app identifier. Linux targets embed this value unchanged.
   *
   * @defaultValue The sanitized package name, or `"muon-app"` when unavailable.
   */
  appId?: string;

  /**
   * Parent directory that receives dist-muon/linux-amd64/ style outputs.
   *
   * @defaultValue The Vite project root.
   */
  outputRoot?: string;

  /**
   * muon config path to embed.
   *
   * @defaultValue Auto-detects `muon.json5`, `muon.jsonc`, then `muon.json`;
   * uses an empty config when none exists.
   */
  configPath?: string;

  /**
   * Static application icon PNG file path.
   *
   * @remarks The icon is used for Windows PE/NSIS resources, Linux desktop
   * entries, and the generated initial title bar icon asset. Target-specific
   * icon paths in `windowsResource` or `linuxDesktop` override this value for
   * that target.
   * @defaultValue Uses top-level `muon.json` `iconPath`, `project.json`, then
   * the packaged `muon-256.png` icon.
   */
  iconPath?: string;

  /**
   * Windows PE and NSIS resource metadata.
   *
   * @defaultValue Uses CLI options, `muon.json` `windows.resource`,
   * `project.json`, `package.json`, then muon defaults.
   */
  windowsResource?: MuonWindowsResourceOptions;

  /**
   * Linux desktop entry metadata.
   *
   * @defaultValue Uses `muon.json` `linux.desktop`, package metadata, then
   * muon defaults.
   */
  linuxDesktop?: MuonLinuxDesktopOptions;

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
 * Import-side capability rule for muon plugin virtual modules.
 */
export interface MuonVitePluginAccessImportOptions {
  /**
   * Importer source globs relative to the Vite project root.
   */
  sources?: readonly string[];
  /**
   * NPM package names allowed to import the virtual module.
   */
  packages?: readonly string[];
  /**
   * Plugin function path globs allowed for matching importers.
   *
   * @remarks Required in validate mode. Simple mode does not use import rules.
   */
  allow?: readonly string[];
}

/**
 * Plugin entry and capability import configuration for muon virtual modules.
 */
export interface MuonVitePluginAccessEntryOptions {
  /**
   * Plugin entry name.
   */
  name: string;
  /**
   * Optional expected SHA-1 signature for the external plugin library.
   *
   * @remarks This is a 40-character hexadecimal SHA-1 digest of the native
   * plugin library bytes followed by `salt`. It is not supported for
   * `internal`.
   */
  signature?: string;
  /**
   * Optional hexadecimal salt appended before checking the plugin signature.
   *
   * @remarks Required when `signature` is specified. It is not supported for
   * `internal`.
   */
  salt?: string;
  /**
   * Plugin function path globs allowed by the runtime plugin policy.
   *
   * @remarks Required in simple mode. Validate mode derives the runtime
   * allowlist from `imports[].allow` and rejects this field in public config.
   */
  allow?: readonly string[];
  /**
   * Validate-mode import rules for this plugin entry.
   */
  imports?: readonly MuonVitePluginAccessImportOptions[];
}

/**
 * Plugin access configuration for muon plugin virtual modules.
 */
export interface MuonVitePluginAccessOptions {
  /**
   * External plugin directory override.
   */
  path?: string;
  /**
   * Plugin exposure mode.
   */
  mode?: "simple" | "validate";
  /**
   * Page URL patterns where the plugin bridge is exposed.
   */
  pages?: readonly string[];
  /**
   * Runtime plugin entries and validate-mode import rules.
   */
  plugins?: readonly MuonVitePluginAccessEntryOptions[];
}

/**
 * Options for the muon Vite development plugin.
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
   * muon-builder downloads and caches the tested CEF artifact from muonPath.
   * @defaultValue The tested CEF artifact downloaded and cached by muon-builder.
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
   * Launch muon automatically during Vite dev startup.
   *
   * @remarks This is independent from Vite's server.open browser startup
   * option. Vite build ignores this option.
   * @defaultValue `true`
   */
  open?: boolean;

  /**
   * Enable the muon debugger defaults during Vite dev startup.
   *
   * @remarks When enabled, the generated development config enables CDP and
   * binds DevTools to F12. Vite build ignores this option.
   * @defaultValue `true`
   */
  enableDebugger?: boolean;

  /**
   * Plugin access mode and virtual module capability imports.
   *
   * @remarks Omit this option to use the `plugin` section from `muon.json`.
   * Pass plugin entries with import rules to override `muon.json` and allow
   * virtual modules such as `muon:executor`. Pass `false` to use simple
   * window-global exposure.
   * @defaultValue `muon.json` plugin config, or validate mode with no
   * capability imports.
   */
  pluginAccess?: false | MuonVitePluginAccessOptions;

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
 * Creates a Vite plugin that launches muon during Vite dev startup.
 *
 * @param options muon plugin options used for development startup and build.
 * @returns Vite plugin instance.
 * @defaultValue `options` defaults to `{}`.
 */
declare const muon: (options?: MuonVitePluginOptions) => Plugin;
export default muon;
