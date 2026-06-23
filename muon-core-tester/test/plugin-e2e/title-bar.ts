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
  sendNativeMouseWheel,
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
import type {
  BrowserInitialTitleBarVisibility,
  BrowserTitleBarType,
} from "./shared.js";

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

interface PageScrollPosition {
  x: number;
  y: number;
}

const testWindowTitle = "muon titlebar main";
const maximizedStates = [
  "_NET_WM_STATE_MAXIMIZED_HORZ",
  "_NET_WM_STATE_MAXIMIZED_VERT",
];
const titleBarHeight = 36;
const titleBarRoundControlSize = 20;
const titleBarRoundControlGap = 12;
const titleBarRoundControlRightInset = 8;
const titleBarControlsWidth =
  titleBarRoundControlSize * 3 +
  titleBarRoundControlGap * 2 +
  titleBarRoundControlRightInset;
const configuredTitleBarBackgroundColor = "#123456";
const appPageBackgroundColor = "#d7ebe5";
const initialTitleBarIconPath = "icons/title-bar-initial.png";
const updatedTitleBarIconPath = "icons/title-bar-updated.png";
const svgTitleBarIconPath = "icons/title-bar-vector.svg";
const expectedTitleBarBackgroundColor: RgbaPixel = {
  red: 0x12,
  green: 0x34,
  blue: 0x56,
  alpha: 255,
};
const expectedAppPageBackgroundColor: RgbaPixel = {
  red: 0xd7,
  green: 0xeb,
  blue: 0xe5,
  alpha: 255,
};
const expectedInitialTitleBarIconColor: RgbaPixel = {
  red: 0xed,
  green: 0x20,
  blue: 0x30,
  alpha: 255,
};
const expectedUpdatedTitleBarIconColor: RgbaPixel = {
  red: 0x20,
  green: 0xd0,
  blue: 0x60,
  alpha: 255,
};
const expectedSvgTitleBarIconColor: RgbaPixel = {
  red: 0x30,
  green: 0x70,
  blue: 0xe0,
  alpha: 255,
};

const createSolidPng = (color: RgbaPixel): Buffer => {
  const png = new PNG({ width: 16, height: 16 });
  for (let index = 0; index < png.data.length; index += 4) {
    png.data[index] = color.red;
    png.data[index + 1] = color.green;
    png.data[index + 2] = color.blue;
    png.data[index + 3] = color.alpha;
  }
  return PNG.sync.write(png);
};

const createSolidSvg = (color: RgbaPixel): string =>
  `<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16"><rect width="16" height="16" fill="rgb(${color.red},${color.green},${color.blue})"/></svg>`;

const createTitleBarPage = (
  title: string,
  faviconPath: string | undefined = undefined,
  pageDraggable: boolean = false,
): string =>
  `<!doctype html><title>${title}</title>${
    faviconPath === undefined
      ? ""
      : `<link rel="icon" type="image/svg+xml" href="${faviconPath}">`
  }<style>html,body{margin:0;min-width:100%;min-height:100%;background:${appPageBackgroundColor};}${
    pageDraggable
      ? "body{-webkit-app-region:drag;}.no-drag{position:fixed;left:24px;top:112px;min-width:120px;min-height:64px;-webkit-app-region:no-drag;}.drag-space{width:1800px;min-height:1600px;padding:220px 24px 24px;box-sizing:border-box;}.scroll-marker{margin-left:1400px;margin-top:1200px;}"
      : ""
  }</style><main class="drag-space">title bar test${
    pageDraggable ? '<div class="scroll-marker">scroll marker</div>' : ""
  }${
    pageDraggable
      ? '<button id="no-drag-button" class="no-drag">click <span id="no-drag-count">0</span></button><script>document.getElementById("no-drag-button").addEventListener("click",()=>{const count=document.getElementById("no-drag-count");count.textContent=String(Number(count.textContent)+1);});</script>'
      : ""
  }</main>`;

const createTitleBarAssetRoot = async (directory: string): Promise<string> => {
  const assetRoot = join(directory, "titlebar-assets");
  const mainRoot = join(assetRoot, "main");
  const iconsRoot = join(mainRoot, "icons");
  await mkdir(iconsRoot, { recursive: true });
  await writeFile(
    join(mainRoot, "index.html"),
    createTitleBarPage(testWindowTitle),
  );
  await writeFile(
    join(mainRoot, "favicon-svg.html"),
    createTitleBarPage(testWindowTitle, svgTitleBarIconPath),
  );
  await writeFile(
    join(mainRoot, "broken-favicon.html"),
    createTitleBarPage(testWindowTitle, "icons/missing.svg"),
  );
  await writeFile(
    join(mainRoot, "no-favicon.html"),
    createTitleBarPage(testWindowTitle),
  );
  await writeFile(
    join(mainRoot, "draggable.html"),
    createTitleBarPage(testWindowTitle, undefined, true),
  );
  await writeFile(
    join(assetRoot, "main", initialTitleBarIconPath),
    createSolidPng(expectedInitialTitleBarIconColor),
  );
  await writeFile(
    join(assetRoot, "main", updatedTitleBarIconPath),
    createSolidPng(expectedUpdatedTitleBarIconColor),
  );
  await writeFile(
    join(assetRoot, "main", svgTitleBarIconPath),
    createSolidSvg(expectedSvgTitleBarIconColor),
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

const readNativeMotifDecorationsByTitle = async (
  title: string,
  env: NodeJS.ProcessEnv,
): Promise<number | undefined> => {
  const id = await findNativeWindowIdByTitle(title, env);
  if (id === undefined) {
    return undefined;
  }
  const { stdout } = await execFileAsync(
    "xprop",
    ["-id", id, "_MOTIF_WM_HINTS"],
    {
      env,
    },
  );
  const output = String(stdout);
  if (output.includes("not found")) {
    return undefined;
  }
  const values = [...output.matchAll(/(?:0x[0-9a-f]+|-?\d+)/gi)].map(
    ([value]) =>
      Number.parseInt(value, value.toLowerCase().startsWith("0x") ? 16 : 10),
  );
  return values[2];
};

const waitForNativeMotifDecorationsByTitle = async (
  title: string,
  expectedDecorations: number,
  timeoutMs: number,
  env: NodeJS.ProcessEnv,
): Promise<void> => {
  const deadline = Date.now() + timeoutMs;
  let lastDecorations: number | undefined = undefined;
  while (Date.now() < deadline) {
    lastDecorations = await readNativeMotifDecorationsByTitle(title, env);
    if (lastDecorations === expectedDecorations) {
      return;
    }
    await wait(100);
  }
  throw new Error(
    `Timed out waiting for _MOTIF_WM_HINTS decorations ${expectedDecorations}. Last decorations: ${String(lastDecorations)}`,
  );
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

const isPixelNear = (actual: RgbaPixel, expected: RgbaPixel): boolean =>
  Math.abs(actual.red - expected.red) <= 2 &&
  Math.abs(actual.green - expected.green) <= 2 &&
  Math.abs(actual.blue - expected.blue) <= 2 &&
  actual.alpha === expected.alpha;

const waitForTitleBarBackgroundColor = async (
  running: RunningGestamentMuon,
  bounds: NativeWindowBounds,
): Promise<void> => {
  const deadline = Date.now() + cdpCommandTimeoutMs;
  let lastTopPixel: RgbaPixel | undefined = undefined;
  let lastBottomPixel: RgbaPixel | undefined = undefined;
  while (Date.now() < deadline) {
    const capture = await captureRoot(running);
    lastTopPixel = getWindowPixel(capture, bounds, 0, 0);
    lastBottomPixel = getWindowPixel(capture, bounds, 0, titleBarHeight - 2);
    if (
      isPixelNear(lastTopPixel, expectedTitleBarBackgroundColor) &&
      isPixelNear(lastBottomPixel, expectedTitleBarBackgroundColor)
    ) {
      return;
    }
    await wait(100);
  }
  throw new Error(
    `Timed out waiting for title bar background. Last pixels: ${JSON.stringify({
      top: lastTopPixel,
      bottom: lastBottomPixel,
    })}`,
  );
};

const waitForTitleBarHidden = async (
  running: RunningGestamentMuon,
  bounds: NativeWindowBounds,
): Promise<void> => {
  const deadline = Date.now() + cdpCommandTimeoutMs;
  let lastPixel: RgbaPixel | undefined = undefined;
  while (Date.now() < deadline) {
    const capture = await captureRoot(running);
    lastPixel = getWindowPixel(capture, bounds, 0, 0);
    if (isPixelNear(lastPixel, expectedAppPageBackgroundColor)) {
      return;
    }
    await wait(100);
  }
  throw new Error(
    `Timed out waiting for title bar to hide. Last pixel: ${JSON.stringify(lastPixel)}`,
  );
};

const waitForTitleBarIconColor = async (
  running: RunningGestamentMuon,
  bounds: NativeWindowBounds,
  expected: RgbaPixel,
  x: number = 18,
  y: number = 18,
): Promise<void> => {
  const deadline = Date.now() + cdpCommandTimeoutMs;
  let lastPixel: RgbaPixel | undefined = undefined;
  while (Date.now() < deadline) {
    const capture = await captureRoot(running);
    lastPixel = getWindowPixel(capture, bounds, x, y);
    if (isPixelNear(lastPixel, expected)) {
      return;
    }
    await wait(100);
  }
  throw new Error(
    `Timed out waiting for title bar icon color. Last pixel: ${JSON.stringify(lastPixel)}`,
  );
};

const waitForTitleBarIconContrast = async (
  running: RunningGestamentMuon,
  bounds: NativeWindowBounds,
  reference: RgbaPixel,
): Promise<void> => {
  const deadline = Date.now() + cdpCommandTimeoutMs;
  let lastIconPixels = 0;
  while (Date.now() < deadline) {
    const capture = await captureRoot(running);
    lastIconPixels = countContrastingWindowPixels(
      capture,
      bounds,
      10,
      10,
      16,
      16,
      reference,
    );
    if (lastIconPixels > 0) {
      return;
    }
    await wait(100);
  }
  throw new Error(
    `Timed out waiting for title bar icon contrast. Last icon pixel count: ${lastIconPixels}`,
  );
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
  expect(bounds.width).toBeGreaterThanOrEqual(1024);
  expect(bounds.height).toBeGreaterThanOrEqual(768 + titleBarHeight);

  const closeCenter =
    bounds.width -
    titleBarRoundControlRightInset -
    titleBarRoundControlSize / 2;
  const controlCenters = [
    bounds.width - titleBarControlsWidth + titleBarRoundControlSize / 2,
    bounds.width -
      titleBarControlsWidth +
      titleBarRoundControlSize * 1.5 +
      titleBarRoundControlGap,
    closeCenter,
  ];

  const deadline = Date.now() + cdpCommandTimeoutMs;
  let lastPixel: RgbaPixel | undefined = undefined;
  let lastIconPixelCounts: number[] = [];
  while (Date.now() < deadline) {
    const capture = await captureRoot(running);
    const pixel = getWindowPixel(capture, bounds, 0, 0);
    lastPixel = pixel;
    const titleBarLuminance = getLuminance(pixel);
    const titleBarLooksPainted =
      pixel.alpha === 255 &&
      (titleBarLuminance < 120 || titleBarLuminance > 200);
    lastIconPixelCounts = controlCenters.map((centerX) =>
      countContrastingWindowPixels(
        capture,
        bounds,
        Math.round(centerX - 8),
        Math.round(titleBarHeight / 2 - 8),
        16,
        16,
        pixel,
      ),
    );
    if (
      titleBarLooksPainted &&
      lastIconPixelCounts.every((iconPixels) => iconPixels > 0)
    ) {
      return;
    }
    await wait(100);
  }
  throw new Error(
    `Timed out waiting for title bar chrome. Last top-left pixel: ${JSON.stringify(
      lastPixel,
    )}, icon pixel counts: ${JSON.stringify(lastIconPixelCounts)}`,
  );
};

const clickTitleBarButton = async (
  running: RunningGestamentMuon,
  bounds: NativeWindowBounds,
  action: "maximize" | "close",
): Promise<void> => {
  const centerX =
    action === "maximize"
      ? bounds.x +
        bounds.width -
        titleBarControlsWidth +
        titleBarRoundControlSize * 1.5 +
        titleBarRoundControlGap
      : bounds.x +
        bounds.width -
        titleBarRoundControlRightInset -
        titleBarRoundControlSize / 2;
  await running.app.input.moveMouseTo(
    Math.round(centerX),
    Math.round(bounds.y + titleBarHeight / 2),
  );
  await running.app.input.setMouseButton("left", true);
  await running.app.input.setMouseButton("left", false);
  await wait(100);
};

const dragMouse = async (
  running: RunningGestamentMuon,
  startX: number,
  startY: number,
  endX: number,
  endY: number,
): Promise<void> => {
  await running.app.input.moveMouseTo(startX, startY);
  await running.app.input.setMouseButton("left", true);
  await wait(100);
  const stepCount = 10;
  for (let step = 1; step <= stepCount; step += 1) {
    await running.app.input.moveMouseTo(
      Math.round(startX + ((endX - startX) * step) / stepCount),
      Math.round(startY + ((endY - startY) * step) / stepCount),
    );
    await wait(50);
  }
  await running.app.input.setMouseButton("left", false);
  await wait(300);
};

const clickPageNoDragControl = async (
  driver: CdpDriver,
  running: RunningGestamentMuon,
  clickX: number,
  clickY: number,
): Promise<void> => {
  const deadline = Date.now() + cdpCommandTimeoutMs;
  while (Date.now() < deadline) {
    await running.app.input.moveMouseTo(clickX, clickY);
    await running.app.input.setMouseButton("left", true);
    await running.app.input.setMouseButton("left", false);
    const clickCount = Number(
      await driver.evaluate(
        'document.getElementById("no-drag-count").textContent',
      ),
    );
    if (clickCount >= 1) {
      return;
    }
    await wait(100);
  }
  throw new Error("Timed out waiting for page no-drag control click");
};

const readPageScrollPosition = async (
  driver: CdpDriver,
): Promise<PageScrollPosition> => ({
  x: Number(await driver.evaluate("window.scrollX")),
  y: Number(await driver.evaluate("window.scrollY")),
});

const waitForPageScrollChange = async (
  driver: CdpDriver,
  initial: PageScrollPosition,
  axis: keyof PageScrollPosition,
): Promise<PageScrollPosition> => {
  const deadline = Date.now() + cdpCommandTimeoutMs;
  let lastPosition = initial;
  while (Date.now() < deadline) {
    lastPosition = await readPageScrollPosition(driver);
    if (lastPosition[axis] > initial[axis]) {
      return lastPosition;
    }
    await wait(100);
  }
  throw new Error(
    `Timed out waiting for page ${axis} scroll. Initial: ${JSON.stringify(
      initial,
    )}, last: ${JSON.stringify(lastPosition)}`,
  );
};

const waitForNativeWindowMove = async (
  initialBounds: NativeWindowBounds,
  timeoutMs: number,
  env: NodeJS.ProcessEnv,
): Promise<NativeWindowBounds> => {
  const deadline = Date.now() + timeoutMs;
  let lastBounds = initialBounds;
  while (Date.now() < deadline) {
    lastBounds = await readNativeWindowBounds(initialBounds.id, env);
    if (lastBounds.x !== initialBounds.x || lastBounds.y !== initialBounds.y) {
      return lastBounds;
    }
    await wait(100);
  }
  throw new Error(
    `Timed out waiting for native window move. Initial: ${JSON.stringify(
      initialBounds,
    )}, last: ${JSON.stringify(lastBounds)}`,
  );
};

const expectNativeWindowStill = async (
  initialBounds: NativeWindowBounds,
  env: NodeJS.ProcessEnv,
): Promise<void> => {
  await wait(300);
  const currentBounds = await readNativeWindowBounds(initialBounds.id, env);
  expect(currentBounds.x).toBe(initialBounds.x);
  expect(currentBounds.y).toBe(initialBounds.y);
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
  browserInitialTitleBarVisibility:
    | BrowserInitialTitleBarVisibility
    | undefined = undefined,
  browserInitialTitleBarIcon: string | undefined = undefined,
  browserTitleBarType: BrowserTitleBarType | undefined = undefined,
): Promise<void> => {
  const directory = await mkdtemp(join(tmpdir(), "muon-titlebar-"));
  const assetRoot = await createTitleBarAssetRoot(directory);
  const running = await startGestamentDebugMuon(
    null,
    assetRoot,
    browserBackgroundColor,
    browserInitialTitleBarVisibility,
    browserInitialTitleBarIcon,
    browserTitleBarType,
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
  "moves the Linux custom window from page CSS draggable regions",
  async () => {
    await withTitleBarMuon(async (driver, running, env, initialBounds) => {
      let bounds = initialBounds;
      await runTitleBarStep("navigate to draggable page", async () => {
        await driver.evaluate('location.href = "asset://main/draggable.html"');
        await driver.evaluate(
          'new Promise((resolve) => { const check = () => document.getElementById("no-drag-button") === null ? setTimeout(check, 50) : resolve(true); check(); })',
        );
      });

      await runTitleBarStep("click page no-drag control", async () => {
        await clickPageNoDragControl(
          driver,
          running,
          initialBounds.x + 84,
          initialBounds.y + titleBarHeight + 130,
        );
        await expectNativeWindowStill(initialBounds, env);
      });

      await runTitleBarStep(
        "wheel page draggable region vertically",
        async () => {
          await driver.evaluate("window.scrollTo(0, 0)");
          const initialScroll = await readPageScrollPosition(driver);
          await sendNativeMouseWheel(
            testWindowTitle,
            bounds.x + 220,
            bounds.y + titleBarHeight + 260,
            "down",
          );
          await waitForPageScrollChange(driver, initialScroll, "y");
        },
      );

      await runTitleBarStep(
        "wheel page draggable region horizontally",
        async () => {
          await driver.evaluate("window.scrollTo(0, 0)");
          const initialScroll = await readPageScrollPosition(driver);
          await sendNativeMouseWheel(
            testWindowTitle,
            bounds.x + 220,
            bounds.y + titleBarHeight + 260,
            "right",
          );
          await waitForPageScrollChange(driver, initialScroll, "x");
        },
      );

      await runTitleBarStep(
        "drag title bar over page draggable regions",
        async () => {
          await dragMouse(
            running,
            bounds.x + 220,
            bounds.y + Math.round(titleBarHeight / 2),
            bounds.x + 320,
            bounds.y + Math.round(titleBarHeight / 2) + 80,
          );
          bounds = await waitForNativeWindowMove(
            bounds,
            cdpCommandTimeoutMs,
            env,
          );
        },
      );

      await runTitleBarStep("drag page background", async () => {
        await dragMouse(
          running,
          bounds.x + 220,
          bounds.y + titleBarHeight + 260,
          bounds.x + 320,
          bounds.y + titleBarHeight + 340,
        );
        await waitForNativeWindowMove(bounds, cdpCommandTimeoutMs, env);
      });
    });
  },
);

titleBarIt(
  "uses browser.backgroundColor for the Linux custom title bar background",
  async () => {
    await withTitleBarMuon(async (_driver, running, _env, bounds) => {
      await runTitleBarStep(
        "verify configured title bar background",
        async () => await waitForTitleBarBackgroundColor(running, bounds),
      );
    }, configuredTitleBarBackgroundColor);
  },
);

titleBarIt("uses the embedded default title bar icon", async () => {
  await withTitleBarMuon(async (_driver, running, _env, bounds) => {
    await runTitleBarStep(
      "verify configured title bar background",
      async () => await waitForTitleBarBackgroundColor(running, bounds),
    );
    await runTitleBarStep(
      "verify embedded default title bar icon",
      async () =>
        await waitForTitleBarIconContrast(
          running,
          bounds,
          expectedTitleBarBackgroundColor,
        ),
    );
  }, configuredTitleBarBackgroundColor);
});

titleBarIt(
  "controls the Linux custom title bar visibility through config and browser API",
  async () => {
    await withTitleBarMuon(
      async (driver, running, _env, bounds) => {
        await runTitleBarStep(
          "verify initial title bar is hidden",
          async () => await waitForTitleBarHidden(running, bounds),
        );
        await runTitleBarStep("show title bar through API", async () => {
          await expect(
            driver.evaluate("window.muon.browser.setTitleBarVisibility(true)"),
          ).resolves.toBeUndefined();
          await waitForTitleBarBackgroundColor(running, bounds);
        });
        await runTitleBarStep("hide title bar through API", async () => {
          await expect(
            driver.evaluate("window.muon.browser.setTitleBarVisibility(false)"),
          ).resolves.toBeUndefined();
          await waitForTitleBarHidden(running, bounds);
        });
      },
      configuredTitleBarBackgroundColor,
      false,
    );
  },
);

titleBarIt("shows and updates the Linux custom title bar icon", async () => {
  await withTitleBarMuon(
    async (driver, running, _env, bounds) => {
      await runTitleBarStep(
        "verify initial title bar icon",
        async () =>
          await waitForTitleBarIconColor(
            running,
            bounds,
            expectedInitialTitleBarIconColor,
          ),
      );
      await runTitleBarStep(
        "verify enlarged initial title bar icon",
        async () =>
          await waitForTitleBarIconColor(
            running,
            bounds,
            expectedInitialTitleBarIconColor,
            8,
            18,
          ),
      );
      await runTitleBarStep("update title bar icon through API", async () => {
        await expect(
          driver.evaluate(
            `window.muon.browser.setTitleBarIcon(${JSON.stringify(
              updatedTitleBarIconPath,
            )})`,
          ),
        ).resolves.toBeUndefined();
        await waitForTitleBarIconColor(
          running,
          bounds,
          expectedUpdatedTitleBarIconColor,
        );
      });
      await runTitleBarStep(
        "update title bar icon to SVG through API",
        async () => {
          await expect(
            driver.evaluate(
              `window.muon.browser.setTitleBarIcon(${JSON.stringify(
                svgTitleBarIconPath,
              )})`,
            ),
          ).resolves.toBeUndefined();
          await waitForTitleBarIconColor(
            running,
            bounds,
            expectedSvgTitleBarIconColor,
          );
        },
      );
      await runTitleBarStep("clear title bar icon through API", async () => {
        await expect(
          driver.evaluate("window.muon.browser.setTitleBarIcon(null)"),
        ).resolves.toBeUndefined();
        await waitForTitleBarIconColor(
          running,
          bounds,
          expectedTitleBarBackgroundColor,
        );
      });
    },
    configuredTitleBarBackgroundColor,
    undefined,
    initialTitleBarIconPath,
  );
});

titleBarIt(
  "updates the Linux custom title bar icon from favicon changes",
  async () => {
    await withTitleBarMuon(
      async (driver, running, _env, bounds) => {
        await runTitleBarStep(
          "verify initial title bar icon",
          async () =>
            await waitForTitleBarIconColor(
              running,
              bounds,
              expectedInitialTitleBarIconColor,
            ),
        );
        await runTitleBarStep("navigate to SVG favicon page", async () => {
          await driver.navigate("asset://main/favicon-svg.html");
          await waitForTitleBarIconColor(
            running,
            bounds,
            expectedSvgTitleBarIconColor,
          );
        });
        await runTitleBarStep(
          "fall back when favicon cannot be loaded",
          async () => {
            await driver.navigate("asset://main/broken-favicon.html");
            await waitForTitleBarIconColor(
              running,
              bounds,
              expectedInitialTitleBarIconColor,
            );
          },
        );
        await runTitleBarStep(
          "fall back when page has no favicon",
          async () => {
            await driver.navigate("asset://main/no-favicon.html");
            await waitForTitleBarIconColor(
              running,
              bounds,
              expectedInitialTitleBarIconColor,
            );
          },
        );
      },
      configuredTitleBarBackgroundColor,
      undefined,
      initialTitleBarIconPath,
    );
  },
);

titleBarIt(
  "controls the Linux native title bar decoration hint through browser API",
  async () => {
    await withTitleBarMuon(
      async (driver, _running, env) => {
        await runTitleBarStep("hide native title bar through API", async () => {
          await expect(
            driver.evaluate("window.muon.browser.setTitleBarVisibility(false)"),
          ).resolves.toBeUndefined();
          await waitForNativeMotifDecorationsByTitle(
            testWindowTitle,
            0,
            cdpCommandTimeoutMs,
            env,
          );
        });
        await runTitleBarStep("show native title bar through API", async () => {
          await expect(
            driver.evaluate("window.muon.browser.setTitleBarVisibility(true)"),
          ).resolves.toBeUndefined();
          await waitForNativeMotifDecorationsByTitle(
            testWindowTitle,
            1,
            cdpCommandTimeoutMs,
            env,
          );
        });
      },
      undefined,
      undefined,
      undefined,
      "native",
    );
  },
);

titleBarIt(
  "rejects non-PNG title bar icons for the Linux native title bar",
  async () => {
    await withTitleBarMuon(
      async (driver) => {
        await runTitleBarStep(
          "reject SVG title bar icon through API",
          async () => {
            await expect(
              driver.evaluate(
                `window.muon.browser.setTitleBarIcon(${JSON.stringify(
                  svgTitleBarIconPath,
                )}).then(() => "resolved", (error) => error instanceof Error ? error.message : String(error))`,
              ),
            ).resolves.toContain("PNG");
          },
        );
      },
      undefined,
      undefined,
      undefined,
      "native",
    );
  },
);
