// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import { PNG } from "pngjs";
import { expect, it } from "vitest";

import {
  cdpCommandTimeoutMs,
  execFileAsync,
  join,
  mkdir,
  mkdtemp,
  parseXpropWindowStateAtoms,
  parseXpropWindowTitle,
  processExitTimeoutMs,
  rm,
  shouldUseValgrind,
  startGestamentDebugMuon,
  stopGestamentMuon,
  tmpdir,
  wait,
  waitForDocumentTitle,
  waitForGestamentMuonExit,
  writeFile,
} from "./shared.js";
import type { CdpDriver, RunningGestamentMuon } from "./shared.js";

interface RgbaPixel {
  red: number;
  green: number;
  blue: number;
  alpha: number;
}

interface NativeWindowBounds {
  id: string;
  x: number;
  y: number;
  width: number;
  height: number;
}

interface RootCapture {
  png: PNG;
  x: number;
  y: number;
}

const testWindowTitle = "muon titlebar main";
const maximizedStates = [
  "_NET_WM_STATE_MAXIMIZED_HORZ",
  "_NET_WM_STATE_MAXIMIZED_VERT",
];
const titleBarHeight = 36;
const titleBarControlWidth = 46;
const titleBarControlsWidth = 138;
const configuredTitleBarBackgroundColor = "#123456";
const expectedTitleBarBackgroundColor: RgbaPixel = {
  red: 0x12,
  green: 0x34,
  blue: 0x56,
  alpha: 255,
};

const createTitleBarAssetRoot = async (directory: string): Promise<string> => {
  const assetRoot = join(directory, "titlebar-assets");
  const mainRoot = join(assetRoot, "main");
  await mkdir(mainRoot, { recursive: true });
  await writeFile(
    join(mainRoot, "index.html"),
    `<!doctype html><title>${testWindowTitle}</title><main>title bar test</main>`,
  );
  return assetRoot;
};

const createX11CommandEnvironment = async (
  running: RunningGestamentMuon,
): Promise<NodeJS.ProcessEnv> => ({
  ...process.env,
  ...(await running.app.environment()),
});

const readNativeClientWindowIds = async (
  env: NodeJS.ProcessEnv,
): Promise<string[]> => {
  const { stdout } = await execFileAsync(
    "xprop",
    ["-root", "_NET_CLIENT_LIST"],
    {
      env,
    },
  );
  return [...new Set(String(stdout).match(/0x[0-9a-f]+/gi) ?? [])];
};

const readNativeWindowTitle = async (
  id: string,
  env: NodeJS.ProcessEnv,
): Promise<string | undefined> => {
  const { stdout } = await execFileAsync(
    "xprop",
    ["-id", id, "_NET_WM_NAME", "WM_NAME"],
    { env },
  );
  return parseXpropWindowTitle(String(stdout));
};

const findNativeWindowIdByTitle = async (
  title: string,
  env: NodeJS.ProcessEnv,
): Promise<string | undefined> => {
  const ids = await readNativeClientWindowIds(env);
  for (const id of ids) {
    try {
      if ((await readNativeWindowTitle(id, env)) === title) {
        return id;
      }
    } catch {
      continue;
    }
  }
  return undefined;
};

const readNativeWindowStateAtomsById = async (
  id: string,
  env: NodeJS.ProcessEnv,
): Promise<string[]> => {
  const { stdout } = await execFileAsync(
    "xprop",
    ["-id", id, "_NET_WM_STATE"],
    {
      env,
    },
  );
  return parseXpropWindowStateAtoms(String(stdout));
};

const readNativeWindowStateAtomsByTitle = async (
  title: string,
  env: NodeJS.ProcessEnv,
): Promise<string[]> => {
  const id = await findNativeWindowIdByTitle(title, env);
  if (id === undefined) {
    return [];
  }
  try {
    return await readNativeWindowStateAtomsById(id, env);
  } catch {
    return [];
  }
};

const parseXwininfoValue = (output: string, label: string): number => {
  const match = new RegExp(`^\\s*${label}:\\s*(-?\\d+)`, "m").exec(output);
  if (match === null || match[1] === undefined) {
    throw new Error(`xwininfo output did not contain '${label}'`);
  }
  return Number.parseInt(match[1], 10);
};

const readNativeWindowBounds = async (
  id: string,
  env: NodeJS.ProcessEnv,
): Promise<NativeWindowBounds> => {
  const { stdout } = await execFileAsync("xwininfo", ["-id", id], { env });
  const output = String(stdout);
  return {
    id,
    x: parseXwininfoValue(output, "Absolute upper-left X"),
    y: parseXwininfoValue(output, "Absolute upper-left Y"),
    width: parseXwininfoValue(output, "Width"),
    height: parseXwininfoValue(output, "Height"),
  };
};

const waitForNativeWindowBoundsByTitle = async (
  title: string,
  timeoutMs: number,
  env: NodeJS.ProcessEnv,
): Promise<NativeWindowBounds> => {
  const deadline = Date.now() + timeoutMs;
  let lastIds: string[] = [];
  while (Date.now() < deadline) {
    lastIds = await readNativeClientWindowIds(env);
    const id = await findNativeWindowIdByTitle(title, env);
    if (id !== undefined) {
      return await readNativeWindowBounds(id, env);
    }
    await wait(100);
  }
  throw new Error(
    `Timed out waiting for native window '${title}'. Last X11 ids: ${JSON.stringify(lastIds)}`,
  );
};

const waitForNativeWindowStatesByTitle = async (
  title: string,
  expectedStates: string[],
  timeoutMs: number,
  env: NodeJS.ProcessEnv,
): Promise<void> => {
  const deadline = Date.now() + timeoutMs;
  let lastStates: string[] = [];
  while (Date.now() < deadline) {
    lastStates = await readNativeWindowStateAtomsByTitle(title, env);
    if (expectedStates.every((state) => lastStates.includes(state))) {
      return;
    }
    await wait(100);
  }
  throw new Error(
    `Timed out waiting for native window states ${JSON.stringify(
      expectedStates,
    )}. Last states: ${JSON.stringify(lastStates)}`,
  );
};

const waitForNativeWindowStatesAbsentByTitle = async (
  title: string,
  absentStates: string[],
  timeoutMs: number,
  env: NodeJS.ProcessEnv,
): Promise<void> => {
  const deadline = Date.now() + timeoutMs;
  let lastStates: string[] = [];
  while (Date.now() < deadline) {
    lastStates = await readNativeWindowStateAtomsByTitle(title, env);
    if (absentStates.every((state) => !lastStates.includes(state))) {
      return;
    }
    await wait(100);
  }
  throw new Error(
    `Timed out waiting for native window states ${JSON.stringify(
      absentStates,
    )} to disappear. Last states: ${JSON.stringify(lastStates)}`,
  );
};

const captureRoot = async (
  running: RunningGestamentMuon,
): Promise<RootCapture> => {
  const capture = await running.app.capture();
  return {
    png: PNG.sync.read(capture.image),
    x: capture.bounds.x,
    y: capture.bounds.y,
  };
};

const getPixel = (png: PNG, x: number, y: number): RgbaPixel => {
  const offset = (y * png.width + x) * 4;
  const red = png.data[offset];
  const green = png.data[offset + 1];
  const blue = png.data[offset + 2];
  const alpha = png.data[offset + 3];
  if (
    red === undefined ||
    green === undefined ||
    blue === undefined ||
    alpha === undefined
  ) {
    throw new Error("pixel coordinate was outside PNG data");
  }
  return { red, green, blue, alpha };
};

const getWindowPixel = (
  capture: RootCapture,
  bounds: NativeWindowBounds,
  x: number,
  y: number,
): RgbaPixel =>
  getPixel(capture.png, bounds.x + x - capture.x, bounds.y + y - capture.y);

const getLuminance = (pixel: RgbaPixel): number =>
  0.2126 * pixel.red + 0.7152 * pixel.green + 0.0722 * pixel.blue;

const expectPixelNear = (actual: RgbaPixel, expected: RgbaPixel): void => {
  expect(Math.abs(actual.red - expected.red)).toBeLessThanOrEqual(2);
  expect(Math.abs(actual.green - expected.green)).toBeLessThanOrEqual(2);
  expect(Math.abs(actual.blue - expected.blue)).toBeLessThanOrEqual(2);
  expect(actual.alpha).toBe(expected.alpha);
};

const countContrastingWindowPixels = (
  capture: RootCapture,
  bounds: NativeWindowBounds,
  left: number,
  top: number,
  width: number,
  height: number,
  reference: RgbaPixel,
): number => {
  let count = 0;
  const referenceLuminance = getLuminance(reference);
  for (let y = top; y < top + height; y += 1) {
    for (let x = left; x < left + width; x += 1) {
      const pixel = getWindowPixel(capture, bounds, x, y);
      if (Math.abs(getLuminance(pixel) - referenceLuminance) > 80) {
        count += 1;
      }
    }
  }
  return count;
};

const expectTitleBarChrome = async (
  running: RunningGestamentMuon,
  bounds: NativeWindowBounds,
): Promise<void> => {
  const capture = await captureRoot(running);
  expect(bounds.width).toBeGreaterThanOrEqual(1024);
  expect(bounds.height).toBeGreaterThanOrEqual(768 + titleBarHeight);

  const pixel = getWindowPixel(capture, bounds, 0, 0);
  expect(pixel.alpha).toBe(255);
  const titleBarLuminance = getLuminance(pixel);
  expect(titleBarLuminance < 120 || titleBarLuminance > 200).toBe(true);

  const controlCenters = [
    bounds.width - titleBarControlsWidth + titleBarControlWidth / 2,
    bounds.width - titleBarControlsWidth + titleBarControlWidth * 1.5,
    bounds.width - titleBarControlWidth / 2,
  ];
  for (const centerX of controlCenters) {
    const iconPixels = countContrastingWindowPixels(
      capture,
      bounds,
      Math.round(centerX - 8),
      Math.round(titleBarHeight / 2 - 8),
      16,
      16,
      pixel,
    );
    expect(iconPixels).toBeGreaterThan(0);
  }
};

const expectTitleBarBackgroundColor = async (
  running: RunningGestamentMuon,
  bounds: NativeWindowBounds,
): Promise<void> => {
  const capture = await captureRoot(running);
  expectPixelNear(
    getWindowPixel(capture, bounds, 0, 0),
    expectedTitleBarBackgroundColor,
  );
  expectPixelNear(
    getWindowPixel(capture, bounds, 0, titleBarHeight - 2),
    expectedTitleBarBackgroundColor,
  );
};

const clickTitleBarButton = async (
  running: RunningGestamentMuon,
  bounds: NativeWindowBounds,
  action: "maximize" | "close",
): Promise<void> => {
  const centerX =
    action === "maximize"
      ? bounds.x + bounds.width - titleBarControlWidth * 1.5
      : bounds.x + bounds.width - titleBarControlWidth / 2;
  await running.app.input.moveMouseTo(
    Math.round(centerX),
    Math.round(bounds.y + titleBarHeight / 2),
  );
  await running.app.input.setMouseButton("left", true);
  await running.app.input.setMouseButton("left", false);
  await wait(100);
};

const runTitleBarStep = async <T>(
  name: string,
  operation: () => Promise<T>,
): Promise<T> => {
  try {
    return await operation();
  } catch (error) {
    throw new Error(`${name}: ${String(error)}`);
  }
};

const withTitleBarMuon = async (
  run: (
    driver: CdpDriver,
    running: RunningGestamentMuon,
    env: NodeJS.ProcessEnv,
    bounds: NativeWindowBounds,
  ) => Promise<void>,
  browserBackgroundColor: string | undefined = undefined,
): Promise<void> => {
  const directory = await mkdtemp(join(tmpdir(), "muon-titlebar-"));
  const assetRoot = await createTitleBarAssetRoot(directory);
  const running = await startGestamentDebugMuon(
    null,
    assetRoot,
    browserBackgroundColor,
  );
  let caughtError: unknown = undefined;

  try {
    await waitForDocumentTitle(
      running.driver,
      testWindowTitle,
      cdpCommandTimeoutMs,
    );
    const env = await createX11CommandEnvironment(running);
    const bounds = await waitForNativeWindowBoundsByTitle(
      testWindowTitle,
      cdpCommandTimeoutMs,
      env,
    );
    await run(running.driver, running, env, bounds);
  } catch (error) {
    caughtError = error;
  }

  const output =
    caughtError === undefined
      ? undefined
      : await running.app.output().catch(() => undefined);
  try {
    await stopGestamentMuon(running);
  } catch (error) {
    caughtError = caughtError ?? error;
  }
  await rm(directory, { recursive: true, force: true });

  if (caughtError !== undefined) {
    throw new Error(`${String(caughtError)}\nMuon stderr:\n${output?.stderr}`);
  }
};

const titleBarIt =
  process.platform === "linux" && !shouldUseValgrind ? it : it.skip;

titleBarIt("renders and controls the Linux custom title bar", async () => {
  await withTitleBarMuon(async (driver, running, env, initialBounds) => {
    let bounds = initialBounds;
    await runTitleBarStep("verify app DOM is untouched", async () => {
      await expect(
        driver.evaluate('document.getElementById("muon-title-bar") === null'),
      ).resolves.toBe(true);
    });
    await runTitleBarStep(
      "verify title bar screenshot",
      async () => await expectTitleBarChrome(running, initialBounds),
    );

    await runTitleBarStep("click maximize", async () => {
      await clickTitleBarButton(running, bounds, "maximize");
      await waitForNativeWindowStatesByTitle(
        testWindowTitle,
        maximizedStates,
        cdpCommandTimeoutMs,
        env,
      );
      bounds = await waitForNativeWindowBoundsByTitle(
        testWindowTitle,
        cdpCommandTimeoutMs,
        env,
      );
    });
    await runTitleBarStep("click restore", async () => {
      await clickTitleBarButton(running, bounds, "maximize");
      await waitForNativeWindowStatesAbsentByTitle(
        testWindowTitle,
        maximizedStates,
        cdpCommandTimeoutMs,
        env,
      );
      bounds = await waitForNativeWindowBoundsByTitle(
        testWindowTitle,
        cdpCommandTimeoutMs,
        env,
      );
    });
    await runTitleBarStep("click close", async () => {
      await clickTitleBarButton(running, bounds, "close");
      await waitForGestamentMuonExit(running.app, processExitTimeoutMs);
    });
  });
});

titleBarIt(
  "uses browser.backgroundColor for the Linux custom title bar background",
  async () => {
    await withTitleBarMuon(async (_driver, running, _env, bounds) => {
      await runTitleBarStep(
        "verify configured title bar background",
        async () => await expectTitleBarBackgroundColor(running, bounds),
      );
    }, configuredTitleBarBackgroundColor);
  },
);
