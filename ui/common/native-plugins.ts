// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { copyFile, mkdir, readFile } from "node:fs/promises";
import { createRequire } from "node:module";
import { basename, join, resolve } from "node:path";
import {
  readPluginArtifact,
  type ResolvedPluginArtifact,
} from "muon-plugin-sdk/artifact";
import type { MuonPluginTarget, buildPlugin } from "muon-plugin-sdk";
import type { MuonPluginAccessEntryOptions } from "./plugin-access.js";

/** An explicit authoring source, common artifact, or prebuilt NPM package. */
export interface MuonNativePluginReference {
  /** Source configuration, relative to muon.json or the Vite project root. */ readonly source?: string;
  /** Common muon-plugin.json path, relative to the same base directory. */ readonly manifest?: string;
  /** NPM package exporting ./muon-plugin.json; never compiled by the consumer. */ readonly package?: string;
  /** Target development sysroot, used only for a source build. */ readonly sysroot?: string;
  /** Android SDK for a source build; defaults to android.sdkPath or ANDROID_HOME. */ readonly androidSdkPath?: string;
}

/** Inputs shared by CLI and Vite plugin resolution. */
export interface ResolveMuonNativePluginsOptions {
  /** Application package root. */ readonly root: string;
  /** Base directory of the configuration supplying references. */ readonly directory: string;
  /** Application-owned plugin entries and policies. */ readonly plugins: readonly MuonPluginAccessEntryOptions[];
  /** Selected native targets, independent of the build host. */ readonly targets: readonly MuonPluginTarget[];
  /** Environment for explicitly requested source builds. */ readonly environment: NodeJS.ProcessEnv;
  /** Android SDK selected by the application. */ readonly androidSdkPath?: string;
}

/**
 * Resolves explicit plugin inputs and finishes source builds before reading catalogs.
 * @param options - Application references and selected targets.
 * @returns Verified artifacts indexed by their runtime registration name.
 * @remarks Prebuilt paths only read files and do not load the authoring SDK or binaries.
 */
export const resolveMuonNativePlugins = async (
  options: ResolveMuonNativePluginsOptions,
): Promise<ReadonlyMap<string, ResolvedPluginArtifact>> => {
  const artifacts = new Map<string, ResolvedPluginArtifact>();
  const require = createRequire(join(options.root, "package.json"));
  for (const plugin of options.plugins) {
    const reference = plugin.native;
    if (!reference) continue;
    const selectors = [
      reference.source,
      reference.manifest,
      reference.package,
    ].filter((value) => value !== undefined);
    if (
      selectors.length !== 1 ||
      selectors.some(
        (value) => typeof value !== "string" || !value || value.includes("\0"),
      ) ||
      plugin.name === "internal"
    )
      throw new Error(
        `Plugin ${plugin.name} requires exactly one native.source, native.manifest or native.package.`,
      );
    if (artifacts.has(plugin.name))
      throw new Error(`Duplicate native plugin name: ${plugin.name}`);
    let path: string;
    if (reference.source !== undefined) {
      let authoring: { buildPlugin: typeof buildPlugin };
      try {
        authoring = require("muon-plugin-sdk");
      } catch (error) {
        throw new Error(
          `Plugin ${plugin.name} requires the authoring SDK. Run npm install -D muon-plugin-sdk in ${options.root}. ${error instanceof Error ? error.message : String(error)}`,
        );
      }
      const output = join(options.root, ".muon/plugins", plugin.name);
      if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(plugin.name))
        throw new Error(`Invalid native plugin name: ${plugin.name}`);
      const androidSdkPath =
        reference.androidSdkPath === undefined
          ? options.androidSdkPath
          : resolve(options.directory, reference.androidSdkPath);
      for (const target of options.targets)
        await authoring.buildPlugin({
          configPath: resolve(options.directory, reference.source),
          outputDirectory: output,
          target,
          environment: options.environment,
          ...(reference.sysroot === undefined
            ? {}
            : { sysroot: resolve(options.directory, reference.sysroot) }),
          ...(androidSdkPath === undefined ? {} : { androidSdkPath }),
        });
      path = join(output, "muon-plugin.json");
    } else if (reference.package !== undefined) {
      if (
        !/^(?:@[a-z0-9._-]+\/)?[a-z0-9._-]+$/.test(reference.package) ||
        reference.package.startsWith(".")
      )
        throw new Error(
          `Invalid native plugin NPM package: ${reference.package}`,
        );
      try {
        path = require.resolve(`${reference.package}/muon-plugin.json`);
      } catch (error) {
        throw new Error(
          `Cannot resolve ${reference.package}/muon-plugin.json. Install the plugin using npm install ${reference.package}; its package must export ./muon-plugin.json. ${error instanceof Error ? error.message : String(error)}`,
        );
      }
    } else path = resolve(options.directory, reference.manifest!);
    const artifact = await readPluginArtifact(path, options.targets);
    if (artifact.manifest.name !== plugin.name)
      throw new Error(
        `Plugin registration name mismatch: requested ${plugin.name}, artifact declares ${artifact.manifest.name}.`,
      );
    artifacts.set(plugin.name, artifact);
  }
  const functions = new Set<string>();
  for (const artifact of artifacts.values())
    for (const path of artifact.manifest.functions) {
      if (functions.has(path))
        throw new Error(`Duplicate native plugin function: ${path}`);
      functions.add(path);
    }
  return artifacts;
};

/**
 * Copies verified runtime files into an application-owned loader directory.
 * @param artifacts - Resolved prebuilt or locally built plugins.
 * @param target - Application output target.
 * @param directory - Runtime loader directory.
 * @returns Absolute entry-library paths by registration name.
 */
export const stageMuonNativePlugins = async (
  artifacts: ReadonlyMap<string, ResolvedPluginArtifact>,
  target: MuonPluginTarget,
  directory: string,
): Promise<ReadonlyMap<string, string>> => {
  const result = new Map<string, string>();
  const files = new Map<string, { source: string; destination: string }>();
  const add = async (source: string, filename: string): Promise<void> => {
    const key = target.startsWith("windows-")
      ? filename.toLowerCase()
      : filename;
    const existing = files.get(key);
    if (
      existing &&
      !(await readFile(existing.source)).equals(await readFile(source))
    )
      throw new Error(`Conflicting native library: ${filename}`);
    files.set(key, { source, destination: join(directory, filename) });
  };
  for (const [name, artifact] of artifacts) {
    const entry = artifact.targets[target];
    if (!entry) throw new Error(`Plugin ${name} does not support ${target}.`);
    const filename = target.startsWith("android-")
      ? basename(entry.library)
      : name + (target.startsWith("windows-") ? ".dll" : ".so");
    await add(entry.library, filename);
    for (const dependency of entry.dependencies)
      await add(dependency, basename(dependency));
    result.set(name, join(directory, filename));
  }
  await mkdir(directory, { recursive: true });
  for (const file of files.values())
    if (resolve(file.source) !== resolve(file.destination))
      await copyFile(file.source, file.destination);
  return result;
};
