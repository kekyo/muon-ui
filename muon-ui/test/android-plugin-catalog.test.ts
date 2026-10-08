// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { createHash } from "node:crypto";
import { mkdir, mkdtemp, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { afterEach, expect, it } from "vitest";
import { collectMuonAndroidPluginAccess } from "../src/android-plugin-catalog.js";
import { resolveMuonPluginAccessOptions } from "../src/plugin-access.js";
import { createMuonCapabilityModuleResolver } from "../src/capability.js";

const roots: string[] = [];
afterEach(async () => {
  for (const root of roots.splice(0))
    await rm(root, { recursive: true, force: true });
});

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
