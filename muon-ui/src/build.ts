// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import { constants, type Stats } from "node:fs";
import {
  access,
  chmod,
  copyFile,
  mkdir,
  mkdtemp,
  readFile,
  readdir,
  rm,
  stat,
  writeFile,
} from "node:fs/promises";
import { createHash, randomBytes } from "node:crypto";
import { dirname, join, relative, resolve, sep } from "node:path";
import { tmpdir } from "node:os";
import { fileURLToPath } from "node:url";
import AdmZip from "adm-zip";
import { parse } from "json5";
import {
  embedMuonConfigInBootstrapFile,
  embedMuonConfigInRuntime,
} from "./embed-config.js";
import { getDefaultMuonPrepareTarget } from "./prepare.js";
import {
  allMuonTargets,
  getMuonTargetDescriptor,
  getMuonTargetRuntimeAppId,
  normalizeMuonTarget,
  type MuonTarget,
  type MuonTargetDescriptor,
} from "./targets.js";
import {
  resolveMuonWindowsResource,
  stripBuildOnlyWindowsResourceConfig,
  updateWindowsPeIconResource,
  updateWindowsPeResources,
  type MuonWindowsResourceOptions,
  type ResolvedMuonWindowsResource,
} from "./windows-resource.js";
import {
  resolveMuonLinuxDesktop,
  stripBuildOnlyLinuxDesktopConfig,
  writeLinuxDesktopDistributionFiles,
  type MuonLinuxDesktopOptions,
  type ResolvedMuonLinuxDesktop,
} from "./linux-desktop.js";
import { appIconAssetEntryName, appIconAssetUrl } from "./app-icon.js";
import type { MuonRuntimePluginConfig } from "./capability.js";

const defaultConfigFileNames = ["muon.json5", "muon.jsonc", "muon.json"];
const appConfigSourcePath = "./assets.zip";
const defaultAppName = "muon-app";
const defaultAppId = "muon-app";
const muonLicenseFileName = "CREDITS.md";
const directoryMode = 0o755;
const executableMode = 0o755;
const assetSaltByteLength = 16;
const moduleDirectory =
  typeof __dirname === "string"
    ? __dirname
    : dirname(fileURLToPath(import.meta.url));

type JsonObject = Record<string, unknown>;

type AssetInput = {
  sourcePath: string;
  prefix: string;
};

type BuildConfig = {
  config: JsonObject;
  directory: string;
};

type ZipEntry = {
  name: string;
  data: Buffer;
};

/**
 * Public Muon runtime target used by CLI, Vite options, and package layout.
 */
export type MuonBuildTarget = MuonTarget;

/**
 * Options for creating redistributable Muon app directories.
 */
export interface MuonBuildOptions {
  /**
   * Project root containing package.json, muon.json, and app assets.
   */
  root?: string;
  /**
   * Directory containing package runtime/ and native/ folders.
   *
   * @remarks This defaults to the installed muon package dist directory.
   */
  packageDirectory?: string;
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
   * Stable base application identifier used for portable runtime state.
   *
   * @remarks Windows target distributions embed `<appId>.<arch>` as their
   * runtime app identifier. Linux targets embed this value unchanged.
   */
  appId?: string;
  /**
   * Parent directory that receives dist-muon/linux-amd64/ style outputs.
   */
  outputRoot?: string;
  /**
   * Directory or ZIP file used as app assets.
   */
  assetSourcePath?: string;
  /**
   * Optional ZIP entry prefix used for the asset source.
   */
  assetPrefix?: string;
  /**
   * Default browser start page embedded when muon config omits one.
   */
  browserStartPage?: string;
  /**
   * Muon config path to embed.
   */
  configPath?: string;
  /**
   * Static application icon PNG file path.
   *
   * @remarks This icon is used as the shared source for Windows PE/NSIS,
   * Linux desktop entries, and the generated initial title bar icon asset.
   * Platform-specific icon paths override it for their target.
   */
  iconPath?: string;
  /**
   * Windows PE and NSIS resource metadata.
   *
   * @defaultValue Uses `muon.json` `windows.resource`, `project.json`,
   * `package.json`, then Muon defaults.
   */
  windowsResource?: MuonWindowsResourceOptions;
  /**
   * Linux desktop entry metadata.
   *
   * @defaultValue Uses `muon.json` `linux.desktop`, package metadata, then
   * Muon defaults.
   */
  linuxDesktop?: MuonLinuxDesktopOptions;
  /**
   * Asset salt override for deterministic tests.
   *
   * @remarks Production builds should omit this option.
   */
  assetSalt?: Uint8Array;
  /**
   * Runtime plugin configuration supplied by a bundler integration.
   *
   * @internal
   */
  runtimePluginConfig?: MuonRuntimePluginConfig;
}

/**
 * Asset metadata generated for a target distribution.
 */
export interface MuonBuildAssetResult {
  /**
   * Path of the generated assets.zip.
   */
  path: string;
  /**
   * SHA-1 signature for the generated asset archive and salt.
   */
  signature: string;
  /**
   * Hex encoded salt embedded into muon.json.
   */
  salt: string;
  /**
   * Number of files written to the ZIP archive.
   */
  entryCount: number;
}

/**
 * Result for one generated target distribution.
 */
export interface MuonBuildTargetResult {
  /**
   * Public target identifier used by the muon npm package.
   */
  target: MuonBuildTarget;
  /**
   * Fixed output directory path for the target.
   */
  distributionDirectoryName: string;
  /**
   * Absolute path of the generated target directory.
   */
  outputPath: string;
  /**
   * Absolute path of the app launcher copied from muon-bootstrap.
   */
  launcherPath: string;
  /**
   * Generated asset archive metadata.
   */
  asset: MuonBuildAssetResult;
  /**
   * Application identifier embedded into this target distribution.
   *
   * @remarks Windows targets append the target architecture to the base
   * `appId`; Linux targets use the base `appId` unchanged.
   */
  runtimeAppId: string;
  /**
   * Config object embedded into muon-core and the app launcher.
   */
  embeddedConfig: JsonObject;
  /**
   * Linux desktop integration metadata when the target is Linux.
   */
  linuxDesktop?: ResolvedMuonLinuxDesktop;
}

/**
 * Result of a Muon app build.
 */
export interface MuonBuildResult {
  /**
   * Absolute project root used for the build.
   */
  root: string;
  /**
   * Sanitized app launcher base name.
   */
  appName: string;
  /**
   * Stable base application identifier used to derive target runtime state.
   *
   * @remarks See each target's `runtimeAppId` for the identifier embedded into
   * that target distribution.
   */
  appId: string;
  /**
   * Generated target distributions.
   */
  targets: MuonBuildTargetResult[];
}

/**
 * Returns the host target used by muon build when no explicit target is passed.
 */
export const getDefaultMuonBuildTarget = (): MuonBuildTarget => {
  return getDefaultMuonPrepareTarget(process.platform, process.arch);
};

/**
 * Normalizes a user-facing public target identifier.
 */
export const normalizeMuonBuildTarget = (target: string): MuonBuildTarget => {
  return normalizeMuonTarget(target, "muon build target");
};

/**
 * Builds CEF-free Muon app distribution directories for one or more targets.
 */
export const buildMuonApp = async (
  options: MuonBuildOptions = {},
): Promise<MuonBuildResult> => {
  const root = resolve(options.root ?? process.cwd());
  const packageDirectory = resolvePackageDirectory(options.packageDirectory);
  const targets = resolveBuildTargets(options);
  const outputRoot = resolve(root, options.outputRoot ?? ".");
  const packageJson = await readPackageJson(root);
  const appName = resolveAppName(packageJson, options.appName);
  const appId = resolveAppId(packageJson, options.appId);
  const buildConfig = await readBuildConfig(root, options.configPath);
  const sourceConfig = applyRuntimePluginConfig(
    buildConfig.config,
    options.runtimePluginConfig ?? { mode: "simple" },
  );
  assertNoUserInitialTitleBarIcon(sourceConfig);
  const resolvedBuildConfig: BuildConfig = {
    ...buildConfig,
    config: sourceConfig,
  };
  const assetInput = resolveAssetInput(
    root,
    options.assetSourcePath,
    options.assetPrefix,
    resolvedBuildConfig,
  );
  const windowsResource = await resolveMuonWindowsResource({
    root,
    packageDirectory,
    packageJson,
    muonConfig: sourceConfig,
    muonConfigDirectory: buildConfig.directory,
    options: options.windowsResource,
    appIconPath: options.iconPath,
    defaults: {
      productName: appName,
      fileDescription: appName,
      companyName: "Unknown",
      version: "0.0.0",
      copyright: undefined,
    },
  });
  const linuxDesktop = await resolveMuonLinuxDesktop({
    root,
    packageDirectory,
    muonConfig: sourceConfig,
    muonConfigDirectory: buildConfig.directory,
    options: options.linuxDesktop,
    appIconPath: options.iconPath,
    defaults: {
      desktopId: appId,
      name: resolveLinuxDesktopDefaultName(packageJson, appName),
      comment: resolvePackageDescription(packageJson),
      categories: ["Utility"],
      startupNotify: true,
    },
  });
  const salt = Buffer.from(
    options.assetSalt ?? randomBytes(assetSaltByteLength),
  );

  const results: MuonBuildTargetResult[] = [];

  for (const target of targets) {
    const result = await buildMuonTarget({
      packageDirectory,
      root,
      outputRoot,
      appName,
      appId,
      target,
      assetInput,
      sourceConfig,
      windowsResource,
      linuxDesktop,
      salt,
      browserStartPage: options.browserStartPage,
    });
    results.push(result);
  }

  return {
    root,
    appName,
    appId,
    targets: results,
  };
};

const resolvePackageDirectory = (
  packageDirectory: string | undefined,
): string => {
  if (packageDirectory !== undefined) {
    return resolve(packageDirectory);
  }

  return moduleDirectory;
};

const resolveBuildTargets = (options: MuonBuildOptions): MuonBuildTarget[] => {
  if (options.allTargets === true) {
    return [...allMuonTargets];
  }

  if (options.targets !== undefined && options.targets.length > 0) {
    return [
      ...new Set(
        options.targets.map((target) => normalizeMuonBuildTarget(target)),
      ),
    ];
  }

  if (options.allTargets !== false) {
    return [...allMuonTargets];
  }

  return [getDefaultMuonBuildTarget()];
};

const resolveAssetInput = (
  root: string,
  assetSourcePath: string | undefined,
  assetPrefix: string | undefined,
  buildConfig: BuildConfig,
): AssetInput => {
  const configuredAssetSourcePath =
    assetSourcePath === undefined
      ? readConfigAssetSourcePath(buildConfig.config)
      : undefined;
  const sourcePath =
    assetSourcePath !== undefined
      ? resolve(root, assetSourcePath)
      : configuredAssetSourcePath !== undefined
        ? resolve(buildConfig.directory, configuredAssetSourcePath)
        : resolve(root, "assets");
  return {
    sourcePath,
    prefix: normalizeZipPrefix(assetPrefix ?? ""),
  };
};

const normalizeZipPrefix = (prefix: string): string => {
  const normalized = prefix
    .replaceAll("\\", "/")
    .split("/")
    .filter((part) => part.length > 0)
    .join("/");

  return normalized.length > 0 ? `${normalized}/` : "";
};

const readPackageJson = async (root: string): Promise<JsonObject> => {
  const packageJsonPath = join(root, "package.json");
  if (!(await fileExists(packageJsonPath))) {
    return {};
  }
  return await readJsonObjectFile(packageJsonPath, "package.json");
};

const resolvePackageName = (packageJson: JsonObject): string => {
  return typeof packageJson.name === "string"
    ? packageJson.name
    : defaultAppName;
};

const resolveAppName = (
  packageJson: JsonObject,
  appName: string | undefined,
): string => {
  if (appName !== undefined) {
    return sanitizeAppName(appName);
  }

  const packageName = resolvePackageName(packageJson);
  const unscopedName = packageName.startsWith("@")
    ? packageName.slice(packageName.indexOf("/") + 1)
    : packageName;

  return sanitizeAppName(unscopedName);
};

const sanitizeAppName = (name: string): string => {
  const sanitized = name
    .trim()
    .toLowerCase()
    .replace(/[^a-z0-9._-]+/g, "-")
    .replace(/^[.-]+/g, "")
    .replace(/[.-]+$/g, "");

  return sanitized.length > 0 ? sanitized : defaultAppName;
};

const resolveAppId = (
  packageJson: JsonObject,
  appId: string | undefined,
): string => {
  if (appId !== undefined) {
    return sanitizeAppId(appId);
  }
  return sanitizeAppId(resolvePackageName(packageJson));
};

const sanitizeAppId = (value: string): string => {
  const unscoped = value.startsWith("@") ? value.slice(1) : value;
  const sanitized = unscoped
    .trim()
    .toLowerCase()
    .replace("/", ".")
    .replace(/[^a-z0-9._-]+/g, ".")
    .replace(/^[.]+/g, "")
    .replace(/[.]+$/g, "");
  return sanitized.length > 0 ? sanitized : defaultAppId;
};

const resolveLinuxDesktopDefaultName = (
  packageJson: JsonObject,
  appName: string,
): string =>
  typeof packageJson.name === "string" && packageJson.name.trim() !== ""
    ? packageJson.name.trim()
    : appName;

const resolvePackageDescription = (packageJson: JsonObject): string =>
  typeof packageJson.description === "string"
    ? packageJson.description.trim()
    : "";

const readBuildConfig = async (
  root: string,
  configPath: string | undefined,
): Promise<BuildConfig> => {
  const resolvedConfigPath = await resolveConfigPath(root, configPath);
  if (resolvedConfigPath === undefined) {
    return {
      config: {},
      directory: root,
    };
  }

  return {
    config: await readJsonObjectFile(resolvedConfigPath, "Muon config file"),
    directory: dirname(resolvedConfigPath),
  };
};

const applyRuntimePluginConfig = (
  sourceConfig: JsonObject,
  runtimePluginConfig: MuonRuntimePluginConfig | undefined,
): JsonObject => {
  if (runtimePluginConfig === undefined) {
    return sourceConfig;
  }

  const sourcePlugin = sourceConfig.plugin;
  if (sourcePlugin !== undefined && !isJsonObject(sourcePlugin)) {
    throw new Error(
      "muon.json plugin must be an object when runtime plugin config is applied.",
    );
  }
  const plugin: JsonObject = sourcePlugin ?? {};

  return {
    ...sourceConfig,
    plugin: {
      ...plugin,
      ...runtimePluginConfig,
    },
  };
};

const readConfigAssetSourcePath = (
  sourceConfig: JsonObject,
): string | undefined => {
  const sourceAsset = sourceConfig.asset;
  if (sourceAsset === undefined) {
    return undefined;
  }
  if (!isJsonObject(sourceAsset)) {
    throw new Error("muon.json asset must be an object when present.");
  }

  const sourceAssetPath = sourceAsset.sourcePath;
  if (sourceAssetPath === undefined) {
    return undefined;
  }
  if (typeof sourceAssetPath !== "string") {
    throw new Error(
      "muon.json asset.sourcePath must be a string when present.",
    );
  }

  return sourceAssetPath;
};

const resolveConfigPath = async (
  root: string,
  configPath: string | undefined,
): Promise<string | undefined> => {
  if (configPath !== undefined) {
    const resolvedPath = resolve(root, configPath);
    if (await fileExists(resolvedPath)) {
      return resolvedPath;
    }

    throw new Error(`Muon config file does not exist: ${resolvedPath}`);
  }

  for (const fileName of defaultConfigFileNames) {
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

const buildMuonTarget = async (input: {
  packageDirectory: string;
  root: string;
  outputRoot: string;
  appName: string;
  appId: string;
  target: MuonBuildTarget;
  assetInput: AssetInput;
  sourceConfig: JsonObject;
  windowsResource: ResolvedMuonWindowsResource;
  linuxDesktop: ResolvedMuonLinuxDesktop;
  salt: Buffer;
  browserStartPage: string | undefined;
}): Promise<MuonBuildTargetResult> => {
  const descriptor = getMuonTargetDescriptor(input.target);
  const sourceRuntimePath = join(
    input.packageDirectory,
    "runtime",
    input.target,
  );
  const sourceBootstrapPath = join(
    input.packageDirectory,
    "native",
    input.target,
    descriptor.bootstrapExecutableName,
  );
  const outputPath = join(
    input.outputRoot,
    descriptor.distributionDirectoryName,
  );
  const launcherPath = join(
    outputPath,
    getLauncherFileName(input.appName, descriptor),
  );
  const assetZipPath = join(outputPath, "assets.zip");
  const runtimeAppId = getMuonTargetRuntimeAppId(input.appId, input.target);
  const appIconPath =
    descriptor.os === "windows"
      ? input.windowsResource.iconPath
      : input.linuxDesktop.iconPath;

  await verifyTargetInputs({
    sourceRuntimePath,
    sourceBootstrapPath,
    descriptor,
    target: input.target,
  });

  await rm(outputPath, { recursive: true, force: true });
  await mkdir(outputPath, { recursive: true, mode: directoryMode });
  await copyRuntimeFiles(sourceRuntimePath, outputPath, descriptor);
  await chmod(
    join(outputPath, descriptor.runtimeExecutableName),
    executableMode,
  );
  await copyFile(sourceBootstrapPath, launcherPath);
  await chmod(launcherPath, executableMode);

  const asset = await writeAssetArchive(
    input.assetInput,
    assetZipPath,
    input.salt,
    [{ name: appIconAssetEntryName, data: await readFile(appIconPath) }],
  );
  const embeddedConfig = createEmbeddedConfig(
    input.sourceConfig,
    asset,
    runtimeAppId,
    input.linuxDesktop.desktopId,
    appIconAssetUrl,
    input.browserStartPage,
  );

  await withTemporaryConfig(embeddedConfig, async (configPath) => {
    await embedMuonConfigInRuntime({
      runtimePath: outputPath,
      configPath,
      outputRuntimePath: undefined,
    });
    await embedMuonConfigInBootstrapFile({
      bootstrapPath: launcherPath,
      configPath,
      outputPath: undefined,
    });
  });

  if (descriptor.os === "windows") {
    await updateWindowsPeIconResource({
      executablePath: join(outputPath, descriptor.runtimeExecutableName),
      resource: input.windowsResource,
      environment: process.env,
      cwd: input.root,
    });
    await updateWindowsPeResources({
      executablePath: launcherPath,
      resource: input.windowsResource,
      environment: process.env,
      cwd: input.root,
    });
  } else if (descriptor.os === "linux") {
    await writeLinuxDesktopDistributionFiles(outputPath, input.linuxDesktop);
  }

  return {
    target: input.target,
    distributionDirectoryName: descriptor.distributionDirectoryName,
    outputPath,
    launcherPath,
    asset,
    runtimeAppId,
    embeddedConfig,
    ...(descriptor.os === "linux" ? { linuxDesktop: input.linuxDesktop } : {}),
  };
};

const verifyTargetInputs = async (input: {
  sourceRuntimePath: string;
  sourceBootstrapPath: string;
  descriptor: MuonTargetDescriptor;
  target: MuonBuildTarget;
}): Promise<void> => {
  await assertDirectory(
    input.sourceRuntimePath,
    `Muon runtime for ${input.target}`,
  );
  await assertFile(
    input.sourceBootstrapPath,
    `Muon bootstrap for ${input.target}`,
  );
  for (const fileName of input.descriptor.runtimeFiles) {
    await assertFile(
      join(input.sourceRuntimePath, fileName),
      `Muon runtime file ${fileName} for ${input.target}`,
    );
  }
  await assertFile(
    join(input.sourceRuntimePath, muonLicenseFileName),
    `Muon license file for ${input.target}`,
  );
};

const getLauncherFileName = (
  appName: string,
  descriptor: MuonTargetDescriptor,
): string => {
  if (
    descriptor.launcherExtension.length > 0 &&
    !appName.endsWith(descriptor.launcherExtension)
  ) {
    return `${appName}${descriptor.launcherExtension}`;
  }

  return appName;
};

const copyRuntimeFiles = async (
  sourceRuntimePath: string,
  outputPath: string,
  descriptor: MuonTargetDescriptor,
): Promise<void> => {
  for (const fileName of descriptor.runtimeFiles) {
    await copyFile(
      join(sourceRuntimePath, fileName),
      join(outputPath, fileName),
    );
  }
  if (descriptor.optionalRuntimeFilePatterns !== undefined) {
    const fileNames = await readdir(sourceRuntimePath);
    for (const fileName of fileNames) {
      if (
        descriptor.optionalRuntimeFilePatterns.some((pattern) =>
          pattern.test(fileName),
        )
      ) {
        await copyFile(
          join(sourceRuntimePath, fileName),
          join(outputPath, fileName),
        );
      }
    }
  }
  await copyFile(
    join(sourceRuntimePath, muonLicenseFileName),
    join(outputPath, muonLicenseFileName),
  );
};

const writeAssetArchive = async (
  input: AssetInput,
  outputPath: string,
  salt: Buffer,
  extraEntries: readonly ZipEntry[],
): Promise<MuonBuildAssetResult> => {
  const sourceStats = await statOrUndefined(input.sourcePath);
  if (sourceStats === undefined) {
    throw new Error(`Muon asset source does not exist: ${input.sourcePath}`);
  }

  const archive = sourceStats.isDirectory()
    ? await createAssetArchiveFromDirectory(input, extraEntries)
    : sourceStats.isFile()
      ? await createAssetArchiveFromZipFile(input.sourcePath, extraEntries)
      : undefined;
  if (archive === undefined) {
    throw new Error(
      `Muon asset source is not a directory or file: ${input.sourcePath}`,
    );
  }
  await writeFile(outputPath, archive);

  const signature = createHash("sha1")
    .update(archive)
    .update(salt)
    .digest("hex");
  return {
    path: outputPath,
    signature,
    salt: salt.toString("hex"),
    entryCount: sourceStats.isDirectory()
      ? readZipEntryCount(archive, outputPath)
      : readZipEntryCount(archive, input.sourcePath),
  };
};

const createAssetArchiveFromDirectory = async (
  input: AssetInput,
  extraEntries: readonly ZipEntry[],
): Promise<Buffer> => {
  const entries = await collectZipEntries(input.sourcePath, input.prefix);
  if (entries.length === 0) {
    throw new Error(`Muon asset source has no files: ${input.sourcePath}`);
  }

  return createZipArchive(appendZipEntries(entries, extraEntries));
};

const createAssetArchiveFromZipFile = async (
  sourcePath: string,
  extraEntries: readonly ZipEntry[],
): Promise<Buffer> => {
  const zip = new AdmZip(await readFile(sourcePath));
  for (const entry of extraEntries) {
    assertSafeZipEntryName(entry.name);
    if (zip.getEntry(entry.name) !== null) {
      throw new Error(
        `Muon app icon asset entry already exists: ${entry.name}`,
      );
    }
    zip.addFile(entry.name, entry.data);
  }
  return zip.toBuffer();
};

const appendZipEntries = (
  entries: readonly ZipEntry[],
  extraEntries: readonly ZipEntry[],
): ZipEntry[] => {
  const output = [...entries];
  const names = new Set(entries.map((entry) => entry.name));
  for (const entry of extraEntries) {
    assertSafeZipEntryName(entry.name);
    if (names.has(entry.name)) {
      throw new Error(
        `Muon app icon asset entry already exists: ${entry.name}`,
      );
    }
    names.add(entry.name);
    output.push(entry);
  }
  return output;
};

const readZipEntryCount = (archive: Buffer, sourcePath: string): number => {
  const endSignature = 0x06054b50;
  const lastPossibleOffset = archive.length - 22;
  const firstPossibleOffset = Math.max(0, lastPossibleOffset - 0xffff);

  for (
    let offset = lastPossibleOffset;
    offset >= firstPossibleOffset;
    offset -= 1
  ) {
    if (archive.readUInt32LE(offset) === endSignature) {
      return archive.readUInt16LE(offset + 10);
    }
  }

  throw new Error(`Muon asset ZIP could not be read: ${sourcePath}`);
};

const collectZipEntries = async (
  sourcePath: string,
  prefix: string,
): Promise<ZipEntry[]> => {
  const entries: ZipEntry[] = [];

  const walk = async (directoryPath: string): Promise<void> => {
    const dirents = await readdir(directoryPath, { withFileTypes: true });
    dirents.sort((a, b) => a.name.localeCompare(b.name));

    for (const dirent of dirents) {
      const childPath = join(directoryPath, dirent.name);
      if (dirent.isDirectory()) {
        await walk(childPath);
      } else if (dirent.isFile()) {
        const relativePath = relative(sourcePath, childPath)
          .split(sep)
          .join("/");
        const name = `${prefix}${relativePath}`;
        assertSafeZipEntryName(name);
        entries.push({
          name,
          data: await readFile(childPath),
        });
      }
    }
  };

  await walk(sourcePath);
  return entries;
};

const assertSafeZipEntryName = (name: string): void => {
  if (
    name.length === 0 ||
    name.startsWith("/") ||
    name.includes("..") ||
    name.includes("\\")
  ) {
    throw new Error(`Unsafe ZIP entry name: ${name}`);
  }
};

const createZipArchive = (entries: readonly ZipEntry[]): Buffer => {
  const zip = new AdmZip();
  for (const entry of entries) {
    zip.addFile(entry.name, entry.data);
  }

  return zip.toBuffer();
};

const createEmbeddedConfig = (
  sourceConfig: JsonObject,
  asset: MuonBuildAssetResult,
  appId: string,
  desktopId: string,
  initialTitleBarIcon: string,
  browserStartPage: string | undefined,
): JsonObject => {
  const sourceAsset = sourceConfig.asset;
  if (sourceAsset !== undefined && !isJsonObject(sourceAsset)) {
    throw new Error("muon.json asset must be an object when present.");
  }
  const sourceBootstrap = sourceConfig.bootstrap;
  if (sourceBootstrap !== undefined && !isJsonObject(sourceBootstrap)) {
    throw new Error("muon.json bootstrap must be an object when present.");
  }

  const runtimeConfig = stripBuildOnlyAppIconConfig(
    stripBuildOnlyLinuxDesktopConfig(
      stripBuildOnlyWindowsResourceConfig(sourceConfig),
    ),
  );
  const sourceBrowser = runtimeConfig.browser;
  if (sourceBrowser !== undefined && !isJsonObject(sourceBrowser)) {
    throw new Error("muon.json browser must be an object when present.");
  }

  const browserConfig: JsonObject = {
    ...(sourceBrowser ?? {}),
    initialTitleBarIcon,
  };
  if (browserStartPage !== undefined && browserConfig.startPage === undefined) {
    browserConfig.startPage = browserStartPage;
  }

  return {
    ...runtimeConfig,
    browser: browserConfig,
    asset: {
      ...(sourceAsset ?? {}),
      sourcePath: appConfigSourcePath,
      signature: asset.signature,
      salt: asset.salt,
    },
    bootstrap: {
      ...(sourceBootstrap ?? {}),
      appId,
      desktopId,
    },
  };
};

const assertNoUserInitialTitleBarIcon = (sourceConfig: JsonObject): void => {
  const sourceBrowser = sourceConfig.browser;
  if (sourceBrowser === undefined) {
    return;
  }
  if (!isJsonObject(sourceBrowser)) {
    throw new Error("muon.json browser must be an object when present.");
  }
  if (sourceBrowser.initialTitleBarIcon !== undefined) {
    throw new Error(
      "muon.json browser.initialTitleBarIcon is generated by muon build; use top-level iconPath instead.",
    );
  }
};

const stripBuildOnlyAppIconConfig = (sourceConfig: JsonObject): JsonObject => {
  const output: JsonObject = {};
  for (const [key, value] of Object.entries(sourceConfig)) {
    if (key !== "iconPath") {
      output[key] = value;
    }
  }
  return output;
};

const withTemporaryConfig = async (
  config: JsonObject,
  callback: (configPath: string) => Promise<void>,
): Promise<void> => {
  const tempDirectory = await mkdtemp(join(tmpdir(), "muon-build-config-"));
  const configPath = join(tempDirectory, "muon.json");
  try {
    await writeFile(configPath, `${JSON.stringify(config, undefined, 2)}\n`);
    await callback(configPath);
  } finally {
    await rm(tempDirectory, { recursive: true, force: true });
  }
};

const assertDirectory = async (path: string, label: string): Promise<void> => {
  const stats = await statOrUndefined(path);
  if (stats === undefined || !stats.isDirectory()) {
    throw new Error(`${label} directory does not exist: ${path}`);
  }
};

const assertFile = async (path: string, label: string): Promise<void> => {
  const stats = await statOrUndefined(path);
  if (stats === undefined || !stats.isFile()) {
    throw new Error(`${label} file does not exist: ${path}`);
  }
};

const statOrUndefined = async (path: string): Promise<Stats | undefined> => {
  try {
    return await stat(path);
  } catch {
    return undefined;
  }
};

const fileExists = async (path: string): Promise<boolean> => {
  try {
    await access(path, constants.F_OK);
    return true;
  } catch {
    return false;
  }
};

const isJsonObject = (value: unknown): value is JsonObject => {
  return typeof value === "object" && value !== null && !Array.isArray(value);
};

const getErrorMessage = (error: unknown): string =>
  error instanceof Error ? error.message : String(error);
