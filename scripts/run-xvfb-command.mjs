import { spawn } from "node:child_process";
import { mkdir, stat } from "node:fs/promises";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

import { createXvfbCommandEnvironment } from "./xvfb-environment.mjs";

const scriptDirectory = dirname(fileURLToPath(import.meta.url));
const projectDirectory = dirname(scriptDirectory);
const sourcePath = join(scriptDirectory, "muon-xvfb-window-manager.c");
const binaryPath = join(
  projectDirectory,
  "node_modules",
  ".cache",
  "muon",
  "muon-xvfb-window-manager",
);

const runProcess = async (command, args, options) =>
  await new Promise((resolvePromise, reject) => {
    const child = spawn(command, args, options);
    child.once("error", reject);
    child.once("exit", (code, signal) => {
      if (signal !== null) {
        reject(new Error(`${command} exited with signal ${signal}`));
        return;
      }
      resolvePromise(code ?? 0);
    });
  });

const fileMtimeMs = async (path) => {
  try {
    return (await stat(path)).mtimeMs;
  } catch {
    return undefined;
  }
};

const ensureWindowManagerBuilt = async () => {
  const [sourceMtimeMs, binaryMtimeMs] = await Promise.all([
    fileMtimeMs(sourcePath),
    fileMtimeMs(binaryPath),
  ]);
  if (sourceMtimeMs === undefined) {
    throw new Error(`Missing Xvfb window manager source: ${sourcePath}`);
  }
  if (binaryMtimeMs !== undefined && binaryMtimeMs >= sourceMtimeMs) {
    return;
  }

  await mkdir(dirname(binaryPath), { recursive: true });
  const code = await runProcess(
    "gcc",
    [
      sourcePath,
      "-std=c99",
      "-Wall",
      "-Wextra",
      "-Werror",
      "-O2",
      "-lX11",
      "-o",
      binaryPath,
    ],
    { stdio: "inherit" },
  );
  if (code !== 0) {
    throw new Error(`gcc exited with code ${code}`);
  }
};

const startWindowManager = async () =>
  await new Promise((resolvePromise, reject) => {
    const child = spawn(binaryPath, [], {
      env: process.env,
      stdio: ["ignore", "pipe", "pipe"],
    });
    let stdout = "";
    let stderr = "";
    let settled = false;

    const finish = (error, value) => {
      if (settled) {
        return;
      }
      settled = true;
      clearTimeout(timer);
      if (error !== undefined) {
        reject(error);
        return;
      }
      resolvePromise(value);
    };

    const timer = setTimeout(() => {
      child.kill("SIGTERM");
      finish(
        new Error(
          `Timed out waiting for Xvfb window manager startup. stderr:\n${stderr}`,
        ),
      );
    }, 5000);

    child.stdout.setEncoding("utf8");
    child.stderr.setEncoding("utf8");
    child.stdout.on("data", (chunk) => {
      stdout += chunk;
      if (stdout.includes("ready\n")) {
        finish(undefined, child);
      }
    });
    child.stderr.on("data", (chunk) => {
      stderr += chunk;
    });
    child.once("error", (error) => {
      finish(error);
    });
    child.once("exit", (code, signal) => {
      if (!settled) {
        finish(
          new Error(
            `Xvfb window manager exited before startup: code=${String(
              code,
            )} signal=${String(signal)} stderr:\n${stderr}`,
          ),
        );
      }
    });
  });

const commandArgs = process.argv.slice(2);
if (commandArgs.length === 0) {
  throw new Error("Usage: run-xvfb-command.mjs <command> [args...]");
}
if (process.env.DISPLAY === undefined || process.env.DISPLAY === "") {
  throw new Error("DISPLAY must be set by xvfb-run before running this script");
}

await ensureWindowManagerBuilt();
const windowManager = await startWindowManager();
const [command, ...args] = commandArgs;
let code;
try {
  code = await runProcess(command, args, {
    env: createXvfbCommandEnvironment(process.env),
    stdio: "inherit",
  });
} finally {
  windowManager.kill("SIGTERM");
}

process.exitCode = code;
