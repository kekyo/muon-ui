// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { mkdtemp, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join, relative } from "node:path";
import { afterAll, beforeAll, expect, it } from "vitest";
import { buildService, createServiceProject } from "muon-plugin-sdk";
import { resolveMuonServices } from "../common/service-config.ts";

let root: string;
let artifact: string;
beforeAll(async () => {
  root = await mkdtemp(join(tmpdir(), "muon-service-config-"));
  const directory = join(root, "native");
  await createServiceProject({
    directory,
    name: "worker",
    language: "c",
    buildSystem: "make",
  });
  artifact = relative(
    root,
    await buildService({
      configPath: join(directory, "muon-plugin.build.json"),
      target: "linux-amd64",
    }),
  );
});
afterAll(async () => {
  if (root) await rm(root, { recursive: true, force: true });
});

it("allows applications without services", async () => {
  expect(await resolveMuonServices(undefined, root, ["linux-amd64"])).toEqual(
    [],
  );
  expect(await resolveMuonServices({}, root, ["linux-amd64"])).toEqual([]);
});

it("resolves one factory into independent logical service definitions", async () => {
  const services = await resolveMuonServices(
    {
      alpha: {
        runtime: "native",
        artifact,
        autostart: true,
        config: { role: "writer" },
      },
      beta: { runtime: "native", artifact, config: { role: "reader" } },
    },
    root,
    ["linux-amd64"],
  );
  expect(
    services.map(({ id, autostart, config }) => ({ id, autostart, config })),
  ).toEqual([
    { id: "alpha", autostart: true, config: { role: "writer" } },
    { id: "beta", autostart: false, config: { role: "reader" } },
  ]);
  expect(services[0]!.artifact.path).toBe(join(root, artifact));
  expect(services[0]!.artifact.targets["linux-amd64"]!.library).toBe(
    services[1]!.artifact.targets["linux-amd64"]!.library,
  );
  expect(services[0]!.artifact.manifest.kind).toBe("service");
});

it.each(["quickjs", "node", "unknown"])(
  "rejects the unavailable %s runtime",
  async (runtime) => {
    await expect(
      resolveMuonServices({ worker: { runtime, artifact } }, root, [
        "linux-amd64",
      ]),
    ).rejects.toThrow(/runtime/);
  },
);

it.each([
  ["../escape", {}],
  ["worker", { autostart: "true" }],
  ["worker", { config: { value: 42 } }],
  ["worker", { config: null }],
  ["worker", { config: { value: "with\0nul" } }],
  ["worker", { start: true }],
] as const)("rejects invalid service definition %s %j", async (id, extra) => {
  await expect(
    resolveMuonServices(
      { [id]: { runtime: "native", artifact, ...extra } },
      root,
      ["linux-amd64"],
    ),
  ).rejects.toThrow(/service/i);
});

it("requires Android foreground configuration only when building Android", async () => {
  await expect(
    resolveMuonServices({ worker: { runtime: "native", artifact } }, root, [
      "android-arm64-v8a",
    ]),
  ).rejects.toThrow(/android/);
});

it("retains service-specific Android purposes and notification text", async () => {
  const android = {
    foregroundServiceType: "specialUse",
    specialUseSubtype: "User-controlled background processing",
    notification: {
      channelId: "background",
      title: "Worker",
      text: "Processing",
    },
  };
  const result = await resolveMuonServices(
    { worker: { runtime: "native", artifact, android } },
    root,
    ["linux-amd64"],
  );
  expect(result[0]!.android).toEqual(android);
  await expect(
    resolveMuonServices(
      {
        worker: {
          runtime: "native",
          artifact,
          android: {
            foregroundServiceType: "specialUse",
            notification: android.notification,
          },
        },
      },
      root,
      ["linux-amd64"],
    ),
  ).rejects.toThrow(/specialUseSubtype/);
});

it("requires an artifact binary for every selected target", async () => {
  await expect(
    resolveMuonServices({ worker: { runtime: "native", artifact } }, root, [
      "windows-amd64",
    ]),
  ).rejects.toThrow(/windows-amd64/);
});
