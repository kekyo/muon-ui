// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

/** Muon-specific settings; agent-rover owns connection configuration. */
export interface WindowsE2eEnvironment {
  /** Section selected by the agent-rover SDK. */
  section: string;
  /** Optional HTTP fixture address override for routed test networks. */
  httpHost: string | undefined;
  /** Directory on the selected Windows machine for test artifacts. */
  workDir: string;
}

/** Whether the caller explicitly requested tests on a shared Windows machine. */
export type WindowsE2eEnvironmentResult =
  | { config: WindowsE2eEnvironment; status: "configured" }
  | { reason: string; status: "skip" };

/**
 * Resolves settings for both Windows runtime and installer test entry points.
 * @param env Environment variables supplied to the test runner.
 * @returns Muon settings when enabled, otherwise an explicit skip reason.
 * @remarks Connection credentials and busy retries are handled by agent-rover.
 */
export const resolveWindowsE2eEnvironment = (
  env: Readonly<Record<string, string | undefined>>,
): WindowsE2eEnvironmentResult => {
  if (env.MUON_E2E_REMOTE_WINDOWS !== "1") {
    return { reason: "MUON_E2E_REMOTE_WINDOWS is not set", status: "skip" };
  }
  return {
    config: {
      section: env.MUON_E2E_AGENT_SECTION?.trim() || "default",
      httpHost: env.MUON_E2E_REMOTE_HTTP_HOST?.trim() || undefined,
      workDir: env.MUON_E2E_REMOTE_WORK_DIR?.trim() || String.raw`C:\muon-e2e`,
    },
    status: "configured",
  };
};
