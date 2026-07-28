// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import type { Plugin, ResolvedConfig, UserConfig, WatchOptions } from "vite";
import { isAbsolute, resolve } from "node:path";

import {
  buildMuonApp,
  resolveMuonNodeProjectForBuildConfig,
  type MuonBuildOptions,
} from "./build.js";
import {
  createMuonCapabilityModuleResolver,
  type MuonCapabilityModuleResolver,
  type MuonRuntimePluginConfig,
} from "./capability.js";
import {
  resolveMuonPluginAccessOptions,
  type MuonPluginAccessEntryOptions,
  type MuonPluginAccessImportOptions,
  type MuonPluginAccessOptions,
  type MuonResolvedPluginAccessOptions,
} from "./plugin-access.js";
import { collectMuonPluginFunctionPathsForAccess } from "./plugin-inspector.js";
import { startMuonViteBrowserBridge } from "./vite-internals.js";
import {
  attachMuonVitePluginOptions,
  attachMuonVitePluginRuntimeState,
} from "./vite-options.js";
import { muonBuildSequenceSuppressViteBuildEnvironmentKey } from "./build-sequence.js";
import { createVitePackagedAssetOptions } from "./vite-assets.js";
import {
  createMuonProgressRenderer,
  type MuonProgressCallback,
} from "./progress.js";
import type { MuonWindowsCodeSigningOptions } from "./windows-code-signing.js";
import {
  assertMuonNodeProjectViteBuildIsSafe,
  assertMuonNodeProjectVitePublicDirectoryIsSafe,
  type ResolvedMuonNodeProject,
} from "./node-project.js";

export type {
  MuonWindowsCodeSigningOptions,
  MuonWindowsCodeSigningTarget,
} from "./windows-code-signing.js";

type MuonWatchIgnored = NonNullable<WatchOptions["ignored"]>;

interface InternalMuonBuildOptions extends MuonBuildOptions {
  progress?: MuonProgressCallback;
}

const getErrorMessage = (error: unknown): string =>
  error instanceof Error ? error.message : String(error);

const muonViteDisableArgument = "--no-muon";
const muonNodeModuleSpecifier = "muon:node";
const muonNodeVirtualModuleId = "\0muon-node";
const muonNodeVirtualModuleSource = `const __muonNodeApi = globalThis.__muon_node_api;
if (typeof __muonNodeApi?.createNode !== "function") {
  throw new Error("muon Node bridge is not available.");
}

export const createNode = __muonNodeApi.createNode.bind(__muonNodeApi);
`;

const isMuonDisabledByViteArguments = (argv: readonly string[]): boolean => {
  const separatorIndex = argv.indexOf("--");
  return (
    separatorIndex >= 0 &&
    argv.slice(separatorIndex + 1).includes(muonViteDisableArgument)
  );
};

/**
 * Windows PE and NSIS resource metadata options.
 */
export interface MuonWindowsResourceOptions {
  /**
   * Windows-specific icon PNG file path override.
   *
   * @remarks Only `.png` files are accepted as app inputs. muon generates the
   * required Windows `.ico` file automatically when updating PE resources or
   * creating NSIS installers. Use top-level `iconPath` for a shared static
   * application icon. Relative paths are resolved from the source that supplied
   * the option.
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
   * Linux-specific desktop icon PNG file path override.
   *
   * @remarks Only `.png` files are accepted as app inputs. Relative paths are
   * resolved from the source that supplied the option. Use top-level
   * `iconPath` for a shared static application icon.
   * @defaultValue Uses top-level `iconPath`, then the packaged `muon-256.png`
   * icon when available.
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
   * Stable base application identifier used for runtime app identity.
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
   * Additional project files copied next to the generated app launcher.
   *
   * @remarks When omitted, `package.json` `files` is used as a candidate list.
   * Only regular files are copied. Asset input paths, `node_modules`, `.git`,
   * and generated target output directories are excluded. Set an empty array to
   * disable package.json files fallback.
   */
  distributionFiles?: readonly string[];

  /**
   * Windows PE and NSIS resource metadata.
   *
   * @defaultValue Uses CLI options, `muon.json` `windows.resource`,
   * `project.json`, `package.json`, then muon defaults.
   */
  windowsResource?: MuonWindowsResourceOptions;

  /**
   * Windows code signing command for generated executable artifacts.
   *
   * @remarks Uses `muon.json` `windows.codeSigning` when omitted. Set false
   * to disable `muon.json` code signing.
   */
  windowsCodeSigning?: false | MuonWindowsCodeSigningOptions;

  /**
   * Linux desktop entry metadata.
   *
   * @defaultValue Uses CLI options, `muon.json` `linux.desktop`, package
   * metadata, then muon defaults.
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
export interface MuonVitePluginAccessImportOptions extends MuonPluginAccessImportOptions {}

/**
 * Plugin entry and capability import configuration for muon virtual modules.
 */
export interface MuonVitePluginAccessEntryOptions extends MuonPluginAccessEntryOptions {}

/**
 * Plugin access configuration for muon plugin virtual modules.
 */
export interface MuonVitePluginAccessOptions extends MuonPluginAccessOptions {
  /**
   * Runtime plugin entries and validate-mode import rules.
   */
  plugins?: readonly MuonVitePluginAccessEntryOptions[];
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
  config?: Readonly<Record<string, string>>;
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
  allowInsecureLocalhost?: boolean;

  /**
   * Close the Vite dev server when the launched muon process exits.
   *
   * @remarks Vite build and `muon run` ignore this option. Recycle exits keep
   * the Vite dev server running so muon can restart.
   * @defaultValue `true`
   */
  exitWithServer?: boolean;

  /**
   * Options used only for Vite development startup.
   *
   * @defaultValue `{}`
   */
  dev?: MuonViteDevOptions;

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
const muon = (options: MuonVitePluginOptions = {}): Plugin => {
  let resolvedConfig: ResolvedConfig | undefined = undefined;
  let capabilityResolver: MuonCapabilityModuleResolver | undefined = undefined;
  let resolvedPluginAccess: MuonResolvedPluginAccessOptions | undefined =
    undefined;
  let loadedCapabilityRuntimePluginConfig: MuonRuntimePluginConfig | undefined =
    undefined;
  let nodeVirtualModuleEnabled = false;

  const getRuntimePluginConfig = (): MuonRuntimePluginConfig =>
    resolveMuonRuntimePluginConfig(capabilityResolver, resolvedPluginAccess);

  const refreshPluginAccess = async (config: ResolvedConfig): Promise<void> => {
    resolvedPluginAccess = await resolveMuonPluginAccessOptions({
      root: config.root,
      configPath: resolveMuonConfigPathForViteCommand(config, options),
      pluginAccess: options.pluginAccess,
      ...(config.command === "serve"
        ? {
            onConfigReadError: (error: unknown): void => {
              config.logger.warn(
                `muon project config will be ignored because it could not be read or parsed: ${getErrorMessage(error)}`,
              );
            },
          }
        : {}),
    });
    const pluginFunctionPaths =
      await collectMuonPluginFunctionPathsForAccess(resolvedPluginAccess);
    capabilityResolver =
      resolvedPluginAccess.mode === "validate"
        ? createMuonCapabilityModuleResolver(config.root, {
            ...resolvedPluginAccess.capabilityOptions,
            functionPaths: pluginFunctionPaths,
          })
        : undefined;
    loadedCapabilityRuntimePluginConfig = undefined;
  };

  const refreshRuntimePluginConfig =
    async (): Promise<MuonRuntimePluginConfig> => {
      if (resolvedConfig === undefined) {
        throw new Error("muon Vite plugin config was not resolved.");
      }
      await refreshPluginAccess(resolvedConfig);
      return getRuntimePluginConfig();
    };

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
    configResolved: async (config): Promise<void> => {
      resolvedConfig = config;
      await refreshPluginAccess(config);
      let nodeProject: ResolvedMuonNodeProject | undefined = undefined;
      try {
        nodeProject = await resolveMuonNodeProjectForBuildConfig(
          config.root,
          resolveMuonConfigPathForViteCommand(config, options),
        );
      } catch (error) {
        if (config.command !== "serve") {
          throw error;
        }
        config.logger.warn(
          `muon Node project preflight will be ignored because the project config could not be read or parsed: ${getErrorMessage(error)}`,
        );
      }
      nodeVirtualModuleEnabled =
        resolvedPluginAccess?.mode === "validate" && nodeProject !== undefined;
      if (
        process.env[muonBuildSequenceSuppressViteBuildEnvironmentKey] !== "1"
      ) {
        if (config.command === "build") {
          const outputDirectory = isAbsolute(config.build.outDir)
            ? config.build.outDir
            : resolve(config.root, config.build.outDir);
          await assertMuonNodeProjectViteBuildIsSafe(
            nodeProject,
            outputDirectory,
            config.publicDir,
          );
        } else {
          await assertMuonNodeProjectVitePublicDirectoryIsSafe(
            nodeProject,
            config.publicDir,
          );
        }
      }
    },
    resolveId: (source, importer) => {
      if (source === muonNodeModuleSpecifier) {
        return nodeVirtualModuleEnabled ? muonNodeVirtualModuleId : undefined;
      }
      return capabilityResolver?.resolveId(source, importer)?.id;
    },
    load: (id) => {
      if (nodeVirtualModuleEnabled && id === muonNodeVirtualModuleId) {
        return muonNodeVirtualModuleSource;
      }
      const source = capabilityResolver?.load(id);
      if (source !== undefined) {
        loadedCapabilityRuntimePluginConfig = getRuntimePluginConfig();
      }
      return source;
    },
    configureServer: async (server) => {
      const pluginOptions = isMuonDisabledByViteArguments(process.argv)
        ? { ...options, open: false }
        : options;
      await startMuonViteBrowserBridge({
        server,
        pluginOptions,
        refreshRuntimePluginConfig,
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
      const muonBuildOptions = createMuonBuildOptions(
        resolvedConfig,
        buildOptions,
        getRuntimePluginConfig(),
      );
      const progressRenderer = createMuonProgressRenderer();
      (muonBuildOptions as InternalMuonBuildOptions).progress =
        progressRenderer.report;
      try {
        await buildMuonApp(muonBuildOptions);
      } finally {
        progressRenderer.flush();
      }
    },
  };

  return attachMuonVitePluginRuntimeState(
    attachMuonVitePluginOptions(plugin, options),
    {
      getResolvedConfig: () => resolvedConfig,
      getRuntimePluginConfig: () =>
        resolvedConfig === undefined
          ? undefined
          : (loadedCapabilityRuntimePluginConfig ?? getRuntimePluginConfig()),
    },
  );
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
  const packagedAssetOptions = createVitePackagedAssetOptions(config.base);
  const options: MuonBuildOptions = {
    root: config.root,
    assetSourcePath: outDir,
    assetPrefix: packagedAssetOptions.assetPrefix,
  };
  if (packagedAssetOptions.browserStartPage !== undefined) {
    options.browserStartPage = packagedAssetOptions.browserStartPage;
  }

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
  if (buildOptions.iconPath !== undefined) {
    options.iconPath = buildOptions.iconPath;
  }
  if (buildOptions.distributionFiles !== undefined) {
    options.distributionFiles = buildOptions.distributionFiles;
  }
  if (buildOptions.windowsResource !== undefined) {
    options.windowsResource = buildOptions.windowsResource;
  }
  if (buildOptions.windowsCodeSigning !== undefined) {
    options.windowsCodeSigning = buildOptions.windowsCodeSigning;
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

const resolveMuonConfigPathForViteCommand = (
  config: ResolvedConfig,
  options: MuonVitePluginOptions,
): string | undefined => {
  if (config.command !== "build" || typeof options.build !== "object") {
    return undefined;
  }
  return options.build.configPath;
};

const resolveMuonRuntimePluginConfig = (
  capabilityResolver: MuonCapabilityModuleResolver | undefined,
  pluginAccess: MuonResolvedPluginAccessOptions | undefined,
): MuonRuntimePluginConfig =>
  pluginAccess?.mode === "simple"
    ? { mode: "simple", ...pluginAccess.runtimeOverlay }
    : {
        ...(capabilityResolver?.getRuntimePluginConfig() ?? {
          mode: "validate",
          capabilities: [],
        }),
        ...(pluginAccess?.runtimeOverlay ?? {}),
      };

export default muon;
