// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile } from "node:child_process";
import {
  appendFile,
  copyFile,
  mkdir,
  mkdtemp,
  readFile,
  rm,
  symlink,
  writeFile,
} from "node:fs/promises";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { promisify } from "node:util";
import { afterAll, beforeAll, expect, it } from "vitest";
import {
  buildPlugin,
  createPluginProject,
  getNativeBuildEnvironment,
} from "muon-plugin-sdk";
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
  configuration.androidApi = 33;
  await copyFile(
    join(repository, "core/common-test/muon_service_native_plugin_fixture.c"),
    join(project, configuration.sources[0]),
  );
  await writeFile(
    join(project, "vendor.c"),
    "int vendor_read(void) { return 42; }\n",
  );
  await appendFile(
    join(project, "Makefile"),
    '\nLDLIBS += -L"$(MUON_PLUGIN_PROJECT)vendor/$(MUON_TARGET)" -Wl,--no-as-needed -lvendor_math -Wl,--as-needed\n',
  );
  configuration.dependencies = {};
  configuration.androidDependenciesWithoutSoname = {};
  for (const target of ["android-arm64-v8a", "android-x86_64"] as const) {
    const env = await getNativeBuildEnvironment({ target });
    const directory = join(project, "vendor", target);
    await mkdir(directory, { recursive: true });
    await promisify(execFile)(
      env.CC!,
      [
        "-shared",
        "-fPIC",
        "-Wl,-z,max-page-size=16384",
        join(project, "vendor.c"),
        "-o",
        join(directory, "libvendor_math.so"),
      ],
      { env },
    );
    configuration.dependencies[target] = [
      "vendor/{{target}}/libvendor_math.so",
    ];
    configuration.androidDependenciesWithoutSoname[target] = [
      "libvendor_math.so",
    ];
    await writeFile(path, JSON.stringify(configuration));
    manifest = await buildPlugin({ configPath: path, target });
  }
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
    33,
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
  for (const abi of ["arm64-v8a", "x86_64"] as const)
    expect(first.plugins[0]!.dependencies?.[abi]?.[0]).toMatch(
      /libvendor_math\.so$/,
    );
  expect(Object.keys(first.plugins[0]!.libraries)).toEqual([
    "arm64-v8a",
    "x86_64",
  ]);
  expect(first.plugins[0]!.config).toEqual([{ key: "mode", value: "shared" }]);
});

it("rejects service plugins newer than the minimum supported Android device", async () => {
  await expect(
    resolveAndroidServices(await definitions(), ["arm64-v8a"], [], 32),
  ).rejects.toThrow(/fixture.*API 33.*32/);
});

it("rejects a different page library using the service plugin SONAME", async () => {
  const resolved = await definitions();
  const services = await resolveAndroidServices(
    resolved,
    ["arm64-v8a", "x86_64"],
    [],
    33,
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
      33,
    ),
  ).rejects.toThrow(/Conflicting Android library/);
});
