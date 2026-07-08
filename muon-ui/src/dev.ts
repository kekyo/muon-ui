// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import { spawn } from "node:child_process";
import { constants } from "node:fs";
import {
  access,
  cp,
  mkdir,
  readFile,
  rm,
  stat,
  writeFile,
} from "node:fs/promises";
import { dirname, isAbsolute, join, resolve } from "node:path";

import { parse } from "json5";
import type { InlineConfig, Plugin, UserConfig } from "vite";

import {
  loadMuonBuildSequenceProject,
  muonBuildSequenceSuppressViteBuildEnvironmentKey,
  resolveMuonViteBuildOptions,
  type MuonBuildSequenceProject,
} from "./build-sequence.js";
import type { MuonRuntimePluginConfig } from "./capability.js";
import { ensureMuonGitignoreEntry } from "./gitignore.js";
import { getDefaultMuonPrepareTarget, runMuonPrepare } from "./prepare.js";
import {
  getMuonExecutablePath,
  resolveMuonRuntimePath,
} from "./vite-internals.js";
import {
  flattenVitePluginOptions,
  getMuonVitePluginOptions,
  getMuonVitePluginRuntimeState,
  type MuonVitePluginRuntimeState,
} from "./vite-options.js";
import { createVitePackagedAssetOptions } from "./vite-assets.js";
import type { MuonVitePluginOptions } from "./vite.js";

const muonRecycleExitCode = 88;

type JsonObject = Record<string, unknown>;

interface LoadedViteMuonOptions {
  root: string;
  pluginOptions: MuonVitePluginOptions | undefined;
}

interface PreparedViteRunAssets {
  root: string;
  pluginOptions: MuonVitePluginOptions;
  configPath: string | undefined;
  assetSourcePath: string;
  browserStartPage: string | undefined;
  runtimePluginConfig: MuonRuntimePluginConfig;
}

interface ProjectConfigResolution {
  configPath: string | undefined;
  config: JsonObject | undefined;
}

interface MuonDevOverrideConfig {
  asset?: {
    sourcePath: string;
  };
  cdp?: {
    enable: true;
  };
  browser?: {
    startPage?: string;
    keybind?: {
      devtools: "f12";
      recycle: "ctrl+f12";
    };
  };
  plugin?: MuonRuntimePluginConfig;
}

/**
 * Options for launching muon directly against local development assets.
 */
export interface MuonDevOptions {
  /**
   * Directory used as the project root.
   */
  root?: string;

  /**
   * Directory containing muon-core runtime files such as muon-core and plugins.
   */
  muonPath?: string;

  /**
   * Directory containing CEF files, or a CEF archive root with Release/Resources.
   */
  cefPath?: string;

  /**
   * Runtime staging directory used for development startup.
   */
  stagePath?: string;

  /**
   * muon config path passed before the generated development override.
   */
  configPath?: string;

  /**
   * Local development asset directory.
   */
  assetSourcePath?: string;

  /**
   * Enable the muon debugger defaults during development startup.
   */
  enableDebugger?: boolean;

  /**
   * Platform used for runtime target resolution.
   *
   * @remarks This is injectable for tests. Production code uses process.platform.
   */
  platform?: NodeJS.Platform;

  /**
   * Architecture used for runtime target resolution.
   *
   * @remarks This is injectable for tests. Production code uses process.arch.
   */
  architecture?: NodeJS.Architecture;

  /**
   * Environment used for child processes.
   */
  environment?: NodeJS.ProcessEnv;

  /**
   * Suppress native builder progress messages.
   */
  quietPrepare?: boolean;
}

/**
 * Result returned after the muon development process exits.
 */
export interface MuonDevResult {
  /**
   * Absolute project root used for development startup.
   */
  root: string;

  /**
   * muon runtime target used for prepare.
   */
  target: string;

  /**
   * Directory containing muon-core files used as the staging source.
   */
  muonPath: string;

  /**
   * Directory or cached archive containing CEF files used as the staging source.
   */
  cefPath: string;

  /**
   * Directory containing the prepared muon runtime.
   */
  stagePath: string;

  /**
   * Prepared muon-core executable path.
   */
  muonExecutablePath: string;

  /**
   * Project config path passed before the generated development override.
   */
  projectConfigPath?: string;

  /**
   * Generated development override config path.
   */
  overrideConfigPath: string;

  /**
   * Effective local asset source path.
   */
  assetSourcePath: string;

  /**
   * Exit code returned by the muon process.
   */
  exitCode: number;
}

const defaultProjectConfigFileNames = ["muon.json5", "muon.jsonc", "muon.json"];

const resolveFromRoot = (root: string, path: string): string =>
  isAbsolute(path) ? path : resolve(root, path);

const isJsonObject = (value: unknown): value is JsonObject =>
  typeof value === "object" && value !== null && !Array.isArray(value);

const getErrorMessage = (error: unknown): string =>
  error instanceof Error ? error.message : String(error);

const fileExists = async (path: string): Promise<boolean> => {
  try {
    const stats = await stat(path);
    return stats.isFile();
  } catch {
    return false;
  }
};

const directoryExists = async (path: string): Promise<boolean> => {
  try {
    const stats = await stat(path);
    return stats.isDirectory();
  } catch {
    return false;
  }
};

const isMissingVitePackageError = (error: unknown): boolean => {
  const candidate = error as { code?: unknown; message?: unknown };
  return (
    candidate.code === "ERR_MODULE_NOT_FOUND" &&
    typeof candidate.message === "string" &&
    candidate.message.includes("vite")
  );
};

const loadViteMuonOptions = async (
  cwd: string,
): Promise<LoadedViteMuonOptions> => {
  let vite: typeof import("vite");
  try {
    vite = await import("vite");
  } catch (error) {
    if (isMissingVitePackageError(error)) {
      return {
        root: resolve(cwd),
        pluginOptions: undefined,
      };
    }
    throw error;
  }

  const loaded = await vite.loadConfigFromFile(
    {
      command: "serve",
      mode: "development",
      isPreview: false,
      isSsrBuild: false,
    },
    undefined,
    cwd,
    "silent",
  );
  if (loaded === null) {
    return {
      root: resolve(cwd),
      pluginOptions: undefined,
    };
  }

  const config = loaded.config as UserConfig;
  const root =
    typeof config.root === "string" ? resolve(cwd, config.root) : resolve(cwd);
  const plugins = await flattenVitePluginOptions(config.plugins);
  const muonPlugins = plugins
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

  return {
    root,
    pluginOptions: muonPlugins[0],
  };
};

const getRuntimePluginConfigFromViteBuild = async (
  root: string,
): Promise<MuonRuntimePluginConfig> => {
  const vite = await import("vite");
  let resolvedRuntimeStates: readonly MuonVitePluginRuntimeState[] = [];
  const inspector: Plugin = {
    name: "muon-run-vite-build-inspector",
    enforce: "post",
    configResolved: (config): void => {
      const runtimeStates = config.plugins
        .map((plugin) => getMuonVitePluginRuntimeState(plugin))
        .filter(
          (runtimeState): runtimeState is MuonVitePluginRuntimeState =>
            runtimeState !== undefined,
        );
      if (runtimeStates.length > 1) {
        throw new Error(
          "Multiple muon() plugin definitions were found in vite.config.*.",
        );
      }
      resolvedRuntimeStates = runtimeStates;
    },
  };
  const previous =
    process.env[muonBuildSequenceSuppressViteBuildEnvironmentKey];
  process.env[muonBuildSequenceSuppressViteBuildEnvironmentKey] = "1";
  try {
    await vite.build({
      root,
      logLevel: "silent",
      plugins: [inspector],
    } satisfies InlineConfig);
  } finally {
    if (previous === undefined) {
      delete process.env[muonBuildSequenceSuppressViteBuildEnvironmentKey];
    } else {
      process.env[muonBuildSequenceSuppressViteBuildEnvironmentKey] = previous;
    }
  }

  const runtimeState = resolvedRuntimeStates[0];
  const runtimePluginConfig = runtimeState?.getRuntimePluginConfig();
  if (runtimePluginConfig === undefined) {
    throw new Error("muon Vite plugin runtime config was not resolved.");
  }
  return runtimePluginConfig;
};

const copyViteOutputAsRunAssets = async (
  project: MuonBuildSequenceProject,
  assetSourcePath: string,
  assetPrefix: string,
): Promise<void> => {
  if (project.viteOutputDirectory === undefined) {
    throw new Error("Vite output directory could not be resolved.");
  }

  await assertDevelopmentAssetDirectory(project.viteOutputDirectory);
  const destinationPath = join(assetSourcePath, ...assetPrefix.split("/"));
  await mkdir(dirname(destinationPath), { recursive: true });
  await cp(project.viteOutputDirectory, destinationPath, {
    recursive: true,
  });
};

const prepareViteRunAssets = async (
  cwd: string,
): Promise<PreparedViteRunAssets | undefined> => {
  const project = await loadMuonBuildSequenceProject(cwd);
  if (project.pluginOptions === undefined) {
    return undefined;
  }

  const buildOptions = resolveMuonViteBuildOptions(project.pluginOptions);
  const packagedAssetOptions = createVitePackagedAssetOptions(
    project.viteBase ?? "/",
  );
  const assetSourcePath = join(project.root, ".muon", "run", "assets");

  await rm(assetSourcePath, { recursive: true, force: true });
  const runtimePluginConfig = await getRuntimePluginConfigFromViteBuild(
    project.viteBuildRoot,
  );
  await copyViteOutputAsRunAssets(
    project,
    assetSourcePath,
    packagedAssetOptions.assetPrefix,
  );

  return {
    root: project.root,
    pluginOptions: project.pluginOptions,
    configPath: buildOptions.configPath,
    assetSourcePath,
    browserStartPage: packagedAssetOptions.browserStartPage,
    runtimePluginConfig,
  };
};

const resolveProjectConfigPath = async (
  root: string,
  configPath: string | undefined,
): Promise<string | undefined> => {
  if (configPath !== undefined) {
    const resolvedPath = resolveFromRoot(root, configPath);
    if (!(await fileExists(resolvedPath))) {
      throw new Error(`muon config file does not exist: ${resolvedPath}`);
    }
    return resolvedPath;
  }

  for (const fileName of defaultProjectConfigFileNames) {
    const candidatePath = join(root, fileName);
    if (await fileExists(candidatePath)) {
      return candidatePath;
    }
  }

  return undefined;
};

const readJsonObjectFile = async (
  filePath: string,
  label: string,
): Promise<JsonObject> => {
  let content: string;
  try {
    content = await readFile(filePath, "utf8");
  } catch (error) {
    throw new Error(
      `${label} could not be read: ${filePath}: ${getErrorMessage(error)}`,
    );
  }

  let parsed: unknown;
  try {
    parsed = parse(content);
  } catch (error) {
    throw new Error(
      `${label} could not be parsed: ${filePath}: ${getErrorMessage(error)}`,
    );
  }

  if (!isJsonObject(parsed)) {
    throw new Error(`${label} must contain a JSON object: ${filePath}`);
  }

  return parsed;
};

const resolveProjectConfig = async (
  root: string,
  configPath: string | undefined,
): Promise<ProjectConfigResolution> => {
  const resolvedConfigPath = await resolveProjectConfigPath(root, configPath);
  if (resolvedConfigPath === undefined) {
    return {
      configPath: undefined,
      config: undefined,
    };
  }

  return {
    configPath: resolvedConfigPath,
    config: await readJsonObjectFile(resolvedConfigPath, "muon config"),
  };
};

const readConfigAssetSourcePath = (
  config: JsonObject | undefined,
): string | undefined => {
  if (config === undefined) {
    return undefined;
  }

  const asset = config.asset;
  if (asset === undefined) {
    return undefined;
  }
  if (!isJsonObject(asset)) {
    throw new Error("muon config asset must be an object when present.");
  }

  const sourcePath = asset.sourcePath;
  if (sourcePath === undefined) {
    return undefined;
  }
  if (typeof sourcePath !== "string") {
    throw new Error("muon config asset.sourcePath must be a string.");
  }
  if (sourcePath.trim().length === 0) {
    throw new Error("muon config asset.sourcePath must not be empty.");
  }
  return sourcePath;
};

const hasConfigBrowserStartPage = (config: JsonObject | undefined): boolean => {
  if (config === undefined) {
    return false;
  }

  const browser = config.browser;
  return isJsonObject(browser) && browser.startPage !== undefined;
};

const assertDevelopmentAssetDirectory = async (
  assetSourcePath: string,
): Promise<void> => {
  if (await directoryExists(assetSourcePath)) {
    return;
  }

  try {
    await access(assetSourcePath, constants.F_OK);
  } catch {
    throw new Error(`muon run asset source does not exist: ${assetSourcePath}`);
  }

  throw new Error(
    `muon run asset source must be a directory: ${assetSourcePath}`,
  );
};

const resolveAssetSource = async (
  root: string,
  assetSourcePath: string | undefined,
  projectConfig: ProjectConfigResolution,
): Promise<{
  assetSourcePath: string;
  overrideAssetSourcePath: string | undefined;
}> => {
  const configuredAssetSourcePath = readConfigAssetSourcePath(
    projectConfig.config,
  );
  if (assetSourcePath !== undefined) {
    const resolvedPath = resolveFromRoot(root, assetSourcePath);
    await assertDevelopmentAssetDirectory(resolvedPath);
    return {
      assetSourcePath: resolvedPath,
      overrideAssetSourcePath: resolvedPath,
    };
  }

  if (
    configuredAssetSourcePath !== undefined &&
    projectConfig.configPath !== undefined
  ) {
    return {
      assetSourcePath: resolve(
        dirname(projectConfig.configPath),
        configuredAssetSourcePath,
      ),
      overrideAssetSourcePath: undefined,
    };
  }

  const defaultAssetSourcePath = resolve(root, "assets");
  await assertDevelopmentAssetDirectory(defaultAssetSourcePath);
  return {
    assetSourcePath: defaultAssetSourcePath,
    overrideAssetSourcePath: defaultAssetSourcePath,
  };
};

const createMuonDevOverrideConfig = (
  assetSourcePath: string | undefined,
  enableDebugger: boolean,
  browserStartPage: string | undefined,
  runtimePluginConfig: MuonRuntimePluginConfig | undefined,
): MuonDevOverrideConfig => {
  const browser =
    browserStartPage === undefined && !enableDebugger
      ? undefined
      : {
          ...(browserStartPage === undefined
            ? {}
            : { startPage: browserStartPage }),
          ...(enableDebugger
            ? {
                keybind: {
                  devtools: "f12" as const,
                  recycle: "ctrl+f12" as const,
                },
              }
            : {}),
        };

  return {
    ...(assetSourcePath === undefined
      ? {}
      : {
          asset: {
            sourcePath: assetSourcePath,
          },
        }),
    ...(enableDebugger
      ? {
          cdp: {
            enable: true,
          },
        }
      : {}),
    ...(browser === undefined ? {} : { browser }),
    ...(runtimePluginConfig === undefined
      ? {}
      : { plugin: runtimePluginConfig }),
  };
};

const writeMuonDevOverrideConfig = async (
  overrideConfigPath: string,
  overrideConfig: MuonDevOverrideConfig,
): Promise<void> => {
  await mkdir(dirname(overrideConfigPath), { recursive: true });
  await writeFile(
    overrideConfigPath,
    `${JSON.stringify(overrideConfig, null, 2)}\n`,
  );
};

const runMuonExecutable = async (
  muonExecutablePath: string,
  configPaths: readonly string[],
  environment: NodeJS.ProcessEnv,
): Promise<number> => {
  const args = configPaths.flatMap((configPath) => ["-c", configPath]);
  const child = spawn(muonExecutablePath, args, {
    cwd: dirname(muonExecutablePath),
    env: environment,
    stdio: ["ignore", "inherit", "inherit"],
  });

  return await new Promise<number>((resolvePromise, reject) => {
    child.once("error", reject);
    child.once("close", (code) => {
      resolvePromise(code ?? 1);
    });
  });
};

const runMuonDevOnce = async (
  options: MuonDevOptions,
  cwd: string,
): Promise<MuonDevResult> => {
  const preparedViteAssets =
    options.assetSourcePath === undefined
      ? await prepareViteRunAssets(cwd)
      : undefined;
  const loadedViteOptions =
    preparedViteAssets === undefined
      ? await loadViteMuonOptions(cwd)
      : {
          root: preparedViteAssets.root,
          pluginOptions: preparedViteAssets.pluginOptions,
        };
  const root = loadedViteOptions.root;
  const pluginOptions = loadedViteOptions.pluginOptions;
  const platform = options.platform ?? process.platform;
  const architecture = options.architecture ?? process.arch;
  const environment = options.environment ?? process.env;
  const target = getDefaultMuonPrepareTarget(platform, architecture);
  const muonPath = resolveMuonRuntimePath({
    root,
    target,
    muonPath: options.muonPath ?? pluginOptions?.muonPath,
  });
  const cefPath =
    options.cefPath !== undefined
      ? resolveFromRoot(root, options.cefPath)
      : pluginOptions?.cefPath === undefined
        ? undefined
        : resolveFromRoot(root, pluginOptions.cefPath);
  const stagePath =
    options.stagePath !== undefined
      ? resolveFromRoot(root, options.stagePath)
      : pluginOptions?.stagePath === undefined
        ? resolve(root, ".muon", target)
        : resolveFromRoot(root, pluginOptions.stagePath);
  const enableDebugger =
    options.enableDebugger ?? pluginOptions?.enableDebugger ?? true;
  const projectConfig = await resolveProjectConfig(
    root,
    options.configPath ?? preparedViteAssets?.configPath,
  );
  const asset =
    preparedViteAssets === undefined
      ? await resolveAssetSource(root, options.assetSourcePath, projectConfig)
      : {
          assetSourcePath: preparedViteAssets.assetSourcePath,
          overrideAssetSourcePath: preparedViteAssets.assetSourcePath,
        };
  const browserStartPage =
    preparedViteAssets?.browserStartPage !== undefined &&
    !hasConfigBrowserStartPage(projectConfig.config)
      ? preparedViteAssets.browserStartPage
      : undefined;
  const overrideConfigPath = join(root, ".muon", "run", "muon.run.json");

  await ensureMuonGitignoreEntry(root);

  const preparedRuntime = await runMuonPrepare({
    muonPath,
    cefPath,
    stageDir: stagePath,
    target,
    cacheDir: environment.MUON_CACHE_DIR,
    force: false,
    quiet: options.quietPrepare === true,
    prepareExecutablePath: undefined,
    environment,
    cwd: root,
  });
  if (preparedRuntime.stagePath === undefined) {
    throw new Error("muon-builder did not return a staged runtime path.");
  }

  await writeMuonDevOverrideConfig(
    overrideConfigPath,
    createMuonDevOverrideConfig(
      asset.overrideAssetSourcePath,
      enableDebugger,
      browserStartPage,
      preparedViteAssets?.runtimePluginConfig,
    ),
  );
  const muonExecutablePath = getMuonExecutablePath(
    preparedRuntime.stagePath,
    platform,
  );
  const configPaths = [
    ...(projectConfig.configPath === undefined
      ? []
      : [projectConfig.configPath]),
    overrideConfigPath,
  ];
  const exitCode = await runMuonExecutable(
    muonExecutablePath,
    configPaths,
    environment,
  );

  return {
    root,
    target,
    muonPath: preparedRuntime.muonPath,
    cefPath: preparedRuntime.cefPath,
    stagePath: preparedRuntime.stagePath,
    muonExecutablePath,
    ...(projectConfig.configPath === undefined
      ? {}
      : { projectConfigPath: projectConfig.configPath }),
    overrideConfigPath,
    assetSourcePath: asset.assetSourcePath,
    exitCode,
  };
};

/**
 * Launches muon directly against local development assets.
 *
 * @param options Development startup options.
 * @returns Development startup result after the muon process exits.
 */
export const runMuonDev = async (
  options: MuonDevOptions = {},
): Promise<MuonDevResult> => {
  const cwd = resolve(options.root ?? process.cwd());
  let result: MuonDevResult;
  do {
    result = await runMuonDevOnce(options, cwd);
  } while (result.exitCode === muonRecycleExitCode);
  return result;
};
