// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";

import type { WindowsRuntimeTarget } from "./windows-matrix.js";

const workspaceDirectory = fileURLToPath(new URL("../", import.meta.url));

const runNpm = async (args: string[]): Promise<void> => {
  await new Promise<void>((resolve, reject) => {
    const child = spawn("npm", args, {
      cwd: workspaceDirectory,
      stdio: "inherit",
    });
    child.once("error", reject);
    child.once("close", (code, signal) => {
      if (code === 0) {
        resolve();
      } else {
        reject(new Error(`npm ${args.join(" ")} failed (${signal ?? code})`));
      }
    });
  });
};

/**
 * Builds the local binaries required by one Windows runtime test suite.
 * @param target Windows architecture to compile in Debug and Release modes.
 * @returns Completion after the runtime and CDP relay are ready for staging.
 * @throws Error when a build command fails, including missing build tools.
 * @remarks The caller must release any remote agent before starting local builds.
 */
export const buildWindowsRuntime = async (
  target: WindowsRuntimeTarget,
): Promise<void> => {
  for (const configuration of ["Debug", "Release"]) {
    await runNpm([
      "run",
      "build:target",
      "--workspace",
      "muon-core",
      "--",
      "test",
      configuration,
      target.target,
    ]);
  }
  await runNpm(["run", "build:windows-cdp-relay"]);
};
