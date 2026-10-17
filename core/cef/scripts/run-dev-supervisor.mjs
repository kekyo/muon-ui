#!/usr/bin/env node
// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { spawn } from "node:child_process";
import { existsSync } from "node:fs";
import { access } from "node:fs/promises";
import { join, resolve } from "node:path";

// A script used exclusively within the muon project to restart muon-core in Recycle State.
// This script is not included in the NPM package.

const recycleExitCode = 88;
const signalExitCodes = {
  SIGINT: 130,
  SIGTERM: 143,
};

const runtimeDirectory = resolve(
  process.argv[2] ?? ".run/dev-linux-amd64-debug",
);
const executableName =
  process.argv[3] ??
  (process.platform === "win32" ? "muon-core.exe" : "muon-core");
const executablePath = resolve(runtimeDirectory, executableName);
const executableArguments = process.argv.slice(4);

// Match the packaged launcher and the UI launchers before loading GTK/GIO modules.
const environment = { ...process.env };
const moduleRoot = join(runtimeDirectory, "linux");
if (process.platform === "linux" && existsSync(moduleRoot)) {
  Object.assign(environment, {
    GTK_EXE_PREFIX: moduleRoot,
    GTK_PATH: join(moduleRoot, "lib", "gtk-3.0"),
    GTK_IM_MODULE_FILE: join(moduleRoot, "gtk-immodules.cache"),
    GDK_PIXBUF_MODULE_FILE: join(moduleRoot, "gdk-pixbuf-loaders.cache"),
    MUON_LINUX_MODULE_ROOT: moduleRoot,
    LD_LIBRARY_PATH:
      runtimeDirectory +
      (environment.LD_LIBRARY_PATH ? `:${environment.LD_LIBRARY_PATH}` : ""),
  });
}

let activeChild = undefined;

const forwardSignal = (signal) => {
  if (activeChild !== undefined && activeChild.exitCode === null) {
    activeChild.kill(signal);
  }
};

process.on("SIGINT", () => forwardSignal("SIGINT"));
process.on("SIGTERM", () => forwardSignal("SIGTERM"));

const runMuonOnce = async () =>
  await new Promise((resolvePromise, reject) => {
    activeChild = spawn(executablePath, executableArguments, {
      cwd: runtimeDirectory,
      env: environment,
      stdio: "inherit",
    });
    activeChild.once("error", reject);
    activeChild.once("close", (code, signal) => {
      activeChild = undefined;
      if (code !== null) {
        resolvePromise(code);
        return;
      }
      resolvePromise(signalExitCodes[signal] ?? 1);
    });
  });

try {
  await access(executablePath);
  let exitCode = 0;
  do {
    exitCode = await runMuonOnce();
  } while (exitCode === recycleExitCode);
  process.exitCode = exitCode;
} catch (error) {
  console.error(error instanceof Error ? error.message : String(error));
  process.exitCode = 1;
}
