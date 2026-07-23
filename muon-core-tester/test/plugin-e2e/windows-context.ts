// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { Buffer } from "node:buffer";
import { randomUUID } from "node:crypto";
import { win32 } from "node:path";

import type { RemoteAgent, RemoteProcessSnapshot } from "agent-rover";

import type { WindowsE2eEnvironment } from "./windows-environment.js";
import type { WindowsE2eTargetName } from "./windows-matrix.js";

export interface WindowsRemoteRuntime {
  debugRuntimeDirectory: string;
  releaseRuntimeDirectory: string;
  relayExecutablePath: string;
  target: WindowsE2eTargetName;
}

export interface WindowsRemoteContext {
  agent: RemoteAgent;
  cdpHost: string;
  cdpPort: number;
  environment: WindowsE2eEnvironment;
  httpHost: string;
  runtime: WindowsRemoteRuntime;
  tempDirectory: string;
}

export interface WindowsRemoteProcessHandle {
  exitCode: number | null;
  name: string;
  pid: number;
  signalCode: null;
  once: (event: "exit", listener: () => void) => WindowsRemoteProcessHandle;
}

let currentWindowsRemoteContext: WindowsRemoteContext | undefined = undefined;

export const setWindowsRemoteContext = (
  context: WindowsRemoteContext,
): void => {
  currentWindowsRemoteContext = context;
};

export const clearWindowsRemoteContext = (): void => {
  currentWindowsRemoteContext = undefined;
};

export const getWindowsRemoteContext = (): WindowsRemoteContext | undefined =>
  currentWindowsRemoteContext;

export const requireWindowsRemoteContext = (): WindowsRemoteContext => {
  const context = getWindowsRemoteContext();
  if (context === undefined) {
    throw new Error("Windows remote e2e context is not configured");
  }
  return context;
};

export const isWindowsRemoteE2e = (): boolean =>
  getWindowsRemoteContext() !== undefined ||
  process.env.MUON_E2E_REMOTE_WINDOWS === "1";

export const isWindowsAbsolutePath = (path: string): boolean =>
  /^[A-Za-z]:[\\/]/.test(path) || path.startsWith("\\\\");

export const joinWindowsPath = (...paths: string[]): string =>
  win32.join(...paths);

export const dirnameWindowsPath = (path: string): string => win32.dirname(path);

export const relativeWindowsPath = (from: string, to: string): string =>
  win32.relative(from, to);

const encodeWindowsFileUrlPath = (path: string): string =>
  path
    .split("/")
    .map((segment) => encodeURIComponent(segment))
    .join("/");

/**
 * Converts a Windows absolute path into a file URL on the Linux test host.
 *
 * @remarks
 * Node's pathToFileURL follows the host platform. Windows e2e runs on a Linux
 * host while the path belongs to the remote VM, so the conversion has to stay
 * Windows-aware here.
 */
export const pathToWindowsFileUrlHref = (path: string): string => {
  const normalized = path.replaceAll("\\", "/");
  const driveMatch = /^([A-Za-z]):(?:\/|$)/u.exec(normalized);
  const driveLetter = driveMatch?.[1];
  if (driveLetter !== undefined) {
    const drive = driveLetter.toUpperCase();
    return `file:///${drive}:${encodeWindowsFileUrlPath(normalized.slice(2))}`;
  }

  if (normalized.startsWith("//")) {
    const withoutPrefix = normalized.slice(2);
    const slashIndex = withoutPrefix.indexOf("/");
    const host =
      slashIndex < 0 ? withoutPrefix : withoutPrefix.slice(0, slashIndex);
    const rest = slashIndex < 0 ? "" : withoutPrefix.slice(slashIndex + 1);
    return `file://${encodeURIComponent(host)}/${encodeWindowsFileUrlPath(
      rest,
    )}`;
  }

  throw new Error(`Expected Windows absolute path: ${path}`);
};

export const allocateWindowsRemoteCdpPort = (): number => {
  const context = requireWindowsRemoteContext();
  do {
    context.cdpPort += 1;
  } while (context.cdpPort === context.environment.port);
  return context.cdpPort;
};

export const createWindowsRemoteProcessHandle = (
  processId: number,
  name: string,
): WindowsRemoteProcessHandle => {
  const handle: WindowsRemoteProcessHandle = {
    exitCode: null,
    name,
    pid: processId,
    signalCode: null,
    once: (_event, _listener) => handle,
  };
  return handle;
};

export const applyWindowsRemoteProcessSnapshot = (
  handle: WindowsRemoteProcessHandle,
  snapshot: RemoteProcessSnapshot,
): void => {
  if (!snapshot.running) {
    handle.exitCode = snapshot.exitCode;
  }
};

export const createWindowsRemoteTempPrefix = (prefix: string): string =>
  joinWindowsPath(requireWindowsRemoteContext().tempDirectory, prefix);

export const createFallbackWindowsRemoteTempDirectory = async (
  prefix: string,
): Promise<string> => {
  const directory = `${prefix}${randomUUID().replaceAll("-", "")}`;
  await requireWindowsRemoteContext().agent.files.mkdir(directory, {
    recursive: true,
  });
  return directory;
};

export const appendWindowsRemoteFile = async (
  path: string,
  data: Buffer,
): Promise<void> => {
  const { agent } = requireWindowsRemoteContext();
  await agent.files.mkdir(dirnameWindowsPath(path), { recursive: true });
  let existing: Buffer<ArrayBufferLike> = Buffer.alloc(0);
  if (await agent.files.exists(path)) {
    try {
      existing = await agent.files.readFile(path);
    } catch {
      // Windows log files can be held open by the process under test.
    }
  }
  await agent.files.writeFile(path, Buffer.concat([existing, data]));
};
