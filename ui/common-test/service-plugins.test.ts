// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { mkdtemp, readFile, rm, symlink, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join, relative } from "node:path";
import { fileURLToPath } from "node:url";
import { afterAll, beforeAll, expect, it } from "vitest";
import { buildPlugin, createPluginProject } from "muon-plugin-sdk";
import { resolveMuonServices } from "../common/service-config.ts";
import { stageMuonDesktopServices } from "../cef/services.ts";

let root: string;
let manifest: string;
beforeAll(async () => {
  root = await mkdtemp(join(tmpdir(), "muon-service-plugins-"));
  await symlink(
    fileURLToPath(new URL("../../node_modules", import.meta.url)),
    join(root, "node_modules"),
    "dir",
  );
  const directory = join(root, "native");
  await createPluginProject({ directory, name: "example", language: "c" });
  for (const target of ["linux-amd64", "windows-amd64"] as const)
    manifest = relative(
      root,
      await buildPlugin({
        configPath: join(directory, "muon-plugin.build.json"),
        target,
      }),
    );
  await writeFile(
    join(root, "service.ts"),
    `
    import { defineService, defineServiceInterface } from 'muon-plugin-sdk/services';
    import { increment } from 'muon:example';
    export default defineService({
      interface: defineServiceInterface({schemaVersion:1,methods:{
        next:{parameters:['int32'],returns:'int32',errors:[]}
      },events:{}}),
      start: () => ({methods:{next: async (_call, value: number) => await increment(value)}, stop:async()=>{}})
    });
  `,
  );
  await writeFile(
    join(root, "internal.ts"),
    `import { quit } from 'muon:browser'; export default quit;`,
  );
}, 120_000);
afterAll(async () => {
  if (root) await rm(root, { recursive: true, force: true });
});

const declaration = (config: Record<string, string>) => ({
  runtime: "quickjs",
  entry: "service.ts",
  plugins: {
    example: {
      native: { manifest },
      allow: ["muon.example.increment"],
      config,
    },
  },
});

it("packages explicit service plugins independently of page permissions", async () => {
  const definitions = await resolveMuonServices(
    {
      first: declaration({ role: "shared" }),
      second: declaration({ role: "shared" }),
    },
    root,
    ["linux-amd64", "windows-amd64"],
  );
  for (const target of ["linux-amd64", "windows-amd64"] as const) {
    const output = join(root, target);
    await stageMuonDesktopServices(definitions, target, output);
    const registry = JSON.parse(
      await readFile(join(output, "muon-services.json"), "utf8"),
    );
    expect(registry.services.first.plugins).toEqual(["muon.example.increment"]);
    expect(registry.services.second.plugins).toEqual([
      "muon.example.increment",
    ]);
    expect(registry.plugins).toHaveLength(1);
    const plugin = registry.plugins[0];
    expect(plugin).toMatchObject({
      name: "example",
      config: { role: "shared" },
      allow: ["muon.example.increment"],
      expectedFunctions: ["muon.example.increment"],
    });
    expect(plugin.signature).toMatch(/^[a-f0-9]{64}$/);
    expect(
      (await readFile(join(output, plugin.library))).length,
    ).toBeGreaterThan(0);
    expect(
      await readFile(join(output, registry.services.first.script), "utf8"),
    ).toContain('"muon:example"');
  }
});

it("rejects process-shared plugin configurations that disagree", async () => {
  await expect(
    resolveMuonServices(
      {
        first: declaration({ role: "first" }),
        second: declaration({ role: "second" }),
      },
      root,
      ["linux-amd64"],
    ),
  ).rejects.toThrow(/Conflicting service plugin.*example/);
});

it("does not grant page host modules to a service", async () => {
  await expect(
    resolveMuonServices(
      { worker: { ...declaration({}), entry: "internal.ts" } },
      root,
      ["linux-amd64"],
    ),
  ).rejects.toThrow(/Service.*not.*muon:browser/);
});

it("requires explicit plugins for a host module import", async () => {
  await expect(
    resolveMuonServices(
      { worker: { runtime: "quickjs", entry: "service.ts" } },
      root,
      ["linux-amd64"],
    ),
  ).rejects.toThrow(/Service.*not.*muon:example/);
});

it.each([
  { native: { manifest: "unused.json" }, allow: [] },
  { allow: ["muon.example.*"] },
  {
    native: { manifest: "unused.json" },
    allow: ["muon.example.*"],
    imports: [],
  },
])("rejects incomplete or page-specific plugin options %j", async (plugin) => {
  await expect(
    resolveMuonServices(
      {
        worker: {
          runtime: "quickjs",
          entry: "service.ts",
          plugins: { example: plugin },
        },
      },
      root,
      ["linux-amd64"],
    ),
  ).rejects.toThrow(/services.worker.plugins/);
});

it("rejects grants with no matching producer functions", async () => {
  const service = declaration({});
  service.plugins.example.allow = ["muon.example.missing"];
  await expect(
    resolveMuonServices({ worker: service }, root, ["linux-amd64"]),
  ).rejects.toThrow(/no allowed functions/);
});

it("requires the QuickJS runtime for service plugin bindings", async () => {
  await expect(
    resolveMuonServices(
      { worker: { runtime: "native", artifact: "unused.json", plugins: {} } },
      root,
      ["linux-amd64"],
    ),
  ).rejects.toThrow(/plugins.*quickjs/);
});
