// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile, spawn } from "node:child_process";
import { once } from "node:events";
import { mkdtemp, rm, writeFile } from "node:fs/promises";
import { createConnection } from "node:net";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { createInterface } from "node:readline";
import { promisify } from "node:util";
import { beforeAll, expect, it } from "vitest";

const execute = promisify(execFile);
beforeAll(async () => {
  await execute("make", ["-C", "../service-host", "all"]);
});

it("launches a host once, reopens the UI, and awaits explicit shutdown through native sessions", async () => {
  const directory = await mkdtemp(join(tmpdir(), "muon-service-session-"));
  try {
    const library = join(directory, "fixture.so");
    await execute("cc", [
      "-std=c99",
      "-shared",
      "-fPIC",
      "-I../../sdk/include",
      "../common-test/muon_service_fixture.c",
      "-o",
      library,
    ]);
    const registry = join(directory, "services.json");
    await writeFile(
      registry,
      JSON.stringify({ services: { worker: { library, autostart: true } } }),
    );
    await execute(resolve("../service-host/.build/linux-process-driver"), [
      resolve("../service-host/.build/muon-service-host"),
      registry,
      join(directory, "control.sock"),
      join(directory, "data"),
    ]);
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
}, 20_000);

it.each([false, true])(
  "releases bootstrap I/O without killing resident=%s services",
  async (resident) => {
    const directory = await mkdtemp(join(tmpdir(), "muon-service-bootstrap-"));
    const library = join(directory, "fixture.so");
    await execute("cc", [
      "-std=c99",
      "-shared",
      "-fPIC",
      "-I../../sdk/include",
      "../common-test/muon_service_fixture.c",
      "-o",
      library,
    ]);
    const registry = join(directory, "services.json");
    const endpoint = join(directory, "control.sock");
    await writeFile(
      registry,
      JSON.stringify({
        services: { worker: { library, autostart: resident } },
      }),
    );
    const child = spawn(
      resolve("../service-host/.build/muon-service-host"),
      [
        "--config",
        registry,
        "--endpoint",
        endpoint,
        "--data-root",
        join(directory, "data"),
        "--bootstrap",
        "stdin",
      ],
      { stdio: ["pipe", "pipe", "pipe"] },
    );
    const exit = once(child, "exit");
    let diagnostic = "";
    child.stderr.on("data", (bytes: Buffer) => {
      diagnostic += bytes.toString();
    });
    try {
      const lines = createInterface({ input: child.stdout });
      for await (const line of lines) {
        if ((JSON.parse(line) as { ready?: boolean }).ready) break;
      }
      // The UI can disappear between process readiness and its first connection.
      child.stdout.destroy();
      child.stdin.end();
      if (resident) {
        const socket = createConnection(endpoint);
        const replies = createInterface({ input: socket })[
          Symbol.asyncIterator
        ]();
        try {
          await once(socket, "connect");
          expect(JSON.parse((await replies.next()).value!)).toEqual({
            event: "ready",
          });
          socket.write(
            '{"requestId":"acquire","op":"acquire","serviceId":"worker"}\n',
          );
          const response = JSON.parse((await replies.next()).value!) as {
            result: unknown;
          };
          expect(response.result).toHaveProperty("serviceId", "worker");
          socket.write('{"requestId":"stop","op":"shutdown"}\n');
          const [code] = await exit;
          expect(code, diagnostic).toBe(0);
        } finally {
          socket.destroy();
        }
      } else {
        const [code] = await exit;
        expect(code, diagnostic).toBe(0);
      }
    } finally {
      if (child.exitCode === null && child.signalCode === null) {
        child.kill("SIGKILL");
        await exit;
      }
      await rm(directory, { recursive: true, force: true });
    }
  },
  20_000,
);
