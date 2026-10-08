// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { type Stats } from "node:fs";
import {
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
import { basename, isAbsolute, join, relative, resolve, sep } from "node:path";
import { tmpdir } from "node:os";

import AdmZip from "adm-zip";

import {
  embedMuonConfigInLauncherFile,
  embedMuonConfigInRuntime,
} from "./embed-config.js";

import {
  getMuonTargetDescriptor,
  getMuonTargetRuntimeAppId,
  type MuonDesktopTarget,
  type MuonTargetDescriptor,
} from "../common/targets.js";
import {
  resolveMuonWindowsResource,
  stripBuildOnlyWindowsResourceConfig,
  updateWindowsPeIconResource,
  updateWindowsPeResources,
  type ResolvedMuonWindowsResource,
} from "./windows-resource.js";
import {
  resolveMuonLinuxDesktop,
  stripBuildOnlyLinuxDesktopConfig,
  writeLinuxDesktopDistributionFiles,
  type ResolvedMuonLinuxDesktop,
} from "./linux-desktop.js";
import {
  resolveMuonWindowsCodeSigning,
  signWindowsExecutable,
  stripBuildOnlyWindowsCodeSigningConfig,
  type ResolvedMuonWindowsCodeSigning,
} from "./windows-code-signing.js";
import { appIconAssetEntryName, appIconAssetUrl } from "../common/app-icon.js";
import type { MuonRuntimePluginConfig } from "../common/capability.js";
import type { MuonProgressCallback } from "../common/progress.js";
import {
  assertMuonNodeProjectAssetSourceIsSafe,
  assertMuonNodeProjectStagingIsSafe,
  createPackagedMuonNodeConfig,
  createMuonNodeRuntimeRequirement,
  resolveMuonNodeProject,
  stageMuonNodeHostArtifacts,
  stageMuonNodeProject,
  type MuonNodeRuntimeRequirement,
  type ResolvedMuonNodeProject,
} from "./node-project.js";

import type {
  InternalMuonBuildOptions,
  MuonBuildOptions,
  MuonBuildAssetResult,
  MuonDesktopBuildTargetResult,
  MuonBuildResult,
} from "../common/build-types.js";
import {
  JsonObject,
  AssetInput,
  BuildConfig,
  resolvePackageDirectory,
  resolveBuildTargets,
  resolveAssetInput,
  readPackageJson,
  resolveAppName,
  resolveAppId,
  readBuildConfig,
  isJsonObject,
} from "../common/build-config.js";
const appConfigSourcePath = "./assets.zip";

const muonLicenseFileName = "CREDITS.md";

const directoryMode = 0o755;

const executableMode = 0o755;

const assetSaltByteLength = 16;

type ZipEntry = {
  name: string;
  data: Buffer;
};

type DistributionFile = {
  sourcePath: string;
  fileName: string;
};

/**
 * Builds CEF-free muon app distribution directories for one or more targets.
 */
export const buildDesktopMuonApp = async (
  options: MuonBuildOptions = {},
): Promise<
  Omit<MuonBuildResult, "targets"> & { targets: MuonDesktopBuildTargetResult[] }
> => {
  const internalOptions = options as InternalMuonBuildOptions;
  const browserProfilePathOverride = internalOptions.browserProfilePathOverride;
  const environment = internalOptions.environment ?? process.env;
  const progress = internalOptions.progress;
  const root = resolve(options.root ?? process.cwd());
  const packageDirectory = resolvePackageDirectory(options.packageDirectory);
  const targets = resolveBuildTargets(options).map((target) => {
    if (target === "android")
      throw new Error("Android is not a desktop target.");
    return target;
  });
  const outputRoot = resolve(root, options.outputRoot ?? ".");
  const packageJson = await readPackageJson(root);
  const appName = resolveAppName(packageJson, options.appName);
  const appId = resolveAppId(packageJson, options.appId);
  const buildConfig = await readBuildConfig(root, options.configPath);
  const nodeProject = await resolveMuonNodeProject(
    buildConfig.config,
    buildConfig.directory,
  );
  const runtimePluginConfig = options.runtimePluginConfig ?? {
    mode: "simple",
  };
  const nodeRuntimeRequirement = createMuonNodeRuntimeRequirement(
    nodeProject,
    true,
  );
  for (const target of targets) {
    await assertMuonNodeProjectStagingIsSafe(
      nodeProject,
      join(
        outputRoot,
        getMuonTargetDescriptor(target).distributionDirectoryName,
      ),
    );
  }
  const { android: _androidBuildConfig, ...desktopConfig } = buildConfig.config;
  const sourceConfig = createPackagedMuonNodeConfig(
    applyRuntimePluginConfig(desktopConfig, runtimePluginConfig),
    nodeProject,
  );
  assertNoUserInitialTitleBarIcon(sourceConfig);
  assertNoUserNodeRuntime(sourceConfig);
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
  await assertMuonNodeProjectAssetSourceIsSafe(
    nodeProject,
    assetInput.sourcePath,
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
  const windowsCodeSigning = resolveMuonWindowsCodeSigning({
    muonConfig: sourceConfig,
    options: options.windowsCodeSigning,
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
  const distributionFiles = await resolveDistributionFiles({
    root,
    packageJson,
    configuredFiles: options.distributionFiles,
    assetSourcePath: assetInput.sourcePath,
    outputPaths: targets.map((target) =>
      join(
        outputRoot,
        getMuonTargetDescriptor(target).distributionDirectoryName,
      ),
    ),
  });

  const results: MuonDesktopBuildTargetResult[] = [];

  for (let index = 0; index < targets.length; index += 1) {
    const target = targets[index] as MuonDesktopTarget;
    progress?.({
      phase: "build",
      status: `Building muon target ${target} (${index + 1}/${targets.length})`,
    });
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
      windowsCodeSigning,
      linuxDesktop,
      distributionFiles,
      nodeProject,
      nodeRuntimeRequirement,
      salt,
      environment,
      browserStartPage: options.browserStartPage,
      browserProfilePathOverride,
      includeRuntimeHelper: options.includeRuntimeHelper === true,
      progress,
    });
    results.push(result);
    progress?.({
      phase: "build",
      status: `Built ${result.outputPath}`,
    });
  }

  return {
    root,
    appName,
    appId,
    targets: results,
  };
};

const readStringArray = (value: unknown, label: string): readonly string[] => {
  if (
    Array.isArray(value) &&
    value.every((entry) => typeof entry === "string")
  ) {
    return value;
  }
  throw new Error(`${label} must be an array of strings.`);
};

const readDistributionFileCandidates = (
  packageJson: JsonObject,
  configuredFiles: readonly string[] | undefined,
): readonly string[] => {
  if (configuredFiles !== undefined) {
    return readStringArray(configuredFiles, "muon distributionFiles");
  }
  if (packageJson.files === undefined) {
    return [];
  }
  return readStringArray(packageJson.files, "package.json files");
};

const isSameOrInsidePath = (parentPath: string, path: string): boolean => {
  const relativePath = relative(parentPath, path);
  return (
    relativePath === "" ||
    (!relativePath.startsWith("..") && !isAbsolute(relativePath))
  );
};

const isExcludedDistributionFilePath = (
  path: string,
  excludedPaths: readonly string[],
): boolean =>
  excludedPaths.some((excludedPath) => isSameOrInsidePath(excludedPath, path));

const resolveDistributionFiles = async (input: {
  root: string;
  packageJson: JsonObject;
  configuredFiles: readonly string[] | undefined;
  assetSourcePath: string;
  outputPaths: readonly string[];
}): Promise<DistributionFile[]> => {
  const candidates = readDistributionFileCandidates(
    input.packageJson,
    input.configuredFiles,
  );
  const excludedPaths = [
    resolve(input.root, "node_modules"),
    resolve(input.root, ".git"),
    input.assetSourcePath,
    ...input.outputPaths,
  ];
  const destinationNames = new Set<string>();
  const files: DistributionFile[] = [];

  for (const candidate of candidates) {
    if (candidate.trim() === "") {
      throw new Error("muon distribution file path must not be empty.");
    }
    if (isAbsolute(candidate)) {
      throw new Error(
        `muon distribution file path must be relative: ${candidate}`,
      );
    }

    const sourcePath = resolve(input.root, candidate);
    if (!isSameOrInsidePath(input.root, sourcePath)) {
      throw new Error(
        `muon distribution file path must stay inside the project root: ${candidate}`,
      );
    }
    if (isExcludedDistributionFilePath(sourcePath, excludedPaths)) {
      continue;
    }

    const stats = await statOrUndefined(sourcePath);
    if (stats === undefined) {
      throw new Error(`muon distribution file does not exist: ${candidate}`);
    }
    if (!stats.isFile()) {
      throw new Error(
        `muon distribution file must be a regular file: ${candidate}`,
      );
    }

    const fileName = basename(sourcePath);
    if (destinationNames.has(fileName)) {
      throw new Error(
        `Duplicate muon distribution file destination: ${fileName}`,
      );
    }
    destinationNames.add(fileName);
    files.push({ sourcePath, fileName });
  }

  return files;
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

/**
 * Resolves the Node project selected by a build-oriented muon configuration.
 *
 * @param root Project root used to locate a default or relative config path.
 * @param configPath Explicit muon config path, or `undefined` to use the
 * default config lookup.
 * @returns The resolved Node project, or `undefined` when Node hosting is not
 * enabled.
 */
export const resolveMuonNodeProjectForBuildConfig = async (
  root: string,
  configPath: string | undefined,
): Promise<ResolvedMuonNodeProject | undefined> => {
  const buildConfig = await readBuildConfig(root, configPath);
  return await resolveMuonNodeProject(
    buildConfig.config,
    buildConfig.directory,
  );
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

const buildMuonTarget = async (input: {
  packageDirectory: string;
  root: string;
  outputRoot: string;
  appName: string;
  appId: string;
  target: MuonDesktopTarget;
  assetInput: AssetInput;
  sourceConfig: JsonObject;
  windowsResource: ResolvedMuonWindowsResource;
  windowsCodeSigning: ResolvedMuonWindowsCodeSigning | undefined;
  linuxDesktop: ResolvedMuonLinuxDesktop;
  distributionFiles: readonly DistributionFile[];
  nodeProject: ResolvedMuonNodeProject | undefined;
  nodeRuntimeRequirement: MuonNodeRuntimeRequirement | undefined;
  salt: Buffer;
  environment: NodeJS.ProcessEnv;
  browserStartPage: string | undefined;
  browserProfilePathOverride: string | undefined;
  includeRuntimeHelper: boolean;
  progress: MuonProgressCallback | undefined;
}): Promise<MuonDesktopBuildTargetResult> => {
  const descriptor = getMuonTargetDescriptor(input.target);
  const sourceRuntimePath = join(
    input.packageDirectory,
    "runtime",
    input.target,
  );
  const sourceLauncherPath = join(
    input.packageDirectory,
    "native",
    input.target,
    descriptor.launcherExecutableName,
  );
  const sourceRuntimeHelperPath =
    input.includeRuntimeHelper &&
    descriptor.runtimeHelperExecutableName !== undefined
      ? join(
          input.packageDirectory,
          "native",
          input.target,
          descriptor.runtimeHelperExecutableName,
        )
      : undefined;
  const outputPath = join(
    input.outputRoot,
    descriptor.distributionDirectoryName,
  );
  const launcherPath = join(
    outputPath,
    getLauncherFileName(input.appName, descriptor),
  );
  const runtimeHelperPath =
    sourceRuntimeHelperPath === undefined ||
    descriptor.runtimeHelperExecutableName === undefined
      ? undefined
      : join(outputPath, descriptor.runtimeHelperExecutableName);
  const assetZipPath = join(outputPath, "assets.zip");
  const runtimeAppId = getMuonTargetRuntimeAppId(input.appId, input.target);
  const appIconPath =
    descriptor.os === "windows"
      ? input.windowsResource.iconPath
      : input.linuxDesktop.iconPath;

  await verifyTargetInputs({
    sourceRuntimePath,
    sourceLauncherPath,
    sourceRuntimeHelperPath,
    descriptor,
    target: input.target,
  });

  await rm(outputPath, { recursive: true, force: true });
  await mkdir(outputPath, { recursive: true, mode: directoryMode });
  await copyRuntimeFiles(sourceRuntimePath, outputPath, descriptor);
  for (const executableName of [
    descriptor.runtimeExecutableName,
    ...descriptor.runtimeAuxiliaryExecutableNames,
  ]) {
    await chmod(join(outputPath, executableName), executableMode);
  }
  await copyFile(sourceLauncherPath, launcherPath);
  await chmod(launcherPath, executableMode);
  if (
    sourceRuntimeHelperPath !== undefined &&
    runtimeHelperPath !== undefined
  ) {
    await copyFile(sourceRuntimeHelperPath, runtimeHelperPath);
    await chmod(runtimeHelperPath, executableMode);
  }
  if (input.nodeProject !== undefined) {
    await stageMuonNodeHostArtifacts(
      sourceRuntimePath,
      outputPath,
      descriptor.os,
    );
    await stageMuonNodeProject(input.nodeProject, outputPath);
  }
  await copyDistributionFiles(input.distributionFiles, outputPath);

  input.progress?.({
    phase: "build",
    status: "Creating assets.zip",
  });
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
    input.browserProfilePathOverride,
  );
  const launcherEmbeddedConfig = createLauncherEmbeddedConfig(
    embeddedConfig,
    input.nodeRuntimeRequirement,
  );

  input.progress?.({
    phase: "build",
    status: "Embedding config",
  });
  await withTemporaryConfig(embeddedConfig, async (configPath) => {
    await embedMuonConfigInRuntime({
      runtimePath: outputPath,
      configPath,
      outputRuntimePath: undefined,
    });
  });
  await withTemporaryConfig(launcherEmbeddedConfig, async (configPath) => {
    await embedMuonConfigInLauncherFile({
      launcherPath: launcherPath,
      configPath,
      outputPath: undefined,
    });
    if (runtimeHelperPath !== undefined) {
      await embedMuonConfigInLauncherFile({
        launcherPath: runtimeHelperPath,
        configPath,
        outputPath: undefined,
      });
    }
  });

  if (descriptor.os === "windows") {
    input.progress?.({
      phase: "build",
      status: "Updating Windows resources",
    });
    await updateWindowsPeIconResource({
      executablePath: join(outputPath, descriptor.runtimeExecutableName),
      resource: input.windowsResource,
      environment: input.environment,
      cwd: input.root,
    });
    await updateWindowsPeResources({
      executablePath: launcherPath,
      resource: input.windowsResource,
      environment: input.environment,
      cwd: input.root,
    });
    if (input.windowsCodeSigning !== undefined) {
      input.progress?.({
        phase: "build",
        status: "Signing Windows executables",
      });
    }
    await signWindowsExecutable({
      codeSigning: input.windowsCodeSigning,
      kind: "runtime",
      target: input.target,
      path: join(outputPath, descriptor.runtimeExecutableName),
      cwd: input.root,
      environment: input.environment,
    });
    await signWindowsExecutable({
      codeSigning: input.windowsCodeSigning,
      kind: "launcher",
      target: input.target,
      path: launcherPath,
      cwd: input.root,
      environment: input.environment,
    });
  } else if (descriptor.os === "linux") {
    input.progress?.({
      phase: "build",
      status: "Writing Linux desktop files",
    });
    await writeLinuxDesktopDistributionFiles(outputPath, input.linuxDesktop);
  }

  return {
    target: input.target,
    distributionDirectoryName: descriptor.distributionDirectoryName,
    outputPath,
    launcherPath,
    ...(runtimeHelperPath === undefined ? {} : { runtimeHelperPath }),
    asset,
    runtimeAppId,
    embeddedConfig,
    ...(descriptor.os === "linux" ? { linuxDesktop: input.linuxDesktop } : {}),
  };
};

const verifyTargetInputs = async (input: {
  sourceRuntimePath: string;
  sourceLauncherPath: string;
  sourceRuntimeHelperPath: string | undefined;
  descriptor: MuonTargetDescriptor;
  target: MuonDesktopTarget;
}): Promise<void> => {
  await assertDirectory(
    input.sourceRuntimePath,
    `muon runtime for ${input.target}`,
  );
  await assertFile(
    input.sourceLauncherPath,
    `muon launcher for ${input.target}`,
  );
  if (input.sourceRuntimeHelperPath !== undefined) {
    await assertFile(
      input.sourceRuntimeHelperPath,
      `muon runtime helper for ${input.target}`,
    );
  }
  for (const fileName of input.descriptor.runtimeFiles) {
    await assertFile(
      join(input.sourceRuntimePath, fileName),
      `muon runtime file ${fileName} for ${input.target}`,
    );
  }
  await assertFile(
    join(input.sourceRuntimePath, muonLicenseFileName),
    `muon license file for ${input.target}`,
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

const copyDistributionFiles = async (
  files: readonly DistributionFile[],
  outputPath: string,
): Promise<void> => {
  for (const file of files) {
    const destinationPath = join(outputPath, file.fileName);
    if ((await statOrUndefined(destinationPath)) !== undefined) {
      throw new Error(
        `muon distribution file destination already exists: ${file.fileName}`,
      );
    }
    await copyFile(file.sourcePath, destinationPath);
  }
};

const writeAssetArchive = async (
  input: AssetInput,
  outputPath: string,
  salt: Buffer,
  extraEntries: readonly ZipEntry[],
): Promise<MuonBuildAssetResult> => {
  const sourceStats = await statOrUndefined(input.sourcePath);
  if (sourceStats === undefined) {
    throw new Error(`muon asset source does not exist: ${input.sourcePath}`);
  }

  const archive = sourceStats.isDirectory()
    ? await createAssetArchiveFromDirectory(input, extraEntries)
    : sourceStats.isFile()
      ? await createAssetArchiveFromZipFile(input.sourcePath, extraEntries)
      : undefined;
  if (archive === undefined) {
    throw new Error(
      `muon asset source is not a directory or file: ${input.sourcePath}`,
    );
  }
  await writeFile(outputPath, archive);

  const signature = createHash("sha256")
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
    throw new Error(`muon asset source has no files: ${input.sourcePath}`);
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
        `muon app icon asset entry already exists: ${entry.name}`,
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
        `muon app icon asset entry already exists: ${entry.name}`,
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

  throw new Error(`muon asset ZIP could not be read: ${sourcePath}`);
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
  browserProfilePathOverride: string | undefined,
): JsonObject => {
  const sourceAsset = sourceConfig.asset;
  if (sourceAsset !== undefined && !isJsonObject(sourceAsset)) {
    throw new Error("muon.json asset must be an object when present.");
  }
  const sourceLauncher = sourceConfig.launcher;
  if (sourceLauncher !== undefined && !isJsonObject(sourceLauncher)) {
    throw new Error("muon.json launcher must be an object when present.");
  }

  const runtimeConfig = stripBuildOnlyAppIconConfig(
    stripBuildOnlyLinuxDesktopConfig(
      stripBuildOnlyWindowsCodeSigningConfig(
        stripBuildOnlyWindowsResourceConfig(sourceConfig),
      ),
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
  if (browserProfilePathOverride !== undefined) {
    browserConfig.profilePath = browserProfilePathOverride;
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
    launcher: {
      ...(sourceLauncher ?? {}),
      appId,
      desktopId,
    },
  };
};

const createLauncherEmbeddedConfig = (
  embeddedConfig: JsonObject,
  nodeRuntimeRequirement: MuonNodeRuntimeRequirement | undefined,
): JsonObject => {
  if (nodeRuntimeRequirement === undefined) {
    return embeddedConfig;
  }
  const launcher = embeddedConfig.launcher;
  if (!isJsonObject(launcher)) {
    throw new Error("generated muon launcher config must be an object");
  }
  return {
    ...embeddedConfig,
    launcher: {
      ...launcher,
      nodeRuntime: nodeRuntimeRequirement,
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

const assertNoUserNodeRuntime = (sourceConfig: JsonObject): void => {
  const sourceLauncher = sourceConfig.launcher;
  if (sourceLauncher === undefined) {
    return;
  }
  if (!isJsonObject(sourceLauncher)) {
    throw new Error("muon.json launcher must be an object when present.");
  }
  if (sourceLauncher.nodeRuntime !== undefined) {
    throw new Error(
      "muon.json launcher.nodeRuntime is generated by muon build.",
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
