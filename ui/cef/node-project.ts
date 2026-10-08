// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import {
  copyFile,
  cp,
  mkdir,
  readFile,
  realpath,
  rm,
  stat,
} from "node:fs/promises";
import {
  basename,
  dirname,
  isAbsolute,
  join,
  relative,
  resolve,
} from "node:path";

import SemverRange from "semver/classes/range.js";

declare const __MUON_NODE_SUPPORTED_ENGINE_RANGE__: string;

type JsonObject = Record<string, unknown>;

const stagedNodeProjectDirectoryName = "node-project";
const nodeBridgeFileName = "node-bridge.mjs";
const supportedNodeRange = __MUON_NODE_SUPPORTED_ENGINE_RANGE__;

/**
 * Node runtime constraint passed from the JavaScript build layer to a native
 * launcher or runtime preparer.
 */
export interface MuonNodeRuntimeRequirement {
  /** Whether the application must fail when no matching Node runtime exists. */
  readonly required: boolean;
  /** Whether `package.json` explicitly contains `engines.node`. */
  readonly engineRangeSpecified: boolean;
  /** Original `package.json` `engines.node` text, or `*` when omitted. */
  readonly engineRange: string;
  /**
   * Normalized comparator sets accepted by both the project and muon bridge.
   *
   * @remarks The outer array is an OR expression and each inner array is an
   * AND expression. An empty inner array represents a wildcard set.
   */
  readonly comparatorSets: readonly (readonly string[])[];
}

/**
 * Node project selected by the `node.project` muon configuration.
 */
export interface ResolvedMuonNodeProject {
  /** Absolute source directory copied into a muon runtime. */
  readonly sourcePath: string;
  /** Whether `package.json` explicitly contains `engines.node`. */
  readonly engineRangeSpecified: boolean;
  /** Original `package.json` `engines.node` text, or `*` when omitted. */
  readonly engineRange: string;
  /**
   * Normalized comparator sets accepted by both the project and muon bridge.
   */
  readonly comparatorSets: readonly (readonly string[])[];
}

const normalizeComparatorSet = (
  comparators: SemverRange["set"][number],
): string[] =>
  comparators
    .map((comparator) => comparator.value)
    .filter((comparator) => comparator !== "");

const comparatorSetsIntersect = (
  left: readonly string[],
  right: readonly string[],
): boolean =>
  new SemverRange(left.join(" ")).intersects(new SemverRange(right.join(" ")));

const intersectNodeEngineRanges = (
  projectRange: SemverRange,
): readonly (readonly string[])[] => {
  const bridgeRange = new SemverRange(supportedNodeRange);
  const comparatorSets: string[][] = [];
  for (const bridgeComparators of bridgeRange.set) {
    const bridgeSet = normalizeComparatorSet(bridgeComparators);
    for (const projectComparators of projectRange.set) {
      const projectSet = normalizeComparatorSet(projectComparators);
      if (comparatorSetsIntersect(bridgeSet, projectSet)) {
        comparatorSets.push([...bridgeSet, ...projectSet]);
      }
    }
  }
  return comparatorSets;
};

const readNodeEngineRequirement = async (
  packageJsonPath: string,
): Promise<
  Pick<
    ResolvedMuonNodeProject,
    "engineRangeSpecified" | "engineRange" | "comparatorSets"
  >
> => {
  let source: string;
  try {
    source = await readFile(packageJsonPath, "utf8");
  } catch (error) {
    throw new Error(
      `muon Node project package.json could not be read: ${packageJsonPath}: ${getErrorMessage(error)}`,
    );
  }

  let packageJson: unknown;
  try {
    packageJson = JSON.parse(source) as unknown;
  } catch (error) {
    throw new Error(
      `muon Node project package.json could not be parsed: ${packageJsonPath}: ${getErrorMessage(error)}`,
    );
  }
  if (!isJsonObject(packageJson)) {
    throw new Error(
      `muon Node project package.json must contain an object: ${packageJsonPath}`,
    );
  }

  const enginesValue = packageJson.engines;
  if (enginesValue !== undefined && !isJsonObject(enginesValue)) {
    throw new Error("package.json engines must be an object");
  }
  const nodeValue = enginesValue?.node;
  if (
    nodeValue !== undefined &&
    (typeof nodeValue !== "string" || nodeValue.trim().length === 0)
  ) {
    throw new Error("package.json engines.node must be a non-empty string");
  }

  const engineRange = nodeValue ?? "*";
  let projectRange: SemverRange;
  try {
    projectRange = new SemverRange(engineRange);
  } catch {
    throw new Error(
      `package.json engines.node is not a valid range: ${engineRange}`,
    );
  }
  const comparatorSets = intersectNodeEngineRanges(projectRange);
  if (comparatorSets.length === 0) {
    throw new Error(
      "package.json engines.node does not overlap the muon Node bridge range",
    );
  }
  return {
    engineRangeSpecified: nodeValue !== undefined,
    engineRange,
    comparatorSets,
  };
};

/**
 * Creates the native runtime constraint for a resolved Node project.
 *
 * @param project Resolved Node project, or `undefined` when Node is disabled.
 * @param required Whether absence of a matching runtime is fatal.
 * @returns The launcher/runtime requirement, or `undefined` for a non-Node app.
 */
export const createMuonNodeRuntimeRequirement = (
  project: ResolvedMuonNodeProject | undefined,
  required: boolean,
): MuonNodeRuntimeRequirement | undefined =>
  project === undefined
    ? undefined
    : {
        required,
        engineRangeSpecified: project.engineRangeSpecified,
        engineRange: project.engineRange,
        comparatorSets: project.comparatorSets,
      };

/**
 * Resolves and validates an optional Node project from muon configuration.
 *
 * @param config Parsed muon configuration object.
 * @param configDirectory Directory used to resolve a relative `node.project`.
 * @returns The resolved project, or `undefined` when Node hosting is not enabled.
 */
export const resolveMuonNodeProject = async (
  config: Readonly<JsonObject> | undefined,
  configDirectory: string,
): Promise<ResolvedMuonNodeProject | undefined> => {
  const node = config?.node;
  if (node === undefined) {
    return undefined;
  }
  if (!isJsonObject(node)) {
    throw new Error("muon.json node must be an object");
  }

  const project = node.project;
  if (project === undefined) {
    throw new Error("muon.json node.project is required when node is present");
  }
  if (typeof project !== "string") {
    throw new Error("muon.json node.project must be a string");
  }
  if (project.trim().length === 0) {
    throw new Error("muon.json node.project must not be empty");
  }

  const configuredSourcePath = resolve(configDirectory, project);
  if (!(await isDirectory(configuredSourcePath))) {
    throw new Error(
      `muon Node project directory does not exist: ${configuredSourcePath}`,
    );
  }
  const sourcePath = await realpath(configuredSourcePath);
  const packageJsonPath = join(sourcePath, "package.json");
  if (!(await isFile(packageJsonPath))) {
    throw new Error(
      `muon Node project package.json does not exist: ${packageJsonPath}`,
    );
  }

  return {
    sourcePath,
    ...(await readNodeEngineRequirement(packageJsonPath)),
  };
};

/**
 * Rewrites a resolved Node project to its packaged runtime location.
 *
 * @param config Source muon configuration.
 * @param project Resolved Node project, when enabled.
 * @returns Configuration suitable for embedding in a built distribution.
 */
export const createPackagedMuonNodeConfig = (
  config: Readonly<JsonObject>,
  project: ResolvedMuonNodeProject | undefined,
): JsonObject => {
  if (project === undefined) {
    return { ...config };
  }

  const node = config.node;
  if (!isJsonObject(node)) {
    throw new Error("muon.json node must be an object");
  }
  return {
    ...config,
    node: {
      ...node,
      project: `./${stagedNodeProjectDirectoryName}`,
    },
  };
};

/**
 * Verifies that staging cannot remove or recursively copy the Node source.
 *
 * @param project Resolved Node project, when enabled.
 * @param runtimeDirectory Runtime directory replaced or used for staging.
 * @returns A promise that resolves when the paths are disjoint.
 */
export const assertMuonNodeProjectStagingIsSafe = (
  project: ResolvedMuonNodeProject | undefined,
  runtimeDirectory: string,
): Promise<void> =>
  assertMuonNodeProjectPathIsDisjoint(
    project,
    runtimeDirectory,
    "runtime directory",
  );

/**
 * Verifies that a Node project cannot be exposed through the app asset source.
 *
 * @param project Resolved Node project, when enabled.
 * @param assetSourcePath Directory or ZIP path used as application assets.
 * @returns A promise that resolves when the paths are disjoint.
 */
export const assertMuonNodeProjectAssetSourceIsSafe = (
  project: ResolvedMuonNodeProject | undefined,
  assetSourcePath: string,
): Promise<void> =>
  assertMuonNodeProjectPathIsDisjoint(project, assetSourcePath, "asset source");

/**
 * Verifies that a Vite build cannot delete or expose a Node project.
 *
 * @param project Resolved Node project, when enabled.
 * @param outputDirectory Vite output directory emptied before a build.
 * @param publicDirectory Vite public directory copied into build output, or an
 * empty string when public files are disabled.
 * @returns A promise that resolves when the Node project is disjoint from both
 * Vite paths.
 */
export const assertMuonNodeProjectViteBuildIsSafe = async (
  project: ResolvedMuonNodeProject | undefined,
  outputDirectory: string,
  publicDirectory: string,
): Promise<void> => {
  await assertMuonNodeProjectPathIsDisjoint(
    project,
    outputDirectory,
    "Vite output directory",
  );
  await assertMuonNodeProjectVitePublicDirectoryIsSafe(
    project,
    publicDirectory,
  );
};

/**
 * Verifies that Vite cannot expose a Node project through its public directory.
 *
 * @param project Resolved Node project, when enabled.
 * @param publicDirectory Vite public directory copied into served or built
 * assets, or an empty string when public files are disabled.
 * @returns A promise that resolves when the Node project and public directory
 * are disjoint.
 */
export const assertMuonNodeProjectVitePublicDirectoryIsSafe = async (
  project: ResolvedMuonNodeProject | undefined,
  publicDirectory: string,
): Promise<void> => {
  if (publicDirectory !== "") {
    await assertMuonNodeProjectPathIsDisjoint(
      project,
      publicDirectory,
      "Vite public directory",
    );
  }
};

/**
 * Verifies that pack cleanup cannot remove a configured Node project.
 *
 * @param project Resolved Node project, when enabled.
 * @param cleanupDirectory Directory recursively removed by muon pack.
 * @returns A promise that resolves when the paths are disjoint.
 */
export const assertMuonNodeProjectPackCleanupIsSafe = (
  project: ResolvedMuonNodeProject | undefined,
  cleanupDirectory: string,
): Promise<void> =>
  assertMuonNodeProjectPathIsDisjoint(
    project,
    cleanupDirectory,
    "pack cleanup directory",
  );

/**
 * Stages a resolved Node project beside a muon runtime executable.
 *
 * @param project Resolved Node project, or `undefined` to remove stale staging.
 * @param runtimeDirectory Runtime directory that receives `node-project/`.
 * @returns Absolute staged project path, or `undefined` when Node is disabled.
 *
 * @remarks Symbolic links are dereferenced so portable ZIP and tar packaging
 * retains linked dependencies as ordinary files and directories.
 */
export const stageMuonNodeProject = async (
  project: ResolvedMuonNodeProject | undefined,
  runtimeDirectory: string,
): Promise<string | undefined> => {
  const destinationPath = resolve(
    runtimeDirectory,
    stagedNodeProjectDirectoryName,
  );
  if (project === undefined) {
    await rm(destinationPath, { recursive: true, force: true });
    return undefined;
  }

  await assertMuonNodeProjectStagingIsSafe(project, runtimeDirectory);
  await mkdir(runtimeDirectory, { recursive: true });
  await assertMuonNodeProjectStagingIsSafe(project, runtimeDirectory);
  await rm(destinationPath, { recursive: true, force: true });
  await cp(project.sourcePath, destinationPath, {
    dereference: true,
    preserveTimestamps: true,
    recursive: true,
  });
  return destinationPath;
};

/**
 * Copies the native Node host plugin and JavaScript bridge into a build output.
 *
 * @param sourceRuntimeDirectory Packaged runtime source for one target.
 * @param outputRuntimeDirectory App distribution runtime directory.
 * @param platform Target operating system.
 */
export const stageMuonNodeHostArtifacts = async (
  sourceRuntimeDirectory: string,
  outputRuntimeDirectory: string,
  platform: "linux" | "windows",
): Promise<void> => {
  const pluginFileName = platform === "windows" ? "node.dll" : "node.so";
  const sourcePluginDirectory = join(sourceRuntimeDirectory, "plugins");
  const outputPluginDirectory = join(outputRuntimeDirectory, "plugins");
  const sourcePluginPath = join(sourcePluginDirectory, pluginFileName);
  const sourceBridgePath = join(sourcePluginDirectory, nodeBridgeFileName);

  await assertFile(sourcePluginPath, `muon Node plugin for ${platform}`);
  await assertFile(sourceBridgePath, "muon Node bridge");
  await mkdir(outputPluginDirectory, { recursive: true });
  await copyFile(sourcePluginPath, join(outputPluginDirectory, pluginFileName));
  await copyFile(
    sourceBridgePath,
    join(outputPluginDirectory, nodeBridgeFileName),
  );
};

/**
 * Validates that a prepared development runtime contains the Node host files.
 *
 * @param runtimeDirectory Prepared runtime directory.
 * @param platform Runtime operating system.
 */
export const assertMuonNodeHostArtifacts = async (
  runtimeDirectory: string,
  platform: NodeJS.Platform,
): Promise<void> => {
  const pluginFileName = platform === "win32" ? "node.dll" : "node.so";
  const pluginDirectory = join(runtimeDirectory, "plugins");
  await assertFile(join(pluginDirectory, pluginFileName), "muon Node plugin");
  await assertFile(
    join(pluginDirectory, nodeBridgeFileName),
    "muon Node bridge",
  );
};

const isJsonObject = (value: unknown): value is JsonObject =>
  typeof value === "object" && value !== null && !Array.isArray(value);

const isSameOrInsidePath = (parentPath: string, path: string): boolean => {
  const relativePath = relative(parentPath, path);
  return (
    relativePath === "" ||
    (!relativePath.startsWith("..") && !isAbsolute(relativePath))
  );
};

const isMissingPathError = (error: unknown): boolean => {
  const code = (error as NodeJS.ErrnoException).code;
  return code === "ENOENT" || code === "ENOTDIR";
};

const getErrorMessage = (error: unknown): string =>
  error instanceof Error ? error.message : String(error);

// Output paths often do not exist yet. Resolve their closest existing
// ancestor so symlink aliases cannot bypass overlap checks, then append the
// still-missing path segments without touching the filesystem.
const resolveCanonicalPotentialPath = async (path: string): Promise<string> => {
  let ancestorPath = resolve(path);
  const missingSegments: string[] = [];

  for (;;) {
    try {
      const canonicalAncestorPath = await realpath(ancestorPath);
      return resolve(canonicalAncestorPath, ...missingSegments.reverse());
    } catch (error) {
      if (!isMissingPathError(error)) {
        throw error;
      }
      const parentPath = dirname(ancestorPath);
      if (parentPath === ancestorPath) {
        throw error;
      }
      missingSegments.push(basename(ancestorPath));
      ancestorPath = parentPath;
    }
  }
};

const assertMuonNodeProjectPathIsDisjoint = async (
  project: ResolvedMuonNodeProject | undefined,
  comparedPath: string,
  comparedPathLabel: string,
): Promise<void> => {
  if (project === undefined) {
    return;
  }
  const [sourcePath, canonicalComparedPath] = await Promise.all([
    resolveCanonicalPotentialPath(project.sourcePath),
    resolveCanonicalPotentialPath(comparedPath),
  ]);
  if (
    isSameOrInsidePath(sourcePath, canonicalComparedPath) ||
    isSameOrInsidePath(canonicalComparedPath, sourcePath)
  ) {
    throw new Error(
      `muon Node project and ${comparedPathLabel} must not overlap: ${sourcePath}, ${canonicalComparedPath}`,
    );
  }
};

const isDirectory = async (path: string): Promise<boolean> => {
  try {
    return (await stat(path)).isDirectory();
  } catch {
    return false;
  }
};

const isFile = async (path: string): Promise<boolean> => {
  try {
    return (await stat(path)).isFile();
  } catch {
    return false;
  }
};

const assertFile = async (path: string, label: string): Promise<void> => {
  if (!(await isFile(path))) {
    throw new Error(`${label} file does not exist: ${path}`);
  }
};
