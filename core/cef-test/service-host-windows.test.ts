// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile } from "node:child_process";
import { randomUUID } from "node:crypto";
import {
  copyFile,
  mkdir,
  mkdtemp,
  readFile,
  readdir,
  rm,
  writeFile,
} from "node:fs/promises";
import { join, resolve, win32 } from "node:path";
import { promisify } from "node:util";
import { afterAll, beforeAll, expect, it } from "vitest";
import { build } from "vite";
import {
  connectWindowsE2eAgent,
  resolveWindowsE2eEnvironment,
} from "../../scripts/windows-e2e-environment.ts";

const execute = promisify(execFile);
let nodeAssets: string;

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
  await mkdir("../../.build", { recursive: true });
  nodeAssets = await mkdtemp(resolve("../../.build/node-windows-driver-"));
  await writeFile(
    join(nodeAssets, "package.json"),
    JSON.stringify({ type: "module" }),
  );
  await build({
    configFile: false,
    logLevel: "silent",
    build: {
      outDir: nodeAssets,
      emptyOutDir: false,
      minify: false,
      lib: {
        entry: {
          "node-driver": resolve("../service-host/node-driver.ts"),
          service: resolve("../../node/test/fixtures/node-service.ts"),
          lifecycle: resolve(
            "../../node/test/fixtures/node-lifecycle-service.ts",
          ),
          "disconnect-bridge": resolve(
            "../../node/test/fixtures/node-disconnect-bridge.ts",
          ),
        },
        formats: ["es"],
        fileName: (_format, entry) => entry + ".mjs",
      },
      rolldownOptions: { external: [/^node:/] },
    },
  });
  await copyFile(
    resolve("../../node/dist/node-bridge.mjs"),
    join(nodeAssets, "node-bridge.mjs"),
  );
}, 600000);

afterAll(async () => {
  if (nodeAssets) await rm(nodeAssets, { recursive: true, force: true });
});

it("hosts native and Node services independently on both Windows architectures", async () => {
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
    const assets = win32.join(directory, "node-assets");
    await agent.files.mkdir(assets);
    for (const name of await readdir(nodeAssets)) {
      await agent.files.writeFile(
        win32.join(assets, name),
        await readFile(join(nodeAssets, name)),
      );
    }
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
      const nodeDriver = await agent.processes.launchManaged({
        path: environment.nodePath,
        arguments: [win32.join(assets, "node-driver.mjs"), remote, assets],
        workingDirectory: directory,
        captureStdout: true,
        captureStderr: true,
        createNoWindow: true,
      });
      try {
        const finished = await nodeDriver.waitForExit({
          timeoutMs: 120000,
          intervalMs: 250,
        });
        const stdout = await nodeDriver.stdoutText();
        const stderr = await nodeDriver.stderrText();
        expect(
          finished.root.exitCode,
          target + "\n" + stdout + "\n" + stderr,
        ).toBe(0);
        expect(stdout).toContain('"nodeService":true,"passed":true');
        console.info(target + "\n" + stdout);
      } finally {
        await nodeDriver.releaseAsync();
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
