// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile } from "node:child_process";
import { randomUUID } from "node:crypto";
import { readFile, readdir } from "node:fs/promises";
import { resolve, win32 } from "node:path";
import { promisify } from "node:util";
import { beforeAll, expect, it } from "vitest";
import {
  connectWindowsE2eAgent,
  resolveWindowsE2eEnvironment,
} from "../../scripts/windows-e2e-environment.ts";

const execute = promisify(execFile);

beforeAll(async () => {
  // Build before reserving the machine: missing host support must fail even
  // when no Windows test session is available. Compiling both architectures
  // has its own preparation budget and must not consume the runtime deadline.
  await execute(
    "make",
    ["-C", "../service-host", "windows-amd64", "windows-i686"],
    {
      maxBuffer: 4 * 1024 * 1024,
    },
  );
}, 600000);

it("hosts native services independently on both Windows architectures", async () => {
  const environment = resolveWindowsE2eEnvironment(process.env);
  const connection = await connectWindowsE2eAgent(environment);
  if (connection.status !== "connected") throw new Error(connection.reason);
  const agent = connection.agent;
  const directory = win32.join(
    environment.workDir,
    "native-services-" + randomUUID(),
  );
  try {
    await agent.files.mkdir(directory, { recursive: true });
    await agent.files.writeFile(
      win32.join(directory, "windows-driver.ts"),
      await readFile(resolve("../service-host/windows-driver.ts")),
    );
    for (const target of ["windows-amd64", "windows-i686"]) {
      const output = resolve("../service-host/.build", target);
      const remote = win32.join(directory, target);
      await agent.files.mkdir(remote, { recursive: true });
      for (const name of await readdir(output)) {
        if (!/\.(exe|dll)$/.test(name)) continue;
        await agent.files.writeFile(
          win32.join(remote, name),
          await readFile(resolve(output, name)),
        );
      }
      const process = await agent.processes.launchManaged({
        path: environment.nodePath,
        arguments: [win32.join(directory, "windows-driver.ts"), remote],
        workingDirectory: directory,
        captureStdout: true,
        captureStderr: true,
        createNoWindow: true,
      });
      try {
        const finished = await process.waitForExit({
          timeoutMs: 120000,
          intervalMs: 250,
        });
        const stdout = await process.stdoutText();
        const stderr = await process.stderrText();
        expect(
          finished.root.exitCode,
          target + "\n" + stdout + "\n" + stderr,
        ).toBe(0);
        expect(stdout).toContain('"passed":true');
        console.info(target + "\n" + stdout);
      } finally {
        await process.releaseAsync();
      }
    }
  } finally {
    try {
      await agent.files.remove(directory, { recursive: true });
    } finally {
      await agent.release();
    }
  }
}, 300000);
