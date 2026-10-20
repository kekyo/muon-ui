// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile, spawn, type ChildProcess } from "node:child_process";
import { EventEmitter, once } from "node:events";
import { watch } from "node:fs";
import {
  access,
  mkdir,
  mkdtemp,
  readFile,
  rename,
  rm,
  symlink,
  writeFile,
} from "node:fs/promises";
import { createServer } from "node:http";
import { homedir, tmpdir } from "node:os";
import { basename, join, resolve } from "node:path";
import { promisify } from "node:util";
import { pathToFileURL } from "node:url";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { InMemoryTransport } from "@modelcontextprotocol/sdk/inMemory.js";
import { createConnection } from "@playwright/mcp";
import { beforeAll, describe, expect, it } from "vitest";
import type { MuonPackResult } from "../../ui/common/pack-types.js";
import type { MuonNotificationEvent } from "../common/renderer/notifications.js";
import { createGnomeNotificationDriver } from "./notifications-accessibility.js";
import { createKdeNotificationDriver } from "./notifications-kde-accessibility.js";

const execute = promisify(execFile);
const enabled =
  process.platform === "linux" &&
  process.env.MUON_TEST_NOTIFICATION_DESKTOP === "1";

describe.skipIf(!enabled)(
  "Packaged CEF notification activation on Linux",
  () => {
    beforeAll(async () => {
      const generated = resolve(
        "../cef/.build/test/linux-amd64/release/generated",
      );
      await execute("bash", ["./build.sh", "dist", "Release", "linux-amd64"], {
        cwd: resolve("../../builder"),
        env: {
          ...process.env,
          MUON_RUNTIME_INFO_HEADER: join(
            generated,
            "muon_runtime_info_generated.h",
          ),
          MUON_CORE_VERSION_HEADER: join(
            generated,
            "muon_core_version_generated.h",
          ),
        },
        maxBuffer: 16 * 1024 * 1024,
      });
    });
    it.each(["simple", "validate"] as const)(
      "launches the installed %s application and preserves pending events through maintenance",
      async (mode) => {
        const project = await mkdtemp(
          join(tmpdir(), "muon-notification-linux-pack-"),
        );
        const artifacts = resolve(
          process.env.MUON_TEST_NOTIFICATION_SHELL === "kde"
            ? ".run/notifications-linux-launch-kde"
            : ".run/notifications-linux-launch",
          mode,
        );
        await mkdir(artifacts, { recursive: true });
        const appId = `dev.muon.notificationtest.pack.${mode}.run${process.pid}`;
        const title = `Muon packaged notifications ${mode} ${process.pid}`;
        const applicationName = `Muon Notifications ${mode} ${process.pid}`;
        let install = join(project, "installed");
        const profile = join(install, "profile");
        const readinessPath = join(project, "page-ready");
        const data =
          process.env.XDG_DATA_HOME ?? join(homedir(), ".local/share");
        const desktopPath = join(data, "applications", `${appId}.desktop`);
        const servicePath = join(data, "dbus-1/services", `${appId}.service`);
        const iconPath = join(
          data,
          "icons/hicolor/256x256/apps",
          `${appId}.png`,
        );
        const events = new EventEmitter();
        const children: ChildProcess[] = [];
        let diagnostics = "";
        let launcher = "";
        let currentPid: number | undefined;
        let client: Client | undefined;
        let closeMcp: (() => Promise<void>) | undefined;
        const driver =
          process.env.MUON_TEST_NOTIFICATION_SHELL === "kde"
            ? await createKdeNotificationDriver()
            : await createGnomeNotificationDriver();
        // Watch the directory because writeTextFile atomically replaces the file.
        // The page signals through its permitted native filesystem API, avoiding
        // a loopback request from the packaged asset origin.
        const readinessWatcher = watch(project, (_, filename) => {
          if (filename === "page-ready") events.emit("page-ready", null);
        });
        const reservation = createServer();
        reservation.listen(0, "127.0.0.1");
        await once(reservation, "listening");
        const cdp = reservation.address();
        if (cdp === null || typeof cdp === "string")
          throw new Error("Missing CDP port");
        await new Promise<void>((done, reject) =>
          reservation.close((error) => (error ? reject(error) : done())),
        );
        const endpoint = `http://127.0.0.1:${cdp.port}`;
        const pageReady = () =>
          once(events, "page-ready", { signal: AbortSignal.timeout(120000) });
        const call = async (
          name: string,
          args: Record<string, unknown>,
        ): Promise<string> => {
          if (!client) throw new Error("MCP is not connected");
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
            function: `async () => { ${source} }`,
          });
          const result = /### Result\n([\s\S]*?)(?:\n### |$)/u.exec(text)?.[1];
          if (result === undefined) throw new Error(text);
          return JSON.parse(result) as T;
        };
        const disconnect = async (): Promise<void> => {
          await client?.close();
          await closeMcp?.();
          client = undefined;
          closeMcp = undefined;
        };
        const getPid = async (): Promise<number> => {
          const result = await execute("gdbus", [
            "call",
            "--session",
            "--dest",
            "org.freedesktop.DBus",
            "--object-path",
            "/org/freedesktop/DBus",
            "--method",
            "org.freedesktop.DBus.GetConnectionUnixProcessID",
            appId,
          ]);
          const pid = Number(result.stdout.match(/uint32 (\d+)/u)?.[1]);
          if (!Number.isInteger(pid) || pid <= 1)
            throw new Error(result.stdout);
          return pid;
        };
        const connect = async (
          ready: Promise<unknown[]>,
          notificationsEnabled: boolean,
        ): Promise<number> => {
          const [error] = await ready;
          if (error) throw new Error(String(error));
          const readiness = await readFile(readinessPath, "utf8");
          if (readiness !== "ready") throw new Error(readiness);
          const mcp = await createConnection({
            browser: { cdpEndpoint: endpoint },
            outputDir: artifacts,
          });
          closeMcp = async () => await mcp.close();
          client = new Client({
            name: "muon-packaged-linux-notifications",
            version: "1",
          });
          const [a, b] = InMemoryTransport.createLinkedPair();
          await mcp.connect(b);
          await client.connect(a);
          expect(await evaluate("return globalThis.ready;")).toBe(true);
          currentPid = await evaluate<number>(
            "return await environmentApi.getProcessId();",
          );
          expect(currentPid).toBeGreaterThan(1);
          if (notificationsEnabled) {
            expect(
              await evaluate(
                "return (await notificationApi.getCapabilities()).launch;",
              ),
            ).toEqual({ state: "supported", reason: null });
            expect(await getPid()).toBe(currentPid);
          } else {
            expect(
              await evaluate(
                "return window.muon?.notifications === undefined;",
              ),
            ).toBe(true);
          }
          return currentPid;
        };
        const start = async (): Promise<number> => {
          const ready = pageReady();
          let connected = false;
          const child = spawn(launcher, [], {
            stdio: ["ignore", "pipe", "pipe"],
          });
          children.push(child);
          child.on("error", (error) =>
            events.emit("page-ready", String(error)),
          );
          child.on("exit", (code, signal) => {
            if (!connected)
              events.emit(
                "page-ready",
                `Launcher exited before readiness: ${code ?? signal}\n${diagnostics}`,
              );
          });
          child.stdout?.on("data", (value) => {
            diagnostics += String(value);
          });
          child.stderr?.on("data", (value) => {
            diagnostics += String(value);
          });
          const pid = await connect(ready, true);
          connected = true;
          return pid;
        };
        const stop = async (): Promise<void> => {
          if (currentPid === undefined)
            throw new Error("No active application");
          const pid = currentPid;
          const pidFile = join(project, "process.pid");
          await writeFile(pidFile, String(pid));
          // procps pidwait uses pidfd_open + epoll, including for D-Bus children
          // that are not descendants of this test process.
          try {
            await Promise.all([
              execute("pidwait", ["-e", "-F", pidFile], { timeout: 30000 }),
              call("browser_tabs", { action: "close" }),
            ]);
          } catch (error) {
            // An exit before pidwait opens the pidfd reports no matched process.
            try {
              process.kill(pid, 0);
            } catch (probe) {
              if ((probe as NodeJS.ErrnoException).code === "ESRCH") {
                await disconnect();
                currentPid = undefined;
                return;
              }
              throw probe;
            }
            const command = await readFile(`/proc/${pid}/cmdline`, "utf8");
            throw new Error(
              `Application did not exit: ${pid} ${command.replaceAll("\0", " ")}\n${JSON.stringify(error, Object.getOwnPropertyNames(error))}`,
            );
          }
          await disconnect();
          currentPid = undefined;
        };
        const minimize = async (): Promise<void> => {
          await execute(
            "xdotool",
            [
              "search",
              "--onlyvisible",
              "--name",
              `^${title}$`,
              "windowminimize",
              "--sync",
            ],
            { timeout: 15000 },
          );
        };
        try {
          const packageDirectory = join(project, "package-dist");
          for (const [kind, path] of [
            ["runtime", resolve("../cef/.run/test-linux-amd64-release")],
            ["native", resolve("../../builder/dist-linux-amd64")],
          ]) {
            await mkdir(join(packageDirectory, kind!), { recursive: true });
            await symlink(path!, join(packageDirectory, kind!, "linux-amd64"));
          }
          await writeFile(
            join(project, "package.json"),
            JSON.stringify({
              name: `muon-notification-pack-${mode}`,
              version: "1.0.0",
              type: "module",
              author: "muon Tester",
            }),
          );
          await writeFile(
            join(project, "index.html"),
            `<!doctype html><meta charset="utf-8"><title>${title}</title><h1>${title}</h1><pre id="events"></pre><script type="module" src="/main.ts"></script>`,
          );
          await writeFile(
            join(project, "main.ts"),
            `
const filesystem = ${mode === "validate" ? "await import('muon:fs')" : "window.muon.fs"};
globalThis.environmentApi = ${mode === "validate" ? "await import('muon:environments')" : "window.muon.environments"};
try {
  globalThis.notificationApi = ${mode === "validate" ? "await import('muon:notifications')" : "window.muon.notifications"};
  globalThis.beginWatch = async () => {
    globalThis.nextEvent = new Promise(resolve => globalThis.receiveEvent = resolve);
    globalThis.watcher = await notificationApi.watchEvents(event => {
      document.querySelector('#events').textContent = JSON.stringify(event);
      receiveEvent(event);
    });
  };
  globalThis.ready = true;
  await filesystem.writeTextFile(${JSON.stringify(readinessPath)}, 'ready', 'utf8', undefined);
} catch(error) {
  await filesystem.writeTextFile(${JSON.stringify(readinessPath)}, String(error), 'utf8', undefined);
}
`,
          );
          const mainSource = await readFile(join(project, "main.ts"), "utf8");
          const config = {
            launcher: { defaultVersionPolicy: "tested" },
            notifications: { enabled: true },
            browser: { titleBarType: "native", profilePath: profile },
            network: { allow: ["asset://main/**"] },
            cdp: { enable: true, port: cdp.port },
            plugin: {
              mode,
              plugins: [
                {
                  name: "internal",
                  ...(mode === "simple"
                    ? {
                        allow: [
                          "muon.notifications.*",
                          "muon.fs.writeTextFile",
                          "muon.environments.getProcessId",
                        ],
                      }
                    : {
                        imports: [
                          {
                            sources: ["main.ts"],
                            allow: [
                              "muon.notifications.*",
                              "muon.fs.writeTextFile",
                              "muon.environments.getProcessId",
                            ],
                          },
                        ],
                      }),
                },
              ],
            },
            linux: { desktop: { name: applicationName } },
          };
          await writeFile(join(project, "muon.json"), JSON.stringify(config));
          await writeFile(
            join(project, "vite.config.mjs"),
            `
import muon from ${JSON.stringify(pathToFileURL(resolve("../../ui/dist/vite.mjs")).href)};
export default {build:{outDir:'web-dist',target:'es2022'},plugins:[muon({
${mode === "simple" ? "pluginAccess:false," : ""}
build:{targets:['linux-amd64'],packageDirectory:${JSON.stringify(packageDirectory)}}})]};
`,
          );
          const pack = async (version: string): Promise<MuonPackResult> => {
            const result = await execute(
              process.execPath,
              [
                resolve("../../ui/dist/cli.cjs"),
                "pack",
                "--json",
                "--target",
                "linux-amd64",
                "--type",
                "tar.gz",
                "--package-directory",
                packageDirectory,
                "--app-id",
                appId,
                "--name",
                "notification-launcher",
                "--package-version",
                version,
                "--icon",
                resolve("../../images/muon-256.png"),
              ],
              { cwd: project, maxBuffer: 16 * 1024 * 1024 },
            );
            await writeFile(
              join(artifacts, `pack-${version}.log`),
              result.stderr,
            );
            return JSON.parse(result.stdout) as MuonPackResult;
          };
          const packed = await pack("1.0.0");
          const built = packed.targets[0]!;
          if (built.target === "android") throw new Error("Unexpected target");
          await mkdir(install);
          await execute("tar", [
            "-xzf",
            packed.artifacts[0]!.path,
            "--strip-components=2",
            "-C",
            install,
          ]);
          launcher = join(install, basename(built.launcherPath));
          const initialPid = await start();
          // Keep this distinct from the live window title: the shell's
          // accessibility cache also contains window labels before its
          // application-registration cache has refreshed.
          await driver.waitForApplication(applicationName);
          expect(
            await evaluate(
              "return (await notificationApi.getCapabilities()).persistence;",
            ),
          ).toEqual({ state: "supported", reason: null });
          const registeredService = await readFile(servicePath, "utf8");
          expect(registeredService).toContain(
            `Exec="${launcher}" --muon-notification-activation`,
          );
          expect(await readFile(desktopPath, "utf8")).toContain(
            `X-muon-Launcher=${launcher}`,
          );
          await minimize();
          const runningTitle = `${appId} running`;
          await evaluate(
            `await beginWatch(); await notificationApi.post({id:'running',title:${JSON.stringify(runningTitle)},body:'Restore the same process'}); return true;`,
          );
          await driver.activateNotification(
            runningTitle,
            null,
            join(artifacts, "running.png"),
          );
          const running = await evaluate<MuonNotificationEvent>(
            "return await nextEvent;",
          );
          expect(await getPid()).toBe(initialPid);
          expect(
            (
              await execute("xdotool", ["getwindowfocus", "getwindowname"])
            ).stdout.trim(),
          ).toBe(title);
          await evaluate(
            `await notificationApi.acknowledgeEvent(${JSON.stringify(running.eventId)}); return true;`,
          );
          const received: MuonNotificationEvent[] = [];
          const coldPids: number[] = [];
          for (const action of [null, "default"] as const) {
            const id = `cold-${action ?? "activate"}`;
            const notificationTitle = `${appId} ${id}`;
            const oldPid = currentPid;
            await minimize();
            await evaluate(
              `await notificationApi.post({id:${JSON.stringify(id)},title:${JSON.stringify(notificationTitle)},body:'The process exits before this click',data:{action:${JSON.stringify(action)}},actions:[{id:'default',title:'View order'}]}); return true;`,
            );
            await driver.waitForNotification(
              notificationTitle,
              join(artifacts, `${id}-before.png`),
            );
            await stop();
            await driver.waitForNotification(
              notificationTitle,
              join(artifacts, `${id}-after.png`),
            );
            const ready = pageReady();
            await driver.activateNotification(
              notificationTitle,
              action === null ? null : "View order",
              join(artifacts, `${id}-click.png`),
            );
            const coldPid = await connect(ready, true);
            expect(coldPid).not.toBe(oldPid);
            coldPids.push(coldPid);
            const event = await evaluate<MuonNotificationEvent>(
              "await beginWatch(); return (await notificationApi.getPendingEvents())[0] ?? await nextEvent;",
            );
            expect(event).toMatchObject({
              id,
              type: action === null ? "activate" : "action",
              actionId: action,
              data: { action },
            });
            received.push(event);
            await stop();
            await start();
            expect(
              await evaluate(
                "return await notificationApi.getPendingEvents();",
              ),
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
          await minimize();
          const pendingTitle = `${appId} pending`;
          await evaluate(
            `await beginWatch(); await notificationApi.post({id:'pending',title:${JSON.stringify(pendingTitle)},body:'Keep this received event'}); return true;`,
          );
          await driver.activateNotification(
            pendingTitle,
            null,
            join(artifacts, "pending.png"),
          );
          const pending = await evaluate<MuonNotificationEvent>(
            "return await nextEvent;",
          );
          await minimize();
          const historyTitle = `${appId} history`;
          await evaluate(
            `await notificationApi.post({id:'history',title:${JSON.stringify(historyTitle)},body:'Clear this notification'}); return true;`,
          );
          await driver.waitForNotification(
            historyTitle,
            join(artifacts, "history.png"),
          );
          // Maintenance must reject while the live process owns the bus name.
          await expect(
            execute(launcher, ["--muon-notification-clear-history"], {
              timeout: 30000,
            }),
          ).rejects.toMatchObject({ code: 1 });
          await stop();
          await writeFile(
            servicePath,
            registeredService.replace(
              `X-muon-AppId=${appId}`,
              "X-muon-AppId=foreign",
            ),
          );
          await expect(
            execute(launcher, ["--muon-notification-unregister"], {
              timeout: 30000,
            }),
          ).rejects.toMatchObject({ code: 1 });
          await writeFile(servicePath, registeredService);
          await execute(launcher, ["--muon-notification-clear-history"], {
            timeout: 30000,
          });
          await driver.waitForRemoval(historyTitle);
          await start();
          expect(
            await evaluate("return await notificationApi.getPendingEvents();"),
          ).toEqual([pending]);
          await minimize();
          const disabledTitle = `${appId} disabled update`;
          await evaluate(
            `await notificationApi.post({id:'disabled',title:${JSON.stringify(disabledTitle)},body:'Withdraw after a disabled update'}); return true;`,
          );
          await driver.waitForNotification(
            disabledTitle,
            join(artifacts, "disabled-before.png"),
          );
          await stop();
          await writeFile(
            join(project, "muon.json"),
            JSON.stringify({ ...config, notifications: { enabled: false } }),
          );
          await writeFile(
            join(project, "main.ts"),
            `const filesystem = ${mode === "validate" ? "await import('muon:fs')" : "window.muon.fs"}; globalThis.environmentApi = ${mode === "validate" ? "await import('muon:environments')" : "window.muon.environments"}; globalThis.ready = true; await filesystem.writeTextFile(${JSON.stringify(readinessPath)}, 'ready', 'utf8', undefined);`,
          );
          const disabled = await pack("1.0.1");
          await execute("tar", [
            "-xzf",
            disabled.artifacts[0]!.path,
            "--strip-components=2",
            "-C",
            install,
          ]);
          const ready = pageReady();
          const disabledChild = spawn(launcher, [], {
            stdio: ["ignore", "pipe", "pipe"],
          });
          children.push(disabledChild);
          disabledChild.stderr?.on("data", (value) => {
            diagnostics += String(value);
          });
          await connect(ready, false);
          await driver.waitForRemoval(disabledTitle);
          await expect(access(servicePath)).rejects.toThrow();
          expect(await readFile(desktopPath, "utf8")).not.toContain(
            "DBusActivatable=true",
          );
          const snapshot = JSON.parse(
            await readFile(join(profile, "notifications.json"), "utf8"),
          ) as { pending: MuonNotificationEvent[]; revisions: unknown[] };
          expect(snapshot.revisions).toEqual([]);
          expect(snapshot.pending).toEqual([expect.objectContaining(pending)]);
          // Public APIs are absent in the disabled build; persisted pending events
          // remain application data and are verified again after re-enabling.
          await writeFile(
            join(artifacts, "disabled-snapshot.json"),
            JSON.stringify(snapshot, null, 2),
          );
          await stop();
          await writeFile(join(project, "muon.json"), JSON.stringify(config));
          await writeFile(join(project, "main.ts"), mainSource);
          const restored = await pack("1.0.2");
          await execute("tar", [
            "-xzf",
            restored.artifacts[0]!.path,
            "--strip-components=2",
            "-C",
            install,
          ]);
          await start();
          expect(
            await evaluate("return await notificationApi.getPendingEvents();"),
          ).toEqual([pending]);
          await evaluate(
            `await notificationApi.acknowledgeEvent(${JSON.stringify(pending.eventId)}); return true;`,
          );
          await minimize();
          const movedTitle = `${appId} moved`;
          await evaluate(
            `await notificationApi.post({id:'moved',title:${JSON.stringify(movedTitle)},body:'Open the updated and relocated application',data:{version:'1.0.2',location:'before-move'}}); return true;`,
          );
          await driver.waitForNotification(
            movedTitle,
            join(artifacts, "moved-before.png"),
          );
          await stop();
          const updated = await pack("1.0.3");
          await execute("tar", [
            "-xzf",
            updated.artifacts[0]!.path,
            "--strip-components=2",
            "-C",
            install,
          ]);
          const movedInstall = join(project, "moved application");
          await rename(install, movedInstall);
          install = movedInstall;
          launcher = join(install, basename(launcher));
          const movedPid = await start();
          expect(await readFile(servicePath, "utf8")).toContain(
            `Exec="${launcher}" --muon-notification-activation`,
          );
          expect(await readFile(desktopPath, "utf8")).toContain(
            `X-muon-Launcher=${launcher}`,
          );
          await stop();
          const movedReady = pageReady();
          await driver.activateNotification(
            movedTitle,
            null,
            join(artifacts, "moved-click.png"),
          );
          const movedColdPid = await connect(movedReady, true);
          expect(movedColdPid).not.toBe(movedPid);
          const movedEvents = await evaluate<MuonNotificationEvent[]>(
            "return await notificationApi.getPendingEvents();",
          );
          expect(movedEvents).toEqual([
            expect.objectContaining({
              id: "moved",
              type: "activate",
              actionId: null,
              data: { version: "1.0.2", location: "before-move" },
            }),
          ]);
          await evaluate(
            `await notificationApi.acknowledgeEvent(${JSON.stringify(movedEvents[0]!.eventId)}); return true;`,
          );
          await minimize();
          const unregisterTitle = `${appId} unregister`;
          await evaluate(
            `await notificationApi.post({id:'unregister',title:${JSON.stringify(unregisterTitle)},body:'Remove this notification when unregistering'}); return true;`,
          );
          await driver.waitForNotification(
            unregisterTitle,
            join(artifacts, "unregister-before.png"),
          );
          await stop();
          await execute(launcher, ["--muon-notification-unregister"], {
            timeout: 30000,
          });
          await driver.waitForRemoval(unregisterTitle);
          await expect(access(servicePath)).rejects.toThrow();
          await expect(access(desktopPath)).rejects.toThrow();
          await expect(access(iconPath)).rejects.toThrow();
          await writeFile(
            join(artifacts, "result.json"),
            JSON.stringify(
              {
                initialPid,
                coldPids,
                running,
                received,
                pending,
                disabledUpdate: true,
                movedPid,
                movedColdPid,
                movedEvents,
                unregistered: true,
              },
              null,
              2,
            ),
          );
        } finally {
          readinessWatcher.close();
          if (currentPid === undefined) {
            try {
              currentPid = await getPid();
            } catch {
              /* The application never acquired its dedicated bus name. */
            }
          }
          if (currentPid !== undefined) {
            try {
              const executable = (
                await readFile(`/proc/${currentPid}/cmdline`, "utf8")
              ).split("\0")[0];
              if (executable !== join(install, "muon-core"))
                throw new Error(`Unexpected test process: ${executable}`);
              const pidFile = join(project, "cleanup.pid");
              await writeFile(pidFile, String(currentPid));
              const exited = execute("pidwait", ["-F", pidFile], {
                timeout: 30000,
              });
              process.kill(currentPid, "SIGTERM");
              await exited;
            } catch {
              /* Already exited. */
            }
          }
          await disconnect();
          for (const child of children) {
            if (child.exitCode === null && child.signalCode === null) {
              const closed = once(child, "close", {
                signal: AbortSignal.timeout(30000),
              });
              child.kill("SIGTERM");
              await closed;
            }
          }
          if (launcher) {
            try {
              await execute(launcher, ["--muon-notification-unregister"], {
                timeout: 30000,
              });
            } catch {
              /* The explicit successful unregister already removed ownership. */
            }
          }
          for (const path of [desktopPath, servicePath, iconPath])
            await rm(path, { force: true });
          await execute("gtk-update-icon-cache", [
            "--force",
            "--ignore-theme-index",
            join(data, "icons/hicolor"),
          ]);
          await driver.close();
          await writeFile(join(artifacts, "launcher.log"), diagnostics);
          await rm(project, { recursive: true, force: true });
        }
      },
      300000,
    );
  },
);
