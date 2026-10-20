// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { createHash } from "node:crypto";
import { mkdir, mkdtemp, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { afterEach, expect, it } from "vitest";
import { collectMuonAndroidPluginAccess } from "../android/plugin-catalog.js";
import { resolveMuonPluginAccessOptions } from "../common/plugin-access.js";
import { createMuonCapabilityModuleResolver } from "../common/capability.js";

const roots: string[] = [];
const previousCall = Object.getOwnPropertyDescriptor(
  globalThis,
  "__muon_plugin_call",
);
afterEach(async () => {
  for (const root of roots.splice(0))
    await rm(root, { recursive: true, force: true });
  if (previousCall === undefined)
    Reflect.deleteProperty(globalThis, "__muon_plugin_call");
  else Object.defineProperty(globalThis, "__muon_plugin_call", previousCall);
});

it.each([true, false])(
  "publishes notification permission imports only when enabled: %s",
  async (enabled) => {
    const root = await mkdtemp(join(tmpdir(), "muon-notification-catalog-"));
    roots.push(root);
    await writeFile(
      join(root, "muon.json"),
      JSON.stringify({
        notifications: { enabled },
        plugin: {
          mode: "validate",
          plugins: [
            {
              name: "internal",
              imports: [
                {
                  sources: ["src/**"],
                  allow: [
                    "muon.android.notifications.*",
                    "muon.notifications.*",
                  ],
                },
              ],
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
    if (!enabled) {
      await expect(
        collectMuonAndroidPluginAccess(root, undefined, undefined, access),
      ).rejects.toThrow(/unavailable/);
      return;
    }
    const catalog = await collectMuonAndroidPluginAccess(
      root,
      undefined,
      undefined,
      access,
    );
    const resolver = createMuonCapabilityModuleResolver(root, {
      backend: "android",
      ...catalog,
    });
    const id = resolver.resolveId(
      "muon:android.notifications",
      join(root, "src/main.ts"),
    )!.id;
    Reflect.set(globalThis, "__muon_plugin_call", async () => undefined);
    const module = await import(
      `data:text/javascript,${encodeURIComponent(resolver.load(id)!)}`
    );
    expect(Object.keys(module).sort()).toEqual([
      "ensureChannel",
      "getCapabilities",
      "getChannel",
      "getPermissionState",
      "requestPermission",
    ]);
    const commonId = resolver.resolveId(
      "muon:notifications",
      join(root, "src/main.ts"),
    )!.id;
    const common = await import(
      `data:text/javascript,${encodeURIComponent(resolver.load(commonId)!)}`
    );
    expect(Object.keys(common).sort()).toEqual([
      "acknowledgeEvent",
      "cancel",
      "getCapabilities",
      "getPendingEvents",
      "getStatus",
      "post",
      "watchEvents",
    ]);
  },
);

it("resolves Android wildcards per plugin and checks source/package importers", async () => {
  const root = await mkdtemp(join(tmpdir(), "muon-android-catalog-"));
  roots.push(root);
  await mkdir(join(root, "node_modules/approved"), { recursive: true });
  await writeFile(
    join(root, "node_modules/approved/package.json"),
    JSON.stringify({ name: "approved" }),
  );
  const library = Buffer.from("not executable on the host");
  await writeFile(join(root, "alpha.so"), library);
  await writeFile(
    join(root, "alpha.json"),
    JSON.stringify({
      schemaVersion: 1,
      functions: ["muon.test.alpha.alphaAdd"],
      sha256: {
        "arm64-v8a": createHash("sha256").update(library).digest("hex"),
      },
    }),
  );
  await writeFile(
    join(root, "muon.json"),
    JSON.stringify({
      android: {
        abis: ["arm64-v8a"],
        plugins: [
          {
            name: "alpha",
            metadata: "alpha.json",
            libraries: { "arm64-v8a": "alpha.so" },
          },
        ],
      },
      plugin: {
        mode: "validate",
        plugins: [
          {
            name: "internal",
            imports: [{ sources: ["src/**"], allow: ["muon.**"] }],
          },
          {
            name: "alpha",
            imports: [
              {
                sources: ["src/**"],
                packages: ["approved"],
                allow: ["muon.test.alpha.*"],
              },
            ],
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
  const catalog = await collectMuonAndroidPluginAccess(
    root,
    undefined,
    undefined,
    access,
  );
  expect(catalog.imports[0]!.allow).not.toContain("muon.test.alpha.alphaAdd");
  expect(catalog.imports[1]!.allow).toEqual(["muon.test.alpha.alphaAdd"]);
  const resolver = createMuonCapabilityModuleResolver(root, {
    backend: "android",
    ...catalog,
  });
  expect(
    resolver.resolveId("muon:test.alpha", join(root, "src/main.ts")),
  ).toBeDefined();
  expect(
    resolver.resolveId(
      "muon:test.alpha",
      join(root, "node_modules/approved/index.js"),
    ),
  ).toBeDefined();
  expect(() =>
    resolver.resolveId("muon:test.alpha", join(root, "other/main.ts")),
  ).toThrow(/not allowed/);
  expect(() =>
    resolver.resolveId(
      "muon:test.alpha",
      join(root, "node_modules/denied/index.js"),
    ),
  ).toThrow(/not allowed/);
});
