// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile } from "node:child_process";
import { mkdir, mkdtemp, symlink, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { promisify } from "node:util";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { InMemoryTransport } from "@modelcontextprotocol/sdk/inMemory.js";
import { createConnection } from "@playwright/mcp";
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
    const buildApk = async (disabledDefault: boolean) => {
      await writeFile(
        join(root, "muon.json"),
        JSON.stringify({
          android: { applicationId: app, abis: ["x86_64"] },
          plugin: {
            mode: "simple",
            plugins: [{ name: "internal", allow: ["muon.services.*"] }],
          },
          services: {
            worker,
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
      await shell("input", "keyevent", "KEYCODE_WAKEUP");
      await expect
        .poll(
          async () => {
            if ((await userState()) === "RUNNING_UNLOCKED") return true;
            await shell("wm", "dismiss-keyguard");
            await shell(
              "uiautomator",
              "dump",
              "/data/local/tmp/muon-keyguard.xml",
            );
            const view = await shell(
              "cat",
              "/data/local/tmp/muon-keyguard.xml",
            );
            if (view.includes("com.android.systemui:id/pinEntry")) {
              // SystemUI can recreate the PIN view during boot. Clear a partial entry
              // before entering the credential and observe Android's actual user state.
              await shell(
                "input",
                "keyevent",
                ...Array.from(pin, () => "KEYCODE_DEL"),
                ...Array.from(pin, (digit) => "KEYCODE_" + digit),
                "KEYCODE_ENTER",
              );
            }
            return (await userState()) === "RUNNING_UNLOCKED";
          },
          { timeout: 60000 },
        )
        .toBe(true);
      await shell("cmd", "activity", "wait-for-broadcast-barrier");
    };
    const listed = await shell("pm", "list", "packages", app);
    if (listed.split("\n").includes("package:" + app))
      await adb("uninstall", app);
    await adb("install", await buildApk(false));
    try {
      expect(
        await page(`await muon.services.setAutostart('disabled',false);
        await muon.services.setAutostart('worker',true);
        return (await muon.services.getState('worker')).autostart;`),
      ).toBe(true);
      await reboot();
      await expect
        .poll(serviceLog, { timeout: 30000 })
        .toContain("boot-test-start worker ");
      expect(await serviceLog()).not.toContain("boot-test-start disabled ");
      expect(await shell("dumpsys", "activity", "services", app)).toContain(
        "isForeground=true",
      );
      expect(await shell("dumpsys", "activity", "activities")).not.toContain(
        activity,
      );
      expect((await shell("ps", "-A", "-o", "NAME")).split("\n")).not.toContain(
        app,
      );
      expect(
        await page(`const handle=await muon.services.acquire('worker');
        try { return await handle.call('echo',['after boot']); } finally { await handle.release(); }`),
      ).toBe("after boot");
      expect(
        (await serviceLog()).match(/boot-test-start worker /gu),
      ).toHaveLength(1);
      await page(
        "await muon.services.setAutostart('worker',false); return true;",
      );
      // The saved OFF also wins over a new ON default in an application update.
      await adb("install", "-r", await buildApk(true));
      await reboot();
      expect(await shell("dumpsys", "activity", "services", app)).not.toContain(
        "isForeground=true",
      );
      expect(await serviceLog()).not.toContain("boot-test-start");
      expect(
        await page(
          "return (await muon.services.getState('disabled')).autostart;",
        ),
      ).toBe(false);
    } catch (error) {
      const diagnostic = await Promise.all([
        shell("dumpsys", "package", app),
        shell("dumpsys", "activity", "broadcasts", app),
        adb("logcat", "-b", "system", "-d", "-t", "4000"),
      ]);
      await writeFile(
        join(root, "boot-diagnostics.log"),
        diagnostic.join("\n"),
      );
      console.error("Boot test diagnostics:", root);
      throw error;
    } finally {
      await adb("uninstall", app);
    }
  },
  360000,
);
