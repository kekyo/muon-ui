// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { mkdtemp, mkdir, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { afterEach, expect, it } from "vitest";
import { buildPlugin, createPluginProject } from "muon-plugin-sdk";
import {
  resolveMuonNativePlugins,
  stageMuonNativePlugins,
} from "../common/native-plugins.js";
import { resolveMuonPluginAccessOptions } from "../common/plugin-access.js";
import { collectMuonPluginFunctionPathsForAccess } from "../cef/plugin-inspector.js";
import { build as viteBuild } from "vite";
import { pathToFileURL } from "node:url";
import { vi } from "vitest";
import muon from "../common/vite.js";

const directories: string[] = [];
afterEach(async () => {
  vi.unstubAllEnvs();
  vi.unstubAllGlobals();
  for (const path of directories.splice(0))
    await rm(path, { recursive: true, force: true });
});

it("uses an NPM producer catalog and stages a cross-target DLL without running a compiler or inspector", async () => {
  const root = await mkdtemp(join(tmpdir(), "muon-native-package-"));
  directories.push(root);
  const source = join(root, "source");
  await createPluginProject({
    directory: source,
    name: "example",
    language: "c",
  });
  const packaged = join(root, "node_modules/example-native");
  await buildPlugin({
    configPath: join(source, "muon-plugin.build.json"),
    target: "windows-amd64",
    outputDirectory: packaged,
  });
  await writeFile(
    join(packaged, "package.json"),
    JSON.stringify({
      name: "example-native",
      version: "1.0.0",
      exports: { "./muon-plugin.json": "./muon-plugin.json" },
      types: "./types/index.d.ts",
    }),
  );
  await writeFile(
    join(root, "muon.json"),
    JSON.stringify({
      plugin: {
        mode: "validate",
        plugins: [
          {
            name: "example",
            native: { package: "example-native" },
            imports: [{ sources: ["src/**"], allow: ["muon.example.*"] }],
          },
        ],
      },
    }),
  );
  const access = await resolveMuonPluginAccessOptions({
    root,
    configPath: undefined,
    pluginAccess: undefined,
  });
  const artifacts = await resolveMuonNativePlugins({
    root,
    directory: root,
    plugins: access.plugins,
    targets: ["windows-amd64"],
    environment: { PATH: "", MUON_PLUGIN_INSPECTOR_PATH: "/missing" },
  });
  expect(
    await collectMuonPluginFunctionPathsForAccess(
      { ...access, nativeArtifacts: artifacts },
      { PATH: "", MUON_PLUGIN_INSPECTOR_PATH: "/missing" },
    ),
  ).toEqual(["muon.example.increment"]);
  const output = join(root, "app");
  await mkdir(output);
  const staged = await stageMuonNativePlugins(
    artifacts,
    "windows-amd64",
    output,
  );
  expect(staged.get("example")).toBe(join(output, "example.dll"));
  await mkdir(join(root, "src"));
  await writeFile(
    join(root, "src/main.ts"),
    'export { increment } from "muon:example";\n',
  );
  vi.stubEnv("MUON_SUPPRESS_VITE_MUON_BUILD", "1");
  vi.stubEnv("MUON_PLUGIN_INSPECTOR_PATH", "/missing");
  await viteBuild({
    root,
    configFile: false,
    logLevel: "silent",
    plugins: [muon({ open: false, build: { targets: ["windows-amd64"] } })],
    build: {
      lib: {
        entry: join(root, "src/main.ts"),
        formats: ["es"],
        fileName: () => "test.mjs",
      },
    },
  });
  const calls: unknown[][] = [];
  vi.stubGlobal("__muon_plugin_call", (...args: unknown[]) => {
    calls.push(args);
    return Promise.resolve(42);
  });
  const module = await import(
    /* @vite-ignore */ pathToFileURL(join(root, "dist/test.mjs")).href
  );
  expect(await module.increment(41)).toBe(42);
  expect(calls[0]).toContain("muon.example.increment");
});

it("builds a project-local source reference before reading its catalog", async () => {
  const root = await mkdtemp(join(tmpdir(), "muon-native-local-"));
  directories.push(root);
  const source = join(root, "local");
  await createPluginProject({
    directory: source,
    name: "local",
    language: "c",
  });
  // Install only the SDK entry point for this isolated authoring application.
  const sdkPackage = join(root, "node_modules/muon-plugin-sdk");
  await mkdir(sdkPackage, { recursive: true });
  await writeFile(
    join(sdkPackage, "package.json"),
    JSON.stringify({ name: "muon-plugin-sdk", main: "index.cjs" }),
  );
  await writeFile(
    join(sdkPackage, "index.cjs"),
    `module.exports = require(${JSON.stringify(resolve("../sdk/dist/index.cjs"))});\n`,
  );
  const artifacts = await resolveMuonNativePlugins({
    root,
    directory: root,
    plugins: [
      {
        name: "local",
        native: { source: "local/muon-plugin.build.json" },
        allow: ["muon.local.*"],
      },
    ],
    targets: ["linux-amd64"],
    environment: process.env,
  });
  expect(artifacts.get("local")!.manifest.functions).toEqual([
    "muon.local.increment",
  ]);
  expect(
    artifacts
      .get("local")!
      .targets["linux-amd64"]!.library.startsWith(
        join(root, ".muon/plugins/local"),
      ),
  ).toBe(true);
});
