// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { createHash } from "node:crypto";
import { readFile } from "node:fs/promises";
import { createRequire, isBuiltin } from "node:module";
import { dirname, join } from "node:path";
import { runScriptOnceToText } from "funcity";
import type { buildService, MuonPluginTarget } from "muon-plugin-sdk";
import type { MuonServiceBuildOptions } from "./service-plugins.ts";
import {
  createMuonServiceInputPlugin,
  observeMuonNativeSource,
  type MuonServiceInputObserver,
} from "./service-inputs.ts";

/**
 * Builds an explicitly selected native factory with the application's SDK.
 * @param id - Validated logical service identity.
 * @param configPath - Absolute SDK source configuration path.
 * @param targets - Native targets required by the application.
 * @param sysroot - Explicit target sysroot, or undefined for the SDK default.
 * @param options - Application-owned output root and build environment.
 * @returns Absolute service artifact manifest path, ready for verification.
 * @remarks Builds never overwrite the producer's directory or a running host's
 * staged files. Changed factory configurations use a separate artifact directory.
 */
export const buildMuonNativeService = async (
  id: string,
  configPath: string,
  targets: readonly MuonPluginTarget[],
  sysroot: string | undefined,
  options: MuonServiceBuildOptions,
): Promise<string> => {
  options.onInput?.({ path: configPath, kind: "file" });
  const require = createRequire(join(options.root, "package.json"));
  let authoring: { buildService: typeof buildService };
  try {
    authoring = require("muon-plugin-sdk");
  } catch (error) {
    throw new Error(
      `services.${id} requires the authoring SDK. Run npm install -D muon-plugin-sdk in ${options.root}. ${error instanceof Error ? error.message : String(error)}`,
    );
  }
  const identity = createHash("sha256")
    .update(configPath)
    .update("\0")
    .update(await readFile(configPath))
    .digest("hex");
  const outputDirectory = join(
    options.root,
    ".muon/service-build",
    id,
    identity,
  );
  await observeMuonNativeSource(
    configPath,
    options.root,
    targets,
    outputDirectory,
    options.onInput,
  );
  for (const target of targets)
    await authoring.buildService({
      configPath,
      outputDirectory,
      target,
      environment: options.environment,
      ...(sysroot === undefined ? {} : { sysroot }),
      ...(options.androidSdkPath === undefined
        ? {}
        : { androidSdkPath: options.androidSdkPath }),
    });
  return join(outputDirectory, "muon-plugin.json");
};

/**
 * Bundles one declared TS/JS service with its portable runtime adapter.
 * @param entry - Absolute author entry exporting a service definition.
 * @param modules - Explicitly granted host module specifiers.
 * @param onInput - Optional observer for source inputs, including failed builds.
 * @param project - Directory containing the service package, or the entry directory by default.
 * @returns Self-contained ESM source, evaluated only by the service host.
 * @remarks Requires Vite 8 and the author's muon-plugin-sdk installation.
 * Node builtins and external chunks are rejected for the QuickJS runtime.
 */
export const buildMuonQuickJsService = async (
  entry: string,
  modules: readonly string[],
  onInput?: MuonServiceInputObserver,
  project = dirname(entry),
): Promise<string> => {
  const vite = await import("vite");
  if (Number(vite.version.split(".")[0]) < 8)
    throw new Error("QuickJS services require Vite 8 or newer.");
  const source = await runScriptOnceToText(
    `
import definition from {{entry}};
import { createServiceRuntime } from 'muon-plugin-sdk/services';
globalThis.__muonServiceInterface = definition.interface;
globalThis.__muonServiceRuntime = createServiceRuntime(definition, {
  io: globalThis.__muonServiceIo,
  callback: globalThis.__muonServiceCallback,
  emit: globalThis.__muonServiceEmit,
});
`,
    {
      sourceId: "muon-quickjs-service.fc",
      variables: new Map([
        ["entry", JSON.stringify(vite.normalizePath(entry))],
      ]),
    },
  );
  if (source === undefined)
    throw new Error("Unable to generate the QuickJS service entry.");
  const virtual = vite.normalizePath(join(project, ".muon-service-entry.ts"));
  const result = await vite.build({
    configFile: false,
    root: project,
    publicDir: false,
    logLevel: "warn",
    plugins: [
      createMuonServiceInputPlugin(entry, onInput),
      {
        name: "muon-quickjs-service",
        enforce: "pre",
        resolveId: (id, importer) => {
          if (modules.includes(id)) return { id, external: true };
          if (id.startsWith("muon:"))
            throw new Error(`Service is not allowed to import ${id}.`);
          if (
            isBuiltin(id) ||
            id.startsWith("node:") ||
            /\.node(?:\?|$)/iu.test(id)
          )
            throw new Error(
              `QuickJS services cannot import ${id} from ${importer ?? entry}.`,
            );
          if (vite.normalizePath(id) === virtual) return "\0" + virtual;
          return undefined;
        },
        load: (id) => {
          if (/\.node(?:\?|$)/iu.test(id))
            throw new Error(`QuickJS services cannot load native addon ${id}.`);
          return id === "\0" + virtual ? source : undefined;
        },
      },
    ],
    build: {
      write: false,
      target: "es2022",
      minify: false,
      modulePreload: false,
      lib: { entry: virtual, formats: ["es"] },
      rolldownOptions: {
        output: { codeSplitting: false },
        onwarn: (warning) => {
          throw new Error(`QuickJS service build: ${warning.message}`);
        },
      },
    },
  });
  const outputs = Array.isArray(result) ? result : [result];
  if (outputs.length !== 1 || !("output" in outputs[0]!))
    throw new Error(
      "QuickJS service build must produce one JavaScript bundle.",
    );
  const files = outputs[0]!.output;
  if (
    files.length !== 1 ||
    files[0]!.type !== "chunk" ||
    files[0]!.imports.some((id) => !modules.includes(id)) ||
    files[0]!.dynamicImports.length
  )
    throw new Error(
      "QuickJS service bundles cannot depend on external modules or assets.",
    );
  if (Buffer.byteLength(files[0]!.code, "utf8") > 8 * 1024 * 1024)
    throw new Error("QuickJS service bundles cannot exceed 8 MiB.");
  return files[0]!.code;
};
