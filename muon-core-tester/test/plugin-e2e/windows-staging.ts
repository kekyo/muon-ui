// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import { createHash } from "node:crypto";
import { readFile, stat } from "node:fs/promises";
import { resolve } from "node:path";

import type { RemoteAgent } from "agent-rover";

import type { WindowsE2eEnvironment } from "./windows-environment.js";
import type { WindowsRuntimeTarget } from "./windows-matrix.js";
import {
  joinWindowsPath,
  type WindowsRemoteRuntime,
} from "./windows-context.js";

const hashBuffer = (buffer: Buffer): string =>
  createHash("sha256").update(buffer).digest("hex");

const normalizeWindowsProcessPath = (path: string): string =>
  path
    .replaceAll("/", "\\")
    .replace(/[\\]+$/u, "")
    .toLowerCase();

const isWindowsProcessPathInside = (
  processPath: string,
  directory: string,
): boolean => {
  if (processPath.length === 0) {
    return false;
  }
  const normalizedProcessPath = normalizeWindowsProcessPath(processPath);
  const normalizedDirectory = normalizeWindowsProcessPath(directory);
  return (
    normalizedProcessPath === normalizedDirectory ||
    normalizedProcessPath.startsWith(`${normalizedDirectory}\\`)
  );
};

/**
 * Terminates stale remote Windows test processes that can lock staged runtime files.
 *
 * @remarks Internal e2e helper exported for infrastructure tests.
 */
export const cleanupWindowsStagedRuntimeProcesses = async (
  agent: RemoteAgent,
  runtimeDirectories: readonly string[],
  relayExecutablePath: string,
): Promise<void> => {
  const relayPath = normalizeWindowsProcessPath(relayExecutablePath);
  const processes = await agent.processes.list();
  for (const processInfo of processes) {
    const processName = processInfo.name.toLowerCase();
    const processPath = normalizeWindowsProcessPath(processInfo.path);
    const isRuntimeProcess = runtimeDirectories.some((directory) =>
      isWindowsProcessPathInside(processPath, directory),
    );
    const isRelayProcess =
      processPath === relayPath || processName === "muon-cdp-relay.exe";
    if (!processInfo.running || (!isRuntimeProcess && !isRelayProcess)) {
      continue;
    }

    try {
      await agent.processes.kill(processInfo.id);
      await agent.processes.waitForExit(processInfo.id, {
        intervalMs: 100,
        timeoutMs: 3000,
      });
    } catch {
      // The stale test process may have exited between list and kill.
    }
  }
};

/**
 * Synchronizes one local Windows runtime directory into the remote test area.
 *
 * @remarks Internal e2e helper exported for infrastructure tests.
 */
export const stageWindowsRuntimeDirectory = async (
  agent: RemoteAgent,
  localDirectory: string,
  remoteDirectory: string,
): Promise<void> => {
  const localStat = await stat(localDirectory);
  if (!localStat.isDirectory()) {
    throw new Error(`Expected Windows runtime directory: ${localDirectory}`);
  }

  await agent.files.syncDirectory({
    checksum: "sha256",
    localPath: localDirectory,
    mode: "mirror",
    onLockedFile: "killRelatedProcessesAndRetry",
    relatedProcessPaths: [remoteDirectory],
    remotePath: remoteDirectory,
  });
};

const getRemoteRelayPath = (remoteTargetRoot: string): string =>
  joinWindowsPath(remoteTargetRoot, "muon-cdp-relay.exe");

const stageRelayExecutable = async (
  agent: RemoteAgent,
  target: WindowsRuntimeTarget,
  remoteTargetRoot: string,
): Promise<string> => {
  const localRelayPath = resolve(
    "test",
    ".run",
    "windows-cdp-relay",
    target.target,
    "muon-cdp-relay.exe",
  );
  const localRelay = await readFile(localRelayPath);
  const remoteRelayPath = getRemoteRelayPath(remoteTargetRoot);
  const remoteHash = (await agent.files.exists(remoteRelayPath))
    ? hashBuffer(await agent.files.readFile(remoteRelayPath))
    : undefined;
  const localHash = hashBuffer(localRelay);
  if (remoteHash !== localHash) {
    await agent.files.mkdir(remoteTargetRoot, { recursive: true });
    await agent.files.writeFile(remoteRelayPath, localRelay);
  }
  return remoteRelayPath;
};

export const stageWindowsRuntime = async (
  agent: RemoteAgent,
  environment: WindowsE2eEnvironment,
  target: WindowsRuntimeTarget,
): Promise<WindowsRemoteRuntime> => {
  const remoteTargetRoot = joinWindowsPath(environment.workDir, target.target);
  const debugRuntimeDirectory = joinWindowsPath(remoteTargetRoot, "debug");
  const releaseRuntimeDirectory = joinWindowsPath(remoteTargetRoot, "release");
  const relayExecutablePath = getRemoteRelayPath(remoteTargetRoot);
  const runtimeDirectories = [
    debugRuntimeDirectory,
    releaseRuntimeDirectory,
  ] as const;

  await cleanupWindowsStagedRuntimeProcesses(
    agent,
    runtimeDirectories,
    relayExecutablePath,
  );

  await stageWindowsRuntimeDirectory(
    agent,
    resolve("..", target.debugRuntimeDirectory),
    debugRuntimeDirectory,
  );
  await stageWindowsRuntimeDirectory(
    agent,
    resolve("..", target.releaseRuntimeDirectory),
    releaseRuntimeDirectory,
  );
  const stagedRelayExecutablePath = await stageRelayExecutable(
    agent,
    target,
    remoteTargetRoot,
  );

  return {
    debugRuntimeDirectory,
    releaseRuntimeDirectory,
    relayExecutablePath: stagedRelayExecutablePath,
    target: target.target,
  };
};
