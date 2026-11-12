// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile, spawn } from "node:child_process";
import { randomUUID } from "node:crypto";
import { EventEmitter, on, once } from "node:events";
import {
  copyFile,
  mkdir,
  mkdtemp,
  readFile,
  symlink,
  writeFile,
} from "node:fs/promises";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { createInterface } from "node:readline";
import { promisify } from "node:util";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { InMemoryTransport } from "@modelcontextprotocol/sdk/inMemory.js";
import { createConnection } from "@playwright/mcp";
import { expect, it } from "vitest";
import { buildMuonApp } from "../common/build.ts";
import { buildPlugin, createPluginProject } from "muon-plugin-sdk";

const execute = promisify(execFile);

it.skipIf(process.env.MUON_ANDROID_QUICKJS_E2E !== "1").each([
  { native: false, label: "QuickJS" },
  { native: true, label: "QuickJS native modules" },
])(
  "$label builds a TS service APK and observes work after its UI process dies",
  async ({ native }) => {
    const serial = process.env.ANDROID_SERIAL;
    if (!serial)
      throw new Error("ANDROID_SERIAL must identify the test device.");
    const root = await mkdtemp(join(tmpdir(), "muon-quickjs-apk-"));
    const nonce = randomUUID();
    const app = native ? "dev.muon.e2e.quickjsplugins" : "dev.muon.e2e.quickjs";
    const adb = async (...args: string[]) =>
      (await execute("adb", ["-s", serial, ...args])).stdout;
    await mkdir(join(root, "web"));
    await symlink(resolve("../node_modules"), join(root, "node_modules"));
    await writeFile(
      join(root, "package.json"),
      JSON.stringify({
        name: "quickjs-service-driver",
        version: "1.0.0",
        type: "module",
      }),
    );
    await writeFile(
      join(root, "web/index.html"),
      "<!doctype html><title>QuickJS service</title><h1>QuickJS service</h1>",
    );
    await writeFile(
      join(root, "service.ts"),
      `
import {defineService,defineServiceInterface} from 'muon-plugin-sdk/services';
import type {} from 'muon-plugin-sdk/service-globals';
export default defineService({
  interface: defineServiceInterface({schemaVersion:1, methods:{
    echo:{parameters:['string'],returns:'string',errors:[]},
    echoBytes:{parameters:['bytes'],returns:'bytes',errors:[]},
    info:{parameters:[],returns:'string',errors:[]}
  },events:{}}),
  start: context => {
    let count=0;
    let timer: ReturnType<typeof setTimeout>;
    const tick=()=>{
      console.log('work:'+context.config.nonce+':'+JSON.stringify({count:++count,at:Date.now()}));
      timer=setTimeout(tick,100);
    };
    timer=setTimeout(tick,100);
    return {methods:{
      echo:(_call,value)=>value,
      echoBytes:(_call,value)=>value,
      info:()=>JSON.stringify({id:context.id,generation:context.generation,label:context.config.label,count})
    },stop:async()=>{clearTimeout(timer);await new Promise<void>(done=>setTimeout(done,1));}};
  }
});`,
    );
    let plugins: Record<string, unknown> | undefined;
    if (native) {
      const directory = join(root, "native");
      await createPluginProject({ directory, name: "fixture", language: "c" });
      const configPath = join(directory, "muon-plugin.build.json");
      const build = JSON.parse(await readFile(configPath, "utf8"));
      await copyFile(
        resolve("../core/common-test/muon_service_native_plugin_fixture.c"),
        join(directory, build.sources[0]),
      );
      const allow = [
        "echo",
        "echoBytes",
        "echoInt64",
        "echoUint64",
        "callback",
        "fail",
        "makeEcho",
        "wrapped",
      ].map((name) => `fixture.api.${name}`);
      build.functions = allow;
      await writeFile(configPath, JSON.stringify(build));
      let manifest = "";
      for (const target of ["android-arm64-v8a", "android-x86_64"] as const)
        manifest = await buildPlugin({ configPath, target });
      plugins = { fixture: { native: { manifest }, allow } };
      await copyFile(
        resolve("../core/common-test/services/quickjs-native-service.ts"),
        join(root, "native-service.ts"),
      );
      await writeFile(
        join(root, "service.ts"),
        `
import {defineService,defineServiceInterface} from 'muon-plugin-sdk/services';
import type {} from 'muon-plugin-sdk/service-globals';
import definition from './native-service.ts';
import {echo} from 'fixture:api';
export default defineService({
  interface: defineServiceInterface({...definition.interface,methods:{...definition.interface.methods,
    info:{parameters:[],returns:'string',errors:[]}}}),
  start: async context => {
    const original=await definition.start(context);
    let count=0,active=true;
    let timer: ReturnType<typeof setTimeout>;
    let pending=Promise.resolve();
    const tick=()=>{pending=(async()=>{
      const work=await echo(JSON.stringify({count:++count,at:Date.now()}));
      console.log('work:'+context.config.nonce+':'+work);
      if(active)timer=setTimeout(tick,100);
    })()};
    timer=setTimeout(tick,100);
    return {methods:{...original.methods,
      info:()=>JSON.stringify({id:context.id,generation:context.generation,label:context.config.label,count})
    },stop:async()=>{active=false;clearTimeout(timer);await pending;await original.stop();}};
  }
});`,
      );
    }
    await writeFile(
      join(root, "muon.json"),
      JSON.stringify({
        browser: { startPage: `asset://main/index.html?run=${nonce}` },
        android: {
          applicationId: app,
          label: "QuickJS service",
          abis: ["arm64-v8a", "x86_64"],
        },
        plugin: {
          mode: "simple",
          plugins: [{ name: "internal", allow: ["muon.services.*"] }],
        },
        services: {
          ...(native
            ? {
                "borrowed.v1": {
                  runtime: "quickjs",
                  entry: "service.ts",
                  plugins,
                  config: { label: "Borrowed", nonce: `${nonce}-borrowed` },
                  android: {
                    foregroundServiceType: "specialUse",
                    specialUseSubtype: "Borrowed service test",
                    notification: {
                      channelId: "borrowed",
                      title: "Borrowed worker",
                      text: "Processing",
                    },
                  },
                },
              }
            : {}),
          "worker.v1": {
            runtime: "quickjs",
            entry: "service.ts",
            ...(plugins ? { plugins } : {}),
            autostart: true,
            config: { label: "日本語😀", nonce },
            android: {
              foregroundServiceType: "specialUse",
              specialUseSubtype: "Background service test",
              notification: {
                channelId: "worker",
                title: "Background worker",
                text: "Processing",
              },
            },
          },
        },
      }),
    );
    const built = await buildMuonApp({
      root,
      targets: ["android"],
      assetSourcePath: "web",
      packageDirectory: resolve("dist"),
    });
    const apk = built.targets.find((target) => target.target === "android");
    if (!apk || apk.target !== "android")
      throw new Error("Android build result missing.");
    await writeFile(join(root, "build.json"), JSON.stringify(built, null, 2));
    const events = new EventEmitter();
    const logcat = spawn("adb", [
      "-s",
      serial,
      "logcat",
      "-v",
      "brief",
      "-T",
      "1",
      "MuonActivity:I",
      "MuonNativeService:I",
      "*:S",
    ]);
    const lines = createInterface({ input: logcat.stdout });
    lines.on("line", (line) => events.emit("line", line));
    const readUntil = async (
      accept: (line: string) => boolean,
      signal: AbortSignal,
    ) => {
      for await (const [line] of on(events, "line", { signal }))
        if (accept(String(line))) return String(line);
      throw new Error("Log stream ended before the requested service event.");
    };
    let client: Client | undefined;
    let closeServer: (() => Promise<void>) | undefined;
    let port: string | undefined;
    const closePage = async () => {
      await client?.close();
      client = undefined;
      await closeServer?.();
      closeServer = undefined;
      if (port !== undefined) {
        await adb("forward", "--remove", `tcp:${port}`);
        port = undefined;
      }
    };
    const openPage = async () => {
      const abort = new AbortController();
      const ready = readUntil(
        (line) => line.includes(`Muon page ready:`) && line.includes(nonce),
        AbortSignal.any([abort.signal, AbortSignal.timeout(30000)]),
      );
      try {
        await Promise.all([
          ready,
          adb(
            "shell",
            "am",
            "start",
            "-W",
            "-n",
            `${app}/dev.muon.runtime.MuonAppActivity`,
          ),
        ]);
      } finally {
        abort.abort();
        await Promise.allSettled([ready]);
      }
      const pid = (await adb("shell", "pidof", app)).trim();
      expect(pid).toMatch(/^\d+$/u);
      port = (
        await adb(
          "forward",
          "tcp:0",
          `localabstract:webview_devtools_remote_${pid}`,
        )
      ).trim();
      const server = await createConnection({
        browser: { cdpEndpoint: `http://127.0.0.1:${port}` },
        outputDir: root,
      });
      closeServer = async () => await server.close();
      client = new Client({ name: "quickjs-generated-apk", version: "1" });
      const [a, b] = InMemoryTransport.createLinkedPair();
      await server.connect(b);
      await client.connect(a);
      return pid;
    };
    const evaluate = async <T>(source: string): Promise<T> => {
      const result = await client!.callTool({
        name: "browser_evaluate",
        arguments: { function: `async()=>{${source}}` },
      });
      const text = (result.content as { type: string; text?: string }[])
        .filter((x) => x.type === "text")
        .map((x) => x.text ?? "")
        .join("\n");
      if (result.isError) throw new Error(text);
      const json = /### Result\n([\s\S]*?)(?:\n### |$)/u.exec(text)?.[1];
      if (!json) throw new Error(text);
      return JSON.parse(json) as T;
    };
    try {
      const installed = await adb("shell", "pm", "list", "packages", app);
      if (installed.split(/\r?\n/u).includes(`package:${app}`))
        await adb("uninstall", app);
      await adb("install", apk.packagePath);
      await adb(
        "shell",
        "pm",
        "grant",
        app,
        "android.permission.POST_NOTIFICATIONS",
      );
      const pid = await openPage();
      expect(
        await evaluate(
          `globalThis.service=await muon.services.acquire('worker.v1');return await service.call('echo',[${JSON.stringify(native ? "APK日本語😀" : "APK\0日本語😀")}]);`,
        ),
      ).toBe(native ? "APK日本語😀" : "APK\0日本語😀");
      if (native) {
        expect(
          await evaluate(
            `return String(await service.call('echoInt64',[-9223372036854775808n]));`,
          ),
        ).toBe("-9223372036854775808");
        expect(
          await evaluate(
            `return String(await service.call('echoUint64',[18446744073709551615n]));`,
          ),
        ).toBe("18446744073709551615");
        expect(
          await evaluate(
            `return await service.call('callback',['nested',async value=>await service.call('echo',[value+'😀'])]);`,
          ),
        ).toBe("nested😀");
        expect(
          await evaluate(`return await service.call('proxy',['proxy']);`),
        ).toBe("proxy");
        expect(
          await evaluate(`return await service.call('failure',[]);`),
        ).toContain("fixture native failure");
        expect(
          await evaluate(
            `try{await service.call('echo',['a\\0b']);return 'unexpected success'}catch(error){return error.code}`,
          ),
        ).toBe("invalid-result");
        expect(
          await evaluate(`let done;const received=new Promise(resolve=>done=resolve);
          const subscription=await service.subscribe('progress',value=>done(value));
          await service.call('emit',['event😀']);const value=await received;await subscription.close();return value;`),
        ).toBe("event😀");
      }
      expect(
        await evaluate(`const bytes=new Uint8Array(1024*1024);bytes[0]=37;bytes[bytes.length-1]=91;
        const result=await service.call('echoBytes',[bytes]);return [result.length,result[0],result[result.length-1]];`),
      ).toEqual([1048576, 37, 91]);
      if (native) {
        expect(
          await evaluate(`
          const other=await muon.services.acquire('borrowed.v1');
          let entered;const ready=new Promise(resolve=>entered=resolve);
          const running=(async()=>{
            try{await other.call('callback',['pending',async()=>{
              entered();return await new Promise(()=>{});
            }]);return false}catch{return true}
          })();
          await ready;await other.release();const cancelled=await running;
          let stopped;const stop=new Promise(resolve=>stopped=resolve);
          const watch=await muon.services.watchState('borrowed.v1',state=>{
            if(state.state==='stopped')stopped();
          });await stop;await watch.close();
          const restarted=await muon.services.acquire('borrowed.v1');
          const result=[cancelled,restarted.generation!==other.generation,
            await restarted.call('echo',['new generation']),await service.call('echo',['resident continues'])];
          await restarted.release();return result;
        `),
        ).toEqual([true, true, "new generation", "resident continues"]);
      }
      const before = JSON.parse(
        await evaluate<string>(`return await service.call('info',[]);`),
      ) as { generation: string; id: string; label: string };
      expect(before).toMatchObject({ id: "worker.v1", label: "日本語😀" });
      await closePage();
      // Killing the UI PID is deliberately different from force-stop, which also
      // terminates the service process. Logcat observes work without a lease.
      await adb("shell", "run-as", app, "kill", "-9", pid);
      const diedAt = Number((await adb("shell", "date", "+%s%3N")).trim());
      const work = await readUntil((line) => {
        const value = line.split(`work:${nonce}:`)[1];
        return value !== undefined && Number(JSON.parse(value).at) > diedAt;
      }, AbortSignal.timeout(30000));
      expect(work).toContain(`work:${nonce}:`);
      expect(
        (await adb("shell", "pidof", `${app}:muon_services`)).trim(),
      ).toMatch(/^\d+$/u);
      await openPage();
      const after = await evaluate<{
        generation: string;
        clientReferences: number;
      }>(
        `globalThis.service=await muon.services.acquire('worker.v1');return await muon.services.getState('worker.v1');`,
      );
      expect(after.generation).toBe(before.generation);
      expect(after.clientReferences).toBe(1);
      expect(
        await evaluate(`await muon.services.setAutostart('worker.v1',false);await service.release();
        let done;const stopped=new Promise(resolve=>done=resolve);
        const watch=await muon.services.watchState('worker.v1',state=>{if(state.state==='stopped')done();});
        await stopped;await watch.close();return (await muon.services.getState('worker.v1')).state;`),
      ).toBe("stopped");
      if (native) {
        expect(
          await evaluate(`
          const restarted=await muon.services.acquire('worker.v1');
          const result=[restarted.generation!==${JSON.stringify(before.generation)},
            await restarted.call('echo',['runtime reloaded'])];
          await restarted.release();
          let done;const stopped=new Promise(resolve=>done=resolve);
          const watch=await muon.services.watchState('worker.v1',state=>{if(state.state==='stopped')done();});
          await stopped;await watch.close();return result;
        `),
        ).toEqual([true, "runtime reloaded"]);
      }
    } finally {
      await closePage();
      await adb("shell", "am", "force-stop", app);
      lines.close();
      if (logcat.exitCode === null) {
        const exited = once(logcat, "exit");
        logcat.kill();
        await exited;
      }
    }
  },
  240000,
);
