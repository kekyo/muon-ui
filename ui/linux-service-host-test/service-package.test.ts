// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { spawn } from "node:child_process";
import { once } from "node:events";
import {
  cp,
  mkdir,
  mkdtemp,
  readFile,
  rename,
  rm,
  symlink,
  writeFile,
} from "node:fs/promises";
import { createConnection } from "node:net";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { createInterface } from "node:readline";
import { expect, it } from "vitest";
import { buildService, createServiceProject } from "muon-plugin-sdk";
import { resolveMuonServices } from "../common/service-config.ts";
import { stageMuonDesktopServices } from "../cef/services.ts";
import { createMuonServicesApi } from "../../core/common/renderer/services.ts";

it.each(["native", "source", "node"] as const)(
  "loads an SDK %s service from a relocated desktop distribution",
  async (runtime) => {
    const root = await mkdtemp(join(tmpdir(), "muon-service-package-"));
    let host: ReturnType<typeof spawn> | undefined;
    try {
      const producer = join(root, "producer");
      let definition: Record<string, unknown>;
      if (runtime !== "node") {
        await createServiceProject({
          directory: producer,
          name: "worker",
          language: "c",
          buildSystem: "make",
        });
        if (runtime === "source") {
          await symlink(
            resolve("../node_modules"),
            join(root, "node_modules"),
            "dir",
          );
          await cp(
            resolve("../sdk/examples/services/native-echo.c"),
            join(producer, "service.c"),
          );
          definition = {
            runtime: "native",
            source: "./producer/muon-plugin.build.json",
            autostart: true,
          };
        } else {
          const artifact = await buildService({
            configPath: join(producer, "muon-plugin.build.json"),
            target: "linux-amd64",
          });
          definition = { runtime, artifact, autostart: true };
        }
      } else {
        await mkdir(producer);
        await writeFile(
          join(producer, "package.json"),
          JSON.stringify({ name: "worker", type: "module" }),
        );
        await symlink(
          resolve("../node_modules"),
          join(producer, "node_modules"),
          "dir",
        );
        await cp(
          resolve("../sdk/examples/services/node-files.ts"),
          join(producer, "worker.ts"),
        );
        await cp(
          resolve("../sdk/examples/services/node-greeting.txt"),
          join(producer, "node-greeting.txt"),
        );
        definition = {
          runtime,
          project: producer,
          entry: "./worker.ts",
          autostart: true,
        };
      }
      const definitions = await resolveMuonServices(
        {
          worker: definition,
        },
        root,
        ["linux-amd64"],
      );
      const original = join(root, "original");
      await stageMuonDesktopServices(definitions, "linux-amd64", original);
      if (runtime === "node") {
        await mkdir(join(original, "runtimes/node/bin"), { recursive: true });
        await cp(process.execPath, join(original, "runtimes/node/bin/node"));
        await mkdir(join(original, "plugins"));
        await cp(
          resolve("../node/dist/node-service-bridge.mjs"),
          join(original, "node-service-bridge.mjs"),
        );
      }
      const relocated = join(root, "relocated");
      await rename(original, relocated);
      await rm(producer, { recursive: true, force: true });
      await rm(join(root, ".muon"), { recursive: true, force: true });
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
      if (runtime === "node" || runtime === "source") {
        let sequence = 0;
        const waiting = new Map<
          string,
          { resolve: (value: unknown) => void; reject: (error: Error) => void }
        >();
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
            job.reject(new Error("Service host disconnected"));
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
        const service = await api.acquire("worker");
        expect(
          await service.call(runtime === "node" ? "greet" : "echo", [
            "distribution",
          ]),
        ).toBe(runtime === "node" ? "Hello distribution" : "distribution");
        await service.release();
      }
      socket.write('{"requestId":"shutdown","op":"shutdown"}\n');
      await exit;
      socket.destroy();
      expect(host.exitCode, diagnostic).toBe(0);
      if (runtime === "node")
        expect(
          await readFile(join(root, "state/worker/last-name.txt"), "utf8"),
        ).toBe("distribution");
    } finally {
      if (host && host.exitCode === null && host.signalCode === null) {
        const exit = once(host, "exit");
        host.kill("SIGKILL");
        await exit;
      }
      await rm(root, { recursive: true, force: true });
    }
  },
);
