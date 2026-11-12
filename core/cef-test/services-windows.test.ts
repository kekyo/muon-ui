// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { buildPlugin, readPluginArtifact } from "muon-plugin-sdk";
import { execFile } from "node:child_process";
import { createHash, randomUUID } from "node:crypto";
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
import { buildMuonQuickJsService } from "../../ui/common/service-build.ts";
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
const nativeAllow = [
  "echo",
  "echoInt64",
  "echoUint64",
  "echoBytes",
  "callback",
  "fail",
  "makeEcho",
  "wrapped",
].map((name) => "fixture.api." + name);
let nativeLibrary = "";
let nativeSignature = "";
let nativeBundle = "";
let cdpHost = "";

beforeAll(async () => {
  const nativeTarget = target.target;
  const manifest = await buildPlugin({
    configPath: resolve("../common-test/muon-service-plugin.build.json"),
    outputDirectory: resolve(".run/service-native-fixture", nativeTarget),
    target: nativeTarget,
  });
  const artifact = await readPluginArtifact(manifest, [nativeTarget]);
  nativeLibrary = artifact.targets[nativeTarget]!.library;
  nativeSignature = artifact.manifest.targets[nativeTarget]!.library.sha256;
  nativeBundle = await buildMuonQuickJsService(
    resolve("../common-test/services/quickjs-native-service.ts"),
    ["fixture:api"],
  );
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
    const appId = "dev.muon.services." + randomUUID();
    const registrationId =
      appId +
      "-" +
      createHash("sha256")
        .update(appId + "\n" + win32.join(directory, "profile"))
        .digest("hex")
        .slice(0, 16);
    const runKey = String.raw`HKCU\Software\Microsoft\Windows\CurrentVersion\Run`;
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
      } catch (error) {
        throw new Error(
          `${String(error)}\nstdout: ${await child.stdoutText()}\nstderr: ${await child.stderrText()}`,
          { cause: error },
        );
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
      "muon.services.setAutostart",
      "muon.services.watchState",
      "muon.services.rpc",
      "muon.environments.getAutostart",
      "muon.environments.setAutostart",
      "muon.browser.close",
      "muon.browser.shutdown",
    ];
    const resolver = createMuonCapabilityModuleResolver("/windows-services", {
      backend: "cef",
      functionPaths: functions,
      imports: [{ sources: ["main.ts"], allow: functions }],
    });
    for (const name of ["services", "browser", "environments"]) {
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
    for (const page of ["index.html", "popup.html"])
      await remote.files.writeFile(
        win32.join(assets, page),
        Buffer.from(`<!doctype html><meta charset="utf-8">
      <title>${title}</title><h1>${title}</h1><pre id="state">Ready</pre>
      <script>globalThis.pageReady=new Promise(resolve=>globalThis.finishPageReady=resolve);</script>
      <script type="module">
      try {
        globalThis.serviceApi=${mode === "simple" ? "window.muon.services" : "await import('./services.mjs')"};
        globalThis.browserApi=${mode === "simple" ? "window.muon.browser" : "await import('./browser.mjs')"};
        globalThis.settingsApi=${mode === "simple" ? "window.muon.environments" : "await import('./environments.mjs')"};
        globalThis.observer=await serviceApi.watchState('resident', state=>document.querySelector('#state').textContent=JSON.stringify(state));
        if(location.pathname.endsWith('/popup.html'))globalThis.handle=await serviceApi.acquire('resident');
      } catch(error) {globalThis.setupError=String(error);}
      finishPageReady();</script>`),
      );
    const script = win32.join(directory, "quickjs-service.mjs");
    await remote.files.writeFile(
      script,
      Buffer.from(
        await buildMuonQuickJsService(
          resolve("../../sdk/test/fixtures/quickjs-service.ts"),
          [],
        ),
      ),
    );
    const nativeScript = win32.join(directory, "native-script.mjs");
    const nativePlugin = win32.join(directory, "fixture.dll");
    await remote.files.writeFile(nativeScript, Buffer.from(nativeBundle));
    await remote.files.writeFile(nativePlugin, await readFile(nativeLibrary));
    const registry = win32.join(directory, "muon-services.json");
    const library = win32.join(runtime, "probe-service.dll");
    await remote.files.writeFile(
      registry,
      Buffer.from(
        JSON.stringify({
          plugins: [
            {
              name: "fixture",
              library: nativePlugin,
              signature: nativeSignature,
              salt: "",
              config: {},
              allow: nativeAllow,
              expectedFunctions: nativeAllow,
            },
          ],
          services: {
            nativeScript: {
              runtime: "quickjs",
              script: nativeScript,
              autostart: true,
              plugins: nativeAllow,
            },
            script: { runtime: "quickjs", script, autostart: true },
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
          launcher: { appId },
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
    const applications: WindowsTestProcess[] = [];
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
      // The independent host inherits the first launch's log files, so retain
      // those diagnostics after later UI sessions replace the current process.
      for (const application of applications)
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
      const launch = async (iteration: number, login: boolean) => {
        const captureDirectory = win32.join(directory, "capture-" + iteration);
        await remote.files.mkdir(captureDirectory, { recursive: true });
        application = await launchWindowsTestProcess(
          remote,
          {
            path: win32.join(runtime, "muon-core.exe"),
            arguments: ["-c", config, ...(login ? ["--muon-autostart"] : [])],
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
        applications.push(application);
      };
      const start = async (iteration: number, login: boolean) => {
        await launch(iteration, login);
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
      await launch(0, true);
      expect(
        (
          await remote.processes.waitForExit(application!.id, {
            timeoutMs: 30000,
          })
        ).exitCode,
        await application!.stderrText(),
      ).toBe(0);
      await capture();
      const atLogin = await probe();
      await start(1, false);
      expect(await evaluate("return await settingsApi.getAutostart();")).toBe(
        false,
      );
      expect(
        await evaluate(
          "return (await serviceApi.getState('resident')).clientReferences;",
        ),
      ).toBe(0);
      expect(
        await evaluate(
          "await settingsApi.setAutostart(true); return await settingsApi.getAutostart();",
        ),
      ).toBe(true);
      expect(
        await evaluate(
          "await settingsApi.setAutostart(false); return await settingsApi.getAutostart();",
        ),
      ).toBe(false);
      const first = await evaluate<{ generation: string }>(
        "globalThis.handle=await serviceApi.acquire('resident');globalThis.borrowed=await serviceApi.acquire('borrowed');return {generation:handle.generation};",
      );

      expect(
        await evaluate(`
        const text = '日本\\0🙂';
        const bytes = new Uint8Array(1024 * 1024).fill(197);
        const echoed = await handle.call('echoBytes', [bytes]);
        const abort = new AbortController();
        let entered;
        const started = new Promise(resolve => entered = resolve);
        const cancelled = (async () => {
          try {
            await handle.call('callback', ['pending', () => {
              entered(); return new Promise(() => {});
            }], {signal: abort.signal});
            return 'unexpected-success';
          } catch(error) { return error.code; }
        })();
        await started;
        abort.abort();
        return {
          text: await handle.call('echo', [text]),
          integer: String(await handle.call('echoUint64', [(1n << 64n) - 1n])),
          binary: echoed.length === bytes.length && echoed.every(value => value === 197),
          escaped: (await handle.call('echo', ['\\0'.repeat(1024 * 1024)])).length,
          nested: await handle.call('callback', ['outer', async value =>
            await handle.call('callback', [value, async inner => inner + '-nested'])]),
          cancelled: await cancelled,
        };
      `),
      ).toEqual({
        text: "日本\0🙂",
        integer: "18446744073709551615",
        binary: true,
        escaped: 1024 * 1024,
        nested: "outer-nested",
        cancelled: "cancelled",
      });
      const scriptGeneration = await evaluate<string>(
        "globalThis.scriptHandle = await serviceApi.acquire('script'); return scriptHandle.generation;",
      );
      expect(
        await evaluate(`
        const bytes = new Uint8Array(1024 * 1024).fill(197);
        let echoed;
        try { echoed = await scriptHandle.call('echoBytes', [bytes]); }
        catch (error) { throw new Error('QuickJS echoBytes failed: ' + String(error) + ' ' + JSON.stringify(await serviceApi.getState('script'))); }
        let received;
        const event = new Promise(resolve => received = resolve);
        const subscription = await scriptHandle.subscribe('progress', received);
        await scriptHandle.call('emit', [47]);
        const value = await event;
        await subscription.close();
        return {
          echo: await scriptHandle.call('echo', ['日本\\0🙂']),
          bytes: echoed.length === bytes.length && echoed.every(value => value === 197),
          callback: await scriptHandle.call('callback', ['outer', async value =>
            await handle.call('callback', [value, async inner => inner + '-native'])]),
          event: value,
        };
      `),
      ).toEqual({
        echo: "日本\0🙂",
        bytes: true,
        callback: "outer-native",
        event: 47,
      });
      const nativeGeneration = await evaluate<string>(
        "globalThis.nativeHandle = await serviceApi.acquire('nativeScript'); return nativeHandle.generation;",
      );
      expect(
        await evaluate(`
        const bytes = new Uint8Array(1024 * 1024).fill(197);
        const echoed = await nativeHandle.call('echoBytes', [bytes]);
        let received;
        const event = new Promise(resolve => received = resolve);
        const subscription = await nativeHandle.subscribe('progress', received);
        await nativeHandle.call('emit', ['native event']);
        const notified = await event;
        await subscription.close();
        return {
          text: await nativeHandle.call('echo', ['日本語😀']),
          signed: String(await nativeHandle.call('echoInt64', [-(1n << 63n)])),
          unsigned: String(await nativeHandle.call('echoUint64', [(1n << 64n) - 1n])),
          bytes: echoed.length === bytes.length && echoed.every(value => value === 197),
          callback: await nativeHandle.call('callback', ['outer', async value => await scriptHandle.call('echo', [value + '-reentrant'])]),
          proxy: await nativeHandle.call('proxy', ['owned']),
          failure: (await nativeHandle.call('failure', [])).includes('fixture native failure'),
          event: notified,
        };
      `),
      ).toEqual({
        text: "日本語😀",
        signed: "-9223372036854775808",
        unsigned: "18446744073709551615",
        bytes: true,
        callback: "outer-reentrant",
        proxy: "owned",
        failure: true,
        event: "native event",
      });
      await evaluate(`
        globalThis.rpcEvents = [];
        globalThis.rpcEvent = new Promise(resolve => globalThis.rpcNext = resolve);
        globalThis.rpcSubscription = await handle.subscribe('progress', value => {
          rpcEvents.push(value); rpcNext();
        });
        let entered;
        const started = new Promise(resolve => entered = resolve);
        globalThis.mainPending = (async () => {
          try { return await handle.call('callback', ['main', async () => {
            entered(); return await new Promise(resolve => globalThis.rpcContinue = resolve);
          }]); } catch(error) { return error.code; }
        })();
        await started;
        let appeared;
        const ready = new Promise(resolve => appeared = resolve);
        const watcher = await serviceApi.watchState('resident', state => {
          if (state.clientReferences === 2) appeared();
        });
        window.open(new URL('popup.html', location.href).href);
        await ready;
        await watcher.close();
        return true;
      `);
      const tabs = async (
        action: string,
        index: number | undefined,
      ): Promise<string> => {
        const result = await client!.callTool({
          name: "browser_tabs",
          arguments: {
            action,
            ...(index === undefined ? {} : { index }),
          },
        });
        const text = (result.content as { type: string; text?: string }[])
          .filter((item) => item.type === "text")
          .map((item) => item.text ?? "")
          .join("\n");
        if (result.isError) throw new Error(text);
        return text;
      };
      const listing = await tabs("list", undefined);
      const peer = Number(listing.match(/- (\d+):[^\n]*popup\.html/)?.[1]);
      expect(Number.isFinite(peer), listing).toBe(true);
      await tabs("select", peer);
      expect(
        await evaluate(`
        await globalThis.pageReady;
        const result = await handle.call('callback', ['peer', async value => value + '-callback']);
        await handle.call('emit', [31]);
        let entered;
        const started = new Promise(resolve => entered = resolve);
        globalThis.peerPending = (async () => {
          try { return await handle.call('callback', ['pending', () => {
            entered(); return new Promise(() => {});
          }]); } catch(error) { return error.code; }
        })();
        await started;
        return result;
      `),
      ).toBe("peer-callback");
      await tabs("select", 0);
      await evaluate(`
        await rpcEvent;
        let gone;
        globalThis.rpcGone = new Promise(resolve => gone = resolve);
        globalThis.rpcWatch = await serviceApi.watchState('resident', state => {
          if (state.clientReferences === 1) gone();
        });
        return true;
      `);
      await tabs("close", peer);
      await tabs("select", 0);
      expect(
        await evaluate(`
        await rpcGone;
        await rpcWatch.close();
        rpcContinue('main-continued');
        const result = await mainPending;
        globalThis.rpcEvent = new Promise(resolve => globalThis.rpcNext = resolve);
        await handle.call('emit', [32]);
        await rpcEvent;
        await rpcSubscription.close();
        return {result, events: rpcEvents, alive: await handle.call('echo', ['still-running'])};
      `),
      ).toEqual({
        result: "main-continued",
        events: [31, 32],
        alive: "still-running",
      });

      const before = await probe();
      expect(before.pid).toBe(atLogin.pid);
      expect(before.generation).toBe(atLogin.generation);
      expect(
        await evaluate(
          "await serviceApi.setAutostart('resident', false); return await serviceApi.getState('resident');",
        ),
      ).toMatchObject({
        autostart: false,
        resident: false,
        clientReferences: 1,
        state: "running",
      });
      expect(
        await evaluate(
          "await serviceApi.setAutostart('resident', true); await serviceApi.setAutostart('resident', true); return await serviceApi.getState('resident');",
        ),
      ).toMatchObject({
        autostart: true,
        resident: true,
        clientReferences: 1,
        generation: before.generation,
      });
      await runNode(`import {execFile} from 'node:child_process';import {promisify} from 'node:util';
        await promisify(execFile)('reg.exe',${JSON.stringify(["query", runKey, "/v", registrationId])});`);
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
      await start(2, false);
      expect(
        await evaluate(
          "globalThis.handle=await serviceApi.acquire('resident');return handle.generation;",
        ),
      ).toBe(first.generation);
      expect(
        await evaluate(
          "globalThis.scriptHandle = await serviceApi.acquire('script'); return scriptHandle.generation;",
        ),
      ).toBe(scriptGeneration);
      expect(
        await evaluate(
          "return await scriptHandle.call('echo', ['after-ui-close']);",
        ),
      ).toBe("after-ui-close");
      expect(
        await evaluate(
          "globalThis.nativeHandle = await serviceApi.acquire('nativeScript'); return nativeHandle.generation;",
        ),
      ).toBe(nativeGeneration);
      expect(
        await evaluate(
          "return await nativeHandle.call('echo', ['native-after-ui-close']);",
        ),
      ).toBe("native-after-ui-close");
      const resident = await observeClose(ports[0]);
      await evaluate("await settingsApi.setAutostart(true); return true;");
      const shutdown = await Promise.allSettled([
        evaluate("await browserApi.shutdown(0);return true;"),
        remote.processes.waitForExit(application!.id, { timeoutMs: 30000 }),
      ]);
      diagnostics.push({
        phase: "shutdown",
        results: shutdown.map((result) =>
          result.status === "rejected"
            ? { status: result.status, reason: String(result.reason) }
            : result,
        ),
        processes: (await remote.processes.list()).filter((process) =>
          process.path.toLowerCase().startsWith(runtime.toLowerCase() + "\\"),
        ),
      });
      expect(shutdown[1].status, JSON.stringify(diagnostics.at(-1))).toBe(
        "fulfilled",
      );
      if (shutdown[1].status === "fulfilled")
        expect(shutdown[1].value.exitCode).toBe(0);
      expect(
        (await resident.waitForExit({ timeoutMs: 30000 })).root.exitCode,
        await resident.stderrText(),
      ).toBe(0);
      await capture();
      await disconnect();
      await start(3, true);
      expect(await evaluate("return await settingsApi.getAutostart();")).toBe(
        true,
      );
      const fromLogin = await probe();
      expect(fromLogin.pid).not.toBe(before.pid);
      await evaluate(
        "await settingsApi.setAutostart(false); await serviceApi.setAutostart('resident', false); return true;",
      );
      const loginExit = await Promise.allSettled([
        evaluate("await browserApi.shutdown(0); return true;"),
        remote.processes.waitForExit(application!.id, { timeoutMs: 30000 }),
      ]);
      expect(loginExit[1].status).toBe("fulfilled");
      if (loginExit[1].status === "fulfilled")
        expect(loginExit[1].value.exitCode).toBe(0);
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
          await runNode(`import {execFile} from 'node:child_process';import {promisify} from 'node:util';
            try { await promisify(execFile)('reg.exe',${JSON.stringify(["delete", runKey, "/v", registrationId, "/f"])}); }
            catch {}`);
        }
      }
    }
  },
  180000,
);
