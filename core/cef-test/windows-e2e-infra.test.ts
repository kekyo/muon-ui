// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join, relative, resolve } from "node:path";

import { describe, expect, it } from "vitest";
import type {
  RemoteAgent,
  RemoteDirectorySyncOptions,
  RemoteDirectorySyncResult,
  RemoteProcessSnapshot,
} from "agent-rover";

import {
  createWindowsE2eMatrix,
  resolveWindowsRuntimeTarget,
  windowsRuntimeTargets,
} from "./plugin-e2e/windows-matrix.js";
import { resolveWindowsE2eEnvironment } from "../../scripts/windows-e2e-environment.js";
import { pathToWindowsFileUrlHref } from "./plugin-e2e/windows-context.js";
import {
  cleanupWindowsStagedRuntimeProcesses,
  stageWindowsRuntime,
  stageWindowsRuntimeDirectory,
} from "./plugin-e2e/windows-staging.js";

interface FakeStagingAgent {
  agent: RemoteAgent;
  getKilledProcessIds: () => number[];
  getSyncDirectoryOptions: () => RemoteDirectorySyncOptions[];
  getWaitedProcessIds: () => number[];
}

const createProcessSnapshot = (
  id: number,
  name: string,
  path: string,
  running: boolean,
): RemoteProcessSnapshot => ({
  createdAt: "2026-07-09T00:00:00.000Z",
  exitCode: running ? null : 0,
  id,
  name,
  parentProcessId: null,
  path,
  running,
});

const createFakeStagingAgent = (
  initialProcesses: readonly RemoteProcessSnapshot[],
): FakeStagingAgent => {
  let processes = [...initialProcesses];
  const killedProcessIds: number[] = [];
  const syncDirectoryOptions: RemoteDirectorySyncOptions[] = [];
  const waitedProcessIds: number[] = [];

  const agent = {
    files: {
      syncDirectory: async (
        options: RemoteDirectorySyncOptions,
      ): Promise<RemoteDirectorySyncResult> => {
        syncDirectoryOptions.push(options);
        return {
          bytesUploaded: 0,
          createdDirectories: 0,
          deletedDirectories: 0,
          deletedFiles: 0,
          skippedFiles: 0,
          uploadedFiles: 0,
        };
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
    getSyncDirectoryOptions: () => syncDirectoryOptions,
    getWaitedProcessIds: () => waitedProcessIds,
  };
};

describe("Windows e2e environment", () => {
  it("uses the win11 agent section without host or token variables", () => {
    expect(resolveWindowsE2eEnvironment({})).toEqual({
      section: "win11",
      httpHost: undefined,
      workDir: String.raw`C:\muon-e2e`,
    });
  });

  it("selects a named section and Muon-specific test paths", () => {
    expect(
      resolveWindowsE2eEnvironment({
        MUON_E2E_AGENT_SECTION: " vm ",
        MUON_E2E_REMOTE_HTTP_HOST: "10.10.0.1",
        MUON_E2E_REMOTE_WORK_DIR: String.raw`D:\muon-test`,
      }),
    ).toEqual({
      section: "vm",
      httpHost: "10.10.0.1",
      workDir: String.raw`D:\muon-test`,
    });
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
        debugRuntimeDirectory: "core/cef/.run/test-windows-i686-debug",
        platform: "win32",
        releaseRuntimeDirectory: "core/cef/.run/test-windows-i686-release",
        target: "windows-i686",
      },
      {
        caseNames,
        debugRuntimeDirectory: "core/cef/.run/test-windows-amd64-debug",
        platform: "win64",
        releaseRuntimeDirectory: "core/cef/.run/test-windows-amd64-release",
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

describe("Windows e2e staging", () => {
  const debugRuntimeDirectory = String.raw`C:\muon-e2e\windows-i686\debug`;
  const releaseRuntimeDirectory = String.raw`C:\muon-e2e\windows-i686\release`;
  const relayExecutablePath = String.raw`C:\muon-e2e\windows-i686\muon-cdp-relay.exe`;
  const runtimeDirectories = [
    debugRuntimeDirectory,
    releaseRuntimeDirectory,
  ] as const;

  it("finds repository-relative runtimes from the nested test workspace", async () => {
    const repository = resolve("../..");
    const localDirectory = await mkdtemp(
      join(repository, ".windows-staging-test-"),
    );
    const staged = new Error("Runtime upload observed");
    const agent = {
      processes: { list: async () => [] },
      files: {
        syncDirectory: async (options: RemoteDirectorySyncOptions) => {
          expect(
            await readFile(join(options.localPath, "muon-core.exe"), "utf8"),
          ).toBe("runtime from the repository root");
          throw staged;
        },
      },
    } as unknown as RemoteAgent;
    try {
      await writeFile(
        join(localDirectory, "muon-core.exe"),
        "runtime from the repository root",
      );
      await expect(
        stageWindowsRuntime(
          agent,
          {
            section: "win11",
            httpHost: "linux.test",
            workDir: String.raw`C:\muon-e2e`,
          },
          {
            ...resolveWindowsRuntimeTarget("windows-i686"),
            debugRuntimeDirectory: relative(repository, localDirectory),
          },
        ),
      ).rejects.toBe(staged);
    } finally {
      await rm(localDirectory, { recursive: true, force: true });
    }
  });

  it("terminates stale processes that can hold staged runtime files", async () => {
    const fake = createFakeStagingAgent([
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
    ]);

    await cleanupWindowsStagedRuntimeProcesses(
      fake.agent,
      runtimeDirectories,
      relayExecutablePath,
    );

    expect(fake.getKilledProcessIds()).toEqual([10, 11, 12]);
    expect(fake.getWaitedProcessIds()).toEqual([10, 11, 12]);
  });

  it("uses agent-rover directory sync for runtime staging", async () => {
    const fake = createFakeStagingAgent([]);
    const localDirectory = await mkdtemp(join(tmpdir(), "muon-runtime-"));
    try {
      await writeFile(join(localDirectory, "muon-core.exe"), "runtime");

      await stageWindowsRuntimeDirectory(
        fake.agent,
        localDirectory,
        debugRuntimeDirectory,
      );

      expect(fake.getSyncDirectoryOptions()).toEqual([
        {
          checksum: "sha256",
          localPath: localDirectory,
          mode: "mirror",
          onLockedFile: "killRelatedProcessesAndRetry",
          relatedProcessPaths: [debugRuntimeDirectory],
          remotePath: debugRuntimeDirectory,
        },
      ]);
    } finally {
      await rm(localDirectory, { force: true, recursive: true });
    }
  });
});
