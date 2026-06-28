// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import type { Plugin, ResolvedConfig, UserConfig, WatchOptions } from "vite";
import { isAbsolute, resolve } from "node:path";

import { buildMuonApp, type MuonBuildOptions } from "./build.js";
import { startMuonViteBrowserBridge } from "./vite-internals.js";
import { attachMuonVitePluginOptions } from "./vite-options.js";

const suppressViteMuonBuildEnvironmentKey = "MUON_SUPPRESS_VITE_MUON_BUILD";

type MuonWatchIgnored = NonNullable<WatchOptions["ignored"]>;

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
const muon = (options: MuonVitePluginOptions = {}): Plugin => {
  let resolvedConfig: ResolvedConfig | undefined = undefined;

  const plugin: Plugin = {
    name: "muon",
    config: (config): Omit<UserConfig, "plugins"> | null => {
      if (config.server?.watch === null) {
        return null;
      }

      return {
        server: {
          watch: createMuonWatchOptions(config.server?.watch),
        },
      };
    },
    configResolved: (config) => {
      resolvedConfig = config;
    },
    configureServer: async (server) => {
      await startMuonViteBrowserBridge({
        server,
        pluginOptions: options,
        platform: process.platform,
        architecture: process.arch,
        environment: process.env,
      });
    },
    closeBundle: async () => {
      if (resolvedConfig === undefined || resolvedConfig.command !== "build") {
        return;
      }
      if (process.env[suppressViteMuonBuildEnvironmentKey] === "1") {
        return;
      }
      if (options.build === false) {
        return;
      }

      const buildOptions =
        typeof options.build === "object" ? options.build : {};
      await buildMuonApp(createMuonBuildOptions(resolvedConfig, buildOptions));
    },
  };

  return attachMuonVitePluginOptions(plugin, options);
};

const isMuonStagingWatchPath = (path: string): boolean => {
  const normalized = path.replaceAll("\\", "/");
  return (
    normalized === ".muon" ||
    normalized.startsWith(".muon/") ||
    normalized.endsWith("/.muon") ||
    normalized.includes("/.muon/")
  );
};

const mergeMuonWatchIgnored = (
  ignored: WatchOptions["ignored"] | undefined,
): MuonWatchIgnored => {
  const muonIgnored = (path: string): boolean => isMuonStagingWatchPath(path);
  if (ignored === undefined) {
    return muonIgnored;
  }
  return Array.isArray(ignored)
    ? [...ignored, muonIgnored]
    : [ignored, muonIgnored];
};

const createMuonWatchOptions = (
  watch: WatchOptions | undefined,
): WatchOptions => ({
  ...(watch ?? {}),
  ignored: mergeMuonWatchIgnored(watch?.ignored),
});

const createMuonBuildOptions = (
  config: ResolvedConfig,
  buildOptions: MuonViteBuildOptions,
): MuonBuildOptions => {
  const outDir = isAbsolute(config.build.outDir)
    ? config.build.outDir
    : resolve(config.root, config.build.outDir);
  const options: MuonBuildOptions = {
    root: config.root,
    assetSourcePath: outDir,
    assetPrefix: "main",
  };

  if (buildOptions.allTargets !== undefined) {
    options.allTargets = buildOptions.allTargets;
  }
  if (buildOptions.targets !== undefined) {
    options.targets = buildOptions.targets;
  }
  if (buildOptions.appName !== undefined) {
    options.appName = buildOptions.appName;
  }
  if (buildOptions.appId !== undefined) {
    options.appId = buildOptions.appId;
  }
  if (buildOptions.outputRoot !== undefined) {
    options.outputRoot = buildOptions.outputRoot;
  }
  if (buildOptions.configPath !== undefined) {
    options.configPath = buildOptions.configPath;
  }
  if (buildOptions.packageDirectory !== undefined) {
    options.packageDirectory = buildOptions.packageDirectory;
  }
  if (buildOptions.assetSalt !== undefined) {
    options.assetSalt = buildOptions.assetSalt;
  }

  return options;
};

export default muon;
