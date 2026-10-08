// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import {
  buildMuonAndroidTarget,
  prepareMuonAndroidTarget,
} from "../android/target.js";

import { resolve } from "node:path";

import { type MuonDesktopTarget } from "./targets.js";

import type {
  InternalMuonBuildOptions,
  MuonBuildOptions,
  MuonDesktopBuildTargetResult,
  MuonBuildResult,
} from "./build-types.js";
import {
  resolvePackageDirectory,
  resolveBuildTargets,
  resolveAssetInput,
  readPackageJson,
  resolveAppName,
  readBuildConfig,
} from "./build-config.js";
import { buildDesktopMuonApp } from "../cef/build.js";
export type * from "./build-types.js";
export {
  getDefaultMuonBuildTarget,
  normalizeMuonBuildTarget,
} from "./build-config.js";
export { resolveMuonNodeProjectForBuildConfig } from "../cef/build.js";

/**
 * Builds the explicitly selected desktop and/or Android applications.
 * @param options - Project, assets, selected targets and platform configuration.
 * @returns Per-target results discriminated by target.
 */
export function buildMuonApp(
  options: Omit<MuonBuildOptions, "targets"> & {
    targets?: readonly MuonDesktopTarget[];
  },
): Promise<
  Omit<MuonBuildResult, "targets"> & { targets: MuonDesktopBuildTargetResult[] }
>;

export function buildMuonApp(
  options?: MuonBuildOptions,
): Promise<MuonBuildResult>;

export async function buildMuonApp(
  options: MuonBuildOptions = {},
): Promise<MuonBuildResult> {
  const targets = resolveBuildTargets(options);
  if (!targets.includes("android")) return await buildDesktopMuonApp(options);
  const root = resolve(options.root ?? process.cwd());
  const packageJson = await readPackageJson(root);
  const buildConfig = await readBuildConfig(root, options.configPath);
  const internal = options as InternalMuonBuildOptions;
  const android = await buildMuonAndroidTarget({
    root,
    packageDirectory: resolvePackageDirectory(options.packageDirectory),
    packageJson,
    config: buildConfig.config,
    configDirectory: buildConfig.directory,
    options,
    assets: resolveAssetInput(
      root,
      options.assetSourcePath,
      options.assetPrefix,
      buildConfig,
    ),
    environment: internal.environment ?? process.env,
    progress: internal.progress,
  });
  const desktop = targets.filter((target) => target !== "android");
  const result: MuonBuildResult =
    desktop.length === 0
      ? {
          root,
          appName: resolveAppName(packageJson, options.appName),
          appId: android.applicationId,
          targets: [],
        }
      : await buildDesktopMuonApp({
          ...options,
          targets: desktop,
          allTargets: false,
        });
  result.targets.push(android);
  return result;
}

/**
 * Validates and prepares the installed Android toolchain for a project.
 * @param options - Project and optional Android SDK override.
 * @returns Validated SDK/JDK paths and the packaged version requirements.
 */
export const prepareMuonAndroid = async (options: MuonBuildOptions = {}) => {
  const root = resolve(options.root ?? process.cwd());
  const config = await readBuildConfig(root, options.configPath);
  return await prepareMuonAndroidTarget(
    root,
    resolvePackageDirectory(options.packageDirectory),
    config.config,
    config.directory,
    options.android,
    (options as InternalMuonBuildOptions).environment ?? process.env,
  );
};
