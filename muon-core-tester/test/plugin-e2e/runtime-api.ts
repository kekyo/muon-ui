// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import { createHash } from "node:crypto";
import {
  mkdtemp as nodeMkdtemp,
  readFile as nodeReadFile,
  rm as nodeRm,
  writeFile as nodeWriteFile,
} from "node:fs/promises";
import { tmpdir as nodeTmpdir } from "node:os";
import { join as nodeJoin } from "node:path";
import { PNG } from "pngjs";
import { expect, it } from "vitest";

import {
  MUON_PORT,
  MUON_APP_URL,
  DEBUG_MUON_DIRECTORY,
  PLUGIN_SUFFIX,
  TEST_BROWSER_PLUGIN_ALLOW_PATTERNS,
  TEST_NETWORK_ALLOW_PATTERNS,
  TEST_PLUGIN_DIRECTORY,
  TEST_PLUGIN_ALLOW_PATTERNS,
  access,
  browserFunctionNames,
  cdpCommandTimeoutMs,
  cdpStartupTimeoutMs,
  configuredDevToolsShortcuts,
  connectToMuonCdp,
  constants,
  createBrowserShortcutConfig,
  ctrl0ZoomShortcut,
  ctrlF12RecycleShortcut,
  ctrlMinusZoomShortcut,
  ctrlPlusZoomShortcut,
  ctrlShiftF10RecycleShortcut,
  ctrlRReloadShortcut,
  ctrlShiftIDevToolsShortcut,
  ctrlShiftRShortcut,
  describeMuonPluginBridge,
  dispatchDevToolsShortcut,
  dispatchKeyboardShortcut,
  evaluateRejection,
  expectDebugMuonStartupFailure,
  expectNoDevTools,
  expectNoPageLoad,
  expectProcessExitCode,
  execFileAsync,
  f11FullscreenShortcut,
  f12DevToolsShortcut,
  f5ReloadShortcut,
  getCurrentTargetIds,
  getOuterSize,
  getWindowBounds,
  join,
  listCdpTargets,
  listProcessGroupCommandLines,
  mkdir,
  mkdtemp,
  openPopupTarget,
  pathToFileUrlHref,
  processExitTimeoutMs,
  readFile,
  rm,
  runBuiltinFsAbortScenarios,
  runBuiltinFsAdditionalOperations,
  runBuiltinFsDialogValidation,
  runBuiltinFsFileUriOperations,
  runBuiltinFsRoundtrip,
  runFunctionWrapperAbortLeaseScenarios,
  sendNativeKeyboardShortcut,
  shiftF9DevToolsShortcut,
  shouldUseValgrind,
  startDebugMuon,
  startDebugMuonLauncher,
  startMuon,
  startReleaseMuon,
  stopMuon,
  targetTimeoutMs,
  tmpdir,
  delay,
  waitForCdp,
  waitForDocumentTitle,
  waitForInnerWidth,
  waitForMuonStderr,
  waitForDevToolsTarget,
  waitForNativeActiveWindowTitle,
  waitForNativeWindowTitle,
  waitForNativeWindowTitleAbsent,
  waitForNativeWindowStatesAbsent,
  waitForNativeWindowStates,
  waitForOuterSizeChange,
  waitForWindowBounds,
  waitForProcessExit,
  waitForProcessExitOrTimeout,
  waitForTargetClosed,
  withMuon,
  withMuonBrowserConfig,
  withMuonEnvironment,
  writeFile,
} from "./shared.js";
import {
  getWindowsRemoteContext,
  isWindowsRemoteE2e,
} from "./windows-context.js";
import type {
  BrowserInitialWindowState,
  BrowserWindowBounds,
  CdpDriver,
  CdpTarget,
  KeyboardShortcutEvent,
} from "./shared.js";

interface ExecutorSpawnOptions {
  args: string[];
  command: string;
}

const isLocalLinuxE2e = process.platform === "linux" && !isWindowsRemoteE2e();
const windowsIt = isWindowsRemoteE2e() ? it : it.skip;

const windowsRemoteTarget = (): string | undefined =>
  getWindowsRemoteContext()?.runtime.target;

const expectedRuntimeTarget = (): string =>
  windowsRemoteTarget() ?? "linux-amd64";

const expectedCefTarget = (): string => {
  const target = expectedRuntimeTarget();
  if (target === "linux-amd64") {
    return "linux64";
  }
  if (target === "linux-armhf") {
    return "linuxarm";
  }
  if (target === "linux-arm64") {
    return "linuxarm64";
  }
  if (target === "windows-i686") {
    return "windows32";
  }
  if (target === "windows-amd64") {
    return "windows64";
  }
  return target;
};

const calculatePluginSignature = async (
  pluginName: string,
  salt: string,
): Promise<string> => {
  const pluginPath = join(
    TEST_PLUGIN_DIRECTORY,
    `${pluginName}${PLUGIN_SUFFIX}`,
  );
  return createHash("sha256")
    .update(await readFile(pluginPath))
    .update(Buffer.from(salt, "hex"))
    .digest("hex");
};

const expectedRuntimeExecutableName = (): string =>
  isWindowsRemoteE2e()
    ? "muon-core.exe"
    : process.platform === "win32"
      ? "muon-core.exe"
      : "muon-core";

const expectedCefArtifactNamePart = (): string =>
  `_${expectedCefTarget()}_minimal.tar.bz2`;

const isCdpWebSocketFailure = (error: unknown): boolean =>
  error instanceof Error &&
  /CDP WebSocket (?:(?:connection )?failed|closed|is closed)/u.test(
    error.message,
  );

const getMainPageTarget = async (): Promise<CdpTarget> => {
  const target = (
    await listCdpTargets({
      port: MUON_PORT,
      timeoutMs: cdpCommandTimeoutMs,
    })
  ).find(
    (candidate) =>
      candidate.type === "page" &&
      candidate.url === MUON_APP_URL &&
      candidate.webSocketDebuggerUrl !== undefined,
  );
  if (target === undefined) {
    throw new Error("Main page CDP target was not found");
  }
  return target;
};

const evaluateWithWindowsCdpReconnect = async <T>(
  driver: CdpDriver,
  targetId: string,
  expression: string,
): Promise<{ driver: CdpDriver; value: T }> => {
  let currentDriver: CdpDriver | undefined = driver;
  let lastError: unknown = undefined;
  const deadline = Date.now() + cdpCommandTimeoutMs;

  while (Date.now() < deadline) {
    if (currentDriver !== undefined) {
      try {
        return {
          driver: currentDriver,
          value: await currentDriver.evaluate<T>(expression),
        };
      } catch (error) {
        if (!isWindowsRemoteE2e() || !isCdpWebSocketFailure(error)) {
          throw error;
        }
        lastError = error;
        currentDriver.close();
        currentDriver = undefined;
      }
    }

    try {
      currentDriver = await connectToMuonCdp({
        port: MUON_PORT,
        targetId,
        timeoutMs: Math.max(1, Math.min(5000, deadline - Date.now())),
      });
    } catch (error) {
      lastError = error;
      await delay(100);
    }
  }

  throw new Error(
    `Timed out evaluating Windows remote CDP target '${targetId}': ${String(
      lastError,
    )}`,
  );
};

const readMainDocumentTitle = async (
  driver: CdpDriver,
  mainTargetId: string,
): Promise<{ driver: CdpDriver; title: string }> => {
  const result = await evaluateWithWindowsCdpReconnect<string>(
    driver,
    mainTargetId,
    "document.title",
  );
  return { driver: result.driver, title: result.value };
};

const requestBrowserClose = async (
  driver: CdpDriver,
): Promise<CdpDriver | undefined> => {
  try {
    const closeResult = await driver.evaluate<string>(
      `window.muon.browser.close(); "requested"`,
    );
    expect(closeResult).toBe("requested");
    return driver;
  } catch (error) {
    if (!isWindowsRemoteE2e() || !isCdpWebSocketFailure(error)) {
      throw error;
    }
    driver.close();
    return undefined;
  }
};

const createExecutorOkSpawnOptions = (): ExecutorSpawnOptions =>
  isWindowsRemoteE2e()
    ? {
        args: [
          "-NoProfile",
          "-ExecutionPolicy",
          "Bypass",
          "-Command",
          "[Console]::Out.Write('ok')",
        ],
        command: "powershell.exe",
      }
    : {
        args: ["-e", "process.stdout.write('ok')"],
        command: process.execPath,
      };

const createExecutorStdinSpawnOptions = (): ExecutorSpawnOptions => {
  if (isWindowsRemoteE2e()) {
    return {
      args: [
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-Command",
        "$inputText = [Console]::In.ReadToEnd(); [Console]::Out.Write('stdout:' + $inputText); [Console]::Error.Write('stderr:ok'); exit 7",
      ],
      command: "powershell.exe",
    };
  }

  const childScript = `
    let input = "";
    process.stdin.setEncoding("utf8");
    process.stdin.on("data", (chunk) => {
      input += chunk;
    });
    process.stdin.on("end", () => {
      process.stdout.write("stdout:" + input);
      process.stderr.write("stderr:ok");
      process.exitCode = 7;
    });
  `;
  return {
    args: ["-e", childScript],
    command: process.execPath,
  };
};

const createExecutorStreamingSpawnOptions = (): ExecutorSpawnOptions => {
  if (isWindowsRemoteE2e()) {
    return {
      args: [
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-Command",
        "$stdout = [Console]::OpenStandardOutput(); $stderr = [Console]::OpenStandardError(); Start-Sleep -Milliseconds 100; $stdout.Write([byte[]](111,49), 0, 2); $stdout.Flush(); Start-Sleep -Milliseconds 100; $stderr.Write([byte[]](101,49), 0, 2); $stderr.Flush(); $inputStream = [Console]::OpenStandardInput(); $buffer = New-Object byte[] 4096; $memory = New-Object System.IO.MemoryStream; while (($read = $inputStream.Read($buffer, 0, $buffer.Length)) -gt 0) { $memory.Write($buffer, 0, $read) }; $prefix = [Text.Encoding]::UTF8.GetBytes('stdin:'); $stdout.Write($prefix, 0, $prefix.Length); $bytes = $memory.ToArray(); $stdout.Write($bytes, 0, $bytes.Length); $done = [Text.Encoding]::UTF8.GetBytes('done'); $stderr.Write($done, 0, $done.Length); exit 7",
      ],
      command: "powershell.exe",
    };
  }

  const childScript = `
    const input = [];
    setTimeout(() => process.stdout.write(Buffer.from([111, 49])), 100);
    setTimeout(() => process.stderr.write(Buffer.from([101, 49])), 200);
    process.stdin.on("data", (chunk) => {
      input.push(Buffer.from(chunk));
    });
    process.stdin.on("end", () => {
      process.stdout.write(Buffer.concat([Buffer.from("stdin:"), ...input]));
      process.stderr.write("done");
      process.exitCode = 7;
    });
  `;
  return {
    args: ["-e", childScript],
    command: process.execPath,
  };
};

const createExecutorLongRunningSpawnOptions = (
  marker: string,
): ExecutorSpawnOptions => {
  if (isWindowsRemoteE2e()) {
    return {
      args: [
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-Command",
        `$marker = ${JSON.stringify(marker)}; Start-Sleep -Seconds 30`,
      ],
      command: "powershell.exe",
    };
  }

  return {
    args: [
      "-e",
      `const marker = ${JSON.stringify(marker)}; setInterval(() => {}, 1000);`,
    ],
    command: process.execPath,
  };
};

interface WindowsCommandResult {
  exitCode: number | null;
  stderr: string;
  stdout: string;
}

const windowsPowerShellPath = String.raw`C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe`;
const windowsTrayCallbackMessage = 0x8000 + 0x4d3;
const windowsTrayPrimaryActivateMessage = 0x0202;
const windowsTraySecondaryActivateMessage = 0x0208;
const windowsTrayContextMenuMessage = 0x007b;
let windowsTrayCallbackSequence = 0;

const createTrayIconPng = (
  red: number = 24,
  green: number = 120,
  blue: number = 180,
): Buffer => {
  const png = new PNG({ width: 16, height: 16 });
  for (let index = 0; index < png.data.length; index += 4) {
    png.data[index] = red;
    png.data[index + 1] = green;
    png.data[index + 2] = blue;
    png.data[index + 3] = 255;
  }
  return PNG.sync.write(png);
};

const trayIconPng = createTrayIconPng();
const trayAltIconPng = createTrayIconPng(220, 80, 32);
const titleIconPng = createTrayIconPng(80, 180, 80);

const createTrayAssetRoot = async (directory: string): Promise<string> => {
  const assetRoot = join(directory, "tray-assets");
  const mainRoot = join(assetRoot, "main");
  const iconsRoot = join(mainRoot, "icons");
  await mkdir(iconsRoot, { recursive: true });
  await writeFile(
    join(mainRoot, "index.html"),
    "<!doctype html><title>muon hidden tray</title>",
  );
  await writeFile(join(iconsRoot, "tray.png"), trayIconPng);
  await writeFile(join(iconsRoot, "tray-alt.png"), trayAltIconPng);
  await writeFile(join(iconsRoot, "title.png"), titleIconPng);
  return assetRoot;
};

const runWindowsPowerShell = async (
  remoteDirectory: string,
  label: string,
  script: string,
  timeoutMs: number,
): Promise<WindowsCommandResult> => {
  const context = getWindowsRemoteContext();
  if (context === undefined) {
    throw new Error("Windows remote e2e context is not configured");
  }
  const scriptPath = join(remoteDirectory, `${label}.ps1`);
  await writeFile(
    scriptPath,
    Buffer.concat([Buffer.from([0xef, 0xbb, 0xbf]), Buffer.from(script)]),
  );
  const processInfo = await context.agent.processes.launchManaged({
    arguments: [
      "-NoProfile",
      "-Sta",
      "-ExecutionPolicy",
      "Bypass",
      "-File",
      scriptPath,
    ],
    captureStderr: true,
    captureStdout: true,
    createNoWindow: true,
    path: windowsPowerShellPath,
    workingDirectory: remoteDirectory,
  });
  try {
    const snapshot = await processInfo.waitForExit({
      intervalMs: 250,
      timeoutMs,
    });
    return {
      exitCode: snapshot.root.exitCode,
      stderr: await processInfo.stderrText(),
      stdout: await processInfo.stdoutText(),
    };
  } finally {
    await processInfo.releaseAsync();
  }
};

const sendWindowsTrayCallback = async (
  remoteDirectory: string,
  notifyId: number,
  eventMessage: number,
  selectFirstMenuItem: boolean = false,
): Promise<void> => {
  const keysBlock = selectFirstMenuItem
    ? `
Start-Sleep -Milliseconds 300
[MuonTrayTestNative]::SetCursorPos(190, 172) | Out-Null
Start-Sleep -Milliseconds 100
[MuonTrayTestNative]::mouse_event([UInt32]0x0002, [UInt32]0, [UInt32]0, [UInt32]0, [UIntPtr]::Zero)
Start-Sleep -Milliseconds 50
[MuonTrayTestNative]::mouse_event([UInt32]0x0004, [UInt32]0, [UInt32]0, [UInt32]0, [UIntPtr]::Zero)
Start-Sleep -Milliseconds 300
`
    : "";
  const script = `
$ErrorActionPreference = 'Stop'
Add-Type @"
using System;
using System.Runtime.InteropServices;

public static class MuonTrayTestNative {
  public static readonly IntPtr HWND_MESSAGE = new IntPtr(-3);

  [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
  public static extern IntPtr FindWindowExW(IntPtr hwndParent, IntPtr hwndChildAfter, string lpszClass, string lpszWindow);

  [DllImport("user32.dll", SetLastError = true)]
  public static extern bool PostMessageW(IntPtr hWnd, UInt32 msg, IntPtr wParam, IntPtr lParam);

  [DllImport("user32.dll", SetLastError = true)]
  public static extern bool SetCursorPos(int x, int y);

  [DllImport("user32.dll", SetLastError = true)]
  public static extern void mouse_event(UInt32 dwFlags, UInt32 dx, UInt32 dy, UInt32 dwData, UIntPtr dwExtraInfo);
}
"@

$deadline = [DateTime]::UtcNow.AddMilliseconds(${String(cdpCommandTimeoutMs)})
$hwnd = [IntPtr]::Zero
while ([DateTime]::UtcNow -lt $deadline) {
  $hwnd = [MuonTrayTestNative]::FindWindowExW([MuonTrayTestNative]::HWND_MESSAGE, [IntPtr]::Zero, 'MuonBrowserTrayMessageWindow', $null)
  if ($hwnd -ne [IntPtr]::Zero) {
    break
  }
  Start-Sleep -Milliseconds 50
}
if ($hwnd -eq [IntPtr]::Zero) {
  throw 'Timed out waiting for MuonBrowserTrayMessageWindow.'
}

[MuonTrayTestNative]::SetCursorPos(160, 160) | Out-Null
if (-not [MuonTrayTestNative]::PostMessageW($hwnd, [UInt32]${String(windowsTrayCallbackMessage)}, [IntPtr]${String(notifyId)}, [IntPtr]${String(eventMessage)})) {
  $lastError = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
  throw "PostMessageW failed: $lastError"
}
${keysBlock}
Write-Output 'ok'
`;
  const result = await runWindowsPowerShell(
    remoteDirectory,
    `windows-tray-callback-${String((windowsTrayCallbackSequence += 1)).padStart(3, "0")}-${String(notifyId)}-${String(eventMessage)}`,
    script,
    cdpCommandTimeoutMs,
  );
  expect(
    result.stdout.trim(),
    `Windows tray callback failed.\nstdout:\n${result.stdout}\nstderr:\n${result.stderr}`,
  ).toBe("ok");
};

const withMuonInitialWindowState = async (
  initialWindowState: BrowserInitialWindowState,
  run: (driver: CdpDriver) => Promise<void>,
  assetSourcePath: string | undefined = undefined,
): Promise<void> => {
  const running = await startDebugMuon(
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
    initialWindowState,
    assetSourcePath,
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
    await run(driver);
  } catch (error) {
    throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
  } finally {
    await stopMuon(running, driver);
  }
};

const findStatusNotifierItemBusName = async (): Promise<string> => {
  const deadline = Date.now() + cdpCommandTimeoutMs;
  let lastError: unknown = undefined;
  while (Date.now() < deadline) {
    try {
      const { stdout } = await execFileAsync(
        "gdbus",
        [
          "call",
          "--session",
          "--dest",
          "org.freedesktop.DBus",
          "--object-path",
          "/org/freedesktop/DBus",
          "--method",
          "org.freedesktop.DBus.ListNames",
        ],
        { timeout: cdpCommandTimeoutMs },
      );
      const names = Array.from(
        String(stdout).matchAll(/'([^']+)'/gu),
        (match) => match[1],
      ).filter((name): name is string => name !== undefined);
      const busName = names.find((name) =>
        name.startsWith("org.freedesktop.StatusNotifierItem-"),
      );
      if (busName !== undefined) {
        return busName;
      }
    } catch (error) {
      lastError = error;
    }
    await delay(50);
  }
  throw new Error(
    `Timed out waiting for StatusNotifierItem bus name: ${String(lastError)}`,
  );
};

const callTrayDbusMethod = async (
  busName: string,
  objectPath: string,
  method: string,
  args: string[],
): Promise<void> => {
  await execFileAsync(
    "gdbus",
    [
      "call",
      "--session",
      "--dest",
      busName,
      "--object-path",
      objectPath,
      "--method",
      method,
      ...args,
    ],
    { timeout: cdpCommandTimeoutMs },
  );
};

const waitForBrowserTrayEvent = async (
  driver: CdpDriver,
  type: string,
  trayId: string | undefined = undefined,
): Promise<Record<string, unknown>> => {
  const deadline = Date.now() + cdpCommandTimeoutMs;
  while (Date.now() < deadline) {
    const event = await driver.evaluate<Record<string, unknown> | null>(
      `((globalThis.__muonHiddenTrayEvents ?? []).find((candidate) => candidate.type === ${JSON.stringify(type)} && (${trayId === undefined ? "true" : `candidate.trayId === ${JSON.stringify(trayId)}`})) ?? null)`,
    );
    if (event !== null) {
      return event;
    }
    await delay(50);
  }
  throw new Error(`Timed out waiting for tray event: ${type}`);
};

const waitForRecycledMuon = async (
  previousProcessId: number,
): Promise<{ driver: CdpDriver; processId: number }> => {
  const deadline =
    Date.now() + Math.max(processExitTimeoutMs, cdpStartupTimeoutMs);
  let lastError: unknown = undefined;
  while (Date.now() < deadline) {
    try {
      await waitForCdp(1000);
      const driver = await connectToMuonCdp({
        port: MUON_PORT,
        timeoutMs: cdpCommandTimeoutMs,
      });
      try {
        const processId = await driver.evaluate<number>(
          "window.muon.environments.getProcessId()",
        );
        if (processId !== previousProcessId) {
          return { driver, processId };
        }
      } catch (error) {
        lastError = error;
      }
      driver.close();
    } catch (error) {
      lastError = error;
    }
    await delay(200);
  }
  throw new Error(`Timed out waiting for recycled muon: ${String(lastError)}`);
};

const dispatchRecycleKeyboardShortcut = async (
  driver: CdpDriver,
  event: KeyboardShortcutEvent,
): Promise<CdpDriver | undefined> => {
  try {
    await dispatchKeyboardShortcut(driver, event);
    return driver;
  } catch (error) {
    if (!isWindowsRemoteE2e() || !isCdpWebSocketFailure(error)) {
      throw error;
    }
    driver.close();
    return undefined;
  }
};

const waitForTextFileContent = async (
  path: string,
  predicate: (content: string) => boolean,
  description: string,
): Promise<string> => {
  const deadline = Date.now() + targetTimeoutMs;
  let lastContent: string | undefined = undefined;
  let lastError: unknown = undefined;
  while (Date.now() < deadline) {
    try {
      lastContent = await readFile(path, "utf8");
      if (predicate(lastContent)) {
        return lastContent;
      }
    } catch (error) {
      lastError = error;
    }
    await delay(100);
  }
  throw new Error(
    `Timed out waiting for ${description}: ${lastContent ?? String(lastError)}`,
  );
};

const expectWindowBoundsShape = (bounds: BrowserWindowBounds): void => {
  expect(Number.isSafeInteger(bounds.x)).toBe(true);
  expect(Number.isSafeInteger(bounds.y)).toBe(true);
  expect(Number.isSafeInteger(bounds.width)).toBe(true);
  expect(Number.isSafeInteger(bounds.height)).toBe(true);
  expect(bounds.width).toBeGreaterThan(0);
  expect(bounds.height).toBeGreaterThan(0);
};

const createNativeShortcutDragPageUrl = (title: string): string =>
  `data:text/html;charset=utf-8,${encodeURIComponent(
    `<!doctype html>
<title>${title}</title>
<style>
html,
body {
  width: 100%;
  height: 100%;
  margin: 0;
  -webkit-app-region: drag;
  background: #d7ebe5;
}
</style>
<main>native shortcut drag region</main>`,
  )}`;

const adhocLibrarySource = `#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static char marker_path[4096];
static int marker_enabled = 0;

static void write_marker(const char* label) {
  if (!marker_enabled) {
    return;
  }
  FILE* file = fopen(marker_path, "a");
  if (file == NULL) {
    return;
  }
  fprintf(file, "%s\\n", label);
  fclose(file);
}

__attribute__((destructor)) static void muon_adhoc_unload_marker(void) {
  write_marker("unloaded");
}

int32_t muon_adhoc_set_marker_path(const char* path) {
  size_t length;
  if (path == NULL) {
    return 0;
  }
  length = strlen(path);
  if (length >= sizeof(marker_path)) {
    return 0;
  }
  memcpy(marker_path, path, length + 1);
  marker_enabled = 1;
  return 1;
}

int32_t muon_adhoc_add(int32_t left, int32_t right) {
  return left + right;
}

int32_t muon_adhoc_sleep_then_add(
    int32_t left,
    int32_t right,
    uint32_t milliseconds) {
  struct timespec request;
  request.tv_sec = milliseconds / 1000u;
  request.tv_nsec = (long)(milliseconds % 1000u) * 1000000L;
  while (nanosleep(&request, &request) != 0 && errno == EINTR) {
  }
  return left + right;
}

void* muon_adhoc_alloc(uint64_t size) {
  if (size > (uint64_t)SIZE_MAX) {
    return NULL;
  }
  return malloc((size_t)size);
}

void muon_adhoc_free(void* pointer) {
  free(pointer);
}
`;

const adhocWindowsLibrarySource = `#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static char marker_path[4096];
static int marker_enabled = 0;

static void write_marker(const char* label) {
  HANDLE file;
  DWORD written;
  if (!marker_enabled) {
    return;
  }
  file = CreateFileA(
      marker_path,
      FILE_APPEND_DATA,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      NULL,
      OPEN_ALWAYS,
      FILE_ATTRIBUTE_NORMAL,
      NULL);
  if (file == INVALID_HANDLE_VALUE) {
    return;
  }
  WriteFile(file, label, (DWORD)strlen(label), &written, NULL);
  WriteFile(file, "\\r\\n", 2, &written, NULL);
  CloseHandle(file);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)instance;
  (void)reserved;
  if (reason == DLL_PROCESS_DETACH) {
    write_marker("unloaded");
  }
  return TRUE;
}

__declspec(dllexport) int32_t __cdecl muon_adhoc_set_marker_path(
    const char* path) {
  size_t length;
  if (path == NULL) {
    return 0;
  }
  length = strlen(path);
  if (length >= sizeof(marker_path)) {
    return 0;
  }
  memcpy(marker_path, path, length + 1);
  marker_enabled = 1;
  return 1;
}

__declspec(dllexport) int32_t __cdecl muon_adhoc_add(
    int32_t left,
    int32_t right) {
  return left + right;
}

__declspec(dllexport) int32_t __cdecl muon_adhoc_sleep_then_add(
    int32_t left,
    int32_t right,
    uint32_t milliseconds) {
  Sleep(milliseconds);
  return left + right;
}

__declspec(dllexport) void* __cdecl muon_adhoc_alloc(size_t size) {
  return malloc(size);
}

__declspec(dllexport) void __cdecl muon_adhoc_free(void* pointer) {
  free(pointer);
}
`;

const adhocWindowsLibraryDefinition = `LIBRARY muon_adhoc_library
EXPORTS
  muon_adhoc_set_marker_path
  muon_adhoc_add
  muon_adhoc_sleep_then_add
  muon_adhoc_alloc
  muon_adhoc_free
`;

interface AdhocLibraryBuild {
  readonly directory: string;
  readonly libraryPath: string;
  readonly markerPath: string;
  readonly contextMarkerPath: string;
}

interface AdhocCallValues {
  readonly addResult: number;
  readonly callAfterReleaseRejected: boolean;
  readonly invalidSignatureRejected: boolean;
  readonly missingLibraryRejected: boolean;
  readonly missingSymbolRejected: boolean;
  readonly markerAccepted: number;
  readonly pointerNonZero: boolean;
  readonly releaseSettledBeforeCall: boolean;
  readonly responsivenessElapsed: number;
  readonly sleepResult: number;
  readonly ticksDuringCall: number;
}

const expectAdhocCallValues = (values: AdhocCallValues): void => {
  const serialized = JSON.stringify(values);
  const expectEqual = <T>(name: string, actual: T, expected: T): void => {
    if (actual !== expected) {
      throw new Error(
        `Expected ${name} to be ${String(expected)}, got ${String(actual)}: ${serialized}`,
      );
    }
  };

  expectEqual("addResult", values.addResult, 42);
  expectEqual(
    "callAfterReleaseRejected",
    values.callAfterReleaseRejected,
    true,
  );
  expectEqual(
    "invalidSignatureRejected",
    values.invalidSignatureRejected,
    true,
  );
  expectEqual("markerAccepted", values.markerAccepted, 1);
  expectEqual("missingLibraryRejected", values.missingLibraryRejected, true);
  expectEqual("missingSymbolRejected", values.missingSymbolRejected, true);
  expectEqual("pointerNonZero", values.pointerNonZero, true);
  expectEqual(
    "releaseSettledBeforeCall",
    values.releaseSettledBeforeCall,
    false,
  );
  expectEqual(
    "responsivenessElapsed type",
    typeof values.responsivenessElapsed,
    "number",
  );
  expectEqual("sleepResult", values.sleepResult, 42);
  expectEqual("ticksDuringCall type", typeof values.ticksDuringCall, "number");
  expect(values.responsivenessElapsed, serialized).toBeLessThan(900);
  expect(values.ticksDuringCall, serialized).toBeGreaterThan(0);
};

const buildAdhocTestLibrary = async (): Promise<AdhocLibraryBuild> => {
  const directory = await mkdtemp(join(tmpdir(), "muon-adhoc-library-"));
  const sourcePath = join(directory, "muon_adhoc_library.c");
  const libraryPath = join(directory, "libmuon_adhoc_library.so");
  const markerPath = join(directory, "release-marker.txt");
  const contextMarkerPath = join(directory, "context-marker.txt");
  try {
    await writeFile(sourcePath, adhocLibrarySource, "utf8");
    await execFileAsync(
      "gcc",
      [
        sourcePath,
        "-std=c99",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-O2",
        "-shared",
        "-fPIC",
        "-o",
        libraryPath,
      ],
      { timeout: cdpCommandTimeoutMs },
    );
  } catch (error) {
    await rm(directory, { recursive: true, force: true });
    throw error;
  }
  return {
    directory,
    libraryPath,
    markerPath,
    contextMarkerPath,
  };
};

const buildWindowsAdhocTestLibrary = async (): Promise<AdhocLibraryBuild> => {
  const context = getWindowsRemoteContext();
  if (context === undefined) {
    throw new Error("Windows remote e2e context is not configured");
  }

  const compiler =
    context.runtime.target === "windows-i686"
      ? "i686-w64-mingw32-gcc"
      : "x86_64-w64-mingw32-gcc";
  const localDirectory = await nodeMkdtemp(
    nodeJoin(nodeTmpdir(), "muon-adhoc-windows-library-"),
  );
  let remoteDirectory: string | undefined = undefined;
  try {
    const sourcePath = nodeJoin(localDirectory, "muon_adhoc_library.c");
    const definitionPath = nodeJoin(localDirectory, "muon_adhoc_library.def");
    const localLibraryPath = nodeJoin(localDirectory, "muon_adhoc_library.dll");
    await nodeWriteFile(sourcePath, adhocWindowsLibrarySource, "utf8");
    await nodeWriteFile(definitionPath, adhocWindowsLibraryDefinition, "utf8");
    await execFileAsync(
      compiler,
      [
        sourcePath,
        definitionPath,
        "-std=c99",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-O2",
        "-shared",
        "-o",
        localLibraryPath,
      ],
      { timeout: cdpCommandTimeoutMs },
    );

    remoteDirectory = await mkdtemp(
      join(tmpdir(), "muon-adhoc-windows-library-"),
    );
    const libraryPath = join(remoteDirectory, "muon_adhoc_library.dll");
    const markerPath = join(remoteDirectory, "release-marker.txt");
    const contextMarkerPath = join(remoteDirectory, "context-marker.txt");
    await writeFile(libraryPath, await nodeReadFile(localLibraryPath));
    return {
      directory: remoteDirectory,
      libraryPath,
      markerPath,
      contextMarkerPath,
    };
  } catch (error) {
    if (remoteDirectory !== undefined) {
      await rm(remoteDirectory, { recursive: true, force: true });
    }
    throw error;
  } finally {
    await nodeRm(localDirectory, { recursive: true, force: true });
  }
};

describeMuonPluginBridge("muon plugin bridge - runtime APIs", () => {
  const linuxIt = isLocalLinuxE2e ? it : it.skip;

  it("starts with internal plugins and exposes configured APIs", async () => {
    await withMuon([], async (driver) => {
      await expect(
        driver.evaluate("typeof window.muon.fs.readFile"),
      ).resolves.toBe("function");
      await expect(
        driver.evaluate("typeof window.muon.fs.writeFile"),
      ).resolves.toBe("function");
      await expect(
        driver.evaluate("typeof window.muon.fs.dialogs.selectFile"),
      ).resolves.toBe("function");
      await expect(
        driver.evaluate("typeof window.muon.environments.getVariables"),
      ).resolves.toBe("function");
      await expect(
        driver.evaluate("typeof window.muon.environments.getConfigValues"),
      ).resolves.toBe("function");
      await expect(
        driver.evaluate("typeof window.muon.environments.getAutostart"),
      ).resolves.toBe("function");
      await expect(
        driver.evaluate("typeof window.muon.environments.getRuntimeInfo"),
      ).resolves.toBe("function");
      await expect(
        driver.evaluate("typeof window.muon.environments.setAutostart"),
      ).resolves.toBe("function");
      await expect(
        driver.evaluate("typeof window.muon.launcher.getSettings"),
      ).resolves.toBe("function");
      await expect(
        driver.evaluate("typeof window.muon.launcher.setSettings"),
      ).resolves.toBe("function");
      await expect(
        driver.evaluate("typeof window.muon.launcher.triggerUpdate"),
      ).resolves.toBe("function");
      await expect(
        driver.evaluate(`typeof window.muon.environments["run"]`),
      ).resolves.toBe("undefined");
      await expect(
        driver.evaluate("typeof window.muon.executor.spawn"),
      ).resolves.toBe("function");
      await expect(
        driver.evaluate("typeof window.muon.executor.run"),
      ).resolves.toBe("undefined");
      await expect(
        driver.evaluate(
          `(${JSON.stringify(browserFunctionNames)}).map((name) => typeof window.muon.browser[name])`,
        ),
      ).resolves.toEqual(browserFunctionNames.map(() => "function"));
      await expect(driver.evaluate("typeof window.muon.test")).resolves.toBe(
        "undefined",
      );
    });
  });

  it("does not expose plugin APIs when plugins allow is empty", async () => {
    const running = await startDebugMuon(
      ["muon_test_plugin_alpha"],
      TEST_NETWORK_ALLOW_PATTERNS,
      {},
      undefined,
      [],
    );
    let driver: CdpDriver | undefined = undefined;
    try {
      driver = await connectToMuonCdp({
        port: MUON_PORT,
        timeoutMs: cdpCommandTimeoutMs,
      });
      await driver.navigate(
        "data:text/html,<title>muon plugin deny</title>",
        cdpCommandTimeoutMs,
      );
      await expect(driver.evaluate("typeof window.muon")).resolves.toBe(
        "undefined",
      );
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
    } finally {
      await stopMuon(running, driver);
    }
  });

  it("does not load plugin libraries omitted from muon.json", async () => {
    const markerDirectory = await mkdtemp(
      join(tmpdir(), "muon-plugin-marker-"),
    );
    const markerPath = join(markerDirectory, "marker.txt");
    const running = await startDebugMuon(
      ["muon_test_plugin_alpha", "muon_test_plugin_load_marker"],
      TEST_NETWORK_ALLOW_PATTERNS,
      { MUON_TEST_PLUGIN_LOAD_MARKER: markerPath },
      undefined,
      TEST_PLUGIN_ALLOW_PATTERNS,
      ["muon_test_plugin_alpha"],
    );
    let driver: CdpDriver | undefined = undefined;
    try {
      driver = await connectToMuonCdp({
        port: MUON_PORT,
        timeoutMs: cdpCommandTimeoutMs,
      });
      await driver.navigate(
        "data:text/html,<title>muon plugin explicit load</title>",
        cdpCommandTimeoutMs,
      );
      await expect(access(markerPath, constants.F_OK)).rejects.toThrow();
      await expect(
        driver.evaluate("typeof window.muon.test.loadMarker"),
      ).resolves.toBe("undefined");
      await expect(
        driver.evaluate("window.muon.test.alpha.alphaName()"),
      ).resolves.toBe("alpha");
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
    } finally {
      await stopMuon(running, driver);
      await rm(markerDirectory, { recursive: true, force: true });
    }
  });

  it("does not load plugin libraries with an empty allow list", async () => {
    const markerDirectory = await mkdtemp(
      join(tmpdir(), "muon-plugin-marker-"),
    );
    const markerPath = join(markerDirectory, "marker.txt");
    const running = await startDebugMuon(
      ["muon_test_plugin_load_marker"],
      TEST_NETWORK_ALLOW_PATTERNS,
      { MUON_TEST_PLUGIN_LOAD_MARKER: markerPath },
      undefined,
      [],
    );
    let driver: CdpDriver | undefined = undefined;
    try {
      driver = await connectToMuonCdp({
        port: MUON_PORT,
        timeoutMs: cdpCommandTimeoutMs,
      });
      await driver.navigate(
        "data:text/html,<title>muon plugin empty allow</title>",
        cdpCommandTimeoutMs,
      );
      await expect(access(markerPath, constants.F_OK)).rejects.toThrow();
      await expect(driver.evaluate("typeof window.muon")).resolves.toBe(
        "undefined",
      );
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
    } finally {
      await stopMuon(running, driver);
      await rm(markerDirectory, { recursive: true, force: true });
    }
  });

  it("loads an external plugin when its signature matches", async () => {
    const pluginName = "muon_test_plugin_alpha";
    const pluginSalt = "deadbeef";
    const pluginSignature = await calculatePluginSignature(
      pluginName,
      pluginSalt,
    );
    expect(pluginSignature).toMatch(/^[0-9a-f]{64}$/);
    const running = await startDebugMuon(
      [pluginName],
      TEST_NETWORK_ALLOW_PATTERNS,
      {},
      undefined,
      TEST_PLUGIN_ALLOW_PATTERNS,
      [pluginName],
      TEST_BROWSER_PLUGIN_ALLOW_PATTERNS,
      [],
      null,
      true,
      undefined,
      undefined,
      undefined,
      undefined,
      undefined,
      undefined,
      undefined,
      undefined,
      {},
      { [pluginName]: pluginSignature },
      { [pluginName]: pluginSalt },
    );
    let driver: CdpDriver | undefined = undefined;
    try {
      driver = await connectToMuonCdp({
        port: MUON_PORT,
        timeoutMs: cdpCommandTimeoutMs,
      });
      await driver.navigate(
        "data:text/html,<title>muon plugin signature match</title>",
        cdpCommandTimeoutMs,
      );
      await expect(
        driver.evaluate("window.muon.test.alpha.alphaName()"),
      ).resolves.toBe("alpha");
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
    } finally {
      await stopMuon(running, driver);
    }
  });

  it("rejects an external plugin before loading when its signature mismatches", async () => {
    const markerDirectory = await mkdtemp(
      join(tmpdir(), "muon-plugin-signature-marker-"),
    );
    const markerPath = join(markerDirectory, "marker.txt");
    const pluginName = "muon_test_plugin_load_marker";
    const pluginSalt = "deadbeef";
    const correctPluginSignature = await calculatePluginSignature(
      pluginName,
      pluginSalt,
    );
    const mismatchedFirstNibble = correctPluginSignature[0] === "0" ? "1" : "0";
    const mismatchedPluginSignature =
      mismatchedFirstNibble + correctPluginSignature.slice(1);
    expect(mismatchedPluginSignature).toMatch(/^[0-9a-f]{64}$/);
    expect(mismatchedPluginSignature).not.toBe(correctPluginSignature);
    const running = await startMuon(
      DEBUG_MUON_DIRECTORY,
      [pluginName],
      TEST_NETWORK_ALLOW_PATTERNS,
      TEST_PLUGIN_ALLOW_PATTERNS,
      false,
      shouldUseValgrind,
      undefined,
      { MUON_TEST_PLUGIN_LOAD_MARKER: markerPath },
      [pluginName],
      TEST_BROWSER_PLUGIN_ALLOW_PATTERNS,
      [],
      null,
      true,
      undefined,
      undefined,
      undefined,
      undefined,
      undefined,
      undefined,
      undefined,
      undefined,
      undefined,
      cdpCommandTimeoutMs,
      undefined,
      { [pluginName]: mismatchedPluginSignature },
      { [pluginName]: pluginSalt },
    );
    try {
      await waitForProcessExit(running, processExitTimeoutMs);
      expect(running.process.exitCode).toBe(1);
      expect(running.stderr).toContain("Plugin signature mismatch");
      await expect(access(markerPath, constants.F_OK)).rejects.toThrow();
    } finally {
      await stopMuon(running, undefined);
      await rm(markerDirectory, { recursive: true, force: true });
    }
  });

  it("rejects plugin namespaces with a single segment during startup", async () => {
    const running = await startMuon(
      DEBUG_MUON_DIRECTORY,
      ["muon_test_plugin_single_namespace"],
      TEST_NETWORK_ALLOW_PATTERNS,
      TEST_PLUGIN_ALLOW_PATTERNS,
      false,
      shouldUseValgrind,
      undefined,
    );
    try {
      await waitForProcessExit(running, processExitTimeoutMs);
      await expectProcessExitCode(running, 1);
      await waitForMuonStderr(
        running,
        "Plugin namespace must contain at least two segments: single",
        cdpCommandTimeoutMs,
      );
    } finally {
      await stopMuon(running, undefined);
    }
  });

  it("filters setup-script wrappers by public function path", async () => {
    const spawnOptions = createExecutorOkSpawnOptions();
    const running = await startDebugMuon(
      [],
      TEST_NETWORK_ALLOW_PATTERNS,
      {},
      undefined,
      ["muon.executor.spawn"],
    );
    let driver: CdpDriver | undefined = undefined;
    try {
      driver = await connectToMuonCdp({
        port: MUON_PORT,
        timeoutMs: cdpCommandTimeoutMs,
      });
      await driver.navigate(
        "data:text/html,<title>muon plugin partial</title>",
        cdpCommandTimeoutMs,
      );
      const values = await driver.evaluate<{
        keys: string[];
        spawnType: string;
        runType: string;
        internalRpcType: string;
        environmentType: string;
        result: {
          processId: number;
          exitCode: number;
          stdout: string;
          stderr: string;
        };
      }>(`(async () => {
        const decoder = new TextDecoder();
        const child = await window.muon.executor.spawn(${JSON.stringify(spawnOptions)});
        const waitResult = await child.wait();
        return {
          keys: Object.keys(window.muon.executor).sort(),
          spawnType: typeof window.muon.executor.spawn,
          runType: typeof window.muon.executor.run,
          internalRpcType: typeof window.muon.executor.__spawnRpc,
          environmentType: typeof window.muon.environments,
          result: {
            processId: waitResult.processId,
            exitCode: waitResult.exitCode,
            stdout: decoder.decode(waitResult.stdout),
            stderr: decoder.decode(waitResult.stderr),
          },
        };
      })()`);
      expect(values).toEqual({
        keys: ["spawn"],
        spawnType: "function",
        runType: "undefined",
        internalRpcType: "function",
        environmentType: "undefined",
        result: {
          processId: expect.any(Number),
          exitCode: 0,
          stdout: "ok",
          stderr: "",
        },
      });
      expect(values.result.processId).toBeGreaterThan(0);
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
    } finally {
      await stopMuon(running, driver);
    }
  });

  it("exposes runtime information through the internal environment API", async () => {
    await withMuonEnvironment(
      [],
      { MUON_E2E_ENVIRONMENTS_TEST: "visible-value" },
      async (driver) => {
        const values = await driver.evaluate<{
          keys: string[];
          runtimeInfo: {
            name: string;
            executableName: string;
            target: string;
            cefTarget: string;
            muonCore: {
              version: string;
              gitCommitHash: string;
              buildDate: string;
              gitCommitDate: string;
            };
            cefReference: {
              version: string;
              distribution: string;
              apiVersion: number;
              apiHash: string;
              artifact: {
                fileName: string;
                url: string;
                sha1: string;
                size: number;
              };
            };
            cefRuntime: {
              version: string;
              apiVersion: number;
              apiHash: string;
            };
            corePayload: string[];
            cef?: unknown;
          };
          internalType: string;
        }>(`(async () => {
          const runtimeInfo = await window.muon.environments.getRuntimeInfo();
          return {
            keys: Object.keys(window.muon.environments).sort(),
            runtimeInfo,
            internalType: typeof window.muon.environments.__getRuntimeInfo,
          };
        })()`);

        expect(values.keys).toEqual([
          "getAutostart",
          "getCommandLine",
          "getConfigValues",
          "getProcessId",
          "getRuntimeInfo",
          "getVariables",
          "setAutostart",
        ]);
        expect(values.internalType).toBe("function");
        expect(values.runtimeInfo).toMatchObject({
          name: "muon-core",
          executableName: expectedRuntimeExecutableName(),
          target: expectedRuntimeTarget(),
          cefTarget: expectedCefTarget(),
          muonCore: {
            version: expect.any(String),
            gitCommitHash: expect.any(String),
            buildDate: expect.any(String),
            gitCommitDate: expect.any(String),
          },
          cefReference: {
            version: expect.any(String),
            distribution: "minimal",
            apiVersion: expect.any(Number),
            apiHash: expect.stringMatching(/^[0-9a-f]{40}$/),
            artifact: {
              fileName: expect.stringContaining(expectedCefArtifactNamePart()),
              url: expect.stringContaining(expectedCefArtifactNamePart()),
              sha1: expect.stringMatching(/^[0-9a-f]{40}$/),
              size: expect.any(Number),
            },
          },
          cefRuntime: {
            version: expect.any(String),
            apiVersion: expect.any(Number),
            apiHash: expect.stringMatching(/^[0-9a-f]{40}$/),
          },
          corePayload: expect.arrayContaining([
            expectedRuntimeExecutableName(),
            "CREDITS.md",
          ]),
        });
        expect(values.runtimeInfo.corePayload).not.toContain(
          "muon-runtime.json",
        );
        expect(values.runtimeInfo.corePayload).not.toContain(
          "THIRD_PARTY_NOTICES.md",
        );
        expect(values.runtimeInfo.cef).toBeUndefined();
        expect(values.runtimeInfo.cefReference.version).toBe(
          values.runtimeInfo.cefRuntime.version,
        );
        expect(values.runtimeInfo.cefReference.apiVersion).toBe(
          values.runtimeInfo.cefRuntime.apiVersion,
        );
        expect(values.runtimeInfo.cefReference.apiHash).toBe(
          values.runtimeInfo.cefRuntime.apiHash,
        );
        expect(values.runtimeInfo.muonCore.version).not.toBe("");
        expect(values.runtimeInfo.muonCore.gitCommitHash).not.toBe("");
        expect(values.runtimeInfo.muonCore.buildDate).not.toBe("");
        expect(values.runtimeInfo.muonCore.gitCommitDate).not.toBe("");
        expect(values.runtimeInfo.cefReference.artifact.size).toBeGreaterThan(
          0,
        );
      },
    );
  });

  it("filters runtime information through the internal plugin policy", async () => {
    const running = await startDebugMuon(
      [],
      TEST_NETWORK_ALLOW_PATTERNS,
      {},
      undefined,
      ["muon.environments.getRuntimeInfo"],
    );
    let driver: CdpDriver | undefined = undefined;
    try {
      driver = await connectToMuonCdp({
        port: MUON_PORT,
        timeoutMs: cdpCommandTimeoutMs,
      });
      await driver.navigate(
        "data:text/html,<title>muon runtime info partial</title>",
        cdpCommandTimeoutMs,
      );
      await expect(
        driver.evaluate(`(async () => {
          const runtimeInfo = await window.muon.environments.getRuntimeInfo();
          return {
            keys: Object.keys(window.muon.environments).sort(),
            runtimeInfoName: runtimeInfo.name,
            variablesType: typeof window.muon.environments.getVariables,
            internalType: typeof window.muon.environments.__getRuntimeInfo,
          };
        })()`),
      ).resolves.toEqual({
        keys: ["getRuntimeInfo"],
        runtimeInfoName: "muon-core",
        variablesType: "undefined",
        internalType: "function",
      });
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
    } finally {
      await stopMuon(running, driver);
    }
  });

  it("manages launcher update settings through the internal launcher API", async () => {
    const stateHome = await mkdtemp(join(tmpdir(), "muon-launcher-state-"));
    const configPath = join(
      stateHome,
      "muon-launcher",
      "runtime",
      "muon-launcher.ini",
    );
    let launcherConfigPath = configPath;
    const launcherEnvironment = isWindowsRemoteE2e()
      ? { LOCALAPPDATA: stateHome }
      : { XDG_STATE_HOME: stateHome };
    try {
      await withMuonEnvironment(
        [],
        launcherEnvironment,
        async (driver, running) => {
          if (running.remoteWindows !== undefined) {
            launcherConfigPath = join(
              running.remoteWindows.configDirectory,
              "..",
              "muon-launcher.ini",
            );
            await rm(launcherConfigPath, { force: true });
          }
          const values = await driver.evaluate<{
            keys: string[];
            initial: {
              cefVersionPolicy: string;
              cefExactVersion: string;
              catalogRefreshIntervalSeconds: number;
            };
            updated: {
              cefVersionPolicy: string;
              cefExactVersion: string;
              catalogRefreshIntervalSeconds: number;
            };
            reverted: {
              cefVersionPolicy: string;
              cefExactVersion: string;
              catalogRefreshIntervalSeconds: number;
            };
            internalType: string;
          }>(`(async () => {
            const initial = await window.muon.launcher.getSettings();
            await window.muon.launcher.setSettings({
              cefVersionPolicy: "compat-latest",
              cefExactVersion: "",
              catalogRefreshIntervalSeconds: 123,
            });
            const updated = await window.muon.launcher.getSettings();
            await window.muon.launcher.setSettings({
              cefVersionPolicy: null,
              catalogRefreshIntervalSeconds: null,
            });
            const reverted = await window.muon.launcher.getSettings();
            await window.muon.launcher.triggerUpdate();
            return {
              keys: Object.keys(window.muon.launcher).sort(),
              initial,
              updated,
              reverted,
              internalType: typeof window.muon.launcher.__triggerUpdate,
            };
          })()`);

          expect(values).toEqual({
            keys: ["getSettings", "setSettings", "triggerUpdate"],
            initial: {
              cefVersionPolicy: "tested",
              cefExactVersion: "",
              catalogRefreshIntervalSeconds: 604800,
            },
            updated: {
              cefVersionPolicy: "compat-latest",
              cefExactVersion: "",
              catalogRefreshIntervalSeconds: 123,
            },
            reverted: {
              cefVersionPolicy: "tested",
              cefExactVersion: "",
              catalogRefreshIntervalSeconds: 604800,
            },
            internalType: "function",
          });
        },
      );
      const launcherConfig = await waitForTextFileContent(
        launcherConfigPath,
        (content) => content.includes("requested=true"),
        "launcher update request settings",
      );
      expect(launcherConfig).not.toContain("versionPolicy=");
    } finally {
      await rm(stateHome, { force: true, recursive: true });
    }
  });

  it("filters launcher functions through the internal plugin policy", async () => {
    const running = await startDebugMuon(
      [],
      TEST_NETWORK_ALLOW_PATTERNS,
      {},
      undefined,
      ["muon.launcher.getSettings"],
    );
    let driver: CdpDriver | undefined = undefined;
    try {
      driver = await connectToMuonCdp({
        port: MUON_PORT,
        timeoutMs: cdpCommandTimeoutMs,
      });
      await driver.navigate(
        "data:text/html,<title>muon launcher partial</title>",
        cdpCommandTimeoutMs,
      );
      await expect(
        driver.evaluate(`(async () => {
          const settings = await window.muon.launcher.getSettings();
          return {
            keys: Object.keys(window.muon.launcher).sort(),
            policy: settings.cefVersionPolicy,
            setSettingsType: typeof window.muon.launcher.setSettings,
            internalType: typeof window.muon.launcher.__getSettings,
          };
        })()`),
      ).resolves.toEqual({
        keys: ["getSettings"],
        policy: "tested",
        setSettingsType: "undefined",
        internalType: "function",
      });
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
    } finally {
      await stopMuon(running, driver);
    }
  });

  it("filters filesystem dialogs through the internal plugin policy", async () => {
    const running = await startDebugMuon(
      [],
      TEST_NETWORK_ALLOW_PATTERNS,
      {},
      undefined,
      ["muon.fs.dialogs.selectFile"],
    );
    let driver: CdpDriver | undefined = undefined;
    try {
      driver = await connectToMuonCdp({
        port: MUON_PORT,
        timeoutMs: cdpCommandTimeoutMs,
      });
      await driver.navigate(
        "data:text/html,<title>muon fs dialogs partial allow</title>",
        cdpCommandTimeoutMs,
      );
      await expect(
        driver.evaluate(`({
          fsType: typeof window.muon.fs,
          readFileType: typeof window.muon.fs.readFile,
          dialogsKeys: Object.keys(window.muon.fs.dialogs).sort(),
          selectFileType: typeof window.muon.fs.dialogs.selectFile,
          selectFilesType: typeof window.muon.fs.dialogs.selectFiles,
          nativeSelectFileType: typeof window.muon.fs.dialogs.__selectFile,
          environmentsType: typeof window.muon.environments,
        })`),
      ).resolves.toEqual({
        fsType: "object",
        readFileType: "undefined",
        dialogsKeys: ["selectFile"],
        selectFileType: "function",
        selectFilesType: "undefined",
        nativeSelectFileType: "function",
        environmentsType: "undefined",
      });
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
    } finally {
      await stopMuon(running, driver);
    }
  });

  it("exposes filesystem dialogs without a standard plugin entry", async () => {
    const running = await startDebugMuon(
      [],
      TEST_NETWORK_ALLOW_PATTERNS,
      {},
      undefined,
      TEST_PLUGIN_ALLOW_PATTERNS,
      [],
      undefined,
      undefined,
      undefined,
      false,
    );
    let driver: CdpDriver | undefined = undefined;
    try {
      driver = await connectToMuonCdp({
        port: MUON_PORT,
        timeoutMs: cdpCommandTimeoutMs,
      });
      await driver.navigate(
        "data:text/html,<title>muon fs dialogs plugin omitted</title>",
        cdpCommandTimeoutMs,
      );
      await expect(
        driver.evaluate(`({
          fsType: typeof window.muon.fs,
          readFileType: typeof window.muon.fs.readFile,
          selectFileType: typeof window.muon.fs.dialogs.selectFile,
        })`),
      ).resolves.toEqual({
        fsType: "object",
        readFileType: "function",
        selectFileType: "function",
      });
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
    } finally {
      await stopMuon(running, driver);
    }
  });

  it("filters built-in browser functions by public function path", async () => {
    const running = await startDebugMuon(
      [],
      TEST_NETWORK_ALLOW_PATTERNS,
      {},
      undefined,
      ["muon.browser.reload"],
    );
    let driver: CdpDriver | undefined = undefined;
    try {
      driver = await connectToMuonCdp({
        port: MUON_PORT,
        timeoutMs: cdpCommandTimeoutMs,
      });
      await driver.navigate(
        "data:text/html,<title>muon browser partial allow</title>",
        cdpCommandTimeoutMs,
      );
      await expect(
        driver.evaluate(`({
          browserKeys: Object.keys(window.muon.browser).sort(),
          reloadType: typeof window.muon.browser.reload,
          hardReloadType: typeof window.muon.browser.hardReload,
          fsType: typeof window.muon.fs,
          executorType: typeof window.muon.executor,
        })`),
      ).resolves.toEqual({
        browserKeys: ["reload"],
        reloadType: "function",
        hardReloadType: "undefined",
        fsType: "undefined",
        executorType: "undefined",
      });
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
    } finally {
      await stopMuon(running, driver);
    }
  });

  it("filters the explicit fullscreen browser function by public function path", async () => {
    const running = await startDebugMuon(
      [],
      TEST_NETWORK_ALLOW_PATTERNS,
      {},
      undefined,
      ["muon.browser.enterFullscreen"],
    );
    let driver: CdpDriver | undefined = undefined;
    try {
      driver = await connectToMuonCdp({
        port: MUON_PORT,
        timeoutMs: cdpCommandTimeoutMs,
      });
      await driver.navigate(
        "data:text/html,<title>muon browser fullscreen partial allow</title>",
        cdpCommandTimeoutMs,
      );
      await expect(
        driver.evaluate(`({
          browserKeys: Object.keys(window.muon.browser).sort(),
          enterFullscreenType: typeof window.muon.browser.enterFullscreen,
          exitFullscreenType: typeof window.muon.browser.exitFullscreen,
          toggleFullscreenType: typeof window.muon.browser.toggleFullscreen,
        })`),
      ).resolves.toEqual({
        browserKeys: ["enterFullscreen"],
        enterFullscreenType: "function",
        exitFullscreenType: "undefined",
        toggleFullscreenType: "undefined",
      });
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
    } finally {
      await stopMuon(running, driver);
    }
  });

  it("filters the window bounds browser wrappers by public function path", async () => {
    const running = await startDebugMuon(
      [],
      TEST_NETWORK_ALLOW_PATTERNS,
      {},
      undefined,
      ["muon.browser.getWindowBounds", "muon.browser.setWindowBounds"],
    );
    let driver: CdpDriver | undefined = undefined;
    try {
      driver = await connectToMuonCdp({
        port: MUON_PORT,
        timeoutMs: cdpCommandTimeoutMs,
      });
      await driver.navigate(
        "data:text/html,<title>muon browser bounds partial allow</title>",
        cdpCommandTimeoutMs,
      );
      await expect(
        driver.evaluate(`({
          browserKeys: Object.keys(window.muon.browser).sort(),
          getWindowBoundsType: typeof window.muon.browser.getWindowBounds,
          setWindowBoundsType: typeof window.muon.browser.setWindowBounds,
          internalGetWindowBoundsType: typeof window.muon.browser.__getWindowBounds,
          internalSetWindowBoundsType: typeof window.muon.browser.__setWindowBounds,
          reloadType: typeof window.muon.browser.reload,
        })`),
      ).resolves.toEqual({
        browserKeys: ["getWindowBounds", "setWindowBounds"],
        getWindowBoundsType: "function",
        setWindowBoundsType: "function",
        internalGetWindowBoundsType: "function",
        internalSetWindowBoundsType: "function",
        reloadType: "undefined",
      });
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
    } finally {
      await stopMuon(running, driver);
    }
  });

  it("filters the shutdown browser wrapper by public function path", async () => {
    const running = await startDebugMuon(
      [],
      TEST_NETWORK_ALLOW_PATTERNS,
      {},
      undefined,
      ["muon.browser.shutdown"],
    );
    let driver: CdpDriver | undefined = undefined;
    try {
      driver = await connectToMuonCdp({
        port: MUON_PORT,
        timeoutMs: cdpCommandTimeoutMs,
      });
      await driver.navigate(
        "data:text/html,<title>muon browser shutdown partial allow</title>",
        cdpCommandTimeoutMs,
      );
      await expect(
        driver.evaluate(`({
          browserKeys: Object.keys(window.muon.browser).sort(),
          shutdownType: typeof window.muon.browser.shutdown,
          internalShutdownType: typeof window.muon.browser.__shutdown,
          reloadType: typeof window.muon.browser.reload,
          fsType: typeof window.muon.fs,
          executorType: typeof window.muon.executor,
        })`),
      ).resolves.toEqual({
        browserKeys: ["shutdown"],
        shutdownType: "function",
        internalShutdownType: "function",
        reloadType: "undefined",
        fsType: "undefined",
        executorType: "undefined",
      });
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
    } finally {
      await stopMuon(running, driver);
    }
  });

  it("exposes environment information and runs child processes", async () => {
    const spawnOptions = createExecutorStdinSpawnOptions();
    await withMuonEnvironment(
      [],
      { MUON_E2E_ENVIRONMENTS_TEST: "visible-value" },
      async (driver) => {
        const values = await driver.evaluate<{
          envValue: string;
          commandLineHasPluginDir: boolean;
          processId: number;
          keys: string[];
          internalType: string;
          runtimeInfoName: string;
          runResult: {
            processId: number;
            exitCode: number;
            stdout: string;
            stderr: string;
          };
          executorKeys: string[];
          executorInternalType: string;
        }>(`(async () => {
          const decoder = new TextDecoder();
          const variables = await window.muon.environments.getVariables();
          const commandLine = await window.muon.environments.getCommandLine();
          const processId = await window.muon.environments.getProcessId();
          const runtimeInfo = await window.muon.environments.getRuntimeInfo();
          const child = await window.muon.executor.spawn(${JSON.stringify(spawnOptions)});
          await child.writeStdin("he");
          await child.writeStdin(new Uint8Array([108, 108, 111]));
          await child.closeStdin();
          const waitResult = await child.wait();
          return {
            envValue: variables.MUON_E2E_ENVIRONMENTS_TEST,
            commandLineHasPluginDir: commandLine.some((value) =>
              value.startsWith("--muon-plugin-dir="),
            ),
            processId,
            keys: Object.keys(window.muon.environments).sort(),
            internalType: typeof window.muon.environments.__getVariables,
            runtimeInfoName: runtimeInfo.name,
            runResult: {
              processId: waitResult.processId,
              exitCode: waitResult.exitCode,
              stdout: decoder.decode(waitResult.stdout),
              stderr: decoder.decode(waitResult.stderr),
            },
            executorKeys: Object.keys(window.muon.executor).sort(),
            executorInternalType: typeof window.muon.executor.__spawnRpc,
          };
        })()`);

        expect(values).toEqual({
          envValue: "visible-value",
          commandLineHasPluginDir: false,
          processId: expect.any(Number),
          keys: [
            "getAutostart",
            "getCommandLine",
            "getConfigValues",
            "getProcessId",
            "getRuntimeInfo",
            "getVariables",
            "setAutostart",
          ],
          internalType: "function",
          runtimeInfoName: "muon-core",
          runResult: {
            processId: expect.any(Number),
            exitCode: 7,
            stdout: "stdout:hello",
            stderr: "stderr:ok",
          },
          executorKeys: [
            "boolType",
            "bufferViewType",
            "float32Type",
            "float64Type",
            "int16Type",
            "int32Type",
            "int64Type",
            "int8Type",
            "loadLibrary",
            "pointerType",
            "spawn",
            "stringType",
            "uint16Type",
            "uint32Type",
            "uint64Type",
            "uint8Type",
            "usizeType",
            "voidType",
          ],
          executorInternalType: "function",
        });
        expect(values.processId).toBeGreaterThan(0);
        expect(values.runResult.processId).toBeGreaterThan(0);
      },
    );
  });

  it("returns independent application config values", async () => {
    await withMuon(
      [],
      async (driver) => {
        await expect(
          driver.evaluate(`(async () => {
            const first = await window.muon.environments.getConfigValues();
            first.apiBaseUrl = "changed";
            const second = await window.muon.environments.getConfigValues();
            return { first, second };
          })()`),
        ).resolves.toEqual({
          first: { apiBaseUrl: "changed", empty: "" },
          second: { apiBaseUrl: "https://api.example", empty: "" },
        });
      },
      {},
      { apiBaseUrl: "https://api.example", empty: "" },
    );
  });

  it("streams child process output and writes stdin incrementally", async () => {
    const spawnOptions = createExecutorStreamingSpawnOptions();
    await withMuonEnvironment([], {}, async (driver) => {
      const values = await driver.evaluate<{
        processId: number;
        exitCode: number;
        stdoutBytesBeforeInput: number[];
        stderrBytesBeforeInput: number[];
        stdoutBytes: number[];
        stderrBytes: number[];
        resultHasStdout: boolean;
        resultHasStderr: boolean;
      }>(`(async () => {
        const delay = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
        const stdoutBytes = [];
        const stderrBytes = [];
        const until = async (predicate) => {
          const deadline = Date.now() + 5000;
          while (Date.now() < deadline) {
            if (predicate()) {
              return;
            }
            await delay(20);
          }
          throw new Error("Timed out waiting for executor callback");
        };
        const child = await window.muon.executor.spawn({
          ...${JSON.stringify(spawnOptions)},
          onStdout: (chunk) => stdoutBytes.push(...Array.from(chunk)),
          onStderr: (chunk) => stderrBytes.push(...Array.from(chunk)),
        });
        await until(() => stdoutBytes.length >= 2 && stderrBytes.length >= 2);
        const stdoutBytesBeforeInput = [...stdoutBytes];
        const stderrBytesBeforeInput = [...stderrBytes];
        await child.writeStdin("a");
        await child.writeStdin(new Uint8Array([98, 0, 99]));
        await child.closeStdin();
        const waitResult = await child.wait();
        await until(() => stderrBytes.length >= 6);
        return {
          processId: waitResult.processId,
          exitCode: waitResult.exitCode,
          stdoutBytesBeforeInput,
          stderrBytesBeforeInput,
          stdoutBytes,
          stderrBytes,
          resultHasStdout: waitResult.stdout !== undefined,
          resultHasStderr: waitResult.stderr !== undefined,
        };
      })()`);

      expect(values).toEqual({
        processId: expect.any(Number),
        exitCode: 7,
        stdoutBytesBeforeInput: [111, 49],
        stderrBytesBeforeInput: [101, 49],
        stdoutBytes: [111, 49, 115, 116, 100, 105, 110, 58, 97, 98, 0, 99],
        stderrBytes: [101, 49, 100, 111, 110, 101],
        resultHasStdout: false,
        resultHasStderr: false,
      });
      expect(values.processId).toBeGreaterThan(0);
    });
  });

  it("terminates a running child process through the executor handle", async () => {
    const marker = `muon-spawn-kill-${Date.now().toString(36)}`;
    const spawnOptions = createExecutorLongRunningSpawnOptions(marker);
    await withMuonEnvironment([], {}, async (driver) => {
      const values = await driver.evaluate<{
        processId: number;
        exitCode: number;
        asyncDisposeExposed: boolean;
        writeAfterCloseRejected: boolean;
        writeAfterReleaseRejected: boolean;
      }>(`(async () => {
        const child = await window.muon.executor.spawn(${JSON.stringify(spawnOptions)});
        await child.kill();
        const waitResult = await child.wait();
        let writeAfterCloseRejected = false;
        let writeAfterReleaseRejected = false;
        const closedChild = await window.muon.executor.spawn(${JSON.stringify(createExecutorStdinSpawnOptions())});
        const asyncDisposeExposed =
          typeof Symbol.asyncDispose !== "symbol" ||
          typeof closedChild[Symbol.asyncDispose] === "function";
        await closedChild.closeStdin();
        try {
          await closedChild.writeStdin("x");
        } catch {
          writeAfterCloseRejected = true;
        }
        await closedChild.release();
        try {
          await closedChild.writeStdin("x");
        } catch {
          writeAfterReleaseRejected = true;
        }
        return {
          processId: waitResult.processId,
          exitCode: waitResult.exitCode,
          asyncDisposeExposed,
          writeAfterCloseRejected,
          writeAfterReleaseRejected,
        };
      })()`);

      expect(values.processId).toBeGreaterThan(0);
      expect(values.exitCode).not.toBe(0);
      expect(values.asyncDisposeExposed).toBe(true);
      expect(values.writeAfterCloseRejected).toBe(true);
      expect(values.writeAfterReleaseRejected).toBe(true);
    });
  });

  linuxIt(
    "terminates executor children when the V8 context is released",
    async () => {
      const marker = `muon-spawn-context-release-${Date.now().toString(36)}`;
      const running = await startDebugMuon(
        [],
        TEST_NETWORK_ALLOW_PATTERNS,
        {},
        undefined,
        ["muon.executor.spawn"],
      );
      let driver: CdpDriver | undefined = undefined;
      try {
        driver = await connectToMuonCdp({
          port: MUON_PORT,
          timeoutMs: cdpCommandTimeoutMs,
        });
        await driver.navigate(
          "data:text/html,<title>muon executor context release</title>",
          cdpCommandTimeoutMs,
        );
        await driver.evaluate(`(async () => {
        globalThis.executorChild = await window.muon.executor.spawn(${JSON.stringify(
          createExecutorLongRunningSpawnOptions(marker),
        )});
      })()`);
        for (let index = 0; index < 50; index += 1) {
          const commandLines = await listProcessGroupCommandLines(
            running.process.pid ?? 0,
          );
          if (
            commandLines.some((commandLine) => commandLine.includes(marker))
          ) {
            break;
          }
          await delay(100);
        }
        expect(
          (await listProcessGroupCommandLines(running.process.pid ?? 0)).some(
            (commandLine) => commandLine.includes(marker),
          ),
        ).toBe(true);

        await driver.navigate(
          "data:text/html,<title>muon executor context released</title>",
          cdpCommandTimeoutMs,
        );
        for (let index = 0; index < 100; index += 1) {
          const commandLines = await listProcessGroupCommandLines(
            running.process.pid ?? 0,
          );
          if (
            !commandLines.some((commandLine) => commandLine.includes(marker))
          ) {
            return;
          }
          await delay(100);
        }
        const commandLines = await listProcessGroupCommandLines(
          running.process.pid ?? 0,
        );
        expect(
          commandLines.some((commandLine) => commandLine.includes(marker)),
        ).toBe(false);
      } catch (error) {
        throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
      } finally {
        await stopMuon(running, driver);
      }
    },
  );

  linuxIt(
    "loads and calls ad-hoc dynamic library functions without blocking the UI",
    async () => {
      const library = await buildAdhocTestLibrary();
      try {
        await withMuonEnvironment([], {}, async (driver) => {
          const values = await driver.evaluate<AdhocCallValues>(`(async () => {
            const delay = (ms) =>
              new Promise((resolve) => setTimeout(resolve, ms));
            const executor = window.muon.executor;
            let missingLibraryRejected = false;
            try {
              await executor.loadLibrary(${JSON.stringify(
                `${library.libraryPath}.missing`,
              )});
            } catch {
              missingLibraryRejected = true;
            }

            const loaded = await executor.loadLibrary(${JSON.stringify(
              library.libraryPath,
            )});
            try {
              const int32Signature = {
                argTypes: [executor.int32Type, executor.int32Type],
                returnType: executor.int32Type,
              };
              const add = await loaded.getFunction(
                "muon_adhoc_add",
                int32Signature,
              );
              const addResult = await add(20, 22);
              const markerAccepted = await (
                await loaded.getFunction("muon_adhoc_set_marker_path", {
                  argTypes: [executor.stringType],
                  returnType: executor.int32Type,
                })
              )(${JSON.stringify(library.markerPath)});
              let missingSymbolRejected = false;
              try {
                await loaded.getFunction(
                  "muon_adhoc_missing_symbol",
                  int32Signature,
                );
              } catch {
                missingSymbolRejected = true;
              }
              let invalidSignatureRejected = false;
              try {
                await loaded.getFunction("muon_adhoc_add", {
                  argTypes: [{ name: "not-a-native-type" }],
                  returnType: executor.int32Type,
                });
              } catch {
                invalidSignatureRejected = true;
              }

              const allocate = await loaded.getFunction("muon_adhoc_alloc", {
                argTypes: [executor.usizeType],
                returnType: executor.pointerType,
              });
              const releasePointer = await loaded.getFunction(
                "muon_adhoc_free",
                {
                  argTypes: [executor.pointerType],
                  returnType: executor.voidType,
                },
              );
              const pointer = await allocate(64n);
              const pointerNonZero = pointer.toString() !== "0";
              await releasePointer(pointer);

              const sleepThenAdd = await loaded.getFunction(
                "muon_adhoc_sleep_then_add",
                {
                  argTypes: [
                    executor.int32Type,
                    executor.int32Type,
                    executor.uint32Type,
                  ],
                  returnType: executor.int32Type,
                },
              );
              let ticks = 0;
              const interval = setInterval(() => {
                ticks += 1;
              }, 10);
              const startedAt = performance.now();
              const sleepPromise = sleepThenAdd(30, 12, 1000);
              await delay(100);
              const responsivenessElapsed = performance.now() - startedAt;
              const ticksDuringCall = ticks;
              const releasePromise = loaded.release();
              let releaseSettled = false;
              const releaseWatcher = (async () => {
                await releasePromise;
                releaseSettled = true;
              })();
              await delay(100);
              const releaseSettledBeforeCall = releaseSettled;
              const sleepResult = await sleepPromise;
              await releaseWatcher;
              clearInterval(interval);

              let callAfterReleaseRejected = false;
              try {
                await add(1, 2);
              } catch {
                callAfterReleaseRejected = true;
              }

              return {
                addResult,
                callAfterReleaseRejected,
                invalidSignatureRejected,
                markerAccepted,
                missingLibraryRejected,
                missingSymbolRejected,
                pointerNonZero,
                releaseSettledBeforeCall,
                responsivenessElapsed,
                sleepResult,
                ticksDuringCall,
              };
            } catch (error) {
              try {
                await loaded.release();
              } catch {}
              throw error;
            }
          })()`);

          expectAdhocCallValues(values);
        });
        const marker = await waitForTextFileContent(
          library.markerPath,
          (content) => content.includes("unloaded"),
          "ad-hoc library release marker",
        );
        expect(marker).toContain("unloaded");
      } finally {
        await rm(library.directory, { recursive: true, force: true });
      }
    },
  );

  linuxIt(
    "unloads ad-hoc dynamic libraries when the V8 context is released",
    async () => {
      const library = await buildAdhocTestLibrary();
      const running = await startDebugMuon(
        [],
        TEST_NETWORK_ALLOW_PATTERNS,
        {},
        undefined,
        ["muon.executor.loadLibrary"],
      );
      let driver: CdpDriver | undefined = undefined;
      try {
        driver = await connectToMuonCdp({
          port: MUON_PORT,
          timeoutMs: cdpCommandTimeoutMs,
        });
        await driver.navigate(
          "data:text/html,<title>muon adhoc context release</title>",
          cdpCommandTimeoutMs,
        );
        await expect(
          driver.evaluate(`(async () => {
            const executor = window.muon.executor;
            const library = await executor.loadLibrary(${JSON.stringify(
              library.libraryPath,
            )});
            const setMarkerPath = await library.getFunction(
              "muon_adhoc_set_marker_path",
              {
                argTypes: [executor.stringType],
                returnType: executor.int32Type,
              },
            );
            const accepted = await setMarkerPath(${JSON.stringify(
              library.contextMarkerPath,
            )});
            globalThis.__muonAdhocContextReleaseLibrary = library;
            return {
              accepted,
              keys: Object.keys(executor).sort(),
            };
          })()`),
        ).resolves.toEqual({
          accepted: 1,
          keys: [
            "boolType",
            "bufferViewType",
            "float32Type",
            "float64Type",
            "int16Type",
            "int32Type",
            "int64Type",
            "int8Type",
            "loadLibrary",
            "pointerType",
            "stringType",
            "uint16Type",
            "uint32Type",
            "uint64Type",
            "uint8Type",
            "usizeType",
            "voidType",
          ],
        });

        await driver.navigate(
          "data:text/html,<title>muon adhoc context released</title>",
          cdpCommandTimeoutMs,
        );
        const marker = await waitForTextFileContent(
          library.contextMarkerPath,
          (content) => content.includes("unloaded"),
          "ad-hoc library context release marker",
        );
        expect(marker).toContain("unloaded");
      } catch (error) {
        throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
      } finally {
        await stopMuon(running, driver);
        await rm(library.directory, { recursive: true, force: true });
      }
    },
  );

  windowsIt(
    "loads and calls Windows ad-hoc dynamic library functions without blocking the UI",
    async () => {
      const library = await buildWindowsAdhocTestLibrary();
      try {
        await withMuonEnvironment([], {}, async (driver) => {
          const values = await driver.evaluate<AdhocCallValues>(`(async () => {
            const delay = (ms) =>
              new Promise((resolve) => setTimeout(resolve, ms));
            const executor = window.muon.executor;
            let missingLibraryRejected = false;
            try {
              await executor.loadLibrary(${JSON.stringify(
                `${library.libraryPath}.missing`,
              )});
            } catch {
              missingLibraryRejected = true;
            }

            const loaded = await executor.loadLibrary(${JSON.stringify(
              library.libraryPath,
            )});
            try {
              const int32Signature = {
                argTypes: [executor.int32Type, executor.int32Type],
                returnType: executor.int32Type,
              };
              const add = await loaded.getFunction(
                "muon_adhoc_add",
                int32Signature,
              );
              const addResult = await add(20, 22);
              const markerAccepted = await (
                await loaded.getFunction("muon_adhoc_set_marker_path", {
                  argTypes: [executor.stringType],
                  returnType: executor.int32Type,
                })
              )(${JSON.stringify(library.markerPath)});
              let missingSymbolRejected = false;
              try {
                await loaded.getFunction(
                  "muon_adhoc_missing_symbol",
                  int32Signature,
                );
              } catch {
                missingSymbolRejected = true;
              }
              let invalidSignatureRejected = false;
              try {
                await loaded.getFunction("muon_adhoc_add", {
                  argTypes: [{ name: "not-a-native-type" }],
                  returnType: executor.int32Type,
                });
              } catch {
                invalidSignatureRejected = true;
              }

              const allocate = await loaded.getFunction("muon_adhoc_alloc", {
                argTypes: [executor.usizeType],
                returnType: executor.pointerType,
              });
              const releasePointer = await loaded.getFunction(
                "muon_adhoc_free",
                {
                  argTypes: [executor.pointerType],
                  returnType: executor.voidType,
                },
              );
              const pointer = await allocate(64n);
              const pointerNonZero = pointer.toString() !== "0";
              await releasePointer(pointer);

              const sleepThenAdd = await loaded.getFunction(
                "muon_adhoc_sleep_then_add",
                {
                  argTypes: [
                    executor.int32Type,
                    executor.int32Type,
                    executor.uint32Type,
                  ],
                  returnType: executor.int32Type,
                },
              );
              let ticks = 0;
              const interval = setInterval(() => {
                ticks += 1;
              }, 10);
              const startedAt = performance.now();
              const sleepPromise = sleepThenAdd(30, 12, 1000);
              await delay(100);
              const responsivenessElapsed = performance.now() - startedAt;
              const ticksDuringCall = ticks;
              const releasePromise = loaded.release();
              let releaseSettled = false;
              const releaseWatcher = (async () => {
                await releasePromise;
                releaseSettled = true;
              })();
              await delay(100);
              const releaseSettledBeforeCall = releaseSettled;
              const sleepResult = await sleepPromise;
              await releaseWatcher;
              clearInterval(interval);

              let callAfterReleaseRejected = false;
              try {
                await add(1, 2);
              } catch {
                callAfterReleaseRejected = true;
              }

              return {
                addResult,
                callAfterReleaseRejected,
                invalidSignatureRejected,
                markerAccepted,
                missingLibraryRejected,
                missingSymbolRejected,
                pointerNonZero,
                releaseSettledBeforeCall,
                responsivenessElapsed,
                sleepResult,
                ticksDuringCall,
              };
            } catch (error) {
              try {
                await loaded.release();
              } catch {}
              throw error;
            }
          })()`);

          expectAdhocCallValues(values);
        });
        const marker = await waitForTextFileContent(
          library.markerPath,
          (content) => content.includes("unloaded"),
          "Windows ad-hoc library release marker",
        );
        expect(marker).toContain("unloaded");
      } finally {
        await rm(library.directory, { recursive: true, force: true });
      }
    },
  );

  windowsIt(
    "unloads Windows ad-hoc dynamic libraries when the V8 context is released",
    async () => {
      const library = await buildWindowsAdhocTestLibrary();
      const running = await startDebugMuon(
        [],
        TEST_NETWORK_ALLOW_PATTERNS,
        {},
        undefined,
        ["muon.executor.loadLibrary"],
      );
      let driver: CdpDriver | undefined = undefined;
      try {
        driver = await connectToMuonCdp({
          port: MUON_PORT,
          timeoutMs: cdpCommandTimeoutMs,
        });
        await driver.navigate(
          "data:text/html,<title>muon adhoc windows context release</title>",
          cdpCommandTimeoutMs,
        );
        await expect(
          driver.evaluate(`(async () => {
            const executor = window.muon.executor;
            const library = await executor.loadLibrary(${JSON.stringify(
              library.libraryPath,
            )});
            const setMarkerPath = await library.getFunction(
              "muon_adhoc_set_marker_path",
              {
                argTypes: [executor.stringType],
                returnType: executor.int32Type,
              },
            );
            const accepted = await setMarkerPath(${JSON.stringify(
              library.contextMarkerPath,
            )});
            globalThis.__muonAdhocContextReleaseLibrary = library;
            return {
              accepted,
              keys: Object.keys(executor).sort(),
            };
          })()`),
        ).resolves.toEqual({
          accepted: 1,
          keys: [
            "boolType",
            "bufferViewType",
            "float32Type",
            "float64Type",
            "int16Type",
            "int32Type",
            "int64Type",
            "int8Type",
            "loadLibrary",
            "pointerType",
            "stringType",
            "uint16Type",
            "uint32Type",
            "uint64Type",
            "uint8Type",
            "usizeType",
            "voidType",
          ],
        });

        await driver.navigate(
          "data:text/html,<title>muon adhoc windows context released</title>",
          cdpCommandTimeoutMs,
        );
        const marker = await waitForTextFileContent(
          library.contextMarkerPath,
          (content) => content.includes("unloaded"),
          "Windows ad-hoc library context release marker",
        );
        expect(marker).toContain("unloaded");
      } catch (error) {
        throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
      } finally {
        await stopMuon(running, driver);
        await rm(library.directory, { recursive: true, force: true });
      }
    },
  );

  linuxIt("manages normal autostart through XDG autostart", async () => {
    const configHome = await mkdtemp(join(tmpdir(), "muon-xdg-config-"));
    const systemConfig = await mkdtemp(join(tmpdir(), "muon-xdg-system-"));
    try {
      await withMuonEnvironment(
        [],
        {
          XDG_CONFIG_HOME: configHome,
          XDG_CONFIG_DIRS: systemConfig,
        },
        async (driver) => {
          const values = await driver.evaluate<{
            initial: boolean | null;
            enabled: boolean | null;
            disabled: boolean | null;
          }>(`(async () => {
            const normalize = (value) => value === undefined ? null : value;
            const initial = normalize(
              await window.muon.environments.getAutostart(),
            );
            await window.muon.environments.setAutostart(true);
            const enabled = normalize(
              await window.muon.environments.getAutostart(),
            );
            await window.muon.environments.setAutostart(false);
            const disabled = normalize(
              await window.muon.environments.getAutostart(),
            );
            return { initial, enabled, disabled };
          })()`);

          expect(values).toEqual({
            initial: false,
            enabled: true,
            disabled: false,
          });
        },
      );
    } finally {
      await rm(configHome, { recursive: true, force: true });
      await rm(systemConfig, { recursive: true, force: true });
    }
  });

  it("reflects page titles to the native browser window", async () => {
    await withMuon([], async (driver) => {
      const title = "muon native title";
      await driver.navigate(
        `data:text/html,<title>${title}</title>`,
        cdpCommandTimeoutMs,
      );
      await waitForDocumentTitle(driver, title, cdpCommandTimeoutMs);
      await expect(
        driver.evaluate(
          `document.title = "muon changed title"; document.title`,
        ),
      ).resolves.toBe("muon changed title");
      await waitForNativeWindowTitle("muon changed title", cdpCommandTimeoutMs);
    });
  });

  it("reloads pages through the built-in browser API", async () => {
    await withMuon([], async (driver) => {
      await driver.navigate(
        "data:text/html,<title>muon browser reload</title>",
        cdpCommandTimeoutMs,
      );
      const reloadEvent = driver.waitForEvent(
        "Page.loadEventFired",
        cdpCommandTimeoutMs,
      );
      await expect(
        driver.evaluate(
          `setTimeout(() => window.muon.browser.reload(), 0); "requested"`,
        ),
      ).resolves.toBe("requested");
      await expect(reloadEvent).resolves.toBeDefined();

      const hardReloadEvent = driver.waitForEvent(
        "Page.loadEventFired",
        cdpCommandTimeoutMs,
      );
      await expect(
        driver.evaluate(
          `setTimeout(() => window.muon.browser.hardReload(), 0); "requested"`,
        ),
      ).resolves.toBe("requested");
      await expect(hardReloadEvent).resolves.toBeDefined();
    });
  });

  it("changes page zoom through the built-in browser API", async () => {
    await withMuon([], async (driver) => {
      await driver.navigate(
        "data:text/html,<title>muon browser zoom</title>",
        cdpCommandTimeoutMs,
      );
      const initialWidth = await driver.evaluate<number>("window.innerWidth");

      await expect(
        driver.evaluate("window.muon.browser.zoomIn()"),
      ).resolves.toBeUndefined();
      const zoomedInWidth = await waitForInnerWidth(
        driver,
        (value) => value < initialWidth,
        cdpCommandTimeoutMs,
      );

      await expect(
        driver.evaluate("window.muon.browser.zoomOut()"),
      ).resolves.toBeUndefined();
      await waitForInnerWidth(
        driver,
        (value) => Math.abs(value - initialWidth) <= 1,
        cdpCommandTimeoutMs,
      );

      await expect(
        driver.evaluate("window.muon.browser.zoomOut()"),
      ).resolves.toBeUndefined();
      await waitForInnerWidth(
        driver,
        (value) => value > initialWidth,
        cdpCommandTimeoutMs,
      );

      await expect(
        driver.evaluate("window.muon.browser.resetZoom()"),
      ).resolves.toBeUndefined();
      await waitForInnerWidth(
        driver,
        (value) =>
          Math.abs(value - initialWidth) <= 1 && value !== zoomedInWidth,
        cdpCommandTimeoutMs,
      );
    });
  });

  it("toggles fullscreen through the built-in browser API", async () => {
    await withMuon([], async (driver) => {
      const initialSize = await getOuterSize(driver);
      await expect(
        driver.evaluate("window.muon.browser.toggleFullscreen()"),
      ).resolves.toBeUndefined();
      await expect(
        waitForOuterSizeChange(driver, initialSize, cdpCommandTimeoutMs),
      ).resolves.toEqual(
        expect.objectContaining({
          width: expect.any(Number),
          height: expect.any(Number),
        }),
      );
    });
  });

  it("enters and exits fullscreen through explicit built-in browser APIs", async () => {
    await withMuon([], async (driver) => {
      const initialSize = await getOuterSize(driver);
      await expect(
        driver.evaluate("window.muon.browser.enterFullscreen()"),
      ).resolves.toBeUndefined();
      const fullscreenSize = await waitForOuterSizeChange(
        driver,
        initialSize,
        cdpCommandTimeoutMs,
      );
      if (isLocalLinuxE2e) {
        await waitForNativeWindowStates(
          "muon test",
          ["_NET_WM_STATE_FULLSCREEN"],
          cdpCommandTimeoutMs,
        );
      }

      await expect(
        driver.evaluate("window.muon.browser.exitFullscreen()"),
      ).resolves.toBeUndefined();
      await expect(
        waitForOuterSizeChange(driver, fullscreenSize, cdpCommandTimeoutMs),
      ).resolves.toEqual(
        expect.objectContaining({
          width: expect.any(Number),
          height: expect.any(Number),
        }),
      );
      if (isLocalLinuxE2e) {
        await waitForNativeWindowStatesAbsent(
          "muon test",
          ["_NET_WM_STATE_FULLSCREEN"],
          cdpCommandTimeoutMs,
        );
      }
    });
  });

  linuxIt(
    "applies configured maximized and fullscreen startup window states",
    async () => {
      await withMuonInitialWindowState("maximized", async (driver) => {
        await expect(driver.evaluate("document.title")).resolves.toBe(
          "muon test",
        );
        await waitForNativeWindowStates(
          "muon test",
          ["_NET_WM_STATE_MAXIMIZED_HORZ", "_NET_WM_STATE_MAXIMIZED_VERT"],
          cdpCommandTimeoutMs,
        );
      });
      await withMuonInitialWindowState("fullscreen", async (driver) => {
        await expect(driver.evaluate("document.title")).resolves.toBe(
          "muon test",
        );
        await waitForNativeWindowStates(
          "muon test",
          ["_NET_WM_STATE_FULLSCREEN"],
          cdpCommandTimeoutMs,
        );
      });
    },
  );

  it("keeps hidden and minimized startup windows controllable from browser APIs", async () => {
    await withMuonInitialWindowState("hidden", async (driver) => {
      await expect(driver.evaluate("document.title")).resolves.toBe(
        "muon test",
      );
      if (isLocalLinuxE2e) {
        await waitForNativeWindowTitleAbsent("muon test", cdpCommandTimeoutMs);
      }

      await expect(
        driver.evaluate("window.muon.browser.show()"),
      ).resolves.toBeUndefined();
      if (isLocalLinuxE2e) {
        await expect(
          waitForNativeWindowTitle("muon test", cdpCommandTimeoutMs),
        ).resolves.toBe(true);
        await waitForNativeWindowStatesAbsent(
          "muon test",
          ["_NET_WM_STATE_HIDDEN", "_NET_WM_STATE_MODAL"],
          cdpCommandTimeoutMs,
        );
      }
    });

    await withMuonInitialWindowState("hidden", async (driver) => {
      await expect(driver.evaluate("document.title")).resolves.toBe(
        "muon test",
      );
      await expect(
        driver.evaluate("window.muon.browser.focus()"),
      ).resolves.toBeUndefined();
      if (isLocalLinuxE2e) {
        await expect(
          waitForNativeWindowTitle("muon test", cdpCommandTimeoutMs),
        ).resolves.toBe(true);
        await waitForNativeActiveWindowTitle("muon test", cdpCommandTimeoutMs);
      }
    });

    await withMuonInitialWindowState("minimized", async (driver) => {
      await expect(
        driver.evaluate("window.muon.browser.restore()"),
      ).resolves.toBeUndefined();
      await expect(driver.evaluate("document.title")).resolves.toBe(
        "muon test",
      );
    });
  });

  linuxIt(
    "creates a tray and receives tray events from a hidden startup window",
    async () => {
      const directory = await mkdtemp(join(tmpdir(), "muon-hidden-tray-"));
      const assetRoot = await createTrayAssetRoot(directory);
      try {
        await withMuonInitialWindowState(
          "hidden",
          async (driver) => {
            await expect(driver.evaluate("document.title")).resolves.toBe(
              "muon test",
            );
            await waitForNativeWindowTitleAbsent(
              "muon test",
              cdpCommandTimeoutMs,
            );
            const traySetup = await driver.evaluate<
              | { ok: true; trayId: string }
              | { message: string; ok: false; stack: string }
            >(`(async () => {
              try {
                globalThis.__muonHiddenTrayEvents = [];
                const handler = (event) => {
                  globalThis.__muonHiddenTrayEvents.push(event);
                };
                const trayId = await window.muon.browser.createTray(
                  {
                    id: "hidden-tray",
                    icon: "icons/tray.png",
                    tooltip: "Hidden tray",
                    menu: [{ id: "open", label: "Open" }],
                  },
                  handler,
                );
                await window.muon.browser.setTrayMenu(
                  trayId,
                  [
                    { id: "open", label: "Open" },
                    {
                      type: "checkbox",
                      id: "ready",
                      label: "Ready",
                      checked: false,
                    },
                  ],
                  handler,
                );
                await window.muon.browser.setTrayIcon(trayId, "icons/tray.png");
                await window.muon.browser.setTrayTooltip(
                  trayId,
                  "Hidden tray updated",
                );
                globalThis.__muonHiddenTrayId = trayId;
                return { ok: true, trayId };
              } catch (error) {
                return {
                  message: String(error?.message ?? error),
                  ok: false,
                  stack: String(error?.stack ?? ""),
                };
              }
            })()`);
            expect(traySetup).toEqual({ ok: true, trayId: "hidden-tray" });

            const busName = await findStatusNotifierItemBusName();
            await callTrayDbusMethod(
              busName,
              "/StatusNotifierItem",
              "org.freedesktop.StatusNotifierItem.Activate",
              ["12", "34"],
            );
            await expect(
              waitForBrowserTrayEvent(driver, "activate"),
            ).resolves.toEqual(
              expect.objectContaining({
                trayId: "hidden-tray",
                type: "activate",
                x: 12,
                y: 34,
              }),
            );

            await callTrayDbusMethod(
              busName,
              "/StatusNotifierItem/Menu",
              "com.canonical.dbusmenu.Event",
              ["2", "clicked", "<''>", "0"],
            );
            await expect(
              waitForBrowserTrayEvent(driver, "menu"),
            ).resolves.toEqual(
              expect.objectContaining({
                checked: true,
                id: "ready",
                trayId: "hidden-tray",
                type: "menu",
              }),
            );

            await expect(
              driver.evaluate(
                "window.muon.browser.removeTray(globalThis.__muonHiddenTrayId)",
              ),
            ).resolves.toBeUndefined();
          },
          assetRoot,
        );
      } finally {
        await rm(directory, { recursive: true, force: true });
      }
    },
  );

  windowsIt(
    "creates Windows trays and receives tray events from a hidden startup window",
    async () => {
      const directory = await mkdtemp(join(tmpdir(), "muon-windows-tray-"));
      const assetRoot = await createTrayAssetRoot(directory);
      try {
        await withMuonInitialWindowState(
          "hidden",
          async (driver) => {
            await expect(driver.evaluate("document.title")).resolves.toBe(
              "muon test",
            );
            const traySetup = await driver.evaluate<
              | { followTrayId: string; ok: true; trayId: string }
              | { message: string; ok: false; stack: string }
            >(`(async () => {
              try {
                globalThis.__muonHiddenTrayEvents = [];
                const handler = (event) => {
                  globalThis.__muonHiddenTrayEvents.push(event);
                };
                await window.muon.browser.setTitleBarIcon("icons/title.png");
                const trayId = await window.muon.browser.createTray(
                  {
                    id: "windows-tray",
                    icon: "icons/tray.png",
                    tooltip: "Windows tray",
                    menu: [{ id: "open", label: "Open" }],
                  },
                  handler,
                );
                const followTrayId = await window.muon.browser.createTray(
                  {
                    id: "windows-follow-tray",
                    tooltip: "Follow tray",
                  },
                  handler,
                );
                await window.muon.browser.setTrayMenu(
                  trayId,
                  [
                    {
                      type: "checkbox",
                      id: "ready",
                      label: "Ready",
                      checked: false,
                    },
                    { id: "open", label: "Open" },
                  ],
                  handler,
                );
                await window.muon.browser.setTrayIcon(
                  trayId,
                  "icons/tray-alt.png",
                );
                await window.muon.browser.setTrayTooltip(
                  trayId,
                  "Windows tray updated",
                );
                await window.muon.browser.setTitleBarIcon("icons/tray-alt.png");
                globalThis.__muonHiddenTrayId = trayId;
                globalThis.__muonWindowsFollowTrayId = followTrayId;
                return { followTrayId, ok: true, trayId };
              } catch (error) {
                return {
                  message: String(error?.message ?? error),
                  ok: false,
                  stack: String(error?.stack ?? ""),
                };
              }
            })()`);
            expect(traySetup).toEqual({
              followTrayId: "windows-follow-tray",
              ok: true,
              trayId: "windows-tray",
            });

            await sendWindowsTrayCallback(
              directory,
              1,
              windowsTrayPrimaryActivateMessage,
            );
            await expect(
              waitForBrowserTrayEvent(driver, "activate", "windows-tray"),
            ).resolves.toEqual(
              expect.objectContaining({
                trayId: "windows-tray",
                type: "activate",
              }),
            );

            await sendWindowsTrayCallback(
              directory,
              1,
              windowsTraySecondaryActivateMessage,
            );
            await expect(
              waitForBrowserTrayEvent(
                driver,
                "secondaryActivate",
                "windows-tray",
              ),
            ).resolves.toEqual(
              expect.objectContaining({
                trayId: "windows-tray",
                type: "secondaryActivate",
              }),
            );

            await sendWindowsTrayCallback(
              directory,
              1,
              windowsTrayContextMenuMessage,
              true,
            );
            await expect(
              waitForBrowserTrayEvent(driver, "menu", "windows-tray"),
            ).resolves.toEqual(
              expect.objectContaining({
                checked: true,
                id: "ready",
                trayId: "windows-tray",
                type: "menu",
              }),
            );

            await sendWindowsTrayCallback(
              directory,
              2,
              windowsTrayPrimaryActivateMessage,
            );
            await expect(
              waitForBrowserTrayEvent(
                driver,
                "activate",
                "windows-follow-tray",
              ),
            ).resolves.toEqual(
              expect.objectContaining({
                trayId: "windows-follow-tray",
                type: "activate",
              }),
            );

            const eventCountBeforeRemoval = await driver.evaluate<number>(
              "(globalThis.__muonHiddenTrayEvents ?? []).length",
            );
            await expect(
              driver.evaluate(
                "window.muon.browser.removeTray(globalThis.__muonHiddenTrayId)",
              ),
            ).resolves.toBeUndefined();
            await sendWindowsTrayCallback(
              directory,
              1,
              windowsTrayPrimaryActivateMessage,
            );
            await delay(250);
            await expect(
              driver.evaluate<number>(
                "(globalThis.__muonHiddenTrayEvents ?? []).length",
              ),
            ).resolves.toBe(eventCountBeforeRemoval);
          },
          assetRoot,
        );
      } finally {
        await rm(directory, { recursive: true, force: true });
      }
    },
  );

  it("maximizes and restores through the built-in browser API", async () => {
    await withMuon([], async (driver) => {
      const initialSize = await getOuterSize(driver);
      await expect(
        driver.evaluate("window.muon.browser.maximize()"),
      ).resolves.toBeUndefined();
      const maximizedSize = await waitForOuterSizeChange(
        driver,
        initialSize,
        cdpCommandTimeoutMs,
      );

      await expect(
        driver.evaluate("window.muon.browser.restore()"),
      ).resolves.toBeUndefined();
      await expect(
        waitForOuterSizeChange(driver, maximizedSize, cdpCommandTimeoutMs),
      ).resolves.toEqual(
        expect.objectContaining({
          width: expect.any(Number),
          height: expect.any(Number),
        }),
      );
    });
  });

  it("reads top-level window bounds through the built-in browser API", async () => {
    await withMuon([], async (driver) => {
      const bounds = await getWindowBounds(driver);
      const viewportSize = await driver.evaluate<{
        width: number;
        height: number;
      }>("({ width: window.innerWidth, height: window.innerHeight })");

      expectWindowBoundsShape(bounds);
      expect(bounds.width).toBeGreaterThanOrEqual(viewportSize.width);
      expect(bounds.height).toBeGreaterThan(viewportSize.height);
    });
  });

  it("sets top-level window bounds through the built-in browser API", async () => {
    await withMuon([], async (driver) => {
      const initialBounds = await getWindowBounds(driver);
      const targetBounds: BrowserWindowBounds = {
        x: initialBounds.x + 24,
        y: initialBounds.y + 24,
        width: Math.max(640, initialBounds.width - 120),
        height: Math.max(520, initialBounds.height - 120),
      };

      await expect(
        driver.evaluate(
          `window.muon.browser.setWindowBounds(${JSON.stringify(targetBounds)})`,
        ),
      ).resolves.toBeUndefined();
      const updatedBounds = await waitForWindowBounds(
        driver,
        targetBounds,
        cdpCommandTimeoutMs,
      );
      expectWindowBoundsShape(updatedBounds);
    });
  });

  it("rejects invalid top-level window bounds", async () => {
    await withMuon([], async (driver) => {
      const calls = [
        "window.muon.browser.setWindowBounds(null)",
        "window.muon.browser.setWindowBounds({ x: 0, y: 0, width: 0, height: 100 })",
        "window.muon.browser.setWindowBounds({ x: 0, y: 0, width: 100, height: -1 })",
        "window.muon.browser.setWindowBounds({ x: 0.5, y: 0, width: 100, height: 100 })",
        "window.muon.browser.setWindowBounds({ x: 2147483648, y: 0, width: 100, height: 100 })",
      ];

      for (const call of calls) {
        await expect(evaluateRejection(driver, call)).resolves.toContain(
          "Invalid window bounds",
        );
      }
    });
  });

  it("runs basic native window operations through the built-in browser API", async () => {
    await withMuon([], async (driver) => {
      await expect(
        driver.evaluate(`(async () => {
          await window.muon.browser.hide();
          await window.muon.browser.show();
          await window.muon.browser.focus();
          await window.muon.browser.blur();
          await window.muon.browser.minimize();
          await window.muon.browser.restore();
          return document.title;
        })()`),
      ).resolves.toBe("muon test");
    });
  });

  it("closes the main browser through the built-in browser API", async () => {
    const running = await startDebugMuon([]);
    let driver: CdpDriver | undefined = undefined;
    try {
      driver = await connectToMuonCdp({
        port: MUON_PORT,
        timeoutMs: cdpCommandTimeoutMs,
      });
      await driver.navigate(
        "data:text/html,<title>muon browser close</title>",
        cdpCommandTimeoutMs,
      );
      driver = await requestBrowserClose(driver);
      driver?.close();
      driver = undefined;
      await expect(
        waitForProcessExit(running, processExitTimeoutMs),
      ).resolves.toBeUndefined();
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
    } finally {
      await stopMuon(running, driver);
    }
  });

  it("keeps popup windows open after closing the main browser", async () => {
    const running = await startDebugMuon([]);
    let driver: CdpDriver | undefined = undefined;
    let popupDriver: CdpDriver | undefined = undefined;
    try {
      driver = await connectToMuonCdp({
        port: MUON_PORT,
        timeoutMs: cdpCommandTimeoutMs,
      });
      const mainTarget = await getMainPageTarget();
      const popupTarget = await openPopupTarget(
        driver,
        MUON_APP_URL,
        "noopener",
        "muonCloseNoopenerPopup",
      );
      popupDriver = await connectToMuonCdp({
        port: MUON_PORT,
        targetId: popupTarget.id,
        timeoutMs: cdpCommandTimeoutMs,
      });

      const initialPopupHref = await evaluateWithWindowsCdpReconnect<string>(
        popupDriver,
        popupTarget.id,
        "document.location.href",
      );
      popupDriver = initialPopupHref.driver;
      expect(initialPopupHref.value).toBe(MUON_APP_URL);
      driver = await requestBrowserClose(driver);
      driver?.close();
      driver = undefined;

      await waitForTargetClosed(mainTarget.id, targetTimeoutMs);
      await expect(waitForProcessExitOrTimeout(running, 1000)).resolves.toBe(
        false,
      );
      const popupHref = await evaluateWithWindowsCdpReconnect<string>(
        popupDriver,
        popupTarget.id,
        "document.location.href",
      );
      popupDriver = popupHref.driver;
      expect(popupHref.value).toBe(MUON_APP_URL);
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
    } finally {
      if (driver !== undefined && popupDriver !== undefined) {
        driver.close();
        driver = undefined;
      }
      await stopMuon(running, popupDriver ?? driver);
    }
  });

  it("keeps opener popup windows open after closing the main browser", async () => {
    const running = await startDebugMuon(
      [],
      TEST_NETWORK_ALLOW_PATTERNS,
      {},
      undefined,
      TEST_PLUGIN_ALLOW_PATTERNS,
      [],
      TEST_BROWSER_PLUGIN_ALLOW_PATTERNS,
      [],
      ["asset://main/**"],
    );
    let driver: CdpDriver | undefined = undefined;
    let popupDriver: CdpDriver | undefined = undefined;
    try {
      driver = await connectToMuonCdp({
        port: MUON_PORT,
        timeoutMs: cdpCommandTimeoutMs,
      });
      const mainTarget = await getMainPageTarget();
      const popupTarget = await openPopupTarget(
        driver,
        MUON_APP_URL,
        "",
        "muonCloseOpenerPopup",
      );
      popupDriver = await connectToMuonCdp({
        port: MUON_PORT,
        targetId: popupTarget.id,
        timeoutMs: cdpCommandTimeoutMs,
      });

      const openerExists = await evaluateWithWindowsCdpReconnect<boolean>(
        popupDriver,
        popupTarget.id,
        "window.opener !== null",
      );
      popupDriver = openerExists.driver;
      expect(openerExists.value).toBe(true);
      driver = await requestBrowserClose(driver);
      driver?.close();
      driver = undefined;

      await waitForTargetClosed(mainTarget.id, targetTimeoutMs);
      await expect(waitForProcessExitOrTimeout(running, 1000)).resolves.toBe(
        false,
      );
      const openerState = await evaluateWithWindowsCdpReconnect<string>(
        popupDriver,
        popupTarget.id,
        `(() => {
          if (window.opener === null) {
            return "null";
          }
          return window.opener.closed ? "closed" : "open";
        })()`,
      );
      popupDriver = openerState.driver;
      expect(openerState.value).toMatch(/^(null|closed)$/u);
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
    } finally {
      if (driver !== undefined && popupDriver !== undefined) {
        driver.close();
        driver = undefined;
      }
      await stopMuon(running, popupDriver ?? driver);
    }
  });

  it("keeps the main browser open after closing a popup browser", async () => {
    const running = await startDebugMuon([]);
    let driver: CdpDriver | undefined = undefined;
    let popupDriver: CdpDriver | undefined = undefined;
    try {
      driver = await connectToMuonCdp({
        port: MUON_PORT,
        timeoutMs: cdpCommandTimeoutMs,
      });
      await expect(driver.evaluate("document.location.href")).resolves.toBe(
        MUON_APP_URL,
      );
      const mainTarget = await getMainPageTarget();
      await driver.evaluate(`document.title = "muon main remains open";`);
      const popupTarget = await openPopupTarget(
        driver,
        MUON_APP_URL,
        "noopener",
        "muonClosePopupOnly",
      );
      popupDriver = await connectToMuonCdp({
        port: MUON_PORT,
        targetId: popupTarget.id,
        timeoutMs: cdpCommandTimeoutMs,
      });

      const popupCloseDriver = await requestBrowserClose(popupDriver);
      popupCloseDriver?.close();
      popupDriver = undefined;

      await waitForTargetClosed(popupTarget.id, targetTimeoutMs);
      await expect(waitForProcessExitOrTimeout(running, 1000)).resolves.toBe(
        false,
      );
      const mainTitle = await readMainDocumentTitle(driver, mainTarget.id);
      driver = mainTitle.driver;
      expect(mainTitle.title).toBe("muon main remains open");
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
    } finally {
      await stopMuon(running, popupDriver ?? driver);
    }
  });

  it("shuts down the process with the default exit code", async () => {
    const running = await startDebugMuon([]);
    let driver: CdpDriver | undefined = undefined;
    try {
      driver = await connectToMuonCdp({
        port: MUON_PORT,
        timeoutMs: cdpCommandTimeoutMs,
      });
      await driver.navigate(
        "data:text/html,<title>muon browser shutdown default</title>",
        cdpCommandTimeoutMs,
      );
      await expect(
        driver.evaluate(`(() => {
          setTimeout(() => {
            window.muon.browser.shutdown();
          }, 50);
          return "scheduled";
        })()`),
      ).resolves.toBe("scheduled");
      driver.close();
      driver = undefined;
      await expect(
        waitForProcessExit(running, processExitTimeoutMs),
      ).resolves.toBeUndefined();
      // Agent-rover can report a clean Windows remote exit as null when the
      // native exit code is 0; non-zero shutdown codes are asserted below.
      expect(
        isWindowsRemoteE2e()
          ? [0, null].includes(running.process.exitCode)
          : running.process.exitCode === 0,
      ).toBe(true);
      expect(running.process.signalCode).toBeNull();
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
    } finally {
      await stopMuon(running, driver);
    }
  });

  it("shuts down the process with the requested exit code", async () => {
    const running = await startDebugMuon([]);
    let driver: CdpDriver | undefined = undefined;
    try {
      driver = await connectToMuonCdp({
        port: MUON_PORT,
        timeoutMs: cdpCommandTimeoutMs,
      });
      await driver.navigate(
        "data:text/html,<title>muon browser shutdown exit code</title>",
        cdpCommandTimeoutMs,
      );
      await expect(
        driver.evaluate(`(() => {
          setTimeout(() => {
            window.muon.browser.shutdown(7);
          }, 50);
          return "scheduled";
        })()`),
      ).resolves.toBe("scheduled");
      driver.close();
      driver = undefined;
      await expect(
        waitForProcessExit(running, processExitTimeoutMs),
      ).resolves.toBeUndefined();
      await expectProcessExitCode(running, 7);
      expect(running.process.signalCode).toBeNull();
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
    } finally {
      await stopMuon(running, driver);
    }
  });

  it("rejects the reserved recycle exit code for shutdown", async () => {
    await withMuon([], async (driver) => {
      await expect(
        evaluateRejection(driver, "window.muon.browser.shutdown(88)"),
      ).resolves.toContain("Invalid shutdown exit code");
    });
  });

  it("recycles the process through the built-in browser API from launcher", async () => {
    const running = await startDebugMuonLauncher([]);
    let driver: CdpDriver | undefined = undefined;
    try {
      driver = await connectToMuonCdp({
        port: MUON_PORT,
        timeoutMs: cdpCommandTimeoutMs,
      });
      await driver.navigate(
        "data:text/html,<title>muon browser recycle api</title>",
        cdpCommandTimeoutMs,
      );
      const firstProcessId = await driver.evaluate<number>(
        "window.muon.environments.getProcessId()",
      );
      await expect(
        driver.evaluate(`(() => {
          setTimeout(() => {
            window.muon.browser.recycle();
          }, 50);
          return "scheduled";
        })()`),
      ).resolves.toBe("scheduled");
      driver.close();
      driver = undefined;
      const recycled = await waitForRecycledMuon(firstProcessId);
      driver = recycled.driver;
      expect(recycled.processId).not.toBe(firstProcessId);
      await expect(driver.evaluate("document.location.href")).resolves.toBe(
        MUON_APP_URL,
      );
      expect(running.process.exitCode).toBeNull();
      expect(running.process.signalCode).toBeNull();
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
    } finally {
      await stopMuon(running, driver);
    }
  });

  it("shuts down the process when multiple browser windows are open", async () => {
    const running = await startDebugMuon(
      [],
      TEST_NETWORK_ALLOW_PATTERNS,
      {},
      undefined,
      TEST_PLUGIN_ALLOW_PATTERNS,
      [],
      TEST_BROWSER_PLUGIN_ALLOW_PATTERNS,
      [],
      ["asset://main/**"],
    );
    let driver: CdpDriver | undefined = undefined;
    try {
      driver = await connectToMuonCdp({
        port: MUON_PORT,
        timeoutMs: cdpCommandTimeoutMs,
      });
      await expect(driver.evaluate("document.location.href")).resolves.toBe(
        MUON_APP_URL,
      );
      const popupTarget = await openPopupTarget(driver, MUON_APP_URL);
      expect(popupTarget.type).toBe("page");
      await expect(
        driver.evaluate(`(() => {
          setTimeout(() => {
            window.muon.browser.shutdown(9);
          }, 50);
          return "scheduled";
        })()`),
      ).resolves.toBe("scheduled");
      driver.close();
      driver = undefined;
      await expect(
        waitForProcessExit(running, processExitTimeoutMs),
      ).resolves.toBeUndefined();
      // The Windows remote process snapshot can lose a non-zero exit code
      // after all windows close together; the exact exit code remains covered
      // by the single-window shutdown test above.
      expect(
        isWindowsRemoteE2e()
          ? [9, null].includes(running.process.exitCode)
          : running.process.exitCode === 9,
      ).toBe(true);
      expect(running.process.signalCode).toBeNull();
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
    } finally {
      await stopMuon(running, driver);
    }
  });

  it("shuts down the process when DevTools is open", async () => {
    const running = await startDebugMuon(
      [],
      TEST_NETWORK_ALLOW_PATTERNS,
      {},
      createBrowserShortcutConfig({ devtools: "f12" }),
    );
    let driver: CdpDriver | undefined = undefined;
    try {
      driver = await connectToMuonCdp({
        port: MUON_PORT,
        timeoutMs: cdpCommandTimeoutMs,
      });
      await driver.navigate(
        "data:text/html,<title>muon browser shutdown devtools</title>",
        cdpCommandTimeoutMs,
      );
      const devToolsTarget = await dispatchDevToolsShortcut(
        driver,
        f12DevToolsShortcut.event,
      );
      expect(devToolsTarget.url).toContain("devtools://");
      await expect(
        driver.evaluate(`(() => {
          setTimeout(() => {
            window.muon.browser.shutdown(123);
          }, 50);
          return "scheduled";
        })()`),
      ).resolves.toBe("scheduled");
      driver.close();
      driver = undefined;
      await expect(
        waitForProcessExit(running, processExitTimeoutMs),
      ).resolves.toBeUndefined();
      // The Windows remote process snapshot can lose a non-zero exit code
      // after DevTools closes with its opener; the exact exit code remains
      // covered by the single-window shutdown test above.
      expect(
        isWindowsRemoteE2e()
          ? [123, null].includes(running.process.exitCode)
          : running.process.exitCode === 123,
      ).toBe(true);
      expect(running.process.signalCode).toBeNull();
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
    } finally {
      await stopMuon(running, driver);
    }
  });

  it("reads and writes files through the built-in filesystem API", async () => {
    const runFilesystemStep = async (
      name: string,
      run: () => Promise<void>,
    ): Promise<void> => {
      try {
        await run();
      } catch (error) {
        throw new Error(`${name}: ${String(error)}`);
      }
    };

    const directory = await mkdtemp(join(tmpdir(), "muon-fs-"));
    try {
      await withMuon([], async (driver) => {
        await runFilesystemStep("function wrapper lifecycle", async () => {
          await runFunctionWrapperAbortLeaseScenarios(driver, directory);
        });
        await runFilesystemStep("roundtrip", async () => {
          await runBuiltinFsRoundtrip(driver, directory);
        });
        await runFilesystemStep("additional operations", async () => {
          await runBuiltinFsAdditionalOperations(driver, directory);
        });
        await runFilesystemStep("file URI operations", async () => {
          await runBuiltinFsFileUriOperations(driver, directory);
        });
        await runFilesystemStep("abort scenarios", async () => {
          await runBuiltinFsAbortScenarios(driver, directory);
        });
        await runFilesystemStep("dialog validation", async () => {
          await runBuiltinFsDialogValidation(driver);
        });
      });
    } finally {
      await rm(directory, { recursive: true, force: true });
    }
  });

  it("enforces the configured readFile byte limit", async () => {
    const directory = await mkdtemp(join(tmpdir(), "muon-fs-limit-"));
    const sourcePath = join(directory, "source.bin");
    const missingPath = join(directory, "missing.bin");
    const source = Array.from({ length: 17 }, (_value, index) => index);
    await writeFile(sourcePath, Buffer.from(source));
    try {
      await withMuon(
        [],
        async (driver) => {
          const results = await driver.evaluate<
            Array<{
              explicitError: string;
              implicitError: string;
              tail: number[];
            }>
          >(`(async () => {
            const captureError = async (operation) => {
              try {
                await operation();
                return "";
              } catch (error) {
                return String(error?.message ?? error);
              }
            };
            const targets = ${JSON.stringify([
              sourcePath,
              pathToFileUrlHref(sourcePath),
            ])};
            const entries = [];
            for (const target of targets) {
              const explicitError = await captureError(() =>
                window.muon.fs.readFile(target, { length: 17 }),
              );
              const implicitError = await captureError(() =>
                window.muon.fs.readFile(target),
              );
              const tail = Array.from(
                new Uint8Array(
                  await window.muon.fs.readFile(target, {
                    position: 1,
                    length: 16,
                  }),
                ),
              );
              entries.push({ explicitError, implicitError, tail });
            }
            return entries;
          })()`);
          for (const result of results) {
            expect(result.explicitError).toContain(
              "readFile length exceeds configured maximum",
            );
            expect(result.implicitError).toContain(
              "readFile result exceeds configured maximum",
            );
            expect(result.tail).toEqual(source.slice(1));
          }

          await expect(
            driver.evaluate(`(async () => {
              const result = await window.muon.fs.readFile(
                ${JSON.stringify(missingPath)},
                { length: 0 },
              );
              return result.byteLength;
            })()`),
          ).resolves.toBe(0);
        },
        { internal: { "fs.readFile.maxBytes": "16" } },
      );
    } finally {
      await rm(directory, { recursive: true, force: true });
    }
  });

  it("rejects an invalid readFile byte limit during startup", async () => {
    await expectDebugMuonStartupFailure(
      [],
      "fs.readFile.maxBytes must be an unsigned decimal byte count",
      {
        internal: { "fs.readFile.maxBytes": "16MiB" },
      },
    );
  });

  it("enforces the configured readTextFile byte limit", async () => {
    const directory = await mkdtemp(join(tmpdir(), "muon-fs-text-limit-"));
    const atLimitPath = join(directory, "at-limit.txt");
    const overLimitPath = join(directory, "over-limit.txt");
    const invalidAfterLimitPath = join(directory, "invalid-after-limit.txt");
    await writeFile(atLimitPath, Buffer.alloc(16, 0x61));
    await writeFile(overLimitPath, Buffer.alloc(17, 0x61));
    await writeFile(
      invalidAfterLimitPath,
      Buffer.concat([Buffer.alloc(16, 0x61), Buffer.from([0xff])]),
    );
    try {
      await withMuon(
        [],
        async (driver) => {
          const results = await driver.evaluate<
            Array<{
              atLimit: string;
              invalidAfterLimitError: string;
              overLimitError: string;
            }>
          >(`(async () => {
            const captureError = async (operation) => {
              try {
                await operation();
                return "";
              } catch (error) {
                return String(error?.message ?? error);
              }
            };
            const targets = ${JSON.stringify([
              {
                atLimit: atLimitPath,
                overLimit: overLimitPath,
                invalidAfterLimit: invalidAfterLimitPath,
              },
              {
                atLimit: pathToFileUrlHref(atLimitPath),
                overLimit: pathToFileUrlHref(overLimitPath),
                invalidAfterLimit: pathToFileUrlHref(invalidAfterLimitPath),
              },
            ])};
            const entries = [];
            for (const target of targets) {
              const atLimit = await window.muon.fs.readTextFile(
                target.atLimit,
                "utf8",
              );
              const overLimitError = await captureError(() =>
                window.muon.fs.readTextFile(target.overLimit, "utf8"),
              );
              const invalidAfterLimitError = await captureError(() =>
                window.muon.fs.readTextFile(
                  target.invalidAfterLimit,
                  "utf8",
                ),
              );
              entries.push({
                atLimit,
                overLimitError,
                invalidAfterLimitError,
              });
            }
            return entries;
          })()`);
          for (const result of results) {
            expect(result.atLimit).toBe("a".repeat(16));
            expect(result.overLimitError).toContain(
              "readTextFile result exceeds configured maximum",
            );
            expect(result.invalidAfterLimitError).toContain(
              "readTextFile result exceeds configured maximum",
            );
            expect(result.invalidAfterLimitError).not.toContain("UTF-8");
          }
        },
        { internal: { "fs.readTextFile.maxBytes": "16" } },
      );
    } finally {
      await rm(directory, { recursive: true, force: true });
    }
  });

  it("rejects an invalid readTextFile byte limit during startup", async () => {
    await expectDebugMuonStartupFailure(
      [],
      "fs.readTextFile.maxBytes must be an unsigned decimal byte count",
      {
        internal: { "fs.readTextFile.maxBytes": "16MiB" },
      },
    );
  });

  it("routes filesystem results to the calling V8 context", async () => {
    const directory = await mkdtemp(join(tmpdir(), "muon-fs-context-"));
    try {
      await withMuon([], async (driver) => {
        await driver.send("Page.enable", undefined);
        const frameTree = await driver.send<{
          frameTree?: { frame?: { id?: string } };
        }>("Page.getFrameTree", undefined);
        const frameId = frameTree.frameTree?.frame?.id;
        expect(frameId).not.toBeUndefined();
        await driver.send("Page.createIsolatedWorld", {
          frameId: frameId ?? "",
          worldName: "muon-test-isolated-world",
        });

        const textPath = join(directory, "context.txt");
        await expect(
          driver.evaluate(`Promise.race([
            (async () => {
              await window.muon.fs.writeTextFile(
                ${JSON.stringify(textPath)},
                "context route",
                "utf8",
              );
              const text = await window.muon.fs.readTextFile(
                ${JSON.stringify(textPath)},
                "utf8",
              );
              return { text };
            })(),
            new Promise((resolve) => {
              setTimeout(() => resolve({ timeout: true }), 3000);
            }),
          ])`),
        ).resolves.toEqual({ text: "context route" });
      });
    } finally {
      await rm(directory, { recursive: true, force: true });
    }
  });

  it("rejects unsupported filesystem operations and invalid text", async () => {
    const directory = await mkdtemp(join(tmpdir(), "muon-fs-invalid-"));
    try {
      const missingPath = join(directory, "missing.txt");
      const directoryPath = join(directory, "directory");
      const invalidUtf8Path = join(directory, "invalid.txt");
      const validTextPath = join(directory, "valid.txt");
      const nulTextPath = join(directory, "nul.txt");
      await mkdir(directoryPath);
      await writeFile(invalidUtf8Path, Buffer.from([0xff, 0xfe]));
      await writeFile(validTextPath, "valid text");

      await withMuon([], async (driver) => {
        const calls = [
          {
            label: "missing readFile",
            call: `window.muon.fs.readFile(${JSON.stringify(missingPath)})`,
          },
          {
            label: "directory readFile",
            call: `window.muon.fs.readFile(${JSON.stringify(directoryPath)})`,
          },
          {
            label: "unsupported text encoding",
            call: `window.muon.fs.readTextFile(${JSON.stringify(validTextPath)}, "utf16")`,
          },
          {
            label: "invalid UTF-8 text",
            call: `window.muon.fs.readTextFile(${JSON.stringify(invalidUtf8Path)}, "utf8")`,
          },
          {
            label: "NUL text write",
            call: `window.muon.fs.writeTextFile(${JSON.stringify(nulTextPath)}, "a\\u0000b", "utf8")`,
          },
          {
            label: "NUL path write",
            call: `window.muon.fs.writeFile(${JSON.stringify(`${directory}/bad\u0000path`)}, new Uint8Array())`,
          },
        ];
        for (const { label, call } of calls) {
          await expect(
            evaluateRejection(driver, call),
            label,
          ).resolves.not.toBe("");
        }
      });
    } finally {
      await rm(directory, { recursive: true, force: true });
    }
  });

  it.each(configuredDevToolsShortcuts)(
    "opens DevTools from the configured $name shortcut",
    async ({ config, event }) => {
      await withMuonBrowserConfig(
        [],
        createBrowserShortcutConfig({ devtools: config }),
        async (driver) => {
          await expect(
            dispatchDevToolsShortcut(driver, event),
          ).resolves.toMatchObject({
            type: "page",
          });
        },
      );
    },
  );

  it("does not open DevTools from default shortcuts when remapped", async () => {
    await withMuonBrowserConfig(
      [],
      createBrowserShortcutConfig({ devtools: shiftF9DevToolsShortcut.config }),
      async (driver) => {
        await expectNoDevTools(driver, f12DevToolsShortcut.event);
        await expectNoDevTools(driver, ctrlShiftIDevToolsShortcut.event);
      },
    );
  });

  it("recycles the process from the configured recycle shortcut", async () => {
    const running = await startDebugMuonLauncher(
      [],
      TEST_NETWORK_ALLOW_PATTERNS,
      {},
      createBrowserShortcutConfig({ recycle: "ctrl+shift+f10" }),
    );
    let driver: CdpDriver | undefined = undefined;
    try {
      driver = await connectToMuonCdp({
        port: MUON_PORT,
        timeoutMs: cdpCommandTimeoutMs,
      });
      await driver.navigate(
        "data:text/html,<title>muon recycle shortcut</title>",
        cdpCommandTimeoutMs,
      );
      const firstProcessId = await driver.evaluate<number>(
        "window.muon.environments.getProcessId()",
      );
      driver = await dispatchRecycleKeyboardShortcut(
        driver,
        ctrlShiftF10RecycleShortcut,
      );
      driver?.close();
      driver = undefined;
      const recycled = await waitForRecycledMuon(firstProcessId);
      driver = recycled.driver;
      expect(recycled.processId).not.toBe(firstProcessId);
      await expect(driver.evaluate("document.location.href")).resolves.toBe(
        MUON_APP_URL,
      );
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
    } finally {
      await stopMuon(running, driver);
    }
  });

  it("recycles the process from the configured Ctrl+F12 shortcut", async () => {
    const running = await startDebugMuonLauncher(
      [],
      TEST_NETWORK_ALLOW_PATTERNS,
      {},
      createBrowserShortcutConfig({ devtools: "f12", recycle: "ctrl+f12" }),
    );
    let driver: CdpDriver | undefined = undefined;
    try {
      driver = await connectToMuonCdp({
        port: MUON_PORT,
        timeoutMs: cdpCommandTimeoutMs,
      });
      await driver.navigate(
        "data:text/html,<title>muon ctrl f12 recycle shortcut</title>",
        cdpCommandTimeoutMs,
      );
      const firstProcessId = await driver.evaluate<number>(
        "window.muon.environments.getProcessId()",
      );
      driver = await dispatchRecycleKeyboardShortcut(
        driver,
        ctrlF12RecycleShortcut,
      );
      driver?.close();
      driver = undefined;
      const recycled = await waitForRecycledMuon(firstProcessId);
      driver = recycled.driver;
      expect(recycled.processId).not.toBe(firstProcessId);
      await expect(driver.evaluate("document.location.href")).resolves.toBe(
        MUON_APP_URL,
      );
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
    } finally {
      await stopMuon(running, driver);
    }
  });

  linuxIt(
    "opens DevTools from native F12 after focusing a draggable page region",
    async () => {
      await withMuonBrowserConfig(
        [],
        createBrowserShortcutConfig({ devtools: "f12" }),
        async (driver) => {
          const title = "muon native f12 drag shortcut";
          await driver.navigate(
            createNativeShortcutDragPageUrl(title),
            cdpCommandTimeoutMs,
          );
          await waitForNativeWindowTitle(title, cdpCommandTimeoutMs);

          const previousTargetIds = await getCurrentTargetIds();
          await sendNativeKeyboardShortcut(title, "f12");
          await expect(
            waitForDevToolsTarget(previousTargetIds, targetTimeoutMs),
          ).resolves.toMatchObject({
            type: "page",
          });
        },
      );
    },
  );

  linuxIt(
    "recycles from native Ctrl+F12 after focusing a draggable page region",
    async () => {
      const running = await startDebugMuonLauncher(
        [],
        TEST_NETWORK_ALLOW_PATTERNS,
        {},
        createBrowserShortcutConfig({ devtools: "f12", recycle: "ctrl+f12" }),
      );
      let driver: CdpDriver | undefined = undefined;
      try {
        driver = await connectToMuonCdp({
          port: MUON_PORT,
          timeoutMs: cdpCommandTimeoutMs,
        });
        const title = "muon native ctrl f12 drag shortcut";
        await driver.navigate(
          createNativeShortcutDragPageUrl(title),
          cdpCommandTimeoutMs,
        );
        await waitForNativeWindowTitle(title, cdpCommandTimeoutMs);
        const firstProcessId = await driver.evaluate<number>(
          "window.muon.environments.getProcessId()",
        );

        await sendNativeKeyboardShortcut(title, "ctrl+f12");
        driver.close();
        driver = undefined;
        const recycled = await waitForRecycledMuon(firstProcessId);
        driver = recycled.driver;
        expect(recycled.processId).not.toBe(firstProcessId);
        await expect(driver.evaluate("document.location.href")).resolves.toBe(
          MUON_APP_URL,
        );
      } catch (error) {
        throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
      } finally {
        await stopMuon(running, driver);
      }
    },
  );

  it("reloads the page from the configured reload shortcut", async () => {
    await withMuonBrowserConfig(
      [],
      createBrowserShortcutConfig({ reload: "f5" }),
      async (driver) => {
        await driver.navigate(
          "data:text/html,<title>muon reload shortcut</title>",
          cdpCommandTimeoutMs,
        );
        const loadEvent = driver.waitForEvent(
          "Page.loadEventFired",
          cdpCommandTimeoutMs,
        );
        await dispatchKeyboardShortcut(driver, f5ReloadShortcut);
        await expect(loadEvent).resolves.toBeDefined();
      },
    );
  });

  it("does not reload from default shortcuts when remapped", async () => {
    await withMuonBrowserConfig(
      [],
      createBrowserShortcutConfig({ reload: "shift+f9" }),
      async (driver) => {
        await driver.navigate(
          "data:text/html,<title>muon reload disabled</title>",
          cdpCommandTimeoutMs,
        );
        await delay(100);
        await expectNoPageLoad(driver, f5ReloadShortcut);
        await expectNoPageLoad(driver, ctrlRReloadShortcut);
      },
    );
  });

  it("reloads the page from a configured modifier shortcut", async () => {
    await withMuonBrowserConfig(
      [],
      createBrowserShortcutConfig({ reload: "shift+f9" }),
      async (driver) => {
        await driver.navigate(
          "data:text/html,<title>muon reload modifier</title>",
          cdpCommandTimeoutMs,
        );
        const loadEvent = driver.waitForEvent(
          "Page.loadEventFired",
          cdpCommandTimeoutMs,
        );
        await dispatchKeyboardShortcut(driver, shiftF9DevToolsShortcut.event);
        await expect(loadEvent).resolves.toBeDefined();
      },
    );
  });

  it("toggles fullscreen from the configured shortcut", async () => {
    await withMuonBrowserConfig(
      [],
      createBrowserShortcutConfig({ fullscreen: "f11" }),
      async (driver) => {
        const initialSize = await getOuterSize(driver);
        await dispatchKeyboardShortcut(driver, f11FullscreenShortcut);
        await expect(
          waitForOuterSizeChange(driver, initialSize, cdpCommandTimeoutMs),
        ).resolves.toEqual(
          expect.objectContaining({
            width: expect.any(Number),
            height: expect.any(Number),
          }),
        );
      },
    );
  });

  it("does not toggle fullscreen from the default shortcut when remapped", async () => {
    await withMuonBrowserConfig(
      [],
      createBrowserShortcutConfig({
        fullscreen: shiftF9DevToolsShortcut.config,
      }),
      async (driver) => {
        const initialSize = await getOuterSize(driver);
        await dispatchKeyboardShortcut(driver, f11FullscreenShortcut);
        await delay(1000);
        await expect(getOuterSize(driver)).resolves.toEqual(initialSize);
      },
    );
  });

  it("toggles fullscreen from a remapped shortcut", async () => {
    await withMuonBrowserConfig(
      [],
      createBrowserShortcutConfig({
        fullscreen: shiftF9DevToolsShortcut.config,
      }),
      async (driver) => {
        const initialSize = await getOuterSize(driver);
        await dispatchKeyboardShortcut(driver, shiftF9DevToolsShortcut.event);
        await expect(
          waitForOuterSizeChange(driver, initialSize, cdpCommandTimeoutMs),
        ).resolves.toEqual(
          expect.objectContaining({
            width: expect.any(Number),
            height: expect.any(Number),
          }),
        );
      },
    );
  });

  it("hard reloads the page from the configured shortcut", async () => {
    await withMuonBrowserConfig(
      [],
      createBrowserShortcutConfig({ hardReload: "ctrl+shift+r" }),
      async (driver) => {
        await driver.navigate(
          "data:text/html,<title>muon hard reload shortcut</title>",
          cdpCommandTimeoutMs,
        );
        const loadEvent = driver.waitForEvent(
          "Page.loadEventFired",
          cdpCommandTimeoutMs,
        );
        await dispatchKeyboardShortcut(driver, ctrlShiftRShortcut);
        await expect(loadEvent).resolves.toBeDefined();
      },
    );
  });

  it("does not hard reload from the default shortcut when remapped", async () => {
    await withMuonBrowserConfig(
      [],
      createBrowserShortcutConfig({
        hardReload: shiftF9DevToolsShortcut.config,
      }),
      async (driver) => {
        await driver.navigate(
          "data:text/html,<title>muon hard reload disabled</title>",
          cdpCommandTimeoutMs,
        );
        await expectNoPageLoad(driver, ctrlShiftRShortcut);
      },
    );
  });

  it("changes page zoom from configured shortcuts", async () => {
    await withMuonBrowserConfig(
      [],
      createBrowserShortcutConfig({
        zoomIn: "ctrl+plus",
        zoomOut: "ctrl+minus",
        resetZoom: "ctrl+0",
      }),
      async (driver) => {
        await driver.navigate(
          "data:text/html,<title>muon zoom shortcuts</title>",
          cdpCommandTimeoutMs,
        );
        const initialWidth = await driver.evaluate<number>("window.innerWidth");

        await dispatchKeyboardShortcut(driver, ctrlPlusZoomShortcut);
        const zoomedInWidth = await waitForInnerWidth(
          driver,
          (value) => value < initialWidth,
          cdpCommandTimeoutMs,
        );

        await dispatchKeyboardShortcut(driver, ctrlMinusZoomShortcut);
        await waitForInnerWidth(
          driver,
          (value) => Math.abs(value - initialWidth) <= 1,
          cdpCommandTimeoutMs,
        );

        await dispatchKeyboardShortcut(driver, ctrlMinusZoomShortcut);
        await waitForInnerWidth(
          driver,
          (value) => value > initialWidth,
          cdpCommandTimeoutMs,
        );

        await dispatchKeyboardShortcut(driver, ctrl0ZoomShortcut);
        await waitForInnerWidth(
          driver,
          (value) =>
            Math.abs(value - initialWidth) <= 1 && value !== zoomedInWidth,
          cdpCommandTimeoutMs,
        );
      },
    );
  });

  it("does not zoom from the default shortcut when remapped", async () => {
    await withMuonBrowserConfig(
      [],
      createBrowserShortcutConfig({
        zoomIn: shiftF9DevToolsShortcut.config,
      }),
      async (driver) => {
        await driver.navigate(
          "data:text/html,<title>muon zoom disabled</title>",
          cdpCommandTimeoutMs,
        );
        const initialWidth = await driver.evaluate<number>("window.innerWidth");
        await dispatchKeyboardShortcut(driver, ctrlPlusZoomShortcut);
        await delay(1000);
        await expect(driver.evaluate("window.innerWidth")).resolves.toBe(
          initialWidth,
        );
      },
    );
  });

  it("keeps muon APIs out of the DevTools frontend", async () => {
    const directory = await mkdtemp(join(tmpdir(), "muon-devtools-fs-"));
    const devToolsDrivers: CdpDriver[] = [];
    try {
      await withMuonBrowserConfig(
        [],
        createBrowserShortcutConfig({ devtools: "f12" }),
        async (driver) => {
          const devToolsTarget = await dispatchDevToolsShortcut(
            driver,
            f12DevToolsShortcut.event,
          );
          const devToolsDriver = await connectToMuonCdp({
            port: MUON_PORT,
            targetId: devToolsTarget.id,
            timeoutMs: cdpCommandTimeoutMs,
          });
          devToolsDrivers.push(devToolsDriver);

          await expect(
            devToolsDriver.evaluate(`({
            href: document.location.href,
            muon: typeof window.muon,
          })`),
          ).resolves.toMatchObject({
            href: expect.stringContaining("devtools://"),
            muon: "undefined",
          });

          const textPath = join(directory, "page.txt");
          await expect(
            driver.evaluate(`(async () => {
            await window.muon.fs.writeTextFile(
              ${JSON.stringify(textPath)},
              "page api still works",
              "utf8",
            );
            return await window.muon.fs.readTextFile(
              ${JSON.stringify(textPath)},
              "utf8",
            );
          })()`),
          ).resolves.toBe("page api still works");
        },
      );
    } finally {
      for (const devToolsDriver of devToolsDrivers) {
        devToolsDriver.close();
      }
      await rm(directory, { recursive: true, force: true });
    }
  });

  it("keeps the main page alive after closing DevTools and exits when the main browser closes", async () => {
    const running = await startDebugMuon(
      ["muon_test_plugin_alpha"],
      TEST_NETWORK_ALLOW_PATTERNS,
      {},
      createBrowserShortcutConfig({ devtools: "f12" }),
    );
    let driver: CdpDriver | undefined = undefined;
    try {
      driver = await connectToMuonCdp({
        port: MUON_PORT,
        timeoutMs: cdpCommandTimeoutMs,
      });
      await driver.navigate(
        "data:text/html,<title>muon lifecycle</title>",
        cdpCommandTimeoutMs,
      );

      const devToolsTarget = await dispatchDevToolsShortcut(
        driver,
        f12DevToolsShortcut.event,
      );
      await driver.send("Target.closeTarget", { targetId: devToolsTarget.id });
      await waitForTargetClosed(devToolsTarget.id, targetTimeoutMs);

      await expect(driver.evaluate("document.title")).resolves.toBe(
        "muon lifecycle",
      );
      await expect(
        driver.evaluate("window.muon.test.alpha.alphaName()"),
      ).resolves.toBe("alpha");

      try {
        await driver.send("Browser.close", undefined);
      } catch {
        // Closing the browser can close the CDP socket before a response arrives.
      }
      driver.close();
      driver = undefined;
      await expect(
        waitForProcessExit(running, processExitTimeoutMs),
      ).resolves.toBeUndefined();
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
    } finally {
      await stopMuon(running, driver);
    }
  });

  it("does not expose the Debug CDP endpoint from a Release build", async () => {
    const running = await startReleaseMuon();
    try {
      await delay(shouldUseValgrind ? 5000 : 1000);
      expect(running.process.exitCode).toBeNull();
      const processGroupId = running.process.pid;
      if (processGroupId === undefined) {
        throw new Error("muon process id is unavailable");
      }

      const commandLines = await listProcessGroupCommandLines(processGroupId);
      expect(commandLines.join("\n")).not.toContain("--remote-debugging-port");
      await expect(
        listCdpTargets({ port: MUON_PORT, timeoutMs: 500 }),
      ).rejects.toThrow();
    } catch (error) {
      throw new Error(`${String(error)}\nMuon stderr:\n${running.stderr}`);
    } finally {
      await stopMuon(running, undefined);
    }
  });
});
