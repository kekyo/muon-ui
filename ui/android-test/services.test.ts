// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { mkdtemp, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { afterAll, beforeAll, expect, it } from "vitest";
import { buildService, createServiceProject } from "muon-plugin-sdk";
import { resolveMuonServices } from "../common/service-config.ts";
import {
  resolveAndroidServices,
  getAndroidServicePermissions,
} from "../android/services.ts";

let root: string;
let manifest: string;
const android = {
  foregroundServiceType: "specialUse",
  specialUseSubtype: "Background processing selected by the user",
  notification: { channelId: "worker", title: "Worker", text: "Running" },
};
beforeAll(async () => {
  root = await mkdtemp(join(tmpdir(), "muon-android-services-"));
  await createServiceProject({
    directory: root,
    name: "service",
    language: "c",
    buildSystem: "make",
  });
  for (const target of ["android-arm64-v8a", "android-x86_64"] as const) {
    manifest = await buildService({
      configPath: join(root, "muon-plugin.build.json"),
      target,
    });
  }
});
afterAll(async () => {
  if (root) await rm(root, { recursive: true, force: true });
});

it("packages one factory as separate logical Android components for both ABIs", async () => {
  const definitions = await resolveMuonServices(
    {
      alpha: {
        runtime: "native",
        artifact: manifest,
        autostart: true,
        android,
        config: { role: "writer" },
      },
      beta: {
        runtime: "native",
        artifact: manifest,
        android: {
          ...android,
          foregroundServiceType: "dataSync",
          specialUseSubtype: undefined,
        },
      },
    },
    root,
    ["android-arm64-v8a", "android-x86_64"],
  );
  const services = await resolveAndroidServices(
    definitions,
    ["arm64-v8a", "x86_64"],
    [],
  );
  expect(services.map(({ id, autostart }) => ({ id, autostart }))).toEqual([
    { id: "alpha", autostart: true },
    { id: "beta", autostart: false },
  ]);
  expect(services[0]!.android.component).not.toBe(
    services[1]!.android.component,
  );
  expect(services[0]!.android.foregroundServiceType).toBe("specialUse");
  expect(services[1]!.android.foregroundServiceType).toBe("dataSync");
  expect(services[0]!.libraries).toEqual(services[1]!.libraries);
  expect(Object.keys(services[0]!.libraries)).toEqual(["arm64-v8a", "x86_64"]);
  expect(getAndroidServicePermissions(services)).toEqual([
    "android.permission.FOREGROUND_SERVICE",
    "android.permission.POST_NOTIFICATIONS",
    "android.permission.FOREGROUND_SERVICE_SPECIAL_USE",
    "android.permission.FOREGROUND_SERVICE_DATA_SYNC",
  ]);
});

it("does not add the permission of another foreground type to a short service", async () => {
  const definitions = await resolveMuonServices(
    {
      worker: {
        runtime: "native",
        artifact: manifest,
        android: {
          foregroundServiceType: "shortService",
          notification: android.notification,
        },
      },
    },
    root,
    ["android-arm64-v8a"],
  );
  const services = await resolveAndroidServices(definitions, ["arm64-v8a"], []);
  expect(getAndroidServicePermissions(services)).toEqual([
    "android.permission.FOREGROUND_SERVICE",
    "android.permission.POST_NOTIFICATIONS",
  ]);
});

it("preserves dotted and hyphenated service identities in Java-safe component names", async () => {
  const definitions = await resolveMuonServices(
    {
      "worker.v1": { runtime: "native", artifact: manifest, android },
      "worker-v1": { runtime: "native", artifact: manifest, android },
    },
    root,
    ["android-arm64-v8a"],
  );
  const services = await resolveAndroidServices(definitions, ["arm64-v8a"], []);
  expect(services.map((service) => service.id)).toEqual([
    "worker.v1",
    "worker-v1",
  ]);
  expect(services[0]!.android.component).not.toBe(
    services[1]!.android.component,
  );
  for (const service of services)
    expect(service.android.component).toMatch(
      /^dev\.muon\.generated\.MuonService_[a-f0-9]+$/u,
    );
});

it("rejects conflicting plugin and service libraries before the package builder runs", async () => {
  const definitions = await resolveMuonServices(
    { worker: { runtime: "native", artifact: manifest, android } },
    root,
    ["android-arm64-v8a"],
  );
  const services = await resolveAndroidServices(definitions, ["arm64-v8a"], []);
  const conflict = join(root, "conflict.so");
  await writeFile(conflict, "different bytes");
  await expect(
    resolveAndroidServices(
      definitions,
      ["arm64-v8a"],
      [
        {
          name: "page",
          soname: services[0]!.soname,
          allow: ["page.run"],
          config: [],
          libraries: { "arm64-v8a": conflict },
        },
      ],
    ),
  ).rejects.toThrow(/Conflicting Android library/);
});
