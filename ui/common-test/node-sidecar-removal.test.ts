// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile } from "node:child_process";
import { promisify } from "node:util";
import { mkdir, mkdtemp, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { build } from "vite";
import { expect, it } from "vitest";
import muon from "../common/vite.ts";

it("does not provide a Node sidecar entry point to a page", async () => {
  const root = await mkdtemp(join(tmpdir(), "muon-removed-page-api-"));
  try {
    await mkdir(join(root, "backend"));
    await writeFile(join(root, "backend/package.json"), "{}");
    await writeFile(
      join(root, "muon.json"),
      JSON.stringify({ node: { project: "./backend" } }),
    );
    await writeFile(
      join(root, "index.html"),
      '<script type="module" src="/main.js"></script>',
    );
    await writeFile(
      join(root, "main.js"),
      'import { createNode } from "muon:node"; console.log(createNode);',
    );
    await expect(
      build({
        root,
        logLevel: "silent",
        plugins: [muon({ open: false, build: false })],
      }),
    ).rejects.toThrow(/muon:node/);
  } finally {
    await rm(root, { recursive: true, force: true });
  }
});

it("does not offer the Android sidecar prototype as a workspace", async () => {
  await expect(
    promisify(execFile)("npm", [
      "pkg",
      "get",
      "name",
      "--workspace=muon-android-poc",
    ]),
  ).rejects.toThrow(/No workspaces found/i);
});
