// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { readFile } from "node:fs/promises";
import { basename, resolve } from "node:path";
import {
  inspectPluginBinary,
  validatePluginLibraryDependencies,
} from "muon-plugin-sdk/artifact";
import type { MuonAndroidAbi, MuonAndroidPackagedPlugin } from "./build.js";

const runtimeLibraries = new Set([
  "libcardio.so",
  "libc++_shared.so",
  "libmuon_android_rpc.so",
]);
const record = (value: unknown, label: string): Record<string, unknown> => {
  if (value === null || typeof value !== "object" || Array.isArray(value))
    throw new Error(`${label} must be an object.`);
  return value as Record<string, unknown>;
};
const text = (value: unknown, label: string): string => {
  if (typeof value !== "string" || !value || value.includes("\0"))
    throw new Error(`${label} must be a nonempty string without NUL.`);
  return value;
};

// Validate ELF inputs without loading them or requiring an Android NDK.
/**
 * Resolves application-owned prebuilt plugins and validates their Android ELF inputs.
 * @param input - Parsed android.plugins value.
 * @param abis - Selected APK ABIs.
 * @param directory - Base directory of the configuration supplying the plugins.
 * @returns Runtime registry entries with absolute library paths.
 */
export const resolveAndroidPlugins = async (
  input: unknown,
  abis: readonly MuonAndroidAbi[],
  directory: string,
): Promise<MuonAndroidPackagedPlugin[]> => {
  if (!Array.isArray(input))
    throw new Error("android.plugins must be an array.");
  const names = new Set<string>();
  const sonames = new Set<string>();
  const result = input.map((value) => {
    const plugin = record(value, "android.plugins entry");
    const name = text(plugin.name, "plugin.name");
    const soname = text(plugin.soname, "plugin.soname");
    if (
      !/^[A-Za-z_][A-Za-z0-9_]*$/u.test(name) ||
      name === "internal" ||
      names.has(name)
    )
      throw new Error("Android plugin names must be valid and unique.");
    if (!/^lib[A-Za-z0-9_]+\.so$/u.test(soname) || sonames.has(soname))
      throw new Error("Android plugin sonames must be valid and unique.");
    if (runtimeLibraries.has(soname))
      throw new Error(`Android library name is reserved: ${soname}`);
    names.add(name);
    sonames.add(soname);
    if (!Array.isArray(plugin.allow) || !plugin.allow.length)
      throw new Error(
        `Android plugin ${name} requires a nonempty allow policy.`,
      );
    const allow = plugin.allow.map((value) => text(value, "plugin.allow"));
    if (allow.some((value) => !/^[A-Za-z0-9_.$*]+$/u.test(value)))
      throw new Error(
        "Android plugin.allow contains an invalid function pattern.",
      );
    const rawLibraries = record(plugin.libraries, "plugin.libraries");
    const libraries: Partial<Record<MuonAndroidAbi, string>> = {};
    for (const abi of abis)
      libraries[abi] = resolve(
        directory,
        text(rawLibraries[abi], `plugin.libraries.${abi}`),
      );
    const config = Object.entries(
      record(plugin.config ?? {}, "plugin.config"),
    ).map(([key, value]) => {
      if (
        !key ||
        key.includes("\0") ||
        typeof value !== "string" ||
        value.includes("\0")
      )
        throw new Error(
          "Android plugin.config must contain string keys and values without NUL.",
        );
      return { key, value };
    });
    if (
      plugin.expectedFunctions !== undefined &&
      (!Array.isArray(plugin.expectedFunctions) ||
        !plugin.expectedFunctions.every((path) => typeof path === "string"))
    )
      throw new Error(`Invalid expected functions for Android plugin ${name}.`);
    const dependencies: Partial<Record<MuonAndroidAbi, readonly string[]>> = {};
    if (plugin.dependencies !== undefined) {
      const raw = record(plugin.dependencies, "plugin.dependencies");
      for (const abi of abis) {
        const paths = raw[abi] ?? [];
        if (!Array.isArray(paths))
          throw new Error("plugin.dependencies entries must be path arrays.");
        dependencies[abi] = paths.map((path) =>
          resolve(directory, text(path, "plugin.dependencies path")),
        );
      }
    }
    return {
      ...(plugin.dependencies === undefined ? {} : { dependencies }),
      name,
      soname,
      allow,
      libraries,
      config,
      ...(plugin.expectedFunctions === undefined
        ? {}
        : { expectedFunctions: plugin.expectedFunctions as string[] }),
    };
  });
  for (const abi of abis) {
    const files = new Map<
      string,
      { path: string; data: Buffer; plugin: boolean }
    >();
    for (const plugin of result) {
      const paths = [
        plugin.libraries[abi]!,
        ...(plugin.dependencies?.[abi] ?? []),
      ];
      for (let index = 0; index < paths.length; index++) {
        const path = paths[index]!;
        const name = index === 0 ? plugin.soname : basename(path);
        if (!/^lib[A-Za-z0-9_.+-]+\.so$/u.test(name))
          throw new Error("Invalid Android dependency filename: " + name);
        const data = await readFile(path);
        const previous = files.get(name);
        if (previous && !previous.data.equals(data))
          throw new Error("Conflicting Android library: " + name);
        files.set(name, {
          path,
          data,
          plugin: index === 0 || previous?.plugin === true,
        });
      }
    }
    const inspected = [...files.entries()].map(([name, file]) => {
      const info = inspectPluginBinary(file.data, `android-${abi}`);
      if (info.soname !== name)
        throw new Error(name + " ELF SONAME must match its packaged name.");
      if (file.plugin && !info.exports.includes("muon_init_plugin"))
        throw new Error(name + " must export muon_init_plugin.");
      return { path: name, info };
    });
    validatePluginLibraryDependencies(inspected, `android-${abi}`);
  }
  return result;
};
