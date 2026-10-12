// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { createHash } from "node:crypto";
import { mkdtemp, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { afterEach, expect, it } from "vitest";
import { readAndroidPluginMetadata } from "../android/plugin-metadata.js";

const roots: string[] = [];
afterEach(async () => {
  for (const root of roots.splice(0))
    await rm(root, { recursive: true, force: true });
});

it("reads a catalog bound to each ABI binary without executing it", async () => {
  const root = await mkdtemp(join(tmpdir(), "muon-android-metadata-"));
  roots.push(root);
  const library = Buffer.from("The catalog reader must not dlopen this input.");
  const metadata = {
    schemaVersion: 1,
    functions: ["muon.test.alpha.alphaAdd", "muon.test.alpha.alphaConfig"],
    sha256: { "arm64-v8a": createHash("sha256").update(library).digest("hex") },
  };
  await writeFile(join(root, "alpha.so"), library);
  await writeFile(join(root, "alpha.json"), JSON.stringify(metadata));
  const plugins = [
    {
      name: "alpha",
      metadata: "alpha.json",
      libraries: { "arm64-v8a": "alpha.so" },
    },
  ];
  const result = await readAndroidPluginMetadata(
    plugins,
    ["arm64-v8a"],
    root,
    true,
  );
  expect(result.get("alpha")).toEqual(metadata.functions);
  await writeFile(join(root, "alpha.so"), "changed binary");
  await expect(
    readAndroidPluginMetadata(plugins, ["arm64-v8a"], root, true),
  ).rejects.toThrow(/alpha.*arm64-v8a.*SHA-256/);
});

it("requires a producer catalog for validate mode", async () => {
  await expect(
    readAndroidPluginMetadata(
      [{ name: "alpha" }],
      ["arm64-v8a"],
      "/unused",
      true,
    ),
  ).rejects.toThrow(/alpha.*metadata/);
  expect(
    (
      await readAndroidPluginMetadata(
        [{ name: "alpha" }],
        ["arm64-v8a"],
        "/unused",
        false,
      )
    ).size,
  ).toBe(0);
});
