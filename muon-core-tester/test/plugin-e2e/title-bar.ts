// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import type { ServerResponse } from "node:http";

import { PNG } from "pngjs";
import { expect, it } from "vitest";
import type { AppWindow, ScreenRect } from "agent-rover";

import {
  cdpCommandTimeoutMs,
  connectToMuonCdp,
  execFileAsync,
  isMuonTitleBarTarget,
  join,
  listCdpTargets,
  mkdir,
  mkdtemp,
  parseXpropWindowStateAtoms,
  parseXpropWindowTitle,
  processExitTimeoutMs,
  rm,
  sendNativeMouseWheel,
  shouldUseValgrind,
  startDebugMuon,
  startGestamentDebugMuon,
  startHttpServer,
  stopMuon,
  stopGestamentMuon,
  stopHttpServer,
  TEST_NETWORK_ALLOW_PATTERNS,
  tmpdir,
  delay,
  waitForDocumentTitle,
  waitForGestamentMuonExit,
  waitForProcessExit,
  writeFile,
} from "./shared.js";
import {
  isWindowsRemoteE2e,
  requireWindowsRemoteContext,
} from "./windows-context.js";
import type { CdpDriver, RunningGestamentMuon, RunningMuon } from "./shared.js";
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

interface WindowsWindowCapture {
  bounds: ScreenRect;
  png: PNG;
  visibleBounds: ScreenRect;
}

interface PageScrollPosition {
  x: number;
  y: number;
}

interface PageAppRegionHitElement {
  tagName: string;
  id: string;
  className: string;
  appRegion: string;
}

interface PageAppRegionHit {
  x: number;
  y: number;
  elements: PageAppRegionHitElement[];
  hasDrag: boolean;
  hasNoDrag: boolean;
}

interface FaviconResponseProbe {
  path: string;
  response: ServerResponse;
  status: "active" | "completed" | "canceled";
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
const windowsTitleBarControlWidth = 46;
const windowsTitleBarControlsWidth = 138;
const configuredTitleBarBackgroundColor = "#123456";
const appPageBackgroundColor = "#d7ebe5";
const initialTitleBarIconPath = "icons/title-bar-initial.png";
const updatedTitleBarIconPath = "icons/title-bar-updated.png";
const overLimitTitleBarIconPath = "icons/title-bar-over-limit.png";
const svgTitleBarIconPath = "icons/title-bar-vector.svg";
const overLimitImageLoadedTitle = "oversized ordinary image loaded";
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
const expectedOverLimitTitleBarIconColor: RgbaPixel = {
  red: 0xa0,
  green: 0x20,
  blue: 0xd0,
  alpha: 255,
};
const expectedSvgTitleBarIconColor: RgbaPixel = {
  red: 0x30,
  green: 0x70,
  blue: 0xe0,
  alpha: 255,
};
const expectedWindowsCloseHoverLightColor: RgbaPixel = {
  red: 0xc4,
  green: 0x2b,
  blue: 0x1c,
  alpha: 255,
};
const expectedWindowsCloseActiveLightColor: RgbaPixel = {
  red: 0xdd,
  green: 0x44,
  blue: 0x2e,
  alpha: 255,
};
const expectedWindowsCloseHoverDarkColor: RgbaPixel = {
  red: 0xdd,
  green: 0x44,
  blue: 0x2e,
  alpha: 255,
};
const expectedWindowsCloseActiveDarkColor: RgbaPixel = {
  red: 0xc4,
  green: 0x2b,
  blue: 0x1c,
  alpha: 255,
};
const faviconResponseLimitBytes = 1024 * 1024;

const createSolidPng = (
  color: RgbaPixel,
  width: number,
  height: number,
): Buffer => {
  const png = new PNG({ width, height });
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

const createSizedSvg = (color: RgbaPixel, byteLength: number): Buffer => {
  const svg = createSolidSvg(color);
  const svgByteLength = Buffer.byteLength(svg);
  if (svgByteLength > byteLength) {
    throw new Error("favicon response size is smaller than the SVG document");
  }
  return Buffer.from(`${svg}${" ".repeat(byteLength - svgByteLength)}`);
};

const trackFaviconResponse = (
  probes: FaviconResponseProbe[],
  path: string,
  response: ServerResponse,
): FaviconResponseProbe => {
  const probe: FaviconResponseProbe = {
    path,
    response,
    status: "active",
  };
  probes.push(probe);
  response.once("finish", () => {
    probe.status = "completed";
  });
  response.once("close", () => {
    if (probe.status === "active") {
      probe.status = "canceled";
    }
  });
  return probe;
};

const waitForFaviconResponseStatus = async (
  probes: FaviconResponseProbe[],
  path: string,
  status: FaviconResponseProbe["status"],
): Promise<void> => {
  const deadline = Date.now() + cdpCommandTimeoutMs;
  while (Date.now() < deadline) {
    if (
      probes.some((probe) => probe.path === path && probe.status === status)
    ) {
      return;
    }
    await delay(50);
  }
  throw new Error(
    `Timed out waiting for ${path} to become ${status}: ${JSON.stringify(
      probes.map((probe) => ({ path: probe.path, status: probe.status })),
    )}`,
  );
};

const setPageFavicon = async (
  driver: CdpDriver,
  faviconUrl: string,
): Promise<void> => {
  await driver.evaluate(`(() => {
    let link = document.getElementById("muon-favicon-response-probe");
    if (link === null) {
      link = document.createElement("link");
      link.id = "muon-favicon-response-probe";
      link.rel = "icon";
      link.type = "image/svg+xml";
      document.head.append(link);
    }
    link.href = ${JSON.stringify(faviconUrl)};
  })()`);
};

const createOverLimitFaviconPage = (): string =>
  `<!doctype html><title>${testWindowTitle}</title><link rel="icon" type="image/png" href="${overLimitTitleBarIconPath}"><img id="ordinary-over-limit-image" src="${overLimitTitleBarIconPath}"><script>const image=document.getElementById("ordinary-over-limit-image");const report=()=>{document.title=image.naturalWidth===257&&image.naturalHeight===257?${JSON.stringify(
    overLimitImageLoadedTitle,
  )}:"oversized ordinary image failed";};if(image.complete){report();}else{image.addEventListener("load",report,{once:true});image.addEventListener("error",()=>{document.title="oversized ordinary image failed";},{once:true});}</script>`;

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
      ? "body{-webkit-app-region:drag;}.no-drag{position:fixed;left:24px;top:112px;min-width:120px;min-height:64px;-webkit-app-region:no-drag;}.no-drag-wheel{position:absolute;left:24px;top:196px;width:160px;height:80px;-webkit-app-region:no-drag;background:rgba(255,255,255,0.01);overflow:hidden;}.drag-space{width:1800px;min-height:1600px;padding:300px 24px 24px;box-sizing:border-box;}.scroll-marker{margin-left:1400px;margin-top:1200px;}"
      : ""
  }</style><main class="drag-space">title bar test${
    pageDraggable ? '<div class="scroll-marker">scroll marker</div>' : ""
  }${
    pageDraggable
      ? '<button id="no-drag-button" class="no-drag">click <span id="no-drag-count">0</span></button><div id="no-drag-wheel" class="no-drag-wheel">wheel</div><script>document.getElementById("no-drag-button").addEventListener("click",()=>{const count=document.getElementById("no-drag-count");count.textContent=String(Number(count.textContent)+1);});</script>'
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
    join(mainRoot, "favicon-over-limit.html"),
    createOverLimitFaviconPage(),
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
    createSolidPng(expectedInitialTitleBarIconColor, 16, 16),
  );
  await writeFile(
    join(assetRoot, "main", updatedTitleBarIconPath),
    createSolidPng(expectedUpdatedTitleBarIconColor, 16, 16),
  );
  await writeFile(
    join(assetRoot, "main", overLimitTitleBarIconPath),
    createSolidPng(expectedOverLimitTitleBarIconColor, 257, 257),
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
    await delay(100);
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
    await delay(100);
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
    await delay(100);
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
    await delay(100);
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

const isPixelNear = (
  actual: RgbaPixel,
  expected: RgbaPixel,
  tolerance = 2,
): boolean =>
  Math.abs(actual.red - expected.red) <= tolerance &&
  Math.abs(actual.green - expected.green) <= tolerance &&
  Math.abs(actual.blue - expected.blue) <= tolerance &&
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
    await delay(100);
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
    await delay(100);
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
    await delay(100);
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
    await delay(100);
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
    await delay(100);
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
  await delay(100);
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
  await delay(100);
  const stepCount = 10;
  for (let step = 1; step <= stepCount; step += 1) {
    await running.app.input.moveMouseTo(
      Math.round(startX + ((endX - startX) * step) / stepCount),
      Math.round(startY + ((endY - startY) * step) / stepCount),
    );
    await delay(50);
  }
  await running.app.input.setMouseButton("left", false);
  await delay(300);
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
    await delay(50);
    await running.app.input.setMouseButton("left", true);
    await delay(50);
    await running.app.input.setMouseButton("left", false);
    const clickCount = Number(
      await driver.evaluate(
        'document.getElementById("no-drag-count").textContent',
      ),
    );
    if (clickCount >= 1) {
      return;
    }
    await delay(100);
  }
  throw new Error("Timed out waiting for page no-drag control click");
};

const readPageScrollPosition = async (
  driver: CdpDriver,
): Promise<PageScrollPosition> => ({
  x: Number(await driver.evaluate("window.scrollX")),
  y: Number(await driver.evaluate("window.scrollY")),
});

const readPageAppRegionHit = async (
  driver: CdpDriver,
  x: number,
  y: number,
): Promise<PageAppRegionHit> =>
  await driver.evaluate<PageAppRegionHit>(`(() => {
    const elements = [];
    for (let current = document.elementFromPoint(${x}, ${y}); current !== null; current = current.parentElement) {
      const appRegion = getComputedStyle(current).webkitAppRegion;
      elements.push({
        tagName: current.tagName,
        id: current.id,
        className: String(current.className ?? ""),
        appRegion,
      });
    }
    return {
      x: ${x},
      y: ${y},
      elements,
      hasDrag: elements.some((element) => element.appRegion === "drag"),
      hasNoDrag: elements.some((element) => element.appRegion === "no-drag"),
    };
  })()`);

const formatPageAppRegionHit = (hit: PageAppRegionHit): string =>
  hit.elements
    .map(
      (element) =>
        `${element.tagName}${element.id.length === 0 ? "" : `#${element.id}`}${
          element.className.length === 0 ? "" : `.${element.className}`
        }[${element.appRegion}]`,
    )
    .join(" > ");

const expectPageDragRegionPoint = async (
  driver: CdpDriver,
  x: number,
  y: number,
): Promise<void> => {
  const hit = await readPageAppRegionHit(driver, x, y);
  if (!hit.hasDrag || hit.hasNoDrag) {
    throw new Error(
      `Expected page point to be a drag region: ${formatPageAppRegionHit(hit)}`,
    );
  }
};

const expectPageNoDragRegionPoint = async (
  driver: CdpDriver,
  x: number,
  y: number,
): Promise<void> => {
  const hit = await readPageAppRegionHit(driver, x, y);
  if (!hit.hasNoDrag) {
    throw new Error(
      `Expected page point to be a no-drag region: ${formatPageAppRegionHit(hit)}`,
    );
  }
};

const sendNativeMouseWheelUntilPageScrollChange = async (
  driver: CdpDriver,
  windowTitle: string,
  rootX: number,
  rootY: number,
  direction: "up" | "down" | "left" | "right",
  axis: keyof PageScrollPosition,
): Promise<PageScrollPosition> => {
  const deadline = Date.now() + cdpCommandTimeoutMs;
  const initial = await readPageScrollPosition(driver);
  let lastPosition = initial;
  while (Date.now() < deadline) {
    await sendNativeMouseWheel(windowTitle, rootX, rootY, direction);
    await delay(100);
    lastPosition = await readPageScrollPosition(driver);
    if (lastPosition[axis] > initial[axis]) {
      return lastPosition;
    }
    await delay(100);
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
    await delay(100);
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
  await delay(300);
  const currentBounds = await readNativeWindowBounds(initialBounds.id, env);
  expect(currentBounds.x).toBe(initialBounds.x);
  expect(currentBounds.y).toBe(initialBounds.y);
};

const captureWindowsWindow = async (
  window: AppWindow,
): Promise<WindowsWindowCapture> => {
  const capture = await window.screenshot();
  return {
    bounds: capture.bounds,
    png: PNG.sync.read(capture.image),
    visibleBounds: capture.visibleBounds,
  };
};

const getWindowsWindowPixel = (
  capture: WindowsWindowCapture,
  x: number,
  y: number,
): RgbaPixel =>
  getPixel(
    capture.png,
    capture.bounds.x + x - capture.visibleBounds.x,
    capture.bounds.y + y - capture.visibleBounds.y,
  );

const waitForWindowsTitleBarWindow = async (): Promise<AppWindow> =>
  await requireWindowsRemoteContext().agent.waitForWindow(
    {
      title: testWindowTitle,
      visible: true,
    },
    {
      intervalMs: 100,
      message: `Timed out waiting for Windows native window '${testWindowTitle}'`,
      timeoutMs: cdpCommandTimeoutMs,
    },
  );

const waitForWindowsWindowMaximized = async (
  expectedMaximized: boolean,
): Promise<AppWindow> => {
  const deadline = Date.now() + cdpCommandTimeoutMs;
  let lastWindow: AppWindow | undefined = undefined;
  while (Date.now() < deadline) {
    lastWindow = await waitForWindowsTitleBarWindow();
    if (lastWindow.maximized === expectedMaximized) {
      return lastWindow;
    }
    await delay(100);
  }
  throw new Error(
    `Timed out waiting for Windows window maximized=${String(
      expectedMaximized,
    )}. Last window: ${JSON.stringify(lastWindow)}`,
  );
};

const waitForWindowsWindowMove = async (
  initialBounds: ScreenRect,
): Promise<AppWindow> => {
  const deadline = Date.now() + cdpCommandTimeoutMs;
  let lastWindow = await waitForWindowsTitleBarWindow();
  while (Date.now() < deadline) {
    lastWindow = await waitForWindowsTitleBarWindow();
    if (
      lastWindow.bounds.x !== initialBounds.x ||
      lastWindow.bounds.y !== initialBounds.y
    ) {
      return lastWindow;
    }
    await delay(100);
  }
  throw new Error(
    `Timed out waiting for Windows window move. Initial: ${JSON.stringify(
      initialBounds,
    )}, last: ${JSON.stringify(lastWindow.bounds)}`,
  );
};

const expectWindowsWindowStill = async (
  initialBounds: ScreenRect,
): Promise<void> => {
  await delay(300);
  const currentWindow = await waitForWindowsTitleBarWindow();
  expect(currentWindow.bounds.x).toBe(initialBounds.x);
  expect(currentWindow.bounds.y).toBe(initialBounds.y);
};

const activateWindowsTitleBarWindowIfAllowed = async (
  window: AppWindow,
): Promise<void> => {
  try {
    await window.activate();
  } catch {
    // SetForegroundWindow can be denied; the following mouse action is enough.
  }
};

const dragWindowsMouse = async (
  startX: number,
  startY: number,
  endX: number,
  endY: number,
): Promise<void> => {
  const context = requireWindowsRemoteContext();
  const start = { x: Math.round(startX), y: Math.round(startY) };
  const end = { x: Math.round(endX), y: Math.round(endY) };
  const window = await waitForWindowsTitleBarWindow();
  await activateWindowsTitleBarWindowIfAllowed(window);
  await context.agent.mouse.move(start);
  await delay(150);
  await context.agent.mouse.drag(start, end, { button: "left" });
  await delay(300);
};

const getWindowsTitleBarButtonPoint = (
  window: AppWindow,
  action: "minimize" | "maximize" | "close",
): { x: number; y: number } => {
  const controlIndex =
    action === "minimize" ? 0 : action === "maximize" ? 1 : 2;
  return {
    x:
      window.bounds.x +
      window.bounds.width -
      windowsTitleBarControlsWidth +
      windowsTitleBarControlWidth * (controlIndex + 0.5),
    y: window.bounds.y + titleBarHeight / 2,
  };
};

const clickWindowsTitleBarButton = async (
  action: "minimize" | "maximize" | "close",
): Promise<void> => {
  const context = requireWindowsRemoteContext();
  const window = await waitForWindowsTitleBarWindow();
  await activateWindowsTitleBarWindowIfAllowed(window);
  await context.agent.mouse.click(
    getWindowsTitleBarButtonPoint(window, action),
    {
      button: "left",
    },
  );
  await delay(150);
};

const doubleClickWindowsTitleBar = async (): Promise<void> => {
  const context = requireWindowsRemoteContext();
  const window = await waitForWindowsTitleBarWindow();
  const point = {
    x: window.bounds.x + Math.min(220, Math.floor(window.bounds.width / 2)),
    y: window.bounds.y + Math.round(titleBarHeight / 2),
  };
  await activateWindowsTitleBarWindowIfAllowed(window);
  await context.agent.mouse.click(point, { button: "left" });
  await delay(80);
  await context.agent.mouse.click(point, { button: "left" });
  await delay(150);
};

const connectToWindowsTitleBarCdp = async (): Promise<CdpDriver> => {
  const deadline = Date.now() + cdpCommandTimeoutMs;
  let lastTargets = "";
  let lastError: unknown = undefined;
  while (Date.now() < deadline) {
    try {
      const targets = await listCdpTargets({
        timeoutMs: Math.max(1, deadline - Date.now()),
      });
      lastTargets = targets
        .map((target) => `${target.id}:${target.type}:${target.title}`)
        .join(", ");
      const target = targets.find(
        (candidate) =>
          candidate.type === "page" &&
          candidate.webSocketDebuggerUrl !== undefined &&
          isMuonTitleBarTarget(candidate),
      );
      if (target !== undefined) {
        return await connectToMuonCdp({
          targetId: target.id,
          timeoutMs: Math.max(1, deadline - Date.now()),
        });
      }
    } catch (error) {
      lastError = error;
    }
    await delay(100);
  }
  throw new Error(
    `Timed out connecting to Windows title bar CDP target. Last targets: ${lastTargets}. Last error: ${String(lastError)}`,
  );
};

const readWindowsTitleBarCloseButtonColor = async (
  driver: CdpDriver,
  colorScheme: "light" | "dark",
  state: "hover" | "pressed",
): Promise<RgbaPixel> => {
  await driver.send("Emulation.setEmulatedMedia", {
    features: [{ name: "prefers-color-scheme", value: colorScheme }],
  });
  return await driver.evaluate<RgbaPixel>(`(() => {
    const api = globalThis.__muonTitleBar;
    if (typeof api?.setNativeHover !== "function") {
      throw new Error("missing native hover bridge");
    }
    if (typeof api?.setNativePressed !== "function") {
      throw new Error("missing native pressed bridge");
    }
    const close = document.getElementById("muon-close");
    if (!(close instanceof HTMLElement)) {
      throw new Error("missing close button");
    }
    api.setNativeHover(null);
    api.setNativePressed(null);
    api.setNativeHover("close");
    if (${JSON.stringify(state)} === "pressed") {
      api.setNativePressed("close");
    }
    const value = getComputedStyle(close).backgroundColor;
    api.setNativePressed(null);
    api.setNativeHover(null);
    const match = /^rgba?\\(\\s*(\\d+)\\s*,\\s*(\\d+)\\s*,\\s*(\\d+)(?:\\s*,\\s*(\\d*(?:\\.\\d+)?|1|0))?\\s*\\)$/u.exec(value);
    if (match === null || match[1] === undefined || match[2] === undefined || match[3] === undefined) {
      throw new Error("unsupported background color: " + value);
    }
    return {
      red: Number(match[1]),
      green: Number(match[2]),
      blue: Number(match[3]),
      alpha: match[4] === undefined ? 255 : Math.round(Number(match[4]) * 255),
    };
  })()`);
};

const expectWindowsTitleBarCloseButtonColor = async (
  driver: CdpDriver,
  colorScheme: "light" | "dark",
  state: "hover" | "pressed",
  expected: RgbaPixel,
): Promise<void> => {
  const actual = await readWindowsTitleBarCloseButtonColor(
    driver,
    colorScheme,
    state,
  );
  if (!isPixelNear(actual, expected, 0)) {
    throw new Error(
      `Expected Windows close button ${colorScheme} ${state} color ${JSON.stringify(
        expected,
      )}, got ${JSON.stringify(actual)}`,
    );
  }
};

const waitForWindowsCloseButtonHover = async (): Promise<void> => {
  const context = requireWindowsRemoteContext();
  const deadline = Date.now() + cdpCommandTimeoutMs;
  let lastPixel: RgbaPixel | undefined = undefined;
  while (Date.now() < deadline) {
    const window = await waitForWindowsTitleBarWindow();
    await activateWindowsTitleBarWindowIfAllowed(window);
    const hoverPoint = getWindowsTitleBarButtonPoint(window, "close");
    await context.agent.mouse.move({
      x: Math.round(hoverPoint.x),
      y: Math.round(hoverPoint.y),
    });
    await delay(100);
    const capture = await captureWindowsWindow(window);
    lastPixel = getWindowsWindowPixel(
      capture,
      window.bounds.width - 8,
      Math.round(titleBarHeight / 2),
    );
    if (isPixelNear(lastPixel, expectedWindowsCloseHoverLightColor, 12)) {
      return;
    }
    await delay(100);
  }
  throw new Error(
    `Timed out waiting for Windows close button hover. Last pixel: ${JSON.stringify(lastPixel)}`,
  );
};

const waitForWindowsWindowTopResize = async (
  initialBounds: ScreenRect,
): Promise<AppWindow> => {
  const deadline = Date.now() + cdpCommandTimeoutMs;
  let lastWindow = await waitForWindowsTitleBarWindow();
  while (Date.now() < deadline) {
    lastWindow = await waitForWindowsTitleBarWindow();
    const initialBottom = initialBounds.y + initialBounds.height;
    const lastBottom = lastWindow.bounds.y + lastWindow.bounds.height;
    if (
      lastWindow.bounds.height < initialBounds.height - 20 &&
      Math.abs(lastBottom - initialBottom) <= 16
    ) {
      return lastWindow;
    }
    await delay(100);
  }
  throw new Error(
    `Timed out waiting for Windows top-edge resize. Initial: ${JSON.stringify(
      initialBounds,
    )}, last: ${JSON.stringify(lastWindow.bounds)}`,
  );
};

const clickWindowsPagePoint = async (
  driver: CdpDriver,
  clickX: number,
  clickY: number,
): Promise<void> => {
  const deadline = Date.now() + cdpCommandTimeoutMs;
  while (Date.now() < deadline) {
    await requireWindowsRemoteContext().agent.mouse.click(
      { x: clickX, y: clickY },
      { button: "left" },
    );
    const clickCount = Number(
      await driver.evaluate(
        'document.getElementById("no-drag-count").textContent',
      ),
    );
    if (clickCount >= 1) {
      return;
    }
    await delay(100);
  }
  throw new Error("Timed out waiting for Windows page no-drag control click");
};

const waitForWindowsTitleBarBackgroundColor = async (
  window: AppWindow,
): Promise<void> => {
  const deadline = Date.now() + cdpCommandTimeoutMs;
  let lastTopPixel: RgbaPixel | undefined = undefined;
  let lastBottomPixel: RgbaPixel | undefined = undefined;
  while (Date.now() < deadline) {
    const capture = await captureWindowsWindow(window);
    lastTopPixel = getWindowsWindowPixel(capture, 8, 8);
    lastBottomPixel = getWindowsWindowPixel(capture, 8, titleBarHeight - 4);
    if (
      isPixelNear(lastTopPixel, expectedTitleBarBackgroundColor, 6) &&
      isPixelNear(lastBottomPixel, expectedTitleBarBackgroundColor, 6)
    ) {
      return;
    }
    await delay(100);
  }
  throw new Error(
    `Timed out waiting for Windows title bar background. Last pixels: ${JSON.stringify(
      {
        top: lastTopPixel,
        bottom: lastBottomPixel,
      },
    )}`,
  );
};

const waitForWindowsTitleBarHidden = async (
  window: AppWindow,
): Promise<void> => {
  const deadline = Date.now() + cdpCommandTimeoutMs;
  let lastPixel: RgbaPixel | undefined = undefined;
  while (Date.now() < deadline) {
    const capture = await captureWindowsWindow(window);
    lastPixel = getWindowsWindowPixel(capture, 8, 8);
    if (isPixelNear(lastPixel, expectedAppPageBackgroundColor, 6)) {
      return;
    }
    await delay(100);
  }
  throw new Error(
    `Timed out waiting for Windows title bar to hide. Last pixel: ${JSON.stringify(lastPixel)}`,
  );
};

const waitForWindowsTitleBarIconColor = async (
  window: AppWindow,
  expected: RgbaPixel,
  x: number = 18,
  y: number = 18,
): Promise<void> => {
  const deadline = Date.now() + cdpCommandTimeoutMs;
  let lastPixel: RgbaPixel | undefined = undefined;
  while (Date.now() < deadline) {
    const capture = await captureWindowsWindow(window);
    lastPixel = getWindowsWindowPixel(capture, x, y);
    if (isPixelNear(lastPixel, expected, 8)) {
      return;
    }
    await delay(100);
  }
  throw new Error(
    `Timed out waiting for Windows title bar icon color. Last pixel: ${JSON.stringify(lastPixel)}`,
  );
};

const waitForWindowsTitleBarIconContrast = async (
  window: AppWindow,
  reference: RgbaPixel,
): Promise<void> => {
  const deadline = Date.now() + cdpCommandTimeoutMs;
  let lastIconPixels = 0;
  while (Date.now() < deadline) {
    const capture = await captureWindowsWindow(window);
    lastIconPixels = countContrastingWindowPixels(
      {
        png: capture.png,
        x: capture.visibleBounds.x,
        y: capture.visibleBounds.y,
      },
      {
        id: window.id,
        x: capture.bounds.x,
        y: capture.bounds.y,
        width: capture.bounds.width,
        height: capture.bounds.height,
      },
      10,
      10,
      22,
      22,
      reference,
    );
    if (lastIconPixels > 0) {
      return;
    }
    await delay(100);
  }
  throw new Error(
    `Timed out waiting for Windows title bar icon contrast. Last icon pixel count: ${lastIconPixels}`,
  );
};

const expectWindowsTitleBarChrome = async (
  window: AppWindow,
): Promise<void> => {
  expect(window.bounds.width).toBeGreaterThanOrEqual(1024);
  expect(window.bounds.height).toBeGreaterThanOrEqual(768);
  const controlCenters = [
    window.bounds.width -
      windowsTitleBarControlsWidth +
      windowsTitleBarControlWidth / 2,
    window.bounds.width -
      windowsTitleBarControlsWidth +
      windowsTitleBarControlWidth * 1.5,
    window.bounds.width - windowsTitleBarControlWidth / 2,
  ];
  const deadline = Date.now() + cdpCommandTimeoutMs;
  let lastPixel: RgbaPixel | undefined = undefined;
  let lastIconPixelCounts: number[] = [];
  while (Date.now() < deadline) {
    const capture = await captureWindowsWindow(window);
    const pixel = getWindowsWindowPixel(capture, 8, 8);
    lastPixel = pixel;
    const titleBarLuminance = getLuminance(pixel);
    const titleBarLooksPainted =
      pixel.alpha === 255 &&
      (titleBarLuminance < 120 || titleBarLuminance > 200);
    lastIconPixelCounts = controlCenters.map((centerX) =>
      countContrastingWindowPixels(
        {
          png: capture.png,
          x: capture.visibleBounds.x,
          y: capture.visibleBounds.y,
        },
        {
          id: window.id,
          x: capture.bounds.x,
          y: capture.bounds.y,
          width: capture.bounds.width,
          height: capture.bounds.height,
        },
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
    await delay(100);
  }
  throw new Error(
    `Timed out waiting for Windows title bar chrome. Last top pixel: ${JSON.stringify(
      lastPixel,
    )}, icon pixel counts: ${JSON.stringify(lastIconPixelCounts)}`,
  );
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

const isCdpWebSocketFailure = (error: unknown): boolean =>
  String(error).includes("CDP WebSocket failed");

const closeWindowsBrowserAndWait = async (
  driver: CdpDriver,
  running: RunningMuon,
): Promise<void> => {
  let closeError: unknown = undefined;
  try {
    await driver.evaluate("window.muon.browser.close()");
  } catch (error) {
    closeError = error;
  }
  await waitForProcessExit(running, processExitTimeoutMs);
  if (closeError !== undefined && !isCdpWebSocketFailure(closeError)) {
    throw closeError;
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
  networkAllowPatterns: string[] = TEST_NETWORK_ALLOW_PATTERNS,
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
    networkAllowPatterns,
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

const withWindowsTitleBarMuon = async (
  run: (
    driver: CdpDriver,
    running: RunningMuon,
    window: AppWindow,
  ) => Promise<void>,
  browserBackgroundColor: string | undefined = undefined,
  browserInitialTitleBarVisibility:
    | BrowserInitialTitleBarVisibility
    | undefined = undefined,
  browserInitialTitleBarIcon: string | undefined = undefined,
  browserTitleBarType: BrowserTitleBarType | undefined = undefined,
): Promise<void> => {
  const directory = await mkdtemp(join(tmpdir(), "muon-windows-titlebar-"));
  const assetRoot = await createTitleBarAssetRoot(directory);
  let running: RunningMuon | undefined = undefined;
  let driver: CdpDriver | undefined = undefined;
  let caughtError: unknown = undefined;

  try {
    running = await startDebugMuon(
      [],
      undefined,
      {},
      undefined,
      undefined,
      undefined,
      undefined,
      [],
      null,
      true,
      undefined,
      assetRoot,
      undefined,
      undefined,
      browserBackgroundColor,
      browserInitialTitleBarVisibility,
      browserInitialTitleBarIcon,
      browserTitleBarType,
    );
    driver = await connectToMuonCdp({
      timeoutMs: cdpCommandTimeoutMs,
    });
    await waitForDocumentTitle(driver, testWindowTitle, cdpCommandTimeoutMs);
    const window = await (
      await waitForWindowsTitleBarWindow()
    ).waitForStableBounds({
      intervalMs: 100,
      stableIterations: 2,
      timeoutMs: cdpCommandTimeoutMs,
    });
    await run(driver, running, window);
  } catch (error) {
    caughtError = error;
  }

  if (running !== undefined) {
    try {
      await stopMuon(running, driver);
    } catch (error) {
      caughtError = caughtError ?? error;
    }
  }
  await rm(directory, { recursive: true, force: true });

  if (caughtError !== undefined) {
    throw new Error(
      `${String(caughtError)}\nMuon stderr:\n${running?.stderr ?? ""}`,
    );
  }
};

const isLocalLinuxTitleBarE2e =
  process.platform === "linux" && !isWindowsRemoteE2e();
const titleBarIt = isLocalLinuxTitleBarE2e && !shouldUseValgrind ? it : it.skip;
const windowsTitleBarIt = isWindowsRemoteE2e() ? it : it.skip;

windowsTitleBarIt(
  "renders and controls the Windows custom title bar",
  async () => {
    await withWindowsTitleBarMuon(async (driver, running, window) => {
      await runTitleBarStep("verify app DOM is untouched", async () => {
        await expect(
          driver.evaluate('document.getElementById("muon-title-bar") === null'),
        ).resolves.toBe(true);
      });
      await runTitleBarStep(
        "verify title bar screenshot",
        async () => await expectWindowsTitleBarChrome(window),
      );

      await runTitleBarStep("maximize through browser API", async () => {
        await expect(
          driver.evaluate("window.muon.browser.maximize()"),
        ).resolves.toBeUndefined();
        await waitForWindowsWindowMaximized(true);
      });
      await runTitleBarStep("restore through browser API", async () => {
        await expect(
          driver.evaluate("window.muon.browser.restore()"),
        ).resolves.toBeUndefined();
        await waitForWindowsWindowMaximized(false);
      });
      await runTitleBarStep("close through browser API", async () => {
        await closeWindowsBrowserAndWait(driver, running);
      });
    });
  },
);

windowsTitleBarIt(
  "uses themed Windows custom title bar close button colors",
  async () => {
    await withWindowsTitleBarMuon(async () => {
      const titleBarDriver = await connectToWindowsTitleBarCdp();
      try {
        await runTitleBarStep(
          "verify light close hover color",
          async () =>
            await expectWindowsTitleBarCloseButtonColor(
              titleBarDriver,
              "light",
              "hover",
              expectedWindowsCloseHoverLightColor,
            ),
        );
        await runTitleBarStep(
          "verify light close pressed color",
          async () =>
            await expectWindowsTitleBarCloseButtonColor(
              titleBarDriver,
              "light",
              "pressed",
              expectedWindowsCloseActiveLightColor,
            ),
        );
        await runTitleBarStep(
          "verify dark close hover color",
          async () =>
            await expectWindowsTitleBarCloseButtonColor(
              titleBarDriver,
              "dark",
              "hover",
              expectedWindowsCloseHoverDarkColor,
            ),
        );
        await runTitleBarStep(
          "verify dark close pressed color",
          async () =>
            await expectWindowsTitleBarCloseButtonColor(
              titleBarDriver,
              "dark",
              "pressed",
              expectedWindowsCloseActiveDarkColor,
            ),
        );
      } finally {
        titleBarDriver.close();
      }
    });
  },
);

windowsTitleBarIt(
  "keeps Windows custom title bar mouse controls responsive across maximize cycles",
  async () => {
    await withWindowsTitleBarMuon(async () => {
      for (let iteration = 0; iteration < 4; iteration += 1) {
        await runTitleBarStep("maximize from title bar button", async () => {
          await clickWindowsTitleBarButton("maximize");
          await waitForWindowsWindowMaximized(true);
        });
        await runTitleBarStep("restore from title bar button", async () => {
          await clickWindowsTitleBarButton("maximize");
          await waitForWindowsWindowMaximized(false);
        });
      }

      await runTitleBarStep(
        "maximize from title bar double click",
        async () => {
          await doubleClickWindowsTitleBar();
          await waitForWindowsWindowMaximized(true);
        },
      );
      await runTitleBarStep("restore from title bar double click", async () => {
        await doubleClickWindowsTitleBar();
        await waitForWindowsWindowMaximized(false);
      });
      await runTitleBarStep(
        "verify close button hover remains responsive",
        async () => await waitForWindowsCloseButtonHover(),
      );
    }, configuredTitleBarBackgroundColor);
  },
);

windowsTitleBarIt(
  "resizes the Windows custom window from the title bar top edge",
  async () => {
    await withWindowsTitleBarMuon(async () => {
      let window = await waitForWindowsTitleBarWindow();
      window = await window.setBounds({
        x: 160,
        y: 160,
        width: 900,
        height: 640,
      });
      window = await window.waitForStableBounds({
        intervalMs: 100,
        stableIterations: 2,
        timeoutMs: cdpCommandTimeoutMs,
      });
      const initialBounds = window.bounds;
      const startX = initialBounds.x + Math.round(initialBounds.width / 2);
      const startY = initialBounds.y + 1;
      await dragWindowsMouse(startX, startY, startX, startY + 80);
      await waitForWindowsWindowTopResize(initialBounds);
    });
  },
);

windowsTitleBarIt(
  "moves the Windows custom window from page CSS draggable regions",
  async () => {
    await withWindowsTitleBarMuon(async (driver, _running, window) => {
      let currentWindow = window;
      await runTitleBarStep("navigate to draggable page", async () => {
        await driver.evaluate('location.href = "asset://main/draggable.html"');
        await driver.evaluate(
          'new Promise((resolve) => { const check = () => document.getElementById("no-drag-button") === null ? setTimeout(check, 50) : resolve(true); check(); })',
        );
      });

      await runTitleBarStep("click page no-drag control", async () => {
        await clickWindowsPagePoint(
          driver,
          currentWindow.bounds.x + 84,
          currentWindow.bounds.y + titleBarHeight + 130,
        );
        await expectWindowsWindowStill(currentWindow.bounds);
      });

      await runTitleBarStep(
        "wheel page no-drag region vertically",
        async () => {
          await driver.evaluate("window.scrollTo(0, 0)");
          await expectPageNoDragRegionPoint(driver, 84, 220);
          await sendNativeMouseWheelUntilPageScrollChange(
            driver,
            testWindowTitle,
            currentWindow.bounds.x + 84,
            currentWindow.bounds.y + titleBarHeight + 220,
            "down",
            "y",
          );
        },
      );

      await runTitleBarStep(
        "wheel page draggable region vertically",
        async () => {
          await driver.evaluate("window.scrollTo(0, 0)");
          await expectPageDragRegionPoint(driver, 220, 260);
          await sendNativeMouseWheelUntilPageScrollChange(
            driver,
            testWindowTitle,
            currentWindow.bounds.x + 220,
            currentWindow.bounds.y + titleBarHeight + 260,
            "down",
            "y",
          );
        },
      );

      await runTitleBarStep(
        "wheel page draggable region horizontally",
        async () => {
          await driver.evaluate("window.scrollTo(0, 0)");
          await expectPageDragRegionPoint(driver, 220, 260);
          await sendNativeMouseWheelUntilPageScrollChange(
            driver,
            testWindowTitle,
            currentWindow.bounds.x + 220,
            currentWindow.bounds.y + titleBarHeight + 260,
            "right",
            "x",
          );
        },
      );

      await runTitleBarStep(
        "drag title bar over page draggable regions",
        async () => {
          await dragWindowsMouse(
            currentWindow.bounds.x + 220,
            currentWindow.bounds.y + Math.round(titleBarHeight / 2),
            currentWindow.bounds.x + 320,
            currentWindow.bounds.y + Math.round(titleBarHeight / 2) + 80,
          );
          currentWindow = await waitForWindowsWindowMove(currentWindow.bounds);
        },
      );

      await runTitleBarStep("drag page background", async () => {
        await dragWindowsMouse(
          currentWindow.bounds.x + 220,
          currentWindow.bounds.y + titleBarHeight + 260,
          currentWindow.bounds.x + 320,
          currentWindow.bounds.y + titleBarHeight + 340,
        );
        await waitForWindowsWindowMove(currentWindow.bounds);
      });
    });
  },
);

windowsTitleBarIt(
  "uses browser.backgroundColor for the Windows custom title bar background",
  async () => {
    await withWindowsTitleBarMuon(async (_driver, _running, window) => {
      await runTitleBarStep(
        "verify configured title bar background",
        async () => await waitForWindowsTitleBarBackgroundColor(window),
      );
    }, configuredTitleBarBackgroundColor);
  },
);

windowsTitleBarIt(
  "uses the embedded default title bar icon on Windows",
  async () => {
    await withWindowsTitleBarMuon(async (_driver, _running, window) => {
      await runTitleBarStep(
        "verify configured title bar background",
        async () => await waitForWindowsTitleBarBackgroundColor(window),
      );
      await runTitleBarStep(
        "verify embedded default title bar icon",
        async () =>
          await waitForWindowsTitleBarIconContrast(
            window,
            expectedTitleBarBackgroundColor,
          ),
      );
    }, configuredTitleBarBackgroundColor);
  },
);

windowsTitleBarIt(
  "controls the Windows custom title bar visibility through config and browser API",
  async () => {
    await withWindowsTitleBarMuon(
      async (driver, _running, window) => {
        await runTitleBarStep(
          "verify initial title bar is hidden",
          async () => await waitForWindowsTitleBarHidden(window),
        );
        await runTitleBarStep("show title bar through API", async () => {
          await expect(
            driver.evaluate("window.muon.browser.setTitleBarVisibility(true)"),
          ).resolves.toBeUndefined();
          await waitForWindowsTitleBarBackgroundColor(window);
        });
        await runTitleBarStep("hide title bar through API", async () => {
          await expect(
            driver.evaluate("window.muon.browser.setTitleBarVisibility(false)"),
          ).resolves.toBeUndefined();
          await waitForWindowsTitleBarHidden(window);
        });
      },
      configuredTitleBarBackgroundColor,
      false,
    );
  },
);

windowsTitleBarIt(
  "shows and updates the Windows custom title bar icon",
  async () => {
    await withWindowsTitleBarMuon(
      async (driver, _running, window) => {
        await runTitleBarStep(
          "verify initial title bar icon",
          async () =>
            await waitForWindowsTitleBarIconColor(
              window,
              expectedInitialTitleBarIconColor,
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
          await waitForWindowsTitleBarIconColor(
            window,
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
            await waitForWindowsTitleBarIconColor(
              window,
              expectedSvgTitleBarIconColor,
            );
          },
        );
        await runTitleBarStep("clear title bar icon through API", async () => {
          await expect(
            driver.evaluate("window.muon.browser.setTitleBarIcon(null)"),
          ).resolves.toBeUndefined();
          await waitForWindowsTitleBarIconColor(
            window,
            expectedTitleBarBackgroundColor,
          );
        });
      },
      configuredTitleBarBackgroundColor,
      undefined,
      initialTitleBarIconPath,
    );
  },
);

windowsTitleBarIt(
  "updates the Windows custom title bar icon from favicon changes",
  async () => {
    await withWindowsTitleBarMuon(
      async (driver, _running, window) => {
        await runTitleBarStep(
          "verify initial title bar icon",
          async () =>
            await waitForWindowsTitleBarIconColor(
              window,
              expectedInitialTitleBarIconColor,
            ),
        );
        await runTitleBarStep("navigate to SVG favicon page", async () => {
          await driver.navigate("asset://main/favicon-svg.html");
          await waitForWindowsTitleBarIconColor(
            window,
            expectedSvgTitleBarIconColor,
          );
        });
        await runTitleBarStep(
          "reject an oversized favicon without blocking an ordinary image",
          async () => {
            await driver.navigate("asset://main/favicon-over-limit.html");
            await waitForDocumentTitle(
              driver,
              overLimitImageLoadedTitle,
              cdpCommandTimeoutMs,
            );
            await waitForWindowsTitleBarIconColor(
              window,
              expectedInitialTitleBarIconColor,
            );
          },
        );
        await runTitleBarStep(
          "fall back when favicon cannot be loaded",
          async () => {
            await driver.navigate("asset://main/broken-favicon.html");
            await waitForWindowsTitleBarIconColor(
              window,
              expectedInitialTitleBarIconColor,
            );
          },
        );
        await runTitleBarStep(
          "fall back when page has no favicon",
          async () => {
            await driver.navigate("asset://main/no-favicon.html");
            await waitForWindowsTitleBarIconColor(
              window,
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

windowsTitleBarIt(
  "keeps the Windows native title bar usable when visibility API has no decoration hint",
  async () => {
    await withWindowsTitleBarMuon(
      async (driver, _running, window) => {
        await expect(
          driver.evaluate("window.muon.browser.setTitleBarVisibility(false)"),
        ).resolves.toBeUndefined();
        const hiddenRequestWindow = await waitForWindowsTitleBarWindow();
        expect(hiddenRequestWindow.visible).toBe(true);
        expect(hiddenRequestWindow.enabled).toBe(true);
        expect(hiddenRequestWindow.bounds.width).toBe(window.bounds.width);

        await expect(
          driver.evaluate("window.muon.browser.setTitleBarVisibility(true)"),
        ).resolves.toBeUndefined();
        const shownRequestWindow = await waitForWindowsTitleBarWindow();
        expect(shownRequestWindow.visible).toBe(true);
        expect(shownRequestWindow.enabled).toBe(true);
      },
      undefined,
      undefined,
      undefined,
      "native",
    );
  },
);

windowsTitleBarIt(
  "rejects non-PNG title bar icons for the Windows native title bar",
  async () => {
    await withWindowsTitleBarMuon(
      async (driver) => {
        const message = await driver.evaluate<string>(`(async () => {
          try {
            await window.muon.browser.setTitleBarIcon(${JSON.stringify(
              svgTitleBarIconPath,
            )});
            return "resolved";
          } catch (error) {
            return error instanceof Error ? error.message : String(error);
          }
        })()`);
        expect(message).toContain("PNG");
      },
      undefined,
      undefined,
      undefined,
      "native",
    );
  },
);

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
        "wheel page no-drag region vertically",
        async () => {
          await driver.evaluate("window.scrollTo(0, 0)");
          await expectPageNoDragRegionPoint(driver, 84, 220);
          await sendNativeMouseWheelUntilPageScrollChange(
            driver,
            testWindowTitle,
            bounds.x + 84,
            bounds.y + titleBarHeight + 220,
            "down",
            "y",
          );
        },
      );

      await runTitleBarStep(
        "wheel page draggable region vertically",
        async () => {
          await driver.evaluate("window.scrollTo(0, 0)");
          await expectPageDragRegionPoint(driver, 220, 260);
          await sendNativeMouseWheelUntilPageScrollChange(
            driver,
            testWindowTitle,
            bounds.x + 220,
            bounds.y + titleBarHeight + 260,
            "down",
            "y",
          );
        },
      );

      await runTitleBarStep(
        "wheel page draggable region horizontally",
        async () => {
          await driver.evaluate("window.scrollTo(0, 0)");
          await expectPageDragRegionPoint(driver, 220, 260);
          await sendNativeMouseWheelUntilPageScrollChange(
            driver,
            testWindowTitle,
            bounds.x + 220,
            bounds.y + titleBarHeight + 260,
            "right",
            "x",
          );
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
          "reject an oversized favicon without blocking an ordinary image",
          async () => {
            await driver.navigate("asset://main/favicon-over-limit.html");
            await waitForDocumentTitle(
              driver,
              overLimitImageLoadedTitle,
              cdpCommandTimeoutMs,
            );
            await waitForTitleBarIconColor(
              running,
              bounds,
              expectedInitialTitleBarIconColor,
            );
          },
        );
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
  "bounds streamed Linux custom title bar favicon responses",
  async () => {
    const exactPath = "/favicon-exact.svg";
    const exactAgainPath = "/favicon-exact-again.svg";
    const knownOverPath = "/favicon-known-over.svg";
    const chunkedOverPath = "/favicon-chunked-over.svg";
    const exactSvg = createSizedSvg(
      expectedSvgTitleBarIconColor,
      faviconResponseLimitBytes,
    );
    const overSvg = Buffer.concat([exactSvg, Buffer.from(" ")]);
    const probes: FaviconResponseProbe[] = [];
    const server = await startHttpServer((request, response) => {
      const path = new URL(request.url ?? "/", "http://127.0.0.1").pathname;
      if (
        path !== exactPath &&
        path !== exactAgainPath &&
        path !== knownOverPath &&
        path !== chunkedOverPath
      ) {
        response.writeHead(404, { "Content-Type": "text/plain" });
        response.end("missing\n");
        return;
      }

      trackFaviconResponse(probes, path, response);
      const headers: Record<string, string | number> = {
        "Cache-Control": "no-store",
        Connection: "close",
        "Content-Type": "image/svg+xml",
      };
      if (path === knownOverPath) {
        headers["Content-Length"] = overSvg.length;
      }
      response.writeHead(200, headers);

      if (path === exactPath || path === exactAgainPath) {
        response.write(exactSvg);
        response.end();
        return;
      }
      if (path === knownOverPath) {
        response.flushHeaders();
        response.write(overSvg.subarray(0, 1));
        return;
      }

      const writeOverflowByte = (): void => {
        if (!response.destroyed) {
          response.write(overSvg.subarray(faviconResponseLimitBytes));
        }
      };
      if (response.write(exactSvg)) {
        setTimeout(writeOverflowByte, 0);
      } else {
        response.once("drain", writeOverflowByte);
      }
    });

    try {
      await withTitleBarMuon(
        async (driver, running, _env, bounds) => {
          await runTitleBarStep(
            "verify initial title bar icon before streamed favicons",
            async () =>
              await waitForTitleBarIconColor(
                running,
                bounds,
                expectedInitialTitleBarIconColor,
              ),
          );
          await runTitleBarStep(
            "accept a chunked favicon exactly at the response limit",
            async () => {
              await setPageFavicon(driver, `${server.origin}${exactPath}`);
              await waitForFaviconResponseStatus(
                probes,
                exactPath,
                "completed",
              );
              await waitForTitleBarIconColor(
                running,
                bounds,
                expectedSvgTitleBarIconColor,
              );
            },
          );
          await runTitleBarStep(
            "reject a known oversized favicon before body completion",
            async () => {
              await setPageFavicon(driver, `${server.origin}${knownOverPath}`);
              await waitForFaviconResponseStatus(
                probes,
                knownOverPath,
                "canceled",
              );
              await waitForTitleBarIconColor(
                running,
                bounds,
                expectedInitialTitleBarIconColor,
              );
            },
          );
          await runTitleBarStep(
            "restore a valid streamed favicon after known-size rejection",
            async () => {
              await setPageFavicon(driver, `${server.origin}${exactAgainPath}`);
              await waitForFaviconResponseStatus(
                probes,
                exactAgainPath,
                "completed",
              );
              await waitForTitleBarIconColor(
                running,
                bounds,
                expectedSvgTitleBarIconColor,
              );
            },
          );
          await runTitleBarStep(
            "reject a chunked favicon one byte above the response limit",
            async () => {
              await setPageFavicon(
                driver,
                `${server.origin}${chunkedOverPath}`,
              );
              await waitForFaviconResponseStatus(
                probes,
                chunkedOverPath,
                "canceled",
              );
              await waitForTitleBarIconColor(
                running,
                bounds,
                expectedInitialTitleBarIconColor,
              );
            },
          );
          await runTitleBarStep(
            "restore a normal favicon after streamed response rejection",
            async () => {
              await setPageFavicon(
                driver,
                "asset://main/icons/title-bar-vector.svg",
              );
              await waitForTitleBarIconColor(
                running,
                bounds,
                expectedSvgTitleBarIconColor,
              );
            },
          );
        },
        configuredTitleBarBackgroundColor,
        undefined,
        initialTitleBarIconPath,
        undefined,
        [...TEST_NETWORK_ALLOW_PATTERNS, `${server.origin}/**`],
      );
    } finally {
      for (const probe of probes) {
        if (probe.status === "active") {
          probe.response.destroy();
        }
      }
      await stopHttpServer(server.server);
    }
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
