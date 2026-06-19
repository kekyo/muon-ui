// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import { constants } from "node:fs";
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

const allTargets = [
  "linux64",
  "linuxarm",
  "linuxarm64",
  "windows32",
  "windows64",
] as const;

const targetAliases: Record<string, MuonBuildTarget> = {
  linux64: "linux64",
  "linux-amd64": "linux64",
  "linux-x64": "linux64",
  amd64: "linux64",
  x64: "linux64",
  linuxarm: "linuxarm",
  "linux-arm": "linuxarm",
  "linux-armv7l": "linuxarm",
  arm: "linuxarm",
  armv7l: "linuxarm",
  linuxarm64: "linuxarm64",
  "linux-arm64": "linuxarm64",
  "linux-aarch64": "linuxarm64",
  arm64: "linuxarm64",
  aarch64: "linuxarm64",
  windows32: "windows32",
  "windows-i686": "windows32",
  "windows-ia32": "windows32",
  win32: "windows32",
  i686: "windows32",
  ia32: "windows32",
  windows64: "windows64",
  "windows-amd64": "windows64",
  "windows-x64": "windows64",
  win64: "windows64",
};

const targetDescriptors: Record<MuonBuildTarget, MuonBuildTargetDescriptor> = {
  linux64: {
    distributionDirectoryName: "dist-linux-amd64",
    runtimeExecutableName: "muon-core",
    bootstrapExecutableName: "muon-bootstrap",
    launcherExtension: "",
    runtimeFiles: ["muon-core", "libmuon-ui.so", "libcardio.so"],
  },
  linuxarm: {
    distributionDirectoryName: "dist-linux-armv7l",
    runtimeExecutableName: "muon-core",
    bootstrapExecutableName: "muon-bootstrap",
    launcherExtension: "",
    runtimeFiles: ["muon-core", "libmuon-ui.so", "libcardio.so"],
  },
  linuxarm64: {
    distributionDirectoryName: "dist-linux-arm64",
    runtimeExecutableName: "muon-core",
    bootstrapExecutableName: "muon-bootstrap",
    launcherExtension: "",
    runtimeFiles: ["muon-core", "libmuon-ui.so", "libcardio.so"],
  },
  windows32: {
    distributionDirectoryName: "dist-windows-i686",
    runtimeExecutableName: "muon-core.exe",
    bootstrapExecutableName: "muon-bootstrap.exe",
    launcherExtension: ".exe",
    runtimeFiles: ["muon-core.exe", "libmuon-ui.dll", "libcardio.dll"],
  },
  windows64: {
    distributionDirectoryName: "dist-windows-amd64",
    runtimeExecutableName: "muon-core.exe",
    bootstrapExecutableName: "muon-bootstrap.exe",
    launcherExtension: ".exe",
    runtimeFiles: ["muon-core.exe", "libmuon-ui.dll", "libcardio.dll"],
  },
};

const defaultConfigFileNames = ["muon.json5", "muon.jsonc", "muon.json"];
const appConfigFromPath = "./assets.zip";
const defaultAppName = "muon-app";
const directoryMode = 0o755;
const executableMode = 0o755;
const assetSaltByteLength = 16;
const moduleDirectory =
  typeof __dirname === "string"
    ? __dirname
    : dirname(fileURLToPath(import.meta.url));

type JsonObject = Record<string, unknown>;

type MuonBuildTargetDescriptor = {
  distributionDirectoryName: string;
  runtimeExecutableName: string;
  bootstrapExecutableName: string;
  launcherExtension: string;
  runtimeFiles: readonly string[];
};

type AssetInput = {
  sourcePath: string;
  prefix: string;
};

type ZipEntry = {
  name: string;
  data: Buffer;
};

/**
 * Muon runtime target used by the npm package layout.
 */
export type MuonBuildTarget =
  | "linux64"
  | "linuxarm"
  | "linuxarm64"
  | "windows32"
  | "windows64";

/**
 * Options for creating redistributable Muon app directories.
 */
export interface MuonBuildOptions {
  /**
   * Project root containing package.json, muon.json, and assets/.
   */
  root?: string;
  /**
   * Directory containing package runtime/ and native/ folders.
   *
   * @remarks This defaults to the installed muon package dist directory.
   */
  packageDirectory?: string;
  /**
   * Target aliases or internal target names to build.
   */
  targets?: readonly string[];
  /**
   * Build every supported target from the installed package.
   */
  allTargets?: boolean;
  /**
   * File name used for the app launcher.
   *
   * @remarks The .exe suffix is added automatically for Windows targets.
   */
  appName?: string;
  /**
   * Parent directory that receives dist-linux-amd64/ style outputs.
   */
  outputRoot?: string;
  /**
   * Directory to zip as app assets.
   */
  assetSourcePath?: string;
  /**
   * Optional ZIP entry prefix used for the asset source.
   */
  assetPrefix?: string;
  /**
   * Muon config path to embed.
   */
  configPath?: string;
  /**
   * Asset salt override for deterministic tests.
   *
   * @remarks Production builds should omit this option.
   */
  assetSalt?: Uint8Array;
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
   * Internal target name used by the muon npm package.
   */
  target: MuonBuildTarget;
  /**
   * Fixed output directory name for the target.
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
   * Config object embedded into muon-core and the app launcher.
   */
  embeddedConfig: JsonObject;
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
   * Generated target distributions.
   */
  targets: MuonBuildTargetResult[];
}

/**
 * Returns the host target used by muon build when no explicit target is passed.
 */
export const getDefaultMuonBuildTarget = (): MuonBuildTarget => {
  return normalizeMuonBuildTarget(
    getDefaultMuonPrepareTarget(process.platform, process.arch),
  );
};

/**
 * Normalizes a user-facing target alias into the npm package target name.
 */
export const normalizeMuonBuildTarget = (target: string): MuonBuildTarget => {
  const normalized = target.trim().toLowerCase();
  if (normalized === "linux-i686" || normalized === "linux-ia32") {
    throw new Error("Linux i686 is not supported by muon build.");
  }

  const resolvedTarget = targetAliases[normalized];
  if (resolvedTarget === undefined) {
    throw new Error(`Unsupported muon build target: ${target}`);
  }

  return resolvedTarget;
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
  const appName = await resolveAppName(root, options.appName);
  const assetInput = resolveAssetInput(
    root,
    options.assetSourcePath,
    options.assetPrefix,
  );
  const sourceConfig = await readBuildConfig(root, options.configPath);
  const salt = Buffer.from(
    options.assetSalt ?? randomBytes(assetSaltByteLength),
  );

  const results: MuonBuildTargetResult[] = [];

  for (const target of targets) {
    const result = await buildMuonTarget({
      packageDirectory,
      outputRoot,
      appName,
      target,
      assetInput,
      sourceConfig,
      salt,
    });
    results.push(result);
  }

  return {
    root,
    appName,
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
    return [...allTargets];
  }

  if (options.targets !== undefined && options.targets.length > 0) {
    return [
      ...new Set(
        options.targets.map((target) => normalizeMuonBuildTarget(target)),
      ),
    ];
  }

  return [getDefaultMuonBuildTarget()];
};

const resolveAssetInput = (
  root: string,
  assetSourcePath: string | undefined,
  assetPrefix: string | undefined,
): AssetInput => {
  const sourcePath = resolve(root, assetSourcePath ?? "assets");
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

const resolveAppName = async (
  root: string,
  appName: string | undefined,
): Promise<string> => {
  if (appName !== undefined) {
    return sanitizeAppName(appName);
  }

  const packageJsonPath = join(root, "package.json");
  const packageJson = await readJsonObjectFile(packageJsonPath, "package.json");
  const packageName =
    typeof packageJson.name === "string" ? packageJson.name : defaultAppName;
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

const readBuildConfig = async (
  root: string,
  configPath: string | undefined,
): Promise<JsonObject> => {
  const resolvedConfigPath = await resolveConfigPath(root, configPath);
  if (resolvedConfigPath === undefined) {
    return {};
  }

  return readJsonObjectFile(resolvedConfigPath, "Muon config file");
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
  outputRoot: string;
  appName: string;
  target: MuonBuildTarget;
  assetInput: AssetInput;
  sourceConfig: JsonObject;
  salt: Buffer;
}): Promise<MuonBuildTargetResult> => {
  const descriptor = targetDescriptors[input.target];
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
  );
  const embeddedConfig = createEmbeddedConfig(input.sourceConfig, asset);

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

  return {
    target: input.target,
    distributionDirectoryName: descriptor.distributionDirectoryName,
    outputPath,
    launcherPath,
    asset,
    embeddedConfig,
  };
};

const verifyTargetInputs = async (input: {
  sourceRuntimePath: string;
  sourceBootstrapPath: string;
  descriptor: MuonBuildTargetDescriptor;
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
    join(input.sourceRuntimePath, "THIRD_PARTY_NOTICES.md"),
    `Muon third-party notices for ${input.target}`,
  );
};

const getLauncherFileName = (
  appName: string,
  descriptor: MuonBuildTargetDescriptor,
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
  descriptor: MuonBuildTargetDescriptor,
): Promise<void> => {
  for (const fileName of descriptor.runtimeFiles) {
    await copyFile(
      join(sourceRuntimePath, fileName),
      join(outputPath, fileName),
    );
  }
  await copyFile(
    join(sourceRuntimePath, "THIRD_PARTY_NOTICES.md"),
    join(outputPath, "THIRD_PARTY_NOTICES.md"),
  );
};

const writeAssetArchive = async (
  input: AssetInput,
  outputPath: string,
  salt: Buffer,
): Promise<MuonBuildAssetResult> => {
  await assertDirectory(input.sourcePath, "Muon asset source");
  const entries = await collectZipEntries(input.sourcePath, input.prefix);
  if (entries.length === 0) {
    throw new Error(`Muon asset source has no files: ${input.sourcePath}`);
  }

  const archive = createZipArchive(entries);
  await writeFile(outputPath, archive);

  const signature = createHash("sha1")
    .update(archive)
    .update(salt)
    .digest("hex");
  return {
    path: outputPath,
    signature,
    salt: salt.toString("hex"),
    entryCount: entries.length,
  };
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
): JsonObject => {
  const sourceAsset = sourceConfig.asset;
  if (sourceAsset !== undefined && !isJsonObject(sourceAsset)) {
    throw new Error("muon.json asset must be an object when present.");
  }

  return {
    ...sourceConfig,
    asset: {
      ...(sourceAsset ?? {}),
      from: appConfigFromPath,
      signature: asset.signature,
      salt: asset.salt,
    },
  };
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
  const stats = await stat(path).catch(() => undefined);
  if (stats === undefined || !stats.isDirectory()) {
    throw new Error(`${label} directory does not exist: ${path}`);
  }
};

const assertFile = async (path: string, label: string): Promise<void> => {
  const stats = await stat(path).catch(() => undefined);
  if (stats === undefined || !stats.isFile()) {
    throw new Error(`${label} file does not exist: ${path}`);
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
