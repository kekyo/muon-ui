// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { dirname } from "node:path";
import type { MuonAndroidOptions, MuonAndroidAbi } from "../android.js";
import { readAndroidPluginMetadata } from "./plugin-metadata.js";
import {
  expandMuonAndroidFunctionAllows,
  muonAndroidBuiltinFunctionPaths,
} from "../../core/android/function-paths.js";
import {
  resolveMuonConfigPath,
  readJsonObjectFile,
  type MuonResolvedPluginAccessOptions,
} from "../common/plugin-access.js";

/**
 * Resolves import permissions against producer catalogs for an Android Vite build.
 * @param root - Vite project root.
 * @param configPath - Application configuration override.
 * @param options - Android build options supplied by Vite.
 * @param access - Validated common plugin configuration.
 * @returns Concrete per-plugin import rules and their public function catalog.
 */
export const collectMuonAndroidPluginAccess = async (
  root: string,
  configPath: string | undefined,
  options: MuonAndroidOptions | undefined,
  access: MuonResolvedPluginAccessOptions,
) => {
  const path = await resolveMuonConfigPath(root, configPath);
  const config =
    path === undefined
      ? {}
      : await readJsonObjectFile(path, "muon config file");
  const android = (config.android ?? {}) as MuonAndroidOptions;
  const abis = options?.abis ?? android.abis ?? ["arm64-v8a", "x86_64"];
  if (
    abis.length === 0 ||
    abis.some((abi) => abi !== "arm64-v8a" && abi !== "x86_64")
  )
    throw new Error("android.abis must select arm64-v8a or x86_64.");
  const libraries = options?.plugins ?? android.plugins ?? [];
  const catalogs = await readAndroidPluginMetadata(
    libraries,
    abis as readonly MuonAndroidAbi[],
    options?.plugins === undefined && path !== undefined ? dirname(path) : root,
    true,
  );
  const imports = access.plugins.flatMap((plugin) => {
    const catalog =
      plugin.name === "internal"
        ? muonAndroidBuiltinFunctionPaths
        : catalogs.get(plugin.name);
    if (catalog === undefined)
      throw new Error(
        `Android plugin ${plugin.name} requires a matching android.plugins metadata entry.`,
      );
    return (plugin.imports ?? []).map((rule) => ({
      ...rule,
      allow: expandMuonAndroidFunctionAllows(rule.allow ?? [], catalog),
    }));
  });
  return { imports, functionPaths: [...catalogs.values()].flat() };
};
