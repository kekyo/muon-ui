// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { spawn } from "node:child_process";

const windowsTests = [
  "plugin-e2e-windows.test.ts",
  "windows-path.test.ts",
  "windows-builder-path.test.ts",
  "notifications-windows.test.ts",
  "server-windows.test.ts",
  "service-host-windows.test.ts",
  "services-windows.test.ts",
];
const filters = process.argv.slice(2);
const args = [
  "run",
  ...windowsTests.flatMap((file) => ["--exclude", file]),
  ...filters,
];
const run = async (command, args, env) =>
  await new Promise((resolve) => {
    const child = spawn(command, args, { env, stdio: "inherit" });
    child.once("error", (error) => {
      console.error(error.message);
      resolve(1);
    });
    child.once("exit", (code) => resolve(code ?? 1));
  });

if (process.env.MUON_PARALLEL_CEF_E2E === "1" && filters.length === 0) {
  // The outer command has already started the first window manager. The
  // second group needs its own display, D-Bus session and debug port.
  const runtimeEnv = { ...process.env, MUON_CEF_E2E_GROUP: "runtime" };
  delete runtimeEnv.MUON_E2E_CDP_PORT;
  const results = await Promise.all([
    run("vitest", args, { ...process.env, MUON_CEF_E2E_GROUP: "application" }),
    run(
      "xvfb-run",
      [
        "-a",
        process.execPath,
        "../../scripts/run-dbus-session-command.mjs",
        process.execPath,
        "../../scripts/run-xvfb-command.mjs",
        "vitest",
        "run",
        "--project",
        "cef",
        "plugin-e2e.test.ts",
      ],
      runtimeEnv,
    ),
  ]);
  process.exitCode = results.find((code) => code !== 0) ?? 0;
} else {
  process.exitCode = await run("vitest", args, process.env);
}
