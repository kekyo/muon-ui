// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { afterAll, describe, expect, it, vi } from "vitest";

import {
  connectWindowsAgent,
  requiredWindowsAgentFeatureNames,
  requiredWindowsAgentProtocolVersion,
} from "./plugin-e2e/windows-agent.js";
import {
  clearWindowsRemoteContext,
  joinWindowsPath,
  setWindowsRemoteContext,
} from "./plugin-e2e/windows-context.js";
import { resolveWindowsE2eEnvironment } from "../../scripts/windows-e2e-environment.js";
import { resolveWindowsRuntimeTarget } from "./plugin-e2e/windows-matrix.js";
import { discoverWindowsTestNetwork } from "./plugin-e2e/windows-network.js";
import { stageWindowsRuntime } from "./plugin-e2e/windows-staging.js";

const windowsE2eEnvironment = resolveWindowsE2eEnvironment(process.env);
const windowsRuntimeTarget = resolveWindowsRuntimeTarget(
  process.env.MUON_E2E_WINDOWS_TARGET,
);
const connection = await connectWindowsAgent(
  windowsE2eEnvironment,
  windowsRuntimeTarget,
);
const describeWindowsE2e =
  connection.status === "connected" ? describe : describe.skip;
const suiteName =
  connection.status === "connected"
    ? `Windows agent-rover e2e (${windowsRuntimeTarget.target})`
    : `Windows agent-rover e2e (${windowsRuntimeTarget.target}: ${connection.reason})`;

const windowsAgent =
  connection.status === "connected" ? connection.agent : undefined;

if (connection.status === "skip") {
  process.stderr.write(`Skipping ${suiteName}\n`);
}

if (windowsAgent !== undefined) {
  vi.setConfig({
    hookTimeout: 180000,
    testTimeout: 180000,
  });
}

if (windowsAgent !== undefined) {
  try {
    const network = await discoverWindowsTestNetwork(
      windowsAgent,
      windowsE2eEnvironment.nodePath,
    );
    const runtime = await stageWindowsRuntime(
      windowsAgent,
      windowsE2eEnvironment,
      windowsRuntimeTarget,
    );
    await windowsAgent.files.mkdir(
      joinWindowsPath(
        windowsE2eEnvironment.workDir,
        windowsRuntimeTarget.target,
        "tmp",
      ),
      { recursive: true },
    );
    setWindowsRemoteContext({
      agent: windowsAgent,
      cdpHost: network.cdpHost,
      cdpPort: 0,
      environment: windowsE2eEnvironment,
      httpHost: windowsE2eEnvironment.httpHost ?? network.httpHost,
      runtime,
      tempDirectory: joinWindowsPath(
        windowsE2eEnvironment.workDir,
        windowsRuntimeTarget.target,
        "tmp",
      ),
    });
  } catch (error) {
    windowsAgent.release();
    throw error;
  }
}

afterAll(async () => {
  clearWindowsRemoteContext();
  await windowsAgent?.release();
});

describeWindowsE2e(suiteName, { concurrent: false }, () => {
  it("connects to a Windows agent with required capabilities", async () => {
    if (windowsAgent === undefined) {
      throw new Error("Windows agent is not connected");
    }

    const capabilities = await windowsAgent.capabilities();
    expect(capabilities.platform).toBe("windows");
    expect(capabilities.protocolVersion).toBe(
      requiredWindowsAgentProtocolVersion,
    );
    expect(capabilities.features).toEqual(
      expect.arrayContaining([...requiredWindowsAgentFeatureNames]),
    );

    expect(typeof windowsAgent.applications.launch).toBe("function");
    expect(typeof windowsAgent.processes.launchManaged).toBe("function");
    expect(typeof windowsAgent.processes.kill).toBe("function");
    expect(typeof windowsAgent.files.syncDirectory).toBe("function");
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

if (windowsAgent !== undefined) {
  await import("./plugin-e2e/app-network.js");
  await import("./plugin-e2e/webgl.js");
  await import("./plugin-e2e/browser-background.js");
  await import("./plugin-e2e/title-bar.js");
  await import("./plugin-e2e/runtime-api.js");
  await import("./plugin-e2e/application-windows-long-path.js");
  await import("./plugin-e2e/filesystem-windows-long-path.js");
  await import("./plugin-e2e/plugin-interop.js");
  await import("./plugin-e2e/node-plugin.js");
  await import("./plugin-e2e/node-plugin-express.js");
  await import("./plugin-e2e/node-plugin-validate.js");
  await import("./plugin-e2e/native-dialog.js");
  await import("./plugin-e2e/node-plugin-windows-job.js");
}
