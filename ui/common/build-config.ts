// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { constants } from "node:fs";
import { access, readFile } from "node:fs/promises";

import { dirname, join, resolve } from "node:path";

import { fileURLToPath } from "node:url";

import { parse } from "json5";

import { getDefaultMuonPrepareTarget } from "../cef/prepare.js";
import {
  allMuonTargets,
  normalizeMuonTarget,
  type MuonDesktopTarget,
} from "./targets.js";

import type { MuonBuildTarget, MuonBuildOptions } from "./build-types.js";
const defaultConfigFileNames = ["muon.json5", "muon.jsonc", "muon.json"];

const defaultAppName = "muon-app";

const defaultAppId = "muon-app";

const moduleDirectory =
  typeof __dirname === "string"
    ? __dirname
    : dirname(fileURLToPath(import.meta.url));

/** Parsed object used by application configuration. */
export type JsonObject = Record<string, unknown>;

/** Resolved asset path and ZIP prefix. */
export type AssetInput = {
  /** Absolute path of the asset directory or archive. */
  sourcePath: string;
  /** Normalized archive entry prefix. */
  prefix: string;
};

/** Application configuration and the directory for relative paths. */
export type BuildConfig = {
  /** Parsed application configuration. */
  config: JsonObject;
  /** Base directory for paths declared in the configuration. */
  directory: string;
};

/**
 * Returns the host target used by muon build when no explicit target is passed.
 */
export const getDefaultMuonBuildTarget = (): MuonDesktopTarget => {
  return getDefaultMuonPrepareTarget(process.platform, process.arch);
};

/**
 * Normalizes a user-facing public target identifier.
 */
export const normalizeMuonBuildTarget = (target: string): MuonBuildTarget => {
  return normalizeMuonTarget(target, "muon build target");
};

/**
 * Resolves an explicit package directory or the installed distribution.
 * @param packageDirectory - Override, or undefined to use this bundle's directory.
 * @returns Absolute package directory.
 */
export const resolvePackageDirectory = (
  packageDirectory: string | undefined,
): string => {
  if (packageDirectory !== undefined) {
    return resolve(packageDirectory);
  }

  return moduleDirectory;
};

/**
 * Selects and normalizes the targets requested by the build options.
 * @param options - Explicit targets and the all-targets setting.
 * @returns Unique targets in build order.
 */
export const resolveBuildTargets = (
  options: MuonBuildOptions,
): MuonBuildTarget[] => {
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

/**
 * Resolves the asset source against its project or configuration directory.
 * @param root - Project directory for option overrides.
 * @param assetSourcePath - Explicit asset path, or undefined to use configuration.
 * @param assetPrefix - Archive prefix, or undefined for the archive root.
 * @param buildConfig - Configuration and its base directory.
 * @returns Absolute asset source and normalized archive prefix.
 */
export const resolveAssetInput = (
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

/**
 * Normalizes an archive prefix to forward slashes.
 * @param prefix - User-provided archive prefix.
 * @returns Empty string or a prefix ending with a slash.
 */
export const normalizeZipPrefix = (prefix: string): string => {
  const normalized = prefix
    .replaceAll("\\", "/")
    .split("/")
    .filter((part) => part.length > 0)
    .join("/");

  return normalized.length > 0 ? `${normalized}/` : "";
};

/**
 * Reads project metadata, allowing projects without package.json.
 * @param root - Project directory.
 * @returns Parsed metadata, or an empty object when package.json is absent.
 */
export const readPackageJson = async (root: string): Promise<JsonObject> => {
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

/**
 * Resolves and sanitizes the launcher name from build options or package metadata.
 * @param packageJson - Project metadata.
 * @param appName - Explicit name, or undefined to use the package name.
 * @returns Launcher name safe for distribution filenames.
 */
export const resolveAppName = (
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

/**
 * Resolves and sanitizes the application identifier.
 * @param packageJson - Project metadata.
 * @param appId - Explicit identifier, or undefined to use the package name.
 * @returns Sanitized application identifier.
 */
export const resolveAppId = (
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

/**
 * Reads the selected application configuration and its base directory.
 * @param root - Project directory.
 * @param configPath - Explicit path, or undefined to search default filenames.
 * @returns Parsed configuration and the directory for relative paths.
 */
export const readBuildConfig = async (
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
    config: await readJsonObjectFile(resolvedConfigPath, "muon config file"),
    directory: dirname(resolvedConfigPath),
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

    throw new Error(`muon config file does not exist: ${resolvedPath}`);
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

/**
 * Reports whether a path exists.
 * @param path - Filesystem path to inspect.
 * @returns Whether the path can be accessed.
 */
export const fileExists = async (path: string): Promise<boolean> => {
  try {
    await access(path, constants.F_OK);
    return true;
  } catch {
    return false;
  }
};

/**
 * Checks for an object that can hold configuration fields.
 * @param value - Parsed JSON value.
 * @returns Whether the value is a non-null object excluding arrays.
 */
export const isJsonObject = (value: unknown): value is JsonObject => {
  return typeof value === "object" && value !== null && !Array.isArray(value);
};

/**
 * Converts a caught value to a diagnostic string.
 * @param error - Caught exception or rejection value.
 * @returns A readable error message.
 */
export const getErrorMessage = (error: unknown): string =>
  error instanceof Error ? error.message : String(error);
