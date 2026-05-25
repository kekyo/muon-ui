// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import { expect, it } from "vitest";
import type { GtkApp, GtkWindowElement } from "gestament";

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
  startGestamentDebugMuon,
  startNativeFileDialogProbe,
  stopGestamentMuon,
  targetTimeoutMs,
  tmpdir,
  wait,
  waitForGestamentMuonExit,
  waitForGestamentNativeWindowClosed,
  waitForMuonFsSelectFile,
  waitForNewPageTarget,
  writeFile,
  type CdpDriver,
  type RuntimeEvaluateResponse,
} from "./shared.js";

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
  await wait(100);
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
  expect(output.exitCode).toBe(0);
  expect(output.exitSignal).toBeNull();
};

const moveDialogAwayFromBrowserClickPoint = async (
  dialogWindow: GtkWindowElement,
  browserBounds: WindowBounds,
): Promise<void> => {
  await dialogWindow
    .moveTo(browserBounds.x + 160, browserBounds.y + 160)
    .catch(() => undefined);
  await wait(100);
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

describeMuonPluginBridge("muon plugin bridge - native dialogs", () => {
  if (process.platform === "linux") {
    it("keeps the opener browser view clickable while an opener popup is open", async () => {
      const pageTitle = "muon opener popup parent input";
      const buttonLabel = "Muon Parent Input Counter";
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
        await wait(200);
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
          await wait(100);
        }
        expect(counter).toBe(1);
      } finally {
        popupDriver?.close();
        await stopGestamentMuon(running);
      }
    });

    it("detects a GTK file dialog with Gestament", async () => {
      const pageTitle = "muon gestament dialog test";
      const title = "Muon Gestament File Dialog Test";
      const buttonLabel = "Muon Probe Select";
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

    it("uses the Muon UI dialog provider for CEF file inputs", async () => {
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
      const title = "Muon Modal Disable Probe";
      const buttonLabel = "Muon Modal Probe Select";
      const pageButtonLabel = "Muon Page Counter Button";
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
        await moveDialogAwayFromBrowserClickPoint(match.window, browserBounds);
        await clickBrowserWindow(running.app, browserBounds, match.window);
        const countWhileDialogOpen = await running.driver.evaluate<number>(
          "window.__muonCounter",
        );
        await match.window.activate().catch(() => undefined);

        const dialogButton = await findGestamentNativeDialogButtonByLabel(
          match.window,
          buttonLabel,
          cdpCommandTimeoutMs,
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
      const title = "Muon Non Modal Disable Probe";
      const buttonLabel = "Muon Non Modal Probe Select";
      const pageButtonLabel = "Muon Non Modal Page Counter Button";
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
        await moveDialogAwayFromBrowserClickPoint(match.window, browserBounds);
        await clickBrowserWindow(running.app, browserBounds, match.window);
        const countWhileDialogOpen = await running.driver.evaluate<number>(
          "window.__muonCounter",
        );
        await match.window.activate().catch(() => undefined);

        const dialogButton = await findGestamentNativeDialogButtonByLabel(
          match.window,
          buttonLabel,
          cdpCommandTimeoutMs,
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

        expect(countWhileDialogOpen).toBe(1);
      } finally {
        await stopGestamentMuon(running);
        await rm(testDirectory, { recursive: true, force: true });
      }
    });

    it("closes a default modal GTK file dialog when the opener browser closes", async () => {
      const pageTitle = "muon modal dialog owner close";
      const title = "Muon Modal Owner Close Probe";
      const buttonLabel = "Muon Modal Owner Close Select";
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
      const title = "Muon Non Modal Owner Close Probe";
      const buttonLabel = "Muon Non Modal Owner Close Select";
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

        const match = await findGestamentNativeWindow(
          running.app,
          title,
          cdpCommandTimeoutMs,
        );
        await closeOwnerBrowserWindow(running.driver);
        await wait(500);
        await expect(
          findGestamentNativeWindow(running.app, title, cdpCommandTimeoutMs),
        ).resolves.toBeDefined();

        const dialogButton = await findGestamentNativeDialogButtonByLabel(
          match.window,
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
  } else {
    it.skip("detects a GTK file dialog with Gestament", () => {
      expect(process.platform).not.toBe("linux");
    });
  }
});
