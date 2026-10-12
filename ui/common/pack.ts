// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { resolve } from "node:path";

import {
  loadMuonBuildSequenceProject,
  muonBuildSequenceSuppressViteBuildEnvironmentKey,
  resolveMuonViteBuildOptions,
} from "./build-sequence.js";

import { allMuonTargets } from "./targets.js";

import type { MuonPackOptions, MuonPackResult } from "./pack-types.js";
import { packDesktopMuonApp } from "../cef/pack.js";
import { packAndroidMuonApp } from "../android/pack.js";
export type * from "./pack-types.js";

export const muonPackSuppressViteBuildEnvironmentKey =
  muonBuildSequenceSuppressViteBuildEnvironmentKey;

/**
 * Builds redistributable desktop packages or a verified, signed Android APK.
 * @param options - Target, artifact, project and signing options.
 * @returns Built distributions and the package files to distribute.
 */
export const packMuonApp = async (
  options: MuonPackOptions,
): Promise<MuonPackResult> => {
  const cwd = resolve(options.root ?? process.cwd());
  const project = await loadMuonBuildSequenceProject(cwd, options.targets);
  const plugin = resolveMuonViteBuildOptions(project.pluginOptions);
  const targets = [
    ...new Set(
      (options.allTargets === true
        ? [...allMuonTargets]
        : (options.targets ?? plugin.targets ?? [])
      )
        .flatMap((target) => target.split(","))
        .map((target) => target.trim().toLowerCase()),
    ),
  ];
  const types = options.types
    ?.flatMap((type) => type.split(","))
    .map((type) => type.trim().toLowerCase());
  if (!targets.includes("android") && !types?.includes("apk"))
    return await packDesktopMuonApp(options, project);
  return await packAndroidMuonApp(options, project, targets, types);
};
