// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { spawn } from "node:child_process";
import { once } from "node:events";
import {
  copyFile,
  mkdtemp,
  readFile,
  rm,
  symlink,
  writeFile,
} from "node:fs/promises";
import { createConnection } from "node:net";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { createInterface } from "node:readline";
import { expect, it } from "vitest";
import { createPluginProject, buildPlugin } from "muon-plugin-sdk";
import { resolveMuonServices } from "../../ui/common/service-config.ts";
import { stageMuonDesktopServices } from "../../ui/cef/services.ts";
import { createMuonServicesApi } from "../common/renderer/services.ts";

it("calls an SDK-built native plugin from independent QuickJS services and reloads after the last release", async () => {
  const directory = await mkdtemp(join(tmpdir(), "muon-service-ffi-"));
  await symlink(
    resolve("../../node_modules"),
    join(directory, "node_modules"),
    "dir",
  );
  const project = join(directory, "native");
  await createPluginProject({
    directory: project,
    name: "fixture",
    language: "c",
  });
  const configPath = join(project, "muon-plugin.build.json");
  const configuration = JSON.parse(await readFile(configPath, "utf8"));
  await copyFile(
    resolve("../common-test/muon_service_native_plugin_fixture.c"),
    join(project, configuration.sources[0]),
  );
  const allow = [
    "echo",
    "echoInt64",
    "echoUint64",
    "echoBytes",
    "callback",
    "fail",
    "makeEcho",
    "wrapped",
  ].map((name) => `fixture.api.${name}`);
  configuration.functions = allow;
  await writeFile(configPath, JSON.stringify(configuration));
  const manifest = await buildPlugin({
    configPath,
    target: "linux-amd64",
  });
  await copyFile(
    resolve("../common-test/services/quickjs-native-service.ts"),
    join(directory, "service.ts"),
  );
  const declaration = {
    runtime: "quickjs",
    entry: "service.ts",
    plugins: {
      fixture: { native: { manifest }, allow },
    },
  };
  const definitions = await resolveMuonServices(
    { first: declaration, second: declaration },
    directory,
    ["linux-amd64"],
  );
  await stageMuonDesktopServices(definitions, "linux-amd64", directory);
  const endpoint = join(directory, "service.sock");
  const host = spawn(
    resolve("../service-host/.build/muon-service-host"),
    [
      "--config",
      join(directory, "muon-services.json"),
      "--endpoint",
      endpoint,
      "--data-root",
      join(directory, "data"),
    ],
    { stdio: ["ignore", "pipe", "pipe"] },
  );
  const exited = once(host, "exit");
  let diagnostics = "";
  host.stderr.on("data", (bytes: Buffer) => {
    diagnostics += bytes.toString();
  });
  const output = createInterface({ input: host.stdout });
  let socket: ReturnType<typeof createConnection> | undefined;
  try {
    await Promise.race([
      new Promise<void>((resolve) =>
        output.on("line", (line) => {
          if (JSON.parse(line).ready) resolve();
        }),
      ),
      (async () => {
        await exited;
        throw new Error(diagnostics);
      })(),
    ]);
    socket = createConnection(endpoint);
    await once(socket, "connect");
    const pending = new Map<
      string,
      { resolve: (value: unknown) => void; reject: (error: Error) => void }
    >();
    createInterface({ input: socket }).on("line", (line) => {
      const message = JSON.parse(line);
      const waiter = pending.get(message.requestId);
      if (!waiter) return;
      pending.delete(message.requestId);
      if (message.error) waiter.reject(new Error(message.error.message));
      else waiter.resolve(message.result);
    });
    socket.on("close", () => {
      for (const waiter of pending.values())
        waiter.reject(new Error("Host disconnected: " + diagnostics));
      pending.clear();
    });
    let sequence = 0;
    const request = (
      op: string,
      fields: Record<string, unknown>,
    ): Promise<unknown> => {
      const requestId = String(++sequence);
      const result = new Promise<unknown>((resolve, reject) => {
        pending.set(requestId, { resolve, reject });
      });
      socket!.write(JSON.stringify({ ...fields, op, requestId }) + "\n");
      return result;
    };
    const api = createMuonServicesApi(async (name, json) =>
      JSON.stringify(await request(name, JSON.parse(json))),
    );
    const first = await api.acquire("first");
    const second = await api.acquire("second");
    expect(await first.call("echo", ["日本語😀"])).toBe("日本語😀");
    expect(await first.call("echoInt64", [-(1n << 63n)])).toBe(-(1n << 63n));
    expect(await first.call("echoUint64", [(1n << 64n) - 1n])).toBe(
      (1n << 64n) - 1n,
    );
    const bytes = new Uint8Array(1024 * 1024).fill(197);
    expect(await second.call("echoBytes", [bytes])).toEqual(bytes);
    expect(
      await first.call("callback", [
        "outer",
        async (text: unknown) =>
          await second.call("echo", [String(text) + "-reentrant"]),
      ]),
    ).toBe("outer-reentrant");
    expect(await first.call("proxy", ["owned"])).toBe("owned");
    expect(await first.call("failure", [])).toContain("fixture native failure");
    let notified!: (value: unknown) => void;
    const event = new Promise<unknown>((resolve) => {
      notified = resolve;
    });
    const subscription = await second.subscribe("progress", notified);
    await second.call("emit", ["native event"]);
    expect(await event).toBe("native event");
    await subscription.close();
    await first.release();
    expect(await second.call("echo", ["other owner"])).toBe("other owner");
    const generation = second.generation;
    await second.release();
    const reopened = await api.acquire("second");
    expect(reopened.generation).not.toBe(generation);
    expect(await reopened.call("echo", ["new generation"])).toBe(
      "new generation",
    );
    await reopened.release();
    await request("shutdown", {});
    const [code] = await exited;
    expect(code, diagnostics).toBe(0);
  } finally {
    socket?.destroy();
    output.close();
    if (host.exitCode === null && host.signalCode === null) {
      host.kill("SIGKILL");
      await exited;
    }
    await rm(directory, { recursive: true, force: true });
  }
});
