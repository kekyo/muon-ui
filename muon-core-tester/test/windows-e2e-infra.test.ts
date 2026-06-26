// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import { describe, expect, it } from "vitest";

import {
  createWindowsE2eMatrix,
  windowsRuntimeTargets,
} from "./plugin-e2e/windows-matrix.js";
import { parseWindowsE2eEnvironment } from "./plugin-e2e/windows-environment.js";

describe("Windows e2e environment", () => {
  it("skips when the Windows agent host is missing", () => {
    expect(
      parseWindowsE2eEnvironment({
        AGENT_ROVER_WIN11_TOKEN: "token",
      }),
    ).toEqual({
      reason: "AGENT_ROVER_WIN11_HOST is not set",
      status: "skip",
    });
  });

  it("skips when the Windows agent token is empty", () => {
    expect(
      parseWindowsE2eEnvironment({
        AGENT_ROVER_WIN11_HOST: "192.0.2.10",
        AGENT_ROVER_WIN11_TOKEN: " ",
      }),
    ).toEqual({
      reason: "AGENT_ROVER_WIN11_TOKEN is not set",
      status: "skip",
    });
  });

  it("uses defaults for optional port and work directory", () => {
    expect(
      parseWindowsE2eEnvironment({
        AGENT_ROVER_WIN11_HOST: "192.0.2.10",
        AGENT_ROVER_WIN11_TOKEN: "token",
      }),
    ).toEqual({
      config: {
        host: "192.0.2.10",
        port: 39397,
        token: "token",
        workDir: String.raw`C:\muon-e2e`,
      },
      status: "configured",
    });
  });

  it("rejects invalid optional ports", () => {
    expect(() =>
      parseWindowsE2eEnvironment({
        AGENT_ROVER_WIN11_HOST: "192.0.2.10",
        AGENT_ROVER_WIN11_PORT: "abc",
        AGENT_ROVER_WIN11_TOKEN: "token",
      }),
    ).toThrow("AGENT_ROVER_WIN11_PORT must be an integer from 1 to 65535");
  });
});

describe("Windows e2e matrix", () => {
  it("runs the shared case list for both Windows targets", () => {
    const caseNames = ["loads the initial app page", "exposes APIs"];
    const matrix = createWindowsE2eMatrix(caseNames);

    expect(windowsRuntimeTargets.map((target) => target.target)).toEqual([
      "windows32",
      "windows64",
    ]);
    expect(windowsRuntimeTargets.map((target) => target.platform)).toEqual([
      "win32",
      "win64",
    ]);
    expect(matrix).toEqual([
      {
        caseNames,
        debugRuntimeDirectory: "muon-core/.run/test-windows32-debug",
        platform: "win32",
        releaseRuntimeDirectory: "muon-core/.run/test-windows32-release",
        target: "windows32",
      },
      {
        caseNames,
        debugRuntimeDirectory: "muon-core/.run/test-windows64-debug",
        platform: "win64",
        releaseRuntimeDirectory: "muon-core/.run/test-windows64-release",
        target: "windows64",
      },
    ]);
    expect(matrix[0]?.caseNames).toBe(matrix[1]?.caseNames);
  });
});
