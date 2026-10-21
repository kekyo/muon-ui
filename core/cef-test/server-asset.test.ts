// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { spawn } from "node:child_process";
import { EventEmitter, once } from "node:events";
import { watch } from "node:fs";
import { mkdir, mkdtemp, rm, writeFile } from "node:fs/promises";
import { request as httpRequest } from "node:http";
import { createConnection as connectTcp, createServer } from "node:net";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { InMemoryTransport } from "@modelcontextprotocol/sdk/inMemory.js";
import { createConnection } from "@playwright/mcp";
import { expect, it } from "vitest";
import { createMuonCapabilityModuleResolver } from "../../ui/common/capability.ts";

it.each(["simple", "validate"] as const)(
  "allows authenticated HTTP and WebSocket from the product asset origin in %s mode",
  async (mode) => {
    const runtime = resolve(
      process.env.MUON_TEST_LINUX_RUNTIME ??
        "../cef/.run/test-linux-amd64-debug",
    );
    const directory = await mkdtemp(join(tmpdir(), "muon-server-asset-"));
    const assets = join(directory, "assets", "main");
    await mkdir(assets, { recursive: true });
    const functionPaths = ["muon.server.createServer", "muon.fs.writeTextFile"];
    const resolver = createMuonCapabilityModuleResolver(directory, {
      backend: "cef",
      functionPaths,
      imports: [{ sources: ["main.ts"], allow: functionPaths }],
    });
    for (const namespace of ["server", "fs"]) {
      const id = resolver.resolveId(
        `muon:${namespace}`,
        join(directory, "main.ts"),
      )!.id;
      await writeFile(join(assets, `${namespace}.mjs`), resolver.load(id)!);
    }
    const readyPath = join(directory, "page-ready");
    await writeFile(
      join(assets, "index.html"),
      `<!doctype html><meta charset="utf-8">
<title>Muon asset HTTP driver</title><h1>Asset HTTP driver</h1><script type="module">
const filesystem = ${mode === "simple" ? "window.muon.fs" : "await import('./fs.mjs')"};
try {
  window.serverApi = ${mode === "simple" ? "window.muon.server" : "await import('./server.mjs')"};
} catch (error) {
  window.setupError = String(error);
}
await filesystem.writeTextFile(${JSON.stringify(readyPath)}, 'ready', 'utf8', undefined);
</script>`,
    );

    const cdpReservation = createServer();
    const serverReservation = createServer();
    cdpReservation.listen(0, "127.0.0.1");
    serverReservation.listen(0, "127.0.0.1");
    await Promise.all([
      once(cdpReservation, "listening"),
      once(serverReservation, "listening"),
    ]);
    const cdpAddress = cdpReservation.address();
    const serverAddress = serverReservation.address();
    if (
      !cdpAddress ||
      typeof cdpAddress === "string" ||
      !serverAddress ||
      typeof serverAddress === "string"
    )
      throw new Error("Cannot reserve asset test ports");
    await Promise.all(
      [cdpReservation, serverReservation].map(async (listener) => {
        await new Promise<void>((done, reject) =>
          listener.close((error) => (error ? reject(error) : done())),
        );
      }),
    );
    const origin = "asset://main";
    const endpoint = `http://127.0.0.1:${serverAddress.port}`;
    const config = join(directory, "muon.json");
    await writeFile(
      config,
      JSON.stringify({
        asset: { sourcePath: join(directory, "assets") },
        browser: {
          startPage: `${origin}/index.html`,
          profilePath: join(directory, "profile"),
          titleBarType: "native",
        },
        network: {
          allow: [
            `${origin}/**`,
            `${endpoint}/**`,
            `${endpoint.replace("http:", "ws:")}/**`,
          ],
          localAccess: {
            loopbackOrigins: [{ scheme: "asset", domain: "main" }],
          },
        },
        plugin: {
          ...(mode === "simple" ? { mode } : resolver.getRuntimePluginConfig()),
          path: join(directory, "plugins"),
          pages: [`${origin}/**`],
          plugins: [{ name: "internal", allow: functionPaths }],
        },
        cdp: { enable: true, port: cdpAddress.port },
      }),
    );
    // A native file notification confirms module initialization without relying
    // on the loopback request that this test is about to exercise.
    const events = new EventEmitter();
    const readinessWatcher = watch(directory, (_event, filename) => {
      if (filename === "page-ready") events.emit("page-ready");
    });
    const pageReady = once(events, "page-ready", {
      signal: AbortSignal.timeout(120000),
    });
    const child = spawn(
      join(runtime, "muon-core"),
      ["-c", config, "--ozone-platform=x11", "--disable-gpu"],
      {
        cwd: runtime,
        stdio: ["ignore", "pipe", "pipe"],
      },
    );
    const exited = once(child, "close");
    let diagnostics = "";
    const debugReady = new Promise<void>((done, reject) => {
      child.once("error", reject);
      child.once("close", () =>
        reject(
          new Error("Muon exited before its debugging endpoint was ready"),
        ),
      );
      child.stdout.on("data", (data: Buffer) => {
        diagnostics += data.toString();
      });
      child.stderr.on("data", (data: Buffer) => {
        diagnostics += data.toString();
        if (diagnostics.includes("DevTools listening on")) done();
      });
    });
    const ready = Promise.all([pageReady, debugReady]);
    const client = new Client({ name: "muon-server-asset-test", version: "1" });
    const mcp = await createConnection({
      browser: { cdpEndpoint: `http://127.0.0.1:${cdpAddress.port}` },
      outputDir: join(directory, "playwright"),
    });
    const [clientTransport, serverTransport] =
      InMemoryTransport.createLinkedPair();
    const call = async (
      name: string,
      args: Record<string, unknown>,
    ): Promise<string> => {
      const result = await client.callTool({ name, arguments: args });
      const text = (result.content as Array<{ type: string; text?: string }>)
        .filter((entry) => entry.type === "text")
        .map((entry) => entry.text ?? "")
        .join("\n");
      if (result.isError) throw new Error(text);
      return text;
    };
    const evaluate = async <T>(source: string): Promise<T> => {
      const text = await call("browser_evaluate", {
        function: `async () => { ${source} }`,
      });
      const result = /### Result\n([\s\S]*?)(?:\n### |$)/u.exec(text)?.[1];
      if (result === undefined) throw new Error(text);
      return JSON.parse(result) as T;
    };
    try {
      await ready;
      await mcp.connect(serverTransport);
      await client.connect(clientTransport);
      const tabs = await call("browser_tabs", { action: "list" });
      const index = tabs.match(/- (\d+):[^\n]*Muon asset HTTP driver/)?.[1];
      expect(index, tabs).toBeDefined();
      await call("browser_tabs", { action: "select", index: Number(index) });
      expect(
        await evaluate(
          `return {url: location.href, api: typeof window.serverApi?.createServer, error: window.setupError ?? null};`,
        ),
      ).toEqual({
        url: `${origin}/index.html`,
        api: "function",
        error: null,
      });
      expect(
        await evaluate(`
        window.token = crypto.randomUUID();
        window.server = await window.serverApi.createServer({
          name: 'asset-driver', port: ${serverAddress.port},
          bearerToken: window.token, allowedOrigins: [${JSON.stringify(origin)}],
        });
        window.handled = 0;
        await window.server.attach({fetch: async request => {
          window.handled++;
          return new Response(await request.arrayBuffer(), {status: 201, headers: {
            'Content-Type': 'application/octet-stream',
            'X-Asset-Driver': 'binary',
            'Mcp-Session-Id': request.headers.get('Mcp-Session-Id') ?? 'missing',
            'MCP-Protocol-Version': request.headers.get('MCP-Protocol-Version') ?? 'missing',
          }});
        }});
        await window.server.listen();
        return 'listening';
      `),
      ).toBe("listening");

      expect(
        await evaluate(`
        const response = await fetch(${JSON.stringify(`${endpoint}/binary`)}, {
          method: 'POST',
          headers: {
            Authorization: 'Bearer ' + window.token,
            'Content-Type': 'application/octet-stream',
            'Mcp-Session-Id': 'asset-session',
            'MCP-Protocol-Version': '2025-06-18',
          },
          body: new Uint8Array([0, 255, 1, 128, 13, 10]),
        });
        return {
          status: response.status, bytes: [...new Uint8Array(await response.arrayBuffer())],
          custom: response.headers.get('X-Asset-Driver'),
          session: response.headers.get('Mcp-Session-Id'),
          protocol: response.headers.get('MCP-Protocol-Version'),
        };
      `),
      ).toEqual({
        status: 201,
        bytes: [0, 255, 1, 128, 13, 10],
        custom: "binary",
        session: "asset-session",
        protocol: "2025-06-18",
      });

      expect(
        await evaluate(`
        const statuses = [];
        for (const headers of [{}, {Authorization: 'Bearer invalid-test-token'}]) {
          const response = await fetch(${JSON.stringify(`${endpoint}/binary`)}, {method: 'POST', headers, body: new Uint8Array([2])});
          statuses.push({status: response.status, challenge: response.headers.get('WWW-Authenticate')});
          await response.arrayBuffer();
        }
        return {statuses, handled: window.handled};
      `),
      ).toEqual({
        statuses: [
          { status: 401, challenge: "Bearer" },
          { status: 401, challenge: "Bearer" },
        ],
        handled: 1,
      });
      await evaluate(`await window.server.close(); return true;`);
      expect(
        await evaluate(`
        window.server = await window.serverApi.createServer({
          name: 'websocket-driver', port: ${serverAddress.port},
          bearerToken: window.token, allowedOrigins: [${JSON.stringify(origin)}],
        });
        window.wsRequests = [];
        window.wsClosed = [];
        window.binding = await window.server.attach({
          fetch: () => new Response('HTTP still works'),
          websocket: {
            accept: async request => {
              window.wsRequests.push([...request.headers]);
              const path = new URL(request.url).pathname;
              if (path === '/wait-accept') { await window.waitForClientAbort(request.signal); return false; }
              if (path === '/wait-open') return {};
              return path === '/chat' ? {protocol: 'chat'} : path === '/echo' ? {} : false;
            },
            open: async socket => {
              window.lastSocket = socket;
              if (new URL(socket.request.url).pathname === '/wait-open') await window.waitForClientAbort(socket.request.signal);
            },
            message: async (socket, data) => {
              if (data === 'wait-abort') { await window.waitForClientAbort(socket.request.signal); return; }
              if (data === 'close') { await socket.close(3001, '完了'); return; }
              if (data === 'throw') throw new Error('private handler detail');
              if (data === 'path') { await socket.send(new URL(socket.request.url).pathname); return; }
              if (data === 'invalid-close') {
                try { await socket.close(1006, 'invalid'); } catch {}
                await socket.send('still open'); return;
              }
              if (data === 'stop') { await window.server.close(); return; }
              await socket.send(data);
            },
            close: (socket, event) => { window.wsClosed.push({id: socket.id, ...event}); },
          },
        });
        await window.server.listen();
        const credential = 'muon.bearer.' + btoa(window.token).replace(/=/g, '').replace(/\\+/g, '-').replace(/\\//g, '_');
        const socket = new WebSocket(${JSON.stringify(`${endpoint.replace("http:", "ws:")}/echo`)}, ['muon.auth', credential]);
        socket.binaryType = 'arraybuffer';
        await new Promise((resolve, reject) => { socket.onopen = resolve; socket.onerror = () => reject(new Error('WebSocket connection failed')); });
        const exchange = async data => {
          const message = new Promise((resolve, reject) => { socket.onmessage = event => resolve(event.data); socket.onerror = reject; });
          socket.send(data);
          return await message;
        };
        const text = await exchange('日本語 WebSocket');
        const bytes = new Uint8Array(256 * 1024).map((_, index) => index % 251);
        const echoed = new Uint8Array(await exchange(bytes));
        const closed = new Promise(resolve => { socket.onclose = event => resolve({code: event.code, reason: event.reason, wasClean: event.wasClean}); });
        socket.close(1000, 'finished');
        return {text, size: echoed.length, exact: bytes.every((value, index) => value === echoed[index]), protocol: socket.protocol, closed: await closed, leaked: JSON.stringify(window.wsRequests).includes(window.token) || JSON.stringify(window.wsRequests).includes('muon.bearer.')};
      `),
      ).toEqual({
        text: "日本語 WebSocket",
        size: 256 * 1024,
        exact: true,
        protocol: "muon.auth",
        closed: { code: 1000, reason: "finished", wasClean: true },
        leaked: false,
      });
      const token = await evaluate<string>("return window.token;");
      const credential = `muon.bearer.${Buffer.from(token).toString("base64url")}`;
      const probe = async (
        headers: Record<string, string>,
        path: string,
      ): Promise<number> =>
        await new Promise<number>((resolveProbe, reject) => {
          const request = httpRequest(`${endpoint}${path}`, {
            headers: {
              Connection: "Upgrade",
              Upgrade: "websocket",
              "Sec-WebSocket-Key": "dGhlIHNhbXBsZSBub25jZQ==",
              "Sec-WebSocket-Version": "13",
              ...headers,
            },
          });
          request.on("error", reject);
          request.on("response", (response) => {
            response.resume();
            resolveProbe(response.statusCode!);
          });
          request.on("upgrade", (response, socket) => {
            socket.destroy();
            resolveProbe(response.statusCode!);
          });
          request.end();
        });
      const beforeRejected = await evaluate<number>(
        "return window.wsRequests.length;",
      );
      for (const [headers, status] of [
        [{}, 401],
        [{ Authorization: "Bearer invalid" }, 401],
        [
          {
            Authorization: `Bearer ${token}`,
            Origin: "https://untrusted.example",
          },
          403,
        ],
        [{ Authorization: `Bearer ${token}`, Host: "untrusted.example" }, 403],
        [
          {
            Authorization: `Bearer ${token}`,
            "Sec-WebSocket-Protocol": `muon.auth, ${credential}`,
          },
          401,
        ],
        [
          { "Sec-WebSocket-Protocol": "muon.auth, muon.bearer.aW52YWxpZA" },
          401,
        ],
      ] as Array<[Record<string, string>, number]>) {
        expect(await probe(headers, "/echo")).toBe(status);
      }
      expect(await evaluate("return window.wsRequests.length;")).toBe(
        beforeRejected,
      );
      expect(await probe({ Authorization: `Bearer ${token}` }, "/denied")).toBe(
        403,
      );
      expect(await probe({ Authorization: `Bearer ${token}` }, "/echo")).toBe(
        101,
      );
      expect(
        await evaluate(
          `return JSON.stringify(window.wsRequests).includes(window.token) || JSON.stringify(window.wsRequests).includes('muon.bearer.');`,
        ),
      ).toBe(false);

      const connect = async (
        path: string,
        protocols: string[],
      ): Promise<WebSocket> => {
        const socket = new WebSocket(
          `${endpoint.replace("http:", "ws:")}${path}`,
          protocols,
        );
        socket.binaryType = "arraybuffer";
        await new Promise<void>((opened, reject) => {
          socket.addEventListener("open", () => opened(), { once: true });
          socket.addEventListener(
            "error",
            () => reject(new Error("External WebSocket connection failed")),
            { once: true },
          );
        });
        return socket;
      };
      const exchange = async (
        socket: WebSocket,
        data: string | Uint8Array,
      ): Promise<unknown> => {
        const received = new Promise<unknown>((resolveMessage, reject) => {
          socket.addEventListener(
            "message",
            (event) => resolveMessage(event.data),
            { once: true },
          );
          socket.addEventListener("error", reject, { once: true });
          socket.addEventListener(
            "close",
            () => reject(new Error("WebSocket closed before echo")),
            { once: true },
          );
        });
        socket.send(typeof data === "string" ? data : new Uint8Array(data));
        return await received;
      };
      const closed = async (
        socket: WebSocket,
      ): Promise<{ code: number; reason: string; wasClean: boolean }> =>
        await new Promise((resolveClose) =>
          socket.addEventListener(
            "close",
            (event) =>
              resolveClose({
                code: event.code,
                reason: event.reason,
                wasClean: event.wasClean,
              }),
            { once: true },
          ),
        );
      const external = await connect("/chat", [
        "chat",
        "muon.auth",
        credential,
      ]);
      expect(external.protocol).toBe("chat");
      expect(await exchange(external, "outside")).toBe("outside");
      expect(await exchange(external, "invalid-close")).toBe("still open");
      expect(await exchange(external, "path")).toBe("/chat");
      const payload = new Uint8Array(1024 * 1024).map(
        (_, index) => index % 239,
      );
      expect(
        new Uint8Array((await exchange(external, payload)) as ArrayBuffer),
      ).toEqual(payload);
      const externalClosed = closed(external);
      external.send("close");
      expect(await externalClosed).toEqual({
        code: 3001,
        reason: "完了",
        wasClean: true,
      });

      const failed = await connect("/echo", ["muon.auth", credential]);
      const failedClosed = closed(failed);
      failed.send("throw");
      expect(await failedClosed).toEqual({
        code: 1011,
        reason: "Handler failed",
        wasClean: true,
      });

      for (const phase of ["message", "open", "accept"] as const) {
        await evaluate(`
          window.abortWaitEntered = new Promise(resolve => window.enterAbortWait = resolve);
          window.clientAbortObserved = new Promise(resolve => window.observeClientAbort = resolve);
          window.waitForClientAbort = async signal => {
            const aborted = new Promise(resolve => {
              if (signal.aborted) resolve();
              else signal.addEventListener('abort', resolve, {once: true});
            });
            window.enterAbortWait();
            await aborted;
            window.observeClientAbort(signal.aborted);
          };
          return true;
        `);
        const raw = connectTcp({ host: "127.0.0.1", port: serverAddress.port });
        raw.on("error", () => {});
        raw.resume();
        try {
          await once(raw, "connect");
          const path = phase === "message" ? "/echo" : `/wait-${phase}`;
          const handshake = Buffer.from(
            [
              `GET ${path} HTTP/1.1`,
              `Host: 127.0.0.1:${serverAddress.port}`,
              "Connection: Upgrade",
              "Upgrade: websocket",
              "Sec-WebSocket-Version: 13",
              "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==",
              `Authorization: Bearer ${token}`,
              "",
              "",
            ].join("\r\n"),
          );
          const text = Buffer.from("wait-abort");
          const mask = Buffer.from([17, 34, 51, 68]);
          const frame =
            phase === "message"
              ? Buffer.concat([
                  Buffer.from([0x81, 0x80 | text.length]),
                  mask,
                  text.map(
                    (value, index) => value ^ mask[index % mask.length]!,
                  ),
                ])
              : Buffer.alloc(0);
          raw.write(Buffer.concat([handshake, frame]));
          expect(
            await evaluate("await window.abortWaitEntered; return true;"),
          ).toBe(true);
          const reset = once(raw, "close");
          raw.resetAndDestroy();
          await reset;
          // This awaits the signal, not elapsed time or a fixed sleep. The MCP
          // request deadline diagnoses a handler stranded after peer reset.
          try {
            expect(
              await evaluate("return await window.clientAbortObserved;"),
            ).toBe(true);
          } catch (error) {
            throw new Error(
              `The ${phase} handler did not observe the peer TCP reset`,
              { cause: error },
            );
          }
        } finally {
          raw.destroy();
        }
      }

      const detached = await connect("/echo", ["muon.auth", credential]);
      expect(await exchange(detached, "before detach")).toBe("before detach");
      const detachedClosed = closed(detached);
      await evaluate("await window.binding.detach(); return true;");
      expect((await detachedClosed).wasClean).toBe(false);
      expect(await probe({ Authorization: `Bearer ${token}` }, "/echo")).toBe(
        503,
      );
      expect(
        await evaluate(`
        try { await window.lastSocket.send('late'); return false; }
        catch { return true; }
      `),
      ).toBe(true);
      await evaluate(`await window.server.close(); return true;`);
      await evaluate(`
        window.startPublicServer = async () => {
          window.server = await window.serverApi.createServer({name: 'public-websocket', port: ${serverAddress.port}, allowedOrigins: [${JSON.stringify(origin)}]});
          await window.server.attach({
            fetch: () => new Response('public HTTP'),
            websocket: {
              accept: () => ({}),
              message: async (socket, data) => {
                if (data === 'stop') await window.server.close();
                else await socket.send(data);
              },
            },
          });
          await window.server.listen();
        };
        await window.startPublicServer(); return true;
      `);
      const publicSocket = await connect("/echo", []);
      expect(await exchange(publicSocket, "no bearer")).toBe("no bearer");
      expect(
        await evaluate(`
        const socket = new WebSocket(${JSON.stringify(endpoint.replace("http:", "ws:") + "/echo")});
        const echoed = new Promise((resolve, reject) => {
          socket.onopen = () => socket.send('asset without bearer');
          socket.onmessage = event => resolve(event.data);
          socket.onerror = reject;
        });
        const value = await echoed;
        const closed = new Promise(resolve => { socket.onclose = resolve; });
        socket.close(1000); await closed; return value;
      `),
      ).toBe("asset without bearer");
      const stopped = closed(publicSocket);
      publicSocket.send("stop");
      expect((await stopped).wasClean).toBe(false);
      await expect(fetch(endpoint)).rejects.toThrow();

      await evaluate("await window.startPublicServer(); return true;");
      const navigated = await connect("/echo", []);
      expect(await exchange(navigated, "before navigation")).toBe(
        "before navigation",
      );
      const navigatedClosed = closed(navigated);
      await call("browser_navigate", { url: `${origin}/index.html?mpa` });
      expect((await navigatedClosed).wasClean).toBe(false);
      await expect(fetch(endpoint)).rejects.toThrow();
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${diagnostics}`, {
        cause: error,
      });
    } finally {
      readinessWatcher.close();
      child.kill("SIGTERM");
      await exited;
      await client.close();
      await mcp.close();
      await rm(directory, { recursive: true, force: true });
    }
  },
);
