// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import {
  copyFile,
  mkdtemp,
  readFile,
  rm,
  symlink,
  writeFile,
} from "node:fs/promises";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { afterAll, beforeAll, expect, it } from "vitest";
import { buildPlugin, createPluginProject } from "muon-plugin-sdk";
import { resolveMuonServices } from "../common/service-config.ts";
import { resolveAndroidServices } from "../android/services.ts";

const repository = fileURLToPath(new URL("../../", import.meta.url));
const android = {
  foregroundServiceType: "specialUse",
  specialUseSubtype: "Explicit user background processing",
  notification: { channelId: "worker", title: "Worker", text: "Running" },
};
let root: string;
let manifest: string;
beforeAll(async () => {
  root = await mkdtemp(join(tmpdir(), "muon-android-service-plugins-"));
  await symlink(
    join(repository, "node_modules"),
    join(root, "node_modules"),
    "dir",
  );
  const project = join(root, "plugin");
  await createPluginProject({
    directory: project,
    name: "fixture",
    language: "c",
    buildSystem: "make",
  });
  const path = join(project, "muon-plugin.build.json");
  const configuration = JSON.parse(await readFile(path, "utf8"));
  const fixture = JSON.parse(
    await readFile(
      join(repository, "core/common-test/muon-service-plugin.build.json"),
      "utf8",
    ),
  );
  configuration.functions = fixture.functions;
  await copyFile(
    join(repository, "core/common-test/muon_service_native_plugin_fixture.c"),
    join(project, configuration.sources[0]),
  );
  await writeFile(path, JSON.stringify(configuration));
  for (const target of ["android-arm64-v8a", "android-x86_64"] as const)
    manifest = await buildPlugin({ configPath: path, target });
  await writeFile(
    join(root, "service.ts"),
    `
    import {defineService} from "muon-plugin-sdk/services";
    import {echo} from "fixture:api";
    export default defineService({
      interface:{schemaVersion:1,methods:{},events:{}},
      async start(){await echo("start");return {methods:{},async stop(){await echo("stop")}}}
    });
  `,
  );
});
afterAll(async () => {
  if (root) await rm(root, { recursive: true, force: true });
});

const definitions = async () =>
  await resolveMuonServices(
    {
      first: {
        runtime: "quickjs",
        entry: "service.ts",
        android,
        plugins: {
          fixture: {
            native: { manifest },
            allow: ["fixture.api.echo"],
            config: { mode: "shared" },
          },
        },
      },
      second: {
        runtime: "quickjs",
        entry: "service.ts",
        android,
        plugins: {
          fixture: {
            native: { manifest },
            allow: ["fixture.api.echo", "fixture.api.echoBytes"],
            config: { mode: "shared" },
          },
        },
      },
    },
    root,
    ["android-arm64-v8a", "android-x86_64"],
    { root: repository, environment: process.env },
  );

it("packages one native plugin for two ABIs while keeping service grants separate", async () => {
  const services = await resolveAndroidServices(
    await definitions(),
    ["arm64-v8a", "x86_64"],
    [],
  );
  const first = services[0]!;
  const second = services[1]!;
  if (first.runtime !== "quickjs" || second.runtime !== "quickjs")
    throw new Error("Expected QuickJS services");
  expect(first.plugins.map((plugin) => plugin.allow)).toEqual([
    ["fixture.api.echo"],
  ]);
  expect(second.plugins.map((plugin) => plugin.allow)).toEqual([
    ["fixture.api.echo", "fixture.api.echoBytes"],
  ]);
  expect(first.plugins[0]!.libraries).toEqual(second.plugins[0]!.libraries);
  expect(Object.keys(first.plugins[0]!.libraries)).toEqual([
    "arm64-v8a",
    "x86_64",
  ]);
  expect(first.plugins[0]!.config).toEqual([{ key: "mode", value: "shared" }]);
});

it("rejects a different page library using the service plugin SONAME", async () => {
  const resolved = await definitions();
  const services = await resolveAndroidServices(
    resolved,
    ["arm64-v8a", "x86_64"],
    [],
  );
  const first = services[0]!;
  if (first.runtime !== "quickjs") throw new Error("Expected QuickJS service");
  const plugin = first.plugins[0]!;
  const conflict = join(dirname(manifest), "conflicting-library.so");
  await writeFile(conflict, "different bytes");
  await expect(
    resolveAndroidServices(
      resolved,
      ["arm64-v8a"],
      [
        {
          ...plugin,
          name: "page",
          libraries: { "arm64-v8a": conflict },
        },
      ],
    ),
  ).rejects.toThrow(/Conflicting Android library/);
});
