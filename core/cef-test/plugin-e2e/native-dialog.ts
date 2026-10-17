// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { expect, it } from "vitest";
import type { GtkApp, GtkWindowElement } from "gestament";
import type { AppWindow } from "agent-rover";
import { createConnection } from "@playwright/mcp";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { InMemoryTransport } from "@modelcontextprotocol/sdk/inMemory.js";
import { spawn } from "node:child_process";
import { PNG } from "pngjs";
import {
  mkdtemp as makeHostDirectory,
  writeFile as writeHostFile,
} from "node:fs/promises";
import { tmpdir as hostTmpdir } from "node:os";

import {
  cdpCommandTimeoutMs,
  connectToMuonCdp,
  describeMuonPluginBridge,
  execFileAsync,
  findGestamentNativeDialogButtonByLabel,
  findGestamentNativeWindow,
  getCurrentTargetIds,
  join,
  mkdtemp,
  MUON_APP_URL,
  MUON_PORT,
  processExitTimeoutMs,
  readNativeFileDialogProbeResult,
  sendNativeClickWithoutFocus,
  rm,
  startDebugMuon,
  startGestamentDebugMuon,
  startNativeFileDialogProbe,
  stopMuon,
  stopGestamentMuon,
  targetTimeoutMs,
  tmpdir,
  delay,
  waitForGestamentMuonExit,
  waitForGestamentNativeWindowClosed,
  waitForMuonFsSelectFile,
  waitForNewPageTarget,
  waitForProcessExit,
  writeFile,
  type CdpDriver,
  type RunningMuon,
  type RuntimeEvaluateResponse,
} from "./shared.js";
import {
  isWindowsRemoteE2e,
  requireWindowsRemoteContext,
} from "./windows-context.js";

interface Point {
  x: number;
  y: number;
}

interface WindowBounds {
  height: number;
  width: number;
  x: number;
  y: number;
}

const containsPoint = (bounds: WindowBounds, point: Point): boolean =>
  point.x >= bounds.x &&
  point.x <= bounds.x + bounds.width &&
  point.y >= bounds.y &&
  point.y <= bounds.y + bounds.height;

const isNativeWindowFocused = async (
  windowId: string,
  environment: NodeJS.ProcessEnv,
): Promise<boolean> => {
  const { stdout } = await execFileAsync("xdotool", ["getwindowfocus"], {
    env: environment,
  });
  let focused = Number(stdout.trim());
  while (focused > 1) {
    if (focused === Number(windowId)) return true;
    // GTK may focus an input-only child instead of its public top-level XID.
    const tree = await execFileAsync(
      "xwininfo",
      ["-id", String(focused), "-tree"],
      {
        env: environment,
      },
    );
    const parent = /Parent window id: (0x[0-9a-f]+)/i.exec(tree.stdout)?.[1];
    if (parent === undefined) return false;
    focused = Number(parent);
  }
  return false;
};

const chooseBrowserClickPoint = (
  browserBounds: WindowBounds,
  excludedBounds: WindowBounds | undefined,
): Point => {
  const yNearTop =
    browserBounds.y + Math.max(24, Math.min(96, browserBounds.height - 24));
  const candidates: Point[] = [
    { x: browserBounds.x + 32, y: yNearTop },
    { x: browserBounds.x + browserBounds.width - 32, y: yNearTop },
    { x: browserBounds.x + 32, y: browserBounds.y + browserBounds.height - 32 },
    {
      x: browserBounds.x + browserBounds.width - 32,
      y: browserBounds.y + browserBounds.height - 32,
    },
    {
      x: browserBounds.x + Math.floor(browserBounds.width / 2),
      y: browserBounds.y + Math.floor(browserBounds.height / 2),
    },
  ];
  const match = candidates.find(
    (point) =>
      containsPoint(browserBounds, point) &&
      (excludedBounds === undefined || !containsPoint(excludedBounds, point)),
  );
  if (match !== undefined) {
    return match;
  }
  return { x: browserBounds.x + 32, y: yNearTop };
};

const clickBrowserWindow = async (
  app: GtkApp,
  browserBounds: WindowBounds,
  excludedWindow: GtkWindowElement | undefined,
): Promise<void> => {
  const excludedBounds = await excludedWindow?.bounds();
  await clickBrowserWindowOutsideBounds(app, browserBounds, excludedBounds);
};

const clickBrowserWindowOutsideBounds = async (
  app: GtkApp,
  browserBounds: WindowBounds,
  excludedBounds: WindowBounds | undefined,
): Promise<void> => {
  const point = chooseBrowserClickPoint(browserBounds, excludedBounds);
  await app.input.moveMouseTo(Math.round(point.x), Math.round(point.y));
  await app.input.setMouseButton("left", true);
  await app.input.setMouseButton("left", false);
  await delay(100);
};

const readBrowserWindowBounds = async (
  driver: CdpDriver,
): Promise<WindowBounds> =>
  await driver.evaluate<WindowBounds>(`({
    x: window.screenX,
    y: window.screenY,
    width: window.outerWidth,
    height: window.outerHeight,
  })`);

const closeOwnerBrowserWindow = async (driver: CdpDriver): Promise<void> => {
  await driver.evaluate(`window.muon.browser.close(); undefined`);
  driver.close();
};

const expectGestamentMuonExitedNormally = async (
  app: GtkApp,
): Promise<void> => {
  const output = await waitForGestamentMuonExit(app, processExitTimeoutMs);
  const detail = `exitSignal=${String(output.exitSignal)} stderr:\n${output.stderr}`;
  expect(output.exitCode, detail).toBe(0);
  expect(output.exitSignal, detail).toBeNull();
};

const moveDialogAwayFromBrowserClickPoint = async (
  dialogWindow: GtkWindowElement,
  browserBounds: WindowBounds,
): Promise<void> => {
  try {
    await dialogWindow.moveTo(
      browserBounds.x + 160,
      Math.max(0, browserBounds.y + 40),
    );
  } catch {
    // Moving the dialog is only a best-effort aid for the click probe.
  }
  await delay(100);
};

const createCounterPage = (pageTitle: string, buttonLabel: string): string =>
  encodeURIComponent(`<!doctype html>
    <title>${pageTitle}</title>
    <style>
      html, body {
        height: 100%;
        margin: 0;
      }
      button {
        border: 0;
        height: 100%;
        width: 100%;
      }
    </style>
    <button aria-label="${buttonLabel}" onclick="window.__muonCounter += 1">
      ${buttonLabel}
    </button>
    <script>window.__muonCounter = 0;</script>`);

const createFileInputPage = (pageTitle: string): string =>
  encodeURIComponent(`<!doctype html>
    <title>${pageTitle}</title>
    <style>
      html, body {
        height: 100%;
        margin: 0;
      }
      input {
        display: block;
        height: 100%;
        width: 100%;
      }
    </style>
    <input id="file" type="file" />
    <button id="counter" style="position:absolute;left:0;top:140px;width:120px;height:80px">Counter</button>
    <script>
      const input = document.querySelector("#file");
      window.__muonFileInputChanged = false;
      window.__muonCounter = 0;
      document.querySelector("#counter").addEventListener("click", () => {
        window.__muonCounter += 1;
      });
      input.addEventListener("change", () => {
        window.__muonFileInputChanged = true;
      });
    </script>`);

const isLocalLinuxNativeDialogE2e =
  process.platform === "linux" && !isWindowsRemoteE2e();

const waitForWindowsWindowByTitle = async (title: string): Promise<AppWindow> =>
  await requireWindowsRemoteContext().agent.waitForWindow(
    {
      title,
      visible: true,
    },
    {
      intervalMs: 100,
      message: `Timed out waiting for Windows window '${title}'`,
      timeoutMs: cdpCommandTimeoutMs,
    },
  );

const waitForWindowsDialogByTitle = async (title: string): Promise<AppWindow> =>
  await requireWindowsRemoteContext().agent.waitForWindow(
    {
      title,
      visible: true,
    },
    {
      intervalMs: 100,
      message: `Timed out waiting for Windows native dialog '${title}'`,
      timeoutMs: cdpCommandTimeoutMs,
    },
  );

const waitForWindowsDialogByTitleRegex = async (
  titleRegex: RegExp,
): Promise<AppWindow> =>
  await requireWindowsRemoteContext().agent.waitForWindow(
    {
      titleRegex,
      visible: true,
    },
    {
      intervalMs: 100,
      message: `Timed out waiting for Windows native dialog matching ${String(
        titleRegex,
      )}`,
      timeoutMs: cdpCommandTimeoutMs,
    },
  );

const waitForWindowsDialogClosed = async (title: string): Promise<void> => {
  await requireWindowsRemoteContext().agent.waitForNoWindow(
    {
      title,
      visible: true,
    },
    {
      intervalMs: 100,
      message: `Timed out waiting for Windows native dialog '${title}' to close`,
      timeoutMs: cdpCommandTimeoutMs,
    },
  );
};

const clickWindowsWindowCenter = async (window: AppWindow): Promise<void> => {
  await requireWindowsRemoteContext().agent.mouse.click(
    {
      x: window.bounds.x + Math.round(window.bounds.width / 2),
      y: window.bounds.y + Math.round(window.bounds.height / 2),
    },
    { button: "left" },
  );
  await delay(100);
};

const findWindowsDialogTextInput = async (
  dialogWindow: AppWindow,
): Promise<AppWindow | undefined> => {
  const deadline = Date.now() + 5000;
  while (Date.now() < deadline) {
    const descendants = await dialogWindow.descendants({ maxDepth: 8 });
    const match = descendants.find(
      (window) =>
        window.visible &&
        window.enabled &&
        window.className === "Edit" &&
        window.bounds.width >= 80 &&
        window.bounds.height >= 12,
    );
    if (match !== undefined) {
      return match;
    }
    await delay(100);
  }
  return undefined;
};

const selectWindowsDialogFile = async (
  dialogWindow: AppWindow,
  selectedPath: string,
): Promise<void> => {
  const context = requireWindowsRemoteContext();
  try {
    await dialogWindow.activate();
    await dialogWindow.focus();
  } catch {
    // SetForegroundWindow can be denied; clicking a child control is enough.
  }
  const textInput = await findWindowsDialogTextInput(dialogWindow);
  await clickWindowsWindowCenter(textInput ?? dialogWindow);
  await context.agent.keyboard.pasteText(selectedPath, {
    restoreClipboard: true,
  });
  await delay(100);
  await context.agent.keyboard.press("Enter");
  await delay(100);
};

const findWindowsDialogButtonByLabel = async (
  dialogWindow: AppWindow,
  buttonLabel: string,
): Promise<AppWindow> => {
  const deadline = Date.now() + cdpCommandTimeoutMs;
  let lastDescendants: readonly AppWindow[] = [];
  while (Date.now() < deadline) {
    lastDescendants = await dialogWindow.descendants({ maxDepth: 8 });
    const match = lastDescendants.find(
      (window) =>
        window.visible && window.enabled && window.title === buttonLabel,
    );
    if (match !== undefined) {
      return match;
    }
    await delay(100);
  }
  throw new Error(
    `Timed out waiting for Windows native dialog button '${buttonLabel}'. Last descendants: ${JSON.stringify(
      lastDescendants.map((window) => ({
        className: window.className,
        enabled: window.enabled,
        title: window.title,
        visible: window.visible,
      })),
    )}`,
  );
};

const clickWindowsBrowserWindow = async (
  browserWindow: AppWindow,
  excludedWindow: AppWindow | undefined,
): Promise<void> => {
  await clickWindowsBrowserWindowOutsideBounds(
    browserWindow,
    excludedWindow?.bounds,
  );
};

const clickWindowsBrowserWindowOutsideBounds = async (
  browserWindow: AppWindow,
  excludedBounds: WindowBounds | undefined,
): Promise<void> => {
  const point = chooseBrowserClickPoint(browserWindow.bounds, excludedBounds);
  await requireWindowsRemoteContext().agent.mouse.click(
    {
      x: Math.round(point.x),
      y: Math.round(point.y),
    },
    { button: "left" },
  );
  await delay(100);
};

const waitForWindowsWindowEnabled = async (
  title: string,
  expectedEnabled: boolean,
): Promise<AppWindow> => {
  const deadline = Date.now() + cdpCommandTimeoutMs;
  let lastWindow: AppWindow | undefined = undefined;
  while (Date.now() < deadline) {
    lastWindow = await waitForWindowsWindowByTitle(title);
    if (lastWindow.enabled === expectedEnabled) {
      return lastWindow;
    }
    await delay(100);
  }
  throw new Error(
    `Timed out waiting for Windows window '${title}' enabled=${String(
      expectedEnabled,
    )}. Last window: ${JSON.stringify(lastWindow)}`,
  );
};

const moveWindowsDialogAwayFromBrowserClickPoint = async (
  dialogWindow: AppWindow,
  browserBounds: WindowBounds,
): Promise<AppWindow> => {
  try {
    return await dialogWindow.setBounds({
      ...dialogWindow.bounds,
      x: browserBounds.x + 160,
      y: Math.max(0, browserBounds.y + 40),
    });
  } catch {
    // Moving common dialogs is best-effort; the click probe has fallback points.
    return dialogWindow;
  } finally {
    await delay(100);
  }
};

const startWindowsNativeDialogMuon = async (
  browserAllowUnsafeJavaScriptParentAccess: string[] | null = null,
): Promise<RunningMuon> =>
  await startDebugMuon(
    [],
    undefined,
    {},
    undefined,
    undefined,
    undefined,
    undefined,
    [],
    browserAllowUnsafeJavaScriptParentAccess,
  );

describeMuonPluginBridge("muon plugin bridge - native dialogs", () => {
  it("preserves standard JavaScript dialogs and before-unload cancellation", async () => {
    const pageTitle = "muon standard dialogs";
    const local = isLocalLinuxNativeDialogE2e
      ? await startGestamentDebugMuon()
      : undefined;
    const remote =
      local === undefined ? await startWindowsNativeDialogMuon() : undefined;
    const driver =
      local?.driver ?? (await connectToMuonCdp({ port: MUON_PORT }));
    const context = isWindowsRemoteE2e()
      ? requireWindowsRemoteContext()
      : undefined;
    const client = new Client({ name: "muon-standard-dialogs", version: "1" });
    const server = await createConnection({
      browser: {
        cdpEndpoint: `http://${context?.cdpHost ?? "127.0.0.1"}:${context?.cdpPort ?? MUON_PORT}`,
      },
    });
    const [clientTransport, serverTransport] =
      InMemoryTransport.createLinkedPair();
    const call = async (
      name: string,
      args: Record<string, unknown>,
    ): Promise<string> => {
      const response = await client.callTool({ name, arguments: args });
      const text = (response.content as Array<{ text?: string }>)
        .map((item) => item.text ?? "")
        .join("\n");
      expect(response.isError, text).not.toBe(true);
      return text;
    };
    const evidence = await makeHostDirectory(
      `${hostTmpdir()}/muon-js-dialogs-`,
    );
    try {
      await driver.navigate(
        `data:text/html;charset=utf-8,${createCounterPage(pageTitle, "Counter")}`,
        cdpCommandTimeoutMs,
      );
      await server.connect(serverTransport);
      await client.connect(clientTransport);
      const tabs = await call("browser_tabs", { action: "list" });
      const tab = tabs.match(/- (\d+):[^\n]*muon standard dialogs/)?.[1];
      expect(tab, tabs).toBeDefined();
      await call("browser_tabs", { action: "select", index: Number(tab) });
      const ownerBounds = await readBrowserWindowBounds(driver);
      const captureDialog = async (name: string): Promise<void> => {
        let image = Buffer.alloc(0);
        // The CDP opening event precedes native painting. Wait for the standard
        // dialog's primary button to render before recording visual evidence.
        await expect
          .poll(
            async () => {
              const capture =
                local !== undefined
                  ? await local.app.capture()
                  : await context!.agent.screenshot();
              image = Buffer.from(capture.image);
              const png = PNG.sync.read(image);
              let bluePixels = 0;
              for (
                let y = Math.max(
                  0,
                  Math.floor(ownerBounds.y + ownerBounds.height / 4),
                );
                y <
                Math.min(
                  png.height,
                  ownerBounds.y + (ownerBounds.height * 3) / 4,
                );
                y += 1
              ) {
                for (
                  let x = Math.max(
                    0,
                    Math.floor(ownerBounds.x + ownerBounds.width / 4),
                  );
                  x <
                  Math.min(
                    png.width,
                    ownerBounds.x + (ownerBounds.width * 3) / 4,
                  );
                  x += 1
                ) {
                  const offset = (y * png.width + x) * 4;
                  const red = png.data[offset]!;
                  const green = png.data[offset + 1]!;
                  const blue = png.data[offset + 2]!;
                  if (blue > red + 50 && blue > green + 20 && red < 150)
                    bluePixels += 1;
                }
              }
              return bluePixels;
            },
            { timeout: cdpCommandTimeoutMs },
          )
          .toBeGreaterThan(500);
        await writeHostFile(`${evidence}/${name}.png`, image);
      };
      for (const scenario of [
        {
          type: "alert",
          expression:
            'alert("Alert probe"); window.dialogResult = "acknowledged"',
          accept: true,
          result: "acknowledged",
        },
        {
          type: "confirm",
          expression: 'window.dialogResult = confirm("Confirm probe")',
          accept: false,
          result: false,
        },
        {
          type: "prompt",
          expression: 'window.dialogResult = prompt("Prompt probe", "initial")',
          accept: true,
          result: "entered",
        },
      ]) {
        const opened = driver.waitForEvent<{
          type: string;
          hasBrowserHandler: boolean;
        }>("Page.javascriptDialogOpening", cdpCommandTimeoutMs);
        await call("browser_evaluate", {
          function: `() => { setTimeout(() => { ${scenario.expression}; }, 0); }`,
        });
        expect(await opened).toMatchObject({
          type: scenario.type,
          hasBrowserHandler: true,
        });
        await captureDialog(scenario.type);
        if (local !== undefined) {
          await clickBrowserWindow(local.app, ownerBounds, undefined);
        } else {
          await clickWindowsBrowserWindow(
            await waitForWindowsWindowByTitle(pageTitle),
            undefined,
          );
        }
        await call("browser_handle_dialog", {
          accept: scenario.accept,
          ...(scenario.type === "prompt" ? { promptText: "entered" } : {}),
        });
        await expect
          .poll(async () => await driver.evaluate("window.dialogResult"))
          .toEqual(scenario.result);
        expect(await driver.evaluate("window.__muonCounter")).toBe(0);
      }
      await call("browser_evaluate", {
        function:
          '() => { window.onbeforeunload = (event) => { event.preventDefault(); event.returnValue = ""; }; }',
      });
      // A native user interaction supplies the activation required by Chromium.
      if (local !== undefined) {
        await clickBrowserWindow(
          local.app,
          await readBrowserWindowBounds(driver),
          undefined,
        );
      } else {
        await clickWindowsBrowserWindow(
          await waitForWindowsWindowByTitle(pageTitle),
          undefined,
        );
      }
      const opening = driver.waitForEvent<{
        type: string;
        hasBrowserHandler: boolean;
      }>("Page.javascriptDialogOpening", cdpCommandTimeoutMs);
      await call("browser_evaluate", {
        function: "() => { setTimeout(() => { location.reload(); }, 0); }",
      });
      expect(await opening).toMatchObject({
        type: "beforeunload",
        hasBrowserHandler: true,
      });
      await captureDialog("beforeunload");
      await call("browser_handle_dialog", { accept: false });
      expect(await driver.evaluate("document.title")).toBe(pageTitle);
      expect(await driver.evaluate("window.__muonCounter")).toBe(1);
      await call("browser_evaluate", {
        function: "() => { window.onbeforeunload = null; }",
      });
    } finally {
      await client.close();
      await server.close();
      if (local !== undefined) await stopGestamentMuon(local);
      if (remote !== undefined) {
        await stopMuon(remote, driver);
      }
    }
  });

  if (isLocalLinuxNativeDialogE2e) {
    it("aborts an earlier GTK file dialog while a later dialog remains open through AbortSignal", async () => {
      const running = await startGestamentDebugMuon();
      const client = new Client({
        name: "muon-dialog-lifecycle",
        version: "1",
      });
      const server = await createConnection({
        browser: { cdpEndpoint: `http://127.0.0.1:${MUON_PORT}` },
      });
      const [clientTransport, serverTransport] =
        InMemoryTransport.createLinkedPair();
      const call = async (
        name: string,
        args: Record<string, unknown>,
      ): Promise<string> => {
        const response = await client.callTool({ name, arguments: args });
        const content = response.content as Array<{
          type: string;
          text?: string;
        }>;
        const result = content.map((item) => item.text ?? "").join("\n");
        expect(response.isError, result).not.toBe(true);
        return result;
      };
      try {
        await running.driver.navigate(
          `data:text/html;charset=utf-8,${createCounterPage("muon abort lifecycle", "Counter")}`,
          cdpCommandTimeoutMs,
        );
        await waitForMuonFsSelectFile(running.driver, cdpCommandTimeoutMs);
        await server.connect(serverTransport);
        await client.connect(clientTransport);
        const tabs = await call("browser_tabs", { action: "list" });
        const tab = tabs.match(/- (\d+):[^\n]*muon abort lifecycle/)?.[1];
        expect(tab, tabs).toBeDefined();
        await call("browser_tabs", { action: "select", index: Number(tab) });
        await call("browser_evaluate", {
          function: `() => {
            window.abortDialogs = ["earlier", "later"].map((title) => {
              const controller = new AbortController();
              const state = { controller, status: "pending", completion: undefined };
              state.completion = (async () => {
                try {
                  await window.muon.fs.dialogs.selectFile({
                    title: "muon abort " + title,
                    signal: controller.signal,
                  });
                  state.status = "selected";
                } catch (error) {
                  state.status = error.name;
                }
                return state.status;
              })();
              return state;
            });
          }`,
        });
        await findGestamentNativeWindow(
          running.app,
          "muon abort earlier",
          cdpCommandTimeoutMs,
        );
        await findGestamentNativeWindow(
          running.app,
          "muon abort later",
          cdpCommandTimeoutMs,
        );
        const first = await call("browser_evaluate", {
          function: `async () => {
            window.abortDialogs[0].controller.abort();
            return await window.abortDialogs[0].completion;
          }`,
        });
        expect(first).toContain("AbortError");
        await waitForGestamentNativeWindowClosed(
          running.app,
          "muon abort earlier",
          cdpCommandTimeoutMs,
        );
        expect(
          await call("browser_evaluate", {
            function: `() => window.abortDialogs[1].status`,
          }),
        ).toContain("pending");
        await findGestamentNativeWindow(
          running.app,
          "muon abort later",
          cdpCommandTimeoutMs,
        );
        const second = await call("browser_evaluate", {
          function: `async () => {
            window.abortDialogs[1].controller.abort();
            return await window.abortDialogs[1].completion;
          }`,
        });
        expect(second).toContain("AbortError");
        await waitForGestamentNativeWindowClosed(
          running.app,
          "muon abort later",
          cdpCommandTimeoutMs,
        );
        await clickBrowserWindow(
          running.app,
          await readBrowserWindowBounds(running.driver),
          undefined,
        );
        expect(await running.driver.evaluate("window.__muonCounter")).toBe(1);
      } finally {
        await client.close();
        await server.close();
        await stopGestamentMuon(running);
      }
    });

    it("keeps the opener browser view clickable while an opener popup is open", async () => {
      const pageTitle = "muon opener popup parent input";
      const buttonLabel = "muon Parent Input Counter";
      const popupUrl = `${MUON_APP_URL}#popup=connected`;
      const popupFeatures = "width=240,height=180";
      const running = await startGestamentDebugMuon(["asset://main/**"]);
      let popupDriver: CdpDriver | undefined = undefined;
      try {
        await running.driver.navigate(MUON_APP_URL, cdpCommandTimeoutMs);
        await running.driver.evaluate(`(() => {
          document.title = ${JSON.stringify(pageTitle)};
          document.body.innerHTML = "";
          const style = document.createElement("style");
          style.textContent =
            "html,body{height:100%;margin:0}button{height:100%;width:100%}";
          document.head.appendChild(style);
          const button = document.createElement("button");
          button.textContent = ${JSON.stringify(buttonLabel)};
          window.__muonCounter = 0;
          button.addEventListener("click", () => {
            window.__muonCounter += 1;
          });
          document.body.appendChild(button);
        })()`);
        const browserBounds = await readBrowserWindowBounds(running.driver);
        const previousTargetIds = await getCurrentTargetIds();
        const response = await running.driver.send<RuntimeEvaluateResponse>(
          "Runtime.evaluate",
          {
            expression: `(() => {
              const popup = window.open(
                ${JSON.stringify(popupUrl)},
                "muonGestamentOpenerPopup",
                ${JSON.stringify(popupFeatures)}
              );
              window.__muonGestamentPopup = popup;
              window.focus();
              return {
                returnedNull: popup === null,
                returnedHasOpener:
                  popup !== null && popup.opener !== null,
              };
            })()`,
            returnByValue: true,
            userGesture: true,
          },
        );
        if (response.exceptionDetails !== undefined) {
          throw new Error("opener popup open script failed");
        }
        expect(response.result?.value).toEqual({
          returnedNull: false,
          returnedHasOpener: true,
        });
        const popupTarget = await waitForNewPageTarget(
          previousTargetIds,
          targetTimeoutMs,
          (target) => target.url === popupUrl,
        );
        popupDriver = await connectToMuonCdp({
          port: MUON_PORT,
          targetId: popupTarget.id,
          timeoutMs: cdpCommandTimeoutMs,
        });
        await expect(
          popupDriver.evaluate("window.opener !== null"),
        ).resolves.toBe(true);
        await popupDriver.evaluate("window.muon.browser.hide()");
        await expect(
          popupDriver.evaluate("document.location.href"),
        ).resolves.toBe(popupUrl);

        await expect(
          running.driver.evaluate("window.__muonCounter"),
        ).resolves.toBe(0);
        await running.driver.evaluate("window.muon.browser.focus()");
        await delay(200);
        await clickBrowserWindow(running.app, browserBounds, undefined);
        const deadline = Date.now() + cdpCommandTimeoutMs;
        let counter = 0;
        while (Date.now() < deadline) {
          counter = await running.driver.evaluate<number>(
            "window.__muonCounter",
          );
          if (counter === 1) {
            break;
          }
          await delay(100);
        }
        expect(counter).toBe(1);
        await popupDriver.evaluate(`(() => {
          document.title = "muon independent owner";
          document.body.innerHTML = '<button style="position:fixed;inset:0;width:100%;height:100%">Independent</button>';
          window.independentClicks = 0;
          document.querySelector("button").onclick = () => { window.independentClicks += 1; };
        })()`);
        await popupDriver.evaluate("window.muon.browser.show()");
        await running.driver.evaluate(`(() => {
          window.ownerDialogAbort = new AbortController();
          window.ownerDialogDone = (async () => {
            try { await window.muon.fs.dialogs.selectFile({ title: "muon other owner remains usable", signal: window.ownerDialogAbort.signal }); }
            catch (error) { return error.name; }
          })();
        })()`);
        await findGestamentNativeWindow(
          running.app,
          "muon other owner remains usable",
          cdpCommandTimeoutMs,
        );
        await popupDriver.evaluate("window.muon.browser.focus()");
        await sendNativeClickWithoutFocus("muon independent owner", 32, 96, {
          ...process.env,
          ...(await running.app.environment()),
        });
        await expect
          .poll(
            async () => await popupDriver!.evaluate("window.independentClicks"),
          )
          .toBe(1);
        expect(await running.driver.evaluate("window.__muonCounter")).toBe(1);
        expect(
          await running.driver.evaluate(
            "(async () => { window.ownerDialogAbort.abort(); return await window.ownerDialogDone; })()",
          ),
        ).toBe("AbortError");
      } finally {
        popupDriver?.close();
        await stopGestamentMuon(running);
      }
    });

    it("detects a GTK file dialog with Gestament", async () => {
      const pageTitle = "muon gestament dialog test";
      const title = "muon Gestament File Dialog Test";
      const buttonLabel = "muon Probe Select";
      const testDirectory = await mkdtemp(
        join(tmpdir(), "muon-gestament-dialog-"),
      );
      const selectedPath = join(testDirectory, "selected.txt");
      await writeFile(selectedPath, "selected", "utf8");
      const running = await startGestamentDebugMuon();
      try {
        await running.driver.navigate(
          `data:text/html,<title>${pageTitle}</title>`,
          cdpCommandTimeoutMs,
        );
        await waitForMuonFsSelectFile(running.driver, cdpCommandTimeoutMs);
        await startNativeFileDialogProbe(running.driver, {
          buttonLabel,
          defaultPath: selectedPath,
          title,
        });

        const match = await findGestamentNativeWindow(
          running.app,
          title,
          cdpCommandTimeoutMs,
        );
        const matched = match.diagnostics.find(
          (diagnostic) =>
            diagnostic.name === title || diagnostic.title === title,
        );
        expect(matched).toMatchObject({
          error: undefined,
          kind: "window",
        });
        expect(
          matched?.seenBy?.includes("x11") === true ||
            matched?.seenBy?.includes("at-spi") === true,
        ).toBe(true);

        const button = await findGestamentNativeDialogButtonByLabel(
          running.app,
          match.window,
          buttonLabel,
          cdpCommandTimeoutMs,
        );
        expect(button.label).toBe(buttonLabel);
        expect(["semantic", "windowText"]).toContain(button.detection);
        await button.click();

        await expect(
          readNativeFileDialogProbeResult(running.driver),
        ).resolves.toEqual({
          status: "fulfilled",
          value: selectedPath,
        });
        await waitForGestamentNativeWindowClosed(
          running.app,
          title,
          cdpCommandTimeoutMs,
        );
      } finally {
        await stopGestamentMuon(running);
        await rm(testDirectory, { recursive: true, force: true });
      }
    });

    it("returns to an overwrite confirmation and aborts its entire dialog branch", async () => {
      const directory = await mkdtemp(join(tmpdir(), "muon-overwrite-"));
      await writeFile(join(directory, "existing.txt"), "keep", "utf8");
      const running = await startGestamentDebugMuon();
      const environment = {
        ...process.env,
        ...(await running.app.environment()),
      };
      try {
        await running.driver.navigate(
          `data:text/html;charset=utf-8,${createCounterPage("muon overwrite owner", "Counter")}`,
          cdpCommandTimeoutMs,
        );
        await waitForMuonFsSelectFile(running.driver, cdpCommandTimeoutMs);
        await running.driver.evaluate(`(() => {
          window.saveController = new AbortController();
          window.saveCompletion = (async () => {
            try {
              await window.muon.fs.dialogs.selectSaveFile({ title: "muon overwrite save", defaultPath: ${JSON.stringify(directory)}, defaultName: "existing.txt", signal: window.saveController.signal });
              return "selected";
            } catch (error) { return error.name; }
          })();
        })()`);
        const chooser = await findGestamentNativeWindow(
          running.app,
          "muon overwrite save",
          cdpCommandTimeoutMs,
        );
        const chooserId = Number((await chooser.window.x11Info()).windowId);
        const save = await findGestamentNativeDialogButtonByLabel(
          running.app,
          chooser.window,
          "Save",
          cdpCommandTimeoutMs,
        );
        await save.click();
        let confirmationId = "";
        await expect
          .poll(
            async () => {
              const clients = await execFileAsync(
                "xprop",
                ["-root", "_NET_CLIENT_LIST"],
                { env: environment },
              );
              for (const id of clients.stdout.match(/0x[0-9a-f]+/gi) ?? []) {
                const property = await execFileAsync(
                  "xprop",
                  ["-id", String(id), "WM_TRANSIENT_FOR"],
                  { env: environment },
                );
                if (
                  Number(/0x[0-9a-f]+/i.exec(property.stdout)?.[0]) ===
                  chooserId
                ) {
                  confirmationId = id;
                  return true;
                }
              }
              return false;
            },
            { timeout: cdpCommandTimeoutMs },
          )
          .toBe(true);
        await sendNativeClickWithoutFocus(
          "muon overwrite owner",
          32,
          96,
          environment,
        );
        await expect
          .poll(
            async () =>
              await isNativeWindowFocused(confirmationId, environment),
          )
          .toBe(true);
        expect(await running.driver.evaluate("window.__muonCounter")).toBe(0);
        expect(
          await running.driver.evaluate(
            "(async () => { window.saveController.abort(); return await window.saveCompletion; })()",
          ),
        ).toBe("AbortError");
        await waitForGestamentNativeWindowClosed(
          running.app,
          "muon overwrite save",
          cdpCommandTimeoutMs,
        );
        const remaining = await execFileAsync("xwininfo", ["-root", "-tree"], {
          env: environment,
        });
        expect(remaining.stdout).not.toContain(
          `0x${Number(confirmationId).toString(16)}`,
        );
        await clickBrowserWindow(
          running.app,
          await readBrowserWindowBounds(running.driver),
          undefined,
        );
        expect(await running.driver.evaluate("window.__muonCounter")).toBe(1);
      } finally {
        await stopGestamentMuon(running);
        await rm(directory, { recursive: true, force: true });
      }
    });

    it("uses the muon UI dialog provider for CEF file inputs", async () => {
      const pageTitle = "muon cef file input dialog";
      const dialogTitle = "Open";
      const directory = await mkdtemp(join(tmpdir(), "muon-html-file-"));
      const selectedPath = join(directory, "selected.txt");
      await writeFile(selectedPath, "HTML selection", "utf8");
      const running = await startGestamentDebugMuon();
      try {
        await running.driver.navigate(
          `data:text/html;charset=utf-8,${createFileInputPage(pageTitle)}`,
          cdpCommandTimeoutMs,
        );
        const browserBounds = await readBrowserWindowBounds(running.driver);
        await clickBrowserWindow(running.app, browserBounds, undefined);

        const match = await findGestamentNativeWindow(
          running.app,
          dialogTitle,
          cdpCommandTimeoutMs,
        );
        const environment = {
          ...process.env,
          ...(await running.app.environment()),
        };
        const owner = await execFileAsync(
          "xdotool",
          ["search", "--name", `^${pageTitle}$`],
          { env: environment },
        );
        const dialogId = String((await match.window.x11Info()).windowId);
        const ownership = await execFileAsync(
          "xprop",
          ["-id", dialogId, "WM_TRANSIENT_FOR"],
          { env: environment },
        );
        expect(Number(/0x[0-9a-f]+/i.exec(ownership.stdout)?.[0])).toBe(
          Number(owner.stdout.trim()),
        );
        await sendNativeClickWithoutFocus(pageTitle, 32, 180, environment);
        await expect
          .poll(async () => await isNativeWindowFocused(dialogId, environment))
          .toBe(true);
        expect(
          await running.driver.evaluate<number>("window.__muonCounter"),
        ).toBe(0);
        const cancelButton = await findGestamentNativeDialogButtonByLabel(
          running.app,
          match.window,
          "Cancel",
          cdpCommandTimeoutMs,
        );
        await cancelButton.click();
        await waitForGestamentNativeWindowClosed(
          running.app,
          dialogTitle,
          cdpCommandTimeoutMs,
        );

        await expect(
          running.driver.evaluate<{
            changed: boolean;
            fileCount: number;
          }>(`({
            changed: window.__muonFileInputChanged,
            fileCount: document.querySelector("#file").files.length,
          })`),
        ).resolves.toEqual({
          changed: false,
          fileCount: 0,
        });
        await sendNativeClickWithoutFocus(pageTitle, 32, 180, environment);
        await expect
          .poll(
            async () =>
              await running.driver.evaluate<number>("window.__muonCounter"),
          )
          .toBe(1);
        await clickBrowserWindow(running.app, browserBounds, undefined);
        const selection = await findGestamentNativeWindow(
          running.app,
          dialogTitle,
          cdpCommandTimeoutMs,
        );
        const selectionId = String((await selection.window.x11Info()).windowId);
        await expect
          .poll(
            async () => await isNativeWindowFocused(selectionId, environment),
          )
          .toBe(true);
        await running.app.input.setModifier("control", true);
        try {
          await running.app.input.pressKey("l");
        } finally {
          await running.app.input.setModifier("control", false);
        }
        await execFileAsync(
          "xdotool",
          ["type", "--clearmodifiers", "--delay", "0", selectedPath],
          { env: environment },
        );
        const openButton = await findGestamentNativeDialogButtonByLabel(
          running.app,
          selection.window,
          "Open",
          cdpCommandTimeoutMs,
        );
        await openButton.click();
        await expect
          .poll(
            async () =>
              await running.driver.evaluate("window.__muonFileInputChanged"),
            { timeout: cdpCommandTimeoutMs },
          )
          .toBe(true);
        expect(
          await running.driver.evaluate(
            'document.querySelector("#file").files[0].text()',
          ),
        ).toBe("HTML selection");
        await clickBrowserWindow(running.app, browserBounds, undefined);
        await findGestamentNativeWindow(
          running.app,
          dialogTitle,
          cdpCommandTimeoutMs,
        );
        await closeOwnerBrowserWindow(running.driver);
        await expectGestamentMuonExitedNormally(running.app);
      } finally {
        await stopGestamentMuon(running);
        await rm(directory, { recursive: true, force: true });
      }
    });

    it("disables the opener browser view while a default modal GTK file dialog is open", async () => {
      const pageTitle = "muon modal dialog disables opener";
      const title = "muon Modal Disable Probe";
      const buttonLabel = "muon Modal Probe Select";
      const pageButtonLabel = "muon Page Counter Button";
      const testDirectory = await mkdtemp(join(tmpdir(), "muon-modal-dialog-"));
      const selectedPath = join(testDirectory, "selected.txt");
      await writeFile(selectedPath, "selected", "utf8");
      const running = await startGestamentDebugMuon();
      const environment = {
        ...process.env,
        ...(await running.app.environment()),
      };
      const competitor = spawn(
        "xmessage",
        [
          "-title",
          "muon modal competitor",
          "-geometry",
          "500x400+500+150",
          "Independent window",
        ],
        { env: environment, stdio: "ignore" },
      );
      let recording: ReturnType<typeof spawn> | undefined;
      let recordingFinished: Promise<number | null> | undefined;
      let recordingError = "";
      try {
        const page = createCounterPage(pageTitle, pageButtonLabel);
        await running.driver.navigate(
          `data:text/html;charset=utf-8,${page}`,
          cdpCommandTimeoutMs,
        );
        const browserBounds = await readBrowserWindowBounds(running.driver);
        await sendNativeClickWithoutFocus(pageTitle, 32, 96, environment);
        await expect
          .poll(
            async () =>
              await running.driver.evaluate<number>("window.__muonCounter"),
          )
          .toBe(1);
        await running.driver.evaluate("window.__muonCounter = 0");
        await waitForMuonFsSelectFile(running.driver, cdpCommandTimeoutMs);
        await startNativeFileDialogProbe(running.driver, {
          buttonLabel,
          defaultPath: selectedPath,
          title,
        });

        const match = await findGestamentNativeWindow(
          running.app,
          title,
          cdpCommandTimeoutMs,
        );
        const dialogButton = await findGestamentNativeDialogButtonByLabel(
          running.app,
          match.window,
          buttonLabel,
          cdpCommandTimeoutMs,
        );
        await moveDialogAwayFromBrowserClickPoint(match.window, browserBounds);
        const dialogBounds = await match.window.bounds();
        const videoDirectory = await mkdtemp(
          join(tmpdir(), "muon-modal-video-"),
        );
        const capture = await running.app.capture();
        await writeFile(join(videoDirectory, "reference.png"), capture.image);
        const reference = PNG.sync.read(capture.image);
        const area = {
          x: dialogBounds.x + 250,
          y: dialogBounds.y + 76,
          width: 200,
          height: 16,
        };
        const frameSize = reference.width * reference.height * 3;
        const ratios: number[] = [];
        const phases: { label: string; frame: number }[] = [];
        let pending: Buffer = Buffer.alloc(0);
        recording = spawn(
          "ffmpeg",
          [
            "-hide_banner",
            "-loglevel",
            "error",
            "-f",
            "x11grab",
            "-framerate",
            "15",
            "-draw_mouse",
            "0",
            "-video_size",
            `${reference.width}x${reference.height}`,
            "-i",
            environment.DISPLAY ?? "",
            "-map",
            "0:v",
            "-c:v",
            "ffv1",
            "-pix_fmt",
            "bgr0",
            join(videoDirectory, "modal-input.mkv"),
            "-map",
            "0:v",
            "-c:v",
            "rawvideo",
            "-pix_fmt",
            "rgb24",
            "-f",
            "rawvideo",
            "pipe:1",
          ],
          { env: environment, stdio: ["pipe", "pipe", "pipe"] },
        );
        recordingFinished = new Promise((resolve) => {
          recording?.once("error", (error) => {
            recordingError += String(error);
            resolve(null);
          });
          recording?.once("close", resolve);
        });
        recording.stderr?.on("data", (bytes: Buffer) => {
          recordingError += bytes.toString();
        });
        recording.stdout?.on("data", (bytes: Buffer) => {
          pending = Buffer.concat([pending, bytes]);
          while (pending.length >= frameSize) {
            const frame = pending.subarray(0, frameSize);
            pending = pending.subarray(frameSize);
            let different = 0;
            for (let y = area.y; y < area.y + area.height; ++y) {
              for (let x = area.x; x < area.x + area.width; ++x) {
                const pixel = y * reference.width + x;
                if (
                  [0, 1, 2].some(
                    (channel) =>
                      Math.abs(
                        (frame[pixel * 3 + channel] ?? 0) -
                          (reference.data[pixel * 4 + channel] ?? 0),
                      ) > 12,
                  )
                )
                  ++different;
              }
            }
            ratios.push(different / (area.width * area.height));
          }
        });
        const expectVideo = async (
          label: string,
          visible: boolean,
        ): Promise<void> => {
          const after = ratios.length;
          await expect
            .poll(
              () => {
                expect(recording?.exitCode, recordingError).toBeNull();
                const samples = ratios.slice(
                  Math.max(after, ratios.length - 6),
                );
                return (
                  samples.length === 6 &&
                  samples.every((ratio) =>
                    visible ? ratio < 0.04 : ratio > 0.2,
                  )
                );
              },
              {
                timeout: cdpCommandTimeoutMs,
                message: `${label}: ${videoDirectory}`,
              },
            )
            .toBe(true);
          phases.push({ label, frame: ratios.length });
          await writeFile(
            join(videoDirectory, "phases.json"),
            JSON.stringify(phases, null, 2),
          );
        };
        await expectVideo("initial dialog", true);
        const { stdout: competitorId } = await execFileAsync(
          "xdotool",
          [
            "search",
            "--sync",
            "--onlyvisible",
            "--name",
            "^muon modal competitor$",
          ],
          { env: environment },
        );
        await execFileAsync(
          "xdotool",
          [
            "windowmove",
            competitorId.trim(),
            String(area.x - 20),
            String(area.y - 20),
          ],
          { env: environment },
        );
        await execFileAsync(
          "xdotool",
          [
            "windowactivate",
            "--sync",
            competitorId.trim(),
            "windowfocus",
            "--sync",
            competitorId.trim(),
          ],
          { env: environment },
        );
        expect(
          (
            await execFileAsync("xdotool", ["getwindowfocus"], {
              env: environment,
            })
          ).stdout.trim(),
        ).toBe(competitorId.trim());
        await expectVideo("covered by another window", false);
        await sendNativeClickWithoutFocus(pageTitle, 32, 96, environment);
        const countWhileDialogOpen = await running.driver.evaluate<number>(
          "window.__muonCounter",
        );
        const dialogId = String(
          Number((await match.window.x11Info()).windowId),
        );
        await expect
          .poll(
            async () => await isNativeWindowFocused(dialogId, environment),
            { timeout: cdpCommandTimeoutMs },
          )
          .toBe(true);
        await expectVideo("owner click without activation", true);

        const { stdout: ownerId } = await execFileAsync(
          "xdotool",
          ["search", "--name", `^${pageTitle}$`],
          { env: environment },
        );
        // Each attempt starts with an independent window covering the chooser.
        // Parent activation, native client clicks and custom controls must all
        // return to the chooser without executing the parent's action.
        for (const interaction of [
          "focus API",
          "window activation",
          "client click",
          "minimize control",
          "maximize control",
          "close control",
        ]) {
          await execFileAsync(
            "xdotool",
            ["windowactivate", "--sync", competitorId.trim()],
            { env: environment, timeout: cdpCommandTimeoutMs },
          );
          await expectVideo(`${interaction}: covered`, false);
          if (interaction === "focus API") {
            await running.driver.evaluate("window.muon.browser.focus()");
          } else if (interaction === "window activation") {
            await execFileAsync("xdotool", ["windowactivate", ownerId.trim()], {
              env: environment,
            });
          } else if (interaction === "client click") {
            await clickBrowserWindow(running.app, browserBounds, match.window);
          } else {
            const offset =
              interaction === "minimize control"
                ? 82
                : interaction === "maximize control"
                  ? 50
                  : 18;
            await running.app.input.moveMouseTo(
              browserBounds.x + browserBounds.width - offset,
              browserBounds.y + 18,
            );
            await running.app.input.setMouseButton("left", true);
            await running.app.input.setMouseButton("left", false);
          }
          await expect
            .poll(
              async () => await isNativeWindowFocused(dialogId, environment),
              { timeout: cdpCommandTimeoutMs },
            )
            .toBe(true);
          await expectVideo(`${interaction}: dialog restored`, true);
          expect(await readBrowserWindowBounds(running.driver)).toEqual(
            browserBounds,
          );
          expect(
            await running.driver.evaluate<number>("window.__muonCounter"),
          ).toBe(0);
        }

        const targetsBeforeKeys = await getCurrentTargetIds();
        for (const key of ["F5", "F12"]) {
          await execFileAsync(
            "xdotool",
            ["windowactivate", "--sync", competitorId.trim()],
            { env: environment, timeout: cdpCommandTimeoutMs },
          );
          await execFileAsync(
            "xdotool",
            ["windowfocus", ownerId.trim(), "key", key],
            { env: environment },
          );
          await expect
            .poll(
              async () => await isNativeWindowFocused(dialogId, environment),
              { timeout: cdpCommandTimeoutMs, message: key },
            )
            .toBe(true);
          expect(
            await running.driver.evaluate<number>("window.__muonCounter"),
          ).toBe(0);
          expect(await getCurrentTargetIds()).toEqual(targetsBeforeKeys);
          await expectVideo(`${key}: dialog restored`, true);
        }

        const enlargedBounds = {
          ...browserBounds,
          width: browserBounds.width + 64,
          height: browserBounds.height + 64,
        };
        await running.driver.evaluate(
          `window.muon.browser.setWindowBounds(${JSON.stringify(enlargedBounds)})`,
        );
        await expect
          .poll(async () => await readBrowserWindowBounds(running.driver))
          .toEqual(enlargedBounds);
        await execFileAsync(
          "xdotool",
          ["windowactivate", "--sync", competitorId.trim()],
          { env: environment, timeout: cdpCommandTimeoutMs },
        );
        await running.app.input.moveMouseTo(
          enlargedBounds.x + 32,
          enlargedBounds.y + enlargedBounds.height - 16,
        );
        await running.app.input.setMouseButton("left", true);
        await running.app.input.setMouseButton("left", false);
        await expect
          .poll(
            async () => await isNativeWindowFocused(dialogId, environment),
            { timeout: cdpCommandTimeoutMs },
          )
          .toBe(true);
        expect(
          await running.driver.evaluate<number>("window.__muonCounter"),
        ).toBe(0);
        await expectVideo("resized owner: dialog restored", true);
        await running.driver.evaluate(
          `window.muon.browser.setWindowBounds(${JSON.stringify(browserBounds)})`,
        );

        await dialogButton.click();
        await expect(
          readNativeFileDialogProbeResult(running.driver),
        ).resolves.toEqual({
          status: "fulfilled",
          value: selectedPath,
        });
        await waitForGestamentNativeWindowClosed(
          running.app,
          title,
          cdpCommandTimeoutMs,
        );

        await expect
          .poll(
            async () =>
              await running.driver.evaluate<{
                focused: boolean;
                active: string;
              }>(
                "({ focused: document.hasFocus(), active: document.activeElement?.tagName })",
              ),
            {
              timeout: cdpCommandTimeoutMs,
              message:
                "Original browser focus should resume after dialog completion",
            },
          )
          .toEqual({ focused: true, active: "BUTTON" });
        await running.app.input.pressKey("Return");
        await expect
          .poll(
            async () =>
              await running.driver.evaluate<number>("window.__muonCounter"),
            {
              timeout: cdpCommandTimeoutMs,
              message:
                "Original browser focus should resume after dialog completion",
            },
          )
          .toBe(1);

        await clickBrowserWindow(running.app, browserBounds, undefined);
        const countAfterDialogClosed = await running.driver.evaluate<number>(
          "window.__muonCounter",
        );

        expect(countWhileDialogOpen).toBe(0);
        expect(countAfterDialogClosed).toBe(2);
      } finally {
        recording?.stdin?.write("q\n");
        const recordingExit =
          recordingFinished === undefined ? 0 : await recordingFinished;
        competitor.kill();
        await stopGestamentMuon(running);
        await rm(testDirectory, { recursive: true, force: true });
        expect(recordingExit, recordingError).toBe(0);
      }
    });

    it("keeps the opener browser view enabled while a non-modal GTK file dialog is open", async () => {
      const pageTitle = "muon non-modal dialog keeps opener enabled";
      const title = "muon Non Modal Disable Probe";
      const buttonLabel = "muon Non Modal Probe Select";
      const pageButtonLabel = "muon Non Modal Page Counter Button";
      const testDirectory = await mkdtemp(
        join(tmpdir(), "muon-non-modal-dialog-"),
      );
      const selectedPath = join(testDirectory, "selected.txt");
      await writeFile(selectedPath, "selected", "utf8");
      const running = await startGestamentDebugMuon();
      try {
        const page = createCounterPage(pageTitle, pageButtonLabel);
        await running.driver.navigate(
          `data:text/html;charset=utf-8,${page}`,
          cdpCommandTimeoutMs,
        );
        const browserBounds = await readBrowserWindowBounds(running.driver);
        await waitForMuonFsSelectFile(running.driver, cdpCommandTimeoutMs);
        await startNativeFileDialogProbe(running.driver, {
          buttonLabel,
          defaultPath: selectedPath,
          modal: false,
          title,
        });

        const match = await findGestamentNativeWindow(
          running.app,
          title,
          cdpCommandTimeoutMs,
        );
        const dialogButton = await findGestamentNativeDialogButtonByLabel(
          running.app,
          match.window,
          buttonLabel,
          cdpCommandTimeoutMs,
        );
        await moveDialogAwayFromBrowserClickPoint(match.window, browserBounds);
        await clickBrowserWindow(running.app, browserBounds, match.window);
        const countWhileDialogOpen = await running.driver.evaluate<number>(
          "window.__muonCounter",
        );
        try {
          await match.window.activate();
        } catch {
          // Activation is best-effort; the next step can still find the button.
        }

        await dialogButton.click();
        await expect(
          readNativeFileDialogProbeResult(running.driver),
        ).resolves.toEqual({
          status: "fulfilled",
          value: selectedPath,
        });
        await waitForGestamentNativeWindowClosed(
          running.app,
          title,
          cdpCommandTimeoutMs,
        );

        expect(countWhileDialogOpen).toBe(1);
      } finally {
        await stopGestamentMuon(running);
        await rm(testDirectory, { recursive: true, force: true });
      }
    });

    it("closes a default modal GTK file dialog when the opener browser closes", async () => {
      const pageTitle = "muon modal dialog owner close";
      const title = "muon Modal Owner Close Probe";
      const buttonLabel = "muon Modal Owner Close Select";
      const testDirectory = await mkdtemp(
        join(tmpdir(), "muon-modal-owner-close-"),
      );
      const selectedPath = join(testDirectory, "selected.txt");
      await writeFile(selectedPath, "selected", "utf8");
      const running = await startGestamentDebugMuon();
      try {
        await running.driver.navigate(
          `data:text/html,<title>${pageTitle}</title>`,
          cdpCommandTimeoutMs,
        );
        await waitForMuonFsSelectFile(running.driver, cdpCommandTimeoutMs);
        await startNativeFileDialogProbe(running.driver, {
          buttonLabel,
          defaultPath: selectedPath,
          title,
        });

        await findGestamentNativeWindow(
          running.app,
          title,
          cdpCommandTimeoutMs,
        );
        await closeOwnerBrowserWindow(running.driver);
        await expectGestamentMuonExitedNormally(running.app);
      } finally {
        await stopGestamentMuon(running);
        await rm(testDirectory, { recursive: true, force: true });
      }
    });

    it("does not reveal a hidden owner when its modal dialog is canceled", async () => {
      const running = await startGestamentDebugMuon();
      const environment = {
        ...process.env,
        ...(await running.app.environment()),
      };
      try {
        await running.driver.navigate(
          `data:text/html;charset=utf-8,${createCounterPage("muon hidden owner", "Counter")}`,
          cdpCommandTimeoutMs,
        );
        const owner = await execFileAsync(
          "xdotool",
          ["search", "--name", "^muon hidden owner$"],
          { env: environment },
        );
        const ownerId = owner.stdout.trim();
        await running.driver.evaluate(`(() => {
          window.hiddenDialogAbort = new AbortController();
          window.hiddenDialogDone = (async () => {
            try { await window.muon.fs.dialogs.selectFile({ title: "muon hidden owner dialog", signal: window.hiddenDialogAbort.signal }); }
            catch (error) { return error.name; }
          })();
        })()`);
        await findGestamentNativeWindow(
          running.app,
          "muon hidden owner dialog",
          cdpCommandTimeoutMs,
        );
        await running.driver.evaluate("window.muon.browser.hide()");
        await expect
          .poll(
            async () =>
              (
                await execFileAsync("xwininfo", ["-id", ownerId], {
                  env: environment,
                })
              ).stdout,
          )
          .toContain("IsUnMapped");
        expect(
          await running.driver.evaluate(
            "(async () => { window.hiddenDialogAbort.abort(); return await window.hiddenDialogDone; })()",
          ),
        ).toBe("AbortError");
        await waitForGestamentNativeWindowClosed(
          running.app,
          "muon hidden owner dialog",
          cdpCommandTimeoutMs,
        );
        expect(
          (
            await execFileAsync("xwininfo", ["-id", ownerId], {
              env: environment,
            })
          ).stdout,
        ).toContain("IsUnMapped");
        await running.driver.evaluate("window.muon.browser.show()");
        await clickBrowserWindow(
          running.app,
          await readBrowserWindowBounds(running.driver),
          undefined,
        );
        expect(await running.driver.evaluate("window.__muonCounter")).toBe(1);
      } finally {
        await stopGestamentMuon(running);
      }
    });

    it("keeps a non-modal GTK file dialog open when the opener browser closes", async () => {
      const pageTitle = "muon non-modal dialog owner close";
      const title = "muon Non Modal Owner Close Probe";
      const buttonLabel = "muon Non Modal Owner Close Select";
      const testDirectory = await mkdtemp(
        join(tmpdir(), "muon-non-modal-owner-close-"),
      );
      const selectedPath = join(testDirectory, "selected.txt");
      await writeFile(selectedPath, "selected", "utf8");
      const running = await startGestamentDebugMuon();
      try {
        await running.driver.navigate(
          `data:text/html,<title>${pageTitle}</title>`,
          cdpCommandTimeoutMs,
        );
        await waitForMuonFsSelectFile(running.driver, cdpCommandTimeoutMs);
        await startNativeFileDialogProbe(running.driver, {
          buttonLabel,
          defaultPath: selectedPath,
          modal: false,
          title,
        });

        await closeOwnerBrowserWindow(running.driver);
        await delay(500);
        const remainingMatch = await findGestamentNativeWindow(
          running.app,
          title,
          cdpCommandTimeoutMs,
        );
        expect(remainingMatch).toBeDefined();
        const dialogButton = await findGestamentNativeDialogButtonByLabel(
          running.app,
          remainingMatch.window,
          buttonLabel,
          cdpCommandTimeoutMs,
        );

        await dialogButton.click();
        await expectGestamentMuonExitedNormally(running.app);
      } finally {
        await stopGestamentMuon(running);
        await rm(testDirectory, { recursive: true, force: true });
      }
    });
  }

  if (isWindowsRemoteE2e()) {
    it("keeps the opener browser view clickable while an opener popup is open on Windows", async () => {
      const pageTitle = "muon windows opener popup parent input";
      const buttonLabel = "muon Windows Parent Input Counter";
      const popupUrl = `${MUON_APP_URL}#popup=connected`;
      const popupFeatures = "width=240,height=180";
      const running = await startWindowsNativeDialogMuon(["asset://main/**"]);
      let driver: CdpDriver | undefined = undefined;
      let popupDriver: CdpDriver | undefined = undefined;
      try {
        driver = await connectToMuonCdp({
          port: MUON_PORT,
          timeoutMs: cdpCommandTimeoutMs,
        });
        await driver.navigate(MUON_APP_URL, cdpCommandTimeoutMs);
        await driver.evaluate(`(() => {
          document.title = ${JSON.stringify(pageTitle)};
          document.body.innerHTML = "";
          const style = document.createElement("style");
          style.textContent =
            "html,body{height:100%;margin:0}button{height:100%;width:100%}";
          document.head.appendChild(style);
          const button = document.createElement("button");
          button.textContent = ${JSON.stringify(buttonLabel)};
          window.__muonCounter = 0;
          button.addEventListener("click", () => {
            window.__muonCounter += 1;
          });
          document.body.appendChild(button);
        })()`);
        const browserWindow = await waitForWindowsWindowByTitle(pageTitle);
        const previousTargetIds = await getCurrentTargetIds();
        const response = await driver.send<RuntimeEvaluateResponse>(
          "Runtime.evaluate",
          {
            expression: `(() => {
              const popup = window.open(
                ${JSON.stringify(popupUrl)},
                "muonWindowsOpenerPopup",
                ${JSON.stringify(popupFeatures)}
              );
              window.__muonWindowsPopup = popup;
              window.focus();
              return {
                returnedNull: popup === null,
                returnedHasOpener:
                  popup !== null && popup.opener !== null,
              };
            })()`,
            returnByValue: true,
            userGesture: true,
          },
        );
        if (response.exceptionDetails !== undefined) {
          throw new Error("Windows opener popup open script failed");
        }
        expect(response.result?.value).toEqual({
          returnedHasOpener: true,
          returnedNull: false,
        });
        const popupTarget = await waitForNewPageTarget(
          previousTargetIds,
          targetTimeoutMs,
          (target) => target.url === popupUrl,
        );
        popupDriver = await connectToMuonCdp({
          port: MUON_PORT,
          targetId: popupTarget.id,
          timeoutMs: cdpCommandTimeoutMs,
        });
        await expect(
          popupDriver.evaluate("window.opener !== null"),
        ).resolves.toBe(true);
        const popupBounds = await popupDriver.evaluate<WindowBounds>(`({
          x: window.screenX,
          y: window.screenY,
          width: window.outerWidth,
          height: window.outerHeight,
        })`);
        await expect(
          popupDriver.evaluate("document.location.href"),
        ).resolves.toBe(popupUrl);

        await expect(driver.evaluate("window.__muonCounter")).resolves.toBe(0);
        await driver.evaluate("window.muon.browser.focus()");
        await delay(200);
        await clickWindowsBrowserWindowOutsideBounds(
          browserWindow,
          popupBounds,
        );
        const deadline = Date.now() + cdpCommandTimeoutMs;
        let counter = 0;
        while (Date.now() < deadline) {
          counter = await driver.evaluate<number>("window.__muonCounter");
          if (counter === 1) {
            break;
          }
          await delay(100);
        }
        expect(counter).toBe(1);
      } finally {
        popupDriver?.close();
        await stopMuon(running, driver);
      }
    });

    it("detects a Windows native file dialog with agent-rover", async () => {
      const pageTitle = "muon windows dialog test";
      const title = "muon Windows File Dialog Test";
      const buttonLabel = "muon Windows Probe Select";
      const testDirectory = await mkdtemp(
        join(tmpdir(), "muon-windows-dialog-"),
      );
      const selectedPath = join(testDirectory, "selected.txt");
      await writeFile(selectedPath, "selected", "utf8");
      const running = await startWindowsNativeDialogMuon();
      let driver: CdpDriver | undefined = undefined;
      try {
        driver = await connectToMuonCdp({
          port: MUON_PORT,
          timeoutMs: cdpCommandTimeoutMs,
        });
        await driver.navigate(
          `data:text/html,<title>${pageTitle}</title>`,
          cdpCommandTimeoutMs,
        );
        await waitForMuonFsSelectFile(driver, cdpCommandTimeoutMs);
        await startNativeFileDialogProbe(driver, {
          buttonLabel,
          defaultPath: selectedPath,
          title,
        });

        const dialog = await waitForWindowsDialogByTitle(title);
        expect(dialog).toMatchObject({
          enabled: true,
          title,
          visible: true,
        });
        await expect(
          findWindowsDialogButtonByLabel(dialog, buttonLabel),
        ).resolves.toBeDefined();
        await selectWindowsDialogFile(dialog, selectedPath);

        await expect(readNativeFileDialogProbeResult(driver)).resolves.toEqual({
          status: "fulfilled",
          value: selectedPath,
        });
        await waitForWindowsDialogClosed(title);
      } finally {
        await stopMuon(running, driver);
        await rm(testDirectory, { recursive: true, force: true });
      }
    });

    it("uses the muon UI dialog provider for CEF file inputs on Windows", async () => {
      const pageTitle = "muon windows cef file input dialog";
      const dialogTitle = /^(Open|開く)$/;
      const running = await startWindowsNativeDialogMuon();
      let driver: CdpDriver | undefined = undefined;
      try {
        driver = await connectToMuonCdp({
          port: MUON_PORT,
          timeoutMs: cdpCommandTimeoutMs,
        });
        await driver.navigate(
          `data:text/html;charset=utf-8,${createFileInputPage(pageTitle)}`,
          cdpCommandTimeoutMs,
        );
        const browserWindow = await waitForWindowsWindowByTitle(pageTitle);
        await clickWindowsBrowserWindow(browserWindow, undefined);

        const dialog = await waitForWindowsDialogByTitleRegex(dialogTitle);
        await expect
          .poll(
            async () => (await waitForWindowsWindowByTitle(pageTitle)).enabled,
          )
          .toBe(false);
        try {
          await dialog.activate();
          await dialog.focus();
        } catch {
          // SetForegroundWindow can be denied; clicking the dialog is enough.
        }
        await clickWindowsWindowCenter(dialog);
        await requireWindowsRemoteContext().agent.keyboard.press("Escape");
        await requireWindowsRemoteContext().agent.waitForNoWindow(
          {
            titleRegex: dialogTitle,
            visible: true,
          },
          {
            intervalMs: 100,
            timeoutMs: cdpCommandTimeoutMs,
          },
        );

        await expect(
          driver.evaluate<{
            changed: boolean;
            fileCount: number;
          }>(`({
            changed: window.__muonFileInputChanged,
            fileCount: document.querySelector("#file").files.length,
          })`),
        ).resolves.toEqual({
          changed: false,
          fileCount: 0,
        });
        await expect
          .poll(
            async () => (await waitForWindowsWindowByTitle(pageTitle)).enabled,
          )
          .toBe(true);
      } finally {
        await stopMuon(running, driver);
      }
    });

    it("disables the opener browser view while a default modal Windows file dialog is open", async () => {
      const pageTitle = "muon windows modal dialog disables opener";
      const title = "muon Windows Modal Disable Probe";
      const buttonLabel = "muon Windows Modal Probe Select";
      const pageButtonLabel = "muon Windows Page Counter Button";
      const testDirectory = await mkdtemp(
        join(tmpdir(), "muon-windows-modal-dialog-"),
      );
      const selectedPath = join(testDirectory, "selected.txt");
      await writeFile(selectedPath, "selected", "utf8");
      const running = await startWindowsNativeDialogMuon();
      let driver: CdpDriver | undefined = undefined;
      try {
        driver = await connectToMuonCdp({
          port: MUON_PORT,
          timeoutMs: cdpCommandTimeoutMs,
        });
        const page = createCounterPage(pageTitle, pageButtonLabel);
        await driver.navigate(
          `data:text/html;charset=utf-8,${page}`,
          cdpCommandTimeoutMs,
        );
        const browserWindow = await waitForWindowsWindowByTitle(pageTitle);
        await waitForMuonFsSelectFile(driver, cdpCommandTimeoutMs);
        await startNativeFileDialogProbe(driver, {
          buttonLabel,
          defaultPath: selectedPath,
          title,
        });

        let dialog = await waitForWindowsDialogByTitle(title);
        dialog = await moveWindowsDialogAwayFromBrowserClickPoint(
          dialog,
          browserWindow.bounds,
        );
        await expect(
          findWindowsDialogButtonByLabel(dialog, buttonLabel),
        ).resolves.toBeDefined();
        await clickWindowsBrowserWindow(browserWindow, dialog);

        await selectWindowsDialogFile(dialog, selectedPath);
        await expect(readNativeFileDialogProbeResult(driver)).resolves.toEqual({
          status: "fulfilled",
          value: selectedPath,
        });
        await waitForWindowsDialogClosed(title);

        const countAfterClickWhileDialogOpen = await driver.evaluate<number>(
          "window.__muonCounter",
        );
        const enabledBrowserWindow = await waitForWindowsWindowEnabled(
          pageTitle,
          true,
        );
        await clickWindowsBrowserWindow(enabledBrowserWindow, undefined);
        const countAfterDialogClosed = await driver.evaluate<number>(
          "window.__muonCounter",
        );

        expect(countAfterClickWhileDialogOpen).toBe(0);
        expect(countAfterDialogClosed).toBe(1);
      } finally {
        await stopMuon(running, driver);
        await rm(testDirectory, { recursive: true, force: true });
      }
    });

    it("keeps the opener browser view enabled while a non-modal Windows file dialog is open", async () => {
      const pageTitle = "muon windows non-modal dialog keeps opener enabled";
      const title = "muon Windows Non Modal Disable Probe";
      const buttonLabel = "muon Windows Non Modal Probe Select";
      const pageButtonLabel = "muon Windows Non Modal Page Counter Button";
      const testDirectory = await mkdtemp(
        join(tmpdir(), "muon-windows-non-modal-dialog-"),
      );
      const selectedPath = join(testDirectory, "selected.txt");
      await writeFile(selectedPath, "selected", "utf8");
      const running = await startWindowsNativeDialogMuon();
      let driver: CdpDriver | undefined = undefined;
      try {
        driver = await connectToMuonCdp({
          port: MUON_PORT,
          timeoutMs: cdpCommandTimeoutMs,
        });
        const page = createCounterPage(pageTitle, pageButtonLabel);
        await driver.navigate(
          `data:text/html;charset=utf-8,${page}`,
          cdpCommandTimeoutMs,
        );
        const browserWindow = await waitForWindowsWindowByTitle(pageTitle);
        await waitForMuonFsSelectFile(driver, cdpCommandTimeoutMs);
        await startNativeFileDialogProbe(driver, {
          buttonLabel,
          defaultPath: selectedPath,
          modal: false,
          title,
        });

        let dialog = await waitForWindowsDialogByTitle(title);
        dialog = await moveWindowsDialogAwayFromBrowserClickPoint(
          dialog,
          browserWindow.bounds,
        );
        await expect(
          findWindowsDialogButtonByLabel(dialog, buttonLabel),
        ).resolves.toBeDefined();
        const enabledBrowserWindow = await waitForWindowsWindowEnabled(
          pageTitle,
          true,
        );
        await clickWindowsBrowserWindow(enabledBrowserWindow, dialog);

        await selectWindowsDialogFile(dialog, selectedPath);
        await expect(readNativeFileDialogProbeResult(driver)).resolves.toEqual({
          status: "fulfilled",
          value: selectedPath,
        });
        await waitForWindowsDialogClosed(title);

        const countAfterClickWhileDialogOpen = await driver.evaluate<number>(
          "window.__muonCounter",
        );
        expect(countAfterClickWhileDialogOpen).toBe(1);
      } finally {
        await stopMuon(running, driver);
        await rm(testDirectory, { recursive: true, force: true });
      }
    });

    it("closes a default modal Windows file dialog when the opener browser closes", async () => {
      const pageTitle = "muon windows modal dialog owner close";
      const title = "muon Windows Modal Owner Close Probe";
      const buttonLabel = "muon Windows Modal Owner Close Select";
      const testDirectory = await mkdtemp(
        join(tmpdir(), "muon-windows-modal-owner-close-"),
      );
      const selectedPath = join(testDirectory, "selected.txt");
      await writeFile(selectedPath, "selected", "utf8");
      const running = await startWindowsNativeDialogMuon();
      let driver: CdpDriver | undefined = undefined;
      try {
        driver = await connectToMuonCdp({
          port: MUON_PORT,
          timeoutMs: cdpCommandTimeoutMs,
        });
        await driver.navigate(
          `data:text/html,<title>${pageTitle}</title>`,
          cdpCommandTimeoutMs,
        );
        await waitForMuonFsSelectFile(driver, cdpCommandTimeoutMs);
        await startNativeFileDialogProbe(driver, {
          buttonLabel,
          defaultPath: selectedPath,
          title,
        });

        const browserWindow = await waitForWindowsWindowByTitle(pageTitle);
        await waitForWindowsDialogByTitle(title);
        await browserWindow.close();
        driver.close();
        driver = undefined;
        await waitForWindowsDialogClosed(title);
        await waitForProcessExit(running, processExitTimeoutMs);
      } finally {
        await stopMuon(running, driver);
        await rm(testDirectory, { recursive: true, force: true });
      }
    });

    it("keeps a non-modal Windows file dialog open when the opener browser closes", async () => {
      const pageTitle = "muon windows non-modal dialog owner close";
      const title = "muon Windows Non Modal Owner Close Probe";
      const buttonLabel = "muon Windows Non Modal Owner Close Select";
      const testDirectory = await mkdtemp(
        join(tmpdir(), "muon-windows-non-modal-owner-close-"),
      );
      const selectedPath = join(testDirectory, "selected.txt");
      await writeFile(selectedPath, "selected", "utf8");
      const running = await startWindowsNativeDialogMuon();
      let driver: CdpDriver | undefined = undefined;
      try {
        driver = await connectToMuonCdp({
          port: MUON_PORT,
          timeoutMs: cdpCommandTimeoutMs,
        });
        await driver.navigate(
          `data:text/html,<title>${pageTitle}</title>`,
          cdpCommandTimeoutMs,
        );
        await waitForMuonFsSelectFile(driver, cdpCommandTimeoutMs);
        await startNativeFileDialogProbe(driver, {
          buttonLabel,
          defaultPath: selectedPath,
          modal: false,
          title,
        });

        const browserWindow = await waitForWindowsWindowByTitle(pageTitle);
        const dialog = await waitForWindowsDialogByTitle(title);
        await expect(
          findWindowsDialogButtonByLabel(dialog, buttonLabel),
        ).resolves.toBeDefined();
        await browserWindow.close();
        driver.close();
        driver = undefined;
        await delay(500);
        await expect(waitForWindowsDialogByTitle(title)).resolves.toBeDefined();

        await selectWindowsDialogFile(dialog, selectedPath);
        await waitForProcessExit(running, processExitTimeoutMs);
      } finally {
        await stopMuon(running, driver);
        await rm(testDirectory, { recursive: true, force: true });
      }
    });
  } else if (!isLocalLinuxNativeDialogE2e) {
    it.skip("detects a GTK file dialog with Gestament", () => {
      expect(process.platform).not.toBe("linux");
    });
  }
});
