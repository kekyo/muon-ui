// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import type { Plugin } from "vite";

/**
 * Options for generating Muon app distributions after Vite build.
 */
export interface MuonViteBuildOptions {
  /**
   * Public target identifiers to build.
   */
  targets?: readonly string[];

  /**
   * Build every supported target from the installed package.
   *
   * @remarks Defaults to true when targets is omitted. Set false to build only
   * the host target.
   */
  allTargets?: boolean;

  /**
   * File name used for the app launcher.
   *
   * @remarks The .exe suffix is added automatically for Windows targets.
   */
  appName?: string;

  /**
   * Stable application identifier used for portable runtime state.
   */
  appId?: string;

  /**
   * Parent directory that receives dist-muon-linux-amd64/ style outputs.
   */
  outputRoot?: string;

  /**
   * Muon config path to embed.
   */
  configPath?: string;

  /**
   * Directory containing package runtime/ and native/ folders.
   *
   * @remarks This defaults to the installed muon package dist directory.
   */
  packageDirectory?: string;

  /**
   * Asset salt override for deterministic tests.
   *
   * @remarks Production builds should omit this option.
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
   */
  muonPath?: string;

  /**
   * Directory containing CEF files, or a CEF archive root with Release/Resources.
   *
   * @remarks Relative paths are resolved from the Vite project root. When omitted,
   * muon-prepare downloads and caches the tested CEF artifact from muonPath.
   */
  cefPath?: string;

  /**
   * Runtime staging directory used for development startup.
   *
   * @remarks Relative paths are resolved from the Vite project root. Defaults to
   * .muon/<public-target>.
   */
  stagePath?: string;

  /**
   * Launch Muon automatically during Vite dev startup.
   *
   * @remarks Defaults to true. This is independent from Vite's server.open
   * browser startup option. Vite build ignores this option.
   */
  open?: boolean;

  /**
   * Enable the Muon debugger defaults during Vite dev startup.
   *
   * @remarks Defaults to true. When enabled, the generated development config
   * enables CDP and binds DevTools to F12. Vite build ignores this option.
   */
  enableDebugger?: boolean;

  /**
   * Build app distributions from Vite output.
   *
   * @remarks Defaults to true during Vite build. Set false to disable the build
   * hook while keeping the development bridge enabled.
   */
  build?: boolean | MuonViteBuildOptions;
}

/**
 * Creates a Vite plugin that launches Muon during Vite dev startup.
 *
 * @param options Muon plugin options used for development startup and build.
 * @returns Vite plugin instance.
 */
declare const muon: (options?: MuonVitePluginOptions) => Plugin;
export default muon;
