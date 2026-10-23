// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile } from "node:child_process";
import {
  copyFile,
  mkdir,
  readFile,
  realpath,
  writeFile,
} from "node:fs/promises";
import { dirname, join, relative } from "node:path";
import { promisify } from "node:util";

import {
  buildCandidateVariables,
  runScriptOnceToText,
  type FunCityLogEntry,
} from "funcity";

import { collectLinuxElfFiles } from "./linux-dependency-manifest.js";

const execute = promisify(execFile);

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
 * Stages current GTK modules and supported external input methods.
 * @param gtkPrefix - Private upstream GTK installation.
 * @param baselineLibraryRoot - Distribution library directory for external input methods.
 * @param runtimeDirectory - Native payload to populate.
 * @returns Source identities and module binaries for the dependency audit.
 */
export const stageLinuxGtkModules = async (
  gtkPrefix: string,
  baselineLibraryRoot: string,
  runtimeDirectory: string,
): Promise<{ files: LinuxBundledSourceFile[]; binaries: string[] }> => {
  const gtkLibraryRoot = join(gtkPrefix, "lib");
  const staged = await stageLinuxBaselineModules(
    gtkLibraryRoot,
    runtimeDirectory,
    [{ directory: "gtk-3.0", cache: undefined }],
  );
  const inputDirectory = "gtk-3.0/3.0.0/immodules";
  // Only these independently packaged modules survive the GTK replacement.
  // Copying the whole system GTK directory would reintroduce old GTK modules.
  for (const name of ["im-ibus.so", "im-fcitx.so", "im-fcitx5.so"]) {
    const file = `linux/lib/${inputDirectory}/${name}`;
    if (staged.files.some((entry) => entry.file === file))
      throw new Error(`Duplicate GTK input method: ${name}`);
    const path = await realpath(
      join(baselineLibraryRoot, inputDirectory, name),
    );
    await copyFile(path, join(runtimeDirectory, file));
    staged.files.push({ path, file });
    staged.binaries.push(path);
  }
  const { stdout } = await execute(
    "./gtk-query-immodules-3.0",
    staged.files
      .filter(({ file }) => file.startsWith(`linux/lib/${inputDirectory}/`))
      .map(({ path }) => path),
    { cwd: join(gtkPrefix, "bin") },
  );
  // Fcitx5 also advertises the legacy "fcitx" alias. Keep that identifier
  // assigned to Fcitx4 and retain Fcitx5's explicit "fcitx5" registration.
  const registrationLogs: FunCityLogEntry[] = [];
  let cache = await runScriptOnceToText(
    "{{replace pattern replacement cache}}",
    {
      sourceId: "muon-gtk-input-identifiers",
      logs: registrationLogs,
      variables: buildCandidateVariables({
        pattern: /(^"[^"\n]*\/im-fcitx5\.so"[^\n]*\n)"fcitx"[^\n]*\n/gm,
        replacement: "$1",
        cache: stdout,
      }),
    },
  );
  if (
    cache === undefined ||
    registrationLogs.some((entry) => entry.type === "error")
  )
    throw new Error("Cannot register GTK input method identifiers");
  for (const root of [gtkLibraryRoot, baselineLibraryRoot]) {
    const logs: FunCityLogEntry[] = [];
    const relocated = await runScriptOnceToText(
      "{{replace pattern replacement cache}}",
      {
        sourceId: "muon-gtk-input-cache",
        logs,
        variables: buildCandidateVariables({
          pattern: new RegExp(root.replace(/[.*+?^${}()|[\]\\]/g, "\\$&"), "g"),
          replacement: "@MUON_RUNTIME@/linux/lib",
          cache,
        }),
      },
    );
    if (relocated === undefined || logs.some((entry) => entry.type === "error"))
      throw new Error("Cannot relocate GTK input method cache");
    cache = relocated;
  }
  const file = "linux/gtk-immodules.cache.in";
  await writeFile(join(runtimeDirectory, file), cache);
  staged.files.push({ path: join(gtkLibraryRoot, "libgtk-3.so.0"), file });
  return staged;
};

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
