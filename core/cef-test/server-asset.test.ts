// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { spawn } from "node:child_process";
import { EventEmitter, once } from "node:events";
import { watch } from "node:fs";
import { mkdir, mkdtemp, rm, writeFile } from "node:fs/promises";
import { createServer } from "node:net";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { InMemoryTransport } from "@modelcontextprotocol/sdk/inMemory.js";
import { createConnection } from "@playwright/mcp";
import { expect, it } from "vitest";
import { createMuonCapabilityModuleResolver } from "../../ui/common/capability.ts";

it.each(["simple", "validate"] as const)(
  "allows authenticated binary HTTP from the product asset origin in %s mode",
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
          allow: [`${origin}/**`, `${endpoint}/**`],
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
