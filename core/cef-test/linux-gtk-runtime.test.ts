// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile } from "node:child_process";
import { mkdtemp, realpath, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { promisify } from "node:util";
import { expect, it } from "vitest";

const execute = promisify(execFile);
const runtime = process.env.MUON_TEST_LINUX_RUNTIME;

it.skipIf(runtime === undefined)(
  "uses system GTK for distribution linkage, file selection and image decoding",
  async () => {
    const directory = await mkdtemp(join(tmpdir(), "muon-gtk-probe-"));
    const executable = join(directory, "probe");
    try {
      await execute("make", ["-C", "native", `OUTPUT=${executable}`]);
      const runtimeDirectory = resolve(runtime!);
      const environment = { ...process.env };
      delete environment.LD_LIBRARY_PATH;
      delete environment.LD_PRELOAD;
      for (const name of ["muon-core", "libmuon-ui.so"]) {
        const { stdout } = await execute(
          "ldd",
          [join(runtimeDirectory, name)],
          { env: environment },
        );
        expect(stdout).not.toContain("not found");
        for (const library of [
          "libgtk-3.so.0",
          "libgdk-3.so.0",
          "libglib-2.0.so.0",
          "libgio-2.0.so.0",
          "libgobject-2.0.so.0",
        ]) {
          const line = stdout
            .split("\n")
            .find((entry) => entry.trim().startsWith(library + " => "));
          expect(line, stdout).toBeDefined();
          const path = await realpath(line!.trim().split(/\s+/)[2]!);
          expect(path).toMatch(/^\/(?:usr\/)?lib\//);
          console.info(`${name}: ${library} => ${path}`);
        }
      }
      const picture = join(directory, "red.svg");
      await writeFile(
        picture,
        '<svg xmlns="http://www.w3.org/2000/svg" width="3" height="2"><rect width="3" height="2" fill="red"/></svg>',
      );
      // Keep host module/data lookup intact while confining chooser settings.
      const { stdout, stderr } = await execute(executable, [picture, "ibus"], {
        env: {
          ...environment,
          GSETTINGS_BACKEND: "memory",
          GTK_USE_PORTAL: "0",
          DBUS_SESSION_BUS_ADDRESS: `unix:path=${directory}/absent-bus`,
          GIO_USE_VFS: "local",
          GTK_IM_MODULE: "gtk-im-context-simple",
          XDG_DATA_HOME: directory,
        },
      });
      console.info(stdout + stderr);
      expect(stderr).not.toMatch(/CRITICAL|Gtk-WARNING/);
      expect(stdout).toContain(
        `backend: ${process.env.GDK_BACKEND === "wayland" ? "GdkWaylandDisplay" : "GdkX11Display"}`,
      );
      expect(stdout).toContain("file chooser: selected=1");
      expect(stdout).toContain("image: decoded=1");
      expect(stdout).toContain("input: ibus system=1");
    } finally {
      await rm(directory, { recursive: true, force: true });
    }
  },
);
