// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import type { Plugin, ResolvedConfig, UserConfig, WatchOptions } from "vite";
import { isAbsolute, resolve } from "node:path";

import { buildMuonApp, type MuonBuildOptions } from "./build.js";
import {
  createMuonCapabilityModuleResolver,
  type MuonCapabilityImportOptions,
  type MuonCapabilityModuleResolver,
  type MuonCapabilityOptions,
  type MuonRuntimePluginConfig,
} from "./capability.js";
import { startMuonViteBrowserBridge } from "./vite-internals.js";
import { attachMuonVitePluginOptions } from "./vite-options.js";
import { muonBuildSequenceSuppressViteBuildEnvironmentKey } from "./build-sequence.js";

type MuonWatchIgnored = NonNullable<WatchOptions["ignored"]>;

/**
 * Windows PE and NSIS resource metadata options.
 */
export interface MuonWindowsResourceOptions {
  /**
   * Windows icon PNG file path.
   *
   * @remarks Only `.png` files are accepted as app inputs. Muon generates the
   * required Windows `.ico` file automatically when updating PE resources or
   * creating NSIS installers. Relative paths are resolved from the source that
   * supplied the option.
   * @defaultValue Uses `windows.resource.iconPath`, then the packaged Muon
   * bootstrap PNG icon when available.
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
 * Linux desktop entry metadata options.
 */
export interface MuonLinuxDesktopOptions {
  /**
   * Desktop entry identifier without the `.desktop` suffix.
   *
   * @defaultValue Uses the resolved Muon `appId`.
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
   * Linux desktop icon PNG file path.
   *
   * @remarks Only `.png` files are accepted as app inputs. Relative paths are
   * resolved from the source that supplied the option.
   * @defaultValue Uses the packaged Muon bootstrap PNG icon when available.
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
   * Parent directory that receives dist-muon/linux-amd64/ style outputs.
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
   * Linux desktop entry metadata.
   *
   * @defaultValue Uses CLI options, `muon.json` `linux.desktop`, package
   * metadata, then Muon defaults.
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
 * Import-side capability rule for Muon plugin virtual modules.
 */
export interface MuonVitePluginAccessImportOptions extends MuonCapabilityImportOptions {}

/**
 * Capability import configuration for Muon plugin virtual modules.
 */
export interface MuonVitePluginAccessOptions extends MuonCapabilityOptions {
  /**
   * Capability imports allowed by importer path.
   */
  imports?: readonly MuonVitePluginAccessImportOptions[];
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
   * Plugin access mode and virtual module capability imports.
   *
   * @remarks Omit this option to use validate mode without plugin
   * capabilities. Pass import rules to allow virtual modules such as
   * `muon:executor`. Pass `false` to use simple window-global exposure.
   * @defaultValue validate mode with no capability imports.
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
 * Creates a Vite plugin that launches Muon during Vite dev startup.
 *
 * @param options Muon plugin options used for development startup and build.
 * @returns Vite plugin instance.
 * @defaultValue `options` defaults to `{}`.
 */
const muon = (options: MuonVitePluginOptions = {}): Plugin => {
  let resolvedConfig: ResolvedConfig | undefined = undefined;
  let capabilityResolver: MuonCapabilityModuleResolver | undefined = undefined;

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
      capabilityResolver =
        options.pluginAccess === false
          ? undefined
          : createMuonCapabilityModuleResolver(
              config.root,
              options.pluginAccess,
            );
    },
    resolveId: (source, importer) =>
      capabilityResolver?.resolveId(source, importer)?.id,
    load: (id) => capabilityResolver?.load(id),
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
      if (
        process.env[muonBuildSequenceSuppressViteBuildEnvironmentKey] === "1"
      ) {
        return;
      }
      if (options.build === false) {
        return;
      }

      const buildOptions =
        typeof options.build === "object" ? options.build : {};
      await buildMuonApp(
        createMuonBuildOptions(
          resolvedConfig,
          buildOptions,
          resolveMuonRuntimePluginConfig(capabilityResolver, options),
        ),
      );
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
  runtimePluginConfig: MuonRuntimePluginConfig,
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
  if (buildOptions.windowsResource !== undefined) {
    options.windowsResource = buildOptions.windowsResource;
  }
  if (buildOptions.linuxDesktop !== undefined) {
    options.linuxDesktop = buildOptions.linuxDesktop;
  }
  if (buildOptions.packageDirectory !== undefined) {
    options.packageDirectory = buildOptions.packageDirectory;
  }
  if (buildOptions.assetSalt !== undefined) {
    options.assetSalt = buildOptions.assetSalt;
  }
  options.runtimePluginConfig = runtimePluginConfig;

  return options;
};

const resolveMuonRuntimePluginConfig = (
  capabilityResolver: MuonCapabilityModuleResolver | undefined,
  options: MuonVitePluginOptions,
): MuonRuntimePluginConfig =>
  options.pluginAccess === false
    ? { mode: "simple" }
    : (capabilityResolver?.getRuntimePluginConfig() ?? {
        mode: "validate",
        capabilities: [],
      });

export default muon;
