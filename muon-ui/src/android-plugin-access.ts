// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import {
  expandMuonAndroidFunctionAllows,
  muonAndroidBuiltinFunctionPaths,
} from "../../muon-android/src/function-paths.js";
import { validateMuonAndroidConfig } from "../../muon-android/src/renderer/android-config.js";
import type { MuonRuntimePluginConfig } from "./capability.js";
import { readMuonPluginAccessOptions } from "./plugin-access.js";
import type { MuonAndroidPluginAccess } from "../../muon-android/src/build.js";

/**
 * Connects common plugin policies to Android library definitions.
 * @param config - Merged application configuration.
 * @param runtime - Vite-generated policy override, or undefined for a CLI build.
 * @param libraries - ABI-specific library definitions from Android settings.
 * @returns Package-owned exposure settings and native plugin inputs.
 */
export const resolveMuonAndroidPluginAccess = (
  config: Record<string, unknown>,
  runtime: MuonRuntimePluginConfig | undefined,
  libraries: unknown,
) => {
  const merged =
    runtime === undefined
      ? config
      : {
          ...config,
          plugin: {
            ...(config.plugin as object | undefined),
            ...runtime,
            // Runtime entries already contain the allowlist derived from imports.
            ...(runtime.mode === "validate"
              ? { mode: "simple", plugins: runtime.plugins ?? [] }
              : {}),
          },
        };
  const plugin = readMuonPluginAccessOptions(merged, "simple");
  if ((plugin.mode ?? "simple") !== "simple") {
    throw new Error(
      "Android plugin.mode validate requires Vite-generated capabilities from the application build.",
    );
  }
  validateMuonAndroidConfig(merged);
  const entries = plugin.plugins ?? [
    { name: "internal", allow: muonAndroidBuiltinFunctionPaths },
  ];
  const names = new Set<string>();
  for (const entry of entries) {
    if (names.has(entry.name))
      throw new Error(`Duplicate plugin.plugins name: ${entry.name}`);
    names.add(entry.name);
  }
  if (!Array.isArray(libraries))
    throw new Error("android.plugins must be an array.");
  const definitions = libraries.map((value: unknown) => {
    if (typeof value !== "object" || value === null || Array.isArray(value)) {
      throw new Error("android.plugins entries must be objects.");
    }
    const definition = value as Record<string, unknown>;
    if (
      !entries.some(
        (entry) => entry.name !== "internal" && entry.name === definition.name,
      )
    ) {
      throw new Error(
        `android.plugins entry ${String(definition.name)} requires a matching plugin.plugins policy.`,
      );
    }
    return definition;
  });
  const plugins = entries
    .filter((entry) => entry.name !== "internal")
    .map((entry) => {
      const matches = definitions.filter(
        (definition) => definition.name === entry.name,
      );
      if (matches.length !== 1)
        throw new Error(
          `plugin.plugins entry ${entry.name} requires exactly one android.plugins library definition.`,
        );
      return {
        ...matches[0],
        name: entry.name,
        allow: entry.allow,
        config: entry.config,
      };
    });
  const internal = entries.find((entry) => entry.name === "internal");
  const base = {
    enabled: plugin.pages === undefined || plugin.pages.length > 0,
    internalAllow: expandMuonAndroidFunctionAllows(internal?.allow ?? []),
  };
  const ids = new Set<string>();
  const pluginAccess: MuonAndroidPluginAccess =
    runtime?.mode === "validate"
      ? {
          ...base,
          mode: "validate",
          capabilities: runtime.capabilities.map((entry) => {
            if (!entry.id || ids.has(entry.id))
              throw new Error(
                "Android capability ids must be nonempty and unique.",
              );
            ids.add(entry.id);
            return {
              id: entry.id,
              allow: expandMuonAndroidFunctionAllows(entry.allow),
            };
          }),
        }
      : { ...base, mode: "simple" };
  return {
    pluginAccess,
    plugins,
  };
};
