// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { spawn } from "node:child_process";
import { EventEmitter, once } from "node:events";
import { mkdtemp, rm, writeFile } from "node:fs/promises";
import { createServer, request } from "node:http";
import { createServer as reservePort } from "node:net";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { InMemoryTransport } from "@modelcontextprotocol/sdk/inMemory.js";
import { createConnection } from "@playwright/mcp";
import { expect, it } from "vitest";
import { createMuonCapabilityModuleResolver } from "../../ui/common/capability.ts";

it.each(["simple", "validate"] as const)(
  "serves page-owned HTTP with native authentication and CORS in %s mode",
  async (mode) => {
    const runtime = resolve(
      process.env.MUON_TEST_LINUX_RUNTIME ??
        "../cef/.run/test-linux-amd64-debug",
    );
    const directory = await mkdtemp(join(tmpdir(), "muon-server-"));
    const resolver = createMuonCapabilityModuleResolver(directory, {
      backend: "cef",
      functionPaths: ["muon.server.createServer"],
      imports: [{ sources: ["main.ts"], allow: ["muon.server.*"] }],
    });
    const moduleId = resolver.resolveId(
      "muon:server",
      join(directory, "main.ts"),
    )!.id;
    const moduleSource = resolver.load(moduleId)!;
    const pageEvents = new EventEmitter();
    const pageReady = once(pageEvents, "ready", {
      signal: AbortSignal.timeout(120000),
    });
    const pageServer = createServer((incoming, response) => {
      if (incoming.url === "/server.mjs") {
        response.setHeader("Content-Type", "text/javascript");
        response.end(moduleSource);
        return;
      }
      if (incoming.url === "/ready") {
        response.end("ready");
        pageEvents.emit("ready");
        return;
      }
      response.setHeader("Content-Type", "text/html");
      response.end(
        `<!doctype html><title>Muon server driver</title><h1>HTTP server driver</h1><script type=module>
      window.serverApi = ${mode === "simple" ? "window.muon.server" : "await import('/server.mjs')"};
      await fetch('/ready');</script>`,
      );
    });
    pageServer.listen(0, "127.0.0.1");
    await once(pageServer, "listening");
    const pageAddress = pageServer.address();
    if (!pageAddress || typeof pageAddress === "string")
      throw new Error("Missing page port");
    const origin = `http://127.0.0.1:${pageAddress.port}`;
    const reservation = reservePort();
    reservation.listen(0, "127.0.0.1");
    await once(reservation, "listening");
    const address = reservation.address();
    if (!address || typeof address === "string")
      throw new Error("Missing CDP port");
    reservation.close();
    await once(reservation, "close");
    const serverReservation = reservePort();
    serverReservation.listen(0, "127.0.0.1");
    await once(serverReservation, "listening");
    const serverAddress = serverReservation.address();
    if (!serverAddress || typeof serverAddress === "string")
      throw new Error("Missing HTTP port");
    const port = serverAddress.port;
    serverReservation.close();
    await once(serverReservation, "close");
    const config = join(directory, "muon.json");
    await writeFile(
      config,
      JSON.stringify({
        browser: { startPage: origin, profilePath: join(directory, "profile") },
        network: {
          allow: [`${origin}/**`, `http://127.0.0.1:${port}/**`],
          localAccess: {
            loopbackOrigins: [
              { scheme: "http", domain: "127.0.0.1", port: pageAddress.port },
            ],
          },
        },
        plugin: {
          ...(mode === "simple" ? { mode } : resolver.getRuntimePluginConfig()),
          path: join(directory, "plugins"),
          pages: [`${origin}/**`],
          plugins: [{ name: "internal", allow: ["muon.server.createServer"] }],
        },
        cdp: { enable: true, port: address.port },
      }),
    );
    const child = spawn(
      join(runtime, "muon-core"),
      ["-c", config, "--ozone-platform=x11", "--disable-gpu"],
      { cwd: runtime, stdio: ["ignore", "pipe", "pipe"] },
    );
    let diagnostics = "";
    const exited = once(child, "close");
    const ready = new Promise<void>((resolveReady, reject) => {
      child.once("error", reject);
      child.once("close", () =>
        reject(new Error(`Muon exited\n${diagnostics}`)),
      );
      child.stdout.on("data", (data: Buffer) => {
        diagnostics += data.toString();
      });
      child.stderr.on("data", (data: Buffer) => {
        diagnostics += data.toString();
        if (diagnostics.includes("DevTools listening on")) resolveReady();
      });
    });
    const client = new Client({ name: "muon-server-test", version: "1" });
    const mcp = await createConnection({
      browser: { cdpEndpoint: `http://127.0.0.1:${address.port}` },
      outputDir: join(directory, "mcp"),
    });
    const [a, b] = InMemoryTransport.createLinkedPair();
    const evaluate = async (source: string): Promise<string> => {
      const result = await client.callTool({
        name: "browser_evaluate",
        arguments: { function: source },
      });
      const text = (result.content as Array<{ type: string; text?: string }>)
        .map((entry) => entry.text ?? "")
        .join("\n");
      if (result.isError) throw new Error(`${text}\n${diagnostics}`);
      return text;
    };
    const send = async (
      headers: Record<string, string | string[]>,
    ): Promise<number> =>
      await new Promise<number>((resolveResponse, reject) => {
        const call = request(
          `http://127.0.0.1:${port}/state`,
          { headers },
          (response) => {
            response.resume();
            resolveResponse(response.statusCode ?? 0);
          },
        );
        call.on("error", (error) =>
          reject(
            new Error("HTTP request failed", {
              cause: error,
            }),
          ),
        );
        call.end();
      });
    try {
      await ready;
      await pageReady;
      await mcp.connect(b);
      await client.connect(a);
      const tabs = await client.callTool({
        name: "browser_tabs",
        arguments: { action: "list" },
      });
      const tabText = (tabs.content as Array<{ text?: string }>)
        .map((entry) => entry.text ?? "")
        .join("\n");
      const tab = tabText.match(/- (\d+):[^\n]*Muon server driver/);
      expect(tab, tabText).not.toBeNull();
      if (tab)
        await client.callTool({
          name: "browser_tabs",
          arguments: { action: "select", index: Number(tab[1]) },
        });
      expect(
        await evaluate("() => typeof window.serverApi?.createServer"),
        diagnostics,
      ).toContain("function");
      await evaluate(`async () => {
      window.server = await window.serverApi.createServer({name: 'driver', port: ${port}});
      window.value = 0;
      window.slowStarted = new Promise(resolve => { window.markSlowStarted = resolve; });
      window.binding = await window.server.attach({fetch: async request => {
        if (new URL(request.url).pathname === '/slow') {
          window.markSlowStarted();
          await new Promise(resolve => { window.finishSlow = resolve; });
          return new Response('stale');
        }
        if (new URL(request.url).pathname === '/binary') return new Response(await request.arrayBuffer(), {status: 201, headers: {'x-driver': 'binary'}});
        if (request.method === 'POST') window.value = (await request.json()).value;
        return Response.json({value: window.value});
      }});
    }`);
      await expect(fetch(`http://127.0.0.1:${port}/state`)).rejects.toThrow();
      await evaluate("async () => await window.server.listen()");
      expect(
        await evaluate(`async () => {
      const failures = [];
      for (const operation of [
        () => window.serverApi.createServer({name:'driver',port:${port}}),
        () => window.serverApi.createServer({name:'public',host:'0.0.0.0',port:${port}}),
        () => window.server.attach({fetch:() => new Response('duplicate')})
      ]) { try { await operation(); } catch { failures.push(true); } }
      window.conflict = await window.serverApi.createServer({name:'conflict',port:${port}});
      await window.conflict.attach({fetch:() => new Response('retry')});
      try { await window.conflict.listen(); } catch { failures.push(true); }
      return failures.length;
    }`),
      ).toMatch(/Result\s+4/);
      expect(
        await (await fetch(`http://127.0.0.1:${port}/state`)).json(),
      ).toEqual({ value: 0 });
      expect(
        await (
          await fetch(`http://127.0.0.1:${port}/state`, {
            method: "POST",
            body: JSON.stringify({ value: 42 }),
          })
        ).json(),
      ).toEqual({ value: 42 });
      const binary = await fetch(`http://127.0.0.1:${port}/binary`, {
        method: "POST",
        body: new Uint8Array([0, 255, 128, 10]),
      });
      expect(binary.status).toBe(201);
      expect(binary.headers.get("x-driver")).toBe("binary");
      expect([...new Uint8Array(await binary.arrayBuffer())]).toEqual([
        0, 255, 128, 10,
      ]);
      expect(await send({ Host: "attacker.example" })).toBe(403);
      expect(await send({ Origin: "https://attacker.example" })).toBe(403);
      expect(await send({ Origin: "null" })).toBe(403);
      const slowResponse = fetch(`http://127.0.0.1:${port}/slow`);
      await evaluate("async () => await window.slowStarted");
      // Another request completes while the first handler awaits application work.
      expect(await send({})).toBe(200);
      await evaluate("async () => await window.binding.detach()");
      expect((await slowResponse).status).toBe(503);
      expect(await send({})).toBe(503);
      await evaluate(
        'async () => { window.binding = await window.server.attach({fetch: () => new Response("rebound")}); }',
      );
      await evaluate("() => window.finishSlow()");
      expect(await (await fetch(`http://127.0.0.1:${port}/state`)).text()).toBe(
        "rebound",
      );
      await evaluate("async () => await window.server.close()");
      await expect(fetch(`http://127.0.0.1:${port}/state`)).rejects.toThrow();
      await evaluate("async () => await window.conflict.listen()");
      expect(await (await fetch(`http://127.0.0.1:${port}/state`)).text()).toBe(
        "retry",
      );
      await evaluate("async () => await window.conflict.close()");
      expect(
        await evaluate(`async () => {
        const invalid = [
          {bearerToken:''}, {bearerToken:'space token'}, {bearerToken:'=token'},
          {bearerToken:'token=middle'}, {bearerToken:'a'.repeat(4097)}, {bearerToken:null},
          {allowedOrigins:['*']}, {allowedOrigins:['null']}, {allowedOrigins:[location.origin+'/path']},
          {allowedOrigins:[location.origin+'/']}, {allowedOrigins:[location.origin+'?query']},
          {allowedOrigins:[location.origin+'#hash']}, {allowedOrigins:['http://user@localhost']},
          {allowedOrigins:null}, {allowedOrigins:[42]}, {allowedOrigins:Array(65).fill(location.origin)}
        ];
        let rejected = 0;
        for (const options of invalid) {
          try { const server = await window.serverApi.createServer({name:'invalid',port:${port},...options}); await server.close(); }
          catch { rejected++; }
        }
        return rejected === invalid.length;
      }`),
      ).toMatch(/Result\s+true/);
      await evaluate(`async () => {
      window.server = await window.serverApi.createServer({name:'private',port:${port},bearerToken:'test-secret',allowedOrigins:[location.origin]});
      window.accepted = 0;
      window.binding = await window.server.attach({fetch: async request => {
        window.accepted++;
        return new Response(await request.arrayBuffer(), {status:201,headers:{'x-driver':'protected','mcp-session-id':'example-session'}});
      }});
      await window.server.listen();
    }`);
      expect(await send({})).toBe(401);
      expect(await send({ Authorization: "Bearer wrong" })).toBe(401);
      expect(await send({ Authorization: "Basic test-secret" })).toBe(401);
      expect(await send({ Authorization: "Bearer test-secreT" })).toBe(401);
      expect(
        await send({ Authorization: ["Bearer test-secret", "Bearer wrong"] }),
      ).toBe(401);
      expect(await send({ Authorization: "Bearer test-secret" })).toBe(201);
      expect(
        await send({
          Authorization: "bEaReR  test-secret",
          Host: `localhost:${port}`,
        }),
      ).toBe(201);
      expect(
        await send({ Authorization: "Bearer test-secret", Origin: "" }),
      ).toBe(403);
      expect(
        await send({
          Authorization: "Bearer test-secret",
          Origin: [origin, "https://attacker.example"],
        }),
      ).toBe(403);
      expect(
        await send({
          Authorization: "Bearer test-secret",
          Origin: "https://attacker.example",
        }),
      ).toBe(403);
      expect(
        await send({
          Authorization: "Bearer test-secret",
          Host: "attacker.example",
        }),
      ).toBe(403);
      const preflight = await fetch(`http://127.0.0.1:${port}/binary`, {
        method: "OPTIONS",
        headers: {
          Origin: origin,
          "Access-Control-Request-Method": "POST",
          "Access-Control-Request-Headers":
            "authorization,content-type,mcp-session-id",
        },
      });
      expect(preflight.status).toBe(204);
      expect(preflight.headers.get("access-control-allow-origin")).toBe(origin);
      expect(preflight.headers.get("access-control-allow-methods")).toBe(
        "POST",
      );
      expect(preflight.headers.get("access-control-allow-headers")).toContain(
        "authorization",
      );
      for (const headers of [
        { Origin: origin, "Access-Control-Request-Method": "TRACE" },
        {
          Origin: origin,
          "Access-Control-Request-Method": "POST",
          "Access-Control-Request-Headers": "authorization, invalid header",
        },
        { "Access-Control-Request-Method": "POST" },
      ]) {
        expect(
          (
            await fetch(`http://127.0.0.1:${port}/binary`, {
              method: "OPTIONS",
              headers,
            })
          ).status,
        ).toBe(400);
      }
      expect(
        (
          await fetch(`http://127.0.0.1:${port}/binary`, {
            method: "OPTIONS",
            headers: { Origin: origin },
          })
        ).status,
      ).toBe(401);
      expect(await evaluate("() => window.accepted")).toMatch(/Result\s+2/);
      expect(
        await evaluate(`async () => {
      const response = await fetch('http://127.0.0.1:${port}/binary', {method:'POST',headers:{Authorization:'Bearer test-secret','Content-Type':'application/octet-stream','Mcp-Session-Id':'example-session'},body:new Uint8Array([0,255,128,10])});
      return {status:response.status,driver:response.headers.get('x-driver'),session:response.headers.get('mcp-session-id'),body:[...new Uint8Array(await response.arrayBuffer())]};
    }`),
      ).toContain('"status": 201');
      expect(await evaluate("() => window.accepted")).toMatch(/Result\s+3/);
      const unauthorized = await fetch(`http://127.0.0.1:${port}/binary`, {
        headers: { Origin: origin },
      });
      expect(unauthorized.status).toBe(401);
      expect(unauthorized.headers.get("www-authenticate")).toBe("Bearer");
      expect(unauthorized.headers.get("access-control-allow-origin")).toBe(
        origin,
      );
      await evaluate("async () => await window.binding.detach()");
      expect(
        await send({ Authorization: "Bearer test-secret", Origin: origin }),
      ).toBe(503);
      expect(await send({})).toBe(401);
      await evaluate("async () => await window.server.close()");
      await evaluate(`async () => {
        window.server = await window.serverApi.createServer({name:'both',port:${port},allowedOrigins:[location.origin]});
        await window.server.attach({fetch:() => new Response('shared')});
        await window.server.listen();
      }`);
      expect(
        await (await fetch(`http://127.0.0.1:${port}/shared`)).text(),
      ).toBe("shared");
      expect(
        await evaluate(
          `async () => await (await fetch('http://127.0.0.1:${port}/shared')).text()`,
        ),
      ).toContain('"shared"');
      await evaluate("async () => await window.server.close()");
      await evaluate(`async () => {
      window.server = await window.serverApi.createServer({name: 'driver', port: ${port}});
      await window.server.attach({fetch: () => new Response('reload')});
      await window.server.listen();
    }`);
      await client.callTool({
        name: "browser_navigate",
        arguments: { url: `${origin}/next` },
      });
      // A native operation in the new context provides a completion barrier after
      // releasing the old page, without a fixed sleep.
      await evaluate(`async () => {
      const server = await window.serverApi.createServer({name:'after-navigation',port:${port}});
      await server.close();
    }`);
      await expect(fetch(`http://127.0.0.1:${port}/state`)).rejects.toThrow();
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${diagnostics}`, {
        cause: error,
      });
    } finally {
      child.kill("SIGTERM");
      await exited;
      await client.close();
      await mcp.close();
      pageServer.closeAllConnections();
      await new Promise<void>((resolveClose, reject) =>
        pageServer.close((error) => (error ? reject(error) : resolveClose())),
      );
      await rm(directory, { recursive: true, force: true });
    }
  },
);
