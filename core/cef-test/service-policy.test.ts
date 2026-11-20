// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile, spawn, type ChildProcess } from "node:child_process";
import { once } from "node:events";
import {
  access,
  mkdtemp,
  readFile,
  rename,
  rm,
  writeFile,
} from "node:fs/promises";
import { createConnection } from "node:net";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { createInterface } from "node:readline";
import { promisify } from "node:util";
import { expect, it } from "vitest";
import {
  createMuonServicesApi,
  type MuonServiceState,
} from "../common/renderer/services.ts";

const execute = promisify(execFile);

it("persists independent autostart overrides through host death while observers own no references", async () => {
  const directory = await mkdtemp(join(tmpdir(), "muon-service-policy-"));
  const children: ChildProcess[] = [];
  const sockets: ReturnType<typeof createConnection>[] = [];
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
    const endpoint = join(directory, "control.sock");
    const data = join(directory, "data");
    const registration = join(directory, "xdg/autostart/service-app.desktop");
    const startup = JSON.stringify({
      executable: join(directory, "service-app"),
      source: "normal",
      id: "",
      arguments: ["--muon-autostart", "-c", registry],
      refresh: true,
    });
    await writeFile(
      registry,
      JSON.stringify({
        services: {
          worker: { library, autostart: true },
          second: { library, autostart: false },
        },
      }),
    );
    const launch = async () => {
      const child = spawn(
        resolve("../service-host/.build/muon-service-host"),
        [
          "--config",
          registry,
          "--endpoint",
          endpoint,
          "--data-root",
          data,
          "--startup",
          startup,
        ],
        {
          stdio: ["ignore", "pipe", "pipe"],
          env: {
            ...process.env,
            XDG_CONFIG_HOME: join(directory, "xdg"),
            XDG_CONFIG_DIRS: join(directory, "system-xdg"),
          },
        },
      );
      children.push(child);
      const exited = once(child, "exit");
      let diagnostic = "";
      child.stderr.on("data", (bytes: Buffer) => {
        diagnostic += bytes.toString();
      });
      const lines = createInterface({ input: child.stdout });
      let readyOutput = false;
      for await (const line of lines) {
        if (JSON.parse(line).ready) {
          readyOutput = true;
          break;
        }
      }
      if (!readyOutput) {
        await exited;
        throw new Error(diagnostic);
      }
      const socket = createConnection(endpoint);
      sockets.push(socket);
      const pending = new Map<
        string,
        { resolve: (json: string) => void; reject: (error: Error) => void }
      >();
      const ready = new Promise<void>((resolve) => {
        createInterface({ input: socket }).on("line", (line) => {
          const response = JSON.parse(line);
          if (response.event === "ready") {
            resolve();
            return;
          }
          const callback = pending.get(response.requestId);
          if (!callback) return;
          pending.delete(response.requestId);
          if (response.error)
            callback.reject(new Error(response.error.message));
          else callback.resolve(JSON.stringify(response.result));
        });
      });
      socket.on("close", () => {
        for (const callback of pending.values())
          callback.reject(new Error("Host disconnected"));
        pending.clear();
      });
      await ready;
      let next = 0;
      const call = async (op: string, json: string) => {
        const requestId = String(++next);
        const reply = new Promise<string>((resolve, reject) => {
          pending.set(requestId, { resolve, reject });
        });
        socket.write(
          JSON.stringify({ requestId, op, ...JSON.parse(json) }) + "\n",
        );
        return await reply;
      };
      return { child, exited, call, api: createMuonServicesApi(call) };
    };
    const first = await launch();
    expect(await first.call("getUiAutostart", "{}")).toBe("false");
    expect(await readFile(registration, "utf8")).toContain(
      '"--muon-autostart"',
    );
    const handle = await first.api.acquire("worker");
    const states: MuonServiceState[] = [];
    let stopped: (() => void) | undefined;
    const observedStop = new Promise<void>((resolve) => {
      stopped = resolve;
    });
    const watch = await first.api.watchState("worker", (state) => {
      states.push(state);
      if (state.state === "stopped") stopped!();
    });
    await Promise.all([
      first.api.setAutostart("worker", false),
      first.api.setAutostart("second", true),
      first.call("setUiAutostart", '{"enabled":true}'),
    ]);
    expect(await first.call("getUiAutostart", "{}")).toBe("true");
    await first.call("setUiAutostart", '{"enabled":false}');
    await access(registration);
    expect(await first.api.getState("worker")).toMatchObject({
      state: "running",
      autostart: false,
      resident: false,
      clientReferences: 1,
      generation: handle.generation,
    });
    await handle.release();
    await observedStop;
    expect(states.at(-1)).toMatchObject({
      state: "stopped",
      clientReferences: 0,
    });
    await watch.close();

    // Replacing the policy parent with a file deterministically injects a write
    // failure, without requiring permissions or relying on timing a disk write.
    await rename(join(data, ".autostart"), join(data, ".policy-backup"));
    await writeFile(join(data, ".autostart"), "blocked");
    await expect(first.api.setAutostart("worker", true)).rejects.toThrow();
    expect(await first.api.getState("worker")).toMatchObject({
      state: "stopped",
      autostart: false,
      resident: false,
    });
    await rm(join(data, ".autostart"));
    await rename(join(data, ".policy-backup"), join(data, ".autostart"));
    first.child.kill("SIGKILL");
    await first.exited;

    const restored = await launch();
    expect(await restored.call("getUiAutostart", "{}")).toBe("false");
    expect(await restored.api.getState("worker")).toMatchObject({
      state: "stopped",
      autostart: false,
      resident: false,
      generation: "0",
      clientReferences: 0,
    });
    const second = await restored.api.acquire("second");
    expect(await restored.api.getState("second")).toMatchObject({
      state: "running",
      autostart: true,
      resident: true,
    });
    await second.release();
    await restored.api.setAutostart("second", true);
    expect(await restored.api.getState("second")).toMatchObject({
      generation: second.generation,
      clientReferences: 0,
      resident: true,
    });
    await restored.api.setAutostart("second", false);
    await expect(access(registration)).rejects.toThrow();
    await restored.call("setUiAutostart", '{"enabled":true}');
    await access(registration);
    await restored.call("setUiAutostart", '{"enabled":false}');
    await expect(access(registration)).rejects.toThrow();
    await restored.call("shutdown", "{}");
    expect((await restored.exited)[0]).toBe(0);
    await writeFile(join(data, ".autostart", "worker"), "corrupt");
    await expect(
      execute(
        resolve("../service-host/.build/muon-service-host"),
        ["--config", registry, "--endpoint", endpoint, "--data-root", data],
        { timeout: 10_000 },
      ),
    ).rejects.toThrow("Invalid service autostart preference");
  } finally {
    for (const socket of sockets) socket.destroy();
    for (const child of children) {
      if (child.exitCode === null && child.signalCode === null) {
        const exited = once(child, "exit");
        child.kill("SIGKILL");
        await exited;
      }
    }
    await rm(directory, { recursive: true, force: true });
  }
}, 30_000);
