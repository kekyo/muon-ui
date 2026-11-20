// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile, spawn, type ChildProcess } from "node:child_process";
import { EventEmitter, once } from "node:events";
import {
  chmod,
  copyFile,
  cp,
  mkdir,
  mkdtemp,
  readFile,
  readdir,
  rm,
  writeFile,
} from "node:fs/promises";
import { createServer } from "node:net";
import { tmpdir } from "node:os";
import { basename, dirname, join, resolve, win32 } from "node:path";
import { promisify } from "node:util";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { InMemoryTransport } from "@modelcontextprotocol/sdk/inMemory.js";
import { createConnection } from "@playwright/mcp";
import { expect, it, vi } from "vitest";
import {
  connectWindowsE2eAgent,
  resolveWindowsE2eEnvironment,
} from "../../scripts/windows-e2e-environment.ts";
import {
  launchWindowsTestProcess,
  type WindowsTestProcess,
} from "./plugin-e2e/windows-agent.ts";
import { discoverWindowsTestNetwork } from "./plugin-e2e/windows-network.ts";
import { exerciseConsumerNetwork } from "./service-consumer-network.ts";
import { runServiceConsumerDevelopment } from "./service-consumer-development.ts";

const target = process.env.MUON_SERVICE_CONSUMER_TARGET ?? "linux-amd64";

// Hold all three page references until the UI closes. Each runtime saves its own bytes.
const savedServiceValues = (next: number[] | undefined) => `
  globalThis.fileHandles = [];
  const values = [];
  for (const name of ['nativeState','quickState','nodeState']) {
    const handle = await api.acquire(name);
    fileHandles.push(handle);
    values.push(Array.from(await handle.call('get', [])));
    if (${JSON.stringify(next !== undefined)}) await handle.call('set', [new Uint8Array(${JSON.stringify(next ?? [])})]);
  }
  return values;
`;

const cleanupResources = async (actions: (() => Promise<unknown>)[]) => {
  const results = await Promise.allSettled(
    actions.map(async (action) => await action()),
  );
  const failures = results.flatMap((result) =>
    result.status === "rejected" ? [result.reason] : [],
  );
  if (failures.length)
    throw new AggregateError(failures, "Consumer cleanup failed");
};

it("drains independent consumer resources after a cleanup failure", async () => {
  const started: string[] = [];
  let complete!: () => void;
  const pending = new Promise<void>((resolve) => {
    complete = resolve;
  });
  const failure = new Error("Close failed");
  let drained = false;
  const cleanup = cleanupResources([
    async () => {
      started.push("mcp");
      throw failure;
    },
    async () => {
      started.push("host");
      await pending;
      drained = true;
    },
  ]);
  const result = Promise.allSettled([cleanup]);
  await Promise.resolve();
  expect(started).toEqual(["mcp", "host"]);
  expect(drained).toBe(false);
  complete();
  expect((await result)[0]!.status).toBe("rejected");
  expect(drained).toBe(true);
});

const runWindowsConsumer = async (
  distribution: string,
  launcherName: string,
  port: number,
  servicePorts: readonly number[],
  root: string,
) => {
  const environment = resolveWindowsE2eEnvironment(process.env);
  const connection = await connectWindowsE2eAgent(environment);
  if (connection.status !== "connected") throw new Error(connection.reason);
  const agent = connection.agent;
  const remoteRoot = win32.join(
    environment.workDir,
    "service-consumers",
    basename(root),
  );
  const runtime = win32.join(remoteRoot, "distribution with spaces 日本");
  let child: WindowsTestProcess | undefined;
  let relay:
    Awaited<ReturnType<typeof agent.processes.launchManaged>> | undefined;
  let client: Client | undefined;
  let closeMcp: (() => Promise<void>) | undefined;
  let iteration = 0;
  let failed = false;
  const logs: string[] = [];
  const runNode = async (source: string) => {
    const process = await agent.processes.launchManaged({
      path: environment.nodePath,
      arguments: ["--input-type=module", "-e", source],
      captureStdout: true,
      captureStderr: true,
      createNoWindow: true,
    });
    try {
      const state = await agent.processes.waitForExit(process.id, {
        timeoutMs: 30000,
      });
      if (state.exitCode !== 0) throw new Error(await process.stderrText());
      return await process.stdoutText();
    } finally {
      await process.releaseAsync();
    }
  };
  const disconnect = async () => {
    try {
      await cleanupResources([
        async () => await client?.close(),
        async () => await closeMcp?.(),
      ]);
    } finally {
      client = undefined;
      closeMcp = undefined;
    }
  };
  const evaluate = async (source: string) => {
    const result = await client!.callTool({
      name: "browser_evaluate",
      arguments: { function: `async()=>{${source}}` },
    });
    const text = (result.content as { text?: string }[])
      .map((item) => item.text ?? "")
      .join("\n");
    if (result.isError) throw new Error(text);
    return JSON.parse(/### Result\n([\s\S]*?)(?:\n### |$)/u.exec(text)![1]!);
  };
  try {
    await agent.files.syncDirectory({
      localPath: distribution,
      remotePath: runtime,
      mode: "mirror",
      checksum: "sha256",
    });
    const relayPath = win32.join(remoteRoot, "cdp-relay.exe");
    await agent.files.writeFile(
      relayPath,
      await readFile(
        resolve(".run/windows-cdp-relay", target, "muon-cdp-relay.exe"),
      ),
    );
    const host = (await discoverWindowsTestNetwork(agent, environment.nodePath))
      .cdpHost;
    relay = await agent.processes.launchManaged({
      path: relayPath,
      arguments: ["0", String(port)],
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
    const endpoint = `http://${host}:${relayPort}`;
    const launch = async () => {
      const capture = win32.join(remoteRoot, "capture-" + iteration++);
      await agent.files.mkdir(capture, { recursive: true });
      const launched = await launchWindowsTestProcess(
        agent,
        {
          path: win32.join(runtime, launcherName),
          arguments: [],
          workingDirectory: runtime,
          environment: { LOCALAPPDATA: win32.join(remoteRoot, "local") },
        },
        capture,
        false,
      );
      logs.push(win32.join(capture, "muon-stderr.log"));
      child = {
        ...launched,
        stderrText: async () =>
          await runNode(
            `import {readFile} from 'node:fs/promises';process.stdout.write(await readFile(${JSON.stringify(logs.at(-1))},'utf8'));`,
          ),
      };
      await vi.waitUntil(
        async () => {
          if (!(await child!.rootSnapshot()).running)
            throw new Error(await child!.stderrText());
          try {
            const result = await fetch(endpoint + "/json/list", {
              signal: AbortSignal.timeout(1000),
            });
            return ((await result.json()) as { title: string }[]).some(
              (page) => page.title === "Packaged services",
            );
          } catch {
            return false;
          }
        },
        { timeout: 180000, interval: 100 },
      );
      const mcp = await createConnection({
        browser: { cdpEndpoint: endpoint },
        outputDir: join(root, "mcp", String(iteration)),
      });
      closeMcp = async () => await mcp.close();
      const [a, b] = InMemoryTransport.createLinkedPair();
      client = new Client({ name: "service-consumer", version: "1" });
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
    };
    const closeUi = async (all: boolean) => {
      const closing = agent.processes.waitForExit(child!.id, {
        timeoutMs: 120000,
      });
      await Promise.allSettled([
        evaluate(
          `await browser.${all ? "shutdown(0)" : "close()"};return true;`,
        ),
        closing,
      ]);
      const stopped = await closing;
      expect(stopped.running, await child!.stderrText()).toBe(false);
      // Unmanaged applications can exit before the remote agent acquires a
      // process handle; its documented snapshot then has an unknown exit code.
      if (stopped.exitCode !== null)
        expect(stopped.exitCode, await child!.stderrText()).toBe(0);
      await disconnect();
    };
    await launch();
    const first = await evaluate(
      `globalThis.native=await api.acquire('native');globalThis.script=await api.acquire('script');globalThis.extension=await api.acquire('extension');return {native:native.generation,script:script.generation,extension:extension.generation,base64:await script.call('base64',[new Uint8Array([255,0])]),text:await native.call('echo',['日本\\0🙂']),next:await extension.call('next',[41])};`,
    );
    expect(first.text).toBe("日本\0🙂");
    expect(first.next).toBe(42);
    expect(first.base64).toBe("/wA=");
    expect(await evaluate(exerciseConsumerNetwork(servicePorts))).toBe(true);
    expect(await evaluate(savedServiceValues([5, 128, 1]))).toEqual(
      Array(3).fill([0, 255]),
    );
    const firstNode = await evaluate(
      `globalThis.nodeService=await api.acquire('node');return {generation:nodeService.generation,identity:await nodeService.call('identity',[]),greeting:await nodeService.call('greet',['packaged'])};`,
    );
    expect(firstNode.greeting).toBe("Hello packaged");
    expect(firstNode.identity).toMatch(
      new RegExp(
        `^\\d+:${target === "windows-i686" ? "ia32" : "x64"}:1\\.2\\.3$`,
      ),
    );
    expect(
      await evaluate(
        `let done;const event=new Promise(resolve=>done=resolve);const subscription=await script.subscribe('progress',done);await script.call('notify',['packaged event']);const value=await event;await subscription.close();return value;`,
      ),
    ).toBe("packaged event");
    await closeUi(false);
    // Read-only diagnostics do not hold a service reference. Compare clocks on
    // the remote machine so buffered output cannot pass the after-close check.
    const closedAt = Number(await runNode("console.log(Date.now());"));
    const progress = await vi.waitUntil(
      async () => {
        const log = await child!.stderrText();
        const matches = [...log.matchAll(/consumer-work:(\{.*\})/g)];
        return matches.some((match) => JSON.parse(match[1]!).at > closedAt);
      },
      { timeout: 15000, interval: 100 },
    );
    expect(progress).toBe(true);
    await launch();
    const reopened = await evaluate(
      `globalThis.native=await api.acquire('native');globalThis.script=await api.acquire('script');globalThis.extension=await api.acquire('extension');await api.setAutostart('extension',false);return {native:native.generation,script:script.generation,extension:extension.generation,next:await extension.call('next',[99]),autostart:(await api.getState('extension')).autostart};`,
    );
    expect(await evaluate(savedServiceValues([0, 17, 240]))).toEqual(
      Array(3).fill([5, 128, 1]),
    );
    expect(reopened).toEqual({
      native: first.native,
      script: first.script,
      extension: first.extension,
      next: 100,
      autostart: false,
    });
    const reopenedNode = await evaluate(
      `globalThis.nodeService=await api.acquire('node');return {generation:nodeService.generation,identity:await nodeService.call('identity',[]),greeting:await nodeService.call('greet',['packaged again'])};`,
    );
    expect(reopenedNode).toEqual({
      ...firstNode,
      greeting: "Hello packaged again",
    });
    await evaluate("await extension.release();return true;");
    await closeUi(true);
    await launch();
    expect(
      await evaluate("return (await api.getState('extension')).autostart;"),
    ).toBe(false);
    expect(
      await evaluate(
        `const handle=await api.acquire('node');try{return await handle.call('saved',[])}finally{await handle.release()}`,
      ),
    ).toBe("packaged again");
    expect(await evaluate(savedServiceValues(undefined))).toEqual(
      Array(3).fill([0, 17, 240]),
    );
    await evaluate(
      "await api.setAutostart('native',false);await api.setAutostart('script',false);await api.setAutostart('node',false);return true;",
    );
    await closeUi(true);
  } catch (error) {
    failed = true;
    throw error;
  } finally {
    if (client) {
      try {
        await evaluate(
          "for(const id of ['native','script','extension','node'])await api.setAutostart(id,false);await browser.shutdown(0);return true;",
        );
      } catch {}
    }
    try {
      await cleanupResources([
        disconnect,
        async () => await relay?.releaseAsync(),
        async () => {
          // Restrict fallback cleanup to this consumer, including the runtime
          // installed under its private LOCALAPPDATA directory.
          try {
            const owned = (await agent.processes.list()).filter(
              (process) =>
                process.running &&
                process.path
                  .toLowerCase()
                  .startsWith(remoteRoot.toLowerCase() + "\\"),
            );
            await cleanupResources(
              owned.map((process) => async () => {
                await agent.processes.kill(process.id);
                await agent.processes.waitForExit(process.id, {
                  timeoutMs: 30000,
                });
              }),
            );
          } finally {
            // Registry state is per user even with a private LOCALAPPDATA. Only
            // remove Run values whose command points into this test's directory.
            await runNode(`import {execFile} from 'node:child_process';import {promisify} from 'node:util';
            const execute=promisify(execFile),key='HKCU\\\\Software\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Run';
            let output='';try{output=(await execute('reg.exe',['query',key])).stdout;}catch(error){if(error.code!==1)throw error;}
            for(const line of output.split(/\\r?\\n/u)){const match=/^\\s+(.+?)\\s+REG_SZ\\s+(.*)$/u.exec(line);if(match&&match[2].toLowerCase().includes(${JSON.stringify(remoteRoot.toLowerCase() + "\\")}))await execute('reg.exe',['delete',key,'/v',match[1],'/f']);}`);
          }
        },
        async () => {
          for (const [index, log] of logs.entries()) {
            try {
              await writeFile(
                join(root, `windows-${index}.log`),
                await runNode(
                  `import {readFile} from 'node:fs/promises';process.stdout.write(await readFile(${JSON.stringify(log)},'utf8'));`,
                ),
              );
            } catch {}
          }
        },
      ]);
    } catch (error) {
      if (!failed) throw error;
      console.error(error);
    } finally {
      agent.release();
      console.log("Windows service consumer distribution: " + runtime);
    }
  }
};

it.skipIf(
  !process.env.MUON_SERVICE_CONSUMER_UI ||
    !process.env.MUON_SERVICE_CONSUMER_SDK,
)(
  "builds native, QuickJS and Node services using only published packages and retains them across UI sessions",
  async () => {
    const root = await mkdtemp(join(tmpdir(), "muon-service-consumer-"));
    const app = join(root, "app");
    await mkdir(app);
    const execute = promisify(execFile);
    const run = async (
      command: string,
      args: string[],
      environment: NodeJS.ProcessEnv = process.env,
    ) => {
      try {
        return (
          await execute(command, args, {
            cwd: app,
            env: environment,
            maxBuffer: 32 * 1024 * 1024,
          })
        ).stdout;
      } catch (error) {
        const failure = error as Error & { stdout?: string; stderr?: string };
        throw new Error(
          failure.message + "\n" + failure.stdout + "\n" + failure.stderr,
        );
      }
    };
    await writeFile(
      join(app, "package.json"),
      JSON.stringify({
        name: "service-package-consumer-" + basename(root).toLowerCase(),
        version: "1.0.0",
        type: "module",
        private: true,
        workspaces: ["quickjs"],
        scripts: { dependencies: "reskill" },
      }),
    );
    const version = async (name: string) =>
      JSON.parse(
        await readFile(
          resolve("../../node_modules", name, "package.json"),
          "utf8",
        ),
      ).version;
    await run("npm", [
      "install",
      "--ignore-scripts",
      "--no-audit",
      "--no-fund",
      resolve(process.env.MUON_SERVICE_CONSUMER_UI!),
      resolve(process.env.MUON_SERVICE_CONSUMER_SDK!),
      ...(await Promise.all(
        ["vite", "typescript", "prettier-max", "resolved-killer", "semver"].map(
          async (name) => name + "@" + (await version(name)),
        ),
      )),
    ]);
    const sdk = join(app, "node_modules/muon-plugin-sdk");
    const sdkCli = join(sdk, "dist/cli.mjs");
    await run(process.execPath, [
      sdkCli,
      "init",
      "producer/echo",
      "--name",
      "echo",
      "--kind",
      "service",
      "--language",
      "c",
    ]);
    await copyFile(
      join(sdk, "examples/services/native-echo.c"),
      join(app, "producer/echo/service.c"),
    );
    await run(process.execPath, [
      sdkCli,
      "init",
      "producer/counter",
      "--name",
      "counter",
      "--language",
      "c",
    ]);
    for (const name of ["state", "http", "server"]) {
      await run(process.execPath, [
        sdkCli,
        "init",
        "producer/" + name,
        "--name",
        name,
        "--kind",
        "service",
        "--language",
        "c",
      ]);
      await copyFile(
        join(sdk, "examples/services/native-" + name + ".c"),
        join(app, "producer", name, "service.c"),
      );
    }
    for (const name of ["echo", "counter", "state", "http", "server"]) {
      await run(process.execPath, [
        sdkCli,
        "build",
        "producer/" + name + "/muon-plugin.build.json",
        "--target",
        target,
        "--out",
        "artifacts/" + name,
      ]);
    }
    await rm(join(app, "producer"), { recursive: true });
    for (const name of ["echo", "counter", "state", "http", "server"])
      await rm(join(app, "artifacts", name, ".build"), {
        recursive: true,
        force: true,
      });
    for (const name of ["quickjs-native.ts", "counter.d.ts"])
      await copyFile(join(sdk, "examples/services", name), join(app, name));
    const quickjs = join(app, "quickjs");
    await cp(join(sdk, "examples/services/quickjs-project"), quickjs, {
      recursive: true,
    });
    await run("npm", [
      "install",
      "--ignore-scripts",
      "--no-audit",
      "--no-fund",
    ]);
    await run("npm", [
      "run",
      "typecheck",
      "--workspace",
      "quickjs-echo-service",
    ]);
    await run("npm", ["test", "--workspace", "quickjs-echo-service"]);
    const invalid = join(quickjs, "src/invalid.ts");
    await writeFile(
      invalid,
      "import service from './service'; import type { ServiceMethods } from 'muon-plugin-sdk/services'; declare const methods: ServiceMethods<typeof service.interface>; methods.base64({id:'1',signal:null!}, 'wrong');",
    );
    await expect(
      run("npm", ["run", "typecheck", "--workspace", "quickjs-echo-service"]),
    ).rejects.toThrow(/Uint8Array/);
    await rm(invalid);
    await mkdir(join(app, "worker"));
    for (const directory of [app, quickjs, join(app, "worker")])
      await writeFile(join(directory, "initial.bin"), new Uint8Array([0, 255]));
    for (const directory of [quickjs, join(app, "worker")])
      for (const name of ["state", "http", "server"])
        await copyFile(
          join(sdk, "examples/services", name + ".ts"),
          join(directory, name + ".ts"),
        );
    // Include the actual service entry points in the standalone ES2022-only type check.
    const compiler = JSON.parse(
      await readFile(join(quickjs, "tsconfig.json"), "utf8"),
    );
    compiler.include.push("http.ts", "state.ts", "server.ts");
    await writeFile(join(quickjs, "tsconfig.json"), JSON.stringify(compiler));
    await run("npm", [
      "run",
      "typecheck",
      "--workspace",
      "quickjs-echo-service",
    ]);
    await writeFile(
      invalid,
      [
        "import type { ServiceStartContext, ServiceStopReason, ServiceInterface } from 'muon-plugin-sdk/services';",
        "declare const context: ServiceStartContext<ServiceInterface>;",
        "const stop: ServiceStopReason = 'bad-stop-reason';",
        "context.fs.writeBytes('file', 'bad-file-bytes');",
        "context.server.listen({name:'test',port:23456,fetch:()=>({body:123})});",
        "context.server.listen({name:'test',port:23456,fetch:()=>({}),websocket:{accept:()=>true,message:()=>{}}});",
        "context.http.request({url:'http://localhost',responseType:'invalid-representation'});",
      ].join("\n"),
    );
    let errors = "";
    try {
      await run("npm", [
        "run",
        "typecheck",
        "--workspace",
        "quickjs-echo-service",
      ]);
    } catch (error) {
      errors = error instanceof Error ? error.message : String(error);
    }
    for (const line of [3, 4, 5, 6, 7])
      expect(errors).toContain("invalid.ts(" + line + ",");
    await rm(invalid);
    for (const name of ["node-files.ts", "node-greeting.txt"])
      await copyFile(
        join(sdk, "examples/services", name),
        join(app, "worker", name),
      );
    await writeFile(
      join(app, "worker/package.json"),
      JSON.stringify({
        name: "consumer-worker",
        type: "module",
        engines: { node: ">=20" },
        dependencies: {
          "muon-plugin-sdk": JSON.parse(
            await readFile(join(sdk, "package.json"), "utf8"),
          ).version,
          semver: await version("semver"),
        },
      }),
    );
    await writeFile(
      join(app, "worker/entry.ts"),
      `
import { readFile } from 'node:fs/promises';
import { join } from 'node:path';
import semver from 'semver';
import { defineService, defineServiceInterface } from 'muon-plugin-sdk/services';
import original from './node-files.ts';
export default defineService({
  interface: defineServiceInterface({ ...original.interface, methods: {
    ...original.interface.methods,
    identity: { parameters: [], returns: 'string', errors: [] },
    saved: { parameters: [], returns: 'string', errors: [] },
  }}),
  start: async context => {
    const running = await original.start(context);
    return { ...running, methods: { ...running.methods,
      identity: () => process.pid + ':' + process.arch + ':' + semver.valid('1.2.3'),
      saved: async () => await readFile(join(context.dataDirectory, 'last-name.txt'), 'utf8'),
    }};
  },
});`,
    );
    await writeFile(
      join(quickjs, "resident.ts"),
      `
      import {defineService} from 'muon-plugin-sdk/services';
      import type {} from 'muon-plugin-sdk/service-globals';
      import original from './src/service.ts';
      export default defineService({...original,start:async context=>{
        const running=await original.start(context); let count=0;
        let timer: ReturnType<typeof setTimeout>; const tick=()=>{console.log('consumer-work:'+JSON.stringify({count:++count,at:Date.now()}));timer=setTimeout(tick,100)};timer=setTimeout(tick,100);
        return {...running,stop:async reason=>{clearTimeout(timer);await running.stop(reason)}};
      }});`,
    );
    await mkdir(join(app, "src"));
    await writeFile(
      join(app, "index.html"),
      '<!doctype html><title>Packaged services</title><h1>Packaged services</h1><script type="module" src="/src/main.ts"></script>',
    );
    await writeFile(
      join(app, "src/main.ts"),
      `import type {} from 'muon-ui'; Object.assign(globalThis,{api:window.muon.services,browser:window.muon.browser}); document.querySelector('h1')!.textContent='Packaged services ready';`,
    );
    await writeFile(
      join(app, "vite.config.ts"),
      `import {defineConfig} from 'vite'; import muon from 'muon-ui/vite'; import prettierMax from 'prettier-max'; export default defineConfig({plugins:[prettierMax(),muon()],build:{target:'es2022'}});`,
    );
    await writeFile(
      join(app, "tsconfig.json"),
      JSON.stringify({
        compilerOptions: {
          target: "ES2022",
          module: "ESNext",
          moduleResolution: "Bundler",
          allowImportingTsExtensions: true,
          strict: true,
          noEmit: true,
          skipLibCheck: true,
        },
        include: ["src", "*.ts"],
      }),
    );
    const servicePorts: number[] = [];
    const reservations = Array.from({ length: 3 }, () => createServer());
    try {
      for (const server of reservations) {
        server.listen(0, "127.0.0.1");
        await once(server, "listening");
        const addr = server.address();
        if (!addr || typeof addr === "string")
          throw new Error("Missing service listener port");
        servicePorts.push(addr.port);
      }
    } finally {
      for (const server of reservations) {
        const closed = once(server, "close");
        server.close();
        await closed;
      }
    }
    const networkServices: Record<string, unknown> = {};
    for (const [index, name] of ["native", "quick", "node"].entries()) {
      for (const [suffix, example] of [
        ["Http", "http"],
        ["Listener", "server"],
      ] as const) {
        const source =
          name === "native"
            ? {
                runtime: "native",
                artifact: "artifacts/" + example + "/muon-plugin.json",
              }
            : {
                runtime: name === "quick" ? "quickjs" : "node",
                project: name === "quick" ? "quickjs" : "worker",
                entry: example + ".ts",
              };
        const port = servicePorts[index]!;
        networkServices[name + suffix] = {
          ...source,
          config: { port: String(port), bearerToken: "local-service" },
          io:
            suffix === "Http"
              ? { http: { origins: ["http://127.0.0.1:" + port] } }
              : { server: { listen: true } },
        };
      }
    }
    const listener = createServer();
    listener.listen(0, "127.0.0.1");
    await once(listener, "listening");
    const address = listener.address();
    if (!address || typeof address === "string")
      throw new Error("Missing CDP port");
    await new Promise<void>((resolve) => listener.close(() => resolve()));
    await writeFile(
      join(app, "muon.json"),
      JSON.stringify({
        services: {
          ...networkServices,
          nativeState: {
            runtime: "native",
            artifact: "artifacts/state/muon-plugin.json",
            io: {
              fs: { data: "read-write", assets: { initial: "initial.bin" } },
            },
          },
          quickState: {
            runtime: "quickjs",
            project: "quickjs",
            entry: "state.ts",
            io: {
              fs: { data: "read-write", assets: { initial: "initial.bin" } },
            },
          },
          nodeState: {
            runtime: "node",
            project: "worker",
            entry: "state.ts",
            io: {
              fs: { data: "read-write", assets: { initial: "initial.bin" } },
            },
          },
          native: {
            runtime: "native",
            artifact: "artifacts/echo/muon-plugin.json",
            autostart: true,
          },
          script: {
            runtime: "quickjs",
            project: "quickjs",
            entry: "resident.ts",
            autostart: true,
          },
          node: {
            runtime: "node",
            project: "worker",
            entry: "entry.ts",
            autostart: true,
          },
          extension: {
            runtime: "quickjs",
            entry: "quickjs-native.ts",
            autostart: true,
            plugins: {
              counter: {
                native: { manifest: "artifacts/counter/muon-plugin.json" },
                allow: ["muon.counter.increment"],
              },
            },
          },
        },
        plugin: {
          mode: "simple",
          plugins: [
            {
              name: "internal",
              allow: [
                "muon.services.*",
                "muon.browser.close",
                "muon.browser.shutdown",
              ],
            },
          ],
        },
        browser: { titleBarType: "native" },
        cdp: { enable: true, port: address.port },
      }),
    );
    const guards = join(root, "no-native-build");
    await runServiceConsumerDevelopment(
      app,
      root,
      target,
      address.port,
      servicePorts,
    );
    await mkdir(guards);
    for (const name of [
      "gcc",
      "g++",
      "cc",
      "c++",
      "clang",
      "clang++",
      "cmake",
      "make",
      "ninja",
      "pkg-config",
      "x86_64-w64-mingw32-gcc",
      "x86_64-w64-mingw32-g++",
      "i686-w64-mingw32-gcc",
      "i686-w64-mingw32-g++",
    ]) {
      const path = join(guards, name);
      await writeFile(
        path,
        "#!/bin/sh\necho Native compilation is forbidden >&2\nexit 99\n",
      );
      await chmod(path, 0o755);
    }
    const environment = {
      ...process.env,
      PATH: guards + ":" + process.env.PATH,
    };
    await run(
      process.execPath,
      [join(app, "node_modules/typescript/bin/tsc"), "--noEmit"],
      environment,
    );
    const built = JSON.parse(
      await run(
        process.execPath,
        [
          join(app, "node_modules/muon-ui/dist/cli.cjs"),
          "build",
          "--target",
          target,
          "--json",
        ],
        environment,
      ),
    ).targets[0];
    // Move the complete distribution and remove every source, package and SDK.
    const distribution = join(root, "distribution with spaces 日本");
    await cp(dirname(built.launcherPath), distribution, { recursive: true });
    const launcher = join(distribution, built.launcherPath.split("/").at(-1));
    await rm(app, { recursive: true });
    if (target.startsWith("windows-")) {
      await execute(process.execPath, [resolve("build-windows-cdp-relay.mjs")]);
      await runWindowsConsumer(
        distribution,
        basename(launcher),
        address.port,
        servicePorts,
        root,
      );
      return;
    }
    let child: ChildProcess | undefined;
    let exited: Promise<unknown[]> | undefined;
    let client: Client | undefined;
    let closeMcp: (() => Promise<void>) | undefined;
    let diagnostic = "";
    let failed = false;
    const work = new EventEmitter();
    const disconnect = async () => {
      try {
        await cleanupResources([
          async () => await client?.close(),
          async () => await closeMcp?.(),
        ]);
      } finally {
        client = undefined;
        closeMcp = undefined;
      }
    };
    const launch = async () => {
      child = spawn(launcher, ["--ozone-platform=x11", "--disable-gpu"], {
        cwd: distribution,
        stdio: ["ignore", "pipe", "pipe"],
        env: {
          ...process.env,
          XDG_CONFIG_HOME: join(root, "xdg"),
          XDG_STATE_HOME: join(root, "state"),
        },
      });
      exited = once(child, "exit", { signal: AbortSignal.timeout(120000) });
      child.stdout!.on("data", (chunk) => {
        diagnostic += String(chunk);
      });
      let lines = "";
      child.stderr!.on("data", (chunk) => {
        diagnostic += String(chunk);
        lines += String(chunk);
        while (lines.includes("\n")) {
          const end = lines.indexOf("\n");
          const line = lines.slice(0, end);
          lines = lines.slice(end + 1);
          const observed = /consumer-work:(\{.*\})/.exec(line);
          if (observed) work.emit("progress", JSON.parse(observed[1]!));
        }
      });
      const endpoint = `http://127.0.0.1:${address.port}`;
      await vi.waitUntil(
        async () => {
          if (child!.exitCode !== null) throw new Error(diagnostic);
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
        outputDir: join(root, "mcp"),
      });
      closeMcp = async () => {
        await mcp.close();
      };
      const [a, b] = InMemoryTransport.createLinkedPair();
      client = new Client({ name: "service-consumer", version: "1" });
      await mcp.connect(b);
      await client.connect(a);
      await client.callTool({
        name: "browser_tabs",
        arguments: { action: "list" },
      });
      const result = await client.callTool({
        name: "browser_wait_for",
        arguments: { text: "Packaged services ready" },
      });
      if (result.isError) throw new Error(JSON.stringify(result));
    };
    const evaluate = async (source: string) => {
      const result = await client!.callTool({
        name: "browser_evaluate",
        arguments: { function: `async()=>{${source}}` },
      });
      const text = (result.content as { type: string; text?: string }[])
        .map((item) => item.text ?? "")
        .join("\n");
      if (result.isError) throw new Error(text + "\n" + diagnostic);
      return JSON.parse(/### Result\n([\s\S]*?)(?:\n### |$)/u.exec(text)![1]!);
    };
    try {
      await launch();
      const first = await evaluate(
        `globalThis.native=await api.acquire('native');globalThis.script=await api.acquire('script');globalThis.extension=await api.acquire('extension');return {native:native.generation,script:script.generation,extension:extension.generation,base64:await script.call('base64',[new Uint8Array([255,0])]),text:await native.call('echo',['日本\\0🙂']),next:await extension.call('next',[41])};`,
      );
      expect(first.text).toBe("日本\0🙂");
      expect(first.next).toBe(42);
      expect(first.base64).toBe("/wA=");
      expect(await evaluate(exerciseConsumerNetwork(servicePorts))).toBe(true);
      expect(await evaluate(savedServiceValues([5, 128, 1]))).toEqual(
        Array(3).fill([0, 255]),
      );
      const firstNode = await evaluate(
        `globalThis.nodeService=await api.acquire('node');return {generation:nodeService.generation,identity:await nodeService.call('identity',[]),greeting:await nodeService.call('greet',['packaged'])};`,
      );
      expect(firstNode.greeting).toBe("Hello packaged");
      expect(firstNode.identity).toMatch(/^\d+:x64:1\.2\.3$/u);
      expect(
        await evaluate(
          `let done;const event=new Promise(resolve=>done=resolve);const subscription=await script.subscribe('progress',done);await script.call('notify',['packaged event']);const value=await event;await subscription.close();return value;`,
        ),
      ).toBe("packaged event");
      await Promise.allSettled([
        evaluate("await browser.close();return true;"),
        exited!,
      ]);
      expect(child!.exitCode, diagnostic).toBe(0);
      await disconnect();
      const closedAt = Date.now();
      let observation: { at: number; count: number };
      do {
        [observation] = (await once(work, "progress", {
          signal: AbortSignal.timeout(10000),
        })) as [{ at: number; count: number }];
      } while (observation.at <= closedAt);
      expect(observation.count).toBeGreaterThan(0);
      await launch();
      const reopened = await evaluate(
        `globalThis.native=await api.acquire('native');globalThis.script=await api.acquire('script');globalThis.extension=await api.acquire('extension');await api.setAutostart('extension',false);return {native:native.generation,script:script.generation,extension:extension.generation,next:await extension.call('next',[99]),autostart:(await api.getState('extension')).autostart};`,
      );
      expect(await evaluate(savedServiceValues([0, 17, 240]))).toEqual(
        Array(3).fill([5, 128, 1]),
      );
      expect(reopened).toEqual({
        native: first.native,
        script: first.script,
        extension: first.extension,
        next: 100,
        autostart: false,
      });
      const reopenedNode = await evaluate(
        `globalThis.nodeService=await api.acquire('node');return {generation:nodeService.generation,identity:await nodeService.call('identity',[]),greeting:await nodeService.call('greet',['packaged again'])};`,
      );
      expect(reopenedNode).toEqual({
        ...firstNode,
        greeting: "Hello packaged again",
      });
      await evaluate(`await extension.release();return true;`);
      await Promise.allSettled([
        evaluate("await browser.shutdown(0);return true;"),
        exited!,
      ]);
      expect(child!.exitCode, diagnostic).toBe(0);
      await disconnect();
      await launch();
      expect(
        await evaluate(`return (await api.getState('extension')).autostart;`),
      ).toBe(false);
      expect(
        await evaluate(
          `const handle=await api.acquire('node');try{return await handle.call('saved',[])}finally{await handle.release()}`,
        ),
      ).toBe("packaged again");
      expect(await evaluate(savedServiceValues(undefined))).toEqual(
        Array(3).fill([0, 17, 240]),
      );
      await evaluate(
        "await api.setAutostart('native',false);await api.setAutostart('script',false);await api.setAutostart('node',false);return true;",
      );
      await Promise.allSettled([
        evaluate("await browser.shutdown(0);return true;"),
        exited!,
      ]);
      expect(child!.exitCode, diagnostic).toBe(0);
    } catch (error) {
      failed = true;
      throw error;
    } finally {
      if (client && child?.exitCode === null) {
        try {
          await evaluate(
            "for(const id of ['native','script','extension','node'])await api.setAutostart(id,false);await browser.shutdown(0);return true;",
          );
        } catch {}
      }
      try {
        await cleanupResources([
          disconnect,
          async () => {
            if (child && child.exitCode === null && child.signalCode === null) {
              child.kill();
              await exited;
            }
            // A daemon can outlive an already closed UI when an assertion
            // fails. Match its explicit private data root before terminating it.
            for (const pid of await readdir("/proc")) {
              if (!/^\d+$/u.test(pid)) continue;
              let args: string[];
              try {
                args = (await readFile(`/proc/${pid}/cmdline`, "utf8")).split(
                  "\0",
                );
              } catch {
                continue;
              }
              const index = args.indexOf("--data-root");
              if (
                index < 0 ||
                !args[index + 1]?.startsWith(join(root, "state") + "/")
              )
                continue;
              try {
                process.kill(Number(pid), "SIGKILL");
              } catch (error) {
                if ((error as NodeJS.ErrnoException).code !== "ESRCH")
                  throw error;
              }
              await vi.waitUntil(
                async () => {
                  try {
                    return !(await readFile(`/proc/${pid}/cmdline`, "utf8"));
                  } catch (error) {
                    if ((error as NodeJS.ErrnoException).code === "ENOENT")
                      return true;
                    throw error;
                  }
                },
                { timeout: 15000, interval: 100 },
              );
            }
          },
        ]);
      } catch (error) {
        if (!failed) throw error;
        console.error(error);
      } finally {
        await writeFile(join(root, "diagnostics.log"), diagnostic);
        // Preserve this independent distribution as an inspectable release artifact.
        console.log("Service consumer distribution: " + distribution);
      }
    }
  },
  900000,
);
