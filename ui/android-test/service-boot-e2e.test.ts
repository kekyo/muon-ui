// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile } from "node:child_process";
import { mkdir, mkdtemp, rm, symlink, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { promisify } from "node:util";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { InMemoryTransport } from "@modelcontextprotocol/sdk/inMemory.js";
import { createConnection } from "@playwright/mcp";
import { buildService, createServiceProject } from "muon-plugin-sdk";
import { expect, it } from "vitest";
import { buildMuonApp } from "../common/build.ts";

const execute = promisify(execFile);
const app = "dev.muon.e2e.serviceboot";
const activity = app + "/dev.muon.runtime.MuonAppActivity";

it.skipIf(process.env.MUON_ANDROID_SERVICE_BOOT_E2E !== "1")(
  "starts saved resident services on first unlock without opening the app",
  async () => {
    const serial = process.env.ANDROID_SERIAL;
    const pin = process.env.MUON_ANDROID_SERVICE_BOOT_PIN;
    if (!serial?.startsWith("emulator-") || !pin?.match(/^\d+$/u))
      throw new Error(
        "Select a disposable PIN-locked emulator with ANDROID_SERIAL and MUON_ANDROID_SERVICE_BOOT_PIN.",
      );
    const adb = async (...args: string[]) =>
      (
        await execute("adb", ["-s", serial, ...args], { timeout: 60000 })
      ).stdout.trim();
    const shell = async (...args: string[]) => await adb("shell", ...args);
    const userState = async () =>
      await shell("am", "get-started-user-state", "0");
    expect(await userState()).toBe("RUNNING_UNLOCKED");
    expect(
      Number(await shell("getprop", "ro.build.version.sdk")),
    ).toBeGreaterThanOrEqual(35);

    const root = await mkdtemp(join(tmpdir(), "muon-service-boot-"));
    await mkdir(join(root, "web"));
    await symlink(resolve("../node_modules"), join(root, "node_modules"));
    await writeFile(
      join(root, "web/index.html"),
      "<!doctype html><title>Service boot</title>",
    );
    await writeFile(
      join(root, "package.json"),
      JSON.stringify({
        name: "service-boot-test",
        version: "1.0.0",
        type: "module",
      }),
    );
    await writeFile(
      join(root, "worker.ts"),
      `
import {defineService, defineServiceInterface} from 'muon-plugin-sdk/services';
export default defineService({
  interface: defineServiceInterface({schemaVersion:1, methods:{echo:{parameters:['string'],returns:'string',errors:[]}}, events:{}}),
  start: context => {
    console.log('boot-test-start ' + context.id + ' ' + context.generation);
    return {methods:{echo: (_call, value) => value}, stop: async () => {}};
  }
});`,
    );
    const worker = {
      runtime: "quickjs",
      entry: "worker.ts",
      android: {
        foregroundServiceType: "specialUse",
        specialUseSubtype: "Test user-enabled resident services",
        notification: { channelId: "worker", title: "Worker", text: "Running" },
      },
    };
    const nativeDirectory = join(root, "native");
    await createServiceProject({
      directory: nativeDirectory,
      name: "bootnative",
      language: "c",
      buildSystem: "make",
    });
    const artifact = await buildService({
      configPath: join(nativeDirectory, "muon-plugin.build.json"),
      target: "android-x86_64",
    });
    const buildApk = async (disabledDefault: boolean) => {
      await writeFile(
        join(root, "muon.json"),
        JSON.stringify({
          android: {
            applicationId: app,
            abis: ["x86_64"],
            permissions: ["android.permission.ACCESS_COARSE_LOCATION"],
          },
          plugin: {
            mode: "simple",
            plugins: [{ name: "internal", allow: ["muon.services.*"] }],
          },
          services: {
            worker,
            native: { runtime: "native", artifact, android: worker.android },
            sync: {
              ...worker,
              android: {
                ...worker.android,
                foregroundServiceType: "specialUse|dataSync",
              },
            },
            location: {
              ...worker,
              android: {
                foregroundServiceType: "location",
                notification: worker.android.notification,
              },
            },
            disabled: { ...worker, autostart: disabledDefault },
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
        throw new Error("Missing Android package");
      return apk.packagePath;
    };

    const dismissKeyguard = async () => {
      // Only dismiss the keyguard when the test actually opens the UI. Restore the
      // disposable emulator's PIN before launching the app or performing another reboot.
      if (
        (await shell("dumpsys", "window")).includes("isKeyguardShowing=true")
      ) {
        await shell("locksettings", "clear", "--old", pin);
        try {
          await shell("input", "keyevent", "KEYCODE_WAKEUP");
          await shell("wm", "dismiss-keyguard");
          await expect
            .poll(async () => await shell("dumpsys", "window"), {
              timeout: 30000,
            })
            .toContain("isKeyguardShowing=false");
        } finally {
          await shell("locksettings", "set-pin", pin);
        }
      }
    };
    const page = async (body: string) => {
      await shell("am", "start", "-W", "-n", activity);
      const pid = await shell("pidof", app);
      await expect
        .poll(
          async () =>
            await adb("logcat", "-d", "--pid=" + pid, "MuonActivity:I", "*:S"),
          { timeout: 30000 },
        )
        .toContain("Muon page ready:");
      const port = await adb(
        "forward",
        "tcp:0",
        "localabstract:webview_devtools_remote_" + pid,
      );
      const server = await createConnection({
        browser: { cdpEndpoint: "http://127.0.0.1:" + port },
        outputDir: root,
      });
      const client = new Client({ name: "service-boot", version: "1" });
      try {
        const [a, b] = InMemoryTransport.createLinkedPair();
        await server.connect(b);
        await client.connect(a);
        const result = await client.callTool({
          name: "browser_evaluate",
          arguments: { function: "async()=>{" + body + "}" },
        });
        const content = result.content as { text?: string }[];
        const output = content.map((block) => block.text).join("\n");
        if (result.isError) throw new Error(output);
        const json = /### Result\n([\s\S]*?)(?:\n### |$)/u.exec(output)?.[1];
        if (!json) throw new Error(output);
        return JSON.parse(json);
      } finally {
        await client.close();
        await server.close();
        await adb("forward", "--remove", "tcp:" + port);
      }
    };
    const serviceLog = async () =>
      await adb("logcat", "-d", "-v", "brief", "MuonNativeService:I", "*:S");
    const reboot = async () => {
      const previous = await shell("cat", "/proc/sys/kernel/random/boot_id");
      expect(await shell("dumpsys", "package", app)).toMatch(
        /User 0: [^\n]*stopped=false notLaunched=false/u,
      );
      // Use Android's normal shutdown so PackageManager persists the first launch.
      // An immediate adb reboot can restore the just-installed package's stopped flag.
      await shell("svc", "power", "reboot", "--no-wait");
      await adb("wait-for-disconnect");
      await adb("wait-for-device");
      await expect
        .poll(async () => await shell("getprop", "sys.boot_completed"), {
          timeout: 180000,
        })
        .toBe("1");
      expect(await shell("cat", "/proc/sys/kernel/random/boot_id")).not.toBe(
        previous,
      );
      expect(await userState()).toBe("RUNNING_LOCKED");
      expect(await shell("dumpsys", "activity", "services", app)).not.toContain(
        "isForeground=true",
      );
      // Verify the real lock credential through Android's lock service. This unlocks
      // credential-protected storage without starting an Activity or depending on SystemUI animations.
      await shell("locksettings", "verify", "--old", pin);
      await expect.poll(userState, { timeout: 30000 }).toBe("RUNNING_UNLOCKED");
      // User storage becomes available before Android finishes delivering BOOT_COMPLETED.
      await expect
        .poll(
          async () =>
            await adb("logcat", "-b", "system", "-d", "-s", "UserController:I"),
          { timeout: 90000 },
        )
        .toContain("Finished processing BOOT_COMPLETED for u0");
      await shell("cmd", "activity", "wait-for-broadcast-barrier");
    };
    const listed = await shell("pm", "list", "packages", app);
    if (listed.split("\n").includes("package:" + app))
      await adb("uninstall", app);
    await adb("install", await buildApk(false));
    try {
      await dismissKeyguard();
      expect(
        await page(`await muon.services.setAutostart('disabled',false);
        for (const id of ['worker','native','sync','location']) await muon.services.setAutostart(id,true);
        return (await muon.services.getState('worker')).autostart;`),
      ).toBe(true);
      await reboot();
      await expect
        .poll(serviceLog, { timeout: 30000 })
        .toContain("boot-test-start worker ");
      expect(await serviceLog()).not.toContain("boot-test-start disabled ");
      await expect
        .poll(serviceLog, { timeout: 30000 })
        .toContain("native: native");
      await expect
        .poll(serviceLog, { timeout: 30000 })
        .toMatch(/sync: .*BOOT_COMPLETED.*dataSync/u);
      expect(await serviceLog()).not.toMatch(
        /boot-test-start (sync|location) /u,
      );
      expect(await shell("dumpsys", "activity", "services", app)).toContain(
        "isForeground=true",
      );
      expect(await shell("dumpsys", "activity", "activities")).not.toContain(
        activity,
      );
      expect((await shell("ps", "-A", "-o", "NAME")).split("\n")).not.toContain(
        app,
      );
      // Replay a system-only broadcast on the disposable userdebug image as the UI reconnects.
      await dismissKeyguard();
      const [broadcast] = await Promise.all([
        shell(
          "su",
          "0",
          "am",
          "broadcast",
          "-a",
          "android.intent.action.BOOT_COMPLETED",
          "-n",
          app + "/dev.muon.runtime.MuonServiceBootReceiver",
        ),
        page("return true;"),
      ]);
      expect(broadcast).toContain("Broadcast completed");
      // Opening the Activity alone restores eligible demand, even if the page never calls a service.
      await expect
        .poll(serviceLog, { timeout: 30000 })
        .toContain("boot-test-start sync ");
      const state =
        await page(`const handle=await muon.services.acquire('worker');
        try { return { echo:await handle.call('echo',['after boot']), generation:handle.generation,
          native:await muon.services.getState('native') }; } finally { await handle.release(); }`);
      expect(state).toMatchObject({
        echo: "after boot",
        generation: "1",
        native: { state: "running", generation: "1", clientReferences: 0 },
      });
      const denied = await page(
        "return await muon.services.getState('location');",
      );
      expect(denied).toMatchObject({
        state: "failed",
        autostart: true,
        clientReferences: 0,
      });
      expect(denied.error).toMatch(/permission/i);
      await shell("input", "keyevent", "KEYCODE_HOME");
      await shell(
        "pm",
        "grant",
        app,
        "android.permission.ACCESS_COARSE_LOCATION",
      );
      await page("return true;");
      await expect
        .poll(serviceLog, { timeout: 30000 })
        .toContain("boot-test-start location ");
      expect((await serviceLog()).match(/native: native\b/gu)).toHaveLength(1);
      expect(
        (await serviceLog()).match(/boot-test-start sync /gu),
      ).toHaveLength(1);
      expect(
        (await serviceLog()).match(/boot-test-start worker /gu),
      ).toHaveLength(1);
      await page(
        "for (const id of ['worker','native','sync','location']) await muon.services.setAutostart(id,false); return true;",
      );
      // The saved OFF also wins over a new ON default in an application update.
      await adb("install", "-r", await buildApk(true));
      await reboot();
      expect(await shell("dumpsys", "activity", "services", app)).not.toContain(
        "isForeground=true",
      );
      expect(await serviceLog()).not.toContain("boot-test-start");
      expect(await serviceLog()).not.toContain("native: native");
      await dismissKeyguard();
      expect(
        await page(
          "return (await muon.services.getState('disabled')).autostart;",
        ),
      ).toBe(false);
    } catch (error) {
      const diagnostic = await Promise.allSettled([
        shell("dumpsys", "package", app),
        shell("dumpsys", "activity", "broadcasts", app),
        adb("logcat", "-b", "system", "-d", "-t", "4000"),
      ]);
      await writeFile(
        join(root, "boot-diagnostics.log"),
        diagnostic
          .map((entry) =>
            entry.status === "fulfilled" ? entry.value : String(entry.reason),
          )
          .join("\n"),
      );
      console.error("Boot test diagnostics:", root);
      throw error;
    } finally {
      await adb("uninstall", app);
    }
    await rm(root, { recursive: true, force: true });
  },
  360000,
);
