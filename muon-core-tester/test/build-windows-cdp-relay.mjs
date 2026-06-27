// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import { execFile } from "node:child_process";
import { mkdir } from "node:fs/promises";
import { dirname, resolve } from "node:path";

const sourcePath = resolve("test", "plugin-e2e", "windows-cdp-relay.c");

const run = async (command, args) => {
  await new Promise((resolvePromise, reject) => {
    const child = execFile(command, args, (error, stdout, stderr) => {
      if (error !== null) {
        reject(
          new Error(
            `${command} failed\nstdout:\n${stdout}\nstderr:\n${stderr}`,
          ),
        );
        return;
      }
      resolvePromise();
    });
    child.stdout?.resume();
    child.stderr?.resume();
  });
};

const buildRelay = async (target, compiler) => {
  const outputPath = resolve(
    "test",
    ".run",
    "windows-cdp-relay",
    target,
    "muon-cdp-relay.exe",
  );
  await mkdir(dirname(outputPath), { recursive: true });
  await run(compiler, [
    "-std=c99",
    "-O2",
    "-Wall",
    "-Wextra",
    "-pedantic",
    "-o",
    outputPath,
    sourcePath,
    "-lws2_32",
  ]);
};

await buildRelay("windows32", "i686-w64-mingw32-gcc");
await buildRelay("windows64", "x86_64-w64-mingw32-gcc");
