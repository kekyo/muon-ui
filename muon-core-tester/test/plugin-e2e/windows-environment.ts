// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

export const DEFAULT_WINDOWS_E2E_PORT = 39397;
export const DEFAULT_WINDOWS_E2E_WORK_DIR = String.raw`C:\muon-e2e`;

export interface WindowsE2eEnvironment {
  host: string;
  httpHost: string;
  port: number;
  token: string;
  workDir: string;
}

export interface ConfiguredWindowsE2eEnvironment {
  config: WindowsE2eEnvironment;
  status: "configured";
}

export interface SkippedWindowsE2eEnvironment {
  reason: string;
  status: "skip";
}

export type WindowsE2eEnvironmentResult =
  | ConfiguredWindowsE2eEnvironment
  | SkippedWindowsE2eEnvironment;

const readRequiredValue = (
  env: Readonly<Record<string, string | undefined>>,
  name: string,
): string | undefined => {
  const value = env[name];
  if (value === undefined) {
    return undefined;
  }

  const trimmed = value.trim();
  return trimmed === "" ? undefined : trimmed;
};

const readOptionalWorkDir = (
  env: Readonly<Record<string, string | undefined>>,
): string => {
  const value = env.AGENT_ROVER_WIN11_WORK_DIR;
  if (value === undefined || value.trim() === "") {
    return DEFAULT_WINDOWS_E2E_WORK_DIR;
  }
  return value;
};

const deriveDefaultHttpHost = (agentHost: string): string => {
  const match = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.\d{1,3}$/.exec(agentHost);
  if (match === null) {
    return agentHost;
  }
  return `${match[1]}.${match[2]}.${match[3]}.1`;
};

const readOptionalHttpHost = (
  env: Readonly<Record<string, string | undefined>>,
  agentHost: string,
): string => {
  const value = env.MUON_E2E_REMOTE_HTTP_HOST;
  if (value === undefined || value.trim() === "") {
    return deriveDefaultHttpHost(agentHost);
  }
  return value.trim();
};

const readOptionalPort = (
  env: Readonly<Record<string, string | undefined>>,
): number => {
  const value = env.AGENT_ROVER_WIN11_PORT;
  if (value === undefined || value.trim() === "") {
    return DEFAULT_WINDOWS_E2E_PORT;
  }

  const trimmed = value.trim();
  if (!/^[0-9]+$/.test(trimmed)) {
    throw new Error(
      "AGENT_ROVER_WIN11_PORT must be an integer from 1 to 65535",
    );
  }

  const port = Number(trimmed);
  if (!Number.isSafeInteger(port) || port < 1 || port > 65535) {
    throw new Error(
      "AGENT_ROVER_WIN11_PORT must be an integer from 1 to 65535",
    );
  }
  return port;
};

export const parseWindowsE2eEnvironment = (
  env: Readonly<Record<string, string | undefined>>,
): WindowsE2eEnvironmentResult => {
  const host = readRequiredValue(env, "AGENT_ROVER_WIN11_HOST");
  if (host === undefined) {
    return {
      reason: "AGENT_ROVER_WIN11_HOST is not set",
      status: "skip",
    };
  }

  const token = readRequiredValue(env, "AGENT_ROVER_WIN11_TOKEN");
  if (token === undefined) {
    return {
      reason: "AGENT_ROVER_WIN11_TOKEN is not set",
      status: "skip",
    };
  }

  return {
    config: {
      host,
      httpHost: readOptionalHttpHost(env, host),
      port: readOptionalPort(env),
      token,
      workDir: readOptionalWorkDir(env),
    },
    status: "configured",
  };
};
