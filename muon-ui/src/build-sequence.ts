// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import { isAbsolute, resolve } from "node:path";

import type { InlineConfig, ResolvedConfig } from "vite";

import {
  buildMuonApp,
  type MuonBuildOptions,
  type MuonBuildResult,
} from "./build.js";
import {
  flattenVitePluginOptions,
  getMuonVitePluginOptions,
} from "./vite-options.js";
import type { MuonViteBuildOptions, MuonVitePluginOptions } from "./vite.js";
import { mergeMuonWindowsResourceOptions } from "./windows-resource.js";

/**
 * Environment variable used to prevent the Vite plugin build hook from running
 * when a CLI command owns the Muon build sequence.
 */
export const muonBuildSequenceSuppressViteBuildEnvironmentKey =
  "MUON_SUPPRESS_VITE_MUON_BUILD";

/**
 * Resolved project metadata used by the CLI build sequence.
 */
export interface MuonBuildSequenceProject {
  /** Project root used for Muon config and package metadata. */
  root: string;
  /** Root passed to Vite when the sequence must run `vite build`. */
  viteBuildRoot: string;
  /** Absolute Vite output directory used as Muon app assets. */
  viteOutputDirectory: string | undefined;
  /** Muon Vite plugin options if the project config contains the plugin. */
  pluginOptions: MuonVitePluginOptions | undefined;
}

/**
 * Options for running the CLI-oriented Muon build sequence.
 */
export interface MuonBuildSequenceOptions extends MuonBuildOptions {
  /** Target default used only when no Muon Vite plugin defines build options. */
  defaultAllTargets?: boolean;
}

const isMissingVitePackageError = (error: unknown): boolean => {
  const candidate = error as { code?: unknown; message?: unknown };
  return (
    candidate.code === "ERR_MODULE_NOT_FOUND" &&
    typeof candidate.message === "string" &&
    candidate.message.includes("vite")
  );
};

const resolveViteOutputDirectory = (config: ResolvedConfig): string => {
  return isAbsolute(config.build.outDir)
    ? config.build.outDir
    : resolve(config.root, config.build.outDir);
};

const findMuonVitePluginOptions = async (
  plugins: unknown,
): Promise<MuonVitePluginOptions | undefined> => {
  const flattenedPlugins = await flattenVitePluginOptions(plugins);
  const muonPlugins = flattenedPlugins
    .map((plugin) => getMuonVitePluginOptions(plugin))
    .filter(
      (pluginOptions): pluginOptions is MuonVitePluginOptions =>
        pluginOptions !== undefined,
    );

  if (muonPlugins.length > 1) {
    throw new Error(
      "Multiple muon() plugin definitions were found in vite.config.*.",
    );
  }

  return muonPlugins[0];
};

/**
 * Loads Vite configuration only far enough to discover whether a Muon plugin
 * controls the build sequence.
 */
export const loadMuonBuildSequenceProject = async (
  cwd: string,
): Promise<MuonBuildSequenceProject> => {
  const resolvedCwd = resolve(cwd);
  let vite: typeof import("vite");
  try {
    vite = await import("vite");
  } catch (error) {
    if (isMissingVitePackageError(error)) {
      return {
        root: resolvedCwd,
        viteBuildRoot: resolvedCwd,
        viteOutputDirectory: undefined,
        pluginOptions: undefined,
      };
    }
    throw error;
  }

  const resolvedConfig = await vite.resolveConfig(
    { root: resolvedCwd, logLevel: "silent" } satisfies InlineConfig,
    "build",
    "production",
    "production",
  );
  const pluginOptions = await findMuonVitePluginOptions(resolvedConfig.plugins);
  if (pluginOptions === undefined) {
    return {
      root: resolvedCwd,
      viteBuildRoot: resolvedCwd,
      viteOutputDirectory: undefined,
      pluginOptions: undefined,
    };
  }

  return {
    root: resolvedConfig.root,
    viteBuildRoot: resolvedCwd,
    viteOutputDirectory: resolveViteOutputDirectory(resolvedConfig),
    pluginOptions,
  };
};

/**
 * Returns Muon build options from a Muon Vite plugin declaration.
 */
export const resolveMuonViteBuildOptions = (
  pluginOptions: MuonVitePluginOptions | undefined,
): MuonViteBuildOptions => {
  if (pluginOptions?.build === false) {
    throw new Error("Muon build is disabled by muon({ build: false }).");
  }
  return typeof pluginOptions?.build === "object" ? pluginOptions.build : {};
};

const runViteBuild = async (root: string): Promise<void> => {
  const vite = await import("vite");
  const previous =
    process.env[muonBuildSequenceSuppressViteBuildEnvironmentKey];
  process.env[muonBuildSequenceSuppressViteBuildEnvironmentKey] = "1";
  try {
    await vite.build({ root, logLevel: "silent" } satisfies InlineConfig);
  } finally {
    if (previous === undefined) {
      delete process.env[muonBuildSequenceSuppressViteBuildEnvironmentKey];
    } else {
      process.env[muonBuildSequenceSuppressViteBuildEnvironmentKey] = previous;
    }
  }
};

const hasExplicitTargets = (options: MuonBuildSequenceOptions): boolean =>
  options.targets !== undefined && options.targets.length > 0;

const copyDefinedBuildOptions = (
  output: MuonBuildOptions,
  input: MuonBuildSequenceOptions,
  usesViteAssets: boolean,
): void => {
  if (input.assetSourcePath !== undefined) {
    if (usesViteAssets) {
      throw new Error(
        "--assets cannot be used when a muon() Vite plugin provides the build sequence.",
      );
    }
    output.assetSourcePath = input.assetSourcePath;
  }
  if (input.assetPrefix !== undefined) {
    output.assetPrefix = input.assetPrefix;
  }
  if (input.targets !== undefined && input.targets.length > 0) {
    output.targets = input.targets;
    if (input.allTargets === undefined) {
      output.allTargets = false;
    }
  }
  if (input.allTargets !== undefined) {
    output.allTargets = input.allTargets;
  }
  if (input.appName !== undefined) {
    output.appName = input.appName;
  }
  if (input.appId !== undefined) {
    output.appId = input.appId;
  }
  if (input.outputRoot !== undefined) {
    output.outputRoot = input.outputRoot;
  }
  if (input.configPath !== undefined) {
    output.configPath = input.configPath;
  }
  if (input.windowsResource !== undefined) {
    const windowsResource = mergeMuonWindowsResourceOptions(
      input.windowsResource,
      output.windowsResource,
    );
    if (windowsResource !== undefined) {
      output.windowsResource = windowsResource;
    }
  }
  if (input.packageDirectory !== undefined) {
    output.packageDirectory = input.packageDirectory;
  }
  if (input.assetSalt !== undefined) {
    output.assetSalt = input.assetSalt;
  }
};

/**
 * Runs the project build sequence used by `muon build` and `muon pack`.
 */
export const runMuonBuildSequence = async (
  options: MuonBuildSequenceOptions = {},
  loadedProject?: MuonBuildSequenceProject,
): Promise<MuonBuildResult> => {
  const project =
    loadedProject ??
    (await loadMuonBuildSequenceProject(options.root ?? process.cwd()));
  const pluginBuildOptions = resolveMuonViteBuildOptions(project.pluginOptions);
  const usesViteAssets = project.pluginOptions !== undefined;
  const buildOptions: MuonBuildOptions = {
    root: project.root,
  };

  if (usesViteAssets) {
    if (project.viteOutputDirectory === undefined) {
      throw new Error("Vite output directory could not be resolved.");
    }
    Object.assign(buildOptions, pluginBuildOptions);
    buildOptions.assetSourcePath = project.viteOutputDirectory;
    buildOptions.assetPrefix = "main";
    await runViteBuild(project.viteBuildRoot);
  } else if (
    options.defaultAllTargets !== undefined &&
    options.allTargets === undefined &&
    !hasExplicitTargets(options)
  ) {
    buildOptions.allTargets = options.defaultAllTargets;
  }

  copyDefinedBuildOptions(buildOptions, options, usesViteAssets);
  return await buildMuonApp(buildOptions);
};
