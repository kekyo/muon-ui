// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile } from "node:child_process";
import { mkdir, mkdtemp, readFile, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { promisify } from "node:util";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { InMemoryTransport } from "@modelcontextprotocol/sdk/inMemory.js";
import { createConnection } from "@playwright/mcp";
import sharp from "sharp";
import { expect, it } from "vitest";

const execute = promisify(execFile);

it
  .skipIf(process.env.MUON_ANDROID_NOTIFICATIONS_E2E !== "1")
  .each(["simple", "validate"])(
  "delivers durable notification events from a public CLI APK in %s mode",
  async (mode) => {
    const serial = process.env.ANDROID_SERIAL;
    const archive = process.env.MUON_ANDROID_NOTIFICATION_TGZ;
    if (!serial || !archive)
      throw new Error(
        "Set ANDROID_SERIAL and MUON_ANDROID_NOTIFICATION_TGZ to a staged muon-ui package.",
      );
    const root = await mkdtemp(join(tmpdir(), `muon-notification-${mode}-`));
    const output = resolve(`.run/android-notifications/${mode}`);
    await mkdir(output, { recursive: true });
    await writeFile(join(output, "project.txt"), root);
    const applicationId = `dev.muon.e2e.notifications.${mode}`;
    const run = async (command: string, args: string[]): Promise<string> =>
      (await execute(command, args, { cwd: root, maxBuffer: 32 * 1024 * 1024 }))
        .stdout;
    const adb = async (...args: string[]): Promise<string> =>
      await run("adb", ["-s", serial, ...args]);
    const versions = await Promise.all(
      ["vite", "typescript", "prettier-max", "resolved-killer"].map(
        async (name) => [
          name,
          JSON.parse(
            await readFile(
              resolve(`../node_modules/${name}/package.json`),
              "utf8",
            ),
          ).version,
        ],
      ),
    );
    await writeFile(
      join(root, "package.json"),
      JSON.stringify({
        name: "notification-driver",
        version: "1.0.0",
        private: true,
        type: "module",
        devDependencies: Object.fromEntries(versions),
      }),
    );
    await run("npm", [
      "install",
      "--ignore-scripts",
      "--no-audit",
      "--no-fund",
      resolve(archive),
    ]);
    await execute(
      resolve("../core/android-test/android/gradlew"),
      [":observer:assembleDebug", ":observer:assembleDebugAndroidTest"],
      {
        cwd: resolve("../core/android-test/android"),
        maxBuffer: 16 * 1024 * 1024,
      },
    );
    for (const path of [
      "apk/debug/observer-debug.apk",
      "apk/androidTest/debug/observer-debug-androidTest.apk",
    ])
      await adb(
        "install",
        "-r",
        resolve("../core/android-test/observer/build/outputs", path),
      );
    const config = {
      notifications: { enabled: true, defaultCategory: "general" },
      android: {
        applicationId,
        label: `Muon notifications ${mode}`,
        versionCode: 1,
        abis: ["arm64-v8a"],
        notifications: {
          smallIcon: "notification.png",
          channels: [
            {
              id: "general",
              name: "General notifications",
              description: "Orders",
              importance: "default",
            },
            { id: "quiet", name: "Quiet notifications", importance: "low" },
          ],
        },
      },
      plugin: {
        mode,
        plugins: [
          {
            name: "internal",
            ...(mode === "simple"
              ? {
                  allow: [
                    "muon.notifications.*",
                    "muon.android.notifications.*",
                    "muon.android.settings.*",
                  ],
                }
              : {
                  imports: [
                    {
                      sources: ["main.ts"],
                      allow: [
                        "muon.notifications.*",
                        "muon.android.notifications.*",
                        "muon.android.settings.*",
                      ],
                    },
                  ],
                }),
          },
        ],
      },
    };
    await writeFile(join(root, "muon.json"), JSON.stringify(config));
    await writeFile(
      join(root, "notification.png"),
      await sharp(
        Buffer.from(
          '<svg xmlns="http://www.w3.org/2000/svg" width="72" height="72" viewBox="0 0 24 24"><path fill="white" d="M12 2a6 6 0 0 0-6 6v6l-2 3v1h16v-1l-2-3V8a6 6 0 0 0-6-6Zm-3 18a3 3 0 0 0 6 0Z"/></svg>',
        ),
      )
        .png()
        .toBuffer(),
    );
    await writeFile(
      join(root, "tsconfig.json"),
      JSON.stringify({
        compilerOptions: {
          target: "ES2022",
          module: "ESNext",
          moduleResolution: "Bundler",
          strict: true,
          noEmit: true,
          lib: ["ES2022", "DOM"],
          skipLibCheck: true,
        },
        include: ["main.ts", "vite.config.ts"],
      }),
    );
    await writeFile(
      join(root, "vite.config.ts"),
      `import {defineConfig} from 'vite';\nimport muon from 'muon-ui/vite';\nimport prettierMax from 'prettier-max';\nexport default defineConfig({plugins:[prettierMax(),muon({${mode === "simple" ? "pluginAccess:false," : ""}build:{targets:['android']}})],build:{target:'es2022'}});`,
    );
    await writeFile(
      join(root, "index.html"),
      '<html><head><meta name="viewport" content="width=device-width,initial-scale=1"><title>Notification driver</title></head><body><h1>Native notifications</h1><div id="buttons"></div><pre id="result">Starting</pre><pre id="events">[]</pre><script type="module" src="/main.ts"></script></body></html>',
    );
    await writeFile(
      join(root, "main.ts"),
      `import type {} from 'muon-ui';
${mode === "validate" ? `import * as notifications from 'muon:notifications';\nimport * as android from 'muon:android.notifications';\nimport * as settings from 'muon:android.settings';` : `const notifications=window.muon.notifications!;\nconst android=window.muon.android!.notifications!;\nconst settings=window.muon.android!.settings;`}
const output=document.querySelector('#result')!;
const events=new Map<string,MuonNotificationEvent>();
const receive=(event:MuonNotificationEvent)=>{if(events.has(event.eventId))return;events.set(event.eventId,event);document.querySelector('#events')!.textContent=JSON.stringify([...events.values()]);};
let watcher:MuonNotificationWatcher|undefined;
const startWatching=async()=>{if(watcher===undefined)watcher=await notifications.watchEvents(receive);for(const event of await notifications.getPendingEvents())receive(event);};
const stopWatching=async()=>{await watcher?.close();watcher=undefined;};
const operations: Record<string,()=>Promise<unknown>>={
  'Post order':async()=>await notifications.post({id:'order',title:'Muon order original',body:'Preparing',data:{order:42}}),
  'Update order':async()=>await notifications.post({id:'order',title:'Muon order updated',body:'Ready',data:{order:42}}),
  'Post other':async()=>await notifications.post({id:'other',title:'Muon other',body:'Independent',category:'general',data:{order:43}}),
  'Cancel order':async()=>await notifications.cancel('order'),
  'Cancel other':async()=>await notifications.cancel('other'),
  'Read state':async()=>({status:await notifications.getStatus('general'),capabilities:await notifications.getCapabilities(),channel:await android.getChannel('general')}),
  'Post dynamic':async()=>{await android.ensureChannel({id:'dynamic',name:'Dynamic notifications',importance:'low'});await notifications.post({id:'dynamic',title:'Muon dynamic',body:'Created explicitly',category:'dynamic'});},
  'Open channel settings':async()=>await settings.open({type:'notification-channel',channelId:'general'}),
  'Start watching':startWatching,
  'Stop watching':stopWatching,
};
Reflect.set(globalThis,'notificationDriver',{notifications,android,settings,events,startWatching,stopWatching});
for(const [label,action] of Object.entries(operations)) {const button=document.createElement('button');button.textContent=label;button.onclick=async()=>{try{output.textContent=JSON.stringify({label,result:await action()??null});}catch(error){output.textContent=JSON.stringify({label,error:String(error)});}};document.querySelector('#buttons')!.append(button);}
try{await startWatching();output.textContent='Ready';}catch(error){output.textContent='Startup failed: '+String(error);throw error;}`,
    );
    await run(process.execPath, [
      join(root, "node_modules/typescript/bin/tsc"),
      "--noEmit",
    ]);
    const buildText = await run(process.execPath, [
      join(root, "node_modules/muon-ui/dist/cli.cjs"),
      "build",
      "--target",
      "android",
      "--json",
    ]);
    await writeFile(join(output, "build.json"), buildText);
    const result = JSON.parse(buildText).targets[0];
    const installed = await adb(
      "shell",
      "pm",
      "list",
      "packages",
      applicationId,
    );
    if (installed.split(/\r?\n/u).includes(`package:${applicationId}`))
      await adb("uninstall", applicationId);
    await adb("install", result.packagePath);
    await adb(
      "shell",
      "pm",
      "grant",
      applicationId,
      "android.permission.POST_NOTIFICATIONS",
    );
    await adb(
      "shell",
      "am",
      "start",
      "-W",
      "-f",
      "0x10008000",
      "-n",
      `${applicationId}/dev.muon.runtime.MuonAppActivity`,
    );
    const pid = (await adb("shell", "pidof", applicationId)).trim();
    expect(pid).toMatch(/^\d+$/u);
    await expect
      .poll(async () => await adb("shell", "cat", "/proc/net/unix"), {
        timeout: 60000,
      })
      .toContain(`webview_devtools_remote_${pid}`);
    const port = (
      await adb(
        "forward",
        "tcp:0",
        `localabstract:webview_devtools_remote_${pid}`,
      )
    ).trim();
    let client = new Client({
      name: "muon-notification-package-test",
      version: "1",
    });
    let mcp = await createConnection({
      browser: { cdpEndpoint: `http://127.0.0.1:${port}` },
      outputDir: output,
    });
    const [clientTransport, serverTransport] =
      InMemoryTransport.createLinkedPair();
    const call = async (
      name: string,
      args: Record<string, unknown>,
    ): Promise<string> => {
      const response = await client.callTool({ name, arguments: args });
      const text = (response.content as Array<{ type: string; text?: string }>)
        .filter((value) => value.type === "text")
        .map((value) => value.text ?? "")
        .join("\n");
      if (response.isError) throw new Error(text);
      return text;
    };
    const evaluate = async <T>(script: string): Promise<T> => {
      const text = await call("browser_evaluate", { function: script });
      const match = /### Result\n([\s\S]*?)(?:\n###|$)/u.exec(text);
      if (match === null)
        throw new Error(`No browser evaluation result: ${text}`);
      return JSON.parse(match[1]!.trim()) as T;
    };
    const disconnect = async (): Promise<void> => {
      await client.close();
      await mcp.close();
    };
    const reconnect = async (): Promise<void> => {
      await expect
        .poll(
          async () =>
            (await adb("shell", "ps", "-A", "-o", "NAME"))
              .split(/\r?\n/u)
              .some((name) => name.trim() === applicationId),
          { timeout: 30000 },
        )
        .toBe(true);
      const newPid = (await adb("shell", "pidof", applicationId)).trim();
      expect(newPid).toMatch(/^\d+$/u);
      await expect
        .poll(async () => await adb("shell", "cat", "/proc/net/unix"), {
          timeout: 60000,
        })
        .toContain(`webview_devtools_remote_${newPid}`);
      await adb(
        "forward",
        `tcp:${port}`,
        `localabstract:webview_devtools_remote_${newPid}`,
      );
      client = new Client({
        name: "muon-notification-package-test",
        version: "1",
      });
      mcp = await createConnection({
        browser: { cdpEndpoint: `http://127.0.0.1:${port}` },
        outputDir: output,
      });
      const [nextClient, nextServer] = InMemoryTransport.createLinkedPair();
      await mcp.connect(nextServer);
      await client.connect(nextClient);
      await waitForReady();
    };
    const stopProcess = async (): Promise<void> => {
      const processId = (await adb("shell", "pidof", applicationId)).trim();
      expect(processId).toMatch(/^\d+$/u);
      await disconnect();
      await adb("shell", "input", "keyevent", "KEYCODE_HOME");
      // Terminate only this debug-enabled driver's process. Unlike force-stop,
      // this preserves its OS notifications and PendingIntents, and does not
      // depend on ActivityManager having already moved the app to the cache.
      await adb("shell", "run-as", applicationId, "kill", "-9", processId);
      await expect
        .poll(
          async () => {
            const running = await adb("shell", "ps", "-A", "-o", "NAME");
            return running
              .split(/\r?\n/u)
              .some((name) => name.trim() === applicationId);
          },
          { timeout: 30000 },
        )
        .toBe(false);
    };
    const launch = async (): Promise<void> => {
      await adb(
        "shell",
        "am",
        "start",
        "-W",
        "-n",
        `${applicationId}/dev.muon.runtime.MuonAppActivity`,
      );
      await reconnect();
    };
    const tapNotification = async (
      title: string,
      label: string,
    ): Promise<void> => {
      await adb("shell", "cmd", "statusbar", "expand-notifications");
      const observation = await adb(
        "shell",
        "am",
        "instrument",
        "-w",
        "-r",
        "-e",
        "class",
        "dev.muon.e2e.observer.NotificationDisplayTest",
        "-e",
        "expectedBase64",
        Buffer.from(title).toString("base64"),
        "-e",
        "click",
        "true",
        "dev.muon.e2e.observer.test/androidx.test.runner.AndroidJUnitRunner",
      );
      await writeFile(join(output, `${label}-observer.log`), observation);
      expect(observation).toMatch(/OK \(1 test\)/u);
    };
    const pendingEvents = async (): Promise<
      Array<{
        eventId: string;
        id: string;
        type: string;
        actionId: string | null;
        data: unknown;
      }>
    > =>
      await evaluate(
        "async()=>await globalThis.notificationDriver.notifications.getPendingEvents()",
      );
    const waitForEvent = async (id: string): Promise<void> => {
      await call("browser_evaluate", {
        function: `async()=>{const element=document.querySelector('#events');const found=()=>JSON.parse(element.textContent).some(event=>event.id===${JSON.stringify(id)});if(!found())await new Promise(resolve=>{const observer=new MutationObserver(()=>{if(found()){observer.disconnect();resolve();}});observer.observe(element,{childList:true,subtree:true,characterData:true});});return JSON.parse(element.textContent);}`,
      });
    };
    const waitForReady = async (): Promise<void> => {
      await call("browser_evaluate", {
        function:
          "async()=>{const state=()=>document.querySelector('#result')?.textContent;const ready=()=>state()!==undefined&&state()!=='Starting';if(!ready())await new Promise(resolve=>{const observer=new MutationObserver(()=>{if(ready()){observer.disconnect();resolve();}});observer.observe(document,{childList:true,subtree:true,characterData:true});});if(state().startsWith('Startup failed:'))throw new Error(state());return state();}",
      });
    };
    const records = async (): Promise<string[]> => {
      const dump = await adb("shell", "dumpsys", "notification", "--noredact");
      return dump
        .split(/(?=\s*NotificationRecord\()/u)
        .filter((record) =>
          new RegExp(
            `^\\s*NotificationRecord\\([^\\n]*pkg=${applicationId.replaceAll(".", "\\.")}\\b`,
          ).test(record),
        );
    };
    const click = async (label: string): Promise<void> => {
      await call("browser_click", { target: `button:text-is("${label}")` });
      await call("browser_evaluate", {
        function: `async()=>{const element=document.querySelector('#result');const label=${JSON.stringify(label)};const done=()=>{try{return JSON.parse(element.textContent).label===label}catch{return false}};if(!done())await new Promise(resolve=>{const observer=new MutationObserver(()=>{if(done()){observer.disconnect();resolve()}});observer.observe(element,{childList:true,subtree:true,characterData:true});});const result=JSON.parse(element.textContent);if(result.error)throw new Error(result.error);return result;}`,
      });
    };
    try {
      await mcp.connect(serverTransport);
      await client.connect(clientTransport);
      await waitForReady();
      await call("browser_evaluate", {
        function: `()=>{if(${mode === "validate"}&&globalThis.muon!==undefined)throw new Error('validate exposed globals');return document.querySelector('#result').textContent;}`,
      });
      await click("Read state");
      await evaluate(
        "async()=>{const capabilities=await globalThis.notificationDriver.notifications.getCapabilities();for(const key of ['interaction','launch','actions'])if(capabilities[key].state!=='supported')throw new Error(JSON.stringify(capabilities));return capabilities;}",
      );
      await call("browser_evaluate", {
        function:
          "async()=>{const api=globalThis.notificationDriver;const state=await api.notifications.getStatus();if(state.app.state!=='enabled'||state.category!==null)throw new Error(JSON.stringify(state));const channel=await api.android.getChannel('general');if(channel.id!=='general'||!channel.enabled)throw new Error(JSON.stringify(channel));let refused=false;try{await api.notifications.post({id:'invalid',title:'No category',body:'Must fail',category:'missing'})}catch{refused=true}if(!refused)throw new Error('Unknown category was accepted');return state;}",
      });
      await click("Post order");
      await expect
        .poll(
          async () =>
            (await records()).filter((record) => record.includes("tag=order "))
              .length,
        )
        .toBe(1);
      await adb("shell", "cmd", "statusbar", "expand-notifications");
      const observation = await adb(
        "shell",
        "am",
        "instrument",
        "-w",
        "-r",
        "-e",
        "class",
        "dev.muon.e2e.observer.NotificationDisplayTest",
        "-e",
        "expectedBase64",
        Buffer.from("Muon order original").toString("base64"),
        "dev.muon.e2e.observer.test/androidx.test.runner.AndroidJUnitRunner",
      );
      await writeFile(join(output, "display-observer.log"), observation);
      expect(observation).toMatch(/OK \(1 test\)/u);
      const screen = await execute(
        "adb",
        ["-s", serial, "exec-out", "screencap", "-p"],
        { encoding: "buffer" },
      );
      await writeFile(join(output, "notification.png"), screen.stdout);
      await adb("shell", "cmd", "statusbar", "collapse");
      await click("Update order");
      await expect
        .poll(
          async () =>
            (await records()).filter(
              (record) =>
                record.includes("tag=order ") &&
                record.includes("Muon order updated"),
            ).length,
        )
        .toBe(1);
      await click("Post other");
      await expect
        .poll(
          async () =>
            (await records()).filter((record) =>
              /tag=(?:order|other) /u.test(record),
            ).length,
        )
        .toBe(2);
      await click("Cancel order");
      await expect
        .poll(
          async () =>
            (await records()).filter((record) => record.includes("tag=order "))
              .length,
        )
        .toBe(0);
      expect(
        (await records()).some((record) => record.includes("tag=other ")),
      ).toBe(true);
      await adb("shell", "input", "keyevent", "KEYCODE_HOME");
      await adb("shell", "cmd", "statusbar", "expand-notifications");
      const activation = await adb(
        "shell",
        "am",
        "instrument",
        "-w",
        "-r",
        "-e",
        "class",
        "dev.muon.e2e.observer.NotificationDisplayTest",
        "-e",
        "expectedBase64",
        Buffer.from("Muon other").toString("base64"),
        "-e",
        "click",
        "true",
        "dev.muon.e2e.observer.test/androidx.test.runner.AndroidJUnitRunner",
      );
      await writeFile(join(output, "activation-observer.log"), activation);
      expect(activation).toMatch(/OK \(1 test\)/u);
      await expect
        .poll(async () =>
          (await records()).some((record) => record.includes("tag=other ")),
        )
        .toBe(false);
      await expect
        .poll(async () => {
          const activities = await adb(
            "shell",
            "dumpsys",
            "activity",
            "activities",
          );
          return activities
            .split(/\r?\n/u)
            .some(
              (line) =>
                /(?:topResumedActivity|mResumedActivity)/u.test(line) &&
                line.includes(
                  `${applicationId}/dev.muon.runtime.MuonAppActivity`,
                ),
            );
        })
        .toBe(true);
      await click("Read state");
      await click("Post dynamic");
      await expect
        .poll(async () =>
          (await records()).some((record) => record.includes("tag=dynamic ")),
        )
        .toBe(true);
      await click("Open channel settings");
      await adb(
        "shell",
        "uiautomator",
        "dump",
        "/sdcard/muon-channel-settings.xml",
      );
      const settings = await adb(
        "shell",
        "cat",
        "/sdcard/muon-channel-settings.xml",
      );
      expect(settings).toContain("General notifications");
      expect(settings).toContain("com.android.settings");
      await writeFile(join(output, "settings.xml"), settings);
      await adb("shell", "input", "keyevent", "4");
      await click("Cancel other");
      await call("browser_evaluate", {
        function:
          "async()=>{await globalThis.notificationDriver.notifications.cancel('dynamic');return 'All posted notifications cancelled';}",
      });
      await expect
        .poll(
          async () =>
            (await records()).filter((record) =>
              /tag=(?:order|other|dynamic) /u.test(record),
            ).length,
        )
        .toBe(0);
      await waitForEvent("other");
      const firstEvents = await pendingEvents();
      expect(firstEvents).toHaveLength(1);
      expect(firstEvents[0]).toMatchObject({
        id: "other",
        type: "activate",
        actionId: null,
        data: { order: 43 },
      });
      const pageUrl = await evaluate<string>("()=>location.href");
      await call("browser_navigate", { url: pageUrl });
      await waitForReady();
      await waitForEvent("other");
      expect(await pendingEvents()).toEqual(firstEvents);
      await evaluate(
        "async()=>{for(const event of await globalThis.notificationDriver.notifications.getPendingEvents())await globalThis.notificationDriver.notifications.acknowledgeEvent(event.eventId);return true;}",
      );
      await call("browser_navigate", { url: pageUrl });
      await waitForReady();
      expect(await pendingEvents()).toEqual([]);

      // A closed page subscription must not consume operations. Registering again
      // and reading pending events recovers the event without automatic ACK.
      await click("Stop watching");
      await evaluate(
        "async()=>{await globalThis.notificationDriver.notifications.post({id:'paused',title:'Muon paused subscription',body:'Still retained',data:{paused:true}});return true;}",
      );
      await tapNotification("Muon paused subscription", "paused");
      await expect
        .poll(async () =>
          (await records()).some((record) => record.includes("tag=paused ")),
        )
        .toBe(false);
      const paused = await pendingEvents();
      expect(paused).toHaveLength(1);
      expect(paused[0]).toMatchObject({ id: "paused", data: { paused: true } });
      expect(
        await evaluate(
          "()=>[...globalThis.notificationDriver.events.values()]",
        ),
      ).toEqual([]);
      await click("Start watching");
      await waitForEvent("paused");
      await evaluate(
        "async()=>{for(const event of await globalThis.notificationDriver.notifications.getPendingEvents())await globalThis.notificationDriver.notifications.acknowledgeEvent(event.eventId);return true;}",
      );

      await evaluate(
        "async()=>{await globalThis.notificationDriver.notifications.post({id:'action',title:'Muon named action',body:'Open from a named action',data:{order:44},actions:[{id:'default',title:'Muon view order'}]});return true;}",
      );
      await tapNotification("Muon view order", "action");
      await waitForEvent("action");
      expect(await pendingEvents()).toEqual([
        expect.objectContaining({
          id: "action",
          type: "action",
          actionId: "default",
          data: { order: 44 },
        }),
      ]);
      await evaluate(
        "async()=>{for(const event of await globalThis.notificationDriver.notifications.getPendingEvents())await globalThis.notificationDriver.notifications.acknowledgeEvent(event.eventId);return true;}",
      );

      // Activate the retained OS notification before a replacement WebView
      // initializes, then retain the unacknowledged event across another exit.
      await evaluate(
        "async()=>{const api=globalThis.notificationDriver.notifications;await api.post({id:'cold',title:'Muon cold old',body:'Old revision',data:{revision:1}});await api.post({id:'cold',title:'Muon cold updated',body:'Saved before shutdown',data:{revision:2}});return true;}",
      );
      await expect
        .poll(
          async () =>
            (await records()).filter((record) => record.includes("tag=cold "))
              .length,
        )
        .toBe(1);
      await stopProcess();
      expect(
        (await records()).filter((record) => record.includes("tag=cold ")),
      ).toHaveLength(1);
      await tapNotification("Muon cold updated", "cold");
      await reconnect();
      await waitForEvent("cold");
      const cold = await pendingEvents();
      expect(cold).toEqual([
        expect.objectContaining({
          id: "cold",
          type: "activate",
          actionId: null,
          data: { revision: 2 },
        }),
      ]);
      await stopProcess();
      await launch();
      await waitForEvent("cold");
      expect(await pendingEvents()).toEqual(cold);

      await evaluate(
        "async()=>{await globalThis.notificationDriver.notifications.post({id:'update',title:'Muon before APK update',body:'Pending during replacement',data:{revision:3}});return true;}",
      );
      await stopProcess();
      expect(
        (await records()).filter((record) => record.includes("tag=update ")),
      ).toHaveLength(1);
      config.android.versionCode = 2;
      await writeFile(join(root, "muon.json"), JSON.stringify(config));
      const updateBuild = await run(process.execPath, [
        join(root, "node_modules/muon-ui/dist/cli.cjs"),
        "build",
        "--target",
        "android",
        "--json",
      ]);
      await writeFile(join(output, "update-build.json"), updateBuild);
      const updatedApk = JSON.parse(updateBuild).targets[0].packagePath;
      await adb("install", "-r", updatedApk);
      // This Pixel's package replacement clears OS notifications. The durable
      // event already received before replacement must still be recoverable.
      await expect
        .poll(async () =>
          (await records()).some((record) => record.includes("tag=update ")),
        )
        .toBe(false);
      await launch();
      await waitForEvent("cold");
      expect(await pendingEvents()).toEqual(cold);
      await evaluate(
        "async()=>{for(const event of await globalThis.notificationDriver.notifications.getPendingEvents())await globalThis.notificationDriver.notifications.acknowledgeEvent(event.eventId);return true;}",
      );
      await stopProcess();
      await launch();
      expect(await pendingEvents()).toEqual([]);
      await writeFile(
        join(output, "result.json"),
        JSON.stringify(
          {
            mode,
            serial,
            apk: result.packagePath,
            posted: true,
            updated: true,
            cancelled: true,
            dynamicChannel: true,
            channelSettings: true,
            notificationTap: true,
            eventReplay: true,
            acknowledgement: true,
            namedAction: true,
            processRestart: true,
            applicationUpdate: true,
            notificationsClearedByPackageUpdate: true,
          },
          null,
          2,
        ),
      );
    } finally {
      await disconnect();
      await adb("forward", "--remove", `tcp:${port}`);
      await adb("shell", "am", "force-stop", applicationId);
    }
  },
  600000,
);
