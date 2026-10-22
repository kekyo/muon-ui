// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { mkdir, readFile, writeFile } from "node:fs/promises";
import { join, resolve, win32 } from "node:path";
import type { RemoteAgent, RemoteManagedProcess } from "agent-rover";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { InMemoryTransport } from "@modelcontextprotocol/sdk/inMemory.js";
import { createConnection } from "@playwright/mcp";
import { afterAll, beforeAll, describe, expect, it, vi } from "vitest";
import { createMuonCapabilityModuleResolver } from "../../ui/common/capability.ts";
import {
  connectWindowsE2eAgent,
  resolveWindowsE2eEnvironment,
} from "../../scripts/windows-e2e-environment.ts";
import { connectWindowsAgent } from "./plugin-e2e/windows-agent.ts";
import { resolveWindowsRuntimeTarget } from "./plugin-e2e/windows-matrix.ts";
import { discoverWindowsTestNetwork } from "./plugin-e2e/windows-network.ts";
import { stageWindowsRuntimeDirectory } from "./plugin-e2e/windows-staging.ts";

const target = resolveWindowsRuntimeTarget(process.env.MUON_E2E_WINDOWS_TARGET);
const environment = resolveWindowsE2eEnvironment(process.env);
const connection = await connectWindowsE2eAgent(environment);
const describeWindowsE2e =
  connection.status === "connected" ? describe : describe.skip;
if (connection.status === "connected") connection.agent.release();
else
  process.stderr.write(`Skipping Windows server tests: ${connection.reason}\n`);
const remoteBase = win32.join(environment.workDir, "server", target.target);
const runtime = win32.join(remoteBase, "runtime");
const root = win32.join(remoteBase, `run-${process.pid}`);
const relayPath = win32.join(remoteBase, "muon-cdp-relay.exe");
const artifacts = resolve(".run/server-windows", target.target);
let agent: RemoteAgent | undefined;
let cdpHost = "";

beforeAll(async () => {
  // Build before acquiring the limited Windows session. The shared helper also
  // prepares the CDP relay and preserves agent-rover's busy retry behavior.
  const connected = await connectWindowsAgent(environment, target);
  if (connected.status !== "connected") throw new Error(connected.reason);
  agent = connected.agent;
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
  await agent.files.mkdir(root, { recursive: true });
  await mkdir(artifacts, { recursive: true });
  for (const name of ["socket", "http", "websocket"]) {
    const executable = `muon_server_${name}_test.exe`;
    await agent.files.writeFile(
      win32.join(runtime, executable),
      await readFile(
        resolve(`../cef/.build/test/${target.target}/release`, executable),
      ),
    );
  }
}, 600000);

afterAll(async () => {
  if (agent === undefined) return;
  try {
    if (await agent.files.exists(root))
      await agent.files.remove(root, { recursive: true });
  } finally {
    agent.release();
  }
});

// Run these clients on Windows itself: only the debugging relay is reachable
// from the Linux test runner; the product HTTP/WS listener stays on loopback.
const externalClient = `
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
const { endpoint, token } = JSON.parse(await readFile(process.argv[2], 'utf8'));
const headers = { Authorization: 'Bearer ' + token };
const unauthorized = await fetch(endpoint + '/binary', {method:'POST', body:'denied'});
assert.equal(unauthorized.status, 401);
await unauthorized.arrayBuffer();
const body = Uint8Array.from({length:256*1024}, (_, i) => i % 251);
const response = await fetch(endpoint + '/binary', {method:'POST', headers, body});
assert.equal(response.status, 201);
assert.equal(response.headers.get('X-Muon-Server'), 'windows');
assert.deepEqual(new Uint8Array(await response.arrayBuffer()), body);
const stream = await fetch(endpoint + '/stream', {headers});
const reader = stream.body.getReader();
let events = '';
while (events.length < 'data: first\\n\\n'.length) { const next = await reader.read(); assert.equal(next.done,false); events += new TextDecoder().decode(next.value); }
assert.equal(events, 'data: first\\n\\n');
const release = await fetch(endpoint + '/release', {method:'POST', headers});
await release.arrayBuffer();
for (;;) { const next = await reader.read(); if (next.done) break; events += new TextDecoder().decode(next.value); }
assert.equal(events, 'data: first\\n\\ndata: second\\n\\n');
const credential = 'muon.bearer.' + Buffer.from(token).toString('base64url');
const socket = new WebSocket(endpoint.replace('http:', 'ws:') + '/echo', ['muon.auth', credential]);
socket.binaryType = 'arraybuffer';
await new Promise((resolve, reject) => {socket.onopen=resolve; socket.onerror=()=>reject(new Error('External WebSocket failed'));});
const exchange = async data => {
  const pending = new Promise((resolve, reject) => {socket.onmessage=event=>resolve(event.data); socket.onerror=reject;});
  socket.send(data); return await pending;
};
assert.equal(await exchange('Windows external 日本語'), 'Windows external 日本語');
assert.deepEqual(new Uint8Array(await exchange(body)), body);
const closed = new Promise(resolve => {socket.onclose=event=>resolve({code:event.code,reason:event.reason,wasClean:event.wasClean});});
socket.close(1000, 'external complete');
assert.deepEqual(await closed, {code:1000,reason:'external complete',wasClean:true});
console.log(JSON.stringify({httpBytes:body.length,sse:events,websocketBytes:body.length,closed:true,unauthorized:401}));
`;

describeWindowsE2e(`Windows loopback server (${target.target})`, () => {
  it.each([
    ["http", []],
    ["websocket", []],
    ["socket", ["binary"]],
    ["socket", ["backpressure"]],
    ["socket", ["cancellation"]],
    ["socket", ["many"]],
  ] as const)("verifies native %s %j on Windows", async (name, args) => {
    if (agent === undefined) throw new Error("Windows agent is unavailable");
    const native = await agent.processes.launchManaged({
      path: win32.join(runtime, `muon_server_${name}_test.exe`),
      arguments: [...args],
      workingDirectory: runtime,
      captureStdout: true,
      captureStderr: true,
      createNoWindow: true,
    });
    try {
      const result = await native.waitForExit({ timeoutMs: 120000 });
      const stdout = await native.stdoutText();
      const stderr = await native.stderrText();
      await writeFile(
        join(artifacts, `native-${name}-${args[0] ?? "all"}.json`),
        JSON.stringify(
          { exitCode: result.root.exitCode, stdout, stderr },
          null,
          2,
        ),
      );
      expect(result.root.exitCode, stdout + stderr).toBe(0);
    } finally {
      await native.releaseAsync();
    }
  });

  it.each(["simple", "validate"] as const)(
    "serves HTTP, incremental SSE and WebSocket to Windows and CEF in %s mode",
    async (mode) => {
      if (agent === undefined) throw new Error("Windows agent is unavailable");
      const remote = agent;
      const directory = win32.join(root, mode);
      const assets = win32.join(directory, "assets", "main");
      await remote.files.mkdir(assets, { recursive: true });
      const portsProcess = await remote.processes.launchManaged({
        path: environment.nodePath,
        arguments: [
          "--input-type=module",
          "-e",
          `
        import {createServer} from 'node:net';
        const listeners = [createServer(), createServer()];
        for (const server of listeners) await new Promise(resolve => server.listen(0,'127.0.0.1',resolve));
        console.log(JSON.stringify(listeners.map(server=>server.address().port)));
        for (const server of listeners) await new Promise(resolve=>server.close(resolve));
      `,
        ],
        captureStdout: true,
        captureStderr: true,
        createNoWindow: true,
      });
      let cdpPort: number;
      let port: number;
      try {
        const result = await portsProcess.waitForExit({ timeoutMs: 30000 });
        expect(result.root.exitCode, await portsProcess.stderrText()).toBe(0);
        [cdpPort, port] = JSON.parse(await portsProcess.stdoutText()) as [
          number,
          number,
        ];
      } finally {
        await portsProcess.releaseAsync();
      }
      const endpoint = `http://127.0.0.1:${port}`;
      const resolver = createMuonCapabilityModuleResolver("/windows-server", {
        backend: "cef",
        functionPaths: ["muon.server.createServer"],
        imports: [
          { sources: ["main.ts"], allow: ["muon.server.createServer"] },
        ],
      });
      const id = resolver.resolveId(
        "muon:server",
        "/windows-server/main.ts",
      )!.id;
      await remote.files.writeFile(
        win32.join(assets, "server.mjs"),
        Buffer.from(resolver.load(id)!),
      );
      const title = `Muon Windows server ${target.target} ${mode}`;
      await remote.files.writeFile(
        win32.join(assets, "index.html"),
        Buffer.from(`<!doctype html><meta charset="utf-8">
      <title>${title}</title><h1>${title}</h1><pre id="result"></pre>
      <script>globalThis.pageReady = new Promise(resolve => globalThis.finishPageReady = resolve);</script>
      <script type="module">
      try { window.serverApi = ${mode === "simple" ? "window.muon.server" : "await import('./server.mjs')"}; }
      catch (error) { window.setupError = String(error); }
      finishPageReady();</script>`),
      );
      const configPath = win32.join(directory, "muon.json");
      await remote.files.writeFile(
        configPath,
        Buffer.from(
          JSON.stringify({
            asset: { sourcePath: win32.join(directory, "assets") },
            browser: {
              startPage: "asset://main/index.html",
              profilePath: win32.join(directory, "profile"),
              titleBarType: "native",
            },
            network: {
              allow: [
                "asset://main/**",
                `${endpoint}/**`,
                `${endpoint.replace("http:", "ws:")}/**`,
              ],
              localAccess: {
                loopbackOrigins: [{ scheme: "asset", domain: "main" }],
              },
            },
            plugin: {
              ...(mode === "simple"
                ? { mode }
                : resolver.getRuntimePluginConfig()),
              pages: ["asset://main/**"],
              plugins: [
                { name: "internal", allow: ["muon.server.createServer"] },
              ],
            },
            cdp: { enable: true, port: cdpPort },
          }),
        ),
      );
      let application: RemoteManagedProcess | undefined;
      let relay: RemoteManagedProcess | undefined;
      let closeMcp: (() => Promise<void>) | undefined;
      const client = new Client({ name: "muon-windows-server", version: "1" });
      try {
        relay = await remote.processes.launchManaged({
          path: relayPath,
          arguments: ["0", String(cdpPort)],
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
        application = await remote.processes.launchManaged({
          path: win32.join(runtime, "muon-core.exe"),
          arguments: ["-c", configPath],
          workingDirectory: directory,
          captureStdout: true,
          captureStderr: true,
        });
        const cdpEndpoint = `http://${cdpHost}:${relayPort}`;
        await vi.waitUntil(
          async () => {
            if (!(await application!.rootSnapshot()).running)
              throw new Error(
                `Muon exited: ${await application!.stderrText()}`,
              );
            try {
              const response = await fetch(cdpEndpoint + "/json/list", {
                signal: AbortSignal.timeout(1000),
              });
              const pages = (await response.json()) as { title: string }[];
              return pages.some((page) => page.title === title);
            } catch {
              return false;
            }
          },
          { timeout: 30000, interval: 100 },
        );
        const mcp = await createConnection({
          browser: { cdpEndpoint },
          outputDir: join(artifacts, mode),
        });
        closeMcp = async () => await mcp.close();
        const [a, b] = InMemoryTransport.createLinkedPair();
        await mcp.connect(b);
        await client.connect(a);
        const call = async (
          name: string,
          args: Record<string, unknown>,
        ): Promise<string> => {
          const result = await client.callTool({ name, arguments: args });
          const text = (result.content as { type: string; text?: string }[])
            .filter((item) => item.type === "text")
            .map((item) => item.text ?? "")
            .join("\n");
          if (result.isError) throw new Error(text);
          return text;
        };
        const evaluate = async <T>(source: string): Promise<T> => {
          const text = await call("browser_evaluate", {
            function: `async () => {await globalThis.pageReady; ${source}}`,
          });
          const result = /### Result\n([\s\S]*?)(?:\n### |$)/u.exec(text)?.[1];
          if (result === undefined) throw new Error(text);
          return JSON.parse(result) as T;
        };
        expect(
          await evaluate(
            "return {api:typeof serverApi?.createServer,error:window.setupError??null};",
          ),
        ).toEqual({ api: "function", error: null });
        const token = await evaluate<string>(`
        window.token = crypto.randomUUID();
        window.server = await serverApi.createServer({name:'windows-driver',port:${port},bearerToken:token,allowedOrigins:['asset://main']});
        window.wsClosed = [];
        await server.attach({fetch:async request=>{
          const path = new URL(request.url).pathname;
          if (path === '/stream') return new Response(new ReadableStream({start(controller){window.streamController=controller;controller.enqueue(new TextEncoder().encode('data: first\\n\\n'));}}),{headers:{'Content-Type':'text/event-stream'}});
          if (path === '/release') {streamController.enqueue(new TextEncoder().encode('data: second\\n\\n'));streamController.close();return new Response(null,{status:204});}
          return new Response(await request.arrayBuffer(),{status:201,headers:{'X-Muon-Server':'windows'}});
        },websocket:{accept:request=>new URL(request.url).pathname==='/echo'?{}:false,message:async(socket,data)=>{await socket.send(data);},close:(_socket,event)=>{wsClosed.push(event);}}});
        await server.listen(); return token;
      `);
        const clientPath = win32.join(directory, "external-client.mjs");
        const clientConfig = win32.join(directory, "external-client.json");
        await remote.files.writeFile(clientPath, Buffer.from(externalClient));
        await remote.files.writeFile(
          clientConfig,
          Buffer.from(JSON.stringify({ endpoint, token })),
        );
        const external = await remote.processes.launchManaged({
          path: environment.nodePath,
          arguments: [clientPath, clientConfig],
          workingDirectory: directory,
          captureStdout: true,
          captureStderr: true,
          createNoWindow: true,
        });
        try {
          const result = await external.waitForExit({ timeoutMs: 60000 });
          const stdout = await external.stdoutText();
          const stderr = await external.stderrText();
          await writeFile(
            join(artifacts, `${mode}-external.json`),
            JSON.stringify(
              { exitCode: result.root.exitCode, stdout, stderr },
              null,
              2,
            ),
          );
          expect(result.root.exitCode, stdout + stderr).toBe(0);
          expect(JSON.parse(stdout)).toEqual({
            httpBytes: 256 * 1024,
            sse: "data: first\n\ndata: second\n\n",
            websocketBytes: 256 * 1024,
            closed: true,
            unauthorized: 401,
          });
        } finally {
          await external.releaseAsync();
        }
        const browser = await evaluate(`
        const endpoint=${JSON.stringify(endpoint)};
        const headers={Authorization:'Bearer '+token};
        const body=Uint8Array.from({length:256*1024},(_,i)=>i%251);
        const response=await fetch(endpoint+'/binary',{method:'POST',headers,body});
        const http=new Uint8Array(await response.arrayBuffer());
        const stream=await fetch(endpoint+'/stream',{headers});const reader=stream.body.getReader();
        let sse='';
        while(sse.length < 'data: first\\n\\n'.length){const next=await reader.read();if(next.done)throw new Error('SSE ended before its first event');sse+=new TextDecoder().decode(next.value);}
        const incremental=sse==='data: first\\n\\n';
        const release=await fetch(endpoint+'/release',{method:'POST',headers});await release.arrayBuffer();
        for(;;){const next=await reader.read();if(next.done)break;sse+=new TextDecoder().decode(next.value);}
        const credential='muon.bearer.'+btoa(token).replace(/=/g,'').replace(/\\+/g,'-').replace(/\\//g,'_');
        const socket=new WebSocket(endpoint.replace('http:','ws:')+'/echo',['muon.auth',credential]);socket.binaryType='arraybuffer';
        await new Promise((resolve,reject)=>{socket.onopen=resolve;socket.onerror=()=>reject(new Error('CEF WebSocket failed'));});
        const exchange=async data=>{const pending=new Promise((resolve,reject)=>{socket.onmessage=event=>resolve(event.data);socket.onerror=reject;});socket.send(data);return await pending;};
        const text=await exchange('CEF 日本語');const bytes=new Uint8Array(await exchange(body));
        const closed=new Promise(resolve=>{socket.onclose=event=>resolve({code:event.code,reason:event.reason,wasClean:event.wasClean});});socket.close(1000,'browser complete');
        const result={status:response.status,header:response.headers.get('X-Muon-Server'),httpBytes:http.length,httpExact:body.every((v,i)=>v===http[i]),incremental,sse,text,websocketBytes:bytes.length,websocketExact:body.every((v,i)=>v===bytes[i]),closed:await closed};
        document.querySelector('#result').textContent=JSON.stringify(result);return result;
      `);
        expect(browser).toEqual({
          status: 201,
          header: "windows",
          httpBytes: 256 * 1024,
          httpExact: true,
          incremental: true,
          sse: "data: first\n\ndata: second\n\n",
          text: "CEF 日本語",
          websocketBytes: 256 * 1024,
          websocketExact: true,
          closed: { code: 1000, reason: "browser complete", wasClean: true },
        });
        await writeFile(
          join(artifacts, `${mode}-browser.json`),
          JSON.stringify(browser, null, 2),
        );
        expect(await call("browser_snapshot", {})).toContain(
          "browser complete",
        );
        expect(
          await evaluate(`
            await server.close();
            let refused = false;
            try { await fetch(${JSON.stringify(endpoint + "/closed")}); }
            catch { refused = true; }
            return {closed: wsClosed.length, refused};
          `),
        ).toEqual({ closed: 2, refused: true });
      } finally {
        try {
          if (application !== undefined)
            await writeFile(
              join(artifacts, `${mode}-runtime.log`),
              (await application.stdoutText()) +
                (await application.stderrText()),
            );
        } finally {
          await client.close();
          if (closeMcp !== undefined) await closeMcp();
          if (application !== undefined) await application.releaseAsync();
          if (relay !== undefined) await relay.releaseAsync();
        }
      }
    },
  );
});
