// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { readFile } from "node:fs/promises";
import { createRequire } from "node:module";
import { dirname, isAbsolute, join, resolve } from "node:path";
import { runScriptOnceToText } from "funcity";
import type { Plugin } from "vite";
import type { getPluginTarget, MuonPluginTarget } from "muon-plugin-sdk";
import type {
  ResolvedPluginArtifact,
  ResolvedServiceArtifact,
} from "muon-plugin-sdk/artifact";

/** One source input observed while resolving or staging services. */
export interface MuonServiceInput {
  /** Absolute input path, including a missing file that can repair a build. */
  readonly path: string;
  /** Exact file, project/package tree, or native source/build files in a tree. */
  readonly kind: "file" | "tree" | "native";
}

/**
 * Reports an input before reading it so failed builds remain repairable.
 * @param input - Source file or source tree; generated .muon outputs are excluded by watchers.
 */
export type MuonServiceInputObserver = (input: MuonServiceInput) => void;

/**
 * Records the producer manifest and selected runtime binary dependencies.
 * @param artifact - Verified service or service-plugin producer output.
 * @param targets - Native binaries used by this build.
 * @param observe - Optional development input collector.
 */
export const observeMuonServiceArtifact = (
  artifact: ResolvedPluginArtifact | ResolvedServiceArtifact,
  targets: readonly MuonPluginTarget[],
  observe: MuonServiceInputObserver | undefined,
): void => {
  if (!observe) return;
  observe({ path: artifact.path, kind: "file" });
  for (const target of targets) {
    const binary = artifact.targets[target];
    if (!binary) continue;
    for (const path of [binary.library, ...binary.dependencies])
      observe({ path, kind: "file" });
  }
};

/**
 * Records an SDK native project's declared inputs and compiler support files.
 * @param configPath - Absolute source build configuration.
 * @param root - Consumer root used to locate its installed authoring SDK.
 * @param targets - Native targets whose declared dependencies are required.
 * @param outputDirectory - Authoring output root used to expand build variables.
 * @param observe - Optional development input collector.
 * @returns Completion after explicit source/header paths and build directories are recorded.
 */
export const observeMuonNativeSource = async (
  configPath: string,
  root: string,
  targets: readonly MuonPluginTarget[],
  outputDirectory: string,
  observe: MuonServiceInputObserver | undefined,
): Promise<void> => {
  if (!observe) return;
  observe({ path: configPath, kind: "file" });
  const directory = dirname(configPath);
  observe({ path: directory, kind: "native" });
  const value = JSON.parse(await readFile(configPath, "utf8"));
  for (const field of ["sources", "headers", "types", "licenses"]) {
    if (!Array.isArray(value[field])) continue;
    for (const path of value[field])
      if (typeof path === "string") {
        const absolute = resolve(directory, path);
        observe({ path: absolute, kind: "file" });
        if (field === "sources" || field === "headers")
          observe({ path: dirname(absolute), kind: "native" });
      }
  }
  const workingDirectory = resolve(directory, value.build?.directory ?? ".");
  observe({ path: workingDirectory, kind: "native" });
  for (const tool of [
    ...(value.build?.tools ?? []),
    ...(value.build?.commands ?? []).map(
      (item: { command: string }) => item.command,
    ),
  ])
    if (typeof tool === "string" && /[\\/]/u.test(tool))
      observe({ path: resolve(workingDirectory, tool), kind: "file" });
  const require = createRequire(join(root, "package.json"));
  const sdk = resolve(dirname(require.resolve("muon-plugin-sdk")), "..");
  for (const name of ["include", "make", "cmake", "native"])
    observe({ path: join(sdk, name), kind: "tree" });
  const authoring: {
    getPluginTarget: typeof getPluginTarget;
  } = require("muon-plugin-sdk");
  for (const target of targets) {
    const descriptor = authoring.getPluginTarget(target);
    const build = join(outputDirectory, ".build", target);
    const output = join(
      build,
      (descriptor.os === "android" ? "lib" : "") +
        value.name +
        descriptor.extension,
    );
    for (const path of [
      ...(value.dependencies?.[target] ?? []),
      ...(value.linkLibraries?.[target] ?? []),
    ]) {
      const rendered = await runScriptOnceToText(path, {
        sourceId: configPath,
        variables: new Map<string, unknown>([
          ["source", directory],
          ["build", build],
          ["output", output],
          ["sdk", sdk],
          ["target", target],
        ]),
      });
      if (rendered === undefined)
        throw new Error(`Invalid native dependency path: ${path}`);
      observe({ path: resolve(directory, rendered), kind: "file" });
    }
  }
};

/**
 * Captures JavaScript module inputs, including unresolved relative imports.
 * @param entry - Absolute author entry.
 * @param observe - Optional development input collector.
 * @returns A passive Vite plugin; it does not resolve or transform author modules.
 */
export const createMuonServiceInputPlugin = (
  entry: string,
  observe: MuonServiceInputObserver | undefined,
): Plugin => {
  const file = (path: string): void => {
    if (isAbsolute(path))
      observe?.({ path: path.split("?")[0]!, kind: "file" });
  };
  file(entry);
  for (let path = dirname(entry); ; path = dirname(path)) {
    file(join(path, "package.json"));
    file(join(path, "tsconfig.json"));
    if (path === dirname(path)) break;
  }
  return {
    name: "muon-service-inputs",
    enforce: "pre",
    resolveId: (id, importer) => {
      if (observe && id.startsWith(".") && importer && isAbsolute(importer)) {
        const path = resolve(dirname(importer), id);
        for (const extension of [
          "",
          ".ts",
          ".tsx",
          ".js",
          ".jsx",
          ".mjs",
          ".mts",
          ".json",
          "/index.ts",
          "/index.js",
        ])
          file(path + extension);
      }
      return null;
    },
    load: (id) => {
      file(id);
      return null;
    },
  };
};
