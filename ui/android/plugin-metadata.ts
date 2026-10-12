// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { createHash } from "node:crypto";
import { readFile } from "node:fs/promises";
import { resolve } from "node:path";
import type { MuonAndroidAbi } from "./build.js";

const record = (value: unknown): Record<string, unknown> => {
  if (typeof value !== "object" || value === null || Array.isArray(value))
    throw new Error("Android plugin metadata must contain objects.");
  return value as Record<string, unknown>;
};

/**
 * Reads producer-supplied Android catalogs without loading native libraries.
 * @param input - Android plugin definitions containing catalog and library paths.
 * @param abis - Selected application ABIs.
 * @param directory - Base directory for paths in these definitions.
 * @param required - Whether every definition must supply a catalog.
 * @returns Public function paths by logical plugin name, with ABI hashes checked.
 */
export const readAndroidPluginMetadata = async (
  input: unknown,
  abis: readonly MuonAndroidAbi[],
  directory: string,
  required: boolean,
): Promise<ReadonlyMap<string, readonly string[]>> => {
  if (!Array.isArray(input))
    throw new Error("android.plugins must be an array.");
  const catalogs = new Map<string, readonly string[]>();
  const paths = new Set<string>();
  for (const raw of input) {
    const plugin = record(raw);
    const name = String(plugin.name);
    if (plugin.metadata === undefined && !required) continue;
    if (typeof plugin.metadata !== "string" || !plugin.metadata)
      throw new Error(
        `Android plugin ${name} requires a metadata file for validate mode.`,
      );
    if (catalogs.has(name))
      throw new Error(`Duplicate Android plugin metadata: ${name}`);
    const metadata = record(
      JSON.parse(await readFile(resolve(directory, plugin.metadata), "utf8")),
    );
    if (
      metadata.schemaVersion !== 1 ||
      !Array.isArray(metadata.functions) ||
      metadata.functions.length === 0
    )
      throw new Error(`Invalid Android plugin metadata for ${name}.`);
    const functions = metadata.functions.map((path: unknown) => {
      if (
        typeof path !== "string" ||
        !/^[A-Za-z_$][\w$]*(?:\.[A-Za-z_$][\w$]*){2,}$/u.test(path) ||
        paths.has(path)
      )
        throw new Error(
          `Invalid or duplicate Android plugin function: ${String(path)}`,
        );
      paths.add(path);
      return path;
    });
    const hashes = record(metadata.sha256);
    const libraries = record(plugin.libraries);
    for (const abi of abis) {
      const library = libraries[abi];
      const expected = hashes[abi];
      if (
        typeof library !== "string" ||
        typeof expected !== "string" ||
        !/^[a-f0-9]{64}$/u.test(expected)
      )
        throw new Error(
          `Android plugin ${name} (${abi}) requires a library and SHA-256 metadata.`,
        );
      const actual = createHash("sha256")
        .update(await readFile(resolve(directory, library)))
        .digest("hex");
      if (actual !== expected)
        throw new Error(
          `Android plugin ${name} (${abi}) SHA-256 metadata mismatch.`,
        );
    }
    catalogs.set(name, functions);
  }
  return catalogs;
};
