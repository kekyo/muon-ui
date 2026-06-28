// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import { createHash } from "node:crypto";
import { readdir, readFile, stat } from "node:fs/promises";
import { join, relative, resolve } from "node:path";

import type { RemoteAgent } from "agent-rover";

import type { WindowsE2eEnvironment } from "./windows-environment.js";
import type { WindowsRuntimeTarget } from "./windows-matrix.js";
import {
  joinWindowsPath,
  type WindowsRemoteRuntime,
} from "./windows-context.js";

interface DirectoryManifestFile {
  hash: string;
  path: string;
  size: number;
}

interface DirectoryManifest {
  files: DirectoryManifestFile[];
  version: 1;
}

const manifestFileName = ".muon-e2e-manifest.json";
const stagingRemoveRetryDelaysMs = [100, 250, 500, 1000] as const;

const toRemoteRelativePath = (path: string): string =>
  path.split(/[\\/]+/).join("\\");

const hashBuffer = (buffer: Buffer): string =>
  createHash("sha256").update(buffer).digest("hex");

const wait = async (delayMs: number): Promise<void> => {
  await new Promise<void>((resolve) => {
    setTimeout(resolve, delayMs);
  });
};

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
 * Removes a staged remote Windows runtime directory after clearing stale locks.
 *
 * @remarks Internal e2e helper exported for infrastructure tests.
 */
export const removeWindowsStagedRuntimeDirectory = async (
  agent: RemoteAgent,
  remoteDirectory: string,
  runtimeDirectories: readonly string[],
  relayExecutablePath: string,
): Promise<void> => {
  let lastError: unknown = undefined;
  for (const delayMs of [0, ...stagingRemoveRetryDelaysMs]) {
    if (delayMs > 0) {
      await wait(delayMs);
    }
    await cleanupWindowsStagedRuntimeProcesses(
      agent,
      runtimeDirectories,
      relayExecutablePath,
    );
    try {
      await agent.files.remove(remoteDirectory, { recursive: true });
      return;
    } catch (error) {
      lastError = error;
    }
  }

  if (lastError instanceof Error) {
    throw lastError;
  }
  throw new Error(
    `Failed to remove staged Windows runtime: ${remoteDirectory}`,
  );
};

const collectDirectoryManifest = async (
  rootDirectory: string,
): Promise<DirectoryManifest> => {
  const files: DirectoryManifestFile[] = [];

  const visit = async (directory: string): Promise<void> => {
    const entries = await readdir(directory, { withFileTypes: true });
    for (const entry of entries) {
      const fullPath = join(directory, entry.name);
      if (entry.isDirectory()) {
        await visit(fullPath);
        continue;
      }
      if (!entry.isFile()) {
        continue;
      }

      const data = await readFile(fullPath);
      files.push({
        hash: hashBuffer(data),
        path: relative(rootDirectory, fullPath)
          .split(/[\\/]+/)
          .join("/"),
        size: data.byteLength,
      });
    }
  };

  await visit(rootDirectory);
  files.sort((left, right) => left.path.localeCompare(right.path));
  return { files, version: 1 };
};

const readRemoteManifest = async (
  agent: RemoteAgent,
  remoteDirectory: string,
): Promise<string | undefined> => {
  const manifestPath = joinWindowsPath(remoteDirectory, manifestFileName);
  if (!(await agent.files.exists(manifestPath))) {
    return undefined;
  }
  return (await agent.files.readFile(manifestPath)).toString("utf8");
};

const uploadDirectory = async (
  agent: RemoteAgent,
  localDirectory: string,
  remoteDirectory: string,
): Promise<void> => {
  const visit = async (directory: string): Promise<void> => {
    const entries = await readdir(directory, { withFileTypes: true });
    for (const entry of entries) {
      const fullPath = join(directory, entry.name);
      const remotePath = joinWindowsPath(
        remoteDirectory,
        toRemoteRelativePath(relative(localDirectory, fullPath)),
      );
      if (entry.isDirectory()) {
        await agent.files.mkdir(remotePath, { recursive: true });
        await visit(fullPath);
        continue;
      }
      if (!entry.isFile()) {
        continue;
      }

      await agent.files.mkdir(joinWindowsPath(remotePath, ".."), {
        recursive: true,
      });
      await agent.files.writeFile(remotePath, await readFile(fullPath));
    }
  };

  await agent.files.mkdir(remoteDirectory, { recursive: true });
  await visit(localDirectory);
};

const stageRuntimeDirectory = async (
  agent: RemoteAgent,
  localDirectory: string,
  remoteDirectory: string,
  runtimeDirectories: readonly string[],
  relayExecutablePath: string,
): Promise<void> => {
  const localStat = await stat(localDirectory);
  if (!localStat.isDirectory()) {
    throw new Error(`Expected Windows runtime directory: ${localDirectory}`);
  }

  const manifest = JSON.stringify(
    await collectDirectoryManifest(localDirectory),
    null,
    2,
  );
  const remoteManifest = await readRemoteManifest(agent, remoteDirectory);
  const remoteExecutable = joinWindowsPath(remoteDirectory, "muon-core.exe");
  if (
    remoteManifest === manifest &&
    (await agent.files.exists(remoteExecutable))
  ) {
    return;
  }

  if (await agent.files.exists(remoteDirectory)) {
    await removeWindowsStagedRuntimeDirectory(
      agent,
      remoteDirectory,
      runtimeDirectories,
      relayExecutablePath,
    );
  }
  await uploadDirectory(agent, localDirectory, remoteDirectory);
  await agent.files.writeFile(
    joinWindowsPath(remoteDirectory, manifestFileName),
    Buffer.from(manifest, "utf8"),
  );
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

  await stageRuntimeDirectory(
    agent,
    resolve("..", target.debugRuntimeDirectory),
    debugRuntimeDirectory,
    runtimeDirectories,
    relayExecutablePath,
  );
  await stageRuntimeDirectory(
    agent,
    resolve("..", target.releaseRuntimeDirectory),
    releaseRuntimeDirectory,
    runtimeDirectories,
    relayExecutablePath,
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
