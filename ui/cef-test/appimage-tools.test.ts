// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { mkdir, mkdtemp, readdir, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";

import { afterEach, describe, expect, it, vi } from "vitest";

import { ensureLinuxAppImageTools } from "../cef/appimage-tools.js";

const directories: string[] = [];
afterEach(async () => {
  vi.unstubAllGlobals();
  for (const directory of directories.splice(0))
    await rm(directory, { recursive: true, force: true });
});

describe.skipIf(process.platform !== "linux" || process.arch !== "x64")(
  "AppImage build tools",
  () => {
    it("rejects an unverified download without publishing it to the tool cache", async () => {
      const directory = await mkdtemp(join(tmpdir(), "muon-appimage-tools-"));
      directories.push(directory);
      vi.stubGlobal(
        "fetch",
        vi.fn(async () => new Response("invalid tool")),
      );
      await expect(
        ensureLinuxAppImageTools(
          "linux-amd64",
          {
            ...process.env,
            MUON_APPIMAGE_TOOLS_DIR: directory,
          },
          undefined,
        ),
      ).rejects.toThrow("SHA-256 mismatch for appimagetool");
      expect(await readdir(directory)).toEqual([]);
    });

    it("rejects a modified cached tool before executing or downloading anything", async () => {
      const directory = await mkdtemp(join(tmpdir(), "muon-appimage-tools-"));
      directories.push(directory);
      const parent = join(directory, "appimagetool/1.9.1");
      await mkdir(parent, { recursive: true });
      await writeFile(
        join(parent, "appimagetool-x86_64.AppImage"),
        "modified tool",
      );
      const download = vi.fn();
      vi.stubGlobal("fetch", download);
      await expect(
        ensureLinuxAppImageTools(
          "linux-amd64",
          {
            ...process.env,
            MUON_APPIMAGE_TOOLS_DIR: directory,
          },
          undefined,
        ),
      ).rejects.toThrow("SHA-256 mismatch for appimagetool");
      expect(download).not.toHaveBeenCalled();
    });
  },
);
