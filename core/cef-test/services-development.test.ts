// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile, spawn, type ChildProcess } from "node:child_process";
import { EventEmitter, once } from "node:events";
import {
  access,
  mkdir,
  mkdtemp,
  readdir,
  rm,
  writeFile,
} from "node:fs/promises";
import { createServer as createHttpServer } from "node:http";
import { createServer as createTcpServer } from "node:net";
import { join, resolve } from "node:path";
import { promisify } from "node:util";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { InMemoryTransport } from "@modelcontextprotocol/sdk/inMemory.js";
import { createConnection as createMcp } from "@playwright/mcp";
import { packageService } from "muon-plugin-sdk/artifact";
import { createLogger, createServer, type ViteDevServer } from "vite";
import { expect, it } from "vitest";
import muon from "muon-ui/vite";

const runtime = resolve("../cef/.run/test-linux-amd64-release");

it.each(["run", "vite"] as const)(
  "calls native and QuickJS services from a CEF development UI via %s",
  async (command) => {
    await mkdir("../../.build", { recursive: true });
    const root = await mkdtemp(resolve("../../.build/cef-development-"));
    const events = new EventEmitter();
    let child: ChildProcess | undefined;
    let vite: ViteDevServer | undefined;
    let client: Client | undefined;
    let closeMcp: (() => Promise<void>) | undefined;
    let diagnostic = "";
    const observer = createHttpServer((request, response) => {
      response.setHeader("Access-Control-Allow-Origin", "asset://main");
      response.end("ok");
      events.emit(
        "ready",
        new URL(request.url ?? "/", "http://localhost").searchParams.get(
          "error",
        ),
      );
    });
    const previousExitCode = process.exitCode;
    const previousXdg = process.env.XDG_CONFIG_HOME;
    const previousBuilder = process.env.MUON_BUILDER_PATH;
    try {
      const portReservation = createTcpServer();
      portReservation.listen(0, "127.0.0.1");
      await once(portReservation, "listening");
      const cdpAddress = portReservation.address();
      if (!cdpAddress || typeof cdpAddress === "string")
        throw new Error("Missing CDP port");
      const portReleased = once(portReservation, "close");
      portReservation.close();
      await portReleased;
      observer.listen(0, "127.0.0.1");
      await once(observer, "listening");
      const address = observer.address();
      if (!address || typeof address === "string")
        throw new Error("Missing observer port");
      const origin = `http://127.0.0.1:${address.port}`;
      const library = join(root, "echo.so");
      await promisify(execFile)("cc", [
        "-std=c99",
        "-shared",
        "-fPIC",
        "-Wl,-soname,echo.so",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-I../../sdk/include",
        "../../sdk/examples/services/native-echo.c",
        "-o",
        library,
      ]);
      const artifact = await packageService({
        kind: "service",
        name: "echo",
        outputDirectory: join(root, "artifact"),
        targets: { "linux-amd64": { library, language: "c", cardio: false } },
      });
      await mkdir(join(root, "assets/main"), { recursive: true });
      const html = `<!doctype html><title>Development services</title><link rel="icon" href="data:,"><h1>Development services</h1><pre id="result">Starting</pre>
<script type="module">
  globalThis.serviceReady = (async () => {
    const native = await window.muon.services.acquire('native');
    const quickjs = await window.muon.services.acquire('quickjs');
    const node = await window.muon.node.createNode();
    try {
      const path = await node.importModule('node:path');
      if (await path.basename('/a/page-node.txt') !== 'page-node.txt') throw new Error('Page Node regression');
    } finally { await node.release(); }
    const result = {
      replies: [await native.call('echo', ['native reply']), await quickjs.call('echo', ['QuickJS reply'])],
      generations: [String(native.generation), String(quickjs.generation)]
    };
    await native.release(); await quickjs.release();
    document.querySelector('#result').textContent = JSON.stringify(result);
    return result;
  })();
  let error = '';
  try { await serviceReady; } catch (failure) { error = String(failure); }
  await fetch((location.protocol === 'asset:' ? ${JSON.stringify(origin)} : '') + '/ready?error=' + encodeURIComponent(error));
</script>`;
      await writeFile(join(root, "assets/main/index.html"), html);
      await writeFile(join(root, "index.html"), html);
      await writeFile(
        join(root, "muon.json"),
        JSON.stringify({
          node: { project: resolve("fixtures/node-project") },
          services: {
            native: { runtime: "native", artifact, autostart: true },
            quickjs: {
              runtime: "quickjs",
              entry: resolve("../../sdk/test/fixtures/quickjs-service.ts"),
              autostart: true,
            },
          },
          browser: {
            startPage: "asset://main/index.html",
            profilePath: join(root, "production-profile"),
            titleBarType: "native",
          },
          network: {
            allow: ["asset://main/**", `${origin}/**`],
            localAccess: {
              loopbackOrigins: [{ scheme: "asset", domain: "main" }],
            },
          },
          plugin: {
            mode: "simple",
            pages: ["asset://main/**"],
            plugins: [
              {
                name: "internal",
                allow: [
                  "muon.services.*",
                  "muon.environments.*",
                  "muon.browser.*",
                ],
              },
            ],
          },
          cdp: { enable: true, port: cdpAddress.port },
        }),
      );
      const ready = once(events, "ready", {
        signal: AbortSignal.timeout(60_000),
      });
      let finished: Promise<unknown>;
      const environment = {
        ...process.env,
        XDG_CONFIG_HOME: join(root, "xdg"),
        MUON_BUILDER_PATH: resolve(
          "../../builder/.run/test-linux-amd64-release/muon-builder",
        ),
      };
      if (command === "run") {
        child = spawn(
          process.execPath,
          [
            resolve("../../ui/dist/cli.cjs"),
            "run",
            "--muon-path",
            runtime,
            "--cef-path",
            runtime,
            "--assets",
            "assets",
            "--json",
          ],
          { cwd: root, env: environment, stdio: ["ignore", "pipe", "pipe"] },
        );
        child.stdout!.on("data", (bytes: Buffer) => {
          diagnostic += bytes.toString();
        });
        child.stderr!.on("data", (bytes: Buffer) => {
          diagnostic += bytes.toString();
        });
        finished = once(child, "close");
      } else {
        const logger = createLogger("silent");
        logger.warn = (message) => {
          diagnostic += message + "\n";
        };
        process.env.XDG_CONFIG_HOME = environment.XDG_CONFIG_HOME;
        process.env.MUON_BUILDER_PATH = environment.MUON_BUILDER_PATH;
        vite = await createServer({
          root,
          configFile: false,
          customLogger: logger,
          plugins: [
            {
              name: "development-service-readiness",
              configureServer: (server) => {
                server.middlewares.use("/ready", (request, response) => {
                  response.end("ok");
                  events.emit(
                    "ready",
                    new URL(
                      request.url ?? "/",
                      "http://localhost",
                    ).searchParams.get("error"),
                  );
                });
              },
            },
            muon({
              muonPath: runtime,
              cefPath: runtime,
              pluginAccess: { mode: "simple" },
            }),
          ],
          server: { port: 0 },
        });
        finished = once(vite.httpServer!, "close");
        await vite.listen();
      }
      const [error] = await Promise.race([
        ready,
        (async () => {
          await finished;
          throw new Error(diagnostic);
        })(),
      ]);
      expect(error, diagnostic).toBe("");
      let generations: string[] | undefined;
      for (const recycled of [false, true]) {
        const mcp = await createMcp({
          browser: { cdpEndpoint: `http://127.0.0.1:${cdpAddress.port}` },
          outputDir: join(root, "mcp"),
        });
        closeMcp = async () => await mcp.close();
        const [a, b] = InMemoryTransport.createLinkedPair();
        client = new Client({
          name: "muon-development-services-test",
          version: "1",
        });
        await mcp.connect(b);
        await client.connect(a);
        const result = await client.callTool({
          name: "browser_evaluate",
          arguments: {
            function:
              "async () => { const results = await serviceReady; const uiAutostart = await window.muon.environments.getAutostart(); await window.muon.environments.setAutostart(true); return {results, uiAutostart}; }",
          },
        });
        expect(result.isError, JSON.stringify(result)).not.toBe(true);
        const content = (result.content as { type: string; text?: string }[])
          .filter((item) => item.type === "text")
          .map((item) => item.text ?? "")
          .join("\n");
        const actual = JSON.parse(
          /### Result\n([\s\S]*?)(?:\n### |$)/u.exec(content)?.[1] ?? "null",
        );
        expect(actual.results.replies).toEqual([
          "native reply",
          "QuickJS reply",
        ]);
        expect(actual.uiAutostart).toBe(recycled);
        if (recycled) expect(actual.results.generations).toEqual(generations);
        else generations = actual.results.generations;
        await expect(
          access(join(root, "production-profile")),
        ).rejects.toThrow();
        await expect(access(join(root, "xdg", "autostart"))).rejects.toThrow();
        if (!recycled) {
          const nextPage = once(events, "ready", {
            signal: AbortSignal.timeout(60_000),
          });
          await client.callTool({
            name: "browser_evaluate",
            arguments: {
              function:
                "() => { setTimeout(() => { void window.muon.browser.recycle(); }, 0); return 'recycling'; }",
            },
          });
          await client.close();
          await closeMcp();
          client = undefined;
          closeMcp = undefined;
          const [error] = await Promise.race([
            nextPage,
            (async () => {
              await finished;
              throw new Error(
                diagnostic || "Development command ended during recycle",
              );
            })(),
          ]);
          expect(error, diagnostic).toBe("");
        }
      }
      await client!.callTool({
        name: "browser_evaluate",
        arguments: {
          function:
            "() => { setTimeout(() => { void window.muon.browser.shutdown(); }, 0); return 'closing'; }",
        },
      });
      await finished;
      await vite?.close();
      if (child) expect(child.exitCode, diagnostic).toBe(0);
      expect(
        await readdir(join(root, ".muon", "development", "linux-amd64")),
      ).toEqual([]);
    } catch (error) {
      throw new Error(`Development ${command} failed:\n${diagnostic}`, {
        cause: error,
      });
    } finally {
      await client?.close();
      await closeMcp?.();
      await vite?.close();
      if (child && child.exitCode === null && child.signalCode === null) {
        const stopped = once(child, "close");
        child.kill();
        await stopped;
      }
      const observerClosed = once(observer, "close");
      observer.close();
      await observerClosed;
      process.exitCode = previousExitCode;
      if (previousXdg === undefined) delete process.env.XDG_CONFIG_HOME;
      else process.env.XDG_CONFIG_HOME = previousXdg;
      if (previousBuilder === undefined) delete process.env.MUON_BUILDER_PATH;
      else process.env.MUON_BUILDER_PATH = previousBuilder;
      await rm(root, { recursive: true, force: true });
    }
  },
);
