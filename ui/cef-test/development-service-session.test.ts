// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { once } from "node:events";
import {
  cp,
  mkdir,
  mkdtemp,
  readFile,
  readdir,
  rm,
  writeFile,
} from "node:fs/promises";
import { createServer } from "node:http";
import { createConnection, type Socket } from "node:net";
import { dirname, join, resolve } from "node:path";
import { createInterface } from "node:readline";
import { expect, it } from "vitest";
import { createMuonServicesApi } from "../../core/common/renderer/services.ts";
import { resolveMuonServices } from "../common/service-config.ts";
import {
  startMuonDevelopmentServices,
  type MuonDevelopmentServices,
} from "../cef/development-services.ts";

it("replaces prepared services while retaining preferences and rejecting stale handles", async () => {
  await mkdir("../.build", { recursive: true });
  const root = await mkdtemp(resolve("../.build/service-session-update-"));
  let session: MuonDevelopmentServices | undefined;
  let finishStop: (() => void) | undefined;
  let notifyStop!: () => void;
  const stopping = new Promise<void>((resolve) => {
    notifyStop = resolve;
  });
  const observer = createServer((_request, response) => {
    finishStop = () => response.end("continue");
    notifyStop();
  });
  const sockets: Socket[] = [];
  const connect = async () => {
    const socket = createConnection(session!.config.serviceHost.endpoint!);
    sockets.push(socket);
    await once(socket, "connect");
    const pending = new Map<
      string,
      { resolve: (value: unknown) => void; reject: (error: Error) => void }
    >();
    let sequence = 0;
    createInterface({ input: socket }).on("line", (line) => {
      const message = JSON.parse(line);
      const completion = pending.get(message.requestId);
      if (!completion) return;
      pending.delete(message.requestId);
      if (message.error) completion.reject(new Error(message.error.message));
      else completion.resolve(message.result);
    });
    socket.on("close", () => {
      for (const completion of pending.values())
        completion.reject(new Error("Disconnected"));
      pending.clear();
    });
    return createMuonServicesApi(async (op, json) => {
      if (socket.destroyed) throw new Error("Disconnected");
      const requestId = String(++sequence);
      const response = new Promise<unknown>((resolve, reject) =>
        pending.set(requestId, { resolve, reject }),
      );
      socket.write(
        JSON.stringify({ ...JSON.parse(json), op, requestId }) + "\n",
      );
      return JSON.stringify(await response);
    });
  };
  try {
    const project = join(root, "worker");
    const runtime = join(root, "runtime");
    await mkdir(project);
    await mkdir(join(runtime, "runtimes/node/bin"), { recursive: true });
    await mkdir(join(runtime, "plugins"));
    for (const name of ["muon-service-host", "libcardio.so"])
      await cp(
        resolve("../core/service-host/.build", name),
        join(runtime, name),
      );
    await cp(process.execPath, join(runtime, "runtimes/node/bin/node"));
    await cp(
      resolve("../node/dist/node-bridge.mjs"),
      join(runtime, "plugins/node-bridge.mjs"),
    );
    const manifest = JSON.stringify({ name: "session-worker", type: "module" });
    await writeFile(join(project, "package.json"), manifest);
    await cp(
      resolve("../sdk/examples/services/node-files.ts"),
      join(project, "worker.ts"),
    );
    await writeFile(join(project, "node-greeting.txt"), "Before\n");
    const definitions = {
      worker: { runtime: "node", project, entry: "worker.ts", autostart: true },
    };
    session = await startMuonDevelopmentServices(
      root,
      "linux-amd64",
      runtime,
      await resolveMuonServices(definitions, root, ["linux-amd64"]),
      process.env,
      new AbortController().signal,
    );
    const originalConfig = session.config;
    const api = await connect();
    const old = await api.acquire("worker");
    expect(await old.call("greet", ["update"])).toBe("Before update");
    await api.setAutostart("worker", false);
    await writeFile(join(project, "node-greeting.txt"), "After\n");
    const replacement = await resolveMuonServices(definitions, root, [
      "linux-amd64",
    ]);
    await rm(join(project, "package.json"));
    await expect(session.update(runtime, replacement)).rejects.toThrow(
      /package.json/,
    );
    expect(await old.call("greet", ["still running"])).toBe(
      "Before still running",
    );
    expect(session.config).toEqual(originalConfig);
    await writeFile(join(project, "package.json"), manifest);
    await session.update(runtime, replacement);
    expect(session.config.browser).toEqual(originalConfig.browser);
    expect(session.config.serviceHost.endpoint).toBe(
      originalConfig.serviceHost.endpoint,
    );
    expect(
      await readFile(
        join(
          session.config.browser.profilePath,
          "services/worker/last-name.txt",
        ),
        "utf8",
      ),
    ).toBe("still running");
    await expect(old.call("greet", ["stale"])).rejects.toThrow();
    const current = await connect();
    expect(await current.getState("worker")).toMatchObject({
      autostart: false,
      resident: false,
    });
    const handle = await current.acquire("worker");
    expect(await handle.call("greet", ["update"])).toBe("After update");
    await handle.release();
    const sessionPath = dirname(session.config.browser.profilePath);
    expect(
      (await readdir(sessionPath)).filter((name) =>
        name.startsWith("snapshot-"),
      ),
    ).toHaveLength(1);
    observer.listen(0, "127.0.0.1");
    await once(observer, "listening");
    const address = observer.address();
    if (!address || typeof address === "string")
      throw new Error("Missing stop observer address");
    const source = await readFile(join(project, "worker.ts"), "utf8");
    await writeFile(
      join(project, "worker.ts"),
      source.replace(
        "stop: async () => {",
        `stop: async () => { await fetch("http://127.0.0.1:${address.port}"); throw new Error("author stop failure");`,
      ),
    );
    await session.update(
      runtime,
      await resolveMuonServices(definitions, root, ["linux-amd64"]),
    );
    const failing = await connect();
    const failingHandle = await failing.acquire("worker");
    expect(await failingHandle.call("greet", ["stop"])).toBe("After stop");
    let updateFinished = false;
    const updateResult = (async () => {
      try {
        await session!.update(runtime, replacement);
        return undefined;
      } catch (error) {
        return error;
      } finally {
        updateFinished = true;
      }
    })();
    await stopping;
    expect(updateFinished).toBe(false);
    finishStop!();
    const stopError = await updateResult;
    expect(stopError).toBeInstanceOf(Error);
    expect((stopError as Error).message).toContain("author stop failure");
    await session.update(runtime, replacement);
    const retry = await connect();
    const retried = await retry.acquire("worker");
    expect(await retried.call("greet", ["retry"])).toBe("After retry");
    await retried.release();
    await session.stop();
    expect(await readdir(join(root, ".muon/development/linux-amd64"))).toEqual(
      [],
    );
  } finally {
    finishStop?.();
    for (const socket of sockets) socket.destroy();
    await session?.stop();
    observer.closeAllConnections();
    await new Promise<void>((resolve) => observer.close(() => resolve()));
    await rm(root, { recursive: true, force: true });
  }
});
