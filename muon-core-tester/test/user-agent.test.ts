// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { spawn } from "node:child_process";
import { EventEmitter, once } from "node:events";
import {
  copyFile,
  mkdtemp,
  readdir,
  rm,
  symlink,
  writeFile,
} from "node:fs/promises";
import { createServer } from "node:http";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";

import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { InMemoryTransport } from "@modelcontextprotocol/sdk/inMemory.js";
import { createConnection } from "@playwright/mcp";
import { describe, expect, it } from "vitest";

import { embedMuonConfigInCoreFile } from "../../muon-ui/src/embed-config.js";

const runtimeDirectory = resolve("../muon-core/.run/test-linux-amd64-release");

describe.skipIf(process.platform !== "linux")(
  "browser.userAgent through Playwright MCP",
  () => {
    it.each(["external", "embedded"])(
      "applies configured and default user agents with %s config",
      async (mode) => {
        let defaultUserAgent: string | undefined = undefined;
        for (const userAgent of [
          undefined,
          "",
          "MyApp/1.0 (User Agent Test)",
        ]) {
          const directory = await mkdtemp(join(tmpdir(), "muon-user-agent-"));
          const requests = new EventEmitter();
          const receivedHeaders = new Map<string, string | undefined>();
          const httpServer = createServer((request, response) => {
            const url = new URL(request.url ?? "/", "http://localhost");
            receivedHeaders.set(url.pathname, request.headers["user-agent"]);
            response.setHeader("Cache-Control", "no-store");
            if (url.pathname === "/echo") {
              response.end(request.headers["user-agent"]);
            } else if (url.pathname === "/popup-report") {
              response.end("ok");
              requests.emit("popup", url.searchParams.get("ua"));
            } else {
              response.setHeader("Content-Type", "text/html");
              response.end(
                url.pathname === "/popup"
                  ? `<!doctype html><title>User Agent Popup</title>
                   <script type="module">
                     await fetch('/popup-report?ua=' + encodeURIComponent(navigator.userAgent));
                   </script>`
                  : "<!doctype html><title>User Agent Main</title><p>Ready</p>",
              );
              if (url.pathname === "/initial") {
                requests.emit("initial", request.headers["user-agent"]);
              }
            }
          });
          try {
            httpServer.listen(0, "127.0.0.1");
            await once(httpServer, "listening");
            const address = httpServer.address();
            if (address === null || typeof address === "string") {
              throw new Error("HTTP test server did not get a port");
            }
            const origin = `http://127.0.0.1:${address.port}`;
            const portReservation = createServer();
            portReservation.listen(0, "127.0.0.1");
            await once(portReservation, "listening");
            const debuggerAddress = portReservation.address();
            if (
              debuggerAddress === null ||
              typeof debuggerAddress === "string"
            ) {
              throw new Error("CDP port reservation failed");
            }
            await new Promise<void>((resolveClose, reject) => {
              portReservation.close((error) =>
                error ? reject(error) : resolveClose(),
              );
            });
            const configPath = join(directory, "muon.json");
            await writeFile(
              configPath,
              JSON.stringify({
                browser: {
                  startPage: `${origin}/initial`,
                  profilePath: join(directory, "profile"),
                  titleBarType: "native",
                  ...(userAgent === undefined ? {} : { userAgent }),
                },
                cdp: { enable: true, port: debuggerAddress.port },
                network: { allow: [`${origin}/**`] },
                plugin: { plugins: [] },
              }),
            );
            // Share the large CEF runtime while keeping the executable, embedded
            // configuration and browser profile private to this launch.
            for (const name of await readdir(runtimeDirectory)) {
              if (name !== "muon-core" && name !== "muon.json") {
                await symlink(
                  join(runtimeDirectory, name),
                  join(directory, name),
                );
              }
            }
            const executable = join(directory, "muon-core");
            await copyFile(join(runtimeDirectory, "muon-core"), executable);
            if (mode === "embedded") {
              await embedMuonConfigInCoreFile({
                corePath: executable,
                configPath,
                outputPath: undefined,
              });
              await rm(configPath);
            }
            const client = new Client({
              name: "muon-user-agent-test",
              version: "1.0.0",
            });
            const mcp = await createConnection({
              browser: {
                cdpEndpoint: `http://127.0.0.1:${debuggerAddress.port}`,
              },
              outputDir: join(directory, "mcp-output"),
            });
            const initialRequest = once(requests, "initial", {
              signal: AbortSignal.timeout(120000),
            });
            const child = spawn(
              executable,
              [
                ...(mode === "external" ? ["-c", configPath] : []),
                "--ozone-platform=x11",
                "--disable-gpu",
                "--disable-vulkan",
              ],
              {
                cwd: directory,
                detached: true,
                stdio: ["ignore", "ignore", "pipe"],
              },
            );
            let stderr = "";
            child.stderr.on("data", (chunk: Buffer) => {
              stderr += chunk.toString();
            });
            const processFailure = new Promise<never>((_, reject) => {
              child.once("error", reject);
              child.once("exit", (code, signal) => {
                reject(new Error(`muon exited (${code ?? signal}): ${stderr}`));
              });
            });
            try {
              const [initialUserAgent] = await Promise.race([
                initialRequest,
                processFailure,
              ]);
              expect(typeof initialUserAgent).toBe("string");
              if (userAgent === undefined) {
                expect(initialUserAgent).toContain("Chrome/");
                defaultUserAgent = initialUserAgent as string;
              }
              const expected = userAgent || defaultUserAgent;
              expect(initialUserAgent).toBe(expected);
              const [clientTransport, serverTransport] =
                InMemoryTransport.createLinkedPair();
              await mcp.connect(serverTransport);
              await client.connect(clientTransport);
              const result = await client.callTool({
                name: "browser_evaluate",
                arguments: {
                  function: `async () => {
                  const header = await (await fetch('/echo')).text();
                  return navigator.userAgent === ${JSON.stringify(expected)} &&
                    header === ${JSON.stringify(expected)};
                }`,
                },
              });
              expect(result.isError, JSON.stringify(result)).not.toBe(true);
              expect(result.content).toEqual(
                expect.arrayContaining([
                  expect.objectContaining({
                    type: "text",
                    text: expect.stringContaining("### Result\ntrue"),
                  }),
                ]),
              );
              expect(receivedHeaders.get("/echo")).toBe(expected);
              const popupRequest = once(requests, "popup", {
                signal: AbortSignal.timeout(120000),
              });
              const popupResult = await client.callTool({
                name: "browser_evaluate",
                arguments: { function: "() => { window.open('/popup'); }" },
              });
              expect(popupResult.isError, JSON.stringify(popupResult)).not.toBe(
                true,
              );
              const [popupUserAgent] = await Promise.race([
                popupRequest,
                processFailure,
              ]);
              expect(popupUserAgent).toBe(expected);
              expect(receivedHeaders.get("/popup")).toBe(expected);
              expect(receivedHeaders.get("/popup-report")).toBe(expected);
            } finally {
              await client.close();
              await mcp.close();
              if (
                child.exitCode === null &&
                child.signalCode === null &&
                child.pid !== undefined
              ) {
                process.kill(-child.pid, "SIGTERM");
                await once(child, "close");
              }
            }
          } finally {
            httpServer.closeAllConnections();
            await new Promise<void>((resolveClose) => {
              httpServer.close(() => resolveClose());
            });
            await rm(directory, { recursive: true, force: true });
          }
        }
      },
    );
  },
);
