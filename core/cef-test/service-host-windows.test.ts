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
  prepareWindowsE2eSession,
  resolveWindowsE2eEnvironment,
  type WindowsE2eConnection,
} from "../../scripts/windows-e2e-environment.ts";

const execute = promisify(execFile);
let nodeAssets: string;
const environment = resolveWindowsE2eEnvironment(process.env);
let connection: WindowsE2eConnection;

const prepareNodeAssets = async () => {
  // Build before reserving the machine: missing host support must fail even
  // when no Windows test session is available. The npm test:windows-services
  // entry point compiles both architectures before running this suite.
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
          "server-node": resolve("../../sdk/examples/services/server.ts"),
          "http-node": resolve("../../sdk/examples/services/http.ts"),
          "node-driver": resolve("../service-host/node-driver.ts"),
          service: resolve("../../node/test/fixtures/node-service.ts"),
          "draining-listener": resolve(
            "../../node/test/fixtures/node-draining-listener.ts",
          ),
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
  await execute(process.execPath, [
    "../../sdk/scripts/build-service-runtime-fixture.mjs",
    join(nodeAssets, "http-quick.js"),
    "--entry",
    "../../sdk/examples/services/http.ts",
  ]);
  await execute(process.execPath, [
    "../../sdk/scripts/build-service-runtime-fixture.mjs",
    join(nodeAssets, "server-quick.js"),
    "--entry",
    "../../sdk/examples/services/server.ts",
  ]);
  for (const name of ["localhost-cert.pem", "localhost-key.pem"])
    await copyFile(
      join("../service-host/test-assets", name),
      join(nodeAssets, name),
    );
  await copyFile(
    resolve("../../node/dist/node-service-bridge.mjs"),
    join(nodeAssets, "node-service-bridge.mjs"),
  );
};

beforeAll(async () => {
  connection = await prepareWindowsE2eSession(environment, prepareNodeAssets);
  if (connection.status === "skip")
    process.stderr.write(
      `Skipping Windows service host tests: ${connection.reason}\n`,
    );
}, 600000);

afterAll(async () => {
  if (connection?.status === "connected") connection.agent.release();
  if (nodeAssets) await rm(nodeAssets, { recursive: true, force: true });
});

it("hosts native and Node services independently on both Windows architectures", async ({
  skip,
}) => {
  if (connection.status === "skip") return skip(connection.reason);
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
    await agent.files.remove(directory, { recursive: true });
  }
}, 300000);
