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

it.skipIf(process.env.MUON_ANDROID_SYSTEM_BARS_E2E !== "1")(
  "operates Android system bars and positions the driver through Playwright MCP",
  async () => {
    const serial = process.env.ANDROID_SERIAL;
    if (!serial)
      throw new Error("ANDROID_SERIAL must identify the test device");
    const output = resolve(".run/android-system-bars");
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
    await adb("shell", "am", "force-stop", "dev.muon.testhost");
    await adb(
      "shell",
      "am",
      "start",
      "-W",
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
    const client = new Client({
      name: "muon-android-system-bars-test",
      version: "1",
    });
    const mcp = await createConnection({
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
      const waitForInsets = async (predicate: string): Promise<void> => {
        await call("browser_evaluate", {
          function: `async () => {
            const output = document.querySelector('#system-bars-insets');
            await new Promise(resolve => {
              const matches = () => {
                try { return (${predicate})(JSON.parse(output.textContent)); } catch { return false; }
              };
              if (matches()) { resolve(); return; }
              const observer = new MutationObserver(() => {
                if (matches()) { observer.disconnect(); resolve(); }
              });
              observer.observe(output, { childList: true, characterData: true, subtree: true });
            });
          }`,
        });
      };
      await click("Start insets updates");
      await click("Draw edge to edge");
      await waitForInsets("value => value.contentLayout === 'edge-to-edge'");
      const edge = await call("browser_evaluate", {
        function: `async () => {
          const insets = await window.muon.android.systemBars.getInsets();
          const marker = document.querySelector('#safe-area-marker').getBoundingClientRect();
          const css = parseFloat(getComputedStyle(document.documentElement).getPropertyValue('--muon-safe-area-bottom'));
          const result = { bottom: insets.contentSafeArea.bottom, css, clearance: window.innerHeight - marker.bottom };
          if (Math.abs(result.bottom - result.css) > 1 || Math.abs(result.bottom - result.clearance) > 1)
            throw new Error('Edge content clearance does not match native CSS insets: ' + JSON.stringify(result));
          return result;
        }`,
      });
      await click("Use safe area");
      await waitForInsets("value => value.contentLayout === 'safe-area'");
      const safe = await call("browser_evaluate", {
        function: `async () => {
          const insets = await window.muon.android.systemBars.getInsets();
          const css = getComputedStyle(document.documentElement).getPropertyValue('--muon-safe-area-bottom');
          if (Object.values(insets.contentSafeArea).some(value => value !== 0) || parseFloat(css) !== 0)
            throw new Error('Safe area is being applied twice: ' + JSON.stringify(insets));
          return insets;
        }`,
      });
      for (const visible of [false, true]) {
        await click(visible ? "Show status bar" : "Hide status bar");
        await waitForInsets(
          `value => (value.systemBars.top > 0) === ${visible}`,
        );
        const state = await call("browser_evaluate", {
          function: `async () => {
            const state = await window.muon.android.systemBars.getState();
            if (state.actual.statusBar.visible !== ${visible}) throw new Error(JSON.stringify(state));
            return 'visibility verified';
          }`,
        });
        expect(state).toContain("visibility verified");
      }
      await click("Stop insets updates");
      await writeFile(resolve(output, "result.txt"), [edge, safe].join("\n"));
      await call("browser_take_screenshot", {
        filename: resolve(output, "system-bars.png"),
        type: "png",
      });
    } finally {
      await client.close();
      await mcp.close();
      await adb("forward", "--remove", `tcp:${port}`);
      await adb("shell", "am", "force-stop", "dev.muon.testhost");
    }
  },
  180000,
);
