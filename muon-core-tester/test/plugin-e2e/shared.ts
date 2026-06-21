// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import { execFile, spawn, type ChildProcess } from "node:child_process";
import {
  access,
  appendFile,
  constants,
  copyFile,
  mkdir,
  mkdtemp,
  readFile,
  rm,
  stat,
  writeFile,
} from "node:fs/promises";
import {
  createServer,
  type IncomingMessage,
  type Server,
  type ServerResponse,
} from "node:http";
import { tmpdir } from "node:os";
import { delimiter, join, relative, resolve } from "node:path";
import { pathToFileURL } from "node:url";
import { promisify } from "node:util";

import {
  launchGtkApp,
  type GtkApp,
  type GtkButtonElement,
  type GtkWidgetElement,
  type GtkWindowElement,
  type GtkWindowTextClickOptions,
} from "gestament";
import { afterEach, describe, expect } from "vitest";

import {
  connectToMuonCdp,
  isMuonTitleBarTarget,
  listCdpTargets,
  MUON_TITLE_BAR_TARGET_TITLE,
  type CdpTarget,
  type CdpDriver,
} from "../../src/helper.js";

export {
  access,
  appendFile,
  constants,
  connectToMuonCdp,
  isMuonTitleBarTarget,
  join,
  listCdpTargets,
  mkdir,
  mkdtemp,
  MUON_TITLE_BAR_TARGET_TITLE,
  readFile,
  rm,
  tmpdir,
  writeFile,
};

export type { CdpDriver, CdpTarget };

const resolveOrUndefined = async <T>(
  operation: () => Promise<T>,
): Promise<T | undefined> => {
  try {
    return await operation();
  } catch {
    return undefined;
  }
};

export interface RunningMuon {
  process: ChildProcess;
  pluginDirectory: string;
  stderr: string;
  usesValgrind: boolean;
}

export interface RuntimeEvaluateResponse {
  result?: {
    value?: unknown;
  };
  exceptionDetails?: unknown;
}

export interface NetworkResponseReceivedParams {
  response?: {
    url?: string;
    status?: number;
  };
}

export interface KeyboardShortcutEvent {
  [key: string]: string | number;
  type: string;
  windowsVirtualKeyCode: number;
  nativeVirtualKeyCode: number;
  key: string;
  code: string;
  modifiers: number;
}

export interface DevToolsShortcut {
  name: string;
  config: string;
  event: KeyboardShortcutEvent;
}

export interface BrowserShortcutConfig {
  devtools: string | undefined;
  reload: string | undefined;
  hardReload: string | undefined;
  fullscreen: string | undefined;
  zoomIn: string | undefined;
  zoomOut: string | undefined;
  resetZoom: string | undefined;
}

export type BrowserInitialWindowState =
  | "normal"
  | "hidden"
  | "minimized"
  | "maximized"
  | "fullscreen";

export interface BrowserOuterSize {
  width: number;
  height: number;
}

export interface PluginConfigEntry {
  name: string;
  allow: string[];
}

export interface NetworkAuthorizedOriginConfig {
  scheme: string;
  domain: string;
  port?: number;
}

export interface RunningHttpServer {
  server: Server;
  origin: string;
  port: number;
}

export interface RunningGestamentMuon {
  app: GtkApp;
  driver: CdpDriver;
  pluginDirectory: string;
}

export interface GestamentWindowDiagnostic {
  index: number;
  kind: string | undefined;
  name: string | undefined;
  title: string | undefined;
  seenBy: readonly string[] | undefined;
  matchedBy: string | undefined;
  rawIds: unknown;
  error: string | undefined;
}

export interface GestamentNativeWindowMatch {
  window: GtkWindowElement;
  diagnostics: GestamentWindowDiagnostic[];
}

export interface GestamentNativeDialogButton {
  click: () => Promise<void>;
  detection: "semantic" | "windowText";
  label: string;
}

export interface NativeFileDialogProbeOptions {
  title: string;
  defaultPath: string;
  buttonLabel: string;
  modal?: boolean;
}

export interface NativeDialogProbeResult {
  status: "fulfilled" | "rejected" | "timeout";
  value?: unknown;
  name?: string;
  message?: string;
}

interface GestamentButtonDiagnostic {
  depth: number;
  error: string | undefined;
  indexPath: string;
  kind: string | undefined;
  name: string | undefined;
  roleName: string | undefined;
}

type GestamentChildContainer = GtkWidgetElement & {
  childAt: (index: number) => Promise<GtkWidgetElement | undefined>;
  getChildCount: () => Promise<number>;
};

export const MUON_PORT = 9222;
export const MUON_APP_URL = "asset://main/index.html";
export const TEST_NETWORK_ALLOW_PATTERNS = ["asset://main/**", "data:**"];
export const TEST_BROWSER_PLUGIN_ALLOW_PATTERNS = TEST_NETWORK_ALLOW_PATTERNS;
export const TEST_PLUGIN_ALLOW_PATTERNS = ["muon.**"];
export const DEBUG_MUON_DIRECTORY = resolve(
  "..",
  "muon-core",
  ".run",
  "test-linux64-debug",
);
export const RELEASE_MUON_DIRECTORY = resolve(
  "..",
  "muon-core",
  ".run",
  "test-linux64-release",
);
export const TEST_PLUGIN_DIRECTORY = resolve(
  "..",
  "muon-core",
  ".run",
  "test-linux64-debug",
  "test-plugins",
);
export const PLUGIN_SUFFIX = process.platform === "win32" ? ".dll" : ".so";
export const STANDARD_PLUGIN_NAMES = [];
const STANDARD_PLUGIN_FUNCTION_PATHS: Record<string, string[]> = {};
export const isWindows = process.platform === "win32";
export const shouldExpectFfiClosureTracker =
  process.env.MUON_EXPECT_FFI_CLOSURE_TRACKER === "1";
export const shouldUseValgrind = process.env.MUON_TEST_USE_VALGRIND === "1";
export const shouldForceX11Ozone =
  process.platform === "linux" &&
  process.env.MUON_TEST_XVFB_WINDOW_MANAGER === "1";
export const cdpStartupTimeoutMs = shouldUseValgrind ? 120000 : 30000;
export const cdpCommandTimeoutMs = shouldUseValgrind ? 60000 : 10000;
export const targetTimeoutMs = shouldUseValgrind ? 60000 : 5000;
export const processExitTimeoutMs = shouldUseValgrind ? 120000 : 5000;
export const runningProcesses: RunningMuon[] = [];
export const execFileAsync = promisify(execFile);
export const valgrindArgs = [
  "--tool=memcheck",
  "--leak-check=full",
  "--show-leak-kinds=definite,indirect",
  "--errors-for-leak-kinds=definite,indirect",
  "--error-exitcode=97",
  "--track-origins=yes",
  "--trace-children=no",
];
export const emptyBrowserShortcutConfig: BrowserShortcutConfig = {
  devtools: undefined,
  reload: undefined,
  hardReload: undefined,
  fullscreen: undefined,
  zoomIn: undefined,
  zoomOut: undefined,
  resetZoom: undefined,
};
export const browserFunctionNames = [
  "reload",
  "hardReload",
  "toggleFullscreen",
  "enterFullscreen",
  "exitFullscreen",
  "zoomIn",
  "zoomOut",
  "resetZoom",
  "show",
  "hide",
  "focus",
  "blur",
  "minimize",
  "maximize",
  "restore",
  "close",
  "shutdown",
] as const;

export const getMuonExecutable = (directory: string): string =>
  resolve(
    directory,
    process.platform === "win32" ? "muon-core.exe" : "muon-core",
  );

export const createBrowserShortcutConfig = (
  overrides: Partial<BrowserShortcutConfig>,
): BrowserShortcutConfig => ({
  ...emptyBrowserShortcutConfig,
  ...overrides,
});

export const wait = async (ms: number): Promise<void> => {
  await new Promise<void>((resolvePromise) => {
    setTimeout(resolvePromise, ms);
  });
};

export const requireFile = async (path: string): Promise<void> => {
  const entry = await stat(path);
  if (!entry.isFile()) {
    throw new Error(`Expected file does not exist: ${path}`);
  }
};

export const findExecutableOnPath = async (
  executableName: string,
): Promise<string | undefined> => {
  for (const directory of (process.env.PATH ?? "").split(delimiter)) {
    if (directory === "") {
      continue;
    }
    const candidate = join(directory, executableName);
    try {
      await access(candidate, constants.X_OK);
      return candidate;
    } catch {
      continue;
    }
  }
  return undefined;
};

export const createMuonProcessCommand = async (
  executable: string,
  args: string[],
  useValgrind: boolean,
): Promise<{ command: string; commandArgs: string[] }> => {
  const valgrind = useValgrind
    ? await findExecutableOnPath("valgrind")
    : undefined;
  if (useValgrind && valgrind === undefined) {
    throw new Error("valgrind is required when MUON_TEST_USE_VALGRIND=1");
  }

  const xvfbRun =
    process.env.DISPLAY === undefined
      ? await findExecutableOnPath("xvfb-run")
      : undefined;
  const appCommand = valgrind ?? executable;
  const appArgs =
    valgrind === undefined ? args : [...valgrindArgs, executable, ...args];
  if (xvfbRun === undefined) {
    return { command: appCommand, commandArgs: appArgs };
  }
  return { command: xvfbRun, commandArgs: ["-a", appCommand, ...appArgs] };
};

export const waitForProcessExitOrTimeout = async (
  running: RunningMuon,
  timeoutMs: number,
): Promise<boolean> => {
  try {
    await waitForProcessExit(running, timeoutMs);
    return true;
  } catch {
    return false;
  }
};

export const expectFfiClosureTrackerBalanced = (stderr: string): void => {
  if (!shouldExpectFfiClosureTracker) {
    return;
  }

  const matches = Array.from(
    stderr.matchAll(
      /MUON_FFI_CLOSURE_TRACKER alloc=(\d+) free=(\d+) live=(\d+) high_water=(\d+)/g,
    ),
  );
  if (matches.length === 0) {
    throw new Error("Missing MUON_FFI_CLOSURE_TRACKER marker");
  }
  const marker = matches.at(-1);
  if (marker === undefined) {
    throw new Error("Missing MUON_FFI_CLOSURE_TRACKER marker");
  }
  const allocCount = Number(marker[1] ?? "NaN");
  const freeCount = Number(marker[2] ?? "NaN");
  const liveCount = Number(marker[3] ?? "NaN");
  const highWater = Number(marker[4] ?? "NaN");
  expect(allocCount).toBeGreaterThan(0);
  expect(freeCount).toBe(allocCount);
  expect(liveCount).toBe(0);
  expect(highWater).toBeGreaterThan(0);
};

export const waitForCdp = async (timeoutMs: number): Promise<void> => {
  const deadline = Date.now() + timeoutMs;
  let lastError: unknown = undefined;
  while (Date.now() < deadline) {
    try {
      const targets = await listCdpTargets({
        port: MUON_PORT,
        timeoutMs: 1000,
      });
      if (
        targets.some(
          (target) =>
            target.type === "page" &&
            target.webSocketDebuggerUrl !== undefined &&
            target.url !== "about:blank" &&
            !isMuonTitleBarTarget(target),
        )
      ) {
        return;
      }
    } catch (error) {
      lastError = error;
    }
    await wait(100);
  }
  throw new Error(`Timed out waiting for muon CDP: ${String(lastError)}`);
};

export const waitForDevToolsTarget = async (
  previousTargetIds: Set<string>,
  timeoutMs: number,
): Promise<CdpTarget> => {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    const targets = await listCdpTargets({
      port: MUON_PORT,
      timeoutMs: 1000,
    });
    const devToolsTarget = targets.find(
      (target) =>
        !previousTargetIds.has(target.id) &&
        target.type === "page" &&
        target.url.startsWith("devtools://"),
    );
    if (devToolsTarget !== undefined) {
      return devToolsTarget;
    }
    await wait(100);
  }
  throw new Error("Timed out waiting for DevTools target");
};

export const waitForTargetClosed = async (
  targetId: string,
  timeoutMs: number,
): Promise<void> => {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    const targets = await listCdpTargets({
      port: MUON_PORT,
      timeoutMs: 1000,
    });
    if (!targets.some((target) => target.id === targetId)) {
      return;
    }
    await wait(100);
  }
  throw new Error(`Timed out waiting for CDP target to close: ${targetId}`);
};

export const waitForProcessExit = async (
  running: RunningMuon,
  timeoutMs: number,
): Promise<void> => {
  if (
    running.process.exitCode !== null ||
    running.process.signalCode !== null
  ) {
    return;
  }

  await new Promise<void>((resolvePromise, reject) => {
    const timer = setTimeout(() => {
      reject(
        new Error(`Timed out waiting for muon to exit after ${timeoutMs}ms`),
      );
    }, timeoutMs);
    running.process.once("exit", () => {
      clearTimeout(timer);
      resolvePromise();
    });
  });
};

export const listProcessGroupCommandLines = async (
  processGroupId: number,
): Promise<string[]> => {
  const { stdout } = await execFileAsync("ps", ["-eo", "pgid=,args="]);
  return String(stdout)
    .split("\n")
    .flatMap((line) => {
      const match = /^\s*(\d+)\s+(.*)$/.exec(line);
      if (match === null || Number(match[1]) !== processGroupId) {
        return [];
      }
      const commandLine = match[2];
      return commandLine === undefined ? [] : [commandLine];
    });
};

export const parseXpropWindowTitle = (output: string): string | undefined => {
  for (const line of output.split("\n")) {
    const match = /^(?:_NET_WM_NAME|WM_NAME)(?:\([^)]*\))? = "([^"]*)"/.exec(
      line,
    );
    if (match !== null && match[1] !== undefined) {
      return match[1];
    }
  }
  return undefined;
};

export const parseXpropWindowStateAtoms = (output: string): string[] => {
  for (const line of output.split("\n")) {
    if (!line.startsWith("_NET_WM_STATE")) {
      continue;
    }
    const match = /^_NET_WM_STATE(?:\([^)]*\))? = (.*)$/.exec(line);
    if (match === null || match[1] === undefined) {
      return [];
    }
    return match[1]
      .split(",")
      .map((value) => value.trim())
      .filter((value) => value.length > 0 && value !== "not found.");
  }
  return [];
};

export const listNativeWindowTitles = async (): Promise<
  string[] | undefined
> => {
  if (process.platform !== "linux" || process.env.DISPLAY === undefined) {
    return undefined;
  }
  const xprop = await findExecutableOnPath("xprop");
  if (xprop === undefined) {
    return undefined;
  }

  let rootStdout: string | Buffer;
  try {
    const root = await execFileAsync(xprop, ["-root", "_NET_CLIENT_LIST"]);
    rootStdout = root.stdout;
  } catch {
    return undefined;
  }
  const ids = String(rootStdout).match(/0x[0-9a-f]+/gi) ?? [];
  const titles: string[] = [];
  for (const id of ids) {
    try {
      const { stdout } = await execFileAsync(xprop, [
        "-id",
        id,
        "_NET_WM_NAME",
        "WM_NAME",
      ]);
      const title = parseXpropWindowTitle(String(stdout));
      if (title !== undefined) {
        titles.push(title);
      }
    } catch {
      continue;
    }
  }
  return titles;
};

export const waitForNativeWindowTitleAbsent = async (
  expectedTitle: string,
  timeoutMs: number,
): Promise<void> => {
  const deadline = Date.now() + timeoutMs;
  let lastTitles: string[] | undefined = undefined;
  while (Date.now() < deadline) {
    const titles = await listNativeWindowTitles();
    if (titles === undefined) {
      throw new Error("Native X11 window title inspection is unavailable");
    }
    lastTitles = titles;
    if (!titles.includes(expectedTitle)) {
      return;
    }
    await wait(100);
  }
  throw new Error(
    `Timed out waiting for native window title '${expectedTitle}' to disappear. Last titles: ${JSON.stringify(lastTitles ?? [])}`,
  );
};

export const readNativeWindowStateAtoms = async (
  expectedTitle: string,
): Promise<string[] | undefined> => {
  if (process.platform !== "linux" || process.env.DISPLAY === undefined) {
    return undefined;
  }
  const xprop = await findExecutableOnPath("xprop");
  if (xprop === undefined) {
    return undefined;
  }

  let rootStdout: string | Buffer;
  try {
    const root = await execFileAsync(xprop, ["-root", "_NET_CLIENT_LIST"]);
    rootStdout = root.stdout;
  } catch {
    return undefined;
  }
  const ids = String(rootStdout).match(/0x[0-9a-f]+/gi) ?? [];
  for (const id of ids) {
    try {
      const { stdout } = await execFileAsync(xprop, [
        "-id",
        id,
        "_NET_WM_NAME",
        "WM_NAME",
        "_NET_WM_STATE",
      ]);
      const output = String(stdout);
      if (parseXpropWindowTitle(output) === expectedTitle) {
        return parseXpropWindowStateAtoms(output);
      }
    } catch {
      continue;
    }
  }
  return [];
};

export const readNativeActiveWindowTitle = async (): Promise<
  string | undefined
> => {
  if (process.platform !== "linux" || process.env.DISPLAY === undefined) {
    return undefined;
  }
  const xprop = await findExecutableOnPath("xprop");
  if (xprop === undefined) {
    return undefined;
  }

  let activeWindowStdout: string | Buffer;
  try {
    const activeWindow = await execFileAsync(xprop, [
      "-root",
      "_NET_ACTIVE_WINDOW",
    ]);
    activeWindowStdout = activeWindow.stdout;
  } catch {
    return undefined;
  }
  const id = String(activeWindowStdout).match(/0x[0-9a-f]+/i)?.[0];
  if (id === undefined || id === "0x0") {
    return "";
  }

  try {
    const { stdout } = await execFileAsync(xprop, [
      "-id",
      id,
      "_NET_WM_NAME",
      "WM_NAME",
    ]);
    return parseXpropWindowTitle(String(stdout)) ?? "";
  } catch {
    return "";
  }
};

export const waitForNativeActiveWindowTitle = async (
  expectedTitle: string,
  timeoutMs: number,
): Promise<void> => {
  const deadline = Date.now() + timeoutMs;
  let lastTitle: string | undefined = undefined;
  while (Date.now() < deadline) {
    const title = await readNativeActiveWindowTitle();
    if (title === undefined) {
      throw new Error("Native X11 active window inspection is unavailable");
    }
    lastTitle = title;
    if (title === expectedTitle) {
      return;
    }
    await wait(100);
  }
  throw new Error(
    `Timed out waiting for native active window title '${expectedTitle}'. Last title: ${JSON.stringify(lastTitle ?? "")}`,
  );
};

export const waitForNativeWindowStates = async (
  expectedTitle: string,
  expectedStates: string[],
  timeoutMs: number,
): Promise<void> => {
  const deadline = Date.now() + timeoutMs;
  let lastStates: string[] | undefined = undefined;
  while (Date.now() < deadline) {
    const states = await readNativeWindowStateAtoms(expectedTitle);
    if (states === undefined) {
      throw new Error("Native X11 window state inspection is unavailable");
    }
    lastStates = states;
    if (expectedStates.every((state) => states.includes(state))) {
      return;
    }
    await wait(100);
  }
  throw new Error(
    `Timed out waiting for native window states ${JSON.stringify(
      expectedStates,
    )} on '${expectedTitle}'. Last states: ${JSON.stringify(lastStates ?? [])}`,
  );
};

export const waitForNativeWindowStatesAbsent = async (
  expectedTitle: string,
  absentStates: string[],
  timeoutMs: number,
): Promise<void> => {
  const deadline = Date.now() + timeoutMs;
  let lastStates: string[] | undefined = undefined;
  while (Date.now() < deadline) {
    const states = await readNativeWindowStateAtoms(expectedTitle);
    if (states === undefined) {
      throw new Error("Native X11 window state inspection is unavailable");
    }
    lastStates = states;
    if (absentStates.every((state) => !states.includes(state))) {
      return;
    }
    await wait(100);
  }
  throw new Error(
    `Timed out waiting for native window states ${JSON.stringify(
      absentStates,
    )} to disappear on '${expectedTitle}'. Last states: ${JSON.stringify(
      lastStates ?? [],
    )}`,
  );
};

export const waitForNativeWindowTitle = async (
  expectedTitle: string,
  timeoutMs: number,
): Promise<boolean> => {
  const deadline = Date.now() + timeoutMs;
  let lastTitles: string[] | undefined = undefined;
  while (Date.now() < deadline) {
    const titles = await listNativeWindowTitles();
    if (titles === undefined) {
      return false;
    }
    lastTitles = titles;
    if (titles.includes(expectedTitle)) {
      return true;
    }
    await wait(100);
  }
  throw new Error(
    `Timed out waiting for native window title '${expectedTitle}'. Last titles: ${JSON.stringify(
      lastTitles ?? [],
    )}`,
  );
};

export const getCurrentTargetIds = async (): Promise<Set<string>> => {
  const targets = await listCdpTargets({
    port: MUON_PORT,
    timeoutMs: 1000,
  });
  return new Set(targets.map((target) => target.id));
};

export const waitForPageTargetUrl = async (
  expectedUrl: string,
  timeoutMs: number,
): Promise<CdpTarget> => {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    const targets = await listCdpTargets({
      port: MUON_PORT,
      timeoutMs: 1000,
    });
    const target = targets.find(
      (candidate) => candidate.type === "page" && candidate.url === expectedUrl,
    );
    if (target !== undefined) {
      return target;
    }
    await wait(100);
  }
  throw new Error(`Timed out waiting for page target URL: ${expectedUrl}`);
};

export const waitForNewPageTarget = async (
  previousTargetIds: Set<string>,
  timeoutMs: number,
  matches: (target: CdpTarget) => boolean = () => true,
): Promise<CdpTarget> => {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    const targets = await listCdpTargets({
      port: MUON_PORT,
      timeoutMs: 1000,
    });
    const target = targets.find(
      (candidate) =>
        !previousTargetIds.has(candidate.id) &&
        candidate.type === "page" &&
        !isMuonTitleBarTarget(candidate) &&
        matches(candidate),
    );
    if (target !== undefined) {
      return target;
    }
    await wait(100);
  }
  throw new Error("Timed out waiting for new page target");
};

export const expectNoNewPageTarget = async (
  previousTargetIds: Set<string>,
  timeoutMs: number,
): Promise<void> => {
  await wait(timeoutMs);
  const targets = await listCdpTargets({
    port: MUON_PORT,
    timeoutMs: 1000,
  });
  expect(
    targets.some(
      (candidate) =>
        !previousTargetIds.has(candidate.id) &&
        candidate.type === "page" &&
        !isMuonTitleBarTarget(candidate),
    ),
  ).toBe(false);
};

export const openPopupTarget = async (
  driver: CdpDriver,
  url: string,
  features = "",
  targetName = "_blank",
): Promise<CdpTarget> => {
  const previousTargetIds = await getCurrentTargetIds();
  const response = await driver.send<RuntimeEvaluateResponse>(
    "Runtime.evaluate",
    {
      expression: `window.open(${JSON.stringify(
        url,
      )}, ${JSON.stringify(targetName)}, ${JSON.stringify(features)}); "opened"`,
      returnByValue: true,
      userGesture: true,
    },
  );
  if (response.exceptionDetails !== undefined) {
    throw new Error("popup open script failed");
  }
  return await waitForNewPageTarget(
    previousTargetIds,
    targetTimeoutMs,
    (target) => target.url === url,
  );
};

export const waitForNetworkResponse = async (
  driver: CdpDriver,
  expectedUrl: string,
  timeoutMs: number,
): Promise<NetworkResponseReceivedParams> =>
  await new Promise<NetworkResponseReceivedParams>((resolvePromise, reject) => {
    let unsubscribe: (() => void) | undefined = undefined;
    const timer = setTimeout(() => {
      unsubscribe?.();
      reject(
        new Error(`Timed out waiting for network response: ${expectedUrl}`),
      );
    }, timeoutMs);

    unsubscribe = driver.on<NetworkResponseReceivedParams>(
      "Network.responseReceived",
      (params) => {
        if (params.response?.url !== expectedUrl) {
          return;
        }
        clearTimeout(timer);
        unsubscribe?.();
        resolvePromise(params);
      },
    );
  });

export const startHttpServer = async (
  handler: (request: IncomingMessage, response: ServerResponse) => void,
): Promise<RunningHttpServer> => {
  const server = createServer(handler);
  await new Promise<void>((resolvePromise, reject) => {
    const handleError = (error: Error): void => {
      server.off("listening", handleListening);
      reject(error);
    };
    const handleListening = (): void => {
      server.off("error", handleError);
      resolvePromise();
    };
    server.once("error", handleError);
    server.listen(0, "127.0.0.1", handleListening);
  });

  const address = server.address();
  if (address === null || typeof address === "string") {
    throw new Error("HTTP server did not bind to a TCP port");
  }
  return {
    server,
    origin: `http://127.0.0.1:${String(address.port)}`,
    port: address.port,
  };
};

export const stopHttpServer = async (server: Server): Promise<void> => {
  await new Promise<void>((resolvePromise, reject) => {
    server.close((error) => {
      if (error !== undefined) {
        reject(error);
        return;
      }
      resolvePromise();
    });
  });
};

export const dispatchDevToolsShortcut = async (
  driver: CdpDriver,
  event: KeyboardShortcutEvent,
): Promise<CdpTarget> => {
  const previousTargetIds = await getCurrentTargetIds();
  await driver.send("Input.dispatchKeyEvent", event);
  return await waitForDevToolsTarget(previousTargetIds, targetTimeoutMs);
};

export const expectNoDevTools = async (
  driver: CdpDriver,
  event: KeyboardShortcutEvent,
): Promise<void> => {
  const previousTargetIds = await getCurrentTargetIds();
  await driver.send("Input.dispatchKeyEvent", event);
  await expect(waitForDevToolsTarget(previousTargetIds, 1000)).rejects.toThrow(
    "Timed out waiting for DevTools target",
  );
};

export const dispatchKeyboardShortcut = async (
  driver: CdpDriver,
  event: KeyboardShortcutEvent,
): Promise<void> => {
  await driver.send("Input.dispatchKeyEvent", event);
};

export const expectNoPageLoad = async (
  driver: CdpDriver,
  event: KeyboardShortcutEvent,
): Promise<void> => {
  const loadEvent = driver.waitForEvent("Page.loadEventFired", 1000);
  await dispatchKeyboardShortcut(driver, event);
  await expect(loadEvent).rejects.toThrow(
    "CDP event 'Page.loadEventFired' timed out",
  );
};

export const getOuterSize = async (
  driver: CdpDriver,
): Promise<BrowserOuterSize> =>
  await driver.evaluate<BrowserOuterSize>(
    "({ width: window.outerWidth, height: window.outerHeight })",
  );

export const isDifferentOuterSize = (
  actual: BrowserOuterSize,
  expected: BrowserOuterSize,
): boolean =>
  actual.width !== expected.width || actual.height !== expected.height;

export const waitForOuterSizeChange = async (
  driver: CdpDriver,
  initialSize: BrowserOuterSize,
  timeoutMs: number,
): Promise<BrowserOuterSize> => {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    const size = await getOuterSize(driver);
    if (isDifferentOuterSize(size, initialSize)) {
      return size;
    }
    await wait(100);
  }
  throw new Error("Timed out waiting for outer window size change");
};

export const waitForInnerWidth = async (
  driver: CdpDriver,
  matches: (value: number) => boolean,
  timeoutMs: number,
): Promise<number> => {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    const value = await driver.evaluate<number>("window.innerWidth");
    if (matches(value)) {
      return value;
    }
    await wait(100);
  }
  throw new Error("Timed out waiting for innerWidth change");
};

export const waitForMuonStderr = async (
  running: RunningMuon,
  expected: string,
  timeoutMs: number,
): Promise<void> => {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (running.stderr.includes(expected)) {
      return;
    }
    await wait(100);
  }
  throw new Error(`Timed out waiting for Muon stderr: ${expected}`);
};

export const waitForDocumentTitle = async (
  driver: CdpDriver,
  expectedTitle: string,
  timeoutMs: number,
): Promise<void> => {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    try {
      if ((await driver.evaluate<string>("document.title")) === expectedTitle) {
        return;
      }
    } catch (error) {
      if (!String(error).includes("Cannot find default execution context")) {
        throw error;
      }
    }
    await wait(100);
  }
  throw new Error(`Timed out waiting for document title: ${expectedTitle}`);
};

export const createPluginDirectory = async (
  muonDirectory: string,
  pluginNames: string[],
  includeStandardPlugins = true,
): Promise<string> => {
  const pluginDirectory = await mkdtemp(join(tmpdir(), "muon-plugins-"));
  if (includeStandardPlugins) {
    for (const pluginName of STANDARD_PLUGIN_NAMES) {
      const fileName = `${pluginName}${PLUGIN_SUFFIX}`;
      await copyFile(
        join(muonDirectory, "plugins", fileName),
        join(pluginDirectory, fileName),
      );
    }
  }
  for (const pluginName of pluginNames) {
    const fileName = `${pluginName}${PLUGIN_SUFFIX}`;
    await copyFile(
      join(TEST_PLUGIN_DIRECTORY, fileName),
      join(pluginDirectory, fileName),
    );
  }
  return pluginDirectory;
};

const matchPluginAllowPattern = (pattern: string, target: string): boolean => {
  const memo = new Map<string, boolean>();
  const matchFrom = (patternIndex: number, targetIndex: number): boolean => {
    const memoKey = `${String(patternIndex)}:${String(targetIndex)}`;
    const cached = memo.get(memoKey);
    if (cached !== undefined) {
      return cached;
    }
    let matched = false;
    if (patternIndex >= pattern.length) {
      matched = targetIndex === target.length;
    } else if (pattern[patternIndex] === "\\") {
      const literalIndex = patternIndex + 1;
      matched =
        literalIndex < pattern.length &&
        targetIndex < target.length &&
        pattern[literalIndex] === target[targetIndex] &&
        matchFrom(patternIndex + 2, targetIndex + 1);
    } else if (
      pattern[patternIndex] === "*" &&
      pattern[patternIndex + 1] === "*"
    ) {
      for (
        let nextTargetIndex = targetIndex;
        nextTargetIndex <= target.length;
        nextTargetIndex += 1
      ) {
        if (matchFrom(patternIndex + 2, nextTargetIndex)) {
          matched = true;
          break;
        }
      }
    } else if (pattern[patternIndex] === "*") {
      for (
        let nextTargetIndex = targetIndex;
        nextTargetIndex <= target.length;
        nextTargetIndex += 1
      ) {
        if (matchFrom(patternIndex + 1, nextTargetIndex)) {
          matched = true;
          break;
        }
        if (
          nextTargetIndex >= target.length ||
          target[nextTargetIndex] === "."
        ) {
          break;
        }
      }
    } else {
      matched =
        targetIndex < target.length &&
        pattern[patternIndex] === target[targetIndex] &&
        matchFrom(patternIndex + 1, targetIndex + 1);
    }
    memo.set(memoKey, matched);
    return matched;
  };
  return matchFrom(0, 0);
};

const filterStandardPluginAllowPatterns = (
  pluginName: string,
  allowPatterns: string[],
): string[] => {
  const functionPaths = STANDARD_PLUGIN_FUNCTION_PATHS[pluginName] ?? [];
  if (functionPaths.length === 0) {
    return [];
  }
  return allowPatterns.filter((pattern) =>
    functionPaths.some((functionPath) =>
      matchPluginAllowPattern(pattern, functionPath),
    ),
  );
};

export const createPluginConfigEntries = (
  pluginNames: string[],
  allowPatterns: string[],
  includeStandardPlugins = true,
): PluginConfigEntry[] => {
  const standardPluginEntries = includeStandardPlugins
    ? STANDARD_PLUGIN_NAMES.flatMap((pluginName) => {
        const pluginAllowPatterns = filterStandardPluginAllowPatterns(
          pluginName,
          allowPatterns,
        );
        return pluginAllowPatterns.length === 0
          ? []
          : [{ name: pluginName, allow: pluginAllowPatterns }];
      })
    : [];
  return [
    { name: "internal", allow: allowPatterns },
    ...standardPluginEntries,
    ...pluginNames.map((pluginName) => ({
      name: pluginName,
      allow: allowPatterns,
    })),
  ];
};

export const writeMuonConfig = async (
  directory: string,
  allowPatterns: string[],
  pluginPath: string,
  plugins: PluginConfigEntry[],
  browserConfig: BrowserShortcutConfig | undefined,
  browserPluginAllowPatterns: string[] | null,
  debuggerEnabled: boolean,
  networkAuthorizedOrigins: NetworkAuthorizedOriginConfig[] = [],
  browserAllowUnsafeJavaScriptParentAccess: string[] | null = null,
  browserInitialWindowState: BrowserInitialWindowState | undefined = undefined,
  assetFrom: string | undefined = undefined,
  assetSignature: string | undefined = undefined,
  assetSalt: string | undefined = undefined,
  browserBackgroundColor: string | undefined = undefined,
): Promise<string> => {
  const network: Record<string, unknown> = { allow: allowPatterns };
  if (networkAuthorizedOrigins.length > 0) {
    network.authorizedOrigin = networkAuthorizedOrigins;
  }
  const config: Record<string, unknown> = {
    network,
    plugin: {
      path: pluginPath,
      plugins,
    },
  };
  if (debuggerEnabled) {
    config.cdp = { enable: true, port: MUON_PORT };
  }
  const browser: Record<string, unknown> = {};
  if (browserConfig !== undefined) {
    const keybind: Record<string, string> = {};
    for (const key of [
      "devtools",
      "reload",
      "hardReload",
      "fullscreen",
      "zoomIn",
      "zoomOut",
      "resetZoom",
    ] as const) {
      const value = browserConfig[key];
      if (value !== undefined) {
        keybind[key] = value;
      }
    }
    if (Object.keys(keybind).length > 0) {
      browser.keybind = keybind;
    }
  }
  if (browserPluginAllowPatterns !== null) {
    browser.plugin = { allow: browserPluginAllowPatterns };
  }
  if (browserAllowUnsafeJavaScriptParentAccess !== null) {
    browser.allowUnsafeJavaScriptParentAccess =
      browserAllowUnsafeJavaScriptParentAccess;
  }
  if (browserInitialWindowState !== undefined) {
    browser.initialWindowState = browserInitialWindowState;
  }
  if (browserBackgroundColor !== undefined) {
    browser.backgroundColor = browserBackgroundColor;
  }
  if (Object.keys(browser).length > 0) {
    config.browser = browser;
  }
  if (assetFrom !== undefined) {
    const asset: Record<string, unknown> = { from: assetFrom };
    if (assetSignature !== undefined) {
      asset.signature = assetSignature;
    }
    if (assetSalt !== undefined) {
      asset.salt = assetSalt;
    }
    config.asset = asset;
  }
  await mkdir(directory, { recursive: true });
  const configPath = join(directory, "muon.json");
  await writeFile(configPath, `${JSON.stringify(config, null, 2)}\n`);
  return configPath;
};

export const startMuon = async (
  directory: string,
  pluginNames: string[],
  networkAllowPatterns: string[],
  pluginAllowPatterns: string[],
  waitForDebugPort: boolean,
  useValgrind: boolean,
  browserConfig: BrowserShortcutConfig | undefined,
  environment: NodeJS.ProcessEnv = {},
  configuredPluginNames: string[] = pluginNames,
  browserPluginAllowPatterns: string[] | null = null,
  networkAuthorizedOrigins: NetworkAuthorizedOriginConfig[] = [],
  browserAllowUnsafeJavaScriptParentAccess: string[] | null = null,
  includeStandardPlugins = true,
  browserInitialWindowState: BrowserInitialWindowState | undefined = undefined,
  assetFrom: string | undefined = undefined,
  assetSignature: string | undefined = undefined,
  assetSalt: string | undefined = undefined,
  browserBackgroundColor: string | undefined = undefined,
): Promise<RunningMuon> => {
  const executable = getMuonExecutable(directory);
  await requireFile(executable);
  const pluginConfig = createPluginConfigEntries(
    configuredPluginNames,
    pluginAllowPatterns,
    includeStandardPlugins,
  );
  const pluginDirectory = await createPluginDirectory(
    directory,
    pluginNames,
    includeStandardPlugins,
  );
  const configDirectory = join(directory, ".muon-test-config");
  const pluginPath = relative(configDirectory, pluginDirectory) || ".";
  const configPath = await writeMuonConfig(
    configDirectory,
    networkAllowPatterns,
    pluginPath,
    pluginConfig,
    browserConfig,
    browserPluginAllowPatterns,
    waitForDebugPort,
    networkAuthorizedOrigins,
    browserAllowUnsafeJavaScriptParentAccess,
    browserInitialWindowState,
    assetFrom,
    assetSignature,
    assetSalt,
    browserBackgroundColor,
  );
  const args = shouldForceX11Ozone
    ? [
        "-c",
        configPath,
        "--ozone-platform=x11",
        "--disable-gpu",
        "--disable-vulkan",
      ]
    : ["-c", configPath];
  const { command, commandArgs } = await createMuonProcessCommand(
    executable,
    args,
    useValgrind,
  );
  const child = spawn(command, commandArgs, {
    cwd: directory,
    detached: true,
    env: { ...process.env, ...environment },
    stdio: ["ignore", "pipe", "pipe"],
  });
  const running = {
    process: child,
    pluginDirectory,
    stderr: "",
    usesValgrind: useValgrind,
  };
  child.stderr?.setEncoding("utf8");
  child.stderr?.on("data", (chunk: string) => {
    running.stderr += chunk;
  });
  runningProcesses.push(running);
  if (waitForDebugPort) {
    try {
      await waitForCdp(cdpStartupTimeoutMs);
    } catch (error) {
      try {
        await stopMuon(running, undefined);
      } catch (stopError) {
        throw new Error(
          `${String(error)}\n${String(stopError)}\nMuon stderr:\n${
            running.stderr
          }`,
        );
      }
      throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
    }
  }
  return running;
};

export const startDebugMuon = async (
  pluginNames: string[],
  networkAllowPatterns = TEST_NETWORK_ALLOW_PATTERNS,
  environment: NodeJS.ProcessEnv = {},
  browserConfig: BrowserShortcutConfig | undefined = undefined,
  pluginAllowPatterns = TEST_PLUGIN_ALLOW_PATTERNS,
  configuredPluginNames: string[] = pluginNames,
  browserPluginAllowPatterns:
    | string[]
    | null = TEST_BROWSER_PLUGIN_ALLOW_PATTERNS,
  networkAuthorizedOrigins: NetworkAuthorizedOriginConfig[] = [],
  browserAllowUnsafeJavaScriptParentAccess: string[] | null = null,
  includeStandardPlugins = true,
  browserInitialWindowState: BrowserInitialWindowState | undefined = undefined,
  assetFrom: string | undefined = undefined,
  assetSignature: string | undefined = undefined,
  assetSalt: string | undefined = undefined,
  browserBackgroundColor: string | undefined = undefined,
): Promise<RunningMuon> =>
  await startMuon(
    DEBUG_MUON_DIRECTORY,
    pluginNames,
    networkAllowPatterns,
    pluginAllowPatterns,
    true,
    shouldUseValgrind,
    browserConfig,
    environment,
    configuredPluginNames,
    browserPluginAllowPatterns,
    networkAuthorizedOrigins,
    browserAllowUnsafeJavaScriptParentAccess,
    includeStandardPlugins,
    browserInitialWindowState,
    assetFrom,
    assetSignature,
    assetSalt,
    browserBackgroundColor,
  );

export const startReleaseMuon = async (): Promise<RunningMuon> =>
  await startMuon(
    RELEASE_MUON_DIRECTORY,
    [],
    ["asset://main/**"],
    [],
    false,
    false,
    undefined,
  );

export const startGestamentDebugMuon = async (
  browserAllowUnsafeJavaScriptParentAccess: string[] | null = null,
  assetRoot: string | undefined = undefined,
): Promise<RunningGestamentMuon> => {
  const executable = getMuonExecutable(DEBUG_MUON_DIRECTORY);
  await requireFile(executable);
  const pluginDirectory = await createPluginDirectory(DEBUG_MUON_DIRECTORY, []);
  const configDirectory = join(DEBUG_MUON_DIRECTORY, ".muon-test-config");
  const pluginPath = relative(configDirectory, pluginDirectory) || ".";
  const configPath = await writeMuonConfig(
    configDirectory,
    TEST_NETWORK_ALLOW_PATTERNS,
    pluginPath,
    createPluginConfigEntries([], TEST_PLUGIN_ALLOW_PATTERNS),
    undefined,
    TEST_BROWSER_PLUGIN_ALLOW_PATTERNS,
    true,
    [],
    browserAllowUnsafeJavaScriptParentAccess,
    undefined,
    assetRoot,
  );
  let app: GtkApp | undefined = undefined;
  const args = [
    "--password-store=basic",
    "--use-mock-keychain",
    ...(shouldForceX11Ozone
      ? ["--ozone-platform=x11", "--disable-gpu", "--disable-vulkan"]
      : []),
    "-c",
    configPath,
  ];
  try {
    app = await launchGtkApp(executable, args, {
      cwd: DEBUG_MUON_DIRECTORY,
      env: {
        ELECTRON_OZONE_PLATFORM_HINT: "x11",
        GIO_USE_VFS: "local",
        GNOME_ACCESSIBILITY: "1",
        GTK_MODULES: process.env.GTK_MODULES ?? "gail:atk-bridge",
        GTK_USE_PORTAL: "0",
        OZONE_PLATFORM: "x11",
      },
      outputBufferBytes: 8 * 1024 * 1024,
      timeoutMs: cdpStartupTimeoutMs,
    });
    await waitForCdp(cdpStartupTimeoutMs);
    const driver = await connectToMuonCdp({
      port: MUON_PORT,
      timeoutMs: cdpCommandTimeoutMs,
    });
    return { app, driver, pluginDirectory };
  } catch (error) {
    const currentApp = app;
    const output =
      currentApp === undefined
        ? undefined
        : await resolveOrUndefined(async () => await currentApp.output());
    await app?.release();
    await rm(pluginDirectory, { recursive: true, force: true });
    throw new Error(`${String(error)}\nMuon stderr:\n${output?.stderr ?? ""}`);
  }
};

export const stopGestamentMuon = async (
  running: RunningGestamentMuon,
): Promise<void> => {
  try {
    await running.driver.send("Browser.close", undefined);
  } catch {
    // The process may already be exiting.
  }
  running.driver.close();
  await running.app.release();
  await rm(running.pluginDirectory, { recursive: true, force: true });
};

export const waitForGestamentMuonExit = async (
  app: GtkApp,
  timeoutMs: number,
): Promise<Awaited<ReturnType<GtkApp["output"]>>> => {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    const output = await app.output();
    if (output.exitCode !== null || output.exitSignal !== null) {
      return output;
    }
    await wait(100);
  }
  const output = await app.output();
  throw new Error(
    `Timed out waiting for Gestament muon to exit. exitCode=${String(
      output.exitCode,
    )} exitSignal=${String(output.exitSignal)} stderr:\n${output.stderr}`,
  );
};

export const waitForMuonFsSelectFile = async (
  driver: CdpDriver,
  timeoutMs: number,
): Promise<void> => {
  const deadline = Date.now() + timeoutMs;
  let lastType = "";
  while (Date.now() < deadline) {
    try {
      lastType = await driver.evaluate<string>(
        "typeof window.muon?.fs?.dialogs?.selectFile",
      );
      if (lastType === "function") {
        return;
      }
    } catch (error) {
      lastType = String(error);
    }
    await wait(100);
  }
  throw new Error(
    `Timed out waiting for window.muon.fs.dialogs.selectFile. Last value: ${lastType}`,
  );
};

export const startNativeFileDialogProbe = async (
  driver: CdpDriver,
  options: NativeFileDialogProbeOptions,
): Promise<void> => {
  const response = await driver.send<RuntimeEvaluateResponse>(
    "Runtime.evaluate",
    {
      expression: `new Promise((resolve) => {
        const controller = new AbortController();
        window.__muonNativeDialogProbeController = controller;
        window.__muonNativeDialogProbe = (async () => {
          try {
            const value = await window.muon.fs.dialogs.selectFile({
              title: ${JSON.stringify(options.title)},
              defaultPath: ${JSON.stringify(options.defaultPath)},
              buttonLabel: ${JSON.stringify(options.buttonLabel)},
              ${
                options.modal === undefined
                  ? ""
                  : `modal: ${JSON.stringify(options.modal)},`
              }
              gtk: { localOnly: false },
              signal: controller.signal,
            });
            return { status: "fulfilled", value };
          } catch (error) {
            return {
              status: "rejected",
              name: String(error && error.name ? error.name : ""),
              message: String(error && error.message ? error.message : error),
            };
          }
        })();
        setTimeout(() => resolve("started"), 0);
      })`,
      returnByValue: true,
      awaitPromise: true,
    },
  );
  if (response.exceptionDetails !== undefined) {
    throw new Error("Failed to start Muon native file dialog probe");
  }
};

const isGestamentChildContainer = (
  element: GtkWidgetElement,
): element is GestamentChildContainer =>
  "childAt" in element && "getChildCount" in element;

const scanGestamentButton = async (
  element: GtkWidgetElement,
  buttonName: string,
  indexPath: string,
  depth: number,
  diagnostics: GestamentButtonDiagnostic[],
): Promise<GtkButtonElement | undefined> => {
  try {
    const info = await element.info();
    diagnostics.push({
      depth,
      error: undefined,
      indexPath,
      kind: element.kind,
      name: info.name,
      roleName: info.roleName,
    });
    if (element.kind === "button" && info.name === buttonName) {
      return element;
    }
  } catch (error) {
    diagnostics.push({
      depth,
      error: String(error),
      indexPath,
      kind: element.kind,
      name: undefined,
      roleName: undefined,
    });
    return undefined;
  }

  if (!isGestamentChildContainer(element)) {
    return undefined;
  }

  const childCount = await element.getChildCount();
  for (let index = 0; index < childCount; index += 1) {
    const child = await element.childAt(index);
    if (child === undefined) {
      continue;
    }
    const match = await scanGestamentButton(
      child,
      buttonName,
      `${indexPath}.${index}`,
      depth + 1,
      diagnostics,
    );
    if (match !== undefined) {
      return match;
    }
  }
  return undefined;
};

export const findGestamentButtonByName = async (
  root: GtkWidgetElement,
  buttonName: string,
  timeoutMs: number,
): Promise<GtkButtonElement> => {
  const deadline = Date.now() + timeoutMs;
  let lastDiagnostics: GestamentButtonDiagnostic[] = [];
  while (Date.now() < deadline) {
    const diagnostics: GestamentButtonDiagnostic[] = [];
    const match = await scanGestamentButton(
      root,
      buttonName,
      "root",
      0,
      diagnostics,
    );
    if (match !== undefined) {
      return match;
    }
    lastDiagnostics = diagnostics;
    await wait(100);
  }
  throw new Error(
    `Timed out waiting for Gestament button '${buttonName}'. Last diagnostics: ${JSON.stringify(
      lastDiagnostics,
      null,
      2,
    )}`,
  );
};

export const findGestamentAppButtonByName = async (
  app: GtkApp,
  buttonName: string,
  timeoutMs: number,
): Promise<GtkButtonElement> => {
  const deadline = Date.now() + timeoutMs;
  let lastWindowDiagnostics: GestamentWindowDiagnostic[] = [];
  let lastButtonDiagnostics: GestamentButtonDiagnostic[] = [];
  while (Date.now() < deadline) {
    const count = await app.getWindowCount();
    const windowDiagnostics: GestamentWindowDiagnostic[] = [];
    const buttonDiagnostics: GestamentButtonDiagnostic[] = [];
    for (let index = 0; index < count; index += 1) {
      const result = await readGestamentWindow(app, index);
      windowDiagnostics.push(result.diagnostic);
      if (result.window === undefined) {
        continue;
      }
      const match = await scanGestamentButton(
        result.window,
        buttonName,
        `window[${index}]`,
        0,
        buttonDiagnostics,
      );
      if (match !== undefined) {
        return match;
      }
    }
    lastWindowDiagnostics = windowDiagnostics;
    lastButtonDiagnostics = buttonDiagnostics;
    await wait(100);
  }
  throw new Error(
    `Timed out waiting for Gestament app button '${buttonName}'. Last window diagnostics: ${JSON.stringify(
      lastWindowDiagnostics,
      null,
      2,
    )}. Last button diagnostics: ${JSON.stringify(
      lastButtonDiagnostics,
      null,
      2,
    )}`,
  );
};

const createGestamentNativeDialogTextClickOptions =
  (): GtkWindowTextClickOptions => ({
    minConfidence: 20,
    pageSegmentationModes: ["sparseText", "singleBlock"],
    parameters: { debug_file: "/dev/null" },
    preprocess: { grayscale: true, scale: 2 },
  });

export const findGestamentNativeDialogButtonByLabel = async (
  window: GtkWindowElement,
  buttonName: string,
  timeoutMs: number,
): Promise<GestamentNativeDialogButton> => {
  const deadline = Date.now() + timeoutMs;
  let lastButtonDiagnostics: GestamentButtonDiagnostic[] = [];
  let lastTextError = "";
  while (Date.now() < deadline) {
    const buttonDiagnostics: GestamentButtonDiagnostic[] = [];
    const semanticButton = await scanGestamentButton(
      window,
      buttonName,
      "window",
      0,
      buttonDiagnostics,
    );
    if (semanticButton !== undefined) {
      return {
        click: async () => {
          await semanticButton.click();
        },
        detection: "semantic",
        label: buttonName,
      };
    }
    lastButtonDiagnostics = buttonDiagnostics;
    try {
      await resolveOrUndefined(async () => await window.activate());
      const textClickOptions = createGestamentNativeDialogTextClickOptions();
      const textMatch = await window.findText(buttonName, textClickOptions);
      if (textMatch !== undefined) {
        return {
          click: async () => {
            await window.clickText(
              buttonName,
              createGestamentNativeDialogTextClickOptions(),
            );
          },
          detection: "windowText",
          label: buttonName,
        };
      }
      lastTextError = "Gestament window text was not found";
    } catch (error) {
      lastTextError = String(error);
    }
    await wait(100);
  }
  throw new Error(
    `Timed out waiting for Gestament native dialog button '${buttonName}'. Last semantic diagnostics: ${JSON.stringify(
      lastButtonDiagnostics,
      null,
      2,
    )}. Last Gestament window text error: ${lastTextError}`,
  );
};

export const readGestamentWindow = async (
  app: GtkApp,
  index: number,
): Promise<{
  diagnostic: GestamentWindowDiagnostic;
  window: GtkWindowElement | undefined;
}> => {
  try {
    const window = await app.windowAt(index);
    if (window === undefined) {
      return {
        diagnostic: {
          error: "windowAt returned undefined",
          index,
          kind: undefined,
          matchedBy: undefined,
          name: undefined,
          rawIds: undefined,
          seenBy: undefined,
          title: undefined,
        },
        window: undefined,
      };
    }
    if (window.kind !== "window") {
      return {
        diagnostic: {
          error: `windowAt returned ${window.kind}, expected window`,
          index,
          kind: window.kind,
          matchedBy: undefined,
          name: undefined,
          rawIds: undefined,
          seenBy: undefined,
          title: undefined,
        },
        window: undefined,
      };
    }
    const info = await resolveOrUndefined(async () => await window.info());
    const x11Info = await resolveOrUndefined(
      async () => await window.x11Info(),
    );
    const debug = await resolveOrUndefined(
      async () => await window.debugDiagnostics(),
    );
    return {
      diagnostic: {
        error: undefined,
        index,
        kind: window.kind,
        matchedBy: debug?.matchedBy,
        name: info?.name,
        rawIds: debug?.rawIds,
        seenBy: debug?.seenBy,
        title: x11Info?.title,
      },
      window,
    };
  } catch (error) {
    return {
      diagnostic: {
        error: String(error),
        index,
        kind: undefined,
        matchedBy: undefined,
        name: undefined,
        rawIds: undefined,
        seenBy: undefined,
        title: undefined,
      },
      window: undefined,
    };
  }
};

export const findGestamentNativeWindow = async (
  app: GtkApp,
  title: string,
  timeoutMs: number,
): Promise<GestamentNativeWindowMatch> => {
  const deadline = Date.now() + timeoutMs;
  let lastDiagnostics: GestamentWindowDiagnostic[] = [];
  while (Date.now() < deadline) {
    const count = await app.getWindowCount();
    const diagnostics: GestamentWindowDiagnostic[] = [];
    for (let index = 0; index < count; index += 1) {
      const result = await readGestamentWindow(app, index);
      diagnostics.push(result.diagnostic);
      if (
        result.window !== undefined &&
        (result.diagnostic.name === title || result.diagnostic.title === title)
      ) {
        return { diagnostics, window: result.window };
      }
    }
    lastDiagnostics = diagnostics;
    await wait(100);
  }
  throw new Error(
    `Timed out waiting for Gestament native window '${title}'. Last diagnostics: ${JSON.stringify(
      lastDiagnostics,
      null,
      2,
    )}`,
  );
};

export const waitForGestamentNativeWindowClosed = async (
  app: GtkApp,
  title: string,
  timeoutMs: number,
): Promise<void> => {
  const deadline = Date.now() + timeoutMs;
  let lastDiagnostics: GestamentWindowDiagnostic[] = [];
  while (Date.now() < deadline) {
    const count = await app.getWindowCount();
    const diagnostics: GestamentWindowDiagnostic[] = [];
    let found = false;
    for (let index = 0; index < count; index += 1) {
      const result = await readGestamentWindow(app, index);
      diagnostics.push(result.diagnostic);
      if (
        result.window !== undefined &&
        (result.diagnostic.name === title || result.diagnostic.title === title)
      ) {
        found = true;
      }
    }
    if (!found) {
      return;
    }
    lastDiagnostics = diagnostics;
    await wait(100);
  }
  throw new Error(
    `Timed out waiting for Gestament native window '${title}' to close. Last diagnostics: ${JSON.stringify(
      lastDiagnostics,
      null,
      2,
    )}`,
  );
};

export const readNativeFileDialogProbeResult = async (
  driver: CdpDriver,
): Promise<NativeDialogProbeResult> =>
  await driver.evaluate<NativeDialogProbeResult>(`(async () => {
    const probe = window.__muonNativeDialogProbe;
    if (probe === undefined) {
      throw new Error("Native dialog probe was not started");
    }
    return await Promise.race([
      probe,
      new Promise((resolve) => setTimeout(
        () => resolve({ status: "timeout" }),
        5000,
      )),
    ]);
  })()`);

export const abortNativeFileDialogProbe = async (
  driver: CdpDriver,
): Promise<NativeDialogProbeResult> =>
  await driver.evaluate<NativeDialogProbeResult>(`(async () => {
    const controller = window.__muonNativeDialogProbeController;
    const probe = window.__muonNativeDialogProbe;
    if (controller === undefined || probe === undefined) {
      throw new Error("Native dialog probe was not started");
    }
    controller.abort(new Error("gestament native dialog test abort"));
    return await Promise.race([
      probe,
      new Promise((resolve) => setTimeout(
        () => resolve({ status: "timeout" }),
        5000,
      )),
    ]);
  })()`);

export const stopMuon = async (
  running: RunningMuon,
  driver: CdpDriver | undefined,
): Promise<void> => {
  const runningIndex = runningProcesses.indexOf(running);
  if (runningIndex >= 0) {
    runningProcesses.splice(runningIndex, 1);
  }

  if (driver !== undefined) {
    try {
      await driver.send("Browser.close", undefined);
    } catch {
      // The process may already be exiting.
    }
    driver.close();
  }

  let exited = false;
  if (driver !== undefined) {
    exited = await waitForProcessExitOrTimeout(running, processExitTimeoutMs);
  }

  if (!exited && running.process.pid !== undefined) {
    try {
      process.kill(-running.process.pid, "SIGTERM");
    } catch {
      // The process may already be closed.
    }
  }

  if (!exited) {
    exited = await waitForProcessExitOrTimeout(running, 3000);
  }
  await rm(running.pluginDirectory, { recursive: true, force: true });

  if (running.usesValgrind) {
    if (!exited) {
      throw new Error(`Timed out waiting for Valgrind exit\n${running.stderr}`);
    }
    if (running.process.exitCode !== 0 || running.process.signalCode !== null) {
      throw new Error(
        `Valgrind failed with exit=${String(
          running.process.exitCode,
        )} signal=${String(running.process.signalCode)}\n${running.stderr}`,
      );
    }
  }
};

export const withMuon = async (
  pluginNames: string[],
  run: (driver: CdpDriver, running: RunningMuon) => Promise<void>,
): Promise<void> => {
  const running = await startDebugMuon(pluginNames);
  let driver: CdpDriver | undefined = undefined;
  try {
    driver = await connectToMuonCdp({
      port: MUON_PORT,
      timeoutMs: cdpCommandTimeoutMs,
    });
    await driver.navigate(
      "data:text/html,<title>muon test</title>",
      cdpCommandTimeoutMs,
    );
    await run(driver, running);
  } catch (error) {
    throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
  } finally {
    await stopMuon(running, driver);
  }
};

export const withMuonEnvironment = async (
  pluginNames: string[],
  environment: NodeJS.ProcessEnv,
  run: (driver: CdpDriver, running: RunningMuon) => Promise<void>,
): Promise<void> => {
  const running = await startDebugMuon(
    pluginNames,
    TEST_NETWORK_ALLOW_PATTERNS,
    environment,
  );
  let driver: CdpDriver | undefined = undefined;
  try {
    driver = await connectToMuonCdp({
      port: MUON_PORT,
      timeoutMs: cdpCommandTimeoutMs,
    });
    await driver.navigate(
      "data:text/html,<title>muon test</title>",
      cdpCommandTimeoutMs,
    );
    await run(driver, running);
  } catch (error) {
    throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
  } finally {
    await stopMuon(running, driver);
  }
};

export const withMuonBrowserConfig = async (
  pluginNames: string[],
  browserConfig: BrowserShortcutConfig,
  run: (driver: CdpDriver, running: RunningMuon) => Promise<void>,
): Promise<void> => {
  const running = await startDebugMuon(
    pluginNames,
    TEST_NETWORK_ALLOW_PATTERNS,
    {},
    browserConfig,
  );
  let driver: CdpDriver | undefined = undefined;
  try {
    driver = await connectToMuonCdp({
      port: MUON_PORT,
      timeoutMs: cdpCommandTimeoutMs,
    });
    await driver.navigate(
      "data:text/html,<title>muon test</title>",
      cdpCommandTimeoutMs,
    );
    await run(driver, running);
  } catch (error) {
    throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
  } finally {
    await stopMuon(running, driver);
  }
};

export const withTrackedMuon = async (
  pluginNames: string[],
  run: (driver: CdpDriver, running: RunningMuon) => Promise<void>,
): Promise<void> => {
  const running = await startDebugMuon(pluginNames);
  let driver: CdpDriver | undefined = undefined;
  let caughtError: unknown = undefined;
  try {
    driver = await connectToMuonCdp({
      port: MUON_PORT,
      timeoutMs: cdpCommandTimeoutMs,
    });
    await driver.navigate(
      "data:text/html,<title>muon closure tracker</title>",
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

  if (caughtError !== undefined) {
    throw new Error(`${String(caughtError)}\nMuon stderr:\n${running.stderr}`);
  }
  expectFfiClosureTrackerBalanced(running.stderr);
};

export const expectDebugMuonStartupFailure = async (
  pluginNames: string[],
  expectedStderr: string,
): Promise<void> => {
  const running = await startMuon(
    DEBUG_MUON_DIRECTORY,
    pluginNames,
    TEST_NETWORK_ALLOW_PATTERNS,
    TEST_PLUGIN_ALLOW_PATTERNS,
    false,
    false,
    undefined,
  );
  try {
    await waitForProcessExit(running, processExitTimeoutMs);
    expect(running.process.signalCode).toBeNull();
    expect(running.process.exitCode).not.toBe(0);
    expect(running.stderr).toContain(expectedStderr);
  } finally {
    await stopMuon(running, undefined);
  }
};

export const evaluateRejection = async (
  driver: CdpDriver,
  expression: string,
): Promise<string> => {
  const response = await driver.send<RuntimeEvaluateResponse>(
    "Runtime.evaluate",
    {
      expression: `(async () => {
        try {
          await (${expression});
          return { ok: true, message: "" };
        } catch (error) {
          return {
            ok: false,
            message: String(error && error.message ? error.message : error),
          };
        }
      })()`,
      returnByValue: true,
      awaitPromise: true,
    },
  );
  if (response.exceptionDetails !== undefined) {
    throw new Error("Rejection evaluation failed");
  }
  const value = response.result?.value;
  if (
    typeof value !== "object" ||
    value === null ||
    !("ok" in value) ||
    !("message" in value)
  ) {
    throw new Error("Unexpected rejection evaluation result");
  }
  const result = value as { ok: boolean; message: string };
  expect(result.ok).toBe(false);
  return result.message;
};

export const runBuiltinFsRoundtrip = async (
  driver: CdpDriver,
  directory: string,
): Promise<void> => {
  const binaryPath = join(directory, "binary.bin");
  const textPath = join(directory, "text.txt");
  const unicodePath = join(directory, "ファイル-μon.txt");
  const concurrentDirectory = join(directory, "concurrent");
  await mkdir(concurrentDirectory);

  const values = await driver.evaluate<{
    binary: number[];
    text: string;
    unicode: string;
    emptySize: number;
    concurrent: string[];
  }>(`(async () => {
    const binaryPath = ${JSON.stringify(binaryPath)};
    const textPath = ${JSON.stringify(textPath)};
    const unicodePath = ${JSON.stringify(unicodePath)};
    const concurrentDirectory = ${JSON.stringify(concurrentDirectory)};
    const source = Uint8Array.from([99, 3, 1, 4, 1, 5, 88]);
    await window.muon.fs.writeFile(binaryPath, source.subarray(1, 6));
    const binary = Array.from(new Uint8Array(
      await window.muon.fs.readFile(binaryPath),
    ));
    await window.muon.fs.writeTextFile(textPath, "hello μon", "utf8");
    const text = await window.muon.fs.readTextFile(textPath, "utf-8");
    await window.muon.fs.writeTextFile(unicodePath, "unicode path", "utf8");
    const unicode = await window.muon.fs.readTextFile(unicodePath, "utf8");
    await window.muon.fs.writeFile(binaryPath, new Uint8Array());
    const emptySize = new Uint8Array(await window.muon.fs.readFile(binaryPath))
      .byteLength;
    const concurrent = await Promise.all(
      Array.from({ length: 12 }, async (_value, index) => {
        const path = concurrentDirectory + "/file-" + String(index) + ".txt";
        const value = "value-" + String(index);
        await window.muon.fs.writeTextFile(path, value, "utf8");
        return await window.muon.fs.readTextFile(path, "utf8");
      }),
    );
    return { binary, text, unicode, emptySize, concurrent };
  })()`);

  expect(values).toEqual({
    binary: [3, 1, 4, 1, 5],
    text: "hello μon",
    unicode: "unicode path",
    emptySize: 0,
    concurrent: Array.from({ length: 12 }, (_value, index) => `value-${index}`),
  });
  await expect(readFile(binaryPath)).resolves.toEqual(Buffer.alloc(0));
};

export const runBuiltinFsAdditionalOperations = async (
  driver: CdpDriver,
  directory: string,
): Promise<void> => {
  const rootPath = join(directory, "extra");
  const values = await driver.evaluate<{
    stat: {
      type: string;
      size: number;
      readonly: boolean;
      file: boolean;
      directory: boolean;
      symlink: boolean;
    };
    exists: { present: boolean; missing: boolean };
    access: { readWrite: boolean; execute: boolean; missing: boolean };
    readdir: { names: string[]; dirent: { name: string; directory: boolean } };
    copyRenameAppend: {
      copyText: string;
      noOverwriteRejected: boolean;
      truncated: string;
    };
    partial: number[];
    symlink: {
      lstatType: string;
      symbolicLink: boolean;
      readlinkBasename: string;
      text: string;
    };
    removed: {
      link: boolean;
      emptyDirectory: boolean;
      nested: boolean;
      missingForced: boolean;
    };
    watch: { filenames: string[]; afterCloseCount: number };
    invalid: {
      missingStat: string;
      badOptions: string;
      unlinkDirectory: string;
    };
  }>(`(async () => {
    const rootPath = ${JSON.stringify(rootPath)};
    const wait = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
    const basename = (path) => path.split("/").filter(Boolean).at(-1) ?? "";
    const rejection = async (operation) => {
      try {
        await operation();
        return "";
      } catch (error) {
        return String(error && error.message ? error.message : error);
      }
    };

    await window.muon.fs.mkdir(rootPath, { recursive: true });
    const filePath = rootPath + "/file.txt";
    await window.muon.fs.writeTextFile(filePath, "alpha", "utf8");
    const fileStat = await window.muon.fs.stat(filePath);
    const stat = {
      type: fileStat.type,
      size: fileStat.size,
      readonly: fileStat.readonly,
      file: fileStat.isFile(),
      directory: fileStat.isDirectory(),
      symlink: fileStat.isSymbolicLink(),
    };
    const exists = {
      present: await window.muon.fs.exists(filePath),
      missing: await window.muon.fs.exists(rootPath + "/missing.txt"),
    };
    const access = {
      readWrite: await window.muon.fs.access(filePath, {
        mode: ["read", "write"],
      }),
      execute: await window.muon.fs.access(filePath, { mode: ["execute"] }),
      missing: await window.muon.fs.access(rootPath + "/missing.txt"),
    };

    const nestedPath = rootPath + "/nested/child";
    await window.muon.fs.mkdir(nestedPath, { recursive: true });
    await window.muon.fs.writeTextFile(nestedPath + "/one.txt", "one", "utf8");
    const names = await window.muon.fs.readdir(nestedPath);
    const dirents = await window.muon.fs.readdir(rootPath, {
      withFileTypes: true,
    });
    const nestedDirent = dirents.find((entry) => entry.name === "nested");

    const copyPath = rootPath + "/copy.txt";
    await window.muon.fs.copyFile(filePath, copyPath);
    const copyText = await window.muon.fs.readTextFile(copyPath, "utf8");
    const noOverwriteRejected =
      (await rejection(() =>
        window.muon.fs.copyFile(filePath, copyPath, { overwrite: false }),
      )) !== "";
    const renamedPath = rootPath + "/renamed.txt";
    await window.muon.fs.rename(copyPath, renamedPath);
    await window.muon.fs.appendTextFile(renamedPath, "+text", "utf8");
    await window.muon.fs.appendFile(renamedPath, new Uint8Array([33]));
    await window.muon.fs.truncate(renamedPath, 5);
    const truncated = await window.muon.fs.readTextFile(renamedPath, "utf8");

    const partialPath = rootPath + "/partial.bin";
    await window.muon.fs.writeFile(
      partialPath,
      Uint8Array.from([0, 1, 2, 3, 4]),
    );
    const partialRead = Array.from(
      new Uint8Array(
        await window.muon.fs.readFile(partialPath, {
          position: 1,
          length: 3,
        }),
      ),
    );
    await window.muon.fs.writeFile(
      partialPath,
      Uint8Array.from([9, 9]),
      { position: 2 },
    );
    const partial = Array.from(
      new Uint8Array(await window.muon.fs.readFile(partialPath)),
    );

    const linkPath = rootPath + "/link.txt";
    await window.muon.fs.symlink(renamedPath, linkPath, "file");
    const linkStat = await window.muon.fs.lstat(linkPath);
    const readlinkTarget = await window.muon.fs.readlink(linkPath);
    const realpath = await window.muon.fs.realpath(linkPath);
    const linkedText = await window.muon.fs.readTextFile(realpath, "utf8");

    const watchPath = rootPath + "/watch";
    await window.muon.fs.mkdir(watchPath);
    const events = [];
    const watcher = await window.muon.fs.watch(watchPath, (event) => {
      events.push(event);
    });
    await window.muon.fs.writeTextFile(watchPath + "/watched.txt", "one", "utf8");
    for (let index = 0; index < 30; index += 1) {
      if (events.some((event) => event.filename === "watched.txt")) {
        break;
      }
      await wait(100);
    }
    await watcher.close();
    const afterCloseCount = events.length;
    await window.muon.fs.writeTextFile(watchPath + "/after.txt", "two", "utf8");
    await wait(250);

    await window.muon.fs.unlink(linkPath);
    const emptyDirectory = rootPath + "/empty";
    await window.muon.fs.mkdir(emptyDirectory);
    await window.muon.fs.rmdir(emptyDirectory);
    await window.muon.fs.rm(rootPath + "/nested", { recursive: true });
    await window.muon.fs.rm(rootPath + "/missing-rm", { force: true });

    const invalid = {
      missingStat: await rejection(() =>
        window.muon.fs.stat(rootPath + "/missing-stat"),
      ),
      badOptions: await rejection(() =>
        window.muon.fs.readFile(filePath, { position: 1.5 }),
      ),
      unlinkDirectory: await rejection(() =>
        window.muon.fs.unlink(watchPath),
      ),
    };

    return {
      stat,
      exists,
      access,
      readdir: {
        names,
        dirent: {
          name: nestedDirent ? nestedDirent.name : "",
          directory: nestedDirent ? nestedDirent.isDirectory() : false,
        },
      },
      copyRenameAppend: {
        copyText,
        noOverwriteRejected,
        truncated,
      },
      partial,
      symlink: {
        lstatType: linkStat.type,
        symbolicLink: linkStat.isSymbolicLink(),
        readlinkBasename: basename(readlinkTarget),
        text: linkedText,
      },
      removed: {
        link: await window.muon.fs.exists(linkPath),
        emptyDirectory: await window.muon.fs.exists(emptyDirectory),
        nested: await window.muon.fs.exists(rootPath + "/nested"),
        missingForced: await window.muon.fs.exists(rootPath + "/missing-rm"),
      },
      watch: {
        filenames: events.map((event) => event.filename),
        afterCloseCount,
      },
      invalid,
    };
  })()`);

  expect(values.stat).toMatchObject({
    type: "file",
    size: 5,
    readonly: false,
    file: true,
    directory: false,
    symlink: false,
  });
  expect(values.exists).toEqual({ present: true, missing: false });
  expect(values.access).toEqual({
    readWrite: true,
    execute: false,
    missing: false,
  });
  expect(values.readdir.names).toContain("one.txt");
  expect(values.readdir.dirent).toEqual({
    name: "nested",
    directory: true,
  });
  expect(values.copyRenameAppend).toEqual({
    copyText: "alpha",
    noOverwriteRejected: true,
    truncated: "alpha",
  });
  expect(values.partial).toEqual([0, 1, 9, 9, 4]);
  expect(values.symlink).toEqual({
    lstatType: "symlink",
    symbolicLink: true,
    readlinkBasename: "renamed.txt",
    text: "alpha",
  });
  expect(values.removed).toEqual({
    link: false,
    emptyDirectory: false,
    nested: false,
    missingForced: false,
  });
  expect(values.watch.filenames).toContain("watched.txt");
  expect(values.watch.afterCloseCount).toBe(values.watch.filenames.length);
  expect(values.invalid.missingStat).not.toBe("");
  expect(values.invalid.badOptions).toContain("options.position");
  expect(values.invalid.unlinkDirectory).not.toBe("");
};

export const runBuiltinFsFileUriOperations = async (
  driver: CdpDriver,
  directory: string,
): Promise<void> => {
  const rootPath = join(directory, "uri");
  const rootUri = pathToFileURL(`${rootPath}/`).href;
  const renamedPath = join(rootPath, "renamed.txt");

  const values = await driver.evaluate<{
    binary: number[];
    text: string;
    stat: {
      type: string;
      size: number;
      readonly: boolean;
      file: boolean;
      directory: boolean;
      symlink: boolean;
    };
    exists: { present: boolean; missing: boolean };
    access: { readWrite: boolean; execute: boolean; missing: boolean };
    readdir: { names: string[]; dirent: { name: string; directory: boolean } };
    copyRenameAppend: {
      copyText: string;
      noOverwriteRejected: boolean;
      truncated: string;
    };
    partial: number[];
    symlink: {
      lstatType: string;
      symbolicLink: boolean;
      readlinkTarget: string;
      realpath: string;
      text: string;
    };
    removed: {
      link: boolean;
      emptyDirectory: boolean;
      nested: boolean;
      missingForced: boolean;
    };
    watch: { filenames: string[]; afterCloseCount: number };
    invalid: {
      missingStat: string;
      unlinkDirectory: string;
    };
  }>(`(async () => {
    const rootUri = ${JSON.stringify(rootUri)};
    const renamedPath = ${JSON.stringify(renamedPath)};
    const uri = (path) => new URL(path, rootUri).href;
    const wait = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
    const rejection = async (operation) => {
      try {
        await operation();
        return "";
      } catch (error) {
        return String(error && error.message ? error.message : error);
      }
    };

    await window.muon.fs.mkdir(rootUri, { recursive: true });
    const binaryUri = uri("binary.bin");
    const source = Uint8Array.from([99, 3, 1, 4, 1, 5, 88]);
    await window.muon.fs.writeFile(binaryUri, source.subarray(1, 6));
    const binary = Array.from(new Uint8Array(
      await window.muon.fs.readFile(binaryUri),
    ));

    const fileUri = uri("file.txt");
    await window.muon.fs.writeTextFile(fileUri, "alpha", "utf8");
    const text = await window.muon.fs.readTextFile(fileUri, "utf-8");
    const fileStat = await window.muon.fs.stat(fileUri);
    const stat = {
      type: fileStat.type,
      size: fileStat.size,
      readonly: fileStat.readonly,
      file: fileStat.isFile(),
      directory: fileStat.isDirectory(),
      symlink: fileStat.isSymbolicLink(),
    };
    const exists = {
      present: await window.muon.fs.exists(fileUri),
      missing: await window.muon.fs.exists(uri("missing.txt")),
    };
    const access = {
      readWrite: await window.muon.fs.access(fileUri, {
        mode: ["read", "write"],
      }),
      execute: await window.muon.fs.access(fileUri, { mode: ["execute"] }),
      missing: await window.muon.fs.access(uri("missing.txt")),
    };

    const nestedUri = uri("nested/child/");
    await window.muon.fs.mkdir(nestedUri, { recursive: true });
    await window.muon.fs.writeTextFile(uri("nested/child/one.txt"), "one", "utf8");
    const names = await window.muon.fs.readdir(nestedUri);
    const dirents = await window.muon.fs.readdir(rootUri, {
      withFileTypes: true,
    });
    const nestedDirent = dirents.find((entry) => entry.name === "nested");

    const copyUri = uri("copy.txt");
    await window.muon.fs.copyFile(fileUri, copyUri);
    const copyText = await window.muon.fs.readTextFile(copyUri, "utf8");
    const noOverwriteRejected =
      (await rejection(() =>
        window.muon.fs.copyFile(fileUri, copyUri, { overwrite: false }),
      )) !== "";
    const renamedUri = uri("renamed.txt");
    await window.muon.fs.rename(copyUri, renamedUri);
    await window.muon.fs.appendTextFile(renamedUri, "+text", "utf8");
    await window.muon.fs.appendFile(renamedUri, new Uint8Array([33]));
    await window.muon.fs.truncate(renamedUri, 5);
    const truncated = await window.muon.fs.readTextFile(renamedUri, "utf8");

    const partialUri = uri("partial.bin");
    await window.muon.fs.writeFile(
      partialUri,
      Uint8Array.from([0, 1, 2, 3, 4]),
    );
    const partialRead = Array.from(
      new Uint8Array(
        await window.muon.fs.readFile(partialUri, {
          position: 1,
          length: 3,
        }),
      ),
    );
    await window.muon.fs.writeFile(
      partialUri,
      Uint8Array.from([9, 9]),
      { position: 2 },
    );
    const partial = Array.from(
      new Uint8Array(await window.muon.fs.readFile(partialUri)),
    );

    const linkUri = uri("link.txt");
    await window.muon.fs.symlink(renamedPath, linkUri, "file");
    const linkStat = await window.muon.fs.lstat(linkUri);
    const readlinkTarget = await window.muon.fs.readlink(linkUri);
    const realpath = await window.muon.fs.realpath(linkUri);
    const linkedText = await window.muon.fs.readTextFile(realpath, "utf8");

    const watchUri = uri("watch/");
    await window.muon.fs.mkdir(watchUri);
    const events = [];
    const watcher = await window.muon.fs.watch(watchUri, (event) => {
      events.push(event);
    });
    await window.muon.fs.writeTextFile(uri("watch/watched.txt"), "one", "utf8");
    for (let index = 0; index < 30; index += 1) {
      if (events.some((event) => event.filename === "watched.txt")) {
        break;
      }
      await wait(100);
    }
    await watcher.close();
    const afterCloseCount = events.length;
    await window.muon.fs.writeTextFile(uri("watch/after.txt"), "two", "utf8");
    await wait(250);

    await window.muon.fs.unlink(linkUri);
    const emptyDirectoryUri = uri("empty/");
    await window.muon.fs.mkdir(emptyDirectoryUri);
    await window.muon.fs.rmdir(emptyDirectoryUri);
    await window.muon.fs.rm(uri("nested/"), { recursive: true });
    await window.muon.fs.rm(uri("missing-rm"), { force: true });

    const invalid = {
      missingStat: await rejection(() =>
        window.muon.fs.stat(uri("missing-stat")),
      ),
      unlinkDirectory: await rejection(() =>
        window.muon.fs.unlink(watchUri),
      ),
    };

    return {
      binary,
      text,
      stat,
      exists,
      access,
      readdir: {
        names,
        dirent: {
          name: nestedDirent ? nestedDirent.name : "",
          directory: nestedDirent ? nestedDirent.isDirectory() : false,
        },
      },
      copyRenameAppend: {
        copyText,
        noOverwriteRejected,
        truncated,
      },
      partial,
      symlink: {
        lstatType: linkStat.type,
        symbolicLink: linkStat.isSymbolicLink(),
        readlinkTarget,
        realpath,
        text: linkedText,
      },
      removed: {
        link: await window.muon.fs.exists(linkUri),
        emptyDirectory: await window.muon.fs.exists(emptyDirectoryUri),
        nested: await window.muon.fs.exists(uri("nested/")),
        missingForced: await window.muon.fs.exists(uri("missing-rm")),
      },
      watch: {
        filenames: events.map((event) => event.filename),
        afterCloseCount,
      },
      invalid,
    };
  })()`);

  expect(values.binary).toEqual([3, 1, 4, 1, 5]);
  expect(values.text).toBe("alpha");
  expect(values.stat).toMatchObject({
    type: "file",
    size: 5,
    readonly: false,
    file: true,
    directory: false,
    symlink: false,
  });
  expect(values.exists).toEqual({ present: true, missing: false });
  expect(values.access).toEqual({
    readWrite: true,
    execute: false,
    missing: false,
  });
  expect(values.readdir.names).toContain("one.txt");
  expect(values.readdir.dirent).toEqual({
    name: "nested",
    directory: true,
  });
  expect(values.copyRenameAppend).toEqual({
    copyText: "alpha",
    noOverwriteRejected: true,
    truncated: "alpha",
  });
  expect(values.partial).toEqual([0, 1, 9, 9, 4]);
  expect(values.symlink).toEqual({
    lstatType: "symlink",
    symbolicLink: true,
    readlinkTarget: renamedPath,
    realpath: renamedPath,
    text: "alpha",
  });
  expect(values.removed).toEqual({
    link: false,
    emptyDirectory: false,
    nested: false,
    missingForced: false,
  });
  expect(values.watch.filenames).toContain("watched.txt");
  expect(values.watch.afterCloseCount).toBe(values.watch.filenames.length);
  expect(values.invalid.missingStat).not.toBe("");
  expect(values.invalid.unlinkDirectory).not.toBe("");
};

export const runBuiltinFsAbortScenarios = async (
  driver: CdpDriver,
  directory: string,
): Promise<void> => {
  const existingPath = join(directory, "abort-existing.txt");
  const cleanupPath = join(directory, "abort-cleanup.txt");
  await writeFile(existingPath, "abort target");

  const values = await driver.evaluate<{
    preAborted: { name: string; message: string }[];
    lateAbort: { message: string; addCount: number; removeCount: number };
    cleanup: { text: string; addCount: number; removeCount: number };
    invalidSignal: { name: string; message: string };
  }>(`(async () => {
    const existingPath = ${JSON.stringify(existingPath)};
    const cleanupPath = ${JSON.stringify(cleanupPath)};

    const rejection = async (operation) => {
      try {
        await operation();
        return { name: "", message: "" };
      } catch (error) {
        return {
          name: String(error && error.name ? error.name : ""),
          message: String(error && error.message ? error.message : error),
        };
      }
    };

    const preAborted = [];
    const preAbortOperations = [
      {
        name: "readFile",
        call: (signal) => window.muon.fs.readFile(
          existingPath + ".missing",
          { signal },
        ),
      },
      {
        name: "writeFile",
        call: (signal) => window.muon.fs.writeFile(
          existingPath,
          new Uint8Array([1, 2, 3]),
          { signal },
        ),
      },
      {
        name: "readTextFile",
        call: (signal) => window.muon.fs.readTextFile(
          existingPath,
          "utf8",
          { signal },
        ),
      },
      {
        name: "writeTextFile",
        call: (signal) => window.muon.fs.writeTextFile(
          existingPath,
          "aborted",
          "utf8",
          { signal },
        ),
      },
      {
        name: "stat",
        call: (signal) => window.muon.fs.stat(existingPath, { signal }),
      },
      {
        name: "lstat",
        call: (signal) => window.muon.fs.lstat(existingPath, { signal }),
      },
      {
        name: "exists",
        call: (signal) => window.muon.fs.exists(existingPath, { signal }),
      },
      {
        name: "access",
        call: (signal) => window.muon.fs.access(existingPath, { signal }),
      },
      {
        name: "readdir",
        call: (signal) => window.muon.fs.readdir(existingPath, { signal }),
      },
      {
        name: "mkdir",
        call: (signal) => window.muon.fs.mkdir(
          existingPath + "-dir",
          { signal },
        ),
      },
      {
        name: "rm",
        call: (signal) => window.muon.fs.rm(existingPath, { signal }),
      },
      {
        name: "unlink",
        call: (signal) => window.muon.fs.unlink(existingPath, { signal }),
      },
      {
        name: "rmdir",
        call: (signal) => window.muon.fs.rmdir(existingPath, { signal }),
      },
      {
        name: "rename",
        call: (signal) => window.muon.fs.rename(
          existingPath,
          existingPath + ".renamed",
          { signal },
        ),
      },
      {
        name: "copyFile",
        call: (signal) => window.muon.fs.copyFile(
          existingPath,
          existingPath + ".copy",
          { signal },
        ),
      },
      {
        name: "appendFile",
        call: (signal) => window.muon.fs.appendFile(
          existingPath,
          new Uint8Array([1]),
          { signal },
        ),
      },
      {
        name: "appendTextFile",
        call: (signal) => window.muon.fs.appendTextFile(
          existingPath,
          "aborted",
          "utf8",
          { signal },
        ),
      },
      {
        name: "truncate",
        call: (signal) => window.muon.fs.truncate(existingPath, 1, { signal }),
      },
      {
        name: "realpath",
        call: (signal) => window.muon.fs.realpath(existingPath, { signal }),
      },
      {
        name: "readlink",
        call: (signal) => window.muon.fs.readlink(existingPath, { signal }),
      },
      {
        name: "symlink",
        call: (signal) => window.muon.fs.symlink(
          existingPath,
          existingPath + ".link",
          "file",
          { signal },
        ),
      },
      {
        name: "watch",
        call: (signal) => window.muon.fs.watch(
          existingPath,
          () => undefined,
          { signal },
        ),
      },
      {
        name: "selectFile",
        call: (signal) => window.muon.fs.dialogs.selectFile({ signal }),
      },
      {
        name: "selectFiles",
        call: (signal) => window.muon.fs.dialogs.selectFiles({ signal }),
      },
      {
        name: "selectDirectory",
        call: (signal) => window.muon.fs.dialogs.selectDirectory({ signal }),
      },
      {
        name: "selectDirectories",
        call: (signal) =>
          window.muon.fs.dialogs.selectDirectories({ signal }),
      },
      {
        name: "selectSaveFile",
        call: (signal) => window.muon.fs.dialogs.selectSaveFile({ signal }),
      },
    ];
    for (const operation of preAbortOperations) {
      const controller = new AbortController();
      controller.abort(new Error("pre-aborted " + operation.name));
      const rejected = await rejection(() => operation.call(controller.signal));
      preAborted.push({
        name: operation.name,
        message: rejected.message,
      });
    }

    const createSignal = (reason, abortInMicrotask) => {
      const listeners = new Set();
      const signal = {
        aborted: false,
        reason: undefined,
        addCount: 0,
        removeCount: 0,
        addEventListener(type, listener) {
          if (type !== "abort") {
            return;
          }
          this.addCount += 1;
          listeners.add(listener);
          if (abortInMicrotask) {
            queueMicrotask(() => {
              if (this.aborted) {
                return;
              }
              this.aborted = true;
              this.reason = reason;
              for (const callback of Array.from(listeners)) {
                callback.call(this);
              }
            });
          }
        },
        removeEventListener(type, listener) {
          if (type !== "abort") {
            return;
          }
          this.removeCount += 1;
          listeners.delete(listener);
        },
      };
      return signal;
    };

    const lateSignal = createSignal(new Error("late abort"), true);
    const lateAbortResult = await rejection(() =>
      window.muon.fs.readFile(existingPath, { signal: lateSignal }),
    );
    const lateAbort = {
      message: lateAbortResult.message,
      addCount: lateSignal.addCount,
      removeCount: lateSignal.removeCount,
    };

    const cleanupSignal = createSignal(undefined, false);
    await window.muon.fs.writeTextFile(
      cleanupPath,
      "cleanup",
      "utf8",
      { signal: cleanupSignal },
    );
    const text = await window.muon.fs.readTextFile(
      cleanupPath,
      "utf8",
      { signal: cleanupSignal },
    );
    const cleanup = {
      text,
      addCount: cleanupSignal.addCount,
      removeCount: cleanupSignal.removeCount,
    };

    const invalidSignal = await rejection(() =>
      window.muon.fs.readFile(existingPath, { signal: { aborted: false } }),
    );

    return { preAborted, lateAbort, cleanup, invalidSignal };
  })()`);

  expect(values.preAborted).toEqual([
    { name: "readFile", message: "pre-aborted readFile" },
    { name: "writeFile", message: "pre-aborted writeFile" },
    { name: "readTextFile", message: "pre-aborted readTextFile" },
    { name: "writeTextFile", message: "pre-aborted writeTextFile" },
    { name: "stat", message: "pre-aborted stat" },
    { name: "lstat", message: "pre-aborted lstat" },
    { name: "exists", message: "pre-aborted exists" },
    { name: "access", message: "pre-aborted access" },
    { name: "readdir", message: "pre-aborted readdir" },
    { name: "mkdir", message: "pre-aborted mkdir" },
    { name: "rm", message: "pre-aborted rm" },
    { name: "unlink", message: "pre-aborted unlink" },
    { name: "rmdir", message: "pre-aborted rmdir" },
    { name: "rename", message: "pre-aborted rename" },
    { name: "copyFile", message: "pre-aborted copyFile" },
    { name: "appendFile", message: "pre-aborted appendFile" },
    { name: "appendTextFile", message: "pre-aborted appendTextFile" },
    { name: "truncate", message: "pre-aborted truncate" },
    { name: "realpath", message: "pre-aborted realpath" },
    { name: "readlink", message: "pre-aborted readlink" },
    { name: "symlink", message: "pre-aborted symlink" },
    { name: "watch", message: "pre-aborted watch" },
    { name: "selectFile", message: "pre-aborted selectFile" },
    { name: "selectFiles", message: "pre-aborted selectFiles" },
    { name: "selectDirectory", message: "pre-aborted selectDirectory" },
    { name: "selectDirectories", message: "pre-aborted selectDirectories" },
    { name: "selectSaveFile", message: "pre-aborted selectSaveFile" },
  ]);
  expect(values.lateAbort).toEqual({
    message: "late abort",
    addCount: 1,
    removeCount: 1,
  });
  expect(values.cleanup).toEqual({
    text: "cleanup",
    addCount: 2,
    removeCount: 2,
  });
  expect(values.invalidSignal).toEqual({
    name: "TypeError",
    message: "options.signal must be an AbortSignal",
  });
};

export const runBuiltinFsDialogValidation = async (
  driver: CdpDriver,
): Promise<void> => {
  const values = await driver.evaluate<{
    exposed: string[];
    legacyExposed: string[];
    invalid: Record<string, string>;
  }>(`(async () => {
    const rejection = async (operation) => {
      try {
        await operation();
        return "";
      } catch (error) {
        return String(error && error.message ? error.message : error);
      }
    };
    const exposed = [
      "selectFile",
      "selectFiles",
      "selectDirectory",
      "selectDirectories",
      "selectSaveFile",
    ].map((name) => typeof window.muon.fs.dialogs[name]);
    const legacyExposed = [
      "selectFile",
      "selectFiles",
      "selectDirectory",
      "selectDirectories",
      "selectSaveFile",
    ].map((name) => typeof window.muon.fs[name]);
    const invalid = {
      title: await rejection(() =>
        window.muon.fs.dialogs.selectFile({ title: 1 }),
      ),
      filters: await rejection(() =>
        window.muon.fs.dialogs.selectFile({ filters: "text" }),
      ),
      filterExtension: await rejection(() =>
        window.muon.fs.dialogs.selectFile({
          filters: [{ name: "Bad", extensions: ["bad/path"] }],
        }),
      ),
      modal: await rejection(() =>
        window.muon.fs.dialogs.selectFile({ modal: "yes" }),
      ),
      gtkFlag: await rejection(() =>
        window.muon.fs.dialogs.selectDirectory({ gtk: { localOnly: "yes" } }),
      ),
      gtkMimeTypes: await rejection(() =>
        window.muon.fs.dialogs.selectFile({
          gtk: { mimeTypes: ["text/plain", 1] },
        }),
      ),
      win32Flag: await rejection(() =>
        window.muon.fs.dialogs.selectFile({
          win32: { forceFilesystem: "yes" },
        }),
      ),
      saveName: await rejection(() =>
        window.muon.fs.dialogs.selectSaveFile({ defaultName: 1 }),
      ),
    };
    return { exposed, legacyExposed, invalid };
  })()`);

  expect(values.exposed).toEqual([
    "function",
    "function",
    "function",
    "function",
    "function",
  ]);
  expect(values.legacyExposed).toEqual([
    "undefined",
    "undefined",
    "undefined",
    "undefined",
    "undefined",
  ]);
  expect(values.invalid.title).toContain("options.title");
  expect(values.invalid.filters).toContain("options.filters");
  expect(values.invalid.filterExtension).toContain(
    "options.filters extensions entries",
  );
  expect(values.invalid.modal).toContain("options.modal");
  expect(values.invalid.gtkFlag).toContain("options.gtk.localOnly");
  expect(values.invalid.gtkMimeTypes).toContain("options.gtk.mimeTypes");
  expect(values.invalid.win32Flag).toContain("options.win32.forceFilesystem");
  expect(values.invalid.saveName).toContain("options.defaultName");
};

export const f12DevToolsShortcut: DevToolsShortcut = {
  name: "F12",
  config: "f12",
  event: {
    type: "rawKeyDown",
    windowsVirtualKeyCode: 123,
    nativeVirtualKeyCode: 123,
    key: "F12",
    code: "F12",
    modifiers: 0,
  },
};

export const ctrlShiftIDevToolsShortcut: DevToolsShortcut = {
  name: "Ctrl+Shift+I",
  config: "ctrl+shift+i",
  event: {
    type: "rawKeyDown",
    windowsVirtualKeyCode: 73,
    nativeVirtualKeyCode: 73,
    key: "I",
    code: "KeyI",
    modifiers: 10,
  },
};

export const shiftF9DevToolsShortcut: DevToolsShortcut = {
  name: "Shift+F9",
  config: "shift+f9",
  event: {
    type: "rawKeyDown",
    windowsVirtualKeyCode: 120,
    nativeVirtualKeyCode: 120,
    key: "F9",
    code: "F9",
    modifiers: 8,
  },
};

export const f5ReloadShortcut: KeyboardShortcutEvent = {
  type: "rawKeyDown",
  windowsVirtualKeyCode: 116,
  nativeVirtualKeyCode: 116,
  key: "F5",
  code: "F5",
  modifiers: 0,
};

export const ctrlRReloadShortcut: KeyboardShortcutEvent = {
  type: "rawKeyDown",
  windowsVirtualKeyCode: 82,
  nativeVirtualKeyCode: 82,
  key: "R",
  code: "KeyR",
  modifiers: 2,
};

export const ctrlShiftRShortcut: KeyboardShortcutEvent = {
  type: "rawKeyDown",
  windowsVirtualKeyCode: 82,
  nativeVirtualKeyCode: 82,
  key: "R",
  code: "KeyR",
  modifiers: 10,
};

export const f11FullscreenShortcut: KeyboardShortcutEvent = {
  type: "rawKeyDown",
  windowsVirtualKeyCode: 122,
  nativeVirtualKeyCode: 122,
  key: "F11",
  code: "F11",
  modifiers: 0,
};

export const ctrlPlusZoomShortcut: KeyboardShortcutEvent = {
  type: "rawKeyDown",
  windowsVirtualKeyCode: 187,
  nativeVirtualKeyCode: 187,
  key: "+",
  code: "Equal",
  modifiers: 10,
};

export const ctrlMinusZoomShortcut: KeyboardShortcutEvent = {
  type: "rawKeyDown",
  windowsVirtualKeyCode: 189,
  nativeVirtualKeyCode: 189,
  key: "-",
  code: "Minus",
  modifiers: 2,
};

export const ctrl0ZoomShortcut: KeyboardShortcutEvent = {
  type: "rawKeyDown",
  windowsVirtualKeyCode: 48,
  nativeVirtualKeyCode: 48,
  key: "0",
  code: "Digit0",
  modifiers: 2,
};

export const configuredDevToolsShortcuts = [
  f12DevToolsShortcut,
  ctrlShiftIDevToolsShortcut,
  shiftF9DevToolsShortcut,
];

afterEach(async () => {
  const running = runningProcesses.splice(0);
  for (const processInfo of running) {
    await stopMuon(processInfo, undefined);
  }
});

export const describeMuonPluginBridge = (
  name: string,
  factory: () => void,
): void => {
  (isWindows ? describe.skip : describe)(name, factory);
};
