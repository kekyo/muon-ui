// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import type { Plugin } from "vite";

/**
 * Windows executable artifact kind accepted by code signing options.
 */
export type MuonWindowsCodeSigningTarget =
  | "runtime"
  | "launcher"
  | "nsisInstaller"
  | "nsisUninstaller";

/**
 * External Windows code signing command options.
 */
export interface MuonWindowsCodeSigningOptions {
  /**
   * Executable or script used to sign one file.
   */
  readonly command: string;

  /**
   * Arguments passed to the signing command.
   *
   * @remarks `{path}` is required and is replaced with the executable path.
   * `{target}` and `{kind}` are also replaced when present.
   */
  readonly args: readonly string[];

  /**
   * Artifact kinds to sign.
   *
   * @defaultValue Signs all supported Windows artifact kinds.
   */
  readonly targets?: readonly MuonWindowsCodeSigningTarget[];
}

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
  readonly iconPath?: string;

  /**
   * Product name written to the Windows version resource.
   *
   * @defaultValue Uses `windows.resource.productName`, `project.json`,
   * `package.json`, then the muon launcher name.
   */
  readonly productName?: string;

  /**
   * File description written to the Windows version resource.
   *
   * @defaultValue Uses `windows.resource.fileDescription`, `project.json`,
   * `package.json.description`, then the product name.
   */
  readonly fileDescription?: string;

  /**
   * Company name written to the Windows version resource.
   *
   * @defaultValue Uses `windows.resource.companyName`, `project.json`,
   * `package.json.author`, then `"Unknown"`.
   */
  readonly companyName?: string;

  /**
   * Product and file version string.
   *
   * @remarks PE fixed version fields are normalized to four numeric parts, so
   * `1.2.3` becomes `1.2.3.0`. String fields keep the original value.
   * @defaultValue Uses `windows.resource.version`, `project.json.version`,
   * `package.json.version`, then `"0.0.0"`.
   */
  readonly version?: string;

  /**
   * Legal copyright written to the Windows version resource.
   *
   * @defaultValue Uses `windows.resource.copyright`, `project.json`, then
   * `package.json.copyright` when available.
   */
  readonly copyright?: string;

  /**
   * Windows resource language identifier.
   *
   * @defaultValue `1033` (`en-US`).
   */
  readonly language?: number;

  /**
   * Windows version resource code page.
   *
   * @defaultValue `1200` (`UTF-16LE`).
   */
  readonly codePage?: number;
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
  readonly desktopId?: string;
  /**
   * Application display name.
   *
   * @defaultValue Uses `linux.desktop.name`, then `package.json` name.
   */
  readonly name?: string;
  /**
   * Desktop entry comment.
   *
   * @defaultValue Uses `linux.desktop.comment`, then `package.json`
   * description.
   */
  readonly comment?: string;
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
  readonly iconPath?: string;
  /**
   * Desktop menu categories.
   *
   * @defaultValue `["Utility"]`.
   */
  readonly categories?: readonly string[];
  /**
   * Whether desktop startup notification is requested.
   *
   * @defaultValue `true`.
   */
  readonly startupNotify?: boolean;
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
  readonly targets?: readonly string[];

  /**
   * Build every supported target from the installed package.
   *
   * @remarks Set false to build only the host target when `targets` is omitted.
   * @defaultValue `true` when `targets` is omitted.
   */
  readonly allTargets?: boolean;

  /**
   * File name used for the app launcher.
   *
   * @remarks The .exe suffix is added automatically for Windows targets.
   * @defaultValue The sanitized package name, or `"muon-app"` when unavailable.
   */
  readonly appName?: string;

  /**
   * Stable base application identifier used for portable runtime state.
   *
   * @remarks Windows target distributions embed `<appId>.<arch>` as their
   * runtime app identifier. Linux targets embed this value unchanged.
   *
   * @defaultValue The sanitized package name, or `"muon-app"` when unavailable.
   */
  readonly appId?: string;

  /**
   * Parent directory that receives dist-muon/linux-amd64/ style outputs.
   *
   * @defaultValue The Vite project root.
   */
  readonly outputRoot?: string;

  /**
   * muon config path to embed.
   *
   * @defaultValue Auto-detects `muon.json5`, `muon.jsonc`, then `muon.json`;
   * uses an empty config when none exists.
   */
  readonly configPath?: string;

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
  readonly iconPath?: string;

  /**
   * Additional project files copied next to the generated app launcher.
   *
   * @remarks When omitted, `package.json` `files` is used as a candidate list.
   * Only regular files are copied. Asset input paths, `node_modules`, `.git`,
   * and generated target output directories are excluded. Set an empty array to
   * disable package.json files fallback.
   */
  readonly distributionFiles?: readonly string[];

  /**
   * Windows PE and NSIS resource metadata.
   *
   * @defaultValue Uses CLI options, `muon.json` `windows.resource`,
   * `project.json`, `package.json`, then muon defaults.
   */
  readonly windowsResource?: MuonWindowsResourceOptions;

  /**
   * Windows code signing command for generated executable artifacts.
   *
   * @remarks Uses `muon.json` `windows.codeSigning` when omitted. Set false
   * to disable `muon.json` code signing.
   * @defaultValue Uses `muon.json` `windows.codeSigning` when present.
   */
  readonly windowsCodeSigning?: false | MuonWindowsCodeSigningOptions;

  /**
   * Linux desktop entry metadata.
   *
   * @defaultValue Uses `muon.json` `linux.desktop`, package metadata, then
   * muon defaults.
   */
  readonly linuxDesktop?: MuonLinuxDesktopOptions;

  /**
   * Directory containing package runtime/ and native/ folders.
   *
   * @defaultValue The installed muon package dist directory.
   */
  readonly packageDirectory?: string;

  /**
   * Asset salt override for deterministic tests.
   *
   * @remarks Production builds should omit this option.
   * @defaultValue A random 16-byte salt.
   */
  readonly assetSalt?: Uint8Array;
}

/**
 * Import-side capability rule for muon plugin virtual modules.
 */
export interface MuonVitePluginAccessImportOptions {
  /**
   * Importer source globs relative to the Vite project root.
   */
  readonly sources?: readonly string[];
  /**
   * NPM package names allowed to import the virtual module.
   */
  readonly packages?: readonly string[];
  /**
   * Plugin function path globs allowed for matching importers.
   *
   * @remarks Required in validate mode. Simple mode does not use import rules.
   */
  readonly allow?: readonly string[];
}

/**
 * Plugin entry and capability import configuration for muon virtual modules.
 */
export interface MuonVitePluginAccessEntryOptions {
  /**
   * Plugin entry name.
   */
  readonly name: string;
  /**
   * Optional 64-character hexadecimal SHA-256 digest of the final external
   * `.so` or `.dll` file bytes followed by the decoded `salt` bytes.
   *
   * @remarks Calculate this after all library post-processing, including
   * stripping and code signing. It is not supported for `internal`.
   */
  readonly signature?: string;
  /**
   * Optional hex-encoded salt whose decoded bytes are appended when checking
   * the plugin signature.
   *
   * @remarks Required when `signature` is specified. It is not supported for
   * `internal`.
   */
  readonly salt?: string;
  /**
   * Plugin-defined string key-value configuration entries.
   */
  readonly config?: Readonly<Record<string, string>>;
  /**
   * Plugin function path globs allowed by the runtime plugin policy.
   *
   * @remarks Required in simple mode. Validate mode derives the runtime
   * allowlist from `imports[].allow` and rejects this field in public config.
   */
  readonly allow?: readonly string[];
  /**
   * Validate-mode import rules for this plugin entry.
   */
  readonly imports?: readonly MuonVitePluginAccessImportOptions[];
}

/**
 * Plugin access configuration for muon plugin virtual modules.
 */
export interface MuonVitePluginAccessOptions {
  /**
   * External plugin directory override.
   */
  readonly path?: string;
  /**
   * Plugin exposure mode.
   */
  readonly mode?: "simple" | "validate";
  /**
   * Page URL patterns where the plugin bridge is exposed.
   */
  readonly pages?: readonly string[];
  /**
   * Runtime plugin entries and validate-mode import rules.
   */
  readonly plugins?: readonly MuonVitePluginAccessEntryOptions[];
}

/**
 * Options applied only while the muon Vite development server is running.
 */
export interface MuonViteDevOptions {
  /**
   * Application config values appended to the generated Vite development config.
   *
   * @remarks These values override matching top-level `config` keys from the
   * project muon config because the generated config is loaded last. Vite build
   * and `muon run` ignore these values.
   * @defaultValue `{}`
   */
  readonly config?: Readonly<Record<string, string>>;
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
  readonly muonPath?: string;

  /**
   * Directory containing CEF files, or a CEF archive root with Release/Resources.
   *
   * @remarks Relative paths are resolved from the Vite project root. When omitted,
   * muon-builder downloads and caches the tested CEF artifact from muonPath.
   * @defaultValue The tested CEF artifact downloaded and cached by muon-builder.
   */
  readonly cefPath?: string;

  /**
   * Runtime staging directory used for development startup.
   *
   * @remarks Relative paths are resolved from the Vite project root.
   * @defaultValue `.muon/<public-target>`.
   */
  readonly stagePath?: string;

  /**
   * Launch muon automatically during Vite dev startup.
   *
   * @remarks This is independent from Vite's server.open browser startup
   * option. Vite build ignores this option.
   * @defaultValue `true`
   */
  readonly open?: boolean;

  /**
   * Enable the muon debugger defaults during Vite dev startup.
   *
   * @remarks When enabled, the generated development config enables CDP and
   * binds DevTools to F12. Vite build ignores this option.
   * @defaultValue `true`
   */
  readonly enableDebugger?: boolean;

  /**
   * Override whether invalid HTTPS certificates for localhost are ignored
   * during development startup.
   *
   * @remarks When specified, this writes
   * `network.localAccess.allowInsecureLocalhost` to the generated development
   * config loaded after the project `muon.json`. The `muon run`
   * `--allow-insecure-localhost` flag overrides this option. Vite build ignores
   * this option, and distribution builds use `muon.json`.
   * @defaultValue The project `muon.json` value, or `false` when absent.
   */
  readonly allowInsecureLocalhost?: boolean;

  /**
   * Close the Vite dev server when the launched muon process exits.
   *
   * @remarks Vite build and `muon run` ignore this option. Recycle exits keep
   * the Vite dev server running so muon can restart.
   * @defaultValue `true`
   */
  readonly exitWithServer?: boolean;

  /**
   * Options used only for Vite development startup.
   *
   * @defaultValue `{}`
   */
  readonly dev?: MuonViteDevOptions;

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
  readonly pluginAccess?: false | MuonVitePluginAccessOptions;

  /**
   * Build app distributions from Vite output.
   *
   * @remarks Set false to disable the build hook while keeping the development
   * bridge enabled.
   * @defaultValue `true` during Vite build.
   */
  readonly build?: boolean | MuonViteBuildOptions;
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
