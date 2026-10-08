// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { copyFile, mkdir, writeFile } from "node:fs/promises";
import { basename, join, resolve } from "node:path";

import {
  runMuonBuildSequence,
  type MuonBuildSequenceProject,
} from "../common/build-sequence.js";

import type { MuonPackOptions, MuonPackResult } from "../common/pack-types.js";
import {
  defaultArtifactsDirectory,
  readPackageJson,
  resolveMetadata,
} from "../common/pack-metadata.js";
/**
 * Packages a verified release APK for an explicitly selected Android target.
 * @param options - Project metadata, signing and artifact output options.
 * @param project - Resolved project and Vite build settings.
 * @param targets - Normalized targets requested by the caller.
 * @param types - Requested artifact types, or undefined for APK output.
 * @returns The signed application and its distributable APK artifact.
 */
export const packAndroidMuonApp = async (
  options: MuonPackOptions,
  project: MuonBuildSequenceProject,
  targets: readonly string[],
  types: readonly string[] | undefined,
): Promise<MuonPackResult> => {
  const cwd = resolve(options.root ?? process.cwd());
  if (
    options.allTargets === true ||
    targets.length !== 1 ||
    targets[0] !== "android"
  )
    throw new Error("APK packaging requires only --target android.");
  if (
    types !== undefined &&
    (types.length === 0 || types.some((type) => type !== "apk"))
  )
    throw new Error("Android supports --type apk only.");
  const root = project.root;
  const packageJson = await readPackageJson(root);
  const metadata = resolveMetadata(packageJson, options);
  const build = await runMuonBuildSequence(
    {
      ...options,
      root: cwd,
      targets: ["android"],
      allTargets: false,
      androidRelease: true,
      ...(options.packageVersion === undefined
        ? {}
        : {
            android: {
              ...options.android,
              versionName:
                options.android?.versionName ?? options.packageVersion,
            },
          }),
    },
    project,
  );
  const target = build.targets[0];
  if (target?.target !== "android" || target.signing !== "release")
    throw new Error("Android packaging did not produce a signed release APK.");
  const directory = resolve(
    root,
    options.artifactsDir ?? defaultArtifactsDirectory,
    "apk",
  );
  await mkdir(directory, { recursive: true });
  const path = join(directory, basename(target.packagePath));
  await copyFile(target.packagePath, path);
  await writeFile(
    path + ".json",
    JSON.stringify({ ...target, packagePath: path }, null, 2) + "\n",
  );
  return {
    root,
    packageName: metadata.packageName,
    version: target.versionName,
    appName: build.appName,
    appId: target.applicationId,
    build,
    targets: build.targets,
    artifacts: [{ type: "apk", target: "android", path }],
  };
};
