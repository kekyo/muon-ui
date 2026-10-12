// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { connectRemoteAgent, type RemoteAgent } from "agent-rover";

/** Muon-specific settings; agent-rover owns connection configuration. */
export interface WindowsE2eEnvironment {
  /** Section selected by the agent-rover SDK. */
  section: string;
  /** Optional HTTP fixture address override for routed test networks. */
  httpHost: string | undefined;
  /** Directory on the selected Windows machine for test artifacts. */
  workDir: string;
}

/** A connected test machine, or the reason no connection could be established. */
export type WindowsE2eConnection =
  | {
      /** Session owned by the caller, which must release it after testing. */
      agent: RemoteAgent;
      /** Indicates that remote tests can run. */
      status: "connected";
    }
  | {
      /** SDK connection or configuration error, without credentials. */
      reason: string;
      /** Indicates that no remote session was established. */
      status: "skip";
    };

/**
 * Resolves settings for both Windows runtime and installer test entry points.
 * @param env Environment variables supplied to the test runner.
 * @returns Muon settings for ordinary and dedicated Windows test commands.
 * @remarks Connection credentials and busy retries are handled by agent-rover.
 */
export const resolveWindowsE2eEnvironment = (
  env: Readonly<Record<string, string | undefined>>,
): WindowsE2eEnvironment => ({
  section: env.MUON_E2E_AGENT_SECTION?.trim() || "win11",
  httpHost: env.MUON_E2E_REMOTE_HTTP_HOST?.trim() || undefined,
  workDir: env.MUON_E2E_REMOTE_WORK_DIR?.trim() || String.raw`C:\muon-e2e`,
});

/**
 * Attempts a standard SDK connection before deciding whether to skip tests.
 * @param config Muon settings, including the SDK configuration section.
 * @returns A session to release after testing, or a connection failure reason.
 * @remarks SDK retries remain enabled. Only establishing the connection is
 * caught here; preparation, remote operations, and assertions must still fail.
 */
export const connectWindowsE2eAgent = async (
  config: WindowsE2eEnvironment,
): Promise<WindowsE2eConnection> => {
  try {
    const agent = await connectRemoteAgent({
      section: config.section,
      timeoutMs: 30000,
    });
    return { agent, status: "connected" };
  } catch (error) {
    return {
      reason: error instanceof Error ? error.message : String(error),
      status: "skip",
    };
  }
};
