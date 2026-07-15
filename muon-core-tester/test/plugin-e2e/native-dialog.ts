// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import { expect, it } from "vitest";
import type { GtkApp, GtkWindowElement } from "gestament";
import type { AppWindow } from "agent-rover";

import {
  cdpCommandTimeoutMs,
  connectToMuonCdp,
  describeMuonPluginBridge,
  findGestamentNativeDialogButtonByLabel,
  findGestamentNativeWindow,
  getCurrentTargetIds,
  join,
  mkdtemp,
  MUON_APP_URL,
  MUON_PORT,
  processExitTimeoutMs,
  readNativeFileDialogProbeResult,
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
    <script>
      const input = document.querySelector("#file");
      window.__muonFileInputChanged = false;
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
  if (isLocalLinuxNativeDialogE2e) {
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

    it("uses the muon UI dialog provider for CEF file inputs", async () => {
      const pageTitle = "muon cef file input dialog";
      const dialogTitle = "Open";
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
      } finally {
        await stopGestamentMuon(running);
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

        await clickBrowserWindow(running.app, browserBounds, undefined);
        const countAfterDialogClosed = await running.driver.evaluate<number>(
          "window.__muonCounter",
        );

        expect(countWhileDialogOpen).toBe(0);
        expect(countAfterDialogClosed).toBe(1);
      } finally {
        await stopGestamentMuon(running);
        await rm(testDirectory, { recursive: true, force: true });
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
