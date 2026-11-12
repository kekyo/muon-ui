// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile } from "node:child_process";
import { randomUUID } from "node:crypto";
import { mkdir, readFile, writeFile } from "node:fs/promises";
import { join, resolve, win32 } from "node:path";
import { promisify } from "node:util";
import type { RemoteAgent, RemoteManagedProcess } from "agent-rover";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { InMemoryTransport } from "@modelcontextprotocol/sdk/inMemory.js";
import { createConnection } from "@playwright/mcp";
import { afterAll, beforeAll, expect, it, vi } from "vitest";
import {
  createMuonCapabilityModuleResolver,
  expandMuonServicePermissions,
} from "../../ui/common/capability.ts";
import { resolveWindowsE2eEnvironment } from "../../scripts/windows-e2e-environment.ts";
import {
  connectWindowsAgent,
  launchWindowsTestProcess,
  type WindowsTestProcess,
} from "./plugin-e2e/windows-agent.ts";
import { resolveWindowsRuntimeTarget } from "./plugin-e2e/windows-matrix.ts";
import { discoverWindowsTestNetwork } from "./plugin-e2e/windows-network.ts";
import { stageWindowsRuntimeDirectory } from "./plugin-e2e/windows-staging.ts";

const target = resolveWindowsRuntimeTarget(process.env.MUON_E2E_WINDOWS_TARGET);
const environment = resolveWindowsE2eEnvironment(process.env);
const remoteBase = win32.join(environment.workDir, "services", target.target);
const runtime = win32.join(remoteBase, "runtime");
const root = win32.join(remoteBase, "run-" + randomUUID());
const relayPath = win32.join(remoteBase, "muon-cdp-relay.exe");
const artifacts = resolve(".run/services-windows", target.target);
let agent: RemoteAgent | undefined;
let cdpHost = "";

beforeAll(async () => {
  await promisify(execFile)("make", ["-C", "../service-host", target.target], {
    maxBuffer: 4 * 1024 * 1024,
  });
  const connection = await connectWindowsAgent(environment, target);
  if (connection.status !== "connected") throw new Error(connection.reason);
  agent = connection.agent;
  cdpHost = (await discoverWindowsTestNetwork(agent, environment.nodePath))
    .cdpHost;
  await stageWindowsRuntimeDirectory(
    agent,
    resolve(
      "../..",
      process.env.MUON_TEST_WINDOWS_RUNTIME ?? target.releaseRuntimeDirectory,
    ),
    runtime,
  );
  await agent.files.writeFile(
    relayPath,
    await readFile(
      resolve(".run/windows-cdp-relay", target.target, "muon-cdp-relay.exe"),
    ),
  );
  await agent.files.writeFile(
    win32.join(runtime, "probe-service.dll"),
    await readFile(
      resolve("../service-host/.build", target.target, "probe-service.dll"),
    ),
  );
  await agent.files.mkdir(root, { recursive: true });
  await mkdir(artifacts, { recursive: true });
}, 600000);

afterAll(async () => {
  if (!agent) return;
  try {
    // A failed page setup can occur before the business probe reveals the host
    // PID. Confine fallback cleanup to this test's staged runtime directory.
    for (const child of await agent.processes.list()) {
      if (
        child.running &&
        child.path.toLowerCase().startsWith(runtime.toLowerCase() + "\\")
      ) {
        await agent.processes.kill(child.id);
        await agent.processes.waitForExit(child.id, { timeoutMs: 30000 });
      }
    }
    if (await agent.files.exists(root))
      await agent.files.remove(root, { recursive: true });
  } finally {
    agent.release();
  }
});

it.each(["simple", "validate"] as const)(
  "keeps native work alive after Windows CEF closes in %s mode",
  async (mode) => {
    if (!agent) throw new Error("Windows agent is unavailable");
    const remote = agent;
    const directory = win32.join(root, mode);
    const assets = win32.join(directory, "assets", "main");
    await remote.files.mkdir(assets, { recursive: true });
    const runNode = async (source: string): Promise<string> => {
      const child = await remote.processes.launchManaged({
        path: environment.nodePath,
        arguments: ["--input-type=module", "-e", source],
        captureStdout: true,
        captureStderr: true,
        createNoWindow: true,
      });
      try {
        const finished = await child.waitForExit({ timeoutMs: 30000 });
        expect(finished.root.exitCode, await child.stderrText()).toBe(0);
        return await child.stdoutText();
      } finally {
        await child.releaseAsync();
      }
    };
    const ports = JSON.parse(
      await runNode(`
      import {createServer} from 'node:net';
      const listeners = [createServer(),createServer(),createServer()];
      for (const server of listeners) await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
      console.log(JSON.stringify(listeners.map(server=>server.address().port)));
      for (const server of listeners) await new Promise(resolve=>server.close(resolve));
    `),
    ) as [number, number, number];
    const functions = [
      "muon.services.acquire",
      "muon.services.getState",
      "muon.browser.close",
      "muon.browser.shutdown",
    ];
    const resolver = createMuonCapabilityModuleResolver("/windows-services", {
      backend: "cef",
      functionPaths: functions,
      imports: [{ sources: ["main.ts"], allow: functions }],
    });
    for (const name of ["services", "browser"]) {
      const id = resolver.resolveId(
        "muon:" + name,
        "/windows-services/main.ts",
      )!.id;
      await remote.files.writeFile(
        win32.join(assets, name + ".mjs"),
        Buffer.from(resolver.load(id)!),
      );
    }
    const title = `Muon Windows services ${target.target} ${mode}`;
    await remote.files.writeFile(
      win32.join(assets, "index.html"),
      Buffer.from(`<!doctype html><meta charset="utf-8">
      <title>${title}</title><h1>${title}</h1><pre id="state">Ready</pre>
      <script>globalThis.pageReady=new Promise(resolve=>globalThis.finishPageReady=resolve);</script>
      <script type="module">
      try {
        globalThis.serviceApi=${mode === "simple" ? "window.muon.services" : "await import('./services.mjs')"};
        globalThis.browserApi=${mode === "simple" ? "window.muon.browser" : "await import('./browser.mjs')"};
      } catch(error) {globalThis.setupError=String(error);}
      finishPageReady();</script>`),
    );
    const registry = win32.join(directory, "muon-services.json");
    const library = win32.join(runtime, "probe-service.dll");
    await remote.files.writeFile(
      registry,
      Buffer.from(
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
      ),
    );
    const config = win32.join(directory, "muon.json");
    await remote.files.writeFile(
      config,
      Buffer.from(
        JSON.stringify({
          launcher: { appId: "dev.muon.services." + randomUUID() },
          serviceHost: { registry },
          asset: { sourcePath: win32.join(directory, "assets") },
          browser: {
            startPage: "asset://main/index.html",
            profilePath: win32.join(directory, "profile"),
            titleBarType: "native",
          },
          network: { allow: ["asset://main/**"] },
          plugin: {
            ...(mode === "simple"
              ? { mode }
              : resolver.getRuntimePluginConfig()),
            pages: ["asset://main/**"],
            plugins: [
              {
                name: "internal",
                allow: expandMuonServicePermissions(functions),
              },
            ],
          },
          cdp: { enable: true, port: ports[2] },
        }),
      ),
    );
    let application: WindowsTestProcess | undefined;
    let relay: RemoteManagedProcess | undefined;
    let client: Client | undefined;
    let closeMcp: (() => Promise<void>) | undefined;
    let hostPid: number | undefined;
    const observers: RemoteManagedProcess[] = [];
    const diagnostics: unknown[] = [];
    const disconnect = async () => {
      await client?.close();
      await closeMcp?.();
      client = undefined;
      closeMcp = undefined;
    };
    const evaluate = async <T>(source: string): Promise<T> => {
      const result = await client!.callTool({
        name: "browser_evaluate",
        arguments: {
          function: `async () => {await globalThis.pageReady; ${source}}`,
        },
      });
      const text = (result.content as { type: string; text?: string }[])
        .filter((item) => item.type === "text")
        .map((item) => item.text ?? "")
        .join("\n");
      if (result.isError) throw new Error(text);
      const value = /### Result\n([\s\S]*?)(?:\n### |$)/u.exec(text)?.[1];
      if (value === undefined) throw new Error(text);
      return JSON.parse(value) as T;
    };
    const capture = async () => {
      if (application)
        diagnostics.push({
          pid: application.id,
          stdout: await application.stdoutText(),
          stderr: await application.stderrText(),
        });
    };
    const probe = async () => {
      const nonce = randomUUID();
      const result = JSON.parse(
        await runNode(`
        import {createConnection} from 'node:net';import {once} from 'node:events';import {createInterface} from 'node:readline';
        const socket=createConnection({host:'127.0.0.1',port:${ports[0]}});await once(socket,'connect');
        const response=once(createInterface({input:socket}),'line');socket.write(${JSON.stringify(nonce + "\n")});
        const [line]=await response;socket.destroy();console.log(line);
      `),
      ) as { nonce: string; pid: number; count: number; generation: string };
      expect(result.nonce).toBe(nonce);
      hostPid = result.pid;
      diagnostics.push(result);
      return result;
    };
    const observeClose = async (port: number) => {
      // This independent business connection owns no service reference. Its
      // connected message is the barrier before closing the UI or whole app.
      const observer = await remote.processes.launchManaged({
        path: environment.nodePath,
        arguments: [
          "--input-type=module",
          "-e",
          `
          import {createConnection} from 'node:net';import {once} from 'node:events';
          const socket=createConnection({host:'127.0.0.1',port:${port}});await once(socket,'connect');
          const closed=once(socket,'close');socket.resume();console.log('connected');await closed;console.log('closed');
        `,
        ],
        captureStdout: true,
        captureStderr: true,
        createNoWindow: true,
      });
      observers.push(observer);
      await vi.waitUntil(
        async () => {
          const output = await observer.stdoutText();
          if (!(await observer.rootSnapshot()).running)
            throw new Error(output + (await observer.stderrText()));
          return output.includes("connected");
        },
        { timeout: 30000, interval: 100 },
      );
      return observer;
    };
    try {
      relay = await remote.processes.launchManaged({
        path: relayPath,
        arguments: ["0", String(ports[2])],
        captureStdout: true,
        captureStderr: true,
        createNoWindow: true,
      });
      const relayPort = await vi.waitUntil(
        async () => {
          const match = /^MUON_CDP_RELAY_PORT=(\d+)$/m.exec(
            await relay!.stdoutText(),
          );
          return match ? Number(match[1]) : false;
        },
        { timeout: 30000, interval: 100 },
      );
      const cdpEndpoint = `http://${cdpHost}:${relayPort}`;
      const start = async (iteration: number) => {
        const captureDirectory = win32.join(directory, "capture-" + iteration);
        await remote.files.mkdir(captureDirectory, { recursive: true });
        application = await launchWindowsTestProcess(
          remote,
          {
            path: win32.join(runtime, "muon-core.exe"),
            arguments: ["-c", config],
            workingDirectory: directory,
          },
          captureDirectory,
          false,
        );
        // The redirected files remain writable by the independent service
        // process. Node opens shared reads; the agent's generic file reader
        // denies concurrent write handles on Windows.
        const readLog = async (name: string) =>
          await runNode(
            `import {readFile} from 'node:fs/promises';process.stdout.write(await readFile(${JSON.stringify(win32.join(captureDirectory, name))},'utf8'));`,
          );
        application = {
          ...application,
          stdoutText: async () => await readLog("muon-stdout.log"),
          stderrText: async () => await readLog("muon-stderr.log"),
        };
        await vi.waitUntil(
          async () => {
            if (!(await application!.rootSnapshot()).running)
              throw new Error(await application!.stderrText());
            try {
              const response = await fetch(cdpEndpoint + "/json/list", {
                signal: AbortSignal.timeout(1000),
              });
              return ((await response.json()) as { title: string }[]).some(
                (page) => page.title === title,
              );
            } catch {
              return false;
            }
          },
          { timeout: 60000, interval: 100 },
        );
        const mcp = await createConnection({
          browser: { cdpEndpoint },
          outputDir: join(artifacts, mode, String(iteration)),
        });
        closeMcp = async () => await mcp.close();
        const [a, b] = InMemoryTransport.createLinkedPair();
        client = new Client({ name: "muon-windows-services", version: "1" });
        await mcp.connect(b);
        await client.connect(a);
        expect(
          await evaluate(
            "return {api:typeof serviceApi?.acquire,error:globalThis.setupError??null};",
          ),
        ).toEqual({ api: "function", error: null });
      };
      await start(1);
      const first = await evaluate<{ generation: string }>(
        "globalThis.handle=await serviceApi.acquire('resident');globalThis.borrowed=await serviceApi.acquire('borrowed');return {generation:handle.generation};",
      );
      // Acquisition permission must allow its handle release without exposing
      // an independent release capability in either page API mode.
      await evaluate(
        "const temporary=await serviceApi.acquire('resident');await temporary.release();return true;",
      );
      const before = await probe();
      const borrowed = await observeClose(ports[1]);
      const closed = await Promise.allSettled([
        evaluate("await browserApi.close();return true;"),
        remote.processes.waitForExit(application!.id, { timeoutMs: 30000 }),
      ]);
      expect(closed[1].status).toBe("fulfilled");
      if (closed[1].status === "fulfilled")
        expect(closed[1].value.exitCode).toBe(0);
      await capture();
      await disconnect();
      expect(
        (await borrowed.waitForExit({ timeoutMs: 30000 })).root.exitCode,
        await borrowed.stderrText(),
      ).toBe(0);
      const withoutUi = await probe();
      expect(withoutUi.pid).toBe(before.pid);
      expect(withoutUi.generation).toBe(before.generation);
      expect(withoutUi.count).toBeGreaterThan(before.count);
      await start(2);
      expect(
        await evaluate(
          "globalThis.handle=await serviceApi.acquire('resident');return handle.generation;",
        ),
      ).toBe(first.generation);
      const resident = await observeClose(ports[0]);
      const shutdown = await Promise.allSettled([
        evaluate("await browserApi.shutdown(0);return true;"),
        remote.processes.waitForExit(application!.id, { timeoutMs: 30000 }),
      ]);
      expect(shutdown[1].status).toBe("fulfilled");
      if (shutdown[1].status === "fulfilled")
        expect(shutdown[1].value.exitCode).toBe(0);
      expect(
        (await resident.waitForExit({ timeoutMs: 30000 })).root.exitCode,
        await resident.stderrText(),
      ).toBe(0);
    } finally {
      try {
        await capture();
        await writeFile(
          join(artifacts, mode + ".json"),
          JSON.stringify(diagnostics, null, 2),
        );
      } finally {
        try {
          await disconnect();
        } finally {
          if (application && (await application.rootSnapshot()).running)
            await application.kill();
          if (hostPid && (await remote.processes.exists(hostPid)))
            await remote.processes.kill(hostPid);
          for (const observer of observers) await observer.releaseAsync();
          await relay?.releaseAsync();
        }
      }
    }
  },
  180000,
);
