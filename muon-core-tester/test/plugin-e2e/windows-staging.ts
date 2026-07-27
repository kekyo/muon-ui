// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { createHash } from "node:crypto";
import { mkdtemp, readFile, rm, stat } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";

import type { RemoteAgent } from "agent-rover";

import type { WindowsE2eEnvironment } from "./windows-environment.js";
import type { WindowsRuntimeTarget } from "./windows-matrix.js";
import {
  joinWindowsPath,
  type WindowsRemoteRuntime,
} from "./windows-context.js";

const hashBuffer = (buffer: Buffer): string =>
  createHash("sha256").update(buffer).digest("hex");

const windowsNodePrepareTimeoutMs = 600000;
const windowsNodeRuntimeRequirement = JSON.stringify({
  comparatorSets: [[">=20.19.0", "<21.0.0-0"], [">=22.12.0"]],
  engineRange: ">=18",
  engineRangeSpecified: true,
  required: true,
});

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

const runWindowsNodeRuntimePrepare = async (
  agent: RemoteAgent,
  target: WindowsRuntimeTarget,
  remoteTargetRoot: string,
): Promise<string> => {
  const localBuilderPath = resolve(
    "..",
    "muon-builder",
    ".run",
    `test-${target.target}-release`,
    "muon-builder.exe",
  );
  const localBuilder = await readFile(localBuilderPath);
  const prepareRoot = joinWindowsPath(remoteTargetRoot, "node-prepare");
  const sourceDirectory = joinWindowsPath(prepareRoot, "source");
  const cefDirectory = joinWindowsPath(prepareRoot, "cef");
  const stageDirectory = joinWindowsPath(prepareRoot, "stage");
  const cacheDirectory = joinWindowsPath(
    remoteTargetRoot,
    "node-runtime-cache",
  );
  const builderPath = joinWindowsPath(prepareRoot, "muon-builder.exe");

  if (await agent.files.exists(prepareRoot)) {
    await agent.files.remove(prepareRoot, { recursive: true });
  }
  await agent.files.mkdir(sourceDirectory, { recursive: true });
  await agent.files.mkdir(cefDirectory, { recursive: true });
  await agent.files.writeFile(builderPath, localBuilder);
  await agent.files.writeFile(
    joinWindowsPath(sourceDirectory, "muon-core.exe"),
    Buffer.from("muon Node runtime staging input\r\n"),
  );
  await agent.files.writeFile(
    joinWindowsPath(cefDirectory, "libcef.dll"),
    Buffer.from("muon Node runtime CEF placeholder\r\n"),
  );

  const processInfo = await agent.processes.launchManaged({
    arguments: [
      "runtime",
      "--muon-path",
      sourceDirectory,
      "--cef-path",
      cefDirectory,
      "--stage-dir",
      stageDirectory,
      "--target",
      target.target,
      "--cache-dir",
      cacheDirectory,
      "--node-runtime-requirement",
      windowsNodeRuntimeRequirement,
      "--quiet",
      "--json",
    ],
    captureStderr: true,
    captureStdout: true,
    createNoWindow: true,
    killTreeOnRelease: true,
    path: builderPath,
    workingDirectory: prepareRoot,
  });
  let completed = false;
  try {
    const snapshot = await processInfo.waitForExit({
      intervalMs: 100,
      timeoutMs: windowsNodePrepareTimeoutMs,
    });
    completed = true;
    if (snapshot.root.exitCode !== 0) {
      const stdout = await processInfo.stdoutText();
      const stderr = await processInfo.stderrText();
      throw new Error(
        `Windows Node runtime preparation failed with ${String(
          snapshot.root.exitCode,
        )}\nstdout:\n${stdout}\nstderr:\n${stderr}`,
      );
    }
  } finally {
    if (!completed) {
      try {
        await agent.processes.kill(processInfo.id);
        await agent.processes.waitForExit(processInfo.id, {
          intervalMs: 100,
          timeoutMs: 3000,
        });
      } catch {
        // The builder may have exited between the timeout and cleanup.
      }
    }
    await processInfo.releaseAsync();
  }

  return joinWindowsPath(stageDirectory, "runtimes", "node");
};

const stageWindowsNodeSupport = async (
  agent: RemoteAgent,
  target: WindowsRuntimeTarget,
  remoteTargetRoot: string,
  debugRuntimeDirectory: string,
  releaseRuntimeDirectory: string,
): Promise<string> => {
  const remotePreparedNodeDirectory = await runWindowsNodeRuntimePrepare(
    agent,
    target,
    remoteTargetRoot,
  );
  const localDownloadRoot = await mkdtemp(
    join(tmpdir(), `muon-${target.target}-node-`),
  );
  const localNodeDirectory = join(localDownloadRoot, "node");
  try {
    await agent.files.downloadDirectory({
      localPath: localNodeDirectory,
      remotePath: remotePreparedNodeDirectory,
    });
    await stageWindowsRuntimeDirectory(
      agent,
      localNodeDirectory,
      joinWindowsPath(debugRuntimeDirectory, "runtimes", "node"),
    );
    await stageWindowsRuntimeDirectory(
      agent,
      localNodeDirectory,
      joinWindowsPath(releaseRuntimeDirectory, "runtimes", "node"),
    );
  } finally {
    await rm(localDownloadRoot, { recursive: true, force: true });
    const prepareRoot = joinWindowsPath(remoteTargetRoot, "node-prepare");
    if (await agent.files.exists(prepareRoot)) {
      await agent.files.remove(prepareRoot, { recursive: true });
    }
  }

  const nodeProjectDirectory = joinWindowsPath(
    remoteTargetRoot,
    "node-project",
  );
  await stageWindowsRuntimeDirectory(
    agent,
    resolve("test", "fixtures", "node-project"),
    nodeProjectDirectory,
  );
  return nodeProjectDirectory;
};

/**
 * Stages the Windows runtime, its managed Node.js runtime, and Node fixture.
 *
 * @param agent Connected Windows test agent.
 * @param environment Windows e2e environment.
 * @param target Target architecture and local runtime paths.
 * @returns Remote runtime paths for the selected target.
 */
export const stageWindowsRuntime = async (
  agent: RemoteAgent,
  environment: WindowsE2eEnvironment,
  target: WindowsRuntimeTarget,
): Promise<WindowsRemoteRuntime> => {
  const remoteTargetRoot = joinWindowsPath(environment.workDir, target.target);
  const debugRuntimeDirectory = joinWindowsPath(remoteTargetRoot, "debug");
  const releaseRuntimeDirectory = joinWindowsPath(remoteTargetRoot, "release");
  const nodePrepareDirectory = joinWindowsPath(
    remoteTargetRoot,
    "node-prepare",
  );
  const relayExecutablePath = getRemoteRelayPath(remoteTargetRoot);
  const runtimeDirectories = [
    debugRuntimeDirectory,
    releaseRuntimeDirectory,
    nodePrepareDirectory,
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
  const nodeProjectDirectory = await stageWindowsNodeSupport(
    agent,
    target,
    remoteTargetRoot,
    debugRuntimeDirectory,
    releaseRuntimeDirectory,
  );
  const stagedRelayExecutablePath = await stageRelayExecutable(
    agent,
    target,
    remoteTargetRoot,
  );

  return {
    debugRuntimeDirectory,
    nodeProjectDirectory,
    releaseRuntimeDirectory,
    relayExecutablePath: stagedRelayExecutablePath,
    target: target.target,
  };
};
