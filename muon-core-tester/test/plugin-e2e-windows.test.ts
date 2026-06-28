// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import { afterAll, describe, expect, it, vi } from "vitest";

import {
  connectWindowsAgent,
  requiredWindowsAgentFeatureNames,
} from "./plugin-e2e/windows-agent.js";
import {
  clearWindowsRemoteContext,
  joinWindowsPath,
  setWindowsRemoteContext,
} from "./plugin-e2e/windows-context.js";
import { parseWindowsE2eEnvironment } from "./plugin-e2e/windows-environment.js";
import { resolveWindowsRuntimeTarget } from "./plugin-e2e/windows-matrix.js";
import { stageWindowsRuntime } from "./plugin-e2e/windows-staging.js";

const windowsE2eEnvironment = parseWindowsE2eEnvironment(process.env);
const windowsRuntimeTarget = resolveWindowsRuntimeTarget(
  process.env.MUON_E2E_WINDOWS_TARGET,
);
const windowsCdpRelayBasePort =
  windowsRuntimeTarget.target === "windows-i686" ? 39220 : 39320;
const describeWindowsE2e =
  windowsE2eEnvironment.status === "configured" ? describe : describe.skip;
const suiteName =
  windowsE2eEnvironment.status === "configured"
    ? `Windows agent-rover e2e (${windowsRuntimeTarget.target})`
    : `Windows agent-rover e2e (${windowsE2eEnvironment.reason})`;

const windowsAgent =
  windowsE2eEnvironment.status === "configured"
    ? await connectWindowsAgent(windowsE2eEnvironment.config)
    : undefined;

if (windowsE2eEnvironment.status === "configured") {
  vi.setConfig({
    hookTimeout: 180000,
    testTimeout: 180000,
  });
}

if (
  windowsE2eEnvironment.status === "configured" &&
  windowsAgent !== undefined
) {
  const runtime = await stageWindowsRuntime(
    windowsAgent,
    windowsE2eEnvironment.config,
    windowsRuntimeTarget,
  );
  await windowsAgent.files.mkdir(
    joinWindowsPath(
      windowsE2eEnvironment.config.workDir,
      windowsRuntimeTarget.target,
      "tmp",
    ),
    { recursive: true },
  );
  setWindowsRemoteContext({
    agent: windowsAgent,
    cdpHost: windowsE2eEnvironment.config.host,
    cdpPort: windowsCdpRelayBasePort,
    environment: windowsE2eEnvironment.config,
    httpHost: windowsE2eEnvironment.config.httpHost,
    runtime,
    tempDirectory: joinWindowsPath(
      windowsE2eEnvironment.config.workDir,
      windowsRuntimeTarget.target,
      "tmp",
    ),
  });
}

afterAll(() => {
  clearWindowsRemoteContext();
  windowsAgent?.release();
});

describeWindowsE2e(suiteName, { concurrent: false }, () => {
  it("connects to a Windows agent with required capabilities", async () => {
    if (windowsE2eEnvironment.status !== "configured") {
      throw new Error(windowsE2eEnvironment.reason);
    }
    if (windowsAgent === undefined) {
      throw new Error("Windows agent is not connected");
    }

    const capabilities = await windowsAgent.capabilities();
    expect(capabilities.platform).toBe("windows");
    expect(capabilities.features).toEqual(
      expect.arrayContaining([...requiredWindowsAgentFeatureNames]),
    );

    expect(typeof windowsAgent.applications.launch).toBe("function");
    expect(typeof windowsAgent.processes.kill).toBe("function");
    expect(typeof windowsAgent.files.writeFile).toBe("function");
    expect(typeof windowsAgent.windows).toBe("function");
    expect(typeof windowsAgent.keyboard.press).toBe("function");
    expect(typeof windowsAgent.mouse.click).toBe("function");
    expect(typeof windowsAgent.screenshot).toBe("function");

    const windows = await windowsAgent.windows();
    expect(Array.isArray(windows)).toBe(true);

    const screenshot = await windowsAgent.screenshot();
    expect(screenshot.image.byteLength).toBeGreaterThan(0);
  });
});

if (windowsE2eEnvironment.status === "configured") {
  await import("./plugin-e2e/app-network.js");
  await import("./plugin-e2e/browser-background.js");
  await import("./plugin-e2e/title-bar.js");
  await import("./plugin-e2e/runtime-api.js");
  await import("./plugin-e2e/plugin-interop.js");
  await import("./plugin-e2e/native-dialog.js");
}
