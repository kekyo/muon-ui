// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { readFile } from "node:fs/promises";
import { join } from "node:path";
import type { MuonPluginTarget } from "muon-plugin-sdk";
import {
  inspectPluginBinary,
  validateServiceLibraryDependencies,
  type ResolvedPluginArtifact,
} from "muon-plugin-sdk/artifact";
import { selectMuonPluginFunctionPaths } from "./capability.ts";
import { readMuonPluginAccessOptions } from "./plugin-access.ts";
import { resolveMuonNativePlugins } from "./native-plugins.ts";
import type { MuonServiceInputObserver } from "./service-inputs.ts";

/** One explicit native capability, independent of every page's permissions. */
export interface MuonServicePlugin {
  /** Plugin registration identity. */ readonly name: string;
  /** Verified binaries and producer catalog. */ readonly artifact: ResolvedPluginArtifact;
  /** Concrete function paths allowed to this service. */ readonly allow: readonly string[];
  /** Immutable process-shared plugin settings. */ readonly config: Readonly<
    Record<string, string>
  >;
}

/** Build context for explicit service plugin references. */
export interface MuonServiceBuildOptions {
  /** Package root used to resolve the authoring SDK and NPM plugins. */ readonly root: string;
  /** Environment supplied to explicitly requested source builds. */ readonly environment: NodeJS.ProcessEnv;
  /** Android SDK selected by the application. */ readonly androidSdkPath?: string;
  /** Optional development observer, including inputs that fail validation. */ readonly onInput?: MuonServiceInputObserver;
}

const object = (value: unknown, label: string): Record<string, unknown> => {
  if (value === null || typeof value !== "object" || Array.isArray(value))
    throw new Error(`${label} must be an object.`);
  return value as Record<string, unknown>;
};

const sorted = (
  value: Readonly<Record<string, unknown>>,
): Record<string, unknown> =>
  Object.fromEntries(
    Object.entries(value).sort(([left], [right]) => left.localeCompare(right)),
  );

/**
 * Resolves the explicit native capabilities of each JavaScript service.
 * @param services - Parsed application service definitions.
 * @param directory - Configuration base directory.
 * @param targets - Actual native build targets.
 * @param options - Application package and compiler environment.
 * @returns Service-specific grants backed by a consistent process-wide artifact union.
 * @remarks Source outputs are separate from page plugins. Shared registrations
 * must agree on binaries and settings, even when their service permissions differ.
 */
export const resolveMuonServicePlugins = async (
  services: Record<string, unknown>,
  directory: string,
  targets: readonly MuonPluginTarget[],
  options: MuonServiceBuildOptions,
): Promise<ReadonlyMap<string, readonly MuonServicePlugin[]>> => {
  const result = new Map<string, readonly MuonServicePlugin[]>();
  const shared = new Map<string, string>();
  const cache = new Map<string, ResolvedPluginArtifact>();
  for (const [id, raw] of Object.entries(services)) {
    const service = object(raw, `services.${id}`);
    if (service.plugins === undefined) continue;
    const label = `services.${id}.plugins`;
    if (service.runtime !== "quickjs")
      throw new Error(`${label} requires the quickjs runtime.`);
    const declarations = Object.entries(object(service.plugins, label)).map(
      ([name, raw]) => {
        const value = object(raw, `${label}.${name}`);
        if (!/^[A-Za-z_][A-Za-z0-9_]*$/u.test(name) || name === "internal")
          throw new Error(
            `${label} contains an invalid native plugin name: ${name}`,
          );
        if (
          Object.keys(value).some(
            (key) => !["native", "allow", "config"].includes(key),
          ) ||
          value.native === undefined ||
          !Array.isArray(value.allow) ||
          !value.allow.length
        )
          throw new Error(
            `${label}.${name} requires native and a nonempty allow list; only native, allow and config are supported.`,
          );
        return { ...value, name };
      },
    );
    let parsed;
    try {
      parsed = readMuonPluginAccessOptions(
        { plugin: { plugins: declarations } },
        "simple",
      ).plugins!;
    } catch (error) {
      throw new Error(`${label}: ${String(error)}`);
    }
    const plugins: MuonServicePlugin[] = [];
    for (const declaration of parsed) {
      const key = JSON.stringify([
        declaration.name,
        sorted(declaration.native as Record<string, unknown>),
      ]);
      let artifact = cache.get(key);
      if (!artifact) {
        artifact = (
          await resolveMuonNativePlugins({
            ...options,
            directory,
            targets,
            plugins: [declaration],
            outputDirectory: join(options.root, ".muon/service-plugins", id),
          })
        ).get(declaration.name)!;
        for (const target of targets) {
          const binary = artifact.targets[target]!;
          const inspections = [{ path: binary.library, info: binary.binary }];
          for (const path of binary.dependencies)
            inspections.push({
              path,
              info: inspectPluginBinary(await readFile(path), target),
            });
          validateServiceLibraryDependencies(inspections, target);
        }
        cache.set(key, artifact);
      }
      const config = declaration.config ?? {};
      const identity = JSON.stringify({
        config: sorted(config),
        functions: [...artifact.manifest.functions].sort(),
        targets: targets.map((target) => {
          const entry = artifact.manifest.targets[target]!;
          return [target, entry.library.sha256, entry.dependencies];
        }),
      });
      const previous = shared.get(declaration.name);
      if (previous !== undefined && previous !== identity)
        throw new Error(
          `Conflicting service plugin artifact or configuration: ${declaration.name}`,
        );
      shared.set(declaration.name, identity);
      const allow = selectMuonPluginFunctionPaths(
        artifact.manifest.functions,
        declaration.allow!,
      );
      if (!allow.length)
        throw new Error(
          `Service plugin ${declaration.name} has no allowed functions in its producer catalog.`,
        );
      plugins.push({ name: declaration.name, artifact, allow, config });
    }
    result.set(id, plugins);
  }
  return result;
};

/**
 * Lists module specifiers derived from explicitly granted producer functions.
 * @param plugins - Capabilities of one service.
 * @returns Unique host module names; function exports remain enforced at runtime.
 */
export const getMuonServicePluginModules = (
  plugins: readonly MuonServicePlugin[],
): readonly string[] => [
  ...new Set(
    plugins.flatMap((plugin) =>
      plugin.allow.map((path) =>
        path.slice(0, path.lastIndexOf(".")).replace(".", ":"),
      ),
    ),
  ),
];

/**
 * Coalesces already validated per-service capabilities for one process loader.
 * @param plugins - Separate service capability lists.
 * @returns Unique plugin entries with the union of their concrete function grants.
 */
export const mergeMuonServicePlugins = (
  plugins: readonly (readonly MuonServicePlugin[])[],
): readonly MuonServicePlugin[] => {
  const result = new Map<string, MuonServicePlugin>();
  for (const entries of plugins)
    for (const plugin of entries) {
      const previous = result.get(plugin.name);
      result.set(plugin.name, {
        ...plugin,
        allow: [...new Set([...(previous?.allow ?? []), ...plugin.allow])],
      });
    }
  return [...result.values()];
};
