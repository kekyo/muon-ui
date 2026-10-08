// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { mkdtemp, mkdir, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { afterEach, expect, it } from "vitest";
import { packMuonApp } from "../common/pack.js";

const roots: string[] = [];
afterEach(async () => {
  await Promise.all(
    roots
      .splice(0)
      .map(async (root) => await rm(root, { recursive: true, force: true })),
  );
});
it("rejects an unsigned release request with an actionable signing diagnostic", async () => {
  const root = await mkdtemp(join(tmpdir(), "muon-android-pack-"));
  roots.push(root);
  await writeFile(
    join(root, "package.json"),
    JSON.stringify({ name: "app", version: "1.0.0" }),
  );
  await mkdir(join(root, "assets"));
  await writeFile(join(root, "assets/index.html"), "<h1>Application</h1>");
  await expect(
    packMuonApp({ root, targets: ["android"], types: ["apk"] }),
  ).rejects.toThrow(/android.signing/);
});
it("diagnoses incompatible package and target selections", async () => {
  const root = await mkdtemp(join(tmpdir(), "muon-android-pack-selection-"));
  roots.push(root);
  await writeFile(
    join(root, "package.json"),
    JSON.stringify({ name: "app", version: "1.0.0" }),
  );
  await expect(
    packMuonApp({ root, targets: ["android"], types: ["zip"] }),
  ).rejects.toThrow(/Android.*apk/i);
  await expect(
    packMuonApp({ root, targets: ["linux-amd64"], types: ["apk"] }),
  ).rejects.toThrow(/apk.*android/i);
});
