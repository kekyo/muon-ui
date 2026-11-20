// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile, spawn } from "node:child_process";
import { randomUUID } from "node:crypto";
import { once } from "node:events";
import { mkdtemp, rm, writeFile } from "node:fs/promises";
import { createConnection } from "node:net";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { createInterface } from "node:readline";
import { promisify } from "node:util";
import { expect, it } from "vitest";

const execute = promisify(execFile);

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

it("joins the same resident host when independent processes launch concurrently", async () => {
  const directory = await mkdtemp(join(tmpdir(), "muon-service-launch-race-"));
  const endpoint = join(directory, "control.sock");
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
    const args = [
      "join",
      resolve("../service-host/.build/muon-service-host"),
      registry,
      endpoint,
      join(directory, "data"),
    ];
    // Both callers explicitly reach the launch path, so this reproduces the
    // MissingEndpoint race even when the OS schedules one caller much later.
    const launch = async () => {
      // A resident host inherits diagnostics, so waiting for stderr EOF would
      // wait for the service lifetime instead of the launcher's exit.
      const child = spawn(
        resolve("../service-host/.build/linux-process-driver"),
        args,
        { stdio: ["ignore", "pipe", "inherit"] },
      );
      let stdout = "";
      child.stdout.on("data", (bytes: Buffer) => {
        stdout += bytes.toString();
      });
      const [code] = await once(child, "exit");
      expect(code).toBe(0);
      return { stdout };
    };
    const results = await Promise.allSettled([launch(), launch()]);
    for (const result of results) {
      if (result.status === "rejected") throw result.reason;
      expect(result.value.stdout).toContain('"joined":true');
    }
  } finally {
    const socket = createConnection(endpoint);
    try {
      await once(socket, "connect");
      const ended = once(socket, "end");
      socket.resume();
      socket.write('{"requestId":"cleanup","op":"shutdown"}\n');
      await ended;
    } catch (error) {
      if ((error as NodeJS.ErrnoException).code !== "ENOENT") throw error;
    } finally {
      socket.destroy();
    }
    await rm(directory, { recursive: true, force: true });
  }
}, 20_000);

it("cancels a competing launch wait and acquires after its owner process dies", async () => {
  const key = randomUUID();
  const executable = resolve("../service-host/.build/process-lock-driver");
  const holder = spawn(executable, [key, "hold"]);
  const holderExit = once(holder, "exit");
  const holderLines = createInterface({ input: holder.stdout })[
    Symbol.asyncIterator
  ]();
  let waiter: ReturnType<typeof spawn> | undefined;
  try {
    expect((await holderLines.next()).value).toBe("waiting");
    expect((await holderLines.next()).value).toBe("locked");
    const canceled = await execute(executable, [key, "cancel"]);
    expect(canceled.stdout).toContain("canceled");
    waiter = spawn(executable, [key, "wait"]);
    const waiterExit = once(waiter, "exit");
    const waiterLines = createInterface({ input: waiter.stdout! })[
      Symbol.asyncIterator
    ]();
    expect((await waiterLines.next()).value).toBe("waiting");
    holder.kill("SIGKILL");
    await holderExit;
    expect((await waiterLines.next()).value).toBe("locked");
    expect((await waiterExit)[0]).toBe(0);
    const local = await execute(executable, [key, "local"]);
    expect(local.stdout).toBe("waiting\ncanceled\nlocked\n");
  } finally {
    if (holder.exitCode === null && holder.signalCode === null) {
      holder.kill("SIGKILL");
      await holderExit;
    }
    if (waiter && waiter.exitCode === null && waiter.signalCode === null) {
      const exit = once(waiter, "exit");
      waiter.kill("SIGKILL");
      await exit;
    }
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
