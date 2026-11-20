// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { stat } from "node:fs/promises";
import { basename, dirname, isAbsolute, relative, resolve } from "node:path";
import { watch, type FSWatcher } from "chokidar";
import type { MuonServiceInput } from "../common/service-inputs.ts";

const inside = (parent: string, child: string): boolean => {
  const path = relative(parent, child);
  return (
    path !== ".." &&
    !path.startsWith("../") &&
    !path.startsWith("..\\") &&
    !isAbsolute(path)
  );
};

const nativeInput = (path: string): boolean =>
  /\.(?:c|cc|cpp|cxx|h|hh|hpp|hxx|inc|in|mk|cmake|sh|bat|cmd|json|def|rc)$/iu.test(
    path,
  ) || /^(?:GNUmakefile|[Mm]akefile|CMakeLists\.txt)$/u.test(basename(path));

const matches = (input: MuonServiceInput, path: string): boolean => {
  if (input.path === path) return true;
  if (input.kind === "file" || !inside(input.path, path)) return false;
  if (relative(input.path, path).split(/[\\/]/u).includes("node_modules"))
    return false;
  return input.kind === "tree" || nativeInput(path);
};

/** Independent input watcher, including installed packages ignored by Vite. */
export interface MuonDevelopmentServiceWatcher {
  /**
   * Replaces the watched inputs after scanning them, without a monitoring gap.
   * @param inputs - Current successful inputs, or their union with a failed build.
   * @returns Completion once the replacement watcher is ready and the previous one is closed.
   */
  readonly replace: (inputs: readonly MuonServiceInput[]) => Promise<void>;
  /** Closes every active or initializing watcher. */
  readonly close: () => Promise<void>;
}

/**
 * Watches service inputs separately from page HMR and generated staging files.
 * @param inputs - Initial service and configuration inputs.
 * @param changed - Receives changed source paths; the caller serializes rebuilds.
 * @param failed - Receives filesystem watch failures.
 * @returns Ready watcher with asynchronous replacement and cleanup.
 */
export const createMuonDevelopmentServiceWatcher = async (
  inputs: readonly MuonServiceInput[],
  changed: (path: string) => void,
  failed: (error: Error) => void,
): Promise<MuonDevelopmentServiceWatcher> => {
  let current: FSWatcher | undefined;
  let closed = false;
  const active = new Set<FSWatcher>();
  const finishScan = new Map<FSWatcher, () => void>();
  const replace = async (next: readonly MuonServiceInput[]): Promise<void> => {
    if (closed) return;
    const normalized = next.map((input) => ({
      ...input,
      path: resolve(input.path),
    }));
    const roots = new Set<string>();
    for (const input of normalized) {
      let path = input.path;
      // Watch the nearest existing directory so creating a missing chain of
      // directories/files repairs a failed import without touching its importer.
      for (;;) {
        try {
          if ((await stat(path)).isDirectory()) break;
        } catch (error) {
          if (
            !["ENOENT", "ENOTDIR"].includes(
              (error as NodeJS.ErrnoException).code ?? "",
            )
          )
            throw error;
        }
        const parent = dirname(path);
        if (parent === path) break;
        path = parent;
      }
      roots.add(path);
    }
    if (closed) return;
    // Scan overlapping directories once. Otherwise a second traversal can
    // report existing descendants as newly added service inputs.
    const directories = [...roots].filter(
      (path) =>
        ![...roots].some((parent) => parent !== path && inside(parent, path)),
    );
    const watcher = watch(directories, {
      ignoreInitial: true,
      ignored: (raw, stats) => {
        const path = resolve(raw);
        if (
          path
            .split(/[\\/]/u)
            .some((part) => part === ".muon" || part === ".git")
        )
          return true;
        if (normalized.some((input) => matches(input, path))) return false;
        if (stats?.isFile()) return true;
        return !normalized.some(
          (input) =>
            inside(path, input.path) ||
            (input.kind !== "file" &&
              inside(input.path, path) &&
              !relative(input.path, path)
                .split(/[\\/]/u)
                .includes("node_modules")),
        );
      },
    });
    active.add(watcher);
    watcher.on("all", (event, raw) => {
      const path = resolve(raw);
      if (
        !closed &&
        event !== "addDir" &&
        event !== "unlinkDir" &&
        normalized.some((input) => matches(input, path))
      )
        changed(path);
    });
    watcher.on("error", (error) =>
      failed(error instanceof Error ? error : new Error(String(error))),
    );
    try {
      await new Promise<void>((resolve, reject) => {
        finishScan.set(watcher, resolve);
        watcher.once("ready", resolve);
        watcher.once("error", reject);
      });
      finishScan.delete(watcher);
      if (closed) return;
      const previous = current;
      current = watcher;
      if (previous) {
        await previous.close();
        active.delete(previous);
      }
    } catch (error) {
      await watcher.close();
      active.delete(watcher);
      throw error;
    }
  };
  const close = async (): Promise<void> => {
    closed = true;
    for (const watcher of active) {
      await watcher.close();
      finishScan.get(watcher)?.();
    }
    active.clear();
  };
  try {
    await replace(inputs);
  } catch (error) {
    await close();
    throw error;
  }
  return { replace, close };
};
