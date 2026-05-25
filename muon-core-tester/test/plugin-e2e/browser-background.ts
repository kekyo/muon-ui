// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import { PNG } from "pngjs";
import { expect, it } from "vitest";

import {
  MUON_APP_URL,
  MUON_PORT,
  TEST_BROWSER_PLUGIN_ALLOW_PATTERNS,
  TEST_NETWORK_ALLOW_PATTERNS,
  TEST_PLUGIN_ALLOW_PATTERNS,
  cdpCommandTimeoutMs,
  connectToMuonCdp,
  join,
  mkdir,
  mkdtemp,
  openPopupTarget,
  rm,
  shouldUseValgrind,
  startDebugMuon,
  stopMuon,
  tmpdir,
  waitForDocumentTitle,
  writeFile,
} from "./shared.js";
import type { CdpDriver, RunningMuon } from "./shared.js";

interface RgbPixel {
  red: number;
  green: number;
  blue: number;
}

const configuredBackgroundColor = "#123456";
const expectedBackgroundColor: RgbPixel = {
  red: 0x12,
  green: 0x34,
  blue: 0x56,
};

const createBackgroundAssetRoot = async (
  directory: string,
): Promise<string> => {
  const assetRoot = join(directory, "background-assets");
  const mainRoot = join(assetRoot, "main");
  await mkdir(mainRoot, { recursive: true });
  await writeFile(
    join(mainRoot, "index.html"),
    "<!doctype html><title>muon background main</title>",
  );
  await writeFile(
    join(mainRoot, "popup.html"),
    "<!doctype html><title>muon background popup</title>",
  );
  return assetRoot;
};

const startMuonWithBackgroundColor = async (
  assetRoot: string,
): Promise<RunningMuon> =>
  await startDebugMuon(
    [],
    TEST_NETWORK_ALLOW_PATTERNS,
    {},
    undefined,
    TEST_PLUGIN_ALLOW_PATTERNS,
    [],
    TEST_BROWSER_PLUGIN_ALLOW_PATTERNS,
    [],
    null,
    true,
    undefined,
    assetRoot,
    undefined,
    undefined,
    configuredBackgroundColor,
  );

const getCenterPixel = async (driver: CdpDriver): Promise<RgbPixel> => {
  const screenshot = await driver.screenshot();
  const png = PNG.sync.read(Buffer.from(screenshot));
  const x = Math.floor(png.width / 2);
  const y = Math.floor(png.height / 2);
  const offset = (y * png.width + x) * 4;
  const red = png.data[offset];
  const green = png.data[offset + 1];
  const blue = png.data[offset + 2];
  if (red === undefined || green === undefined || blue === undefined) {
    throw new Error("screenshot center pixel was outside PNG data");
  }
  return { red, green, blue };
};

const expectBackgroundScreenshot = async (driver: CdpDriver): Promise<void> => {
  const pixel = await getCenterPixel(driver);
  expect(Math.abs(pixel.red - expectedBackgroundColor.red)).toBeLessThanOrEqual(
    2,
  );
  expect(
    Math.abs(pixel.green - expectedBackgroundColor.green),
  ).toBeLessThanOrEqual(2);
  expect(
    Math.abs(pixel.blue - expectedBackgroundColor.blue),
  ).toBeLessThanOrEqual(2);
};

const withBackgroundColorMuon = async (
  run: (driver: CdpDriver, running: RunningMuon) => Promise<void>,
): Promise<void> => {
  const directory = await mkdtemp(join(tmpdir(), "muon-background-"));
  const assetRoot = await createBackgroundAssetRoot(directory);
  const running = await startMuonWithBackgroundColor(assetRoot);
  let driver: CdpDriver | undefined = undefined;
  let caughtError: unknown = undefined;

  try {
    driver = await connectToMuonCdp({
      port: MUON_PORT,
      timeoutMs: cdpCommandTimeoutMs,
    });
    await waitForDocumentTitle(
      driver,
      "muon background main",
      cdpCommandTimeoutMs,
    );
    await run(driver, running);
  } catch (error) {
    caughtError = error;
  }

  try {
    await stopMuon(running, driver);
  } catch (error) {
    caughtError = caughtError ?? error;
  }
  await rm(directory, { recursive: true, force: true });

  if (caughtError !== undefined) {
    throw new Error(`${String(caughtError)}\nMuon stderr:\n${running.stderr}`);
  }
};

const backgroundIt = shouldUseValgrind ? it.skip : it;

backgroundIt(
  "uses browser.backgroundColor for documents without a background",
  async () => {
    await withBackgroundColorMuon(async (driver) => {
      await expect(driver.evaluate("document.location.href")).resolves.toBe(
        MUON_APP_URL,
      );
      await expectBackgroundScreenshot(driver);
    });
  },
);

backgroundIt(
  "uses browser.backgroundColor for popup documents without a background",
  async () => {
    await withBackgroundColorMuon(async (driver) => {
      const popupTarget = await openPopupTarget(
        driver,
        "asset://main/popup.html",
      );
      let popupDriver: CdpDriver | undefined = undefined;
      try {
        popupDriver = await connectToMuonCdp({
          port: MUON_PORT,
          targetId: popupTarget.id,
          timeoutMs: cdpCommandTimeoutMs,
        });
        await waitForDocumentTitle(
          popupDriver,
          "muon background popup",
          cdpCommandTimeoutMs,
        );
        await expectBackgroundScreenshot(popupDriver);
      } finally {
        popupDriver?.close();
      }
    });
  },
);
