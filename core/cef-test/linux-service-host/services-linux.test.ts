// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { buildPlugin, readPluginArtifact } from "muon-plugin-sdk";
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
import { buildMuonQuickJsService } from "../../../ui/common/service-build.ts";
import { createMuonCapabilityModuleResolver } from "../../../ui/common/capability.js";

const runtime = resolve(
  process.env.MUON_TEST_SERVICE_RUNTIME ?? "../cef/.run/test-linux-amd64-debug",
);
const execute = promisify(execFile);
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
beforeAll(async () => {
  const nativeTarget = "linux-amd64";
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
});

it.each(["simple", "validate"] as const)(
  "keeps native work alive after CEF closes in %s mode",
  async (mode) => {
    const directory = await mkdtemp(join(tmpdir(), "muon-cef-services-"));
    const events = new EventEmitter();
    let moduleSource = "";
    let browserSource = "";
    let environmentsSource = "";
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
          url.pathname === "/services.mjs"
            ? moduleSource
            : url.pathname === "/environments.mjs"
              ? environmentsSource
              : browserSource,
        );
      } else if (url.pathname === "/ready") {
        response.end("ok");
        events.emit("ready", url.searchParams.get("error"));
      } else {
        response.setHeader("Content-Type", "text/html");
        response.end(`<!doctype html><title>Muon native services</title>
        <h1>Native service lifecycle</h1>
        <label><input id="ui-autostart" type="checkbox">Show UI at login</label>
        <label><input id="service-autostart" type="checkbox">Start service automatically</label>
        <pre id="state">Ready</pre>
        <script>globalThis.pageReady=new Promise(resolve=>globalThis.finishPageReady=resolve);</script>
        <script type="module">
        try {
          globalThis.serviceApi = ${mode === "simple" ? "window.muon?.services" : "await import('/services.mjs')"};
          globalThis.browserApi = ${mode === "simple" ? "window.muon?.browser" : "await import('/browser.mjs')"};
          globalThis.settingsApi = ${mode === "simple" ? "window.muon?.environments" : "await import('/environments.mjs')"};
          if (typeof serviceApi?.acquire !== 'function') throw new Error('Missing service acquire API');
          const ui = document.querySelector('#ui-autostart');
          const service = document.querySelector('#service-autostart');
          const state = document.querySelector('#state');
          ui.checked = await settingsApi.getAutostart();
          ui.addEventListener('change', async () => {
            try { await settingsApi.setAutostart(ui.checked); }
            catch(error) { state.textContent = String(error); }
          });
          service.addEventListener('change', async () => {
            try { await serviceApi.setAutostart('resident', service.checked); }
            catch(error) { state.textContent = String(error); }
          });
          globalThis.observer = await serviceApi.watchState('resident', snapshot => {
            service.checked = snapshot.autostart;
            state.textContent = JSON.stringify(snapshot);
          });
          if (location.pathname.endsWith('/popup.html'))
            globalThis.handle = await serviceApi.acquire('resident');
          await fetch('/ready');
        } catch(error) { await fetch('/ready?error=' + encodeURIComponent(String(error))); }
        finally { finishPageReady(); }
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
        "muon.services.setAutostart",
        "muon.services.watchState",
        "muon.services.rpc",
        "muon.environments.getAutostart",
        "muon.environments.setAutostart",
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
      environmentsSource = resolver.load(
        resolver.resolveId("muon:environments", join(directory, "main.ts"))!.id,
      )!;
      const script = join(directory, "quickjs-service.mjs");
      await execute(process.execPath, [
        "../../sdk/scripts/build-service-runtime-fixture.mjs",
        script,
      ]);
      const nativeScript = join(directory, "native-script.mjs");
      await writeFile(nativeScript, nativeBundle);
      const nativePlugin = nativeLibrary;
      const registry = join(directory, "muon-services.json");
      const library = resolve("../service-host/.build/probe-service.so");
      await writeFile(
        registry,
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
      const start = async (login: boolean) => {
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
            ...(login ? ["--muon-autostart"] : []),
          ],
          {
            cwd: directory,
            stdio: ["ignore", "pipe", "pipe"],
            env: { ...process.env, XDG_CONFIG_HOME: join(directory, "xdg") },
          },
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
          arguments: {
            function: `async () => { await globalThis.pageReady; ${source} }`,
          },
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
      // A login with UI disabled must work with no graphical display or CEF.
      const login = await execute(
        join(directory, "muon-core"),
        ["-c", config, "--muon-autostart"],
        {
          cwd: directory,
          env: {
            ...process.env,
            DISPLAY: "",
            XDG_CONFIG_HOME: join(directory, "xdg"),
          },
          timeout: 30_000,
        },
      );
      diagnostic += login.stdout + login.stderr;
      const atLogin = await probe();
      await start(false);
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
        "globalThis.handle = await serviceApi.acquire('resident'); globalThis.borrowed = await serviceApi.acquire('borrowed'); return {generation:handle.generation};",
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
      await start(false);
      expect(
        await evaluate(
          "globalThis.handle = await serviceApi.acquire('resident'); return handle.generation;",
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
      await evaluate("await settingsApi.setAutostart(true); return true;");
      await Promise.allSettled([
        evaluate("await browserApi.shutdown(0); return true;"),
        shutdown,
      ]);
      expect(application!.exitCode, diagnostic).toBe(0);
      await serviceClosed;
      await disconnect();
      await start(true);
      expect(await evaluate("return await settingsApi.getAutostart();")).toBe(
        true,
      );
      const fromLogin = await probe();
      expect(fromLogin.pid).not.toBe(before.pid);
      const loginExit = once(application!, "exit", {
        signal: AbortSignal.timeout(30000),
      });
      await Promise.allSettled([
        evaluate("await browserApi.shutdown(0); return true;"),
        loginExit,
      ]);
      expect(application!.exitCode, diagnostic).toBe(0);
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
