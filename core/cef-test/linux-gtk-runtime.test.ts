// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile } from "node:child_process";
import { mkdir, mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { promisify } from "node:util";
import { expect, it } from "vitest";
import { buildCandidateVariables, runScriptOnceToText } from "funcity";
import { createMuonRuntimeEnvironment } from "../../ui/cef/runtime-environment.js";

const execute = promisify(execFile);
const runtime = process.env.MUON_TEST_LINUX_RUNTIME;
const legacySchemas = process.env.MUON_TEST_GTK_LEGACY_SCHEMAS;
const schemaModes = [
  "new",
  "none",
  ...(legacySchemas === undefined ? [] : ["old"]),
  ...(process.env.MUON_TEST_GTK_PORTAL === "1" ? ["portal"] : []),
];

it.skipIf(runtime === undefined).each(schemaModes)(
  "initializes bundled GTK and opens a file chooser with %s GNOME schemas",
  async (schemaMode) => {
    const directory = await mkdtemp(join(tmpdir(), "muon-gtk-probe-"));
    const executable = join(directory, "probe");
    try {
      await execute("make", ["-C", "native", `OUTPUT=${executable}`]);
      const runtimeDirectory = resolve(runtime!);
      const picture = join(directory, "red.svg");
      await writeFile(
        picture,
        '<svg xmlns="http://www.w3.org/2000/svg" width="3" height="2"><rect width="3" height="2" fill="red"/></svg>',
      );
      const environment: NodeJS.ProcessEnv = {
        ...process.env,
        GSETTINGS_BACKEND: "memory",
        GTK_USE_PORTAL: schemaMode === "portal" ? "1" : "0",
        G_MESSAGES_DEBUG: schemaMode === "portal" ? "all" : "",
        // Direct settings cases cannot accidentally use the desktop's portal.
        DBUS_SESSION_BUS_ADDRESS:
          schemaMode === "portal"
            ? process.env.DBUS_SESSION_BUS_ADDRESS
            : `unix:path=${directory}/absent-bus`,
        GIO_USE_VFS: "local",
        // The chooser does not require a session-specific IM daemon.
        // The probe separately selects and loads each bundled module below.
        GTK_IM_MODULE: "gtk-im-context-simple",
        XDG_DATA_HOME: directory,
      };
      if (schemaMode === "none" || schemaMode === "old") {
        const data = join(directory, "data");
        await mkdir(data);
        // Neither optional GNOME settings nor GTK support data may be
        // supplied accidentally by the developer's desktop installation.
        environment.XDG_DATA_DIRS = data;
        environment.GSETTINGS_SCHEMA_DIR =
          schemaMode === "old" ? legacySchemas : data;
      }
      const childEnvironment = createMuonRuntimeEnvironment(
        runtimeDirectory,
        "linux",
        environment,
      );
      for (const [name, variable] of [
        ["gtk-immodules", "GTK_IM_MODULE_FILE"],
        ["gdk-pixbuf-loaders", "GDK_PIXBUF_MODULE_FILE"],
      ] as const) {
        const cache = await readFile(
          join(runtimeDirectory, "linux", `${name}.cache.in`),
          "utf8",
        );
        const text = await runScriptOnceToText(
          "{{replace pattern replacement cache}}",
          {
            sourceId: "muon-gtk-probe-cache",
            logs: [],
            variables: buildCandidateVariables({
              pattern: /@MUON_RUNTIME@/g,
              replacement: runtimeDirectory,
              cache,
            }),
          },
        );
        if (text === undefined)
          throw new Error("Cannot prepare GTK probe cache");
        const path = join(directory, `${name}.cache`);
        await writeFile(path, text);
        childEnvironment[variable] = path;
      }
      for (const id of ["ibus", "fcitx", "fcitx5"]) {
        const { stdout, stderr } = await execute(
          executable,
          [schemaMode, picture, id],
          {
            env: childEnvironment,
          },
        );
        console.info(`${schemaMode}/${id}:\n${stdout}${stderr}`);
        expect(stderr).not.toContain("CRITICAL");
        expect(stderr).not.toContain("Gtk-WARNING");
        expect(stdout).toContain(
          `schemas: current=${schemaMode === "new" || schemaMode === "portal" ? 1 : 0} legacy=${schemaMode === "old" ? 1 : 0}`,
        );
        const wayland = process.env.GDK_BACKEND !== "x11";
        expect(stdout).toContain(
          `backend: ${wayland ? "GdkWaylandDisplay" : "GdkX11Display"}`,
        );
        if ((schemaMode === "new" || schemaMode === "old") && wayland)
          expect(stdout).toContain(
            "fonts: antialias=1 hintstyle=hintfull rgba=bgr",
          );
        expect(stdout).toContain("file chooser: shown");
        expect(stdout).toContain("file chooser: selected=1");
        expect(stdout).toContain("image: decoded=1");
        expect(stdout).toContain(`input: ${id} bundled=1`);
        if (schemaMode === "portal" && wayland) {
          expect(stdout).toContain(
            "Using portal setting for org.gnome.desktop.interface font-antialiasing",
          );
          expect(stdout).not.toContain("Failed to use Settings portal");
        }
      }
    } finally {
      await rm(directory, { recursive: true, force: true });
    }
  },
);
