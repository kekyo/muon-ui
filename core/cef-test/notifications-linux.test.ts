// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile, spawn, type ChildProcess } from "node:child_process";
import { EventEmitter, once } from "node:events";
import { constants } from "node:fs";
import {
  copyFile,
  mkdir,
  mkdtemp,
  readdir,
  rm,
  symlink,
  writeFile,
} from "node:fs/promises";
import { createServer } from "node:http";
import { homedir, tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { createInterface } from "node:readline";
import { promisify } from "node:util";

import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { InMemoryTransport } from "@modelcontextprotocol/sdk/inMemory.js";
import { createConnection } from "@playwright/mcp";
import { describe, expect, it, vi } from "vitest";

import { createMuonCapabilityModuleResolver } from "../../ui/common/capability.js";
import { resolveMuonCefNotificationFunctionPaths } from "../../ui/cef/notifications.js";
import {
  createLinuxDesktopEntry,
  quoteDesktopExecArgument,
} from "../../ui/cef/linux-desktop.js";
import { createGnomeNotificationDriver } from "./notifications-accessibility.js";
import { createKdeNotificationDriver } from "./notifications-kde-accessibility.js";
import { runNotificationDesktopCase } from "./notifications-desktop-case.js";

const execute = promisify(execFile);
const desktop = process.env.MUON_TEST_NOTIFICATION_DESKTOP === "1";
const runtime = resolve("../cef/.run/test-linux-amd64-release");
const serverExecutable = resolve(
  "../cef/.build/test/linux-amd64/release/muon_notification_test_server",
);

interface Observation {
  type: string;
  id: number;
  title: string;
  body: string;
  icon: string;
  app: string;
  actions: string[];
  "desktop-entry": string;
  category: string;
}

const stopProcess = async (child: ChildProcess | undefined): Promise<void> => {
  if (
    child === undefined ||
    child.exitCode !== null ||
    child.signalCode !== null
  )
    return;
  const closed = once(child, "close", { signal: AbortSignal.timeout(30000) });
  child.kill("SIGTERM");
  await closed;
};

describe.skipIf(process.platform !== "linux")(
  "Linux notifications through Playwright MCP",
  () => {
    it.each(["simple", "validate"] as const)(
      desktop
        ? "delivers events from the Linux notification UI in %s mode"
        : "shares persisted notifications across pages in %s mode",
      async (mode) => {
        const directory = await mkdtemp(join(tmpdir(), "muon-notifications-"));
        const artifacts = resolve(
          ".run",
          desktop
            ? process.env.MUON_TEST_NOTIFICATION_SHELL === "kde"
              ? "notifications-desktop-kde"
              : "notifications-desktop"
            : "notifications-linux",
          mode,
        );
        await mkdir(artifacts, { recursive: true });
        const desktopFiles: string[] = [];
        const events = new EventEmitter();
        const observations: Observation[] = [];
        let service: ChildProcess | undefined;
        let application: ChildProcess | undefined;
        let bus: ChildProcess | undefined;
        let diagnostics = "";
        let moduleSource = "";
        const web = createServer((request, response) => {
          const url = new URL(request.url ?? "/", "http://localhost");
          const path = url.pathname;
          response.setHeader("Cache-Control", "no-store");
          if (path === "/notifications.mjs") {
            response.setHeader("Content-Type", "text/javascript");
            response.end(moduleSource);
          } else if (path === "/ready") {
            response.end("ok");
            events.emit("page-ready");
          } else if (path === "/page-error") {
            response.end("ok");
            events.emit(
              "page-ready",
              new Error(url.searchParams.get("message") ?? "Page failed"),
            );
          } else {
            response.setHeader("Content-Type", "text/html");
            response.end(`<!doctype html><title>Muon Notifications ${mode}</title>
            <style>
              body { background: white; color: #18202a; font: 16px sans-serif; margin: 24px; }
              label { display: inline-block; margin: 0 16px 12px 0; }
              button { margin: 0 6px 8px 0; padding: 6px 10px; }
              pre { white-space: pre-wrap; overflow-wrap: anywhere; }
            </style>
            <h1>Notification driver</h1>
            <label>Notification ID <input id="notification-id" value="driver-order"></label>
            <label>Title <input id="notification-title" value="Order ready"></label>
            <p>Posting the same ID replaces its previous notification.</p>
            <button id="post">Post / update</button><button id="cancel">Cancel</button>
            <button id="status">Status</button><button id="capabilities">Capabilities</button>
            <button id="watch">Watch events</button><button id="pending">Read pending</button>
            <button id="acknowledge">Acknowledge displayed events</button>
            <pre id="result"></pre><pre id="events"></pre>
            <script>
              addEventListener('error', async event => await fetch('/page-error?message=' + encodeURIComponent(event.message)));
              addEventListener('unhandledrejection', async event => await fetch('/page-error?message=' + encodeURIComponent(String(event.reason))));
            </script>
            <script type="module">
              globalThis.notificationApi = ${mode === "validate" ? "await import('/notifications.mjs')" : "window.muon?.notifications"};
              globalThis.notificationEvents = [];
              globalThis.beginWatch = async () => {
                globalThis.nextEvent = new Promise(resolve => globalThis.receiveEvent = resolve);
                globalThis.watcher = await notificationApi.watchEvents(event => {
                  notificationEvents.push(event);
                  document.querySelector('#events').textContent = JSON.stringify(notificationEvents);
                  receiveEvent(event);
                });
              };
              const commands = {
                post: async () => await notificationApi.post({
                  id: document.querySelector('#notification-id').value,
                  title: document.querySelector('#notification-title').value,
                  body: 'Open this notification to inspect its event.',
                  data: {screen: 'order'}, actions: [{id:'details',title:'View order'}]
                }),
                cancel: async () => await notificationApi.cancel(document.querySelector('#notification-id').value),
                status: async () => await notificationApi.getStatus(),
                capabilities: async () => await notificationApi.getCapabilities(),
                watch: async () => { if (globalThis.watcher) await watcher.close(); await beginWatch(); },
                pending: async () => {
                  const pending = await notificationApi.getPendingEvents();
                  const combined = new Map(notificationEvents.map(event => [event.eventId,event]));
                  for (const event of pending) combined.set(event.eventId,event);
                  notificationEvents = [...combined.values()];
                  document.querySelector('#events').textContent = JSON.stringify(notificationEvents,null,2);
                  return pending;
                },
                acknowledge: async () => {
                  for (const event of notificationEvents) await notificationApi.acknowledgeEvent(event.eventId);
                  notificationEvents = [];
                  document.querySelector('#events').textContent = '[]';
                }
              };
              for (const [name,command] of Object.entries(commands)) {
                document.getElementById(name).addEventListener('click', async () => {
                  try { document.querySelector('#result').textContent = JSON.stringify(await command()) ?? 'Done'; }
                  catch(error) { document.querySelector('#result').textContent = String(error); }
                });
              }
              await fetch('/ready');
            </script>`);
          }
        });
        const client = new Client({
          name: "muon-notifications-linux",
          version: "1",
        });
        let closeMcp: (() => Promise<void>) | undefined;
        let closeDesktop: (() => Promise<void>) | undefined;
        try {
          // An isolated bus without activation directories makes service absence
          // deterministic and prevents this fixture from replacing the desktop.
          const environment = { ...process.env };
          if (!desktop) {
            const busConfig = join(directory, "bus.conf");
            await writeFile(
              busConfig,
              `<busconfig><type>session</type>
          <listen>unix:tmpdir=${directory}</listen><auth>EXTERNAL</auth>
          <policy context="default"><allow send_destination="*"/>
            <allow receive_sender="*"/><allow own="*"/></policy></busconfig>`,
            );
            bus = spawn(
              "dbus-daemon",
              ["--nofork", "--print-address=1", `--config-file=${busConfig}`],
              {
                stdio: ["ignore", "pipe", "pipe"],
              },
            );
            bus.stderr!.on("data", (data: Buffer) => {
              diagnostics += data.toString();
            });
            const busLines = createInterface({ input: bus.stdout! });
            const [busAddress] = await once(busLines, "line", {
              signal: AbortSignal.timeout(30000),
            });
            environment.DBUS_SESSION_BUS_ADDRESS = String(busAddress);
          }
          const control = async (
            method: string,
            args: string[],
          ): Promise<void> => {
            await execute(
              "gdbus",
              [
                "call",
                "--session",
                "--dest",
                "dev.muon.NotificationTest",
                "--object-path",
                "/dev/muon/NotificationTest",
                "--method",
                `dev.muon.NotificationTest.${method}`,
                ...args,
              ],
              { env: environment, timeout: 30000 },
            );
          };
          const startService = async (): Promise<void> => {
            service = spawn(serverExecutable, [], {
              env: environment,
              stdio: ["ignore", "pipe", "pipe"],
            });
            service.stderr!.on("data", (data: Buffer) => {
              diagnostics += data.toString();
            });
            const ready = once(events, "service-ready", {
              signal: AbortSignal.timeout(30000),
            });
            createInterface({ input: service.stdout! }).on("line", (line) => {
              const observation = JSON.parse(line) as Observation;
              observations.push(observation);
              events.emit(
                observation.type === "ready" ? "service-ready" : "observation",
                observation,
              );
            });
            const failure = new Promise<never>((_resolve, reject) => {
              service!.once("error", reject);
              service!.once("close", (code) =>
                reject(
                  new Error(
                    `Notification server exited ${code}: ${diagnostics}`,
                  ),
                ),
              );
            });
            await Promise.race([ready, failure]);
          };
          const observed = async (
            type: string,
            after: number,
          ): Promise<Observation> => {
            for (;;) {
              const value = observations
                .slice(after)
                .find((entry) => entry.type === type);
              if (value !== undefined) return value;
              await once(events, "observation", {
                signal: AbortSignal.timeout(30000),
              });
            }
          };
          web.listen(0, "127.0.0.1");
          await once(web, "listening");
          const address = web.address();
          if (address === null || typeof address === "string")
            throw new Error("HTTP server has no port");
          const origin = `http://127.0.0.1:${address.port}`;
          const reservation = createServer();
          reservation.listen(0, "127.0.0.1");
          await once(reservation, "listening");
          const debugAddress = reservation.address();
          if (debugAddress === null || typeof debugAddress === "string")
            throw new Error("CDP has no port");
          await new Promise<void>((done, reject) =>
            reservation.close((error) => (error ? reject(error) : done())),
          );
          const resolver = createMuonCapabilityModuleResolver(directory, {
            backend: "cef",
            functionPaths: resolveMuonCefNotificationFunctionPaths(
              { enabled: true },
              ["linux-amd64"],
            ),
            imports: [
              { sources: ["main.ts"], allow: ["muon.notifications.*"] },
            ],
          });
          const virtualId = resolver.resolveId(
            "muon:notifications",
            join(directory, "main.ts"),
          )!.id;
          moduleSource = resolver.load(virtualId)!;
          const plugin = {
            ...(mode === "simple"
              ? { mode }
              : resolver.getRuntimePluginConfig()),
            pages: [`${origin}/**`],
            plugins: [{ name: "internal", allow: ["muon.notifications.*"] }],
          };
          const configPath = join(directory, "muon.json");
          const appId = `dev.muon.NotificationTestApp.${mode}.run${process.pid}`;
          await writeFile(
            configPath,
            JSON.stringify({
              launcher: {
                appId,
                desktopId: appId,
              },
              notifications: { enabled: true, defaultCategory: "orders" },
              browser: {
                startPage: origin,
                profilePath: join(directory, "profile"),
                titleBarType: "native",
              },
              network: { allow: [`${origin}/**`] },
              plugin,
              cdp: { enable: true, port: debugAddress.port },
            }),
          );
          for (const name of await readdir(runtime)) {
            if (name !== "muon-core" && name !== "muon.json")
              await symlink(join(runtime, name), join(directory, name));
          }
          const executable = join(directory, "muon-core");
          await copyFile(join(runtime, "muon-core"), executable);
          if (desktop) {
            const dataDirectory =
              process.env.XDG_DATA_HOME ?? join(homedir(), ".local", "share");
            const applications = join(dataDirectory, "applications");
            const icons = join(
              dataDirectory,
              "icons",
              "hicolor",
              "256x256",
              "apps",
            );
            await mkdir(applications, { recursive: true });
            await mkdir(icons, { recursive: true });
            const iconPath = join(icons, `${appId}.png`);
            await copyFile(
              resolve("../../images/muon-256.png"),
              iconPath,
              constants.COPYFILE_EXCL,
            );
            desktopFiles.push(iconPath);
            const entryPath = join(applications, `${appId}.desktop`);
            await writeFile(
              entryPath,
              createLinuxDesktopEntry({
                desktop: {
                  desktopId: appId,
                  name: `Muon Notifications ${mode}`,
                  comment: "Notification test driver",
                  categories: ["Development"],
                  startupNotify: false,
                  notificationsEnabled: false,
                  iconPath,
                  iconFileName: `${appId}.png`,
                  configFileName: "muon.json",
                },
                exec: [executable, "-c", configPath]
                  .map(quoteDesktopExecArgument)
                  .join(" "),
                tryExec: executable,
                icon: iconPath,
                appId: undefined,
                launcherPath: undefined,
              }),
              { flag: "wx" },
            );
            desktopFiles.push(entryPath);
          }
          const pageReady = once(events, "page-ready", {
            signal: AbortSignal.timeout(120000),
          });
          application = spawn(
            executable,
            [
              "-c",
              configPath,
              "--ozone-platform=x11",
              "--disable-gpu",
              "--disable-vulkan",
            ],
            {
              cwd: directory,
              env: environment,
              stdio: ["ignore", "pipe", "pipe"],
            },
          );
          application.stdout!.on("data", (data: Buffer) => {
            diagnostics += data.toString();
          });
          application.stderr!.on("data", (data: Buffer) => {
            diagnostics += data.toString();
          });
          const failed = new Promise<never>((_resolve, reject) => {
            application!.once("error", reject);
            application!.once("close", (code) =>
              reject(new Error(`Muon exited ${code}: ${diagnostics}`)),
            );
          });
          const [pageError] = await Promise.race([pageReady, failed]);
          if (pageError instanceof Error) throw pageError;
          const mcp = await createConnection({
            browser: { cdpEndpoint: `http://127.0.0.1:${debugAddress.port}` },
            outputDir: join(directory, "mcp"),
          });
          closeMcp = async () => await mcp.close();
          const [clientTransport, serverTransport] =
            InMemoryTransport.createLinkedPair();
          await mcp.connect(serverTransport);
          await client.connect(clientTransport);
          const call = async (
            name: string,
            arguments_: Record<string, unknown>,
          ): Promise<string> => {
            const result = await client.callTool({
              name,
              arguments: arguments_,
            });
            const content = result.content as { type: string; text?: string }[];
            const text = content
              .filter((part) => part.type === "text")
              .map((part) => part.text ?? "")
              .join("\n");
            if (result.isError) throw new Error(`${text}\n${diagnostics}`);
            return text;
          };
          const evaluate = async <T>(source: string): Promise<T> => {
            const text = await call("browser_evaluate", {
              function: `async () => { ${source} }`,
            });
            const result = text.match(
              /### Result\n([\s\S]*?)(?:\n### |$)/,
            )?.[1];
            if (result === undefined)
              throw new Error(`No evaluation result: ${text}`);
            return JSON.parse(result) as T;
          };
          expect(await evaluate("return typeof notificationApi?.post;")).toBe(
            "function",
          );
          const rawPost =
            mode === "simple"
              ? "window.muon.notifications.__post(null)"
              : `globalThis.__muon_plugin_call(${JSON.stringify(resolver.getRuntimePluginConfig().capabilities[0]!.id)}, 'muon.notifications.post', [null])`;
          expect(
            await evaluate(
              `try { await ${rawPost}; return ''; } catch(error) { return String(error); }`,
            ),
          ).not.toBe("");
          if (desktop) {
            const driver =
              process.env.MUON_TEST_NOTIFICATION_SHELL === "kde"
                ? await createKdeNotificationDriver()
                : await createGnomeNotificationDriver();
            closeDesktop = async () => await driver.close();
            // GNOME suppresses banners for resident notifications from the
            // focused application. Exercise delivery while our driver is hidden.
            await execute(
              "xdotool",
              [
                "search",
                "--onlyvisible",
                "--name",
                `^Muon Notifications ${mode}$`,
                "windowminimize",
                "--sync",
              ],
              { timeout: 15000 },
            );
            try {
              await runNotificationDesktopCase(
                evaluate,
                {
                  ...driver,
                  activateNotification: async (title, action, screenshot) => {
                    await driver.activateNotification(
                      title,
                      action,
                      screenshot,
                    );
                    await vi.waitFor(
                      async () => {
                        const { stdout } = await execute(
                          "xdotool",
                          ["getwindowfocus", "getwindowname"],
                          { timeout: 15000 },
                        );
                        expect(stdout.trim()).toBe(
                          `Muon Notifications ${mode}`,
                        );
                      },
                      { timeout: 30000 },
                    );
                  },
                },
                artifacts,
                mode,
                "linux",
              );
            } finally {
              await evaluate(
                "for (const id of ['desktop-order','desktop-open','desktop-cancel']) await notificationApi.cancel(id); return true;",
              );
            }
            return;
          }
          expect(
            await evaluate(
              "try { await notificationApi.post({id:'absent',title:'Absent',body:''}); return ''; } catch(error) { return String(error); }",
            ),
          ).toMatch(/notification|service|bus/i);
          await startService();
          const capabilities = await evaluate<
            Record<string, { state: string }>
          >("return await notificationApi.getCapabilities();");
          expect(capabilities.interaction?.state).toBe("supported");
          expect(capabilities.actions?.state).toBe("supported");
          expect(capabilities.launch?.state).toBe("unsupported");
          expect(capabilities.persistence?.state).toBe("unknown");
          expect(
            await evaluate(
              "return await notificationApi.getStatus('arbitrary-purpose');",
            ),
          ).toMatchObject({
            app: { state: "unknown" },
            category: { id: "arbitrary-purpose", state: "unknown" },
          });
          await evaluate("await beginWatch(); return true;");

          const firstIndex = observations.length;
          await evaluate(
            "await notificationApi.post({id:'order',title:'Order one',body:'Plain <text>',data:{version:1}}); return true;",
          );
          const first = await observed("notify", firstIndex);
          expect(first.title).toBe("Order one");
          expect(first.body).toBe("Plain <text>");
          const secondIndex = observations.length;
          await evaluate(
            "await notificationApi.post({id:'order',title:'Order two',body:'Updated',category:'arbitrary-purpose',data:{version:2},actions:[{id:'default',title:'Named default'}]}); return true;",
          );
          const second = await observed("notify", secondIndex);
          expect(second.id).not.toBe(first.id);
          expect(await observed("close", secondIndex)).toMatchObject({
            id: first.id,
          });
          const namedKey =
            second.actions[second.actions.indexOf("Named default") - 1];
          expect(namedKey).toBeTypeOf("string");
          expect(namedKey).not.toBe("default");
          await control("Activate", [String(first.id), "default"]);
          await control("Activate", [String(second.id), namedKey!]);
          const event = await evaluate<{ eventId: string }>(
            "return await nextEvent;",
          );
          expect(event).toMatchObject({
            id: "order",
            type: "action",
            actionId: "default",
            data: { version: 2 },
          });
          expect(
            await evaluate("return await notificationApi.getPendingEvents();"),
          ).toEqual([event]);
          await control("Activate", [String(second.id), "default"]);

          const reloaded = once(events, "page-ready", {
            signal: AbortSignal.timeout(30000),
          });
          await call("browser_navigate", { url: origin });
          await reloaded;
          expect(
            await evaluate("return await notificationApi.getPendingEvents();"),
          ).toEqual([event]);
          await evaluate("await beginWatch(); return true;");
          await evaluate(
            `await notificationApi.acknowledgeEvent(${JSON.stringify(event.eventId)}); await notificationApi.acknowledgeEvent(${JSON.stringify(event.eventId)}); return true;`,
          );
          expect(
            await evaluate("return await notificationApi.getPendingEvents();"),
          ).toEqual([]);

          const cancelIndex = observations.length;
          await evaluate(
            "await notificationApi.post({id:'cross-window',title:'Cancel from popup',body:''}); return true;",
          );
          const cancel = await observed("notify", cancelIndex);
          const popupReady = once(events, "page-ready", {
            signal: AbortSignal.timeout(30000),
          });
          await evaluate("window.open('/popup'); return true;");
          await popupReady;
          const tabs = await call("browser_tabs", { action: "list" });
          const popupIndex = Number(tabs.match(/- (\d+):[^\n]*\/popup/)?.[1]);
          expect(Number.isFinite(popupIndex), tabs).toBe(true);
          await call("browser_tabs", { action: "select", index: popupIndex });
          await evaluate(
            "await beginWatch(); await notificationApi.cancel('cross-window'); return true;",
          );
          expect(await observed("close", cancelIndex)).toMatchObject({
            id: cancel.id,
          });
          await call("browser_tabs", { action: "close", index: popupIndex });
          await call("browser_tabs", { action: "select", index: 0 });
          const finalIndex = observations.length;
          await evaluate(
            "await notificationApi.post({id:'after-popup',title:'Main survives',body:'',data:{window:'main'}}); return true;",
          );
          const final = await observed("notify", finalIndex);
          await control("Activate", [String(final.id), "default"]);
          expect(await evaluate("return await nextEvent;")).toMatchObject({
            id: "after-popup",
            type: "activate",
            actionId: null,
            data: { window: "main" },
          });
          await evaluate(
            "await watcher.close(); await watcher.close(); return true;",
          );
          await control("SetCapabilities", ["['body']"]);
          expect(
            await evaluate("return await notificationApi.getCapabilities();"),
          ).toMatchObject({
            actions: { state: "unsupported" },
            persistence: { state: "unsupported" },
          });
          expect(
            await evaluate(
              "try { await notificationApi.post({id:'unsupported',title:'No actions',body:'',actions:[{id:'x',title:'X'}]}); return ''; } catch(error) { return String(error); }",
            ),
          ).toMatch(/action/i);
          await control("FailNotify", ["true"]);
          expect(
            await evaluate(
              "try { await notificationApi.post({id:'failure',title:'Failure',body:''}); return ''; } catch(error) { return String(error); }",
            ),
          ).toContain("Injected Notify failure");
        } finally {
          await closeDesktop?.();
          await client.close();
          await closeMcp?.();
          await stopProcess(application);
          await stopProcess(service);
          await stopProcess(bus);
          await new Promise<void>((done) => web.close(() => done()));
          await writeFile(join(artifacts, "runtime.log"), diagnostics);
          if (!desktop)
            await writeFile(
              join(artifacts, "observations.json"),
              JSON.stringify(observations, null, 2),
            );
          for (const path of desktopFiles) await rm(path);
          await rm(directory, { recursive: true, force: true });
        }
      },
      180000,
    );
  },
);
