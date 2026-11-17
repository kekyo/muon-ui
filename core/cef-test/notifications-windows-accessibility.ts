// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { mkdir, mkdtemp, readFile, writeFile } from "node:fs/promises";
import { dirname, join, resolve, win32 } from "node:path";
import { fileURLToPath } from "node:url";
import type { RemoteAgent, ScreenRect } from "agent-rover";
import { resolveWindowsE2eEnvironment } from "../../scripts/windows-e2e-environment.js";
import { resolveWindowsRuntimeTarget } from "./plugin-e2e/windows-matrix.js";

/** Actual Windows notification controls, with reversible test permission changes. */
export interface WindowsNotificationDriver {
  /** Captures an exact visible test notification.
   * @param title Exact title belonging to the dedicated test app.
   * @param screenshotPath Local destination PNG path.
   * @returns When UI Automation observes the notification and capture completes.
   */
  waitForNotification(title: string, screenshotPath: string): Promise<void>;
  /** Invokes the matched notification or a named button through UI Automation.
   * @param title Exact title belonging to the dedicated test app.
   * @param actionTitle Exact button title, or null for the ordinary activation.
   * @param screenshotPath Local PNG path captured before invoking the control.
   * @returns When Windows accepts the UI control invocation.
   */
  activateNotification(
    title: string,
    actionTitle: string | null,
    screenshotPath: string,
  ): Promise<void>;
  /** Observes absence in an open notification center.
   * @param title Exact test notification title.
   * @returns When the notification is absent from the visible center.
   */
  waitForRemoval(title: string): Promise<void>;
  /** Changes the OS notification switch through the Settings UI.
   * @param enabled Desired global notification switch value during this test.
   * @returns When the UI reports the requested value.
   */
  setEnabled(enabled: boolean): Promise<void>;
  /** Restores the original notification switch and removes only this test app's registrations.
   * @returns When restoration and dedicated notification cleanup complete.
   * @remarks Call after the test application exits. Does not release the agent lease.
   */
  close(): Promise<void>;
}

const psLiteral = (value: string): string => `'${value.replaceAll("'", "''")}'`;

/** Captures a notification after its screen position and pixels settle.
 * @param bounds Reads the current notification group's physical screen bounds.
 * @param screenshot Captures the supplied rectangle without changing the UI.
 * @returns The complete notification image.
 */
export const captureWindowsNotification = async (
  bounds: () => Promise<ScreenRect>,
  screenshot: (rectangle: ScreenRect) => Promise<Buffer>,
): Promise<Buffer> => {
  const sameBounds = (left: ScreenRect, right: ScreenRect): boolean =>
    left.x === right.x &&
    left.y === right.y &&
    left.width === right.width &&
    left.height === right.height;
  const deadline = performance.now() + 30000;
  let previous: { bounds: ScreenRect; image: Buffer } | undefined;
  do {
    const before = await bounds();
    const image = await screenshot(before);
    const after = await bounds();
    // UIA visibility precedes the shell's slide/expand animation. Both the
    // rectangle around this capture and the next captured frame must agree.
    if (sameBounds(before, after)) {
      if (
        previous &&
        sameBounds(previous.bounds, after) &&
        previous.image.equals(image)
      )
        return image;
      previous = { bounds: after, image };
    } else previous = undefined;
  } while (performance.now() < deadline);
  throw new Error(
    "Notification position and pixels did not settle for capture",
  );
};

/** Creates a directory for one notification test run without replacing evidence.
 * @param parent Parent directory grouping captures for this test mode.
 * @returns A new local artifact directory.
 */
export const createWindowsNotificationArtifacts = async (
  parent: string,
): Promise<string> => {
  await mkdir(parent, { recursive: true });
  return await mkdtemp(join(parent, "run-"));
};

/** Records opening motion and the stable notification before allowing cleanup.
 * @typeParam T Recorded video metadata.
 * @param video In-progress opening video capture.
 * @param notification In-progress notification observation and screenshot.
 * @returns Video metadata after both operations have finished.
 */
export const captureWindowsNotificationOpening = async <T>(
  video: Promise<T>,
  notification: Promise<void>,
): Promise<T> => {
  // Neither capture may outlive the observer whose executable and DLLs are
  // removed in the caller's finally block, including after a recording error.
  const [recorded, observed] = await Promise.allSettled([video, notification]);
  if (recorded.status === "rejected") throw recorded.reason;
  if (observed.status === "rejected") throw observed.reason;
  return recorded.value;
};

/** Creates an observer for the English Windows test machine.
 * @param agent Existing agent-rover connection, owned by the caller.
 * @param appId Dedicated dev.muon.NotificationTest application ID for cleanup.
 * @param displayName Exact app header shown in the notification center.
 * @returns The driver; close it in finally after stopping the test app.
 * @remarks The native observer uses CUIAutomation8 and OS change events; no polling.
 */
export const createWindowsNotificationDriver = async (
  agent: RemoteAgent,
  appId: string,
  displayName: string,
): Promise<WindowsNotificationDriver> => {
  if (!/^dev\.muon\.Notification(?:Test|Probe)\.[\w.-]+$/iu.test(appId))
    throw new Error("Notification cleanup requires a dedicated test app ID");
  const environment = resolveWindowsE2eEnvironment(process.env);
  const target = resolveWindowsRuntimeTarget(
    process.env.MUON_E2E_WINDOWS_TARGET,
  );
  const observer = win32.join(
    environment.workDir,
    "notifications",
    target.target,
    `observer-${process.pid}`,
    "muon_notification_windows_observer.exe",
  );
  const execute = async (
    path: string,
    args: string[],
    timeoutMs: number,
  ): Promise<string> => {
    const process = await agent.processes.launchManaged({
      path,
      arguments: args,
      captureStdout: true,
      captureStderr: true,
      createNoWindow: true,
    });
    try {
      const result = await process.waitForExit({ timeoutMs });
      if (result.root.exitCode !== 0)
        throw new Error(`${path}: ${await process.stderrText()}`);
      return (await process.stdoutText()).trim();
    } finally {
      await process.releaseAsync();
    }
  };
  const powershell = async (script: string): Promise<string> =>
    await execute(
      "powershell.exe",
      [
        "-NoProfile",
        "-NonInteractive",
        "-EncodedCommand",
        Buffer.from(
          `$ErrorActionPreference='Stop';$ProgressPreference='SilentlyContinue';${script}`,
          "utf16le",
        ).toString("base64"),
      ],
      30000,
    );
  const setting = async (desired: boolean | undefined): Promise<boolean> => {
    await agent.applications.launch({
      path: "explorer.exe",
      arguments: ["ms-settings:notifications"],
    });
    await agent.waitForWindow({ title: "Settings" }, { timeoutMs: 10000 });
    const result = await execute(
      observer,
      ["setting", "", "", desired === undefined ? "" : desired ? "1" : "0"],
      35000,
    );
    if (result !== "0" && result !== "1")
      throw new Error(`Unknown notification switch: ${result}`);
    return result === "1";
  };
  let initial: boolean | undefined;
  let closed = false;
  let openedCenter = false;
  const observe = async (
    operation: string,
    title: string,
    action: string,
  ): Promise<string> => {
    try {
      return await execute(
        observer,
        [operation, title, displayName, action],
        35000,
      );
    } catch (error) {
      throw new Error(
        `Windows notification ${operation}: ${title}; action=${action || "<default>"}`,
        { cause: error },
      );
    }
  };
  const center = async (): Promise<void> => {
    if ((await observe("center", "", "")) === "false") {
      await agent.keyboard.press("n", { modifiers: ["Meta"] });
      openedCenter = true;
    }
  };
  const capture = async (
    title: string,
    action: string,
    path: string,
  ): Promise<void> => {
    await center();
    const image = await captureWindowsNotification(
      async () =>
        JSON.parse(await observe("prepare", title, action)) as ScreenRect,
      async (bounds) => (await agent.screenshot({ rect: bounds })).image,
    );
    await mkdir(dirname(path), { recursive: true });
    await writeFile(path, image);
  };
  const close = async (): Promise<void> => {
    if (closed) return;
    closed = true;
    try {
      if (openedCenter && (await observe("center", "", "")) === "true")
        await agent.keyboard.press("Escape");
      await powershell(`[Windows.UI.Notifications.ToastNotificationManager,Windows.UI.Notifications,ContentType=WindowsRuntime]::History.Clear(${psLiteral(appId)})
$root=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser,[Microsoft.Win32.RegistryView]::Registry64)
try{$key=$root.OpenSubKey('Software\\Classes\\AppUserModelId',$true);if($null -ne $key){try{$key.DeleteSubKeyTree(${psLiteral(appId)},$false)}finally{$key.Dispose()}}}finally{$root.Dispose()}`);
    } finally {
      try {
        if (initial !== undefined) await setting(initial);
      } finally {
        if (await agent.files.exists(win32.dirname(observer)))
          await agent.files.remove(win32.dirname(observer), {
            recursive: true,
          });
      }
    }
  };
  try {
    await agent.files.mkdir(win32.dirname(observer), { recursive: true });
    await agent.files.writeFile(
      observer,
      await readFile(
        fileURLToPath(
          new URL(
            `../cef/.build/test/${target.target}/release/muon_notification_windows_observer.exe`,
            import.meta.url,
          ),
        ),
      ),
    );
    for (const name of [
      "libstdc++-6.dll",
      target.target === "windows-amd64"
        ? "libgcc_s_seh-1.dll"
        : "libgcc_s_dw2-1.dll",
    ]) {
      await agent.files.writeFile(
        win32.join(win32.dirname(observer), name),
        await readFile(
          resolve(
            fileURLToPath(new URL("../../", import.meta.url)),
            target.releaseRuntimeDirectory,
            name,
          ),
        ),
      );
    }
    initial = await setting(undefined);
  } catch (error) {
    await close();
    throw error;
  }
  return {
    waitForNotification: async (title, path) => await capture(title, "", path),
    activateNotification: async (title, action, path) => {
      await capture(title, action ?? "", path);
      await observe("activate", title, action ?? "");
    },
    waitForRemoval: async (title) => {
      await center();
      await observe("removed", title, "");
    },
    setEnabled: async (enabled) => {
      await setting(enabled);
    },
    close,
  };
};
