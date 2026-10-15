// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import {
  copyFile,
  mkdir,
  readFile,
  realpath,
  writeFile,
} from "node:fs/promises";
import { dirname, join, relative } from "node:path";

import {
  buildCandidateVariables,
  runScriptOnceToText,
  type FunCityLogEntry,
} from "funcity";

import { collectLinuxElfFiles } from "./linux-dependency-manifest.js";

/** A baseline file copied to the runtime with its source identity intact. */
export interface LinuxBundledSourceFile {
  /** Original file used to identify the source package and license. */
  path: string;
  /** Destination relative to the native runtime payload. */
  file: string;
}

/** A declared group of modules loaded dynamically by GTK or GIO. */
export interface LinuxBaselineModuleGroup {
  /** Directory relative to the baseline's architecture-specific library root. */
  directory: string;
  /** Optional module cache to relocate before runtime startup. */
  cache:
    | {
        /** Installed baseline cache. */
        source: string;
        /** Template path relative to the runtime payload. */
        destination: string;
        /** Library belonging to the package that owns the cache format. */
        owner: string;
      }
    | undefined;
}

/**
 * Stages declared dynamic modules and relocatable caches from a fixed baseline.
 * @param libraryRoot - Architecture-specific baseline library directory.
 * @param runtimeDirectory - Native runtime payload to populate.
 * @param groups - Explicit module groups supported by the baseline.
 * @returns Redistribution inputs and original ELF files to add to the ABI audit.
 */
export const stageLinuxBaselineModules = async (
  libraryRoot: string,
  runtimeDirectory: string,
  groups: readonly LinuxBaselineModuleGroup[],
): Promise<{ files: LinuxBundledSourceFile[]; binaries: string[] }> => {
  const files: LinuxBundledSourceFile[] = [];
  const binaries: string[] = [];
  for (const group of groups) {
    const modulePaths = await collectLinuxElfFiles(
      join(libraryRoot, group.directory),
    );
    for (const path of modulePaths) {
      // Module directories may also contain build-time query executables.
      if (!/\.so(?:\.[^/]+)*$/.test(path)) continue;
      const file = `linux/lib/${relative(libraryRoot, path)}`;
      await mkdir(dirname(join(runtimeDirectory, file)), { recursive: true });
      await copyFile(path, join(runtimeDirectory, file));
      const source = await realpath(path);
      files.push({ path: source, file });
      binaries.push(source);
    }
    if (group.cache !== undefined) {
      const logs: FunCityLogEntry[] = [];
      const text = await runScriptOnceToText(
        "{{replace pattern replacement cache}}",
        {
          sourceId: "muon-linux-module-cache",
          logs,
          variables: buildCandidateVariables({
            pattern: new RegExp(
              libraryRoot.replace(/[.*+?^${}()|[\]\\]/g, "\\$&"),
              "g",
            ),
            replacement: "@MUON_RUNTIME@/linux/lib",
            cache: await readFile(group.cache.source, "utf8"),
          }),
        },
      );
      if (text === undefined || logs.some((entry) => entry.type === "error"))
        throw new Error(
          `Cannot relocate Linux module cache: ${group.cache.source}`,
        );
      await mkdir(dirname(join(runtimeDirectory, group.cache.destination)), {
        recursive: true,
      });
      await writeFile(join(runtimeDirectory, group.cache.destination), text);
      files.push({ path: group.cache.owner, file: group.cache.destination });
    }
  }
  return { files, binaries };
};
