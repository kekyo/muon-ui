// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import {
  mkdir,
  mkdtemp,
  readFile,
  rm,
  symlink,
  writeFile,
} from "node:fs/promises";
import { homedir, tmpdir } from "node:os";
import { basename, join, resolve, win32 } from "node:path";
import { pathToFileURL } from "node:url";
import { execFile } from "node:child_process";
import { promisify } from "node:util";
import type { RemoteAgent, RemoteManagedProcess } from "agent-rover";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { InMemoryTransport } from "@modelcontextprotocol/sdk/inMemory.js";
import { createConnection } from "@playwright/mcp";
import { expect } from "vitest";
import type { MuonPackResult } from "../../ui/common/pack-types.js";
import type { JsonObject } from "../../ui/common/build-config.js";
import type { MuonNotificationEvent } from "../common/renderer/notifications.js";
import {
  createWindowsNotificationDriver,
  type WindowsNotificationDriver,
} from "./notifications-windows-accessibility.js";
import type { WindowsRuntimeTarget } from "./plugin-e2e/windows-matrix.js";

/** Prepared CEF fixture and identity used for actual COM process activation. */
export interface WindowsNotificationLaunchFixture {
  /** Architecture and local runtime build paths. */
  target: WindowsRuntimeTarget;
  /** Remote prepared runtime directory, dedicated to this test suite. */
  runtime: string;
  /** Local evidence output directory. */
  artifacts: string;
  /** Existing relay endpoint, retained across application restarts. */
  endpoint: string;
  /** Expected title of the driver's main window. */
  title: string;
  /** Local CDP port already exposed by the test's relay. */
  cdpPort: number;
  /** Public API mode compiled by the packaged application's Vite plugin. */
  mode: "simple" | "validate";
}

const quote = (value: string): string => `'${value.replaceAll("'", "''")}'`;
const execute = promisify(execFile);

/**
 * Verifies durable activation through the real launcher and Windows COM server.
 * @param agent Existing exclusive Windows session.
 * @param fixture Prepared runtime, configuration and evidence locations.
 * @returns After running/default/action activation and process restart checks.
 * @remarks All CEF page operations use Playwright MCP. The caller must close its
 * previous CEF instance and MCP connection first.
 */
export const runWindowsNotificationLaunchCase = async (
  agent: RemoteAgent,
  fixture: WindowsNotificationLaunchFixture,
): Promise<void> => {
  const { runtime, artifacts, target, mode, endpoint, title } = fixture;
  const project = await mkdtemp(join(tmpdir(), "muon-notification-nsis-"));
  let install = win32.join(
    win32.dirname(runtime),
    `installed-${mode}-${process.pid} 日本語`,
  );
  const installations = [install];
  const installer = win32.join(
    win32.dirname(runtime),
    `cold-${mode}-${process.pid}-setup.exe`,
  );
  let launcher = "";
  let appId = "";
  let clsid = "";
  let appState = "";
  let installed = false;
  let driver: WindowsNotificationDriver | undefined;
  const commands: RemoteManagedProcess[] = [];
  let client: Client | undefined;
  let closeMcp: (() => Promise<void>) | undefined;
  let currentPid: number | undefined;
  const powershell = async (script: string): Promise<string> => {
    const process = await agent.processes.launchManaged({
      path: "powershell.exe",
      arguments: [
        "-NoProfile",
        "-NonInteractive",
        "-EncodedCommand",
        Buffer.from(
          `$ErrorActionPreference='Stop';[Console]::OutputEncoding=[System.Text.UTF8Encoding]::new($false);${script}`,
          "utf16le",
        ).toString("base64"),
      ],
      captureStdout: true,
      captureStderr: true,
      createNoWindow: true,
    });
    try {
      const result = await process.waitForExit({ timeoutMs: 30000 });
      if (result.root.exitCode !== 0)
        throw new Error(await process.stderrText());
      return (await process.stdoutText()).trim();
    } finally {
      await process.releaseAsync();
    }
  };
  const call = async (
    name: string,
    args: Record<string, unknown>,
  ): Promise<string> => {
    if (client === undefined)
      throw new Error("Notification MCP is disconnected");
    const result = await client.callTool({ name, arguments: args });
    const text = (result.content as { type: string; text?: string }[])
      .filter((entry) => entry.type === "text")
      .map((entry) => entry.text ?? "")
      .join("\n");
    if (result.isError) throw new Error(text);
    return text;
  };
  const evaluate = async <T>(source: string): Promise<T> => {
    const text = await call("browser_evaluate", {
      function: `async () => { await globalThis.pageReady; ${source} }`,
    });
    const result = /### Result\n([\s\S]*?)(?:\n### |$)/u.exec(text)?.[1];
    if (result === undefined) throw new Error(`Missing MCP result: ${text}`);
    return JSON.parse(result) as T;
  };
  const disconnect = async (): Promise<void> => {
    await client?.close();
    await closeMcp?.();
    client = undefined;
    closeMcp = undefined;
  };
  const connect = async (): Promise<number> => {
    const window = await agent.waitForWindow(
      { title, visible: true },
      { timeoutMs: 60000 },
    );
    currentPid = window.process.id;
    const mcp = await createConnection({
      browser: { cdpEndpoint: endpoint },
      outputDir: artifacts,
    });
    closeMcp = async () => await mcp.close();
    client = new Client({ name: "muon-windows-cold-launch", version: "1.0.0" });
    const [a, b] = InMemoryTransport.createLinkedPair();
    await mcp.connect(b);
    await client.connect(a);
    expect(await evaluate("return globalThis.ready;")).toBe(true);
    return window.process.id;
  };
  const start = async (): Promise<number> => {
    const process = await agent.processes.launchManaged({
      path: launcher,
      workingDirectory: install,
      captureStdout: true,
      captureStderr: true,
    });
    commands.push(process);
    return await connect();
  };
  const stop = async (): Promise<void> => {
    const pid = currentPid;
    await call("browser_tabs", { action: "close" });
    await disconnect();
    if (pid !== undefined)
      await agent.processes.waitForExit(pid, { timeoutMs: 30000 });
    currentPid = undefined;
  };
  try {
    await mkdir(artifacts, { recursive: true });
    const packageDirectory = join(project, "package-dist");
    const packageName = `muon-notification-cold-${mode}-${process.pid}`;
    const displayName = `Muon Notifications cold ${target.target} ${mode}`;
    for (const directory of ["runtime", "native"]) {
      await mkdir(join(packageDirectory, directory), { recursive: true });
      await symlink(
        resolve("../..", target.releaseRuntimeDirectory),
        join(packageDirectory, directory, target.target),
        "dir",
      );
    }
    await writeFile(
      join(project, "package.json"),
      JSON.stringify({
        name: packageName,
        version: "1.0.0",
        type: "module",
        author: "muon Tester",
      }),
    );
    await writeFile(
      join(project, "index.html"),
      `<!doctype html><meta charset="utf-8">
<script>globalThis.pageReady = new Promise(resolve => globalThis.finishPageReady = resolve);</script>
<title>${title}</title><h1>${displayName}</h1><pre id="events"></pre>
<script type="module" src="/main.ts"></script>`,
    );
    await writeFile(
      join(project, "main.ts"),
      `
globalThis.notificationApi = ${mode === "validate" ? "await import('muon:notifications')" : "window.muon.notifications"};
globalThis.notificationEvents = [];
globalThis.beginWatch = async () => {
  globalThis.nextEvent = new Promise(resolve => globalThis.receiveEvent = resolve);
  globalThis.watcher = await notificationApi.watchEvents(event => {
    notificationEvents.push(event);
    document.querySelector('#events').textContent = JSON.stringify(notificationEvents);
    receiveEvent(event);
  });
};
globalThis.ready = true; finishPageReady();
`,
    );
    const config = {
      launcher: { defaultVersionPolicy: "tested" },
      notifications: { enabled: true, defaultCategory: "orders" },
      browser: { titleBarType: "native" },
      network: { allow: ["asset://main/**"] },
      cdp: { enable: true, port: fixture.cdpPort },
      plugin: {
        mode,
        plugins: [
          {
            name: "internal",
            ...(mode === "simple"
              ? { allow: ["muon.notifications.*"] }
              : {
                  imports: [
                    { sources: ["main.ts"], allow: ["muon.notifications.*"] },
                  ],
                }),
          },
        ],
      },
    };
    await writeFile(join(project, "muon.json"), JSON.stringify(config));
    await writeFile(
      join(project, "vite.config.mjs"),
      `
import muon from ${JSON.stringify(pathToFileURL(resolve("../../ui/dist/vite.mjs")).href)};
export default { build: {outDir:'web-dist',target:'es2022'}, plugins:[muon({
  ${mode === "simple" ? "pluginAccess:false," : ""}
  build:{targets:[${JSON.stringify(target.target)}],packageDirectory:${JSON.stringify(packageDirectory)}}
})] };
`,
    );
    const pack = async (version: string): Promise<MuonPackResult> => {
      const result = await execute(
        process.execPath,
        [
          resolve("../../ui/dist/cli.cjs"),
          "pack",
          "--json",
          "--package-directory",
          packageDirectory,
          "--target",
          target.target,
          "--type",
          "nsis",
          "--app-id",
          `dev.muon.notificationtest.cold.${mode}.run${process.pid}`,
          "--name",
          "notification-launcher",
          "--package-name",
          packageName,
          "--package-version",
          version,
          "--author",
          "muon Tester",
          "--icon",
          resolve("../../images/muon-256.png"),
          "--windows-product-name",
          displayName,
        ],
        { cwd: project, maxBuffer: 16 * 1024 * 1024 },
      );
      await writeFile(join(artifacts, `pack-${version}.log`), result.stderr);
      return JSON.parse(result.stdout) as MuonPackResult;
    };
    const packed = await pack("1.0.0");
    const built = packed.targets[0]!;
    if (built.target === "android")
      throw new Error("Expected a Windows NSIS package");
    appId = built.runtimeAppId;
    clsid = (
      (built.embeddedConfig.launcher as JsonObject)
        .notificationRegistration as JsonObject
    ).activatorClsid as string;
    launcher = win32.join(install, basename(built.launcherPath));
    await agent.files.writeFile(
      installer,
      await readFile(packed.artifacts[0]!.path),
    );
    installed = true;
    await powershell(
      `$app=Start-Process -FilePath ${quote(installer)} -ArgumentList ${quote(`/S /D=${install}`)} -Wait -PassThru; if($app.ExitCode -ne 0){throw ('NSIS install failed: '+$app.ExitCode)}`,
    );
    appState = await powershell(
      `Write-Output (Join-Path $env:LOCALAPPDATA ${quote(appId)})`,
    );
    driver = await createWindowsNotificationDriver(agent, appId, displayName);
    await driver.setEnabled(true);
    const updated = await pack("1.0.1");
    await agent.files.writeFile(
      installer,
      await readFile(updated.artifacts[0]!.path),
    );
    // COM launches outside the test process environment. Seed the ordinary
    // per-user builder cache so the genuine launcher can prepare offline.
    const cache = await powershell(
      "Write-Output (Join-Path $env:LOCALAPPDATA 'muon')",
    );
    const cmake = await readFile(
      resolve(`../cef/.build/dist/${target.target}/release/CMakeCache.txt`),
      "utf8",
    );
    const archive = /^MUON_CEF_PACKAGE:STRING=(.+)$/m.exec(cmake)?.[1]?.trim();
    if (!archive)
      throw new Error("The Windows launcher distribution is not built");
    const remoteArchive = win32.join(cache, "artifacts", "cef", archive);
    if (!(await agent.files.exists(remoteArchive))) {
      const localCache =
        process.env.MUON_CACHE_DIR ??
        (process.env.XDG_CACHE_HOME
          ? join(process.env.XDG_CACHE_HOME, "muon")
          : join(homedir(), ".cache", "muon"));
      await agent.files.mkdir(win32.dirname(remoteArchive), {
        recursive: true,
      });
      await agent.files.writeFile(
        remoteArchive,
        await readFile(join(localCache, "artifacts", "cef", archive)),
      );
    }
    const initialPid = await start();
    expect(
      await evaluate(
        "return (await notificationApi.getCapabilities()).launch;",
      ),
    ).toMatchObject({ state: "supported" });
    expect(
      await evaluate(
        "return (await notificationApi.getCapabilities()).persistence;",
      ),
    ).toEqual({ state: "supported", reason: null });
    const runningTitle = `${appId} running activation`;
    await evaluate(
      `await beginWatch(); await notificationApi.post({id:'running-activation',title:${JSON.stringify(runningTitle)},body:'Restore the existing window'}); return true;`,
    );
    const initialWindow = await agent.waitForWindow({
      title,
      processId: initialPid,
    });
    await initialWindow.minimize();
    await driver.activateNotification(
      runningTitle,
      null,
      join(artifacts, "running-activation.png"),
    );
    const restored = await agent.waitForWindow(
      { title, processId: initialPid, active: true },
      { timeoutMs: 30000 },
    );
    expect(restored.minimized).toBe(false);
    const runningEvent = await evaluate<MuonNotificationEvent>(
      "return await nextEvent;",
    );
    await evaluate(
      `await notificationApi.acknowledgeEvent(${JSON.stringify(runningEvent.eventId)}); return true;`,
    );
    const events: MuonNotificationEvent[] = [];
    for (const action of [null, "default"] as const) {
      const notificationTitle = `${appId} cold ${action ?? "activate"}`;
      const id = `cold-${action ?? "activate"}`;
      const oldPid = currentPid;
      await evaluate(
        `await notificationApi.post({id:${JSON.stringify(id)},title:${JSON.stringify(notificationTitle)},body:'The process exits before this interaction',data:{cold:true,action:${JSON.stringify(action)}},actions:[{id:'default',title:'View order'}]}); return true;`,
      );
      await driver.waitForNotification(
        notificationTitle,
        join(artifacts, `${id}-before-exit.png`),
      );
      await stop();
      const previousInstall = install;
      if (action !== null) {
        install = win32.join(
          win32.dirname(runtime),
          `relocated-${mode}-${process.pid} 日本語`,
        );
        installations.push(install);
        launcher = win32.join(install, basename(built.launcherPath));
      }
      await powershell(
        `$app=Start-Process -FilePath ${quote(installer)} -ArgumentList ${quote(`/S /D=${install}`)} -Wait -PassThru; if($app.ExitCode -ne 0){throw ('NSIS update failed: '+$app.ExitCode)}`,
      );
      if (action !== null) {
        await powershell(
          `$app=Start-Process -FilePath ${quote(win32.join(previousInstall, "Uninstall.exe"))} -ArgumentList ${quote(`/S _?=${previousInstall}`)} -Wait -PassThru; if($app.ExitCode -ne 0){throw ('Old NSIS uninstall failed: '+$app.ExitCode)}`,
        );
        expect(
          await agent.files.exists(
            win32.join(previousInstall, basename(built.launcherPath)),
          ),
        ).toBe(false);
      }
      const registration = JSON.parse(
        await powershell(`
$root=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser,[Microsoft.Win32.RegistryView]::Registry64)
try {
  $key=$root.OpenSubKey(${quote(`Software\\Classes\\CLSID\\${clsid}\\LocalServer32`)})
  try { @{ appId=$key.GetValue('MuonAppId'); executable=$key.GetValue('ServerExecutable'); command=$key.GetValue('') } | ConvertTo-Json }
  finally {$key.Dispose()}
} finally {$root.Dispose()}
`),
      ) as { appId: string; executable: string; command: string };
      await writeFile(
        join(artifacts, `${id}-registration.json`),
        JSON.stringify(registration, null, 2),
      );
      expect(registration).toEqual({
        appId,
        executable: launcher,
        command: `"${launcher}" --muon-notification-activation`,
      });
      await driver.waitForNotification(
        notificationTitle,
        join(artifacts, `${id}-after-exit.png`),
      );
      await driver.activateNotification(
        notificationTitle,
        action === null ? null : "View order",
        join(artifacts, `${id}-activation.png`),
      );
      const activatedPid = await connect();
      expect(activatedPid).not.toBe(oldPid);
      await agent.waitForWindow(
        { title, processId: activatedPid, active: true },
        { timeoutMs: 30000 },
      );
      const event = await evaluate<MuonNotificationEvent>(
        "await beginWatch(); const pending = await notificationApi.getPendingEvents(); return pending[0] ?? await nextEvent;",
      );
      expect(event).toMatchObject({
        id,
        type: action === null ? "activate" : "action",
        actionId: action,
        data: { cold: true, action },
      });
      events.push(event);
      await stop();
      await start();
      expect(
        await evaluate("return await notificationApi.getPendingEvents();"),
      ).toEqual([event]);
      await evaluate(
        `await notificationApi.acknowledgeEvent(${JSON.stringify(event.eventId)}); return true;`,
      );
    }
    await stop();
    await start();
    expect(
      await evaluate("return await notificationApi.getPendingEvents();"),
    ).toEqual([]);
    const pendingTitle = `${appId} maintenance pending`;
    await evaluate(
      `await beginWatch(); await notificationApi.post({id:'maintenance-pending',title:${JSON.stringify(pendingTitle)},body:'Keep the received event through history cleanup'}); return true;`,
    );
    await driver.activateNotification(
      pendingTitle,
      null,
      join(artifacts, "maintenance-pending.png"),
    );
    const pending = await evaluate<MuonNotificationEvent>(
      "return await nextEvent;",
    );
    const clearTitle = `${appId} maintenance history`;
    await evaluate(
      `await notificationApi.post({id:'maintenance-history',title:${JSON.stringify(clearTitle)},body:'Remove only this application history'}); return true;`,
    );
    await driver.waitForNotification(
      clearTitle,
      join(artifacts, "maintenance-before.png"),
    );
    const maintenance = await agent.processes.launchManaged({
      path: launcher,
      arguments: ["--muon-notification-clear-history"],
      captureStdout: true,
      captureStderr: true,
      createNoWindow: true,
    });
    try {
      expect(
        (await maintenance.waitForExit({ timeoutMs: 30000 })).root.exitCode,
        await maintenance.stderrText(),
      ).toBe(0);
    } finally {
      await maintenance.releaseAsync();
    }
    await driver.waitForRemoval(clearTitle);
    expect(
      await evaluate("return await notificationApi.getPendingEvents();"),
    ).toEqual([pending]);
    await evaluate(
      `await notificationApi.acknowledgeEvent(${JSON.stringify(pending.eventId)}); return true;`,
    );
    await stop();
    await writeFile(
      join(artifacts, "cold-launch.json"),
      JSON.stringify(
        {
          initialPid,
          installedBy: "NSIS",
          appId,
          updates: { version: "1.0.1", relocated: true },
          runningEvent,
          events,
          acknowledgedRestart: [],
          maintenance: { retainedEvent: pending, historyRemoved: true },
        },
        null,
        2,
      ),
    );
  } finally {
    try {
      await disconnect();
      if (currentPid !== undefined) {
        try {
          await agent.processes.kill(currentPid);
        } catch {
          /* Already exited. */
        }
      }
      for (const [index, command] of commands.entries()) {
        try {
          await writeFile(
            join(artifacts, `launcher-${index}.log`),
            (await command.stdoutText()) + (await command.stderrText()),
          );
        } finally {
          await command.releaseAsync();
        }
      }
      // A COM-launched process is not a child of the test's managed launcher.
      // Only this dedicated installation and application's runtime are owned.
      const prefixes = [...installations, appState]
        .filter(Boolean)
        .map((path) => path.toLowerCase() + "\\");
      for (const process of await agent.processes.list()) {
        if (
          process.running &&
          prefixes.some((prefix) =>
            process.path.toLowerCase().startsWith(prefix),
          )
        ) {
          try {
            await agent.processes.kill(process.id);
          } catch {
            // The process may have exited while another family member stopped.
          }
        }
      }
    } finally {
      try {
        if (
          installed &&
          (await agent.files.exists(win32.join(install, "Uninstall.exe")))
        ) {
          await powershell(
            `$app=Start-Process -FilePath ${quote(win32.join(install, "Uninstall.exe"))} -ArgumentList ${quote(`/S _?=${install}`)} -Wait -PassThru; if($app.ExitCode -ne 0){throw ('NSIS uninstall failed: '+$app.ExitCode)}`,
          );
          await powershell(`
$root=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser,[Microsoft.Win32.RegistryView]::Registry64)
if($null -ne $root.OpenSubKey(${quote(`Software\\Classes\\CLSID\\${clsid}`)}) -or $null -ne $root.OpenSubKey(${quote(`Software\\Classes\\AppUserModelId\\${appId}`)})){throw 'NSIS notification registration remains after uninstall'}
$root.Dispose()
`);
          expect(await agent.files.exists(appState)).toBe(false);
        }
      } finally {
        try {
          await driver?.close();
        } finally {
          await rm(project, { recursive: true, force: true });
          if (await agent.files.exists(installer))
            await agent.files.remove(installer);
          for (const directory of installations)
            if (await agent.files.exists(directory))
              await agent.files.remove(directory, { recursive: true });
        }
      }
    }
  }
};
