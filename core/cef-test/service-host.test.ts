// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile, spawn, type ChildProcess } from "node:child_process";
import { randomUUID } from "node:crypto";
import { once } from "node:events";
import { mkdtemp, rm, writeFile } from "node:fs/promises";
import { createConnection, createServer } from "node:net";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { createInterface } from "node:readline";
import { promisify } from "node:util";
import { afterEach, expect, it } from "vitest";

const execute = promisify(execFile);
const children: ChildProcess[] = [];
const directories: string[] = [];

afterEach(async () => {
  for (const child of children.splice(0)) {
    if (child.exitCode === null && child.signalCode === null) {
      const stopped = once(child, "exit");
      child.kill("SIGKILL");
      await stopped;
    }
  }
  for (const directory of directories.splice(0))
    await rm(directory, { recursive: true, force: true });
});

it.each(["command", "signal", "backpressure"])(
  "owns services after UI death and drains them on %s shutdown",
  async (shutdown) => {
    const directory = await mkdtemp(join(tmpdir(), "muon-native-host-"));
    directories.push(directory);
    const config = join(directory, "services.json");
    const endpoint = join(directory, "control.sock");
    const library = resolve("../service-host/.build/probe-service.so");
    const ports: number[] = [];
    const reservations = [createServer(), createServer()];
    for (const server of reservations) {
      server.listen(0, "127.0.0.1");
      await once(server, "listening");
      const address = server.address();
      if (!address || typeof address === "string")
        throw new Error("Missing TCP port");
      ports.push(address.port);
    }
    for (const server of reservations) {
      const closed = once(server, "close");
      server.close();
      await closed;
    }
    await writeFile(
      config,
      JSON.stringify({
        services: {
          resident: {
            library,
            autostart: true,
            config: { port: String(ports[0]) },
          },
          borrowed: {
            library,
            autostart: false,
            config: { port: String(ports[1]) },
          },
        },
      }),
    );
    const binary = resolve("../service-host/.build/muon-service-host");
    const args = [
      "--config",
      config,
      "--endpoint",
      endpoint,
      "--data-root",
      join(directory, "data"),
      ...(shutdown === "signal" ? ["--bootstrap", "login"] : []),
    ];
    const host = spawn(binary, args, { stdio: ["pipe", "pipe", "pipe"] });
    children.push(host);
    const hostExit = once(host, "exit");
    let diagnostic = "";
    host.stderr!.on("data", (data: Buffer) => {
      diagnostic += data.toString();
    });
    const observations: Record<string, unknown>[] = [];
    const waiters: {
      match: (value: Record<string, unknown>) => boolean;
      resolve: (value: Record<string, unknown>) => void;
    }[] = [];
    const lines = createInterface({ input: host.stdout! });
    lines.on("line", (line) => {
      const value = JSON.parse(line) as Record<string, unknown>;
      observations.push(value);
      for (const waiter of [...waiters]) {
        if (waiter.match(value)) {
          waiters.splice(waiters.indexOf(waiter), 1);
          waiter.resolve(value);
        }
      }
    });
    const observed = async (
      match: (value: Record<string, unknown>) => boolean,
    ): Promise<Record<string, unknown>> => {
      const existing = observations.find(match);
      if (existing) return existing;
      return await Promise.race([
        new Promise<Record<string, unknown>>((resolve) => {
          waiters.push({ match, resolve });
        }),
        (async () => {
          await hostExit;
          throw new Error(
            "Service host exited before observation: " + diagnostic,
          );
        })(),
      ]);
    };
    await observed((value) => value.ready === true);
    const running = await observed(
      (value) => value.id === "resident" && value.state === "running",
    );
    await expect(execute(binary, args)).rejects.toThrow();

    // This process represents a page transport. Its death must be detected by
    // the host without a JavaScript release or an orderly disconnect callback.
    const page = spawn(
      process.execPath,
      [
        "--input-type=module",
        "-e",
        `
    import {createConnection} from 'node:net';
    import {createInterface} from 'node:readline';
    const socket=createConnection(process.argv[1]);
    socket.on('connect',()=>socket.write(JSON.stringify({requestId:'page',op:'acquire',serviceId:'borrowed'})+String.fromCharCode(10)));
    createInterface({input:socket}).on('line',line=>{if(JSON.parse(line).requestId)process.stdout.write(line+String.fromCharCode(10));});
  `,
        endpoint,
      ],
      { stdio: ["ignore", "pipe", "pipe"] },
    );
    children.push(page);
    const pageExit = once(page, "exit");
    const pageLines = createInterface({ input: page.stdout! });
    const [reply] = await once(pageLines, "line");
    expect(JSON.parse(reply)).toHaveProperty("result.serviceId", "borrowed");
    page.kill("SIGKILL");
    await pageExit;
    await observed(
      (value) => value.id === "borrowed" && value.state === "stopped",
    );

    // The very same C++ source is executed by the Android service-driver APK.
    // A fresh nonce proves work was performed after the page process died.
    const probe = async (): Promise<Record<string, unknown>> => {
      const business = createConnection({ host: "127.0.0.1", port: ports[0]! });
      await once(business, "connect");
      const response = once(createInterface({ input: business }), "line");
      const nonce = randomUUID();
      business.write(nonce + "\n");
      const [line] = await response;
      business.destroy();
      const result = JSON.parse(line) as Record<string, unknown>;
      expect(result).toMatchObject({
        nonce,
        pid: host.pid,
        generation: running.generation,
      });
      return result;
    };
    const workAfterDeath = await probe();

    const nativeClients = await execute(
      resolve("../service-host/.build/linux-client-driver"),
      [endpoint],
    );
    expect(nativeClients.stderr).toBe("");

    const socket = createConnection(endpoint);
    await once(socket, "connect");
    const replies = createInterface({ input: socket })[Symbol.asyncIterator]();
    socket.write(
      '{"requestId":"handshake","op":"getState","serviceId":"resident"}\n',
    );
    expect(JSON.parse((await replies.next()).value!)).toEqual({
      event: "ready",
    });
    expect(JSON.parse((await replies.next()).value!)).toHaveProperty(
      "requestId",
      "handshake",
    );
    let nextRequest = 0;
    const request = async (
      input: Record<string, unknown>,
    ): Promise<Record<string, unknown>> => {
      const requestId = String(++nextRequest);
      socket.write(JSON.stringify({ requestId, ...input }) + "\n");
      for (;;) {
        const line = await replies.next();
        if (line.done)
          throw new Error("Service host disconnected: " + diagnostic);
        const response = JSON.parse(line.value) as Record<string, unknown>;
        if (response.requestId === requestId) return response;
      }
    };
    try {
      const acquired = await request({ op: "acquire", serviceId: "resident" });
      const reference = acquired.result as Record<string, unknown>;
      expect(reference.generation).toBe(running.generation);
      const workAfterReconnect = await probe();
      expect(Number(workAfterReconnect.count)).toBeGreaterThan(
        Number(workAfterDeath.count),
      );
      await request({ op: "release", ...reference });
      const state = await request({ op: "getState", serviceId: "resident" });
      expect(state.result).toMatchObject({
        state: "running",
        resident: true,
        clientReferences: 0,
      });
      let stalled: ReturnType<typeof createConnection> | undefined;
      if (shutdown === "backpressure") {
        stalled = createConnection(endpoint);
        await once(stalled, "connect");
        stalled.pause();
        // The final acquire is an observable barrier after all state replies
        // have been queued for a client that refuses to read them.
        stalled.write(
          Array.from({ length: 4096 }, (_, index) =>
            JSON.stringify({
              requestId: String(index),
              op: "getState",
              serviceId: "resident",
            }),
          ).join("\n") +
            '\n{"requestId":"barrier","op":"acquire","serviceId":"borrowed"}\n',
        );
        await observed(
          (value) =>
            value.id === "borrowed" &&
            value.state === "running" &&
            value.generation !==
              (JSON.parse(reply).result as { generation: string }).generation,
        );
      }
      if (shutdown !== "signal")
        socket.write('{"requestId":"shutdown","op":"shutdown"}\n');
      else host.kill("SIGTERM");
      const [code] = await hostExit;
      stalled?.destroy();
      expect(code, diagnostic).toBe(0);
      const residentStops = diagnostic
        .split(/\r?\n/)
        .filter((line) => line.startsWith("resident: lifecycle "));
      expect(residentStops).toEqual([
        `resident: lifecycle ${running.generation} ${shutdown === "signal" ? "login-autostart host-stop SIGTERM" : "app-start app-exit"}`,
      ]);
      expect(diagnostic).toContain(
        `borrowed: lifecycle ${JSON.parse(reply).result.generation} acquire reference-released`,
      );
      expect(
        observations.some(
          (value) => value.id === "resident" && value.state === "stopped",
        ),
      ).toBe(true);
    } finally {
      socket.destroy();
    }
  },
  20_000,
);

it("reclaims a failed native startup and exits after its final client disconnects", async () => {
  const directory = await mkdtemp(join(tmpdir(), "muon-native-host-failure-"));
  directories.push(directory);
  const endpoint = join(directory, "control.sock");
  const config = join(directory, "services.json");
  await writeFile(
    config,
    JSON.stringify({
      services: {
        worker: {
          library: resolve("../service-host/.build/probe-service.so"),
          autostart: false,
          // The native fixture rejects startup without its required port.
          config: {},
        },
      },
    }),
  );
  const host = spawn(
    resolve("../service-host/.build/muon-service-host"),
    [
      "--config",
      config,
      "--endpoint",
      endpoint,
      "--data-root",
      join(directory, "data"),
    ],
    { stdio: ["ignore", "pipe", "pipe"] },
  );
  children.push(host);
  const exit = once(host, "exit");
  const output = createInterface({ input: host.stdout! });
  await once(output, "line");
  const socket = createConnection(endpoint);
  await once(socket, "connect");
  const replies = createInterface({ input: socket })[Symbol.asyncIterator]();
  try {
    socket.write(
      JSON.stringify({ requestId: "1", op: "acquire", serviceId: "worker" }) +
        "\n",
    );
    expect(JSON.parse((await replies.next()).value!)).toEqual({
      event: "ready",
    });
    const failed = await replies.next();
    expect(JSON.parse(failed.value!)).toMatchObject({
      requestId: "1",
      error: { code: "unavailable" },
    });
    socket.write(
      JSON.stringify({ requestId: "2", op: "getState", serviceId: "worker" }) +
        "\n",
    );
    const observed = await replies.next();
    expect(JSON.parse(observed.value!)).toMatchObject({
      requestId: "2",
      result: { state: "failed", resident: false, clientReferences: 0 },
    });
    socket.end();
    expect((await exit)[0]).toBe(0);
  } finally {
    socket.destroy();
  }
});
