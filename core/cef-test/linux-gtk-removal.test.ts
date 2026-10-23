// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile } from "node:child_process";
import { readdir, realpath } from "node:fs/promises";
import { join, resolve } from "node:path";
import { promisify } from "node:util";
import { expect, it } from "vitest";

const execute = promisify(execFile);
const runtime = process.env.MUON_TEST_SYSTEM_GTK_RUNTIME;

// Temporary removal checks; delete after committing the successful removal.
it.skipIf(runtime === undefined)(
  "removes the private GTK distribution",
  async () => {
    const entries = await readdir(resolve(runtime!), { recursive: true });
    expect(
      entries.filter((entry) =>
        /^(?:lib(?:gtk|gdk|glib|gio|gobject|gmodule|gthread|pango|atk|atspi|harfbuzz|cairo|rsvg|colord|fcitx|Fcitx|proxy|ffi|jpeg|webp|tiff)|linux(?:\/|$)|licenses\/linux(?:\/|$)|linux-bundle\.json)/.test(
          entry,
        ),
      ),
    ).toEqual([]);
  },
);

it.skipIf(runtime === undefined)(
  "resolves distribution GTK from the operating system",
  async () => {
    const directory = resolve(runtime!);
    const environment = { ...process.env };
    delete environment.LD_LIBRARY_PATH;
    delete environment.LD_PRELOAD;
    for (const file of ["muon-core", "libmuon-ui.so"]) {
      const { stdout } = await execute("ldd", [join(directory, file)], {
        env: environment,
      });
      expect(stdout).not.toContain("not found");
      for (const name of [
        "libgtk-3.so.0",
        "libgdk-3.so.0",
        "libglib-2.0.so.0",
        "libgio-2.0.so.0",
        "libgobject-2.0.so.0",
      ]) {
        const line = stdout
          .split("\n")
          .find((item) => item.trim().startsWith(name + " => "));
        expect(line, stdout).toBeDefined();
        const path = line!.trim().split(/\s+/)[2]!;
        expect(await realpath(path)).toMatch(/^\/(?:usr\/)?lib\//);
      }
    }
  },
);
