// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile, spawn } from "node:child_process";
import { once } from "node:events";
import { mkdir, mkdtemp, rename, rm } from "node:fs/promises";
import { createConnection } from "node:net";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { createInterface } from "node:readline";
import { promisify } from "node:util";
import { beforeAll, expect, it } from "vitest";
import { buildService, createServiceProject } from "muon-plugin-sdk";
import { resolveMuonServices } from "../common/service-config.ts";
import { stageMuonDesktopServices } from "../cef/services.ts";

beforeAll(async () => {
  // A clean SDK build can refresh the host's native dependencies. Keep build
  // time separate from the runtime behavior deadline and build only this host.
  await mkdir("../core/service-host/.build", { recursive: true });
  await promisify(execFile)("flock", [
    "../core/service-host/.build/driver-build.lock",
    "make",
    "-C",
    "../core/service-host",
    ".build/muon-service-host",
  ]);
}, 600000);

it("loads an SDK service from a relocated desktop distribution", async () => {
  const root = await mkdtemp(join(tmpdir(), "muon-service-package-"));
  let host: ReturnType<typeof spawn> | undefined;
  try {
    const producer = join(root, "producer");
    await createServiceProject({
      directory: producer,
      name: "worker",
      language: "c",
      buildSystem: "make",
    });
    const artifact = await buildService({
      configPath: join(producer, "muon-plugin.build.json"),
      target: "linux-amd64",
    });
    const definitions = await resolveMuonServices(
      {
        worker: { runtime: "native", artifact, autostart: true },
      },
      root,
      ["linux-amd64"],
    );
    const original = join(root, "original");
    await stageMuonDesktopServices(definitions, "linux-amd64", original);
    const relocated = join(root, "relocated");
    await rename(original, relocated);
    await rm(producer, { recursive: true, force: true });
    const endpoint = join(root, "control.sock");
    host = spawn(
      resolve("../core/service-host/.build/muon-service-host"),
      [
        "--config",
        join(relocated, "muon-services.json"),
        "--endpoint",
        endpoint,
        "--data-root",
        join(root, "state"),
      ],
      { stdio: ["ignore", "pipe", "pipe"] },
    );
    const exit = once(host, "exit");
    let diagnostic = "";
    host.stderr!.on("data", (chunk: Buffer) => {
      diagnostic += chunk.toString();
    });
    const lines = createInterface({ input: host.stdout! });
    const running = new Promise<Record<string, unknown>>((resolve) => {
      lines.on("line", (line) => {
        const state = JSON.parse(line) as Record<string, unknown>;
        if (
          state.id === "worker" &&
          (state.state === "running" || state.state === "failed")
        )
          resolve(state);
      });
    });
    const result = await Promise.race([
      running,
      (async () => {
        await exit;
        throw new Error(diagnostic);
      })(),
    ]);
    expect(result, diagnostic).toMatchObject({
      state: "running",
      resident: true,
    });
    const socket = createConnection(endpoint);
    await once(socket, "connect");
    socket.write('{"requestId":"1","op":"shutdown"}\n');
    await exit;
    socket.destroy();
    expect(host.exitCode, diagnostic).toBe(0);
  } finally {
    if (host && host.exitCode === null && host.signalCode === null) {
      const exit = once(host, "exit");
      host.kill("SIGKILL");
      await exit;
    }
    await rm(root, { recursive: true, force: true });
  }
});
