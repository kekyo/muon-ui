// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { mkdir, readFile, writeFile } from "node:fs/promises";
import { basename, join } from "node:path";
import type { MuonDesktopTarget } from "./targets.ts";
import type { MuonResolvedService } from "../common/service-config.ts";

/** Normalized runtime settings stored beside a desktop application. */
export interface MuonDesktopServiceSettings {
  /** Application default when no user override has been persisted. */
  readonly autostart: boolean;
  /** Immutable strings passed to this logical native instance. */
  readonly config: Readonly<Record<string, string>>;
}

/** Registry inputs selected for one runtime adapter. */
export type MuonDesktopServiceEntry = MuonDesktopServiceSettings &
  (
    | {
        /** Native factory adapter. */ readonly runtime: "native";
        /** Registry-relative shared library. */ readonly library: string;
      }
    | {
        /** JavaScript engine adapter. */ readonly runtime: "quickjs";
        /** Registry-relative bundled source. */ readonly script: string;
      }
  );

/**
 * Packages verified service libraries independently of page plugins.
 * @param definitions - Validated application declarations.
 * @param target - Desktop ABI to select.
 * @param output - Application distribution directory.
 * @returns Registry filename, or undefined when no services are declared.
 * @remarks Services share one loader namespace. Conflicting library filenames
 * are rejected; identical factories/dependencies are copied only once.
 */
export const stageMuonDesktopServices = async (
  definitions: readonly MuonResolvedService[],
  target: MuonDesktopTarget,
  output: string,
): Promise<string | undefined> => {
  if (definitions.length === 0) return undefined;
  const files = new Map<string, { name: string; bytes: Buffer }>();
  const services: Record<string, MuonDesktopServiceEntry> = {};
  const fold = (name: string): string =>
    target.startsWith("windows-") ? name.toLowerCase() : name;
  const reserved = new Set([
    "libcardio.so",
    "libcardio.dll",
    "cardio.dll",
    "libmuon-ui.so",
    "libmuon-ui.dll",
  ]);
  const scripts = new Map<string, string>();
  for (const definition of definitions) {
    if (definition.runtime === "quickjs") {
      const script = "./services/quickjs/" + definition.id + ".mjs";
      scripts.set(script, definition.bundle);
      services[definition.id] = {
        runtime: "quickjs",
        script,
        autostart: definition.autostart,
        config: definition.config,
      };
      continue;
    }
    const artifact = definition.artifact.targets[target];
    if (!artifact)
      throw new Error(
        "Service " + definition.id + " is missing target " + target,
      );
    for (const path of [artifact.library, ...artifact.dependencies]) {
      const name = basename(path);
      const key = fold(name);
      if (reserved.has(key))
        throw new Error("Service library name is reserved: " + name);
      const bytes = await readFile(path);
      const previous = files.get(key);
      if (previous && !previous.bytes.equals(bytes))
        throw new Error("Conflicting service library: " + name);
      if (!previous) files.set(key, { name, bytes });
    }
    services[definition.id] = {
      runtime: "native",
      library: "./services/native/" + basename(artifact.library),
      autostart: definition.autostart,
      config: definition.config,
    };
  }
  const directory = join(output, "services", "native");
  await mkdir(directory, { recursive: true });
  for (const { name, bytes } of files.values())
    await writeFile(join(directory, name), bytes, { mode: 0o755 });
  if (scripts.size) {
    await mkdir(join(output, "services", "quickjs"), { recursive: true });
    for (const [path, source] of scripts)
      await writeFile(join(output, path), source);
  }
  const registry = "muon-services.json";
  await writeFile(
    join(output, registry),
    JSON.stringify({ services }, null, 2) + "\n",
  );
  return registry;
};
