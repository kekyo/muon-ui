// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import { constants } from "node:fs";
import { access, readFile } from "node:fs/promises";
import { extname, isAbsolute, resolve } from "node:path";

import { createNormalizedIconPngData } from "./windows-icon.js";

type JsonObject = Record<string, unknown>;

/**
 * ZIP entry used for the generated static application icon asset.
 */
export const appIconAssetEntryName = "main/.muon/app-icon.png";

/**
 * Runtime asset URL injected as the generated initial title bar icon.
 */
export const appIconAssetUrl = "asset://main/.muon/app-icon.png";

/**
 * Source for a unified static application icon path.
 */
export interface MuonAppIconSource {
  /** Raw icon path as supplied by the source. */
  iconPath: string;
  /** Directory that relative icon paths are resolved from. */
  directory: string;
}

/**
 * Creates a static application icon source from API or CLI options.
 */
export const createAppIconOptionsSource = (
  iconPath: unknown,
  directory: string,
  label: string = "iconPath",
): MuonAppIconSource | undefined => {
  if (iconPath === undefined) {
    return undefined;
  }
  if (typeof iconPath !== "string") {
    throw new Error(`${label} must be a string when present.`);
  }
  if (iconPath.trim() === "") {
    return undefined;
  }
  return {
    iconPath: iconPath.trim(),
    directory,
  };
};

/**
 * Reads the top-level `iconPath` from a muon config object.
 */
export const readConfigAppIconSource = (
  config: JsonObject,
  directory: string,
  label: string,
): MuonAppIconSource | undefined => {
  const value = config.iconPath;
  if (value === undefined) {
    return undefined;
  }
  if (typeof value !== "string") {
    throw new Error(`${label} iconPath must be a string when present.`);
  }
  return createAppIconOptionsSource(value, directory);
};

/**
 * Reads the top-level `iconPath` from project metadata.
 */
export const readProjectAppIconSource = (
  projectJson: JsonObject,
  directory: string,
): MuonAppIconSource | undefined => {
  const value = projectJson.iconPath;
  return typeof value === "string"
    ? createAppIconOptionsSource(value, directory)
    : undefined;
};

/**
 * Resolves and validates the first available static application icon source.
 */
export const resolveMuonAppIconPath = async (
  sources: readonly MuonAppIconSource[],
): Promise<string | undefined> => {
  for (const source of sources) {
    const iconPath = resolveResourcePath(source.directory, source.iconPath);
    await assertMuonAppIconPath(iconPath);
    return iconPath;
  }
  return undefined;
};

const assertMuonAppIconPath = async (iconPath: string): Promise<void> => {
  if (extname(iconPath).toLowerCase() !== ".png") {
    throw new Error(`muon app icon must be a .png file: ${iconPath}`);
  }
  if (!(await fileExists(iconPath))) {
    throw new Error(`muon app icon does not exist: ${iconPath}`);
  }
  await createNormalizedIconPngData(
    await readFile(iconPath),
    iconPath,
    "muon app icon",
  );
};

const resolveResourcePath = (directory: string, value: string): string =>
  isAbsolute(value) ? value : resolve(directory, value);

const fileExists = async (path: string): Promise<boolean> => {
  try {
    await access(path, constants.F_OK);
    return true;
  } catch {
    return false;
  }
};
