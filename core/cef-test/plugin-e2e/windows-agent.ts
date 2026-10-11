// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import {
  connectRemoteAgent,
  type RemoteAgent,
  type RemoteApplicationLaunchOptions,
  type RemoteManagedProcess,
} from "agent-rover";

import type { WindowsE2eEnvironment } from "../../../scripts/windows-e2e-environment.js";
import { joinWindowsPath } from "./windows-context.js";

/** Process operations used by the Windows runtime tests. */
export type WindowsTestProcess = Pick<
  RemoteManagedProcess,
  | "id"
  | "name"
  | "rootSnapshot"
  | "snapshot"
  | "kill"
  | "stdoutText"
  | "stderrText"
  | "releaseAsync"
>;

/**
 * Starts a runtime with an independent lifetime for daemon lifecycle tests.
 * @param agent Connected Windows agent.
 * @param options Application executable, arguments, and environment.
 * @param captureDirectory Existing directory receiving diagnostic logs.
 * @param useManagedJob Whether the agent should own the process tree.
 * @returns Runtime process controls; independent launches retain their log files.
 * @remarks A managed process remains in an agent job even when
 * `killTreeOnRelease` is false, preventing daemon descendants from breaking away.
 */
export const launchWindowsTestProcess = async (
  agent: RemoteAgent,
  options: RemoteApplicationLaunchOptions,
  captureDirectory: string,
  useManagedJob: boolean,
): Promise<WindowsTestProcess> => {
  if (useManagedJob) {
    return await agent.processes.launchManaged({
      ...options,
      captureStdout: true,
      captureStderr: true,
    });
  }

  const stdoutPath = joinWindowsPath(captureDirectory, "muon-stdout.log");
  const stderrPath = joinWindowsPath(captureDirectory, "muon-stderr.log");
  const process = await agent.applications.launch({
    ...options,
    stdoutPath,
    stderrPath,
  });
  return {
    ...process,
    rootSnapshot: async () => await agent.processes.snapshot(process.id),
    snapshot: async () => {
      const root = await agent.processes.snapshot(process.id);
      const allProcesses = await agent.processes.list();
      const familyIds = new Set([process.id]);
      let previousSize = 0;
      while (previousSize !== familyIds.size) {
        previousSize = familyIds.size;
        for (const child of allProcesses) {
          if (
            child.parentProcessId !== null &&
            familyIds.has(child.parentProcessId)
          ) {
            familyIds.add(child.id);
          }
        }
      }
      const processes = allProcesses.filter(
        (child) => child.id !== process.id && familyIds.has(child.id),
      );
      return {
        root,
        processes,
        running: root.running || processes.some((child) => child.running),
      };
    },
    kill: async () => await agent.processes.kill(process.id),
    stdoutText: async () =>
      (await agent.files.readFile(stdoutPath)).toString("utf8"),
    stderrText: async () =>
      (await agent.files.readFile(stderrPath)).toString("utf8"),
    // Independent launches have no agent-owned handle to release. The test
    // closes Muon and explicitly cleans up each daemon fixture it creates.
    releaseAsync: async () => {},
  };
};

export const WINDOWS_AGENT_CONNECTION_TIMEOUT_MS = 30000;

export const requiredWindowsAgentProtocolVersion = "2026-09-09.1";

export const requiredWindowsAgentFeatureNames = [
  "applications.launch",
  "agent.screenshot",
  "file.exists",
  "file.manifest",
  "file.mkdir",
  "file.mkdtemp",
  "file.read",
  "file.remove",
  "file.rename",
  "file.stat",
  "file.write",
  "input.perform",
  "process.kill",
  "process.launchManaged",
  "process.list",
  "process.managedSnapshot",
  "process.releaseManaged",
  "process.snapshot",
  "window.screenshot",
  "windows",
] as const;

export const connectWindowsAgent = async (
  config: WindowsE2eEnvironment,
): Promise<RemoteAgent> =>
  await connectRemoteAgent({
    section: config.section,
    timeoutMs: WINDOWS_AGENT_CONNECTION_TIMEOUT_MS,
  });
