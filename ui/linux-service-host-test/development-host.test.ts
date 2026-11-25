// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { spawn } from "node:child_process";
import { once } from "node:events";
import { mkdtemp, rm, writeFile } from "node:fs/promises";
import { createConnection } from "node:net";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { createInterface } from "node:readline";
import { expect, it } from "vitest";
import { createMuonServicesApi } from "../../core/common/renderer/services.ts";
import { buildMuonQuickJsService } from "../common/service-build.ts";

it.each([false, true])(
  "owns a development host until its services or session end (resident=%s)",
  async (resident) => {
    const directory = await mkdtemp(join(tmpdir(), "muon-dev-host-"));
    const endpoint = join(directory, "control");
    const registry = join(directory, "services.json");
    await writeFile(
      join(directory, "worker.mjs"),
      await buildMuonQuickJsService(
        resolve("../sdk/test/fixtures/quickjs-service.ts"),
        [],
      ),
    );
    await writeFile(
      registry,
      JSON.stringify({
        services: {
          worker: {
            runtime: "quickjs",
            script: "worker.mjs",
            autostart: resident,
          },
        },
      }),
    );
    const host = spawn(
      resolve("../core/service-host/.build/muon-service-host"),
      [
        "--config",
        registry,
        "--endpoint",
        endpoint,
        "--data-root",
        join(directory, "data"),
        "--bootstrap",
        "development",
      ],
      { stdio: ["pipe", "pipe", "pipe"] },
    );
    const exited = once(host, "close");
    let diagnostics = "";
    host.stderr.on("data", (bytes: Buffer) => {
      diagnostics += bytes.toString();
    });
    const output = createInterface({ input: host.stdout });
    const ready = new Promise<void>((resolve) => {
      output.on("line", (line) => {
        if (JSON.parse(line).ready) resolve();
      });
    });
    const sockets: ReturnType<typeof createConnection>[] = [];
    try {
      await Promise.race([
        ready,
        (async () => {
          await exited;
          throw new Error(diagnostics || "Host exited before readiness");
        })(),
      ]);
      const page = async () => {
        const socket = createConnection(endpoint);
        sockets.push(socket);
        await once(socket, "connect");
        const waiting = new Map<
          string,
          {
            resolve: (value: unknown) => void;
            reject: (reason: unknown) => void;
          }
        >();
        let sequence = 0;
        createInterface({ input: socket }).on("line", (line) => {
          const reply = JSON.parse(line);
          const completion = waiting.get(reply.requestId);
          if (!completion) return;
          waiting.delete(reply.requestId);
          if (reply.error) completion.reject(new Error(reply.error.message));
          else completion.resolve(reply.result);
        });
        socket.on("close", () => {
          for (const job of waiting.values())
            job.reject(new Error("Page disconnected"));
          waiting.clear();
        });
        const api = createMuonServicesApi(async (op, json) => {
          const requestId = String(++sequence);
          const response = new Promise<unknown>((resolve, reject) => {
            waiting.set(requestId, { resolve, reject });
          });
          socket.write(
            JSON.stringify({ ...JSON.parse(json), op, requestId }) + "\n",
          );
          return JSON.stringify(await response);
        });
        return { service: await api.acquire("worker"), socket };
      };
      const first = await page();
      expect(await first.service.call("echo", ["before UI close"])).toBe(
        "before UI close",
      );
      const generation = first.service.generation;
      await first.service.release();
      first.socket.destroy();
      // Detach releases the startup/UI guard but leaves session ownership intact.
      host.stdin.write("d");
      if (resident) {
        const second = await page();
        expect(second.service.generation).toBe(generation);
        expect(await second.service.call("echo", ["after UI close"])).toBe(
          "after UI close",
        );
        await second.service.release();
        second.socket.destroy();
        // EOF also occurs when the development supervisor is killed. It must
        // stop resident services, even though they outlive every page.
        host.stdin.end();
      }
      expect(await exited, diagnostics).toEqual([0, null]);
    } finally {
      for (const socket of sockets) socket.destroy();
      if (host.exitCode === null && host.signalCode === null) {
        host.kill("SIGKILL");
        await exited;
      }
      output.close();
      await rm(directory, { recursive: true, force: true });
    }
  },
);
