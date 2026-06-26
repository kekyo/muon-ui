// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import { describe, expect, it } from "vitest";

import {
  connectWindowsAgent,
  requiredWindowsAgentFeatureNames,
} from "./plugin-e2e/windows-agent.js";
import { parseWindowsE2eEnvironment } from "./plugin-e2e/windows-environment.js";

const windowsE2eEnvironment = parseWindowsE2eEnvironment(process.env);
const describeWindowsE2e =
  windowsE2eEnvironment.status === "configured" ? describe : describe.skip;
const suiteName =
  windowsE2eEnvironment.status === "configured"
    ? "Windows agent-rover e2e"
    : `Windows agent-rover e2e (${windowsE2eEnvironment.reason})`;

describeWindowsE2e(suiteName, { concurrent: false }, () => {
  it("connects to a Windows agent with required capabilities", async () => {
    if (windowsE2eEnvironment.status !== "configured") {
      throw new Error(windowsE2eEnvironment.reason);
    }

    const agent = await connectWindowsAgent(windowsE2eEnvironment.config);
    try {
      const capabilities = await agent.capabilities();
      expect(capabilities.platform).toBe("windows");
      expect(capabilities.features).toEqual(
        expect.arrayContaining([...requiredWindowsAgentFeatureNames]),
      );

      expect(typeof agent.applications.launch).toBe("function");
      expect(typeof agent.processes.kill).toBe("function");
      expect(typeof agent.files.writeFile).toBe("function");
      expect(typeof agent.windows).toBe("function");
      expect(typeof agent.keyboard.press).toBe("function");
      expect(typeof agent.mouse.click).toBe("function");
      expect(typeof agent.screenshot).toBe("function");

      const windows = await agent.windows();
      expect(Array.isArray(windows)).toBe(true);

      const screenshot = await agent.screenshot();
      expect(screenshot.image.byteLength).toBeGreaterThan(0);
    } finally {
      agent.release();
    }
  });
});
