// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import {
  Client,
  StreamableHTTPClientTransport,
} from "@modelcontextprotocol/client";
import { Client as PlaywrightClient } from "@modelcontextprotocol/sdk/client/index.js";
import { InMemoryTransport } from "@modelcontextprotocol/sdk/inMemory.js";
import { createConnection } from "@playwright/mcp";
import { spawn } from "node:child_process";
import { randomUUID } from "node:crypto";
import { EventEmitter, once } from "node:events";
import { watch } from "node:fs";
import { mkdir, mkdtemp, rm, writeFile } from "node:fs/promises";
import { createServer } from "node:net";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { build } from "vite";
import { expect, it } from "vitest";
import { createMuonCapabilityModuleResolver } from "../../ui/common/capability.ts";
import { startMcpDriver } from "./fixtures/server-mcp-driver.ts";
import { runCodexMcpClient } from "./fixtures/server-codex-client.ts";

it("keeps MCP application state across per-request SDK server instances", async () => {
  let handler!: (request: Request) => Response | Promise<Response>;
  let visible: unknown;
  const driver = await startMcpDriver(
    async () => ({
      id: "contract",
      name: "contract",
      attach: async (handlers) => {
        handler = handlers.fetch;
        return { detach: async () => {} };
      },
      listen: async () => ({
        url: "http://127.0.0.1:8765",
        host: "127.0.0.1",
        port: 8765,
      }),
      close: async () => {},
    }),
    8765,
    undefined,
    (state) => {
      visible = state;
    },
  );
  const client = new Client(
    { name: "mcp-driver-contract", version: "1" },
    {
      versionNegotiation: { mode: { pin: "2026-07-28" } },
    },
  );
  const transport = new StreamableHTTPClientTransport(
    new URL(driver.endpoint),
    {
      fetch: async (input, init) => await handler(new Request(input, init)),
    },
  );
  try {
    await client.connect(transport);
    expect(
      (await client.listTools()).tools.map((tool) => tool.name).sort(),
    ).toEqual(["read_state", "write_state"]);
    const written = await client.callTool({
      name: "write_state",
      arguments: { value: "contract updated" },
    });
    expect(written.isError).not.toBe(true);
    const read = await client.callTool({ name: "read_state", arguments: {} });
    expect(read.content).toEqual([
      {
        type: "text",
        text: JSON.stringify({ value: "contract updated", revision: 1 }),
      },
    ]);
    expect(visible).toEqual({ value: "contract updated", revision: 1 });
    expect(transport.protocolVersion).toBe("2026-07-28");
    expect(
      driver.observations.some(
        (entry) => entry.contentType === "text/event-stream",
      ),
    ).toBe(true);
  } finally {
    await client.close();
    await driver.close();
  }
});

it.each([
  { mode: "simple", authenticated: false },
  { mode: "simple", authenticated: true },
  { mode: "validate", authenticated: false },
  { mode: "validate", authenticated: true },
] as const)(
  "shares visible application state with external and browser MCP clients ($mode, Bearer=$authenticated)",
  async ({ mode, authenticated }) => {
    const runtime = resolve(
      process.env.MUON_TEST_LINUX_RUNTIME ??
        "../cef/.run/test-linux-amd64-debug",
    );
    const directory = await mkdtemp(join(tmpdir(), "muon-server-mcp-"));
    const assets = join(directory, "assets", "main");
    await mkdir(assets, { recursive: true });
    const bundle = await build({
      configFile: false,
      publicDir: false,
      logLevel: "warn",
      build: {
        write: false,
        minify: false,
        target: "es2022",
        lib: {
          entry: resolve("fixtures/server-mcp-driver.ts"),
          formats: ["es"],
          fileName: () => "driver.mjs",
        },
      },
    });
    for (const output of Array.isArray(bundle) ? bundle : [bundle]) {
      if (!("output" in output)) throw new Error("Missing MCP browser bundle");
      for (const entry of output.output) {
        await writeFile(
          join(assets, entry.fileName),
          entry.type === "chunk" ? entry.code : entry.source,
        );
      }
    }
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
    const reservations = [createServer(), createServer()];
    const ports: number[] = [];
    for (const reservation of reservations) {
      reservation.listen(0, "127.0.0.1");
      await once(reservation, "listening");
      const address = reservation.address();
      if (!address || typeof address === "string")
        throw new Error("Cannot reserve an MCP test port");
      ports.push(address.port);
    }
    for (const reservation of reservations) {
      await new Promise<void>((done, reject) =>
        reservation.close((error) => (error ? reject(error) : done())),
      );
    }
    const cdpPort = ports[0]!;
    const port = ports[1]!;
    const endpoint = `http://127.0.0.1:${port}/mcp`;
    const bearerToken = authenticated ? randomUUID() : undefined;
    const readyPath = join(directory, "page-ready");
    await writeFile(
      join(assets, "index.html"),
      `<!doctype html><meta charset="utf-8">
<title>Muon MCP state driver</title><h1>Muon MCP state driver</h1>
<output id="state" aria-label="Application state"></output><pre id="error"></pre>
<script type="module">
const filesystem = ${mode === "simple" ? "window.muon.fs" : "await import('./fs.mjs')"};
try {
  const {startMcpDriver} = await import('./driver.mjs');
  const {createServer} = ${mode === "simple" ? "window.muon.server" : "await import('./server.mjs')"};
  window.driver = await startMcpDriver(createServer, ${port}, ${JSON.stringify(bearerToken)}, state => {
    document.querySelector('#state').textContent = JSON.stringify(state);
  });
} catch (error) {
  window.setupError = String(error);
  document.querySelector('#error').textContent = window.setupError;
}
await filesystem.writeTextFile(${JSON.stringify(readyPath)}, 'ready', 'utf8', undefined);
</script>`,
    );
    const config = join(directory, "muon.json");
    await writeFile(
      config,
      JSON.stringify({
        asset: { sourcePath: join(directory, "assets") },
        browser: {
          startPage: "asset://main/index.html",
          profilePath: join(directory, "profile"),
          titleBarType: "native",
        },
        network: {
          allow: ["asset://main/**", `http://127.0.0.1:${port}/**`],
          localAccess: {
            loopbackOrigins: [{ scheme: "asset", domain: "main" }],
          },
        },
        plugin: {
          ...(mode === "simple" ? { mode } : resolver.getRuntimePluginConfig()),
          path: join(directory, "plugins"),
          pages: ["asset://main/**"],
          plugins: [{ name: "internal", allow: functionPaths }],
        },
        cdp: { enable: true, port: cdpPort },
      }),
    );
    const events = new EventEmitter();
    const watcher = watch(directory, (_event, filename) => {
      if (filename === "page-ready") events.emit("ready");
    });
    const pageReady = once(events, "ready", {
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
        reject(new Error("Muon exited before the MCP driver was ready")),
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
    const browser = new PlaywrightClient({
      name: "muon-mcp-browser-test",
      version: "1",
    });
    const playwright = await createConnection({
      browser: { cdpEndpoint: `http://127.0.0.1:${cdpPort}` },
      outputDir: join(directory, "playwright"),
    });
    const [clientTransport, serverTransport] =
      InMemoryTransport.createLinkedPair();
    const callBrowser = async (
      name: string,
      args: Record<string, unknown>,
    ): Promise<string> => {
      const result = await browser.callTool({ name, arguments: args });
      const text = (result.content as Array<{ type: string; text?: string }>)
        .filter((entry) => entry.type === "text")
        .map((entry) => entry.text ?? "")
        .join("\n");
      if (result.isError) throw new Error(text);
      return text;
    };
    const evaluate = async <T>(source: string): Promise<T> => {
      const text = await callBrowser("browser_evaluate", {
        function: `async () => { ${source} }`,
      });
      const result = /### Result\n([\s\S]*?)(?:\n### |$)/u.exec(text)?.[1];
      if (result === undefined) throw new Error(text);
      return JSON.parse(result) as T;
    };
    const externalTypes: Array<string | null> = [];
    const external = new Client({
      name: "muon-external-mcp-test",
      version: "1",
    });
    const externalTransport = new StreamableHTTPClientTransport(
      new URL(endpoint),
      {
        requestInit: {
          headers:
            bearerToken === undefined
              ? {}
              : { Authorization: `Bearer ${bearerToken}` },
        },
        fetch: async (input, init) => {
          const response = await fetch(input, init);
          externalTypes.push(
            response.headers.get("Content-Type")?.split(";")[0] ?? null,
          );
          return response;
        },
      },
    );
    const callExternal = async (
      name: string,
      args: Record<string, unknown>,
    ) => {
      const result = await external.callTool({ name, arguments: args });
      expect(result.isError).not.toBe(true);
      const content = result.content.find((entry) => entry.type === "text");
      if (!content || content.type !== "text")
        throw new Error("Missing MCP state result");
      return JSON.parse(content.text) as { value: string; revision: number };
    };
    try {
      await ready;
      await playwright.connect(serverTransport);
      await browser.connect(clientTransport);
      const tabs = await callBrowser("browser_tabs", { action: "list" });
      const index = tabs.match(/- (\d+):[^\n]*Muon MCP state driver/)?.[1];
      expect(index, tabs).toBeDefined();
      await callBrowser("browser_tabs", {
        action: "select",
        index: Number(index),
      });
      expect(
        await evaluate(
          `return {url: location.href, endpoint: window.driver?.endpoint, error: window.setupError ?? null};`,
        ),
      ).toEqual({
        url: "asset://main/index.html",
        endpoint,
        error: null,
      });
      await external.connect(externalTransport);
      expect(
        (await external.listTools()).tools.map((tool) => tool.name).sort(),
      ).toEqual(["read_state", "write_state"]);
      expect(await callExternal("read_state", {})).toEqual({
        value: "initial",
        revision: 0,
      });
      const externalState = {
        value: `External ${mode} ${authenticated}`,
        revision: 1,
      };
      expect(
        await callExternal("write_state", { value: externalState.value }),
      ).toEqual(externalState);
      expect(
        await evaluate(
          `return JSON.parse(document.querySelector('#state').textContent);`,
        ),
      ).toEqual(externalState);
      expect(await callBrowser("browser_snapshot", {})).toContain(
        externalState.value,
      );
      expect(
        await evaluate(
          `return await window.driver.callFromBrowser('read_state', {});`,
        ),
      ).toEqual({
        state: externalState,
        protocol: "2026-07-28",
        discovered: true,
      });
      const browserState = {
        value: `Browser ${mode} ${authenticated}`,
        revision: 2,
      };
      expect(
        await evaluate(
          `return await window.driver.callFromBrowser('write_state', {value: ${JSON.stringify(browserState.value)}});`,
        ),
      ).toEqual({
        state: browserState,
        protocol: "2026-07-28",
        discovered: true,
      });
      expect(await callExternal("read_state", {})).toEqual(browserState);
      expect(
        await evaluate(
          `return JSON.parse(document.querySelector('#state').textContent);`,
        ),
      ).toEqual(browserState);
      expect(await callBrowser("browser_snapshot", {})).toContain(
        browserState.value,
      );
      const observed = await evaluate<{
        observations: Array<{
          origin: string | null;
          host: string | null;
          clonedOrigin: string | null;
          clonedHost: string | null;
          protocol: string | null;
          contentType: string | null;
        }>;
        eras: string[];
      }>(
        `return {observations: window.driver.observations, eras: window.driver.eras};`,
      );
      expect(observed.eras).toContain("legacy");
      expect(observed.eras).toContain("modern");
      expect(observed.observations).toContainEqual({
        origin: "asset://main",
        host: `127.0.0.1:${port}`,
        clonedOrigin: "asset://main",
        clonedHost: `127.0.0.1:${port}`,
        protocol: "2026-07-28",
        contentType: "text/event-stream",
      });
      expect(externalTypes).toContain("text/event-stream");
      if (process.env.MUON_TEST_CODEX_MCP === "1" && mode === "simple") {
        const codexDirectory = join(directory, "codex");
        await mkdir(codexDirectory);
        const codexState = { value: `Codex ${authenticated}`, revision: 3 };
        const events = await runCodexMcpClient(
          codexDirectory,
          endpoint,
          bearerToken,
          codexState.value,
        );
        const codexArtifacts = resolve(".run/server-mcp");
        await mkdir(codexArtifacts, { recursive: true });
        await writeFile(
          join(codexArtifacts, `codex-${mode}-${authenticated}.jsonl`),
          events,
        );
        const calls = events
          .trim()
          .split("\n")
          .map((line) => JSON.parse(line))
          .filter(
            (event) =>
              event.type === "item.completed" &&
              event.item?.type === "mcp_tool_call",
          )
          .map((event) => ({
            server: event.item.server,
            tool: event.item.tool,
            status: event.item.status,
            error: event.item.error,
          }));
        expect(calls).toEqual(
          ["read_state", "write_state", "read_state"].map((tool) => ({
            server: "muon",
            tool,
            status: "completed",
            error: null,
          })),
        );
        expect(await callExternal("read_state", {}), events).toEqual(
          codexState,
        );
        expect(
          await evaluate(
            `return JSON.parse(document.querySelector('#state').textContent);`,
          ),
        ).toEqual(codexState);
        expect(await callBrowser("browser_snapshot", {})).toContain(
          codexState.value,
        );
      }
      await evaluate("await window.driver.close(); return true;");
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${diagnostics}`, {
        cause: error,
      });
    } finally {
      watcher.close();
      await externalTransport.terminateSession();
      await external.close();
      child.kill("SIGTERM");
      await exited;
      await browser.close();
      await playwright.close();
      await rm(directory, { recursive: true, force: true });
    }
  },
  process.env.MUON_TEST_CODEX_MCP === "1" ? 360000 : 180000,
);
