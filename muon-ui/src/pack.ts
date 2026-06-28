// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import { spawn } from "node:child_process";
import { type Stats } from "node:fs";
import {
  chmod,
  cp,
  mkdir,
  readFile,
  readdir,
  rm,
  stat,
  writeFile,
} from "node:fs/promises";
import {
  basename,
  dirname,
  isAbsolute,
  join,
  relative,
  resolve,
  sep,
} from "node:path";

import AdmZip from "adm-zip";
import { parse } from "json5";
import type { InlineConfig, UserConfig } from "vite";

import {
  buildMuonApp,
  type MuonBuildOptions,
  type MuonBuildResult,
  type MuonBuildTarget,
  type MuonBuildTargetResult,
} from "./build.js";
import {
  flattenVitePluginOptions,
  getMuonVitePluginOptions,
} from "./vite-options.js";
import type { MuonViteBuildOptions, MuonVitePluginOptions } from "./vite.js";
import { getMuonTargetDescriptor } from "./targets.js";

const supportedPackTypes = ["zip", "deb", "nsis"] as const;
const defaultArtifactsDirectory = "artifacts";
const defaultPackageBuildDirectory = ".muon/pack";
const suppressViteMuonBuildEnvironmentKey = "MUON_SUPPRESS_VITE_MUON_BUILD";

type JsonObject = Record<string, unknown>;

/**
 * Muon package output type.
 */
export type MuonPackType = (typeof supportedPackTypes)[number];

/**
 * Options for creating redistributable Muon package artifacts.
 */
export interface MuonPackOptions {
  /**
   * Project root.
   */
  root?: string;
  /**
   * Package artifact types to generate.
   */
  types: readonly string[];
  /**
   * Public target identifiers to build.
   */
  targets?: readonly string[];
  /**
   * Build every supported target.
   */
  allTargets?: boolean;
  /**
   * Muon config path to embed.
   */
  configPath?: string;
  /**
   * File name used for the app launcher.
   */
  appName?: string;
  /**
   * Stable application identifier used for portable runtime state.
   */
  appId?: string;
  /**
   * Directory containing package runtime/ and native/ folders.
   */
  packageDirectory?: string;
  /**
   * Directory that receives generated package artifacts.
   */
  artifactsDir?: string;
  /**
   * Package name override.
   */
  packageName?: string;
  /**
   * Package version override.
   */
  packageVersion?: string;
  /**
   * Package description override.
   */
  description?: string;
  /**
   * Package author/maintainer override.
   */
  author?: string;
  /**
   * Environment used for child processes.
   */
  environment?: NodeJS.ProcessEnv;
}

/**
 * Generated package artifact metadata.
 */
export interface MuonPackArtifact {
  /**
   * Package artifact type.
   */
  type: MuonPackType;
  /**
   * Muon target packaged in this artifact.
   */
  target: MuonBuildTarget;
  /**
   * Generated artifact path.
   */
  path: string;
}

/**
 * Result of a Muon package build.
 */
export interface MuonPackResult {
  /**
   * Absolute project root used for packaging.
   */
  root: string;
  /**
   * Debian/installer safe package name.
   */
  packageName: string;
  /**
   * Package version.
   */
  version: string;
  /**
   * Launcher file base name.
   */
  appName: string;
  /**
   * Stable app identifier embedded in launcher/runtime config.
   */
  appId: string;
  /**
   * Muon dist build result.
   */
  build: MuonBuildResult;
  /**
   * Target dist directories used as package inputs.
   */
  targets: MuonBuildTargetResult[];
  /**
   * Generated package artifacts.
   */
  artifacts: MuonPackArtifact[];
}

interface LoadedViteMuonOptions {
  root: string;
  pluginOptions: MuonVitePluginOptions | undefined;
}

interface PackageMetadata {
  packageName: string;
  version: string;
  description: string;
  author: string;
}

const isJsonObject = (value: unknown): value is JsonObject =>
  typeof value === "object" && value !== null && !Array.isArray(value);

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
      command: "build",
      mode: "production",
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

const readJsonObjectFile = async (path: string): Promise<JsonObject> => {
  const parsed = parse(await readFile(path, "utf8"));
  if (!isJsonObject(parsed)) {
    throw new Error(`JSON file must contain an object: ${path}`);
  }
  return parsed;
};

const readPackageJson = async (root: string): Promise<JsonObject> => {
  return await readJsonObjectFile(join(root, "package.json"));
};

const sanitizePackageName = (value: string): string => {
  const unscoped = value.startsWith("@")
    ? value.slice(value.indexOf("/") + 1)
    : value;
  const sanitized = unscoped
    .trim()
    .toLowerCase()
    .replace(/[^a-z0-9+.-]+/g, "-")
    .replace(/^[.+-]+/g, "")
    .replace(/[.+-]+$/g, "");
  return sanitized.length > 0 ? sanitized : "muon-app";
};

const stringifyAuthor = (value: unknown): string | undefined => {
  if (typeof value === "string") {
    return value;
  }
  if (!isJsonObject(value) || typeof value.name !== "string") {
    return undefined;
  }
  return value.email === undefined || typeof value.email !== "string"
    ? value.name
    : `${value.name} <${value.email}>`;
};

const resolveMetadata = (
  packageJson: JsonObject,
  options: MuonPackOptions,
): PackageMetadata => {
  const packageNameSource =
    options.packageName ??
    (typeof packageJson.name === "string" ? packageJson.name : undefined);
  if (packageNameSource === undefined || packageNameSource.trim() === "") {
    throw new Error("package.json name is required for muon pack.");
  }
  const version =
    options.packageVersion ??
    (typeof packageJson.version === "string" ? packageJson.version : undefined);
  if (version === undefined || version.trim() === "") {
    throw new Error("package.json version is required for muon pack.");
  }
  const description =
    options.description ??
    (typeof packageJson.description === "string"
      ? packageJson.description
      : "Muon application");
  const author =
    options.author ?? stringifyAuthor(packageJson.author) ?? "Unknown";
  return {
    packageName: sanitizePackageName(packageNameSource),
    version,
    description,
    author,
  };
};

const normalizePackTypes = (types: readonly string[]): MuonPackType[] => {
  const normalized = types
    .flatMap((value) => value.split(","))
    .map((value) => value.trim().toLowerCase())
    .filter((value) => value.length > 0);
  if (normalized.length === 0) {
    throw new Error("Specify at least one package type with --type.");
  }
  for (const type of normalized) {
    if (!supportedPackTypes.includes(type as MuonPackType)) {
      throw new Error(`Unsupported muon pack type: ${type}`);
    }
  }
  return [...new Set(normalized)] as MuonPackType[];
};

const getPluginBuildOptions = (
  pluginOptions: MuonVitePluginOptions | undefined,
): MuonViteBuildOptions => {
  return typeof pluginOptions?.build === "object" ? pluginOptions.build : {};
};

const resolveBuildOutputDirectory = async (root: string): Promise<string> => {
  const vite = await import("vite");
  const resolved = await vite.resolveConfig(
    { root } satisfies InlineConfig,
    "build",
    "production",
    "production",
  );
  return isAbsolute(resolved.build.outDir)
    ? resolved.build.outDir
    : resolve(resolved.root, resolved.build.outDir);
};

const runViteBuild = async (
  root: string,
  _environment: NodeJS.ProcessEnv,
): Promise<void> => {
  const vite = await import("vite");
  const previous = process.env[suppressViteMuonBuildEnvironmentKey];
  process.env[suppressViteMuonBuildEnvironmentKey] = "1";
  try {
    await vite.build({ root } satisfies InlineConfig);
  } finally {
    if (previous === undefined) {
      delete process.env[suppressViteMuonBuildEnvironmentKey];
    } else {
      process.env[suppressViteMuonBuildEnvironmentKey] = previous;
    }
  }
};

const createBuildOptions = (
  root: string,
  assetSourcePath: string,
  pluginBuildOptions: MuonViteBuildOptions,
  options: MuonPackOptions,
): MuonBuildOptions => {
  const buildOptions: MuonBuildOptions = {
    root,
    assetSourcePath,
    assetPrefix: "main",
  };
  Object.assign(buildOptions, pluginBuildOptions);
  if (options.targets !== undefined && options.targets.length > 0) {
    buildOptions.targets = options.targets;
    buildOptions.allTargets = false;
  } else if (options.allTargets !== undefined) {
    buildOptions.allTargets = options.allTargets;
  }
  if (options.configPath !== undefined) {
    buildOptions.configPath = options.configPath;
  }
  if (options.appName !== undefined) {
    buildOptions.appName = options.appName;
  }
  if (options.appId !== undefined) {
    buildOptions.appId = options.appId;
  }
  if (options.packageDirectory !== undefined) {
    buildOptions.packageDirectory = options.packageDirectory;
  }
  return buildOptions;
};

const assertPackTypeSupportsTarget = (
  type: MuonPackType,
  target: MuonBuildTarget,
): void => {
  const descriptor = getMuonTargetDescriptor(target);
  if (type === "deb" && descriptor.os !== "linux") {
    throw new Error("deb packaging supports only Linux targets.");
  }
  if (type === "nsis" && descriptor.os !== "windows") {
    throw new Error("nsis packaging supports only Windows targets.");
  }
};

const runTool = async (
  executable: string,
  args: readonly string[],
  cwd: string,
  environment: NodeJS.ProcessEnv,
): Promise<void> => {
  const child = spawn(executable, [...args], {
    cwd,
    env: environment,
    stdio: ["ignore", "pipe", "pipe"],
  });
  let stderr = "";
  child.stderr.setEncoding("utf8");
  child.stderr.on("data", (chunk: string) => {
    stderr += chunk;
  });
  const exitCode = await new Promise<number>((resolvePromise, reject) => {
    child.once("error", reject);
    child.once("close", (code) => {
      resolvePromise(code ?? 1);
    });
  });
  if (exitCode !== 0) {
    throw new Error(
      `${executable} failed with exit code ${exitCode}: ${stderr.trim()}`,
    );
  }
};

const statOrUndefined = async (path: string): Promise<Stats | undefined> => {
  try {
    return await stat(path);
  } catch {
    return undefined;
  }
};

const addDirectoryToZip = async (
  zip: AdmZip,
  directory: string,
  entryRoot: string,
): Promise<void> => {
  const walk = async (currentDirectory: string): Promise<void> => {
    const entries = await readdir(currentDirectory, { withFileTypes: true });
    entries.sort((left, right) => left.name.localeCompare(right.name));
    for (const entry of entries) {
      const path = join(currentDirectory, entry.name);
      if (entry.isDirectory()) {
        await walk(path);
      } else if (entry.isFile()) {
        const stats = await stat(path);
        const relativePath = relative(directory, path).split(sep).join("/");
        zip.addFile(
          `${entryRoot}/${relativePath}`,
          await readFile(path),
          undefined,
          stats.mode << 16,
        );
      }
    }
  };
  await walk(directory);
};

const packageZip = async (
  target: MuonBuildTargetResult,
  metadata: PackageMetadata,
  artifactsRoot: string,
): Promise<MuonPackArtifact> => {
  const zip = new AdmZip();
  await addDirectoryToZip(
    zip,
    target.outputPath,
    target.distributionDirectoryName,
  );
  const outputPath = join(
    artifactsRoot,
    `${metadata.packageName}-${metadata.version}-${target.target}.zip`,
  );
  await mkdir(dirname(outputPath), { recursive: true });
  await writeFile(outputPath, zip.toBuffer());
  return {
    type: "zip",
    target: target.target,
    path: outputPath,
  };
};

const packageDeb = async (
  root: string,
  target: MuonBuildTargetResult,
  metadata: PackageMetadata,
  artifactsRoot: string,
  packageBuildRoot: string,
  environment: NodeJS.ProcessEnv,
): Promise<MuonPackArtifact> => {
  const descriptor = getMuonTargetDescriptor(target.target);
  if (descriptor.os !== "linux") {
    throw new Error(`Unsupported deb target: ${target.target}`);
  }
  const architecture = descriptor.arch;
  const packageRoot = join(
    packageBuildRoot,
    "deb",
    `${metadata.packageName}-${target.target}`,
  );
  await rm(packageRoot, { recursive: true, force: true });
  const installRoot = join(packageRoot, "usr", "lib", metadata.packageName);
  const installedDist = join(installRoot, target.distributionDirectoryName);
  await mkdir(installedDist, { recursive: true });
  await cp(target.outputPath, installedDist, { recursive: true });
  const binPath = join(packageRoot, "usr", "bin", metadata.packageName);
  const launcherName = basename(target.launcherPath);
  await mkdir(dirname(binPath), { recursive: true });
  await writeFile(
    binPath,
    [
      "#!/usr/bin/env bash",
      "set -euo pipefail",
      `exec ${JSON.stringify(`/usr/lib/${metadata.packageName}/${target.distributionDirectoryName}/${launcherName}`)} "$@"`,
      "",
    ].join("\n"),
  );
  await chmod(binPath, 0o755);
  const controlPath = join(packageRoot, "DEBIAN", "control");
  await mkdir(dirname(controlPath), { recursive: true });
  await writeFile(
    controlPath,
    [
      `Package: ${metadata.packageName}`,
      `Version: ${metadata.version}`,
      `Architecture: ${architecture}`,
      `Maintainer: ${metadata.author}`,
      `Description: ${metadata.description}`,
      "",
    ].join("\n"),
  );
  const outputPath = join(
    artifactsRoot,
    `${metadata.packageName}-${metadata.version}-${architecture}.deb`,
  );
  await mkdir(dirname(outputPath), { recursive: true });
  await runTool(
    "dpkg-deb",
    ["--build", packageRoot, outputPath],
    root,
    environment,
  );
  return {
    type: "deb",
    target: target.target,
    path: outputPath,
  };
};

const escapeNsis = (value: string): string =>
  value.replaceAll("\\", "\\\\").replaceAll('"', '$\\"');

const packageNsis = async (
  root: string,
  target: MuonBuildTargetResult,
  metadata: PackageMetadata,
  artifactsRoot: string,
  packageBuildRoot: string,
  environment: NodeJS.ProcessEnv,
): Promise<MuonPackArtifact> => {
  const descriptor = getMuonTargetDescriptor(target.target);
  if (descriptor.os !== "windows") {
    throw new Error(`Unsupported nsis target: ${target.target}`);
  }
  const scriptPath = join(
    packageBuildRoot,
    "nsis",
    `${metadata.packageName}-${target.target}.nsi`,
  );
  const outputPath = join(
    artifactsRoot,
    `${metadata.packageName}-${metadata.version}-${descriptor.arch}-setup.exe`,
  );
  await mkdir(dirname(scriptPath), { recursive: true });
  await mkdir(dirname(outputPath), { recursive: true });
  await writeFile(
    scriptPath,
    [
      "Unicode true",
      `Name "${escapeNsis(metadata.packageName)}"`,
      `OutFile "${escapeNsis(outputPath)}"`,
      `InstallDir "$LOCALAPPDATA\\Programs\\${escapeNsis(metadata.packageName)}"`,
      "RequestExecutionLevel user",
      "Section",
      '  SetOutPath "$INSTDIR"',
      `  File /r "${escapeNsis(target.outputPath)}\\*"`,
      `  CreateShortCut "$SMPROGRAMS\\${escapeNsis(metadata.packageName)}.lnk" "$INSTDIR\\${escapeNsis(basename(target.launcherPath))}"`,
      "SectionEnd",
      "",
    ].join("\n"),
  );
  await runTool("makensis", [scriptPath], root, environment);
  if ((await statOrUndefined(outputPath)) === undefined) {
    throw new Error(`makensis did not create installer: ${outputPath}`);
  }
  return {
    type: "nsis",
    target: target.target,
    path: outputPath,
  };
};

/**
 * Builds a Vite app, creates Muon dist directories, and packages them.
 *
 * @param options Pack options.
 * @returns Generated package artifacts.
 */
export const packMuonApp = async (
  options: MuonPackOptions,
): Promise<MuonPackResult> => {
  const cwd = resolve(options.root ?? process.cwd());
  const environment = options.environment ?? process.env;
  const loadedOptions = await loadViteMuonOptions(cwd);
  const root = loadedOptions.root;
  const metadata = resolveMetadata(await readPackageJson(root), options);
  const artifactsRoot = resolve(
    root,
    options.artifactsDir ?? defaultArtifactsDirectory,
  );
  const packageBuildRoot = resolve(root, defaultPackageBuildDirectory);
  const types = normalizePackTypes(options.types);
  const viteOutputDirectory = await resolveBuildOutputDirectory(root);
  await runViteBuild(root, environment);
  const build = await buildMuonApp(
    createBuildOptions(
      root,
      viteOutputDirectory,
      getPluginBuildOptions(loadedOptions.pluginOptions),
      options,
    ),
  );
  for (const type of types) {
    for (const target of build.targets) {
      assertPackTypeSupportsTarget(type, target.target);
    }
  }
  await rm(packageBuildRoot, { recursive: true, force: true });
  await rm(join(artifactsRoot, "deb"), { recursive: true, force: true });
  await rm(join(artifactsRoot, "nsis"), { recursive: true, force: true });
  await mkdir(artifactsRoot, { recursive: true });
  const artifacts: MuonPackArtifact[] = [];
  for (const target of build.targets) {
    for (const type of types) {
      if (type === "zip") {
        artifacts.push(await packageZip(target, metadata, artifactsRoot));
      } else if (type === "deb") {
        artifacts.push(
          await packageDeb(
            root,
            target,
            metadata,
            artifactsRoot,
            packageBuildRoot,
            environment,
          ),
        );
      } else {
        artifacts.push(
          await packageNsis(
            root,
            target,
            metadata,
            artifactsRoot,
            packageBuildRoot,
            environment,
          ),
        );
      }
    }
  }
  return {
    root,
    packageName: metadata.packageName,
    version: metadata.version,
    appName: build.appName,
    appId: build.appId,
    build,
    targets: build.targets,
    artifacts,
  };
};

export const muonPackSuppressViteBuildEnvironmentKey =
  suppressViteMuonBuildEnvironmentKey;
