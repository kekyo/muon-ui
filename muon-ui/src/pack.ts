// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

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
import { basename, dirname, join, relative, resolve, sep } from "node:path";

import AdmZip from "adm-zip";
import { parse } from "json5";
import {
  createDirectoryItem,
  createReadFileItem,
  createTarPacker,
  storeReaderToFile,
  type EntryItem,
} from "tar-vern";

import {
  buildMuonApp,
  getDefaultMuonBuildTarget,
  type MuonBuildOptions,
  type MuonBuildResult,
  type MuonBuildTarget,
  type MuonBuildTargetResult,
} from "./build.js";
import {
  loadMuonBuildSequenceProject,
  muonBuildSequenceSuppressViteBuildEnvironmentKey,
  resolveMuonViteBuildOptions,
  runMuonBuildSequence,
  type MuonBuildSequenceProject,
  type MuonBuildSequenceOptions,
} from "./build-sequence.js";
import type { MuonViteBuildOptions } from "./vite.js";
import { createVitePackagedAssetOptions } from "./vite-assets.js";
import {
  allMuonTargets,
  getMuonTargetDescriptor,
  normalizeMuonTarget,
} from "./targets.js";
import {
  mergeMuonWindowsResourceOptions,
  readMuonConfigForWindowsResource,
  resolveMuonWindowsResource,
  updateWindowsPeIconResource,
  updateWindowsPeResources,
  type MuonWindowsResourceOptions,
  type ResolvedMuonWindowsResource,
} from "./windows-resource.js";
import {
  includesWindowsCodeSigningTarget,
  resolveMuonWindowsCodeSigning,
  signWindowsExecutable,
  type MuonWindowsCodeSigningOptions,
  type MuonWindowsCodeSigningTarget,
  type ResolvedMuonWindowsCodeSigning,
} from "./windows-code-signing.js";
import { createWindowsIconFromPngFile } from "./windows-icon.js";
import {
  createLinuxDesktopEntry,
  mergeMuonLinuxDesktopOptions,
  quoteDesktopExecArgument,
  type MuonLinuxDesktopOptions,
} from "./linux-desktop.js";
import {
  assertMuonNodeProjectPackCleanupIsSafe,
  resolveMuonNodeProject,
} from "./node-project.js";
import type { MuonProgressCallback } from "./progress.js";

const supportedPackTypes = ["zip", "tar.gz", "deb", "nsis"] as const;
const supportedLinuxSandboxModes = ["disabled", "setuid"] as const;
const defaultArtifactsDirectory = "artifacts";
const defaultPackageBuildDirectory = ".muon/pack";
const systemRuntimeRoot = "/var/lib/muon/apps";
const systemCefCacheRoot = "/var/cache/muon/cef";
const runtimeHelperExecutableName = "muon-runtime-helper";
const portableProfilePath = "profile";
const portableInstallMetadata = {
  type: "portable",
  runtimeMode: "in-place",
} as const;
// Keep both names for Debian t64 and pre-t64 runtime packages.
const debRuntimeDependencies = [
  "libc6",
  "libgcc-s1",
  "libstdc++6",
  "libffi8",
  "libexpat1",
  "libdbus-1-3",
  "libglib2.0-0t64 | libglib2.0-0",
  "libgtk-3-0t64 | libgtk-3-0",
  "libatk-bridge2.0-0t64 | libatk-bridge2.0-0",
  "libatk1.0-0t64 | libatk1.0-0",
  "libatspi2.0-0t64 | libatspi2.0-0",
  "libcups2t64 | libcups2",
  "libnspr4",
  "libnss3",
  "libgbm1",
  "libasound2t64 | libasound2",
  "libudev1",
  "libx11-6",
  "libxcb1",
  "libxcomposite1",
  "libxdamage1",
  "libxext6",
  "libxfixes3",
  "libxi6",
  "libxkbcommon0",
  "libxrandr2",
  "libcairo2",
  "libpango-1.0-0",
] as const;

type JsonObject = Record<string, unknown>;

/**
 * muon package output type.
 */
export type MuonPackType = (typeof supportedPackTypes)[number];

/**
 * Linux CEF sandbox strategy used by deb packages.
 */
export type MuonLinuxSandboxMode = (typeof supportedLinuxSandboxModes)[number];

/**
 * Options for creating redistributable muon package artifacts.
 */
export interface MuonPackOptions {
  /**
   * Project root.
   */
  root?: string;
  /**
   * Package artifact types to generate.
   *
   * @remarks Defaults to zip, tar.gz, deb, and nsis when omitted.
   */
  types?: readonly string[];
  /**
   * Public target identifiers to build.
   */
  targets?: readonly string[];
  /**
   * Build every supported target.
   */
  allTargets?: boolean;
  /**
   * muon config path to embed.
   */
  configPath?: string;
  /**
   * Static application icon PNG file path.
   *
   * @remarks Used for Windows PE/NSIS resources, Linux desktop entries, and
   * the generated initial title bar icon asset unless a target-specific icon
   * override is supplied.
   */
  iconPath?: string;
  /**
   * File name used for the app launcher.
   */
  appName?: string;
  /**
   * Stable base application identifier used for runtime app identity.
   *
   * @remarks Windows target distributions embed `<appId>.<arch>` as their
   * runtime app identifier. Linux targets embed this value unchanged.
   */
  appId?: string;
  /**
   * Windows PE and NSIS resource metadata.
   *
   * @defaultValue Uses Vite build options, `muon.json` `windows.resource`,
   * `project.json`, `package.json`, then muon defaults.
   */
  windowsResource?: MuonWindowsResourceOptions;
  /**
   * Windows code signing command for generated executable artifacts.
   *
   * @remarks Set false to disable `muon.json` `windows.codeSigning`.
   * The command is supplied by the application project or CI environment.
   */
  windowsCodeSigning?: false | MuonWindowsCodeSigningOptions;
  /**
   * Linux desktop entry metadata.
   *
   * @defaultValue Uses Vite build options, `muon.json` `linux.desktop`,
   * package metadata, then muon defaults.
   */
  linuxDesktop?: MuonLinuxDesktopOptions;
  /**
   * Linux deb CEF sandbox mode.
   *
   * @remarks `setuid` is opt-in and valid only when packaging Linux deb
   * artifacts. The default `disabled` preserves the existing no-sandbox
   * runtime behavior.
   */
  linuxSandbox?: string;
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
   * muon target packaged in this artifact.
   */
  target: MuonBuildTarget;
  /**
   * Generated artifact path.
   */
  path: string;
}

/**
 * Result of a muon package build.
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
   * Stable base app identifier used to derive target runtime app identifiers.
   */
  appId: string;
  /**
   * muon dist build result.
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

interface PackageMetadata {
  packageName: string;
  version: string;
  description: string;
  author: string;
}

interface MuonPackTargetPlan {
  target: MuonBuildTarget;
  types: MuonPackType[];
}

interface InternalMuonPackOptions extends MuonPackOptions {
  progress?: MuonProgressCallback;
}

interface InternalMuonBuildSequenceOptions extends MuonBuildSequenceOptions {
  environment?: NodeJS.ProcessEnv;
  progress?: MuonProgressCallback;
}

interface InternalMuonBuildOptions extends MuonBuildOptions {
  browserProfilePathOverride: string | undefined;
  environment?: NodeJS.ProcessEnv;
  progress?: MuonProgressCallback;
}

const isJsonObject = (value: unknown): value is JsonObject =>
  typeof value === "object" && value !== null && !Array.isArray(value);

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
      : "muon application");
  const author =
    options.author ?? stringifyAuthor(packageJson.author) ?? "Unknown";
  return {
    packageName: sanitizePackageName(packageNameSource),
    version,
    description,
    author,
  };
};

const createPackMetadataPackageJson = (
  packageJson: JsonObject,
  metadata: PackageMetadata,
): JsonObject => ({
  ...packageJson,
  version: metadata.version,
});

const normalizePackTypes = (
  types: readonly string[] | undefined,
): MuonPackType[] => {
  if (types === undefined) {
    return [...supportedPackTypes];
  }
  const normalized = types
    .flatMap((value) => value.split(","))
    .map((value) => value.trim().toLowerCase())
    .map((value) => (value === "tgz" ? "tar.gz" : value))
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

const normalizeLinuxSandboxMode = (
  mode: string | undefined,
): MuonLinuxSandboxMode => {
  if (mode === undefined) {
    return "disabled";
  }
  const normalized = mode.trim().toLowerCase();
  if (supportedLinuxSandboxModes.includes(normalized as MuonLinuxSandboxMode)) {
    return normalized as MuonLinuxSandboxMode;
  }
  throw new Error(`Unsupported Linux sandbox mode: ${mode}`);
};

const packTargetSelectorTargets: Record<string, readonly MuonBuildTarget[]> = {
  linux: ["linux-amd64", "linux-armhf", "linux-arm64"],
  windows: ["windows-i686", "windows-amd64"],
  amd64: ["linux-amd64", "windows-amd64"],
  arm64: ["linux-arm64"],
  armhf: ["linux-armhf"],
  i686: ["windows-i686"],
};

const normalizePackTargetSelector = (
  selector: string,
): readonly MuonBuildTarget[] => {
  const normalized = selector.trim().toLowerCase();
  if (allMuonTargets.includes(normalized as MuonBuildTarget)) {
    return [normalized as MuonBuildTarget];
  }
  const targets = packTargetSelectorTargets[normalized];
  if (targets !== undefined) {
    return targets;
  }
  throw new Error(`Unsupported muon pack target selector: ${selector}`);
};

const normalizePackTargetSelectors = (
  selectors: readonly string[],
): MuonBuildTarget[] => {
  const targets = selectors
    .flatMap((selector) => selector.split(","))
    .map((selector) => selector.trim())
    .filter((selector) => selector.length > 0)
    .flatMap((selector) => normalizePackTargetSelector(selector));
  return [...new Set(targets)];
};

const normalizePluginBuildTargets = (
  targets: readonly string[],
): MuonBuildTarget[] => {
  return [
    ...new Set(
      targets.map((target) => normalizeMuonTarget(target, "muon pack target")),
    ),
  ];
};

const resolvePackTargetCandidates = (
  options: MuonPackOptions,
  pluginBuildOptions: MuonViteBuildOptions,
): MuonBuildTarget[] => {
  if (options.allTargets === true) {
    return [...allMuonTargets];
  }
  if (options.targets !== undefined && options.targets.length > 0) {
    return normalizePackTargetSelectors(options.targets);
  }
  if (options.allTargets === false) {
    return [getDefaultMuonBuildTarget()];
  }
  if (pluginBuildOptions.allTargets === true) {
    return [...allMuonTargets];
  }
  if (
    pluginBuildOptions.targets !== undefined &&
    pluginBuildOptions.targets.length > 0
  ) {
    return normalizePluginBuildTargets(pluginBuildOptions.targets);
  }
  if (pluginBuildOptions.allTargets === false) {
    return [getDefaultMuonBuildTarget()];
  }
  return [...allMuonTargets];
};

const packTypeSupportsTarget = (
  type: MuonPackType,
  target: MuonBuildTarget,
): boolean => {
  const descriptor = getMuonTargetDescriptor(target);
  return (
    (type === "zip" && descriptor.os === "windows") ||
    (type === "tar.gz" && descriptor.os === "linux") ||
    (type === "deb" && descriptor.os === "linux") ||
    (type === "nsis" && descriptor.os === "windows")
  );
};

const isPortablePackType = (type: MuonPackType): boolean =>
  type === "zip" || type === "tar.gz";

const createPackTargetPlan = (
  types: readonly MuonPackType[],
  targets: readonly MuonBuildTarget[],
): MuonPackTargetPlan[] => {
  const plan = targets
    .map((target) => ({
      target,
      types: types.filter((type) => packTypeSupportsTarget(type, target)),
    }))
    .filter((entry) => entry.types.length > 0);
  if (plan.length === 0) {
    throw new Error("No valid muon pack target and type combinations.");
  }
  return plan;
};

const validateLinuxSandboxModeForPlan = (
  mode: MuonLinuxSandboxMode,
  plan: readonly MuonPackTargetPlan[],
): void => {
  if (mode === "disabled") {
    return;
  }
  for (const entry of plan) {
    const descriptor = getMuonTargetDescriptor(entry.target);
    if (
      descriptor.os !== "linux" ||
      entry.types.length !== 1 ||
      entry.types[0] !== "deb"
    ) {
      throw new Error(
        "--linux-sandbox=setuid is supported only for Linux deb packages. Specify --type deb with Linux targets.",
      );
    }
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

const isUnavailableToolError = (
  error: unknown,
  executable: string,
): boolean => {
  if (!(error instanceof Error)) {
    return false;
  }
  const nodeError = error as NodeJS.ErrnoException;
  return (
    nodeError.code === "ENOENT" &&
    (nodeError.syscall === `spawn ${executable}` ||
      nodeError.path === executable)
  );
};

const reportSkippedUnavailableTool = (
  progress: MuonProgressCallback | undefined,
  executable: string,
  type: MuonPackType,
  target: MuonBuildTarget,
): void => {
  progress?.({
    phase: "pack",
    status: `Warning: ${executable} is not available; skipping ${type} package for ${target}.`,
  });
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

const getPortableArchiveEntryRoot = (
  target: MuonBuildTargetResult,
  metadata: PackageMetadata,
): string => `${metadata.packageName}/${target.target}`;

const packageZip = async (
  target: MuonBuildTargetResult,
  metadata: PackageMetadata,
  artifactsRoot: string,
): Promise<MuonPackArtifact> => {
  const zip = new AdmZip();
  await addDirectoryToZip(
    zip,
    target.outputPath,
    getPortableArchiveEntryRoot(target, metadata),
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

const toArchivePath = (path: string): string => path.split(sep).join("/");

const createTarGzEntryGenerator = async function* (
  directory: string,
  entryRoot: string,
): AsyncGenerator<EntryItem, void, unknown> {
  yield await createDirectoryItem(entryRoot, "exceptName", {
    directoryPath: directory,
  });

  const walk = async function* (
    currentDirectory: string,
  ): AsyncGenerator<EntryItem, void, unknown> {
    const entries = await readdir(currentDirectory, { withFileTypes: true });
    entries.sort((left, right) => left.name.localeCompare(right.name));
    for (const entry of entries) {
      const path = join(currentDirectory, entry.name);
      const relativePath = toArchivePath(relative(directory, path));
      const entryName = `${entryRoot}/${relativePath}`;
      if (entry.isDirectory()) {
        yield await createDirectoryItem(entryName, "exceptName", {
          directoryPath: path,
        });
        yield* walk(path);
      } else if (entry.isFile()) {
        yield await createReadFileItem(entryName, path, "exceptName");
      }
    }
  };

  yield* walk(directory);
};

const packageTarGz = async (
  target: MuonBuildTargetResult,
  metadata: PackageMetadata,
  artifactsRoot: string,
): Promise<MuonPackArtifact> => {
  const outputPath = join(
    artifactsRoot,
    `${metadata.packageName}-${metadata.version}-${target.target}.tar.gz`,
  );
  await mkdir(dirname(outputPath), { recursive: true });
  const packer = createTarPacker(
    createTarGzEntryGenerator(
      target.outputPath,
      getPortableArchiveEntryRoot(target, metadata),
    ),
    "gzip",
  );
  await storeReaderToFile(packer, outputPath);
  return {
    type: "tar.gz",
    target: target.target,
    path: outputPath,
  };
};

const writePortableInstallMetadata = async (
  target: MuonBuildTargetResult,
): Promise<void> => {
  await writeFile(
    join(target.outputPath, "muon-install.json"),
    `${JSON.stringify(portableInstallMetadata, undefined, 2)}\n`,
  );
};

const packageDeb = async (
  root: string,
  target: MuonBuildTargetResult,
  metadata: PackageMetadata,
  artifactsRoot: string,
  packageBuildRoot: string,
  environment: NodeJS.ProcessEnv,
  linuxSandbox: MuonLinuxSandboxMode,
  progress: MuonProgressCallback | undefined,
): Promise<MuonPackArtifact | undefined> => {
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
  const privilegedPreparePath = `/usr/lib/${metadata.packageName}/${target.distributionDirectoryName}/${runtimeHelperExecutableName}`;
  const systemRuntimePath = `${systemRuntimeRoot}/${metadata.packageName}/${target.target}/runtime`;
  await mkdir(installedDist, { recursive: true });
  await cp(target.outputPath, installedDist, { recursive: true });
  if (linuxSandbox === "setuid") {
    if (target.runtimeHelperPath === undefined) {
      throw new Error(
        `muon runtime helper is unavailable for setuid deb target: ${target.target}`,
      );
    }
    await chmod(join(installedDist, runtimeHelperExecutableName), 0o4755);
  }
  await writeFile(
    join(installedDist, "muon-install.json"),
    `${JSON.stringify(
      {
        type: "deb",
        packageName: metadata.packageName,
        launcherPath: `/usr/bin/${metadata.packageName}`,
        ...(linuxSandbox === "setuid"
          ? {
              runtimeMode: "system-setuid",
              systemRuntimePath,
              privilegedPreparePath,
            }
          : {}),
      },
      undefined,
      2,
    )}\n`,
  );
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
  if (target.linuxDesktop === undefined) {
    throw new Error(`Linux desktop metadata is unavailable: ${target.target}`);
  }
  const applicationsPath = join(
    packageRoot,
    "usr",
    "share",
    "applications",
    `${target.linuxDesktop.desktopId}.desktop`,
  );
  await mkdir(dirname(applicationsPath), { recursive: true });
  await writeFile(
    applicationsPath,
    createLinuxDesktopEntry({
      desktop: target.linuxDesktop,
      exec: `${quoteDesktopExecArgument(`/usr/bin/${metadata.packageName}`)} --muon-launch-from=normal`,
      tryExec: `/usr/bin/${metadata.packageName}`,
      icon: target.linuxDesktop.desktopId,
    }),
  );
  const iconPath = join(
    packageRoot,
    "usr",
    "share",
    "icons",
    "hicolor",
    "256x256",
    "apps",
    `${target.linuxDesktop.desktopId}.png`,
  );
  await mkdir(dirname(iconPath), { recursive: true });
  await cp(join(target.outputPath, target.linuxDesktop.iconFileName), iconPath);
  const controlPath = join(packageRoot, "DEBIAN", "control");
  await mkdir(dirname(controlPath), { recursive: true });
  await writeFile(
    controlPath,
    [
      `Package: ${metadata.packageName}`,
      `Version: ${metadata.version}`,
      `Architecture: ${architecture}`,
      `Maintainer: ${metadata.author}`,
      `Depends: ${debRuntimeDependencies.join(", ")}`,
      `Description: ${metadata.description}`,
      "",
    ].join("\n"),
  );
  if (linuxSandbox === "setuid") {
    const postinstPath = join(packageRoot, "DEBIAN", "postinst");
    await writeFile(
      postinstPath,
      [
        "#!/bin/sh",
        "set -e",
        `helper=${JSON.stringify(privilegedPreparePath)}`,
        'if [ ! -f "$helper" ]; then',
        '  echo "muon-runtime-helper is missing: $helper" >&2',
        "  exit 1",
        "fi",
        'chown root:root "$helper"',
        'chmod 4755 "$helper"',
        "exit 0",
        "",
      ].join("\n"),
    );
    await chmod(postinstPath, 0o755);

    const postrmPath = join(packageRoot, "DEBIAN", "postrm");
    await writeFile(
      postrmPath,
      [
        "#!/bin/sh",
        "set -e",
        'if [ "$1" = "purge" ]; then',
        `  rm -rf ${JSON.stringify(`${systemRuntimeRoot}/${metadata.packageName}`)}`,
        `  # Shared CEF cache ${systemCefCacheRoot} is intentionally preserved.`,
        "fi",
        "exit 0",
        "",
      ].join("\n"),
    );
    await chmod(postrmPath, 0o755);
  }
  const outputPath = join(
    artifactsRoot,
    `${metadata.packageName}-${metadata.version}-${architecture}.deb`,
  );
  await mkdir(dirname(outputPath), { recursive: true });
  progress?.({
    phase: "pack",
    status: "Running dpkg-deb",
  });
  try {
    await runTool(
      "dpkg-deb",
      ["--root-owner-group", "--build", packageRoot, outputPath],
      root,
      environment,
    );
  } catch (error) {
    if (isUnavailableToolError(error, "dpkg-deb")) {
      reportSkippedUnavailableTool(progress, "dpkg-deb", "deb", target.target);
      return undefined;
    }
    throw error;
  }
  return {
    type: "deb",
    target: target.target,
    path: outputPath,
  };
};

const escapeNsis = (value: string): string =>
  value.replaceAll("\\", "\\\\").replaceAll('"', '$\\"');

const nsisUninstallRegistryRoot =
  "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall";

const createNsisUninstallRegistryKey = (appId: string): string =>
  `${nsisUninstallRegistryRoot}\\${appId}`;

const packageNsis = async (
  root: string,
  target: MuonBuildTargetResult,
  metadata: PackageMetadata,
  windowsResource: ResolvedMuonWindowsResource,
  windowsCodeSigning: ResolvedMuonWindowsCodeSigning | undefined,
  artifactsRoot: string,
  packageBuildRoot: string,
  environment: NodeJS.ProcessEnv,
  progress: MuonProgressCallback | undefined,
): Promise<MuonPackArtifact | undefined> => {
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
  const launcherFileName = basename(target.launcherPath);
  const nsisDisplayName = `${metadata.packageName} (${descriptor.arch})`;
  const nsisInstallDirectoryName = `${metadata.packageName}-${descriptor.arch}`;
  const uninstallRegistryKey = createNsisUninstallRegistryKey(
    target.runtimeAppId,
  );
  await mkdir(dirname(scriptPath), { recursive: true });
  await mkdir(dirname(outputPath), { recursive: true });
  const iconPath = join(
    dirname(scriptPath),
    `${metadata.packageName}-${target.target}.ico`,
  );
  await createWindowsIconFromPngFile(windowsResource.iconPath, iconPath);
  const codeSigningDirectives = await createNsisCodeSigningDirectives({
    root,
    scriptPath,
    target: target.target,
    codeSigning: windowsCodeSigning,
  });
  await writeFile(
    scriptPath,
    [
      "Unicode true",
      `Name "${escapeNsis(nsisDisplayName)}"`,
      `OutFile "${escapeNsis(outputPath)}"`,
      ...codeSigningDirectives,
      `InstallDir "$LOCALAPPDATA\\Programs\\${escapeNsis(nsisInstallDirectoryName)}"`,
      "RequestExecutionLevel user",
      "ShowInstDetails nevershow",
      "AutoCloseWindow true",
      ...createNsisResourceDirectives(windowsResource, iconPath),
      "Page instfiles",
      "Section",
      '  SetOutPath "$INSTDIR"',
      `  File /r "${escapeNsis(target.outputPath)}\\*"`,
      `  CreateShortCut "$SMPROGRAMS\\${escapeNsis(nsisDisplayName)}.lnk" "$INSTDIR\\${escapeNsis(launcherFileName)}"`,
      '  WriteUninstaller "$INSTDIR\\Uninstall.exe"',
      `  WriteRegStr HKCU "${escapeNsis(uninstallRegistryKey)}" "DisplayName" "${escapeNsis(nsisDisplayName)}"`,
      `  WriteRegStr HKCU "${escapeNsis(uninstallRegistryKey)}" "DisplayVersion" "${escapeNsis(metadata.version)}"`,
      `  WriteRegStr HKCU "${escapeNsis(uninstallRegistryKey)}" "Publisher" "${escapeNsis(metadata.author)}"`,
      `  WriteRegStr HKCU "${escapeNsis(uninstallRegistryKey)}" "InstallLocation" "$INSTDIR"`,
      `  WriteRegStr HKCU "${escapeNsis(uninstallRegistryKey)}" "DisplayIcon" "$\\"$INSTDIR\\${escapeNsis(launcherFileName)}$\\""`,
      `  WriteRegStr HKCU "${escapeNsis(uninstallRegistryKey)}" "UninstallString" "$\\"$INSTDIR\\Uninstall.exe$\\" /S"`,
      `  WriteRegStr HKCU "${escapeNsis(uninstallRegistryKey)}" "QuietUninstallString" "$\\"$INSTDIR\\Uninstall.exe$\\" /S"`,
      `  WriteRegDWORD HKCU "${escapeNsis(uninstallRegistryKey)}" "NoModify" 1`,
      `  WriteRegDWORD HKCU "${escapeNsis(uninstallRegistryKey)}" "NoRepair" 1`,
      "SectionEnd",
      "",
      'Section "Uninstall"',
      `  Delete "$SMPROGRAMS\\${escapeNsis(nsisDisplayName)}.lnk"`,
      `  DeleteRegKey HKCU "${escapeNsis(uninstallRegistryKey)}"`,
      '  RMDir /r "$INSTDIR"',
      `  RMDir /r "$LOCALAPPDATA\\${escapeNsis(target.runtimeAppId)}"`,
      "SectionEnd",
      "",
      "Function .onInstSuccess",
      "  IfSilent +3",
      '  SetOutPath "$INSTDIR"',
      `  Exec "$\\"$INSTDIR\\${escapeNsis(launcherFileName)}$\\""`,
      "FunctionEnd",
      "",
    ].join("\n"),
  );
  progress?.({
    phase: "pack",
    status: "Running makensis",
  });
  try {
    await runTool("makensis", [scriptPath], root, environment);
  } catch (error) {
    if (isUnavailableToolError(error, "makensis")) {
      reportSkippedUnavailableTool(progress, "makensis", "nsis", target.target);
      return undefined;
    }
    throw error;
  }
  if ((await statOrUndefined(outputPath)) === undefined) {
    throw new Error(`makensis did not create installer: ${outputPath}`);
  }
  return {
    type: "nsis",
    target: target.target,
    path: outputPath,
  };
};

const createNsisResourceDirectives = (
  resource: ResolvedMuonWindowsResource,
  iconPath: string,
): string[] => {
  const lines: string[] = [];
  lines.push(`Icon "${escapeNsis(iconPath)}"`);
  lines.push(`UninstallIcon "${escapeNsis(iconPath)}"`);
  lines.push(`VIProductVersion "${escapeNsis(resource.fixedVersion)}"`);
  lines.push(`VIFileVersion "${escapeNsis(resource.fixedVersion)}"`);
  lines.push(
    `VIAddVersionKey /LANG=${resource.language} "CompanyName" "${escapeNsis(resource.companyName)}"`,
  );
  lines.push(
    `VIAddVersionKey /LANG=${resource.language} "FileDescription" "${escapeNsis(resource.fileDescription)}"`,
  );
  lines.push(
    `VIAddVersionKey /LANG=${resource.language} "FileVersion" "${escapeNsis(resource.version)}"`,
  );
  lines.push(
    `VIAddVersionKey /LANG=${resource.language} "ProductName" "${escapeNsis(resource.productName)}"`,
  );
  lines.push(
    `VIAddVersionKey /LANG=${resource.language} "ProductVersion" "${escapeNsis(resource.version)}"`,
  );
  if (resource.copyright !== undefined) {
    lines.push(
      `VIAddVersionKey /LANG=${resource.language} "LegalCopyright" "${escapeNsis(resource.copyright)}"`,
    );
  }
  return lines;
};

const createNsisCodeSigningWrapperScript = (configPath: string): string =>
  [
    "#!/usr/bin/env node",
    "const { readFileSync } = require('node:fs');",
    "const { spawnSync } = require('node:child_process');",
    `const config = JSON.parse(readFileSync(${JSON.stringify(configPath)}, 'utf8'));`,
    "const [kind, target, path] = process.argv.slice(2);",
    "const args = config.args.map((arg) => arg.replaceAll('{path}', path).replaceAll('{target}', target).replaceAll('{kind}', kind));",
    "const result = spawnSync(config.command, args, { cwd: config.cwd, env: process.env, stdio: 'inherit' });",
    "if (result.error) {",
    "  console.error(result.error.message);",
    "  process.exit(1);",
    "}",
    "process.exit(result.status ?? 1);",
    "",
  ].join("\n");

const createNsisCodeSigningDirectiveCommand = (input: {
  wrapperPath: string;
  kind: MuonWindowsCodeSigningTarget;
  target: MuonBuildTarget;
}): string =>
  `"${escapeNsis(process.execPath)}" "${escapeNsis(input.wrapperPath)}" "${input.kind}" "${input.target}" "%1"`;

const createNsisCodeSigningDirectives = async (input: {
  root: string;
  scriptPath: string;
  target: MuonBuildTarget;
  codeSigning: ResolvedMuonWindowsCodeSigning | undefined;
}): Promise<string[]> => {
  const codeSigning = input.codeSigning;
  if (codeSigning === undefined) {
    return [];
  }
  const signsInstaller = includesWindowsCodeSigningTarget(
    codeSigning,
    "nsisInstaller",
  );
  const signsUninstaller = includesWindowsCodeSigningTarget(
    codeSigning,
    "nsisUninstaller",
  );
  if (!signsInstaller && !signsUninstaller) {
    return [];
  }

  const scriptDirectory = dirname(input.scriptPath);
  const configPath = join(
    scriptDirectory,
    `${basename(input.scriptPath)}.sign.json`,
  );
  const wrapperPath = join(
    scriptDirectory,
    `${basename(input.scriptPath)}.sign.cjs`,
  );
  await writeFile(
    configPath,
    `${JSON.stringify(
      {
        command: codeSigning.command,
        args: codeSigning.args,
        cwd: input.root,
      },
      undefined,
      2,
    )}\n`,
  );
  await writeFile(wrapperPath, createNsisCodeSigningWrapperScript(configPath));
  await chmod(wrapperPath, 0o755);

  const lines: string[] = [];
  if (signsInstaller) {
    lines.push(
      `!finalize '${createNsisCodeSigningDirectiveCommand({
        wrapperPath,
        kind: "nsisInstaller",
        target: input.target,
      })}' = 0`,
    );
  }
  if (signsUninstaller) {
    lines.push(
      `!uninstfinalize '${createNsisCodeSigningDirectiveCommand({
        wrapperPath,
        kind: "nsisUninstaller",
        target: input.target,
      })}' = 0`,
    );
  }
  return lines;
};

const reapplyPackWindowsResources = async (
  targets: readonly MuonBuildTargetResult[],
  resource: ResolvedMuonWindowsResource,
  root: string,
  environment: NodeJS.ProcessEnv,
): Promise<void> => {
  for (const target of targets) {
    const descriptor = getMuonTargetDescriptor(target.target);
    if (descriptor.os === "windows") {
      await updateWindowsPeIconResource({
        executablePath: join(
          target.outputPath,
          descriptor.runtimeExecutableName,
        ),
        resource,
        environment,
        cwd: root,
      });
      await updateWindowsPeResources({
        executablePath: target.launcherPath,
        resource,
        environment,
        cwd: root,
      });
    }
  }
};

const signPackWindowsExecutables = async (
  targets: readonly MuonBuildTargetResult[],
  codeSigning: ResolvedMuonWindowsCodeSigning | undefined,
  root: string,
  environment: NodeJS.ProcessEnv,
  progress: MuonProgressCallback | undefined,
): Promise<void> => {
  if (codeSigning === undefined) {
    return;
  }
  progress?.({
    phase: "pack",
    status: "Signing Windows executables",
  });
  for (const target of targets) {
    const descriptor = getMuonTargetDescriptor(target.target);
    if (descriptor.os !== "windows") {
      continue;
    }
    await signWindowsExecutable({
      codeSigning,
      kind: "runtime",
      target: target.target,
      path: join(target.outputPath, descriptor.runtimeExecutableName),
      cwd: root,
      environment,
    });
    await signWindowsExecutable({
      codeSigning,
      kind: "launcher",
      target: target.target,
      path: target.launcherPath,
      cwd: root,
      environment,
    });
  }
};

const createPortableBuildOptions = (input: {
  project: MuonBuildSequenceProject;
  pluginBuildOptions: MuonViteBuildOptions;
  options: MuonPackOptions;
  outputRoot: string;
  targets: readonly MuonBuildTarget[];
  windowsResourceOptions: MuonWindowsResourceOptions | undefined;
  windowsCodeSigningOptions: false | MuonWindowsCodeSigningOptions | undefined;
  linuxDesktopOptions: MuonLinuxDesktopOptions | undefined;
  iconPath: string | undefined;
  environment: NodeJS.ProcessEnv;
  progress: MuonProgressCallback | undefined;
}): MuonBuildOptions => {
  const buildOptions: MuonBuildOptions = {
    root: input.project.root,
    targets: input.targets,
    allTargets: false,
    outputRoot: input.outputRoot,
  };

  if (input.project.pluginOptions !== undefined) {
    if (input.project.viteOutputDirectory === undefined) {
      throw new Error("Vite output directory could not be resolved.");
    }
    Object.assign(buildOptions, input.pluginBuildOptions);
    const packagedAssetOptions = createVitePackagedAssetOptions(
      input.project.viteBase ?? "/",
    );
    buildOptions.assetSourcePath = input.project.viteOutputDirectory;
    buildOptions.assetPrefix = packagedAssetOptions.assetPrefix;
    if (packagedAssetOptions.browserStartPage !== undefined) {
      buildOptions.browserStartPage = packagedAssetOptions.browserStartPage;
    }
  }

  buildOptions.targets = input.targets;
  buildOptions.allTargets = false;
  buildOptions.outputRoot = input.outputRoot;
  if (input.options.configPath !== undefined) {
    buildOptions.configPath = input.options.configPath;
  }
  if (input.iconPath !== undefined) {
    buildOptions.iconPath = input.iconPath;
  }
  if (input.options.appName !== undefined) {
    buildOptions.appName = input.options.appName;
  }
  if (input.options.appId !== undefined) {
    buildOptions.appId = input.options.appId;
  }
  if (input.options.packageDirectory !== undefined) {
    buildOptions.packageDirectory = input.options.packageDirectory;
  }
  if (input.windowsResourceOptions !== undefined) {
    buildOptions.windowsResource = input.windowsResourceOptions;
  }
  if (input.windowsCodeSigningOptions !== undefined) {
    buildOptions.windowsCodeSigning = input.windowsCodeSigningOptions;
  }
  if (input.linuxDesktopOptions !== undefined) {
    buildOptions.linuxDesktop = input.linuxDesktopOptions;
  }
  if (input.progress !== undefined) {
    (buildOptions as InternalMuonBuildOptions).progress = input.progress;
  }
  (buildOptions as InternalMuonBuildOptions).environment = input.environment;
  (buildOptions as InternalMuonBuildOptions).browserProfilePathOverride =
    portableProfilePath;
  return buildOptions;
};

const buildPortableTargets = async (input: {
  project: MuonBuildSequenceProject;
  pluginBuildOptions: MuonViteBuildOptions;
  options: MuonPackOptions;
  packageBuildRoot: string;
  targets: readonly MuonBuildTarget[];
  windowsResourceOptions: MuonWindowsResourceOptions | undefined;
  windowsCodeSigningOptions: false | MuonWindowsCodeSigningOptions | undefined;
  linuxDesktopOptions: MuonLinuxDesktopOptions | undefined;
  iconPath: string | undefined;
  environment: NodeJS.ProcessEnv;
  progress: MuonProgressCallback | undefined;
}): Promise<Map<MuonBuildTarget, MuonBuildTargetResult>> => {
  if (input.targets.length === 0) {
    return new Map();
  }
  input.progress?.({
    phase: "pack",
    status: "Building portable distributions",
  });
  const build = await buildMuonApp(
    createPortableBuildOptions({
      project: input.project,
      pluginBuildOptions: input.pluginBuildOptions,
      options: input.options,
      outputRoot: join(input.packageBuildRoot, "portable-build"),
      targets: input.targets,
      windowsResourceOptions: input.windowsResourceOptions,
      windowsCodeSigningOptions: input.windowsCodeSigningOptions,
      linuxDesktopOptions: input.linuxDesktopOptions,
      iconPath: input.iconPath,
      environment: input.environment,
      progress: input.progress,
    }),
  );
  await Promise.all(build.targets.map(writePortableInstallMetadata));
  return new Map(build.targets.map((target) => [target.target, target]));
};

/**
 * Runs the muon build sequence and creates redistributable packages.
 *
 * @param options Pack options.
 * @returns Generated package artifacts.
 */
export const packMuonApp = async (
  options: MuonPackOptions,
): Promise<MuonPackResult> => {
  const progress = (options as InternalMuonPackOptions).progress;
  const cwd = resolve(options.root ?? process.cwd());
  const environment = options.environment ?? process.env;
  const project = await loadMuonBuildSequenceProject(cwd);
  const root = project.root;
  const packageJson = await readPackageJson(root);
  const metadata = resolveMetadata(packageJson, options);
  const artifactsRoot = resolve(
    root,
    options.artifactsDir ?? defaultArtifactsDirectory,
  );
  const packageBuildRoot = resolve(root, defaultPackageBuildDirectory);
  const types = normalizePackTypes(options.types);
  const linuxSandbox = normalizeLinuxSandboxMode(options.linuxSandbox);
  const pluginBuildOptions = resolveMuonViteBuildOptions(project.pluginOptions);
  const targetPlan = createPackTargetPlan(
    types,
    resolvePackTargetCandidates(options, pluginBuildOptions),
  );
  validateLinuxSandboxModeForPlan(linuxSandbox, targetPlan);
  const buildOptions: MuonBuildSequenceOptions = {
    root: cwd,
    targets: targetPlan.map((entry) => entry.target),
    allTargets: false,
    includeRuntimeHelper: linuxSandbox === "setuid",
  };
  const windowsResourceOptions = mergeMuonWindowsResourceOptions(
    options.windowsResource,
    pluginBuildOptions.windowsResource,
  );
  const windowsCodeSigningOptions =
    options.windowsCodeSigning !== undefined
      ? options.windowsCodeSigning
      : pluginBuildOptions.windowsCodeSigning;
  const linuxDesktopOptions = mergeMuonLinuxDesktopOptions(
    options.linuxDesktop,
    pluginBuildOptions.linuxDesktop,
  );
  const iconPath = options.iconPath ?? pluginBuildOptions.iconPath;
  if (options.configPath !== undefined) {
    buildOptions.configPath = options.configPath;
  }
  if (iconPath !== undefined) {
    buildOptions.iconPath = iconPath;
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
  if (windowsResourceOptions !== undefined) {
    buildOptions.windowsResource = windowsResourceOptions;
  }
  if (windowsCodeSigningOptions !== undefined) {
    buildOptions.windowsCodeSigning = windowsCodeSigningOptions;
  }
  if (linuxDesktopOptions !== undefined) {
    buildOptions.linuxDesktop = linuxDesktopOptions;
  }
  if (progress !== undefined) {
    (buildOptions as InternalMuonBuildSequenceOptions).progress = progress;
  }
  (buildOptions as InternalMuonBuildSequenceOptions).environment = environment;
  const windowsResourceConfig = await readMuonConfigForWindowsResource(
    root,
    options.configPath ?? pluginBuildOptions.configPath,
  );
  const nodeProject = await resolveMuonNodeProject(
    windowsResourceConfig.config,
    windowsResourceConfig.directory,
  );
  await assertMuonNodeProjectPackCleanupIsSafe(nodeProject, packageBuildRoot);
  await assertMuonNodeProjectPackCleanupIsSafe(
    nodeProject,
    join(artifactsRoot, "deb"),
  );
  await assertMuonNodeProjectPackCleanupIsSafe(
    nodeProject,
    join(artifactsRoot, "nsis"),
  );
  const windowsResource = await resolveMuonWindowsResource({
    root,
    packageDirectory:
      options.packageDirectory ?? pluginBuildOptions.packageDirectory ?? "",
    packageJson: createPackMetadataPackageJson(packageJson, metadata),
    muonConfig: windowsResourceConfig.config,
    muonConfigDirectory: windowsResourceConfig.directory,
    options: windowsResourceOptions,
    appIconPath: iconPath,
    defaults: {
      productName: metadata.packageName,
      fileDescription: metadata.description,
      companyName: metadata.author,
      version: metadata.version,
      copyright: undefined,
    },
  });
  const windowsCodeSigning = resolveMuonWindowsCodeSigning({
    muonConfig: windowsResourceConfig.config,
    options: windowsCodeSigningOptions,
  });
  progress?.({
    phase: "pack",
    status: "Building distributions",
  });
  const build = await runMuonBuildSequence(buildOptions, project);
  if (options.packageVersion !== undefined) {
    await reapplyPackWindowsResources(
      build.targets,
      windowsResource,
      root,
      environment,
    );
    await signPackWindowsExecutables(
      build.targets,
      windowsCodeSigning,
      root,
      environment,
      progress,
    );
  }
  const typesByTarget = new Map(
    targetPlan.map((entry) => [entry.target, entry.types] as const),
  );
  const portableTargets = targetPlan
    .filter((entry) => entry.types.some(isPortablePackType))
    .map((entry) => entry.target);
  await rm(packageBuildRoot, { recursive: true, force: true });
  await rm(join(artifactsRoot, "deb"), { recursive: true, force: true });
  await rm(join(artifactsRoot, "nsis"), { recursive: true, force: true });
  await mkdir(artifactsRoot, { recursive: true });
  const portableTargetsByTarget = await buildPortableTargets({
    project,
    pluginBuildOptions,
    options,
    packageBuildRoot,
    targets: portableTargets,
    windowsResourceOptions,
    windowsCodeSigningOptions,
    linuxDesktopOptions,
    iconPath,
    environment,
    progress,
  });
  if (options.packageVersion !== undefined) {
    await reapplyPackWindowsResources(
      [...portableTargetsByTarget.values()],
      windowsResource,
      root,
      environment,
    );
    await signPackWindowsExecutables(
      [...portableTargetsByTarget.values()],
      windowsCodeSigning,
      root,
      environment,
      progress,
    );
  }
  const artifacts: MuonPackArtifact[] = [];
  const totalArtifacts = targetPlan.reduce(
    (sum, entry) => sum + entry.types.length,
    0,
  );
  let artifactIndex = 0;
  for (const target of build.targets) {
    for (const type of typesByTarget.get(target.target) ?? []) {
      artifactIndex += 1;
      progress?.({
        phase: "pack",
        status: `Packaging ${type} ${target.target} (${artifactIndex}/${totalArtifacts})`,
      });
      let artifact: MuonPackArtifact | undefined;
      if (type === "zip") {
        const portableTarget = portableTargetsByTarget.get(target.target);
        if (portableTarget === undefined) {
          throw new Error(`Portable target was not built: ${target.target}`);
        }
        artifact = await packageZip(portableTarget, metadata, artifactsRoot);
      } else if (type === "tar.gz") {
        const portableTarget = portableTargetsByTarget.get(target.target);
        if (portableTarget === undefined) {
          throw new Error(`Portable target was not built: ${target.target}`);
        }
        artifact = await packageTarGz(portableTarget, metadata, artifactsRoot);
      } else if (type === "deb") {
        artifact = await packageDeb(
          root,
          target,
          metadata,
          artifactsRoot,
          packageBuildRoot,
          environment,
          linuxSandbox,
          progress,
        );
      } else {
        artifact = await packageNsis(
          root,
          target,
          metadata,
          windowsResource,
          windowsCodeSigning,
          artifactsRoot,
          packageBuildRoot,
          environment,
          progress,
        );
      }
      if (artifact === undefined) {
        continue;
      }
      artifacts.push(artifact);
      progress?.({
        phase: "pack",
        status: `Wrote ${artifact.path}`,
      });
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
  muonBuildSequenceSuppressViteBuildEnvironmentKey;
