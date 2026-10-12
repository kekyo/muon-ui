// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { readFile } from "node:fs/promises";
import { join } from "node:path";

import { parse } from "json5";

import type { MuonPackOptions } from "./pack-types.js";
/** Default output directory for distributable artifacts. */
export const defaultArtifactsDirectory = "artifacts";

/** Parsed package metadata. */
export type JsonObject = Record<string, unknown>;

/** Resolved package identity and attribution. */
export interface PackageMetadata {
  /** Name safe for package artifact filenames. */
  packageName: string;
  /** Package version. */
  version: string;
  /** Application description. */
  description: string;
  /** Package author or maintainer. */
  author: string;
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

/**
 * Reads package.json for application packaging.
 * @param root - Application project directory.
 * @returns Parsed package metadata.
 */
export const readPackageJson = async (root: string): Promise<JsonObject> => {
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

/**
 * Resolves package identity and attribution from options and project metadata.
 * @param packageJson - Application package metadata.
 * @param options - Explicit packaging overrides.
 * @returns Validated name, version, description and author.
 */
export const resolveMetadata = (
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
