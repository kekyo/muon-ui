// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import { describe, expect, it } from "vitest";
import type { RemoteAgent, RemoteProcessSnapshot } from "agent-rover";

import {
  createWindowsE2eMatrix,
  resolveWindowsRuntimeTarget,
  windowsRuntimeTargets,
} from "./plugin-e2e/windows-matrix.js";
import {
  parseWindowsE2eEnvironment,
  resolveWindowsE2eEnvironment,
} from "./plugin-e2e/windows-environment.js";
import {
  allocateWindowsRemoteCdpPort,
  clearWindowsRemoteContext,
  pathToWindowsFileUrlHref,
  setWindowsRemoteContext,
  type WindowsRemoteContext,
} from "./plugin-e2e/windows-context.js";
import {
  cleanupWindowsStagedRuntimeProcesses,
  removeWindowsStagedRuntimeDirectory,
} from "./plugin-e2e/windows-staging.js";

interface FakeStagingAgent {
  agent: RemoteAgent;
  getKilledProcessIds: () => number[];
  getRemoveAttempts: () => number;
  getWaitedProcessIds: () => number[];
}

const createProcessSnapshot = (
  id: number,
  name: string,
  path: string,
  running: boolean,
): RemoteProcessSnapshot => ({
  exitCode: running ? null : 0,
  id,
  name,
  path,
  running,
});

const createFakeStagingAgent = (
  initialProcesses: readonly RemoteProcessSnapshot[],
  removeFailureCount: number,
): FakeStagingAgent => {
  let processes = [...initialProcesses];
  const killedProcessIds: number[] = [];
  const waitedProcessIds: number[] = [];
  let removeAttempts = 0;

  const agent = {
    files: {
      remove: async (): Promise<void> => {
        removeAttempts += 1;
        if (removeAttempts <= removeFailureCount) {
          throw new Error("DeleteFileW failed.");
        }
      },
    },
    processes: {
      kill: async (processId: number): Promise<void> => {
        killedProcessIds.push(processId);
        processes = processes.map((processInfo) =>
          processInfo.id === processId
            ? { ...processInfo, exitCode: 1, running: false }
            : processInfo,
        );
      },
      list: async (): Promise<readonly RemoteProcessSnapshot[]> => processes,
      waitForExit: async (
        processId: number,
      ): Promise<RemoteProcessSnapshot> => {
        waitedProcessIds.push(processId);
        const processInfo = processes.find(
          (candidate) => candidate.id === processId,
        );
        if (processInfo === undefined) {
          return createProcessSnapshot(processId, "", "", false);
        }
        return processInfo;
      },
    },
  } as unknown as RemoteAgent;

  return {
    agent,
    getKilledProcessIds: () => killedProcessIds,
    getRemoveAttempts: () => removeAttempts,
    getWaitedProcessIds: () => waitedProcessIds,
  };
};

const createFakeWindowsRemoteContext = (
  cdpPort: number,
  reservedPort: number,
): WindowsRemoteContext => ({
  agent: {} as RemoteAgent,
  cdpHost: "192.0.2.10",
  cdpPort,
  environment: {
    host: "192.0.2.10",
    httpHost: "192.0.2.1",
    port: reservedPort,
    token: "token",
    workDir: String.raw`C:\muon-e2e`,
  },
  httpHost: "192.0.2.1",
  runtime: {
    debugRuntimeDirectory: String.raw`C:\muon-e2e\windows-amd64\debug`,
    releaseRuntimeDirectory: String.raw`C:\muon-e2e\windows-amd64\release`,
    relayExecutablePath: String.raw`C:\muon-e2e\windows-amd64\muon-cdp-relay.exe`,
    target: "windows-amd64",
  },
  tempDirectory: String.raw`C:\muon-e2e\windows-amd64\tmp`,
});

describe("Windows e2e environment", () => {
  it("skips remote execution when the Windows e2e flag is missing", () => {
    expect(
      resolveWindowsE2eEnvironment({
        AGENT_ROVER_WIN11_HOST: "192.0.2.10",
        AGENT_ROVER_WIN11_TOKEN: "token",
      }),
    ).toEqual({
      reason: "MUON_E2E_REMOTE_WINDOWS is not set",
      status: "skip",
    });
  });

  it("uses the Windows agent configuration when remote execution is enabled", () => {
    expect(
      resolveWindowsE2eEnvironment({
        AGENT_ROVER_WIN11_HOST: "192.0.2.10",
        AGENT_ROVER_WIN11_TOKEN: "token",
        MUON_E2E_REMOTE_WINDOWS: "1",
      }),
    ).toEqual({
      config: {
        host: "192.0.2.10",
        httpHost: "192.0.2.1",
        port: 39397,
        token: "token",
        workDir: String.raw`C:\muon-e2e`,
      },
      status: "configured",
    });
  });

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
        httpHost: "192.0.2.1",
        port: 39397,
        token: "token",
        workDir: String.raw`C:\muon-e2e`,
      },
      status: "configured",
    });
  });

  it("uses an explicit remote HTTP host when provided", () => {
    expect(
      parseWindowsE2eEnvironment({
        AGENT_ROVER_WIN11_HOST: "192.0.2.10",
        AGENT_ROVER_WIN11_TOKEN: "token",
        MUON_E2E_REMOTE_HTTP_HOST: "10.10.0.1",
      }),
    ).toEqual({
      config: {
        host: "192.0.2.10",
        httpHost: "10.10.0.1",
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
      "windows-i686",
      "windows-amd64",
    ]);
    expect(windowsRuntimeTargets.map((target) => target.platform)).toEqual([
      "win32",
      "win64",
    ]);
    expect(matrix).toEqual([
      {
        caseNames,
        debugRuntimeDirectory: "muon-core/.run/test-windows-i686-debug",
        platform: "win32",
        releaseRuntimeDirectory: "muon-core/.run/test-windows-i686-release",
        target: "windows-i686",
      },
      {
        caseNames,
        debugRuntimeDirectory: "muon-core/.run/test-windows-amd64-debug",
        platform: "win64",
        releaseRuntimeDirectory: "muon-core/.run/test-windows-amd64-release",
        target: "windows-amd64",
      },
    ]);
    expect(matrix[0]?.caseNames).toBe(matrix[1]?.caseNames);
  });

  it("defaults to the 64-bit Windows runtime target", () => {
    expect(resolveWindowsRuntimeTarget(undefined).target).toBe("windows-amd64");
    expect(resolveWindowsRuntimeTarget("").target).toBe("windows-amd64");
  });

  it("accepts target and platform aliases", () => {
    expect(resolveWindowsRuntimeTarget("windows-i686").platform).toBe("win32");
    expect(resolveWindowsRuntimeTarget("win64").target).toBe("windows-amd64");
  });

  it("rejects unknown target aliases", () => {
    expect(() => resolveWindowsRuntimeTarget("linux-amd64")).toThrow(
      "MUON_E2E_WINDOWS_TARGET must be windows-i686, win32, windows-amd64, or win64",
    );
  });
});

describe("Windows e2e paths", () => {
  it("creates file URLs from Windows drive paths on the Linux host", () => {
    expect(
      pathToWindowsFileUrlHref("C:\\muon-e2e\\windows-i686\\debug\\uri\\"),
    ).toBe("file:///C:/muon-e2e/windows-i686/debug/uri/");
  });

  it("escapes Windows file URL path characters without treating them as POSIX paths", () => {
    expect(pathToWindowsFileUrlHref("C:\\muon e2e\\#asset\\μon.txt")).toBe(
      "file:///C:/muon%20e2e/%23asset/%CE%BCon.txt",
    );
  });
});

describe("Windows e2e CDP ports", () => {
  it("skips the agent-rover port when allocating remote CDP relay ports", () => {
    setWindowsRemoteContext(createFakeWindowsRemoteContext(39396, 39397));
    try {
      expect(allocateWindowsRemoteCdpPort()).toBe(39398);
      expect(allocateWindowsRemoteCdpPort()).toBe(39399);
    } finally {
      clearWindowsRemoteContext();
    }
  });
});

describe("Windows e2e staging", () => {
  const debugRuntimeDirectory = String.raw`C:\muon-e2e\windows-i686\debug`;
  const releaseRuntimeDirectory = String.raw`C:\muon-e2e\windows-i686\release`;
  const relayExecutablePath = String.raw`C:\muon-e2e\windows-i686\muon-cdp-relay.exe`;
  const runtimeDirectories = [
    debugRuntimeDirectory,
    releaseRuntimeDirectory,
  ] as const;

  it("terminates stale processes that can hold staged runtime files", async () => {
    const fake = createFakeStagingAgent(
      [
        createProcessSnapshot(
          10,
          "muon-core.exe",
          String.raw`C:\muon-e2e\windows-i686\debug\muon-core.exe`,
          true,
        ),
        createProcessSnapshot(
          11,
          "bootstrap.exe",
          String.raw`C:\muon-e2e\windows-i686\release\bootstrap.exe`,
          true,
        ),
        createProcessSnapshot(12, "muon-cdp-relay.exe", "", true),
        createProcessSnapshot(
          13,
          "muon-core.exe",
          String.raw`C:\other\muon-core.exe`,
          true,
        ),
        createProcessSnapshot(
          14,
          "muon-core.exe",
          String.raw`C:\muon-e2e\windows-i686\debug\muon-core.exe`,
          false,
        ),
      ],
      0,
    );

    await cleanupWindowsStagedRuntimeProcesses(
      fake.agent,
      runtimeDirectories,
      relayExecutablePath,
    );

    expect(fake.getKilledProcessIds()).toEqual([10, 11, 12]);
    expect(fake.getWaitedProcessIds()).toEqual([10, 11, 12]);
  });

  it("retries staging directory removal after clearing stale locks", async () => {
    const fake = createFakeStagingAgent(
      [
        createProcessSnapshot(
          20,
          "muon-core.exe",
          String.raw`C:\muon-e2e\windows-i686\debug\muon-core.exe`,
          true,
        ),
      ],
      2,
    );

    await removeWindowsStagedRuntimeDirectory(
      fake.agent,
      debugRuntimeDirectory,
      runtimeDirectories,
      relayExecutablePath,
    );

    expect(fake.getKilledProcessIds()).toEqual([20]);
    expect(fake.getRemoveAttempts()).toBe(3);
  });
});
