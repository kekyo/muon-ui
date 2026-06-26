// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import { connectRemoteAgent, type RemoteAgent } from "agent-rover";

import type { WindowsE2eEnvironment } from "./windows-environment.js";

export const WINDOWS_AGENT_CONNECTION_TIMEOUT_MS = 30000;

export const requiredWindowsAgentFeatureNames = [
  "applications.launch",
  "agent.screenshot",
  "file.exists",
  "file.mkdir",
  "file.read",
  "file.remove",
  "file.stat",
  "file.write",
  "input.perform",
  "process.kill",
  "process.list",
  "process.snapshot",
  "window.screenshot",
  "windows",
] as const;

export const connectWindowsAgent = async (
  config: WindowsE2eEnvironment,
): Promise<RemoteAgent> =>
  await connectRemoteAgent({
    authToken: config.token,
    host: config.host,
    port: config.port,
    timeoutMs: WINDOWS_AGENT_CONNECTION_TIMEOUT_MS,
  });
