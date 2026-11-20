// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile, spawn } from "node:child_process";
import { randomUUID } from "node:crypto";
import { EventEmitter, on } from "node:events";
import {
  chmod,
  copyFile,
  cp,
  mkdir,
  mkdtemp,
  readFile,
  rm,
  writeFile,
} from "node:fs/promises";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { createInterface } from "node:readline";
import { promisify } from "node:util";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";
import { expect, it } from "vitest";

const closeConsumerResources = async (
  operations: readonly (() => Promise<void>)[],
): Promise<void> => {
  const results = await Promise.allSettled(
    operations.map(async (operation) => await operation()),
  );
  const failures = results
    .filter((result) => result.status === "rejected")
    .map((result) => result.reason);
  if (failures.length)
    throw new AggregateError(failures, "Android consumer cleanup failed");
};

it("drains every consumer resource when one cleanup fails", async () => {
  const closed: string[] = [];
  let finish!: () => void;
  const pending = new Promise<void>((resolve) => (finish = resolve));
  let settled = false;
  const failure = new Error("MCP connection already disconnected");
  const completed = (async () => {
    try {
      await closeConsumerResources([
        async () => {
          closed.push("mcp");
          throw failure;
        },
        async () => {
          closed.push("observer");
          await pending;
        },
        async () => {
          closed.push("application");
        },
      ]);
    } catch (error) {
      settled = true;
      return error;
    }
  })();
  expect(closed).toEqual(["mcp", "observer", "application"]);
  expect(settled).toBe(false);
  finish();
  const error = await completed;
  expect(error).toBeInstanceOf(AggregateError);
  expect((error as AggregateError).errors).toEqual([failure]);
});

it.skipIf(
  !process.env.MUON_SERVICE_CONSUMER_UI ||
    !process.env.MUON_SERVICE_CONSUMER_SDK ||
    process.env.MUON_SERVICE_CONSUMER_ANDROID !== "1",
)(
  "runs packaged native and QuickJS services from signed APK and AAB distributions",
  async () => {
    const serial = process.env.ANDROID_SERIAL;
    const androidSdk = process.env.ANDROID_HOME;
    const java21 = process.env.MUON_JAVA21_HOME;
    if (!serial || !androidSdk || !java21)
      throw new Error(
        "ANDROID_SERIAL, ANDROID_HOME and MUON_JAVA21_HOME are required.",
      );
    const root = await mkdtemp(
      join(tmpdir(), "muon-service-android-consumer-"),
    );
    const app = join(root, "app");
    await mkdir(app);
    const appId = "dev.muon.e2e.serviceconsumer";
    const nonce = randomUUID();
    const environment = {
      ...process.env,
      MUON_CONSUMER_PASSWORD: randomUUID(),
    };
    const execute = promisify(execFile);
    const run = async (
      command: string,
      args: string[],
      cwd = app,
      env: NodeJS.ProcessEnv = environment,
    ): Promise<string> => {
      try {
        return (
          await execute(command, args, {
            cwd,
            env,
            maxBuffer: 64 * 1024 * 1024,
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
        name: "android-service-package-consumer",
        version: "1.0.0",
        type: "module",
        private: true,
        workspaces: ["quickjs"],
        scripts: { dependencies: "reskill" },
      }),
    );
    const versions = await Promise.all(
      ["vite", "typescript", "prettier-max", "resolved-killer"].map(
        async (name) =>
          name +
          "@" +
          JSON.parse(
            await readFile(
              resolve("../node_modules", name, "package.json"),
              "utf8",
            ),
          ).version,
      ),
    );
    await run("npm", [
      "install",
      "--ignore-scripts",
      "--no-audit",
      "--no-fund",
      resolve(process.env.MUON_SERVICE_CONSUMER_UI!),
      resolve(process.env.MUON_SERVICE_CONSUMER_SDK!),
      ...versions,
    ]);
    const sdk = join(app, "node_modules/muon-plugin-sdk");
    const sdkCli = join(sdk, "dist/cli.mjs");
    const serviceExamples = ["echo", "state", "http", "server"];
    for (const name of serviceExamples) {
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
    await run(process.execPath, [
      sdkCli,
      "init",
      "producer/counter",
      "--name",
      "counter",
      "--language",
      "c",
    ]);
    for (const name of [...serviceExamples, "counter"])
      for (const target of ["android-arm64-v8a", "android-x86_64"])
        await run(process.execPath, [
          sdkCli,
          "build",
          `producer/${name}/muon-plugin.build.json`,
          "--target",
          target,
          "--out",
          `artifacts/${name}`,
        ]);
    await rm(join(app, "producer"), { recursive: true });
    for (const name of [...serviceExamples, "counter"])
      await rm(join(app, "artifacts", name, ".build"), {
        recursive: true,
        force: true,
      });
    const project = join(app, "quickjs");
    await cp(join(sdk, "examples/services/quickjs-project"), project, {
      recursive: true,
    });
    await run("npm", [
      "install",
      "--ignore-scripts",
      "--no-audit",
      "--no-fund",
    ]);
    for (const name of ["state.ts", "http.ts", "server.ts"])
      await copyFile(join(sdk, "examples/services", name), join(project, name));
    for (const directory of [project, app])
      await writeFile(join(directory, "initial.bin"), new Uint8Array([0, 255]));
    for (const name of ["quickjs-native.ts", "counter.d.ts"])
      await copyFile(join(sdk, "examples/services", name), join(app, name));
    await writeFile(
      join(project, "resident.ts"),
      `
      import {defineService,defineServiceInterface} from 'muon-plugin-sdk/services';
      import type {} from 'muon-plugin-sdk/service-globals';
      import original from './src/service.ts';
      export default defineService({interface:defineServiceInterface({...original.interface,methods:{...original.interface.methods,
        report:{parameters:['string'],returns:'void',errors:[]}}}),start:async context=>{
        const running=await original.start(context);let count=0;
        let timer:ReturnType<typeof setTimeout>;
        const tick=()=>{console.log('consumer-work:'+context.config.nonce+':'+JSON.stringify({count:++count,at:Date.now()}));timer=setTimeout(tick,100)};
        timer=setTimeout(tick,100);
        return {methods:{...running.methods,report:(_call,value)=>console.log('consumer-result:'+context.config.nonce+':'+value)},
          stop:async reason=>{clearTimeout(timer);await running.stop(reason)}};
      }});`,
    );
    const ports = [49231, 49232];
    await mkdir(join(app, "src"));
    await writeFile(
      join(app, "index.html"),
      '<!doctype html><title>Packaged Android services</title><h1>Running</h1><script type="module" src="/src/main.ts"></script>',
    );
    await writeFile(
      join(app, "src/main.ts"),
      `
      import type {} from 'muon-ui';
      const available=window.muon.services;
      if(!available)throw new Error('Service capability is unavailable');
      const api=available;
      Object.assign(globalThis,{consumerResult:(async()=>{
        const sessionFile='service-consumer-session';
        const session=await window.muon.fs.exists(sessionFile)?Number(await window.muon.fs.readTextFile(sessionFile,'utf8')):0;
        await window.muon.fs.writeTextFile(sessionFile,String(session+1),'utf8');
        const managed=['native','extension'];
        const waitState=async(id:string,state:MuonServiceState['state'])=>{
          let done!:(value:MuonServiceState)=>void;const complete=new Promise<MuonServiceState>(resolve=>done=resolve);
          const watch=await api.watchState(id,value=>{if(value.state===state)done(value)});
          try{return await complete}finally{await watch.close()}
        };
        const initial=await Promise.all(managed.map(async id=>await api.getState(id)));
        let enabled:MuonServiceState[]|undefined;
        if(session===3){
          for(const id of managed){await api.setAutostart(id,true);await api.setAutostart(id,true)}
          enabled=await Promise.all(managed.map(async id=>await waitState(id,'running')));
        }
        const script=await api.acquire('script');let native:Awaited<ReturnType<typeof api.acquire>>|undefined;
        let extension:Awaited<ReturnType<typeof api.acquire>>|undefined;let result:Record<string,unknown>;
        try{native=await api.acquire('native');extension=await api.acquire('extension');
          let received!:(value:unknown)=>void;const event=new Promise(resolve=>received=resolve);
          const subscription=await script.subscribe('progress',value=>received(value));
          await script.call('notify',['event😀']);const notification=await event;await subscription.close();
          if(session===2)for(const id of managed){await api.setAutostart(id,false);await api.setAutostart(id,false)}
          const io=[];
          for(const [index,name] of ['native','quick'].entries()){
            const state=await api.acquire(name+'State');
            const previous=Array.from(await state.call('get',[]) as Uint8Array);
            await state.call('set',[new Uint8Array([0,128,session])]);await state.release();
            await waitState(name+'State','stopped');
            const listener=await api.acquire(name+'Listener');const client=await api.acquire(name+'Http');
            try{
              const url='http://127.0.0.1:'+${JSON.stringify(ports)}[index]+'/echo';
              const headers={authorization:'Bearer local-service'};
              const bytes=await client.call('bytes',[JSON.stringify({url,headers,method:'POST'}),new Uint8Array([0,128,255])]) as Uint8Array;
              const reply=JSON.parse(await client.call('describe',[JSON.stringify({url,headers,responseType:'text'}),new Uint8Array()]) as string);
              io.push({previous,bytes:Array.from(bytes),status:reply.status,owned:reply.text===listener.generation && reply.headers['x-service-generation']===listener.generation});
            }finally{await client.release();await listener.release()}
          }
          result={session,initial,enabled,io,encoded:await script.call('base64',[new Uint8Array([0,128,255])]),
            native:await native.call('echo',['native\\0日本語😀']),script:await script.call('echo',['script\\0日本語😀']),
            extension:await extension.call('next',[41]),event:notification,
            generations:[native.generation,script.generation,extension.generation],
            references:[(await api.getState('native')).clientReferences,(await api.getState('script')).clientReferences,(await api.getState('extension')).clientReferences]};
          if(session===2){
            result.off=await Promise.all(managed.map(async id=>await api.getState(id)));
            await native.release();await extension.release();
            result.stopped=await Promise.all(managed.map(async id=>await waitState(id,'stopped')));
          }
        }catch(error){result={error:String(error)}}
        await script.call('report',[JSON.stringify(result)]);await native?.release();await extension?.release();await script.release();
        document.querySelector('h1')!.textContent=JSON.stringify(result);
        if(new URL(location.href).searchParams.get('close')==='1')await window.muon.browser.close();
        return result;
      })()});`,
    );
    await writeFile(
      join(app, "vite.config.ts"),
      `import {defineConfig} from 'vite';import muon from 'muon-ui/vite';import prettierMax from 'prettier-max';export default defineConfig({plugins:[prettierMax(),muon({open:false})],build:{target:'es2022'}});`,
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
    const keystore = join(root, "signing.p12");
    await run(join(java21, "bin/keytool"), [
      "-genkeypair",
      "-keystore",
      keystore,
      "-storetype",
      "PKCS12",
      "-alias",
      "consumer",
      "-storepass:env",
      "MUON_CONSUMER_PASSWORD",
      "-keypass:env",
      "MUON_CONSUMER_PASSWORD",
      "-keyalg",
      "RSA",
      "-keysize",
      "2048",
      "-validity",
      "3650",
      "-dname",
      "CN=Muon Service Consumer Test",
      "-noprompt",
    ]);
    const android = {
      foregroundServiceType: "specialUse",
      specialUseSubtype: "Packaged service verification",
      notification: {
        channelId: "services",
        title: "Packaged services",
        text: "Processing",
      },
    };
    const services: Record<string, unknown> = {};
    for (const [index, name] of ["native", "quick"].entries()) {
      for (const [suffix, sample] of [
        ["State", "state"],
        ["Http", "http"],
        ["Listener", "server"],
      ] as const) {
        const source =
          name === "native"
            ? {
                runtime: "native",
                artifact: "artifacts/" + sample + "/muon-plugin.json",
              }
            : { runtime: "quickjs", project: "quickjs", entry: sample + ".ts" };
        const port = ports[index]!;
        const io =
          sample === "state"
            ? { fs: { data: "read-write", assets: { initial: "initial.bin" } } }
            : sample === "http"
              ? { http: { origins: ["http://127.0.0.1:" + port] } }
              : { server: { listen: true } };
        services[name + suffix] = {
          ...source,
          android,
          io,
          config: { port: String(port), bearerToken: "local-service" },
        };
      }
    }
    const configuration = {
      browser: {
        startPage: `asset://main/index.html?close=0&consumer=${nonce}`,
      },
      android: {
        applicationId: appId,
        label: "Packaged services",
        abis: ["arm64-v8a", "x86_64"],
        signing: {
          keystore,
          keyAlias: "consumer",
          storePasswordEnv: "MUON_CONSUMER_PASSWORD",
        },
      },
      services: {
        ...services,
        native: {
          runtime: "native",
          artifact: "artifacts/echo/muon-plugin.json",
          autostart: true,
          android,
        },
        script: {
          runtime: "quickjs",
          project: "quickjs",
          entry: "resident.ts",
          autostart: true,
          config: { nonce },
          android,
        },
        extension: {
          runtime: "quickjs",
          entry: "quickjs-native.ts",
          autostart: true,
          android,
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
              "muon.fs.exists",
              "muon.fs.readTextFile",
              "muon.fs.writeTextFile",
            ],
          },
        ],
      },
    };
    await writeFile(join(app, "muon.json"), JSON.stringify(configuration));
    const guards = join(root, "no-native-build");
    await mkdir(guards);
    for (const name of [
      "gcc",
      "g++",
      "cc",
      "c++",
      "clang",
      "clang++",
      "make",
      "cmake",
      "ninja",
      "pkg-config",
    ]) {
      await writeFile(
        join(guards, name),
        "#!/bin/sh\necho Native compilation is forbidden >&2\nexit 99\n",
      );
      await chmod(join(guards, name), 0o755);
    }
    const packagedEnvironment = {
      ...environment,
      PATH: guards + ":" + process.env.PATH,
    };
    await run(
      process.execPath,
      [join(app, "node_modules/typescript/bin/tsc"), "--noEmit"],
      app,
      packagedEnvironment,
    );
    const cli = join(app, "node_modules/muon-ui/dist/cli.cjs");
    const debug = JSON.parse(
      await run(
        process.execPath,
        [cli, "build", "--target", "android", "--json"],
        app,
        packagedEnvironment,
      ),
    ).targets[0];
    const debugApk = join(root, "debug.apk");
    await copyFile(debug.packagePath, debugApk);
    configuration.browser.startPage = `asset://main/index.html?close=1&consumer=${nonce}`;
    await writeFile(join(app, "muon.json"), JSON.stringify(configuration));
    const release = JSON.parse(
      await run(
        process.execPath,
        [cli, "pack", "--target", "android", "--type", "apk", "--json"],
        app,
        packagedEnvironment,
      ),
    ).targets[0];
    expect(release.signing).toBe("release");
    const buildTools = join(androidSdk, "build-tools/36.0.0");
    const badging = await run(join(buildTools, "aapt2"), [
      "dump",
      "badging",
      release.packagePath,
    ]);
    expect(badging).not.toMatch(/^application-debuggable$/mu);
    await run(join(buildTools, "zipalign"), [
      "-c",
      "-P",
      "16",
      "4",
      release.packagePath,
    ]);
    await run(join(buildTools, "apksigner"), [
      "verify",
      "--verbose",
      release.packagePath,
    ]);
    // AAB generation uses the exported Gradle project and standard Android tools;
    // the public Muon pack command currently publishes APKs.
    await run(
      join(release.projectDirectory, "gradlew"),
      ["-p", release.projectDirectory, "bundleRelease"],
      app,
      packagedEnvironment,
    );
    const bundle = join(
      release.projectDirectory,
      "app/build/outputs/bundle/release/app-release.aab",
    );
    await run(join(java21, "bin/jarsigner"), [
      "-keystore",
      keystore,
      "-storepass:env",
      "MUON_CONSUMER_PASSWORD",
      bundle,
      "consumer",
    ]);
    await run(join(java21, "bin/jarsigner"), [
      "-verify",
      "-strict",
      "-keystore",
      keystore,
      "-storepass:env",
      "MUON_CONSUMER_PASSWORD",
      bundle,
      "consumer",
    ]);
    const bundletool =
      process.env.MUON_BUNDLETOOL_JAR ??
      resolve(
        "../core/android-test/android/.generated/bundletool/bundletool-all-1.18.3.jar",
      );
    const passwordFile = join(root, "signing-password.txt");
    await writeFile(passwordFile, environment.MUON_CONSUMER_PASSWORD, {
      mode: 0o600,
    });
    const apks = join(root, "consumer.apks");
    await run(join(java21, "bin/java"), [
      "-jar",
      bundletool,
      "build-apks",
      `--bundle=${bundle}`,
      `--output=${apks}`,
      `--aapt2=${join(buildTools, "aapt2")}`,
      `--ks=${keystore}`,
      "--ks-key-alias=consumer",
      `--ks-pass=file:${passwordFile}`,
      `--key-pass=file:${passwordFile}`,
    ]);
    const distribution = join(root, "distribution");
    await mkdir(distribution);
    await copyFile(debugApk, join(distribution, "debug.apk"));
    await copyFile(release.packagePath, join(distribution, "release.apk"));
    await copyFile(bundle, join(distribution, "release.aab"));
    await rm(app, { recursive: true });
    // Subsequent commands cannot use either installed package or producer source.
    const device = async (...args: string[]) =>
      await run("adb", ["-s", serial, ...args], root);
    const events = new EventEmitter();
    const messages: string[] = [];
    const logcat = spawn(
      "adb",
      [
        "-s",
        serial,
        "logcat",
        "-b",
        "main",
        "-b",
        "events",
        "-v",
        "brief",
        "-T",
        "1",
        "MuonActivity:I",
        "MuonNativeService:I",
        "AndroidRuntime:E",
        "chromium:I",
        "libc:F",
        "wm_on_destroy_called:I",
        "*:S",
      ],
      { stdio: ["ignore", "pipe", "pipe"] },
    );
    let observerClosed = false;
    let observerError: Error | undefined;
    logcat.on("error", (error) => {
      observerError = error;
    });
    logcat.stderr.on("data", (chunk) => {
      messages.push(`adb stderr: ${String(chunk)}`);
    });
    const observerDrained = new Promise<void>((resolve) => {
      logcat.once("close", () => {
        observerClosed = true;
        events.emit("closed");
        resolve();
      });
    });
    const lines = createInterface({ input: logcat.stdout });
    lines.on("line", (line) => {
      messages.push(line);
      events.emit("line", line);
    });
    const waitLine = async (
      start: number,
      accept: (line: string) => boolean,
    ): Promise<string> => {
      for (const line of messages.slice(start)) if (accept(line)) return line;
      if (observerClosed)
        throw observerError ?? new Error("Observer log ended");
      for await (const [line] of on(events, "line", {
        signal: AbortSignal.timeout(45000),
        close: ["closed"],
      }))
        if (accept(String(line))) return String(line);
      throw observerError ?? new Error("Observer log ended");
    };
    const expected = {
      encoded: "AID/",
      native: "native\0日本語😀",
      script: "script\0日本語😀",
      extension: 42,
      event: "event😀",
      references: [1, 1, 1],
    };
    let client: Client | undefined;
    let mcp: StdioClientTransport | undefined;
    let port: string | undefined;
    let failed = false;
    try {
      if (
        (await device("shell", "pm", "list", "packages", appId)).includes(
          `package:${appId}`,
        )
      )
        await device("uninstall", appId);
      await device("install", join(distribution, "debug.apk"));
      await device(
        "shell",
        "pm",
        "grant",
        appId,
        "android.permission.POST_NOTIFICATIONS",
      );
      const first = messages.length;
      await device(
        "shell",
        "am",
        "start",
        "-W",
        "-n",
        `${appId}/dev.muon.runtime.MuonAppActivity`,
      );
      await waitLine(
        first,
        (line) => line.includes("Muon page ready:") && line.includes(nonce),
      );
      const pid = (await device("shell", "pidof", appId)).trim();
      port = (
        await device(
          "forward",
          "tcp:0",
          `localabstract:webview_devtools_remote_${pid}`,
        )
      ).trim();
      mcp = new StdioClientTransport({
        command: process.execPath,
        args: [
          resolve("../node_modules/@playwright/mcp/cli.js"),
          "--cdp-endpoint",
          `http://127.0.0.1:${port}`,
        ],
      });
      client = new Client({ name: "muon-service-consumer", version: "1.0.0" });
      await client.connect(mcp);
      const evaluated = await client.callTool({
        name: "browser_evaluate",
        arguments: { function: "async()=>await globalThis.consumerResult" },
      });
      const text = (evaluated.content as { type: string; text?: string }[])
        .filter((x) => x.type === "text")
        .map((x) => x.text ?? "")
        .join("\n");
      if (evaluated.isError) throw new Error(text);
      const resultText = /### Result\n([\s\S]*?)(?:\n### |$)/u.exec(text)?.[1];
      expect(resultText).toBeDefined();
      const expectedIo = (session: number) =>
        Array.from({ length: 2 }, () => ({
          previous: session === 0 ? [0, 255] : [0, 128, session - 1],
          bytes: [0, 128, 255],
          status: 200,
          owned: true,
        }));
      expect(JSON.parse(resultText!)).toMatchObject({
        ...expected,
        io: expectedIo(0),
      });
      await client.close();
      await mcp.close();
      client = undefined;
      mcp = undefined;
      await device("forward", "--remove", `tcp:${port}`);
      port = undefined;
      await device("uninstall", appId);
      for (const format of ["APK", "AAB"]) {
        if (format === "APK")
          await device("install", join(distribution, "release.apk"));
        else
          await run(
            join(java21, "bin/java"),
            [
              "-jar",
              bundletool,
              "install-apks",
              `--apks=${apks}`,
              `--device-id=${serial}`,
            ],
            root,
          );
        await device(
          "shell",
          "pm",
          "grant",
          appId,
          "android.permission.POST_NOTIFICATIONS",
        );
        let generations: string[] | undefined;
        let previousHostPid: number | undefined;
        for (let session = 0; session < 5; ++session) {
          const start = messages.length;
          await device(
            "shell",
            "am",
            "start",
            "-W",
            "-n",
            `${appId}/dev.muon.runtime.MuonAppActivity`,
          );
          const ready = await waitLine(
            start,
            (line) => line.includes("Muon page ready:") && line.includes(nonce),
          );
          const uiPid = /^I\/MuonActivity\(\s*(\d+)\):/u.exec(ready)?.[1];
          expect(uiPid).toBeDefined();
          const report = await waitLine(start, (line) =>
            line.includes(`consumer-result:${nonce}:`),
          );
          const result = JSON.parse(
            report.split(`consumer-result:${nonce}:`)[1]!,
          );
          expect(result).toMatchObject({
            ...expected,
            io: expectedIo(session),
          });
          expect(result.session).toBe(session);
          if (generations && session < 3)
            expect(result.generations).toEqual(generations);
          generations = result.generations;
          if (session >= 3) {
            expect(previousHostPid).toBeDefined();
            for (const state of result.initial)
              expect(state.lastHostExit).toMatchObject({
                platform: "android",
                processName: `${appId}:muon_services`,
                pid: previousHostPid,
                reason: "user-requested",
              });
          }
          if (session === 2) {
            expect(result.off).toHaveLength(2);
            for (const state of result.off)
              expect(state).toMatchObject({
                autostart: false,
                resident: false,
                state: "running",
                clientReferences: 1,
              });
            expect(result.stopped).toHaveLength(2);
            for (const state of result.stopped)
              expect(state).toMatchObject({
                autostart: false,
                resident: false,
                state: "stopped",
                clientReferences: 0,
              });
          }
          if (session === 3) {
            expect(result.initial).toHaveLength(2);
            for (const state of result.initial)
              expect(state).toMatchObject({
                autostart: false,
                resident: false,
                state: "stopped",
                clientReferences: 0,
              });
            expect(result.enabled).toHaveLength(2);
            for (const state of result.enabled)
              expect(state).toMatchObject({
                autostart: true,
                resident: true,
                state: "running",
                clientReferences: 0,
              });
          }
          if (session === 4) {
            expect(result.initial).toHaveLength(2);
            for (const state of result.initial)
              expect(state).toMatchObject({
                autostart: true,
                resident: true,
                clientReferences: 0,
              });
          }
          await waitLine(
            start,
            (line) =>
              /^I\/wm_on_destroy_called\(\s*(\d+)\):/u.exec(line)?.[1] ===
                uiPid && line.includes("dev.muon.runtime.MuonAppActivity"),
          );
          const closedAt = Number(
            (await device("shell", "date", "+%s%3N")).trim(),
          );
          await waitLine(messages.length, (line) => {
            const json = line.split(`consumer-work:${nonce}:`)[1];
            return json !== undefined && JSON.parse(json).at > closedAt;
          });
          if (session >= 2) {
            previousHostPid = Number(
              (await device("shell", "pidof", `${appId}:muon_services`)).trim(),
            );
            expect(previousHostPid).toBeGreaterThan(0);
            await device("shell", "am", "force-stop", appId);
          }
        }
        await device("shell", "am", "force-stop", appId);
        await device("uninstall", appId);
      }
    } catch (error) {
      failed = true;
      throw error;
    } finally {
      try {
        await closeConsumerResources([
          async () => {
            await client?.close();
          },
          async () => {
            await mcp?.close();
          },
          async () => {
            if (port) await device("forward", "--remove", `tcp:${port}`);
          },
          async () => {
            await device("shell", "am", "force-stop", appId);
          },
          async () => {
            if (!observerClosed) logcat.kill();
            await observerDrained;
            lines.close();
            await writeFile(join(root, "observer.log"), messages.join("\n"));
          },
        ]);
      } catch (error) {
        if (!failed) throw error;
        console.error(error);
      }
    }
  },
  600000,
);
