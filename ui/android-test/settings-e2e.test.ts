// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile } from "node:child_process";
import { mkdir, writeFile } from "node:fs/promises";
import { resolve } from "node:path";
import { promisify } from "node:util";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { InMemoryTransport } from "@modelcontextprotocol/sdk/inMemory.js";
import { createConnection } from "@playwright/mcp";
import { expect, it } from "vitest";

const execute = promisify(execFile);

it.skipIf(process.env.MUON_ANDROID_SETTINGS_E2E !== "1")(
  "opens Android notification settings and observes changed permission through Playwright MCP",
  async () => {
    const serial = process.env.ANDROID_SERIAL;
    if (!serial)
      throw new Error("ANDROID_SERIAL must identify the test device");
    const output = resolve(".run/android-settings");
    await mkdir(output, { recursive: true });
    const adb = async (...args: string[]): Promise<string> =>
      (await execute("adb", ["-s", serial, ...args])).stdout;
    await adb(
      "install",
      "-r",
      resolve(
        "../core/android-test/android/app/build/outputs/apk/debug/app-debug.apk",
      ),
    );
    await adb(
      "shell",
      "pm",
      "grant",
      "dev.muon.testhost",
      "android.permission.POST_NOTIFICATIONS",
    );
    await adb("shell", "am", "force-stop", "dev.muon.testhost");
    await adb(
      "shell",
      "am",
      "start",
      "-W",
      "-f",
      "0x10008000",
      "-n",
      "dev.muon.testhost/dev.muon.runtime.MuonActivity",
    );
    const pid = (await adb("shell", "pidof", "dev.muon.testhost")).trim();
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
      name: "muon-android-settings-test",
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
      const result = await client.callTool({ name, arguments: args });
      const content = result.content as Array<{ type: string; text?: string }>;
      const text = content
        .filter((item) => item.type === "text")
        .map((item) => item.text ?? "")
        .join("\n");
      if (result.isError) throw new Error(text);
      return text;
    };
    try {
      await mcp.connect(serverTransport);
      await client.connect(clientTransport);
      const click = async (label: string): Promise<void> => {
        await call("browser_click", {
          target: `button:text-is("${label}")`,
        });
      };
      await click("Read notification permission");
      await call("browser_evaluate", {
        function: `async () => {
          const state = await window.muon.android.notifications.getPermissionState();
          if (state.runtimePermission !== 'granted' || !state.notificationsEnabled) throw new Error(JSON.stringify(state));
          return state;
        }`,
      });
      await click("Check settings availability");
      await call("browser_evaluate", {
        function: `async () => {
          for (const target of ['app-details', 'notifications']) {
            if (!await window.muon.android.settings.isAvailable(target)) throw new Error('Unavailable: ' + target);
          }
          return 'Both app settings screens are available';
        }`,
      });
      await click("Open notification settings");
      await adb(
        "shell",
        "uiautomator",
        "dump",
        "/sdcard/muon-notification-settings.xml",
      );
      const xml = await adb(
        "shell",
        "cat",
        "/sdcard/muon-notification-settings.xml",
      );
      await writeFile(resolve(output, "settings.xml"), xml);
      const screenshot = await execute(
        "adb",
        ["-s", serial, "exec-out", "screencap", "-p"],
        { encoding: "buffer" },
      );
      await writeFile(
        resolve(output, "settings-screen.png"),
        screenshot.stdout,
      );
      expect(xml).toContain("com.android.settings");
      const switches = [
        ...xml.matchAll(/<node\b[^>]*class="android.widget.Switch"[^>]*>/gu),
      ];
      expect(switches).toHaveLength(1);
      const node = switches[0]![0];
      expect(node).toContain('checked="true"');
      const bounds = /bounds="\[(\d+),(\d+)\]\[(\d+),(\d+)\]"/u.exec(node)!;
      await adb(
        "shell",
        "input",
        "tap",
        String((Number(bounds[1]) + Number(bounds[3])) / 2),
        String((Number(bounds[2]) + Number(bounds[4])) / 2),
      );
      await adb(
        "shell",
        "uiautomator",
        "dump",
        "/sdcard/muon-notification-settings-disabled.xml",
      );
      const disabledXml = await adb(
        "shell",
        "cat",
        "/sdcard/muon-notification-settings-disabled.xml",
      );
      expect(disabledXml).toMatch(
        /class="android.widget.Switch"[^>]*checked="false"/u,
      );
      await writeFile(resolve(output, "disabled.xml"), disabledXml);
      // Android may terminate the app when notification permission is revoked.
      // A fresh process must obtain the changed OS value, not cached state.
      await adb("shell", "am", "force-stop", "dev.muon.testhost");
      await adb(
        "shell",
        "am",
        "start",
        "-W",
        "-f",
        "0x10008000",
        "-n",
        "dev.muon.testhost/dev.muon.runtime.MuonActivity",
      );
      const newPid = (await adb("shell", "pidof", "dev.muon.testhost")).trim();
      await expect
        .poll(async () => await adb("shell", "cat", "/proc/net/unix"), {
          timeout: 60000,
        })
        .toContain(`webview_devtools_remote_${newPid}`);
      await client.close();
      await mcp.close();
      await adb(
        "forward",
        `tcp:${port}`,
        `localabstract:webview_devtools_remote_${newPid}`,
      );
      client = new Client({ name: "muon-android-settings-test", version: "1" });
      mcp = await createConnection({
        browser: { cdpEndpoint: `http://127.0.0.1:${port}` },
        outputDir: output,
      });
      const [nextClient, nextServer] = InMemoryTransport.createLinkedPair();
      await mcp.connect(nextServer);
      await client.connect(nextClient);
      await click("Read notification permission");
      const changed = await call("browser_evaluate", {
        function: `async () => {
          const state = await window.muon.android.notifications.getPermissionState();
          if (state.runtimePermission !== 'denied' || state.notificationsEnabled) throw new Error(JSON.stringify(state));
          return state;
        }`,
      });
      await click("Open notification settings");
      await adb(
        "shell",
        "uiautomator",
        "dump",
        "/sdcard/muon-notification-settings-return.xml",
      );
      const returnXml = await adb(
        "shell",
        "cat",
        "/sdcard/muon-notification-settings-return.xml",
      );
      const returnSwitch =
        /<node\b[^>]*class="android.widget.Switch"[^>]*>/u.exec(returnXml)![0];
      expect(returnSwitch).toContain('checked="false"');
      const returnBounds = /bounds="\[(\d+),(\d+)\]\[(\d+),(\d+)\]"/u.exec(
        returnSwitch,
      )!;
      await adb(
        "shell",
        "input",
        "tap",
        String((Number(returnBounds[1]) + Number(returnBounds[3])) / 2),
        String((Number(returnBounds[2]) + Number(returnBounds[4])) / 2),
      );
      await adb("shell", "input", "keyevent", "4");
      expect((await adb("shell", "pidof", "dev.muon.testhost")).trim()).toBe(
        newPid,
      );
      const resumed = await call("browser_evaluate", {
        function: `async () => {
          const output = document.querySelector('#notification-permission-state');
          return await new Promise(resolve => {
            const read = () => {
              try {
                const state = JSON.parse(output.textContent);
                return state.runtimePermission === 'granted' && state.notificationsEnabled ? state : undefined;
              } catch { return undefined; }
            };
            const current = read();
            if (current) { resolve(current); return; }
            const observer = new MutationObserver(() => {
              const state = read();
              if (state) { observer.disconnect(); resolve(state); }
            });
            observer.observe(output, { childList: true, characterData: true, subtree: true });
          });
        }`,
      });
      await writeFile(
        resolve(output, "result.txt"),
        [changed, resumed].join("\n"),
      );
    } finally {
      await client.close();
      await mcp.close();
      await adb("forward", "--remove", `tcp:${port}`);
      await adb("shell", "am", "force-stop", "dev.muon.testhost");
    }
  },
  180000,
);
