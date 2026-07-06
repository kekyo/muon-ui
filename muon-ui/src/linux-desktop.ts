// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import { constants } from "node:fs";
import { access, readFile, writeFile } from "node:fs/promises";
import { dirname, extname, isAbsolute, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

import { parse } from "json5";

import type { MuonLinuxDesktopOptions } from "./vite.js";
import { createNormalizedIconPngData } from "./windows-icon.js";
import {
  createAppIconOptionsSource,
  readConfigAppIconSource,
  readProjectAppIconSource,
  resolveMuonAppIconPath,
  type MuonAppIconSource,
} from "./app-icon.js";

export type { MuonLinuxDesktopOptions } from "./vite.js";

const defaultConfigFileNames = ["muon.json5", "muon.jsonc", "muon.json"];
const defaultIconFileName = "muon-desktop-icon.png";
const defaultDesktopConfigFileName = "muon-desktop.json";
const moduleDirectory =
  typeof __dirname === "string"
    ? __dirname
    : dirname(fileURLToPath(import.meta.url));

type JsonObject = Record<string, unknown>;

interface LinuxDesktopSource {
  options: MuonLinuxDesktopOptions;
  directory: string;
}

interface LinuxDesktopDefaults {
  desktopId: string;
  name: string;
  comment: string;
  categories: readonly string[];
  startupNotify: boolean;
}

interface ResolveLinuxDesktopInput {
  root: string;
  packageDirectory: string;
  muonConfig: JsonObject;
  muonConfigDirectory: string;
  options: MuonLinuxDesktopOptions | undefined;
  appIconPath: string | undefined;
  defaults: LinuxDesktopDefaults;
}

/**
 * Resolved Linux desktop integration metadata used by build and pack outputs.
 */
export interface ResolvedMuonLinuxDesktop {
  /** Desktop file identifier without the `.desktop` suffix. */
  desktopId: string;
  /** Application display name. */
  name: string;
  /** Application description used as a desktop entry comment. */
  comment: string;
  /** Desktop menu categories. */
  categories: readonly string[];
  /** Whether desktop startup notification is requested. */
  startupNotify: boolean;
  /** Resolved PNG icon file path. */
  iconPath: string;
  /** File name written into target distributions for the normalized icon. */
  iconFileName: string;
  /** File name written into target distributions for bootstrap desktop metadata. */
  configFileName: string;
}

/**
 * muon config file content and directory used for Linux desktop option
 * resolution.
 */
export interface MuonLinuxDesktopConfigSource {
  /** Parsed muon config object. */
  config: JsonObject;
  /** Directory that relative config paths are resolved from. */
  directory: string;
}

/**
 * Reads the muon config file using the same default file names as muon build.
 */
export const readMuonConfigForLinuxDesktop = async (
  root: string,
  configPath: string | undefined,
): Promise<MuonLinuxDesktopConfigSource> => {
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

/**
 * Merges Linux desktop options while preserving field-level fallback.
 */
export const mergeMuonLinuxDesktopOptions = (
  primary: MuonLinuxDesktopOptions | undefined,
  fallback: MuonLinuxDesktopOptions | undefined,
): MuonLinuxDesktopOptions | undefined => {
  if (primary === undefined) {
    return fallback;
  }
  if (fallback === undefined) {
    return primary;
  }

  const merged: MuonLinuxDesktopOptions = { ...fallback };
  if (primary.desktopId !== undefined) {
    merged.desktopId = primary.desktopId;
  }
  if (primary.name !== undefined) {
    merged.name = primary.name;
  }
  if (primary.comment !== undefined) {
    merged.comment = primary.comment;
  }
  if (primary.iconPath !== undefined) {
    merged.iconPath = primary.iconPath;
  }
  if (primary.categories !== undefined) {
    merged.categories = primary.categories;
  }
  if (primary.startupNotify !== undefined) {
    merged.startupNotify = primary.startupNotify;
  }
  return merged;
};

/**
 * Resolves Linux desktop integration metadata from options, config files, and
 * package metadata.
 */
export const resolveMuonLinuxDesktop = async (
  input: ResolveLinuxDesktopInput,
): Promise<ResolvedMuonLinuxDesktop> => {
  const projectJson = await readProjectJson(input.root);
  const sources = [
    createOptionsSource(input.options, input.root, "linuxDesktop"),
    readConfigLinuxDesktopSource(
      input.muonConfig,
      input.muonConfigDirectory,
      "muon.json",
    ),
  ].filter((source): source is LinuxDesktopSource => source !== undefined);
  const appIconSources = [
    createAppIconOptionsSource(input.appIconPath, input.root),
    readConfigAppIconSource(
      input.muonConfig,
      input.muonConfigDirectory,
      "muon.json",
    ),
    readProjectAppIconSource(projectJson, input.root),
  ].filter((source): source is MuonAppIconSource => source !== undefined);

  const desktopId = sanitizeDesktopId(
    resolveStringField(sources, "desktopId", input.defaults.desktopId),
  );
  const name = resolveStringField(sources, "name", input.defaults.name);
  const comment = resolveStringField(
    sources,
    "comment",
    input.defaults.comment,
  );
  const categories = resolveCategories(sources, input.defaults.categories);
  const startupNotify = resolveBooleanField(
    sources,
    "startupNotify",
    input.defaults.startupNotify,
  );
  const iconPath = await resolveLinuxIconPath(
    sources,
    appIconSources,
    await resolveDefaultLinuxDesktopIcon(input.packageDirectory),
  );

  return {
    desktopId,
    name,
    comment,
    categories,
    startupNotify,
    iconPath,
    iconFileName: defaultIconFileName,
    configFileName: defaultDesktopConfigFileName,
  };
};

/**
 * Removes build-only Linux desktop settings from runtime config.
 */
export const stripBuildOnlyLinuxDesktopConfig = (
  sourceConfig: JsonObject,
): JsonObject => {
  const sourceLinux = sourceConfig.linux;
  if (!isJsonObject(sourceLinux)) {
    return sourceConfig;
  }

  const linuxConfig: JsonObject = {};
  for (const [key, value] of Object.entries(sourceLinux)) {
    if (key !== "desktop") {
      linuxConfig[key] = value;
    }
  }

  const output: JsonObject = {};
  for (const [key, value] of Object.entries(sourceConfig)) {
    if (key !== "linux") {
      output[key] = value;
    }
  }
  if (Object.keys(linuxConfig).length > 0) {
    output.linux = linuxConfig;
  }
  return output;
};

/**
 * Writes normalized Linux desktop sidecar files into a target distribution.
 */
export const writeLinuxDesktopDistributionFiles = async (
  outputPath: string,
  desktop: ResolvedMuonLinuxDesktop,
): Promise<void> => {
  const iconData = await createNormalizedIconPngData(
    await readFile(desktop.iconPath),
    desktop.iconPath,
    "Linux desktop icon",
  );
  await writeFile(join(outputPath, desktop.iconFileName), iconData);
  await writeFile(
    join(outputPath, desktop.configFileName),
    `${JSON.stringify(
      {
        desktopId: desktop.desktopId,
        name: desktop.name,
        comment: desktop.comment,
        categories: desktop.categories,
        startupNotify: desktop.startupNotify,
        iconFileName: desktop.iconFileName,
      },
      undefined,
      2,
    )}\n`,
  );
};

/**
 * Creates a Desktop Entry file.
 */
export const createLinuxDesktopEntry = (input: {
  desktop: ResolvedMuonLinuxDesktop;
  exec: string;
  tryExec: string;
  icon: string;
}): string => {
  const lines = [
    "[Desktop Entry]",
    "Type=Application",
    `Name=${escapeDesktopString(input.desktop.name)}`,
  ];
  if (input.desktop.comment.length > 0) {
    lines.push(`Comment=${escapeDesktopString(input.desktop.comment)}`);
  }
  lines.push(`Exec=${input.exec}`);
  lines.push(`TryExec=${input.tryExec}`);
  lines.push(`Icon=${escapeDesktopString(input.icon)}`);
  lines.push(`Terminal=false`);
  if (input.desktop.categories.length > 0) {
    lines.push(`Categories=${input.desktop.categories.join(";")};`);
  }
  lines.push(`StartupNotify=${input.desktop.startupNotify ? "true" : "false"}`);
  lines.push(`StartupWMClass=${escapeDesktopString(input.desktop.desktopId)}`);
  lines.push("X-muon-Managed=true");
  lines.push("");
  return lines.join("\n");
};

/**
 * Quotes one Desktop Entry Exec argument.
 */
export const quoteDesktopExecArgument = (value: string): string => {
  let escaped = '"';
  for (const character of value) {
    if (
      character === '"' ||
      character === "\\" ||
      character === "$" ||
      character === "`"
    ) {
      escaped += "\\";
    }
    escaped += character;
  }
  return `${escaped}"`;
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
  const parsed = parse(await readFile(filePath, "utf8"));
  if (!isJsonObject(parsed)) {
    throw new Error(`${label} must contain a JSON object: ${filePath}`);
  }
  return parsed;
};

const readProjectJson = async (root: string): Promise<JsonObject> => {
  const projectJsonPath = join(root, "project.json");
  if (!(await fileExists(projectJsonPath))) {
    return {};
  }
  return await readJsonObjectFile(projectJsonPath, "project.json");
};

const createOptionsSource = (
  options: MuonLinuxDesktopOptions | undefined,
  directory: string,
  label: string,
): LinuxDesktopSource | undefined => {
  if (options === undefined) {
    return undefined;
  }
  return {
    options: validateLinuxDesktopOptions(options, label),
    directory,
  };
};

const readConfigLinuxDesktopSource = (
  config: JsonObject,
  directory: string,
  label: string,
): LinuxDesktopSource | undefined => {
  const linux = config.linux;
  if (linux === undefined) {
    return undefined;
  }
  if (!isJsonObject(linux)) {
    throw new Error(`${label} linux must be an object when present.`);
  }
  const desktop = linux.desktop;
  if (desktop === undefined) {
    return undefined;
  }
  return createOptionsSource(
    validateLinuxDesktopOptions(desktop, `${label} linux.desktop`),
    directory,
    `${label} linux.desktop`,
  );
};

const validateLinuxDesktopOptions = (
  value: unknown,
  label: string,
): MuonLinuxDesktopOptions => {
  if (!isJsonObject(value)) {
    throw new Error(`${label} must be an object.`);
  }

  const output: MuonLinuxDesktopOptions = {};
  copyOptionalStringDesktopField(value, "desktopId", output);
  copyOptionalStringDesktopField(value, "name", output);
  copyOptionalStringDesktopField(value, "comment", output);
  copyOptionalStringDesktopField(value, "iconPath", output);
  copyOptionalCategoriesField(value, output, label);
  copyOptionalBooleanDesktopField(value, "startupNotify", output, label);
  return output;
};

const copyOptionalStringDesktopField = (
  source: JsonObject,
  key: keyof Pick<
    MuonLinuxDesktopOptions,
    "desktopId" | "name" | "comment" | "iconPath"
  >,
  target: MuonLinuxDesktopOptions,
): void => {
  const value = source[key];
  if (value === undefined) {
    return;
  }
  if (typeof value !== "string") {
    throw new Error(`${key} must be a string when present.`);
  }
  if (value.trim() !== "") {
    target[key] = value.trim();
  }
};

const copyOptionalCategoriesField = (
  source: JsonObject,
  target: MuonLinuxDesktopOptions,
  label: string,
): void => {
  const value = source.categories;
  if (value === undefined) {
    return;
  }
  if (
    !Array.isArray(value) ||
    !value.every(
      (entry) =>
        typeof entry === "string" &&
        entry.trim() !== "" &&
        !entry.includes(";") &&
        !entry.includes("\n") &&
        !entry.includes("\r"),
    )
  ) {
    throw new Error(
      `${label} categories must be a string array without semicolons when present.`,
    );
  }
  target.categories = value.map((entry) => entry.trim());
};

const copyOptionalBooleanDesktopField = (
  source: JsonObject,
  key: keyof Pick<MuonLinuxDesktopOptions, "startupNotify">,
  target: MuonLinuxDesktopOptions,
  label: string,
): void => {
  const value = source[key];
  if (value === undefined) {
    return;
  }
  if (typeof value !== "boolean") {
    throw new Error(`${label} ${key} must be a boolean when present.`);
  }
  target[key] = value;
};

const resolveStringField = (
  sources: readonly LinuxDesktopSource[],
  field: keyof Pick<MuonLinuxDesktopOptions, "desktopId" | "name" | "comment">,
  fallback: string,
): string => {
  for (const source of sources) {
    const value = source.options[field];
    if (value !== undefined && value.trim() !== "") {
      return value.trim();
    }
  }
  return fallback;
};

const resolveCategories = (
  sources: readonly LinuxDesktopSource[],
  fallback: readonly string[],
): readonly string[] => {
  for (const source of sources) {
    if (source.options.categories !== undefined) {
      return source.options.categories;
    }
  }
  return fallback;
};

const resolveBooleanField = (
  sources: readonly LinuxDesktopSource[],
  field: keyof Pick<MuonLinuxDesktopOptions, "startupNotify">,
  fallback: boolean,
): boolean => {
  for (const source of sources) {
    const value = source.options[field];
    if (value !== undefined) {
      return value;
    }
  }
  return fallback;
};

const resolveLinuxIconPath = async (
  sources: readonly LinuxDesktopSource[],
  appIconSources: readonly MuonAppIconSource[],
  defaultIconPath: string | undefined,
): Promise<string> => {
  for (const source of sources) {
    const value = source.options.iconPath;
    if (value !== undefined && value.trim() !== "") {
      const iconPath = resolveResourcePath(source.directory, value);
      await assertIconPath(iconPath, true);
      return iconPath;
    }
  }
  const appIconPath = await resolveMuonAppIconPath(appIconSources);
  if (appIconPath !== undefined) {
    return appIconPath;
  }
  if (defaultIconPath !== undefined && (await fileExists(defaultIconPath))) {
    await assertIconPath(defaultIconPath, false);
    return defaultIconPath;
  }
  throw new Error("Linux desktop icon does not exist.");
};

const assertIconPath = async (
  iconPath: string,
  required: boolean,
): Promise<void> => {
  if (extname(iconPath).toLowerCase() !== ".png") {
    throw new Error(`Linux desktop icon must be a .png file: ${iconPath}`);
  }
  if (required && !(await fileExists(iconPath))) {
    throw new Error(`Linux desktop icon does not exist: ${iconPath}`);
  }
  if (await fileExists(iconPath)) {
    await createNormalizedIconPngData(
      await readFile(iconPath),
      iconPath,
      "Linux desktop icon",
    );
  }
};

const resolveDefaultLinuxDesktopIcon = async (
  packageDirectory: string,
): Promise<string | undefined> => {
  const candidates = [
    join(resolve(packageDirectory), "native", "muon-256.png"),
    join(moduleDirectory, "native", "muon-256.png"),
    join(moduleDirectory, "..", "dist", "native", "muon-256.png"),
    join(moduleDirectory, "..", "..", "images", "muon-256.png"),
  ];
  for (const candidate of candidates) {
    if (await fileExists(candidate)) {
      return candidate;
    }
  }
  return undefined;
};

const sanitizeDesktopId = (value: string): string => {
  const sanitized = value
    .trim()
    .toLowerCase()
    .replace(/[^a-z0-9._-]+/g, "-")
    .replace(/^[.-]+/g, "")
    .replace(/[.-]+$/g, "");
  return sanitized.length > 0 ? sanitized : "muon-app";
};

const escapeDesktopString = (value: string): string =>
  value
    .replaceAll("\\", "\\\\")
    .replaceAll("\n", "\\n")
    .replaceAll("\r", "\\r")
    .replaceAll("\t", "\\t");

const resolveResourcePath = (directory: string, path: string): string => {
  return isAbsolute(path) ? path : resolve(directory, path);
};

const fileExists = async (path: string): Promise<boolean> => {
  try {
    await access(path, constants.F_OK);
    return true;
  } catch {
    return false;
  }
};

const isJsonObject = (value: unknown): value is JsonObject =>
  typeof value === "object" && value !== null && !Array.isArray(value);
