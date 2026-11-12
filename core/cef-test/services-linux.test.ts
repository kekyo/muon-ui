// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile, spawn, type ChildProcess } from "node:child_process";
import { randomUUID } from "node:crypto";
import { EventEmitter, once } from "node:events";
import {
  copyFile,
  mkdir,
  mkdtemp,
  readdir,
  rm,
  symlink,
  writeFile,
} from "node:fs/promises";
import { createServer } from "node:http";
import { createConnection, createServer as createTcpServer } from "node:net";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { createInterface } from "node:readline";
import { promisify } from "node:util";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { InMemoryTransport } from "@modelcontextprotocol/sdk/inMemory.js";
import { createConnection as createMcp } from "@playwright/mcp";
import { beforeAll, expect, it } from "vitest";
import { createMuonCapabilityModuleResolver } from "../../ui/common/capability.js";

const runtime = resolve(
  process.env.MUON_TEST_SERVICE_RUNTIME ?? "../cef/.run/test-linux-amd64-debug",
);
const execute = promisify(execFile);
beforeAll(async () => {
  await mkdir("../service-host/.build", { recursive: true });
  await execute("flock", [
    "../service-host/.build/driver-build.lock",
    "make",
    "-C",
    "../service-host",
    "all",
  ]);
});

it.each(["simple", "validate"] as const)(
  "keeps native work alive after CEF closes in %s mode",
  async (mode) => {
    const directory = await mkdtemp(join(tmpdir(), "muon-cef-services-"));
    const events = new EventEmitter();
    let moduleSource = "";
    let browserSource = "";
    let diagnostic = "";
    let application: ChildProcess | undefined;
    let client: Client | undefined;
    let closeMcp: (() => Promise<void>) | undefined;
    let hostPid: number | undefined;
    const web = createServer((request, response) => {
      const url = new URL(request.url ?? "/", "http://localhost");
      response.setHeader("Cache-Control", "no-store");
      if (url.pathname.endsWith(".mjs")) {
        response.setHeader("Content-Type", "text/javascript");
        response.end(
          url.pathname === "/services.mjs" ? moduleSource : browserSource,
        );
      } else if (url.pathname === "/ready") {
        response.end("ok");
        events.emit("ready", url.searchParams.get("error"));
      } else {
        response.setHeader("Content-Type", "text/html");
        response.end(`<!doctype html><title>Muon native services</title>
        <h1>Native service lifecycle</h1><pre id="state">Ready</pre>
        <script type="module">
        try {
          globalThis.serviceApi = ${mode === "simple" ? "window.muon?.services" : "await import('/services.mjs')"};
          globalThis.browserApi = ${mode === "simple" ? "window.muon?.browser" : "await import('/browser.mjs')"};
          if (typeof serviceApi?.acquire !== 'function') throw new Error('Missing service acquire API');
          await fetch('/ready');
        } catch(error) { await fetch('/ready?error=' + encodeURIComponent(String(error))); }
        </script>`);
      }
    });
    const disconnect = async () => {
      await client?.close();
      await closeMcp?.();
      client = undefined;
      closeMcp = undefined;
    };
    try {
      const ports: number[] = [];
      for (let i = 0; i < 3; i++) {
        const server = createTcpServer();
        server.listen(0, "127.0.0.1");
        await once(server, "listening");
        const address = server.address();
        if (!address || typeof address === "string")
          throw new Error("Missing port");
        ports.push(address.port);
        const closed = once(server, "close");
        server.close();
        await closed;
      }
      web.listen(0, "127.0.0.1");
      await once(web, "listening");
      const address = web.address();
      if (!address || typeof address === "string")
        throw new Error("Missing web port");
      const origin = `http://127.0.0.1:${address.port}`;
      const functions = [
        "muon.services.acquire",
        "muon.services.release",
        "muon.services.getState",
        "muon.browser.close",
        "muon.browser.shutdown",
      ];
      const resolver = createMuonCapabilityModuleResolver(directory, {
        backend: "cef",
        functionPaths: functions,
        imports: [{ sources: ["main.ts"], allow: functions }],
      });
      moduleSource = resolver.load(
        resolver.resolveId("muon:services", join(directory, "main.ts"))!.id,
      )!;
      browserSource = resolver.load(
        resolver.resolveId("muon:browser", join(directory, "main.ts"))!.id,
      )!;
      const registry = join(directory, "muon-services.json");
      const library = resolve("../service-host/.build/probe-service.so");
      await writeFile(
        registry,
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
      const config = join(directory, "muon.json");
      await writeFile(
        config,
        JSON.stringify({
          launcher: { appId: `dev.muon.services.${mode}.${process.pid}` },
          serviceHost: { registry },
          browser: {
            startPage: origin,
            profilePath: join(directory, "profile"),
            titleBarType: "native",
          },
          network: { allow: [`${origin}/**`] },
          plugin: {
            ...(mode === "simple"
              ? { mode }
              : resolver.getRuntimePluginConfig()),
            pages: [`${origin}/**`],
            plugins: [{ name: "internal", allow: functions }],
          },
          cdp: { enable: true, port: ports[2] },
        }),
      );
      for (const name of await readdir(runtime))
        if (name !== "muon-core" && name !== "muon.json")
          await symlink(join(runtime, name), join(directory, name));
      await copyFile(join(runtime, "muon-core"), join(directory, "muon-core"));
      const start = async () => {
        const ready = once(events, "ready", {
          signal: AbortSignal.timeout(60000),
        });
        application = spawn(
          join(directory, "muon-core"),
          [
            "-c",
            config,
            "--ozone-platform=x11",
            "--disable-gpu",
            "--disable-vulkan",
          ],
          { cwd: directory, stdio: ["ignore", "pipe", "pipe"] },
        );
        application.stderr!.on("data", (bytes: Buffer) => {
          diagnostic += bytes.toString();
        });
        application.stdout!.on("data", (bytes: Buffer) => {
          diagnostic += bytes.toString();
        });
        const failed = new Promise<never>((_resolve, reject) => {
          application!.once("error", reject);
          application!.once("exit", (code) =>
            reject(new Error(`CEF exited ${code}: ${diagnostic}`)),
          );
        });
        const [error] = await Promise.race([ready, failed]);
        if (error) throw new Error(String(error) + "\n" + diagnostic);
        const mcp = await createMcp({
          browser: { cdpEndpoint: `http://127.0.0.1:${ports[2]}` },
          outputDir: join(directory, "mcp"),
        });
        closeMcp = async () => await mcp.close();
        const [a, b] = InMemoryTransport.createLinkedPair();
        client = new Client({ name: "muon-services-test", version: "1" });
        await mcp.connect(b);
        await client.connect(a);
      };
      const evaluate = async <T>(source: string): Promise<T> => {
        const result = await client!.callTool({
          name: "browser_evaluate",
          arguments: { function: `async () => { ${source} }` },
        });
        const text = (result.content as { type: string; text?: string }[])
          .filter((x) => x.type === "text")
          .map((x) => x.text ?? "")
          .join("\n");
        if (result.isError) throw new Error(text + "\n" + diagnostic);
        const value = /### Result\n([\s\S]*?)(?:\n### |$)/u.exec(text)?.[1];
        if (!value) throw new Error(text);
        return JSON.parse(value) as T;
      };
      const probe = async () => {
        const socket = createConnection({ host: "127.0.0.1", port: ports[0]! });
        await once(socket, "connect");
        const response = once(createInterface({ input: socket }), "line");
        const nonce = randomUUID();
        socket.write(nonce + "\n");
        const [line] = await response;
        socket.destroy();
        const result = JSON.parse(line) as {
          nonce: string;
          pid: number;
          count: number;
          generation: string;
        };
        expect(result.nonce).toBe(nonce);
        hostPid = result.pid;
        return result;
      };
      await start();
      const first = await evaluate<{ generation: string }>(
        "globalThis.handle = await serviceApi.acquire('resident'); globalThis.borrowed = await serviceApi.acquire('borrowed'); return {generation:handle.generation};",
      );
      const before = await probe();
      const borrowedSocket = createConnection({
        host: "127.0.0.1",
        port: ports[1]!,
      });
      await once(borrowedSocket, "connect");
      const borrowedClosed = once(borrowedSocket, "close");
      borrowedSocket.resume();
      const exited = once(application!, "exit", {
        signal: AbortSignal.timeout(30000),
      });
      await Promise.allSettled([
        evaluate("await browserApi.close(); return true;"),
        exited,
      ]);
      expect(application!.exitCode, diagnostic).toBe(0);
      await disconnect();
      await borrowedClosed;
      const withoutUi = await probe();
      expect(withoutUi.pid).toBe(before.pid);
      expect(withoutUi.count).toBeGreaterThan(before.count);
      await start();
      expect(
        await evaluate(
          "globalThis.handle = await serviceApi.acquire('resident'); return handle.generation;",
        ),
      ).toBe(first.generation);
      const serviceSocket = createConnection({
        host: "127.0.0.1",
        port: ports[0]!,
      });
      await once(serviceSocket, "connect");
      serviceSocket.resume();
      const serviceClosed = once(serviceSocket, "close");
      const shutdown = once(application!, "exit", {
        signal: AbortSignal.timeout(30000),
      });
      await Promise.allSettled([
        evaluate("await browserApi.shutdown(0); return true;"),
        shutdown,
      ]);
      expect(application!.exitCode, diagnostic).toBe(0);
      await serviceClosed;
    } finally {
      await disconnect();
      if (
        application &&
        application.exitCode === null &&
        application.signalCode === null
      ) {
        const stopped = once(application, "exit");
        application.kill("SIGKILL");
        await stopped;
      }
      if (hostPid) {
        try {
          process.kill(hostPid, "SIGTERM");
        } catch {}
      }
      web.closeAllConnections();
      await new Promise<void>((done) => web.close(() => done()));
      await mkdir(resolve(".run/services-linux", mode), { recursive: true });
      await writeFile(
        resolve(".run/services-linux", mode, "runtime.log"),
        diagnostic,
      );
      await rm(directory, { recursive: true, force: true });
    }
  },
);
