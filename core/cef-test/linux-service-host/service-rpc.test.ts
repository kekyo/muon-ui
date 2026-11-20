// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile, spawn } from "node:child_process";
import { once } from "node:events";
import { mkdtemp, rm, writeFile } from "node:fs/promises";
import { createConnection, createServer } from "node:net";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { createInterface } from "node:readline";
import { promisify } from "node:util";
import { expect, it } from "vitest";
import { createMuonServicesApi } from "../../common/renderer/services.ts";

const execute = promisify(execFile);

it.each(["native", "quickjs"] as const)(
  "routes typed %s calls, callbacks and events across independent page sockets",
  async (runtime) => {
    const directory = await mkdtemp(join(tmpdir(), "muon-service-rpc-"));
    const endpoint = join(directory, "service.sock");
    const config = join(directory, "services.json");
    const port = createServer();
    port.listen(0, "127.0.0.1");
    await once(port, "listening");
    const address = port.address();
    if (!address || typeof address === "string")
      throw new Error("No service port");
    const portClosed = once(port, "close");
    port.close();
    await portClosed;
    const script = join(directory, "service.mjs");
    if (runtime === "quickjs")
      await execute(process.execPath, [
        "../../sdk/scripts/build-service-runtime-fixture.mjs",
        script,
      ]);
    const fault = join(directory, "fault.mjs");
    if (runtime === "quickjs")
      await writeFile(
        fault,
        `
      globalThis.__muonServiceInterface = {schemaVersion:1,methods:{spin:{parameters:[],returns:'void',errors:[]}},events:{}};
      globalThis.__muonServiceRuntime = {
        async start(){},
        invoke(){ void (async()=>{ for(;;){} })(); return new Promise(()=>{}); },
        cancel(){}, async stop(){}
      };
    `,
      );
    await writeFile(
      config,
      JSON.stringify({
        services: {
          ...(runtime === "quickjs"
            ? {
                fault: { runtime, script: fault, autostart: true },
                native: {
                  runtime: "native",
                  library: resolve("../service-host/.build/probe-service.so"),
                  autostart: true,
                  config: { port: String(address.port) },
                },
              }
            : {}),
          worker: {
            runtime,
            ...(runtime === "native"
              ? { library: resolve("../service-host/.build/probe-service.so") }
              : { script }),
            autostart: true,
            config: { port: String(address.port) },
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
    const exited = once(host, "exit");
    let diagnostics = "";
    host.stderr!.on("data", (data: Buffer) => {
      diagnostics += data.toString();
    });
    const output = createInterface({ input: host.stdout! });
    const sockets: ReturnType<typeof createConnection>[] = [];
    try {
      const ready = new Promise<void>((resolve) => {
        output.on("line", (line) => {
          if (JSON.parse(line).ready) resolve();
        });
      });
      await Promise.race([
        ready,
        (async () => {
          await exited;
          throw new Error(diagnostics);
        })(),
      ]);
      const page = async (serviceId: string) => {
        const socket = createConnection(endpoint);
        sockets.push(socket);
        await once(socket, "connect");
        const waiting = new Map<
          string,
          { resolve: (value: unknown) => void; reject: (error: Error) => void }
        >();
        let sequence = 0;
        createInterface({ input: socket }).on("line", (line) => {
          try {
            const message = JSON.parse(line);
            if (!message.requestId) return;
            const completion = waiting.get(message.requestId);
            if (!completion) throw new Error("Unexpected service reply");
            waiting.delete(message.requestId);
            if (message.error)
              completion.reject(new Error(message.error.message));
            else completion.resolve(message.result);
          } catch (error) {
            for (const completion of waiting.values())
              completion.reject(error as Error);
            waiting.clear();
            socket.destroy();
          }
        });
        socket.on("close", () => {
          for (const completion of waiting.values())
            completion.reject(new Error("Page disconnected"));
          waiting.clear();
        });
        const request = (
          op: string,
          fields: Record<string, unknown>,
        ): Promise<unknown> => {
          if (socket.destroyed)
            return Promise.reject(new Error("Page disconnected"));
          const requestId = String(++sequence);
          const result = new Promise<unknown>((resolve, reject) => {
            waiting.set(requestId, { resolve, reject });
          });
          socket.write(JSON.stringify({ ...fields, requestId, op }) + "\n");
          return result;
        };
        const api = createMuonServicesApi(async (name, json) =>
          JSON.stringify(await request(name, JSON.parse(json))),
        );
        const service = await api.acquire(serviceId);
        return { socket, request, service };
      };
      const first = await page("worker");
      const second = await page("worker");
      expect(first.service.generation).toBe(second.service.generation);
      expect(await first.service.call("echo", ["日本\0🙂"])).toBe("日本\0🙂");
      expect(await first.service.call("echoInt64", [-(1n << 63n)])).toBe(
        -(1n << 63n),
      );
      expect(await second.service.call("echoUint64", [(1n << 64n) - 1n])).toBe(
        (1n << 64n) - 1n,
      );
      const bytes = new Uint8Array(1024 * 1024).fill(197);
      expect(await first.service.call("echoBytes", [bytes])).toEqual(bytes);
      const escaped = "\0".repeat(1024 * 1024);
      expect(await second.service.call("echo", [escaped])).toBe(escaped);
      expect(
        await first.service.call("callback", [
          "outer",
          async (value: unknown) =>
            await second.service.call("callback", [
              value,
              async (nested: unknown) => String(nested) + "-nested",
            ]),
        ]),
      ).toBe("outer-nested");
      const events: unknown[] = [];
      let received!: () => void;
      const notified = new Promise<void>((resolve) => {
        received = resolve;
      });
      const subscription = await second.service.subscribe(
        "progress",
        (value) => {
          events.push(value);
          received();
        },
      );
      await first.service.call("emit", [23]);
      await notified;
      expect(events).toEqual([23]);
      let entered!: () => void;
      const callbackStarted = new Promise<void>((resolve) => {
        entered = resolve;
      });
      const abandoned = (async () => {
        try {
          await first.service.call("callback", [
            "pending",
            () => {
              entered();
              return new Promise(() => {});
            },
          ]);
        } catch (error) {
          return error;
        }
        throw new Error("Abandoned call unexpectedly succeeded");
      })();
      await callbackStarted;
      first.socket.destroy();
      await abandoned;
      await expect(first.service.release()).rejects.toThrow(
        "Page disconnected",
      );
      expect(await second.service.call("echo", ["still-running"])).toBe(
        "still-running",
      );
      if (runtime === "quickjs") {
        const failed = await page("fault");
        await expect(failed.service.call("spin", [])).rejects.toThrow();
        expect(
          await failed.request("getState", { serviceId: "fault" }),
        ).toMatchObject({
          state: "failed",
          resident: true,
          clientReferences: 0,
        });
        const native = await page("native");
        expect(await native.service.call("echo", ["native-survived"])).toBe(
          "native-survived",
        );
        expect(await second.service.call("echo", ["heap-survived"])).toBe(
          "heap-survived",
        );
        await native.service.release();
        await failed.service.release();
      }
      await subscription.close();
      expect(await subscription.closed).toBeUndefined();
      const workerStopped = new Promise<void>((resolve) => {
        output.on("line", (line) => {
          const state = JSON.parse(line);
          if (state.id === "worker" && state.state === "stopped") resolve();
        });
      });
      await second.request("setAutostart", {
        serviceId: "worker",
        enabled: false,
      });
      expect(
        await second.request("getState", { serviceId: "worker" }),
      ).toMatchObject({
        state: "running",
        resident: false,
        clientReferences: 1,
      });
      expect(await second.service.call("echo", ["after-autostart-off"])).toBe(
        "after-autostart-off",
      );
      await second.service.release();
      await workerStopped;
      await second.request("shutdown", {});
      expect((await exited)[0], diagnostics).toBe(0);
    } finally {
      for (const socket of sockets) socket.destroy();
      output.close();
      if (host.exitCode === null && host.signalCode === null) {
        host.kill("SIGKILL");
        await exited;
      }
      await rm(directory, { recursive: true, force: true });
    }
  },
);
