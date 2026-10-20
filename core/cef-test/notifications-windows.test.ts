// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { randomUUID } from "node:crypto";
import { execFile } from "node:child_process";
import { mkdir, readFile, writeFile } from "node:fs/promises";
import { resolve, join, win32 } from "node:path";
import { promisify } from "node:util";
import type { RemoteAgent, RemoteManagedProcess } from "agent-rover";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { InMemoryTransport } from "@modelcontextprotocol/sdk/inMemory.js";
import { createConnection } from "@playwright/mcp";
import { afterAll, beforeAll, describe, expect, it, vi } from "vitest";
import { createMuonCapabilityModuleResolver } from "../../ui/common/capability.js";
import { resolveMuonCefNotificationFunctionPaths } from "../../ui/cef/notifications.js";
import { resolveWindowsE2eEnvironment } from "../../scripts/windows-e2e-environment.js";
import { connectWindowsAgent } from "./plugin-e2e/windows-agent.js";
import { resolveWindowsRuntimeTarget } from "./plugin-e2e/windows-matrix.js";
import { discoverWindowsTestNetwork } from "./plugin-e2e/windows-network.js";
import { stageWindowsRuntimeDirectory } from "./plugin-e2e/windows-staging.js";
import { runNotificationDesktopCase } from "./notifications-desktop-case.js";
import { createWindowsNotificationDriver } from "./notifications-windows-accessibility.js";
import { runWindowsNotificationLaunchCase } from "./notifications-windows-launch-case.js";
import type { MuonNotificationEvent } from "../common/renderer/notifications.js";

const target = resolveWindowsRuntimeTarget(process.env.MUON_E2E_WINDOWS_TARGET);
const environment = resolveWindowsE2eEnvironment(process.env);
const remoteBase = win32.join(
  environment.workDir,
  "notifications",
  target.target,
);
const runtime = win32.join(remoteBase, "runtime");
const root = win32.join(remoteBase, `run-${process.pid}`);
const relayPath = win32.join(remoteBase, "muon-cdp-relay.exe");
const cdpPort = 19387;
let agent: RemoteAgent | undefined;
let cdpHost = "";

beforeAll(async () => {
  // Package the cold-launch consumer through the actual CLI, built before
  // acquiring the limited Windows session.
  await promisify(execFile)(
    "npm",
    ["run", "build:js", "--workspace", "muon-ui"],
    {
      cwd: resolve("../.."),
      maxBuffer: 16 * 1024 * 1024,
    },
  );
  const connection = await connectWindowsAgent(environment, target);
  if (connection.status !== "connected") throw new Error(connection.reason);
  agent = connection.agent;
  // The runtime build also prepares the host PE resource updater.
  await promisify(execFile)(
    process.execPath,
    ["scripts/stage-muon-builder.mjs"],
    { cwd: resolve("../../ui"), maxBuffer: 16 * 1024 * 1024 },
  );
  cdpHost = (await discoverWindowsTestNetwork(agent)).cdpHost;
  await stageWindowsRuntimeDirectory(
    agent,
    resolve("../..", target.releaseRuntimeDirectory),
    runtime,
  );
  await agent.files.writeFile(
    relayPath,
    await readFile(
      resolve(".run/windows-cdp-relay", target.target, "muon-cdp-relay.exe"),
    ),
  );
  await agent.files.mkdir(root, { recursive: true });
  const native = win32.join(root, "native");
  await agent.files.mkdir(native, { recursive: true });
  for (const name of [
    "libcardio.dll",
    "libstdc++-6.dll",
    target.target === "windows-amd64"
      ? "libgcc_s_seh-1.dll"
      : "libgcc_s_dw2-1.dll",
  ]) {
    await agent.files.writeFile(
      win32.join(native, name),
      await readFile(resolve("../..", target.releaseRuntimeDirectory, name)),
    );
  }
  await agent.files.writeFile(
    win32.join(native, "muon.png"),
    await readFile(resolve("../../images/muon-256.png")),
  );
  for (const name of ["store", "service", "maintenance", "windows_driver"]) {
    const executable = `muon_notification_${name}${name === "windows_driver" ? "" : "_test"}.exe`;
    await agent.files.writeFile(
      win32.join(native, executable),
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

describe(`Windows notifications through Playwright MCP (${target.target})`, () => {
  it.each([
    ["store", "muon_notification_store_test.exe", []],
    ["service", "muon_notification_service_test.exe", []],
    [
      "registration",
      "muon_notification_windows_driver.exe",
      ["--missing-registration"],
    ],
    [
      "callback-shutdown",
      "muon_notification_windows_driver.exe",
      ["--callback-shutdown"],
    ],
    [
      "launch-registration",
      "muon_notification_windows_driver.exe",
      ["--launch-registration"],
    ],
    ["maintenance", "muon_notification_maintenance_test.exe", []],
  ] as const)(
    "verifies native %s behavior on Windows",
    async (name, executable, args) => {
      if (agent === undefined) throw new Error("Windows agent is unavailable");
      const native = win32.join(root, "native");
      const process = await agent.processes.launchManaged({
        path: win32.join(native, executable),
        arguments: [...args],
        workingDirectory: native,
        captureStdout: true,
        captureStderr: true,
        createNoWindow: true,
      });
      try {
        const result = await process.waitForExit({ timeoutMs: 30000 });
        const artifacts = resolve(".run/notifications-windows", target.target);
        await mkdir(artifacts, { recursive: true });
        const stdout = await process.stdoutText();
        const stderr = await process.stderrText();
        await writeFile(
          join(artifacts, `native-${name}.json`),
          JSON.stringify(
            {
              exitCode: result.root.exitCode,
              stdout,
              stderr,
            },
            null,
            2,
          ),
        );
        expect(result.root.exitCode, stdout + stderr).toBe(0);
      } finally {
        await process.releaseAsync();
      }
    },
  );

  it.each(["simple", "validate"] as const)(
    "delivers OS interactions and preserves pending events in %s mode",
    async (mode) => {
      if (agent === undefined) throw new Error("Windows agent is unavailable");
      const remote = agent;
      const directory = win32.join(root, mode);
      const assets = win32.join(directory, "assets", "main");
      const artifacts = resolve(
        ".run/notifications-windows",
        target.target,
        mode,
      );
      await mkdir(artifacts, { recursive: true });
      await remote.files.mkdir(assets, { recursive: true });
      const appId = `dev.muon.NotificationTest.${target.target}.${mode}.run${process.pid}`;
      const appName = `Muon Notifications ${target.target} ${mode}`;
      const pendingTitle = `${appId} pending`;
      const popupTitle = `${appId} popup cancellation`;
      const survivingTitle = `${appId} main survives`;
      const iconPath = win32.join(directory, "notification-icon.png");
      await remote.files.writeFile(
        iconPath,
        await readFile(resolve("../../images/muon-256.png")),
      );
      const resolver = createMuonCapabilityModuleResolver(
        "/notification-driver",
        {
          backend: "cef",
          functionPaths: resolveMuonCefNotificationFunctionPaths(
            { enabled: true },
            [target.target],
          ),
          imports: [
            { sources: ["main.ts"], allow: ["muon.notifications.*"] },
            {
              sources: ["readonly.ts"],
              allow: ["muon.notifications.getStatus"],
            },
          ],
        },
      );
      for (const name of ["main", "readonly"]) {
        const id = resolver.resolveId(
          "muon:notifications",
          `/notification-driver/${name}.ts`,
        )!.id;
        await remote.files.writeFile(
          win32.join(assets, `${name}.mjs`),
          Buffer.from(resolver.load(id)!),
        );
      }
      const title = `Notification driver ${mode}`;
      const html = `<!doctype html><meta charset="utf-8">
      <script>globalThis.pageReady = new Promise(resolve => globalThis.finishPageReady = resolve);</script>
      <title>${title}</title>
      <h1>${appName}</h1><pre id="events"></pre><script type="module">
      globalThis.notificationApi = ${mode === "validate" ? "await import('./main.mjs')" : "window.muon.notifications"};
      globalThis.notificationEvents = [];
      globalThis.beginWatch = async () => {
        globalThis.nextEvent = new Promise(resolve => globalThis.receiveEvent = resolve);
        globalThis.watcher = await notificationApi.watchEvents(event => {
          notificationEvents.push(event); document.querySelector('#events').textContent = JSON.stringify(notificationEvents); receiveEvent(event);
        });
      };
      globalThis.ready = true; finishPageReady();
      </script>`;
      for (const page of ["index.html", "popup.html"])
        await remote.files.writeFile(
          win32.join(assets, page),
          Buffer.from(html),
        );
      const configPath = win32.join(directory, "muon.json");
      const config = {
        launcher: {
          appId,
          notificationRegistration: {
            displayName: appName,
            iconPath,
            activatorClsid: `{${randomUUID()}}`,
          },
        },
        notifications: { enabled: true, defaultCategory: "orders" },
        asset: { sourcePath: win32.dirname(assets) },
        browser: {
          startPage: "asset://main/index.html",
          profilePath: win32.join(directory, "profile"),
          titleBarType: "native",
        },
        network: { allow: ["asset://main/**"] },
        plugin: {
          ...(mode === "simple" ? { mode } : resolver.getRuntimePluginConfig()),
          pages: ["asset://main/**"],
          plugins: [{ name: "internal", allow: ["muon.notifications.*"] }],
        },
        cdp: { enable: true, port: cdpPort },
      };
      await remote.files.writeFile(
        configPath,
        Buffer.from(JSON.stringify(config)),
      );
      let application: RemoteManagedProcess | undefined;
      let relay: RemoteManagedProcess | undefined;
      let closeMcp: (() => Promise<void>) | undefined;
      let driver:
        Awaited<ReturnType<typeof createWindowsNotificationDriver>> | undefined;
      const client = new Client({
        name: "muon-windows-notifications",
        version: "1.0.0",
      });
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
        const endpoint = `http://${cdpHost}:${relayPort}`;
        await vi.waitUntil(
          async () => {
            const snapshot = await application!.rootSnapshot();
            if (!snapshot.running)
              throw new Error(
                `Muon exited ${snapshot.exitCode}: ${await application!.stderrText()}`,
              );
            try {
              const response = await fetch(endpoint + "/json/list", {
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
          browser: { cdpEndpoint: endpoint },
          outputDir: artifacts,
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
            function: `async () => { await globalThis.pageReady; ${source} }`,
          });
          const result = /### Result\n([\s\S]*?)(?:\n### |$)/u.exec(text)?.[1];
          if (result === undefined)
            throw new Error(`No evaluation result: ${text}`);
          return JSON.parse(result) as T;
        };
        expect(await evaluate("return globalThis.ready;")).toBe(true);
        driver = await createWindowsNotificationDriver(remote, appId, appName);
        await driver.setEnabled(true);
        const initialStatus = await evaluate<{ app: { state: string } }>(
          "return await notificationApi.getStatus();",
        );
        expect(["enabled", "unknown"]).toContain(initialStatus.app.state);
        // A freshly registered AUMID may not yet have a Windows notification
        // setting. Observe an actual first post before checking OS disablement.
        const statusTitle = `${appId} status`;
        await evaluate(
          `await notificationApi.post({id:'status-probe',title:${JSON.stringify(statusTitle)},body:'Register this notification source'}); return true;`,
        );
        await driver.waitForNotification(
          statusTitle,
          join(artifacts, "status-probe.png"),
        );
        await evaluate(
          "await notificationApi.cancel('status-probe'); return true;",
        );
        await driver.waitForRemoval(statusTitle);
        await driver.setEnabled(false);
        await vi.waitUntil(
          async () => {
            const status = await evaluate<{ app: { state: string } }>(
              "return await notificationApi.getStatus();",
            );
            return status.app.state === "disabled";
          },
          { timeout: 30000, interval: 100 },
        );
        expect(
          await evaluate(
            "try { await notificationApi.post({id:'disabled',title:'Disabled',body:''}); return ''; } catch(error) { return String(error); }",
          ),
        ).toMatch(/disabled/);
        await driver.setEnabled(true);
        await vi.waitUntil(
          async () => {
            const status = await evaluate<{ app: { state: string } }>(
              "return await notificationApi.getStatus();",
            );
            return status.app.state === "enabled";
          },
          { timeout: 30000, interval: 100 },
        );
        await runNotificationDesktopCase(
          evaluate,
          driver,
          artifacts,
          mode,
          "windows",
        );
        if (mode === "validate") {
          expect(
            await evaluate(
              "return Object.keys(await import('./readonly.mjs'));",
            ),
          ).toEqual(["getStatus"]);
          const capability = resolver
            .getRuntimePluginConfig()
            .capabilities.find(
              (item) =>
                item.allow.length === 1 &&
                item.allow[0] === "muon.notifications.getStatus",
            )!;
          const forbiddenPost = JSON.stringify({
            id: "unauthorized",
            title: `${appId} unauthorized`,
            body: "This valid post must be rejected by its capability",
          });
          expect(
            await evaluate(
              `try { await __muon_plugin_call(${JSON.stringify(capability.id)}, 'muon.notifications.post', [${JSON.stringify(forbiddenPost)}]); return ''; } catch(error) { return String(error); }`,
            ),
          ).toMatch(/capability is not allowed/);
        }
        await evaluate(
          `await beginWatch(); await notificationApi.post({id:'pending',title:${JSON.stringify(pendingTitle)},body:'Reload preserves receipt',data:{pending:true}}); return true;`,
        );
        await driver.activateNotification(
          pendingTitle,
          null,
          join(artifacts, "pending.png"),
        );
        const event = await evaluate<MuonNotificationEvent>(
          "return await nextEvent;",
        );
        await call("browser_navigate", { url: "asset://main/index.html" });
        expect(
          await evaluate("return await notificationApi.getPendingEvents();"),
        ).toEqual([event]);
        await evaluate(
          `await notificationApi.acknowledgeEvent(${JSON.stringify(event.eventId)}); await notificationApi.acknowledgeEvent(${JSON.stringify(event.eventId)}); await beginWatch(); await notificationApi.post({id:'popup-cancel',title:${JSON.stringify(popupTitle)},body:''}); return true;`,
        );
        await driver.waitForNotification(
          popupTitle,
          join(artifacts, "popup-before.png"),
        );
        await evaluate("window.open('asset://main/popup.html'); return true;");
        // Muon's default popup policy isolates the opener. Observe the new
        // browser through MCP instead of requiring cross-window script access.
        const popup = await vi.waitUntil(
          async () => {
            const tabs = await call("browser_tabs", { action: "list" });
            const index = Number(tabs.match(/- (\d+):[^\n]*popup\.html/)?.[1]);
            return Number.isFinite(index) ? index : false;
          },
          { timeout: 30000, interval: 100 },
        );
        await call("browser_tabs", { action: "select", index: popup });
        await vi.waitUntil(
          async () => await evaluate("return globalThis.ready === true;"),
          { timeout: 30000, interval: 100 },
        );
        await evaluate(
          "await beginWatch(); await notificationApi.cancel('popup-cancel'); return true;",
        );
        await driver.waitForRemoval(popupTitle);
        await call("browser_tabs", { action: "close", index: popup });
        await call("browser_tabs", { action: "select", index: 0 });
        await evaluate(
          `await notificationApi.post({id:'main-survives',title:${JSON.stringify(survivingTitle)},body:'',data:{main:true}}); return true;`,
        );
        await driver.activateNotification(
          survivingTitle,
          null,
          join(artifacts, "main-survives.png"),
        );
        expect(await evaluate("return await nextEvent;")).toMatchObject({
          id: "main-survives",
          data: { main: true },
        });
        await evaluate(
          "for (const event of await notificationApi.getPendingEvents()) await notificationApi.acknowledgeEvent(event.eventId); await watcher.close(); await watcher.close(); return true;",
        );
        expect(
          await evaluate("return await notificationApi.getPendingEvents();"),
        ).toEqual([]);
        await writeFile(
          join(artifacts, "lifecycle.json"),
          JSON.stringify(
            {
              event,
              initialStatus,
              reload: true,
              popupCancellation: true,
              mainSurvives: true,
            },
            null,
            2,
          ),
        );
        await call("browser_tabs", { action: "close" });
        expect(
          (await application.waitForExit({ timeoutMs: 30000 })).root.exitCode,
          await application.stderrText(),
        ).toBe(0);
        await client.close();
        await closeMcp();
        closeMcp = undefined;
        await driver.close();
        driver = undefined;
        await runWindowsNotificationLaunchCase(remote, {
          target,
          runtime,
          artifacts,
          endpoint,
          title,
          cdpPort,
          mode,
        });
      } finally {
        try {
          await client.close();
          await closeMcp?.();
          if (application !== undefined)
            await writeFile(
              join(artifacts, "runtime.log"),
              (await application.stdoutText()) +
                (await application.stderrText()),
            );
        } finally {
          try {
            await application?.releaseAsync();
          } finally {
            try {
              await relay?.releaseAsync();
            } finally {
              await driver?.close();
            }
          }
        }
      }
    },
    300000,
  );
});
