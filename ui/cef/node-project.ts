// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { readFile, realpath, stat } from "node:fs/promises";
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
  /** Original engines range for one project, or the intersection for multiple projects. */
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
 * Node.js service project and its runtime engine requirements.
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
 * Combines the Node.js service projects into one required runtime constraint.
 * @param projects - Service projects built for the application.
 * @returns The runtime requirement when at least one Node.js service is configured.
 */
export const createMuonNodeRuntimeRequirement = (
  projects: readonly ResolvedMuonNodeProject[],
): MuonNodeRuntimeRequirement | undefined => {
  if (projects.length === 0) return undefined;
  let sets = projects[0]!.comparatorSets;
  for (const current of projects.slice(1)) {
    const merged: string[][] = [];
    for (const left of sets)
      for (const right of current.comparatorSets) {
        if (comparatorSetsIntersect(left, right))
          merged.push([...new Set([...left, ...right])]);
      }
    sets = [...new Map(merged.map((set) => [set.join(" "), set])).values()];
    if (!sets.length)
      throw new Error(
        `Muon Node projects require incompatible engines.node ranges: ${projects.map((item) => `${item.sourcePath}: ${item.engineRange}`).join("; ")}`,
      );
  }
  return {
    required: true,
    engineRangeSpecified: projects.some((item) => item.engineRangeSpecified),
    engineRange:
      projects.length === 1
        ? projects[0]!.engineRange
        : sets.map((set) => set.join(" ") || "*").join(" || "),
    comparatorSets: sets,
  };
};

/**
 * Resolves service project directories without compiling their source files.
 * @param config - Muon configuration, including the optional services map.
 * @param directory - Directory containing that configuration.
 * @returns Projects that must be protected from asset exposure or cleanup.
 */
export const resolveMuonNodeServiceProjects = async (
  config: Readonly<JsonObject> | undefined,
  directory: string,
): Promise<readonly ResolvedMuonNodeProject[]> => {
  if (!isJsonObject(config?.services)) return [];
  const projects: ResolvedMuonNodeProject[] = [];
  for (const [id, value] of Object.entries(config.services)) {
    if (!isJsonObject(value) || value.runtime !== "node") continue;
    try {
      projects.push(await resolveMuonNodeProject(value.project, directory));
    } catch (error) {
      throw new Error(`services.${id}: ${getErrorMessage(error)}`);
    }
  }
  return projects;
};

/**
 * Resolves the project directory and engine range of a Node.js service.
 * @param project - Configured service project path.
 * @param configDirectory - Directory containing the configuration.
 * @returns The resolved service project.
 */
export const resolveMuonNodeProject = async (
  project: unknown,
  configDirectory: string,
): Promise<ResolvedMuonNodeProject> => {
  if (typeof project !== "string" || project.trim().length === 0)
    throw new Error("project must be a non-empty string");

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
 * Verifies that staging cannot remove or recursively copy the Node source.
 *
 * @param projects Resolved Node.js service projects.
 * @param runtimeDirectory Runtime directory replaced or used for staging.
 * @returns A promise that resolves when the paths are disjoint.
 */
export const assertMuonNodeProjectStagingIsSafe = (
  projects: readonly ResolvedMuonNodeProject[],
  runtimeDirectory: string,
): Promise<void> =>
  assertMuonNodeProjectPathIsDisjoint(
    projects,
    runtimeDirectory,
    "runtime directory",
  );

/**
 * Verifies that a Node project cannot be exposed through the app asset source.
 *
 * @param projects Resolved Node.js service projects.
 * @param assetSourcePath Directory or ZIP path used as application assets.
 * @returns A promise that resolves when the paths are disjoint.
 */
export const assertMuonNodeProjectAssetSourceIsSafe = (
  projects: readonly ResolvedMuonNodeProject[],
  assetSourcePath: string,
): Promise<void> =>
  assertMuonNodeProjectPathIsDisjoint(
    projects,
    assetSourcePath,
    "asset source",
  );

/**
 * Verifies that a Vite build cannot delete or expose a Node project.
 *
 * @param projects Resolved Node.js service projects.
 * @param outputDirectory Vite output directory emptied before a build.
 * @param publicDirectory Vite public directory copied into build output, or an
 * empty string when public files are disabled.
 * @returns A promise that resolves when the Node project is disjoint from both
 * Vite paths.
 */
export const assertMuonNodeProjectViteBuildIsSafe = async (
  projects: readonly ResolvedMuonNodeProject[],
  outputDirectory: string,
  publicDirectory: string,
): Promise<void> => {
  await assertMuonNodeProjectPathIsDisjoint(
    projects,
    outputDirectory,
    "Vite output directory",
  );
  await assertMuonNodeProjectVitePublicDirectoryIsSafe(
    projects,
    publicDirectory,
  );
};

/**
 * Verifies that Vite cannot expose a Node project through its public directory.
 *
 * @param projects Resolved Node.js service projects.
 * @param publicDirectory Vite public directory copied into served or built
 * assets, or an empty string when public files are disabled.
 * @returns A promise that resolves when the Node project and public directory
 * are disjoint.
 */
export const assertMuonNodeProjectVitePublicDirectoryIsSafe = async (
  projects: readonly ResolvedMuonNodeProject[],
  publicDirectory: string,
): Promise<void> => {
  if (publicDirectory !== "") {
    await assertMuonNodeProjectPathIsDisjoint(
      projects,
      publicDirectory,
      "Vite public directory",
    );
  }
};

/**
 * Verifies that pack cleanup cannot remove a configured Node project.
 *
 * @param projects Resolved Node.js service projects.
 * @param cleanupDirectory Directory recursively removed by muon pack.
 * @returns A promise that resolves when the paths are disjoint.
 */
export const assertMuonNodeProjectPackCleanupIsSafe = (
  projects: readonly ResolvedMuonNodeProject[],
  cleanupDirectory: string,
): Promise<void> =>
  assertMuonNodeProjectPathIsDisjoint(
    projects,
    cleanupDirectory,
    "pack cleanup directory",
  );

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
  projects: readonly ResolvedMuonNodeProject[],
  comparedPath: string,
  comparedPathLabel: string,
): Promise<void> => {
  if (projects.length === 0) return;
  const canonicalComparedPath =
    await resolveCanonicalPotentialPath(comparedPath);
  for (const { sourcePath } of projects) {
    if (
      isSameOrInsidePath(sourcePath, canonicalComparedPath) ||
      isSameOrInsidePath(canonicalComparedPath, sourcePath)
    ) {
      throw new Error(
        `muon Node project and ${comparedPathLabel} must not overlap: ${sourcePath}, ${canonicalComparedPath}`,
      );
    }
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
