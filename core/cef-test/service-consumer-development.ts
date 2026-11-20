// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { exerciseConsumerNetwork } from "./service-consumer-network.ts";
import { execFile, spawn } from "node:child_process";
import { once } from "node:events";
import { readFile, readdir, writeFile } from "node:fs/promises";
import { basename, join, resolve, win32 } from "node:path";
import { promisify } from "node:util";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { InMemoryTransport } from "@modelcontextprotocol/sdk/inMemory.js";
import { createConnection } from "@playwright/mcp";
import type { RemoteAgent, RemoteManagedProcess } from "agent-rover";
import { expect, vi } from "vitest";
import {
  connectWindowsE2eAgent,
  resolveWindowsE2eEnvironment,
} from "../../scripts/windows-e2e-environment.ts";
import { discoverWindowsTestNetwork } from "./plugin-e2e/windows-network.ts";

/**
 * Runs the installed CLI and Vite plugin with real CEF and Node service children.
 * @param app - Independent npm consumer containing all three service runtimes.
 * @param root - Local diagnostic directory retained by the enclosing test.
 * @param target - Consumer target; development runs on Linux or Windows amd64.
 * @param port - CDP port reserved by the consumer fixture.
 * @param servicePorts - Loopback ports in native, QuickJS, Node.js order.
 * @returns Completion after both development commands and their children exit.
 */
export const runServiceConsumerDevelopment = async (
  app: string,
  root: string,
  target: string,
  port: number,
  servicePorts: readonly number[],
): Promise<void> => {
  if (target === "windows-i686") return;
  const windows = target === "windows-amd64";
  const environment = resolveWindowsE2eEnvironment(process.env);
  let agent: RemoteAgent | undefined;
  let relay: RemoteManagedProcess | undefined;
  let directory = app;
  let node = process.execPath;
  let endpoint = `http://127.0.0.1:${port}`;
  const remoteNode = async (source: string): Promise<string> => {
    const child = await agent!.processes.launchManaged({
      path: node,
      arguments: ["--input-type=module", "-e", source],
      captureStdout: true,
      captureStderr: true,
      createNoWindow: true,
    });
    try {
      const state = await child.waitForExit({ timeoutMs: 30000 });
      if (state.root.exitCode !== 0) throw new Error(await child.stderrText());
      return await child.stdoutText();
    } finally {
      await child.releaseAsync();
    }
  };
  try {
    if (windows) {
      await promisify(execFile)(process.execPath, [
        resolve("build-windows-cdp-relay.mjs"),
      ]);
      const connection = await connectWindowsE2eAgent(environment);
      if (connection.status !== "connected") throw new Error(connection.reason);
      agent = connection.agent;
      node = environment.nodePath;
      directory = win32.join(
        environment.workDir,
        "service-dev-consumers",
        basename(root),
      );
      await agent.files.syncDirectory({
        localPath: app,
        remotePath: directory,
        mode: "mirror",
        checksum: "sha256",
        exclude: ["node_modules/**", ".muon/**", "package-lock.json"],
      });
      for (const [name, source] of [
        ["muon-ui", process.env.MUON_SERVICE_CONSUMER_UI!],
        ["muon-plugin-sdk", process.env.MUON_SERVICE_CONSUMER_SDK!],
      ])
        await agent.files.writeFile(
          win32.join(directory, name + ".tgz"),
          await readFile(source!),
        );
      await remoteNode(
        `import {readFile,writeFile} from 'node:fs/promises';const path=${JSON.stringify(win32.join(directory, "package.json"))};const value=JSON.parse(await readFile(path,'utf8'));value.dependencies['muon-ui']='file:./muon-ui.tgz';value.dependencies['muon-plugin-sdk']='file:./muon-plugin-sdk.tgz';await writeFile(path,JSON.stringify(value));`,
      );
      const install = await agent.processes.launchManaged({
        path: node,
        arguments: [
          win32.join(win32.dirname(node), "node_modules/npm/bin/npm-cli.js"),
          "install",
          "--ignore-scripts",
          "--no-audit",
          "--no-fund",
        ],
        workingDirectory: directory,
        captureStdout: true,
        captureStderr: true,
        createNoWindow: true,
      });
      try {
        const state = await install.waitForExit({ timeoutMs: 300000 });
        expect(state.root.exitCode, await install.stderrText()).toBe(0);
      } finally {
        await install.releaseAsync();
      }
      const relayPath = win32.join(directory, "cdp-relay.exe");
      await agent.files.writeFile(
        relayPath,
        await readFile(
          resolve(".run/windows-cdp-relay/windows-amd64/muon-cdp-relay.exe"),
        ),
      );
      const network = await discoverWindowsTestNetwork(agent, node);
      relay = await agent.processes.launchManaged({
        path: relayPath,
        arguments: ["0", String(port)],
        captureStdout: true,
        captureStderr: true,
        createNoWindow: true,
      });
      const relayPort = await vi.waitUntil(
        async () => {
          const match = /^MUON_CDP_RELAY_PORT=(\d+)$/mu.exec(
            await relay!.stdoutText(),
          );
          return match ? Number(match[1]) : false;
        },
        { timeout: 30000, interval: 100 },
      );
      endpoint = `http://${network.cdpHost}:${relayPort}`;
    }
    for (const command of ["run", "vite"] as const) {
      let client: Client | undefined;
      let closeMcp: (() => Promise<void>) | undefined;
      let remote: RemoteManagedProcess | undefined;
      let remoteSocket: RemoteManagedProcess | undefined;
      let localSocket: ReturnType<typeof spawn> | undefined;
      let socketExit: Promise<unknown[]> | undefined;
      let local: ReturnType<typeof spawn> | undefined;
      let exited: Promise<unknown[]> | undefined;
      let diagnostics = "";
      const args =
        command === "run"
          ? ["node_modules/muon-ui/dist/cli.cjs", "run"]
          : [
              "node_modules/vite/bin/vite.js",
              "--host",
              "127.0.0.1",
              "--port",
              "0",
            ];
      const evaluate = async (source: string) => {
        const result = await client!.callTool({
          name: "browser_evaluate",
          arguments: { function: `async()=>{${source}}` },
        });
        const text = (result.content as { text?: string }[])
          .map((item) => item.text ?? "")
          .join("\n");
        if (result.isError) throw new Error(text);
        return JSON.parse(
          /### Result\n([\s\S]*?)(?:\n### |$)/u.exec(text)![1]!,
        );
      };
      try {
        if (agent)
          remote = await agent.processes.launchManaged({
            path: node,
            arguments: args,
            workingDirectory: directory,
            captureStdout: true,
            captureStderr: true,
            createNoWindow: true,
          });
        else {
          local = spawn(node, args, {
            cwd: directory,
            stdio: ["ignore", "pipe", "pipe"],
            env: {
              ...process.env,
              XDG_CONFIG_HOME: join(root, "development-xdg"),
              XDG_STATE_HOME: join(root, "development-state"),
            },
          });
          exited = once(local, "exit");
          local.stdout!.on("data", (chunk) => {
            diagnostics += String(chunk);
          });
          local.stderr!.on("data", (chunk) => {
            diagnostics += String(chunk);
          });
        }
        await vi.waitUntil(
          async () => {
            if (remote && !(await remote.rootSnapshot()).running)
              throw new Error(await remote.stderrText());
            if (local && (local.exitCode !== null || local.signalCode !== null))
              throw new Error(diagnostics);
            try {
              const response = await fetch(endpoint + "/json/list", {
                signal: AbortSignal.timeout(1000),
              });
              return ((await response.json()) as { type: string }[]).some(
                (page) => page.type === "page",
              );
            } catch {
              return false;
            }
          },
          { timeout: 120000, interval: 100 },
        );
        const mcp = await createConnection({
          browser: { cdpEndpoint: endpoint },
          outputDir: join(root, "development-mcp-" + command),
        });
        closeMcp = async () => {
          await mcp.close();
        };
        const [a, b] = InMemoryTransport.createLinkedPair();
        client = new Client({
          name: "service-consumer-development",
          version: "1",
        });
        await mcp.connect(b);
        await client.connect(a);
        await client.callTool({
          name: "browser_tabs",
          arguments: { action: "list" },
        });
        const ready = await client.callTool({
          name: "browser_wait_for",
          arguments: { text: "Packaged services ready" },
        });
        if (ready.isError) throw new Error(JSON.stringify(ready));
        const first = await evaluate(
          `globalThis.developmentNode=await api.acquire('node');const native=await api.acquire('native');const script=await api.acquire('script');try{return {node:await developmentNode.call('greet',['development']),identity:await developmentNode.call('identity',[]),native:await native.call('echo',['development']),script:await script.call('echo',['development'])}}finally{await native.release();await script.release()}`,
        );
        expect(first).toMatchObject({
          node: "Hello development",
          native: "development",
          script: "development",
        });
        expect(await evaluate(exerciseConsumerNetwork(servicePorts))).toBe(
          true,
        );
        if (command === "vite") {
          await evaluate(
            "globalThis.heldListener=await api.acquire('quickListener');return heldListener.generation;",
          );
          // A process outside the UI keeps its socket open across the page reload.
          const socketProgram = `
            import {once} from 'node:events';
            const socket=new WebSocket('ws://127.0.0.1:${servicePorts[1]}/echo',
              ['muon.auth','muon.bearer.'+Buffer.from('local-service').toString('base64url')]);
            await once(socket,'open');
            const closed=once(socket,'close');const echo=once(socket,'message');
            socket.send('before update');
            if((await echo)[0].data!=='before update')throw new Error('Unexpected WebSocket reply');
            console.log('socket ready');
            await closed;
          `;
          let socketOutput = "";
          if (agent)
            remoteSocket = await agent.processes.launchManaged({
              path: node,
              arguments: ["--input-type=module", "-e", socketProgram],
              captureStdout: true,
              captureStderr: true,
              createNoWindow: true,
            });
          else {
            localSocket = spawn(
              node,
              ["--input-type=module", "-e", socketProgram],
              { stdio: ["ignore", "pipe", "pipe"] },
            );
            socketExit = once(localSocket, "exit");
            localSocket.stdout!.on("data", (chunk) => {
              socketOutput += String(chunk);
            });
            localSocket.stderr!.on("data", (chunk) => {
              socketOutput += String(chunk);
            });
          }
          await vi.waitUntil(
            async () => {
              if (remoteSocket && !(await remoteSocket.rootSnapshot()).running)
                throw new Error(await remoteSocket.stderrText());
              if (localSocket?.exitCode != null) throw new Error(socketOutput);
              return (
                remoteSocket ? await remoteSocket.stdoutText() : socketOutput
              ).includes("socket ready");
            },
            { timeout: 15000, interval: 100 },
          );
          if (agent)
            await agent.files.writeFile(
              win32.join(directory, "worker/node-greeting.txt"),
              Buffer.from("Updated\n"),
            );
          else
            await writeFile(
              join(directory, "worker/node-greeting.txt"),
              "Updated\n",
            );
          await vi.waitUntil(
            async () => {
              try {
                return await evaluate(
                  `const handle=await api.acquire('node');try{return await handle.call('greet',['development'])==='Updated development'}finally{await handle.release()}`,
                );
              } catch {
                // Reloading destroys the old page context and service handles.
                return false;
              }
            },
            { timeout: 30000, interval: 100 },
          );
          if (remoteSocket) {
            const state = await remoteSocket.waitForExit({ timeoutMs: 15000 });
            expect(state.root.exitCode, await remoteSocket.stderrText()).toBe(
              0,
            );
          } else {
            await vi.waitUntil(() => localSocket!.exitCode !== null, {
              timeout: 15000,
            });
            await socketExit;
            expect(localSocket!.exitCode, socketOutput).toBe(0);
          }
          expect(await evaluate(exerciseConsumerNetwork(servicePorts))).toBe(
            true,
          );
          const identity = await evaluate(
            `const handle=await api.acquire('node');try{return await handle.call('identity',[])}finally{await handle.release()}`,
          );
          expect(identity).not.toBe(first.identity);
        }
        // Shutdown closes the development UI and its resident host together.
        await Promise.allSettled([
          evaluate("await browser.shutdown(0);return true;"),
        ]);
        if (remote) {
          const state = await remote.waitForExit({ timeoutMs: 30000 });
          expect(state.running).toBe(false);
          expect(state.root.exitCode, await remote.stderrText()).toBe(0);
        } else {
          await exited;
          expect(local!.exitCode, diagnostics).toBe(0);
        }
        const sessions = agent
          ? JSON.parse(
              await remoteNode(
                `import {readdir} from 'node:fs/promises';console.log(JSON.stringify(await readdir(${JSON.stringify(win32.join(directory, ".muon/development/windows-amd64"))})));`,
              ),
            )
          : await readdir(join(directory, ".muon/development/linux-amd64"));
        expect(sessions).toEqual([]);
      } finally {
        const cleanup = await Promise.allSettled([
          (async () => {
            await client?.close();
          })(),
          (async () => {
            await closeMcp?.();
          })(),
          (async () => {
            await remoteSocket?.releaseAsync();
            if (
              localSocket &&
              localSocket.exitCode === null &&
              localSocket.signalCode === null
            ) {
              localSocket.kill();
              await socketExit;
            }
          })(),
          (async () => {
            if (remote) {
              diagnostics =
                (await remote.stdoutText()) + (await remote.stderrText());
              await remote.releaseAsync();
            }
            if (local && local.exitCode === null && local.signalCode === null) {
              local.kill("SIGINT");
              await exited;
            }
          })(),
        ]);
        await writeFile(join(root, `development-${command}.log`), diagnostics);
        const failures = cleanup.flatMap((result) =>
          result.status === "rejected" ? [result.reason] : [],
        );
        if (failures.length)
          throw new AggregateError(
            failures,
            "Development consumer cleanup failed",
          );
      }
    }
  } finally {
    try {
      await relay?.releaseAsync();
    } finally {
      agent?.release();
    }
    // Restore the source fixture before constructing the independent distribution.
    await writeFile(join(app, "worker/node-greeting.txt"), "Hello\n");
  }
};
