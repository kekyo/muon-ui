// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import { spawn } from "node:child_process";

type JsonObject = Record<string, unknown>;

/**
 * Windows executable artifact kind accepted by code signing options.
 */
export type MuonWindowsCodeSigningTarget =
  | "runtime"
  | "launcher"
  | "nsisInstaller"
  | "nsisUninstaller";

/**
 * External Windows code signing command options.
 */
export interface MuonWindowsCodeSigningOptions {
  /**
   * Executable or script used to sign one file.
   */
  command: string;
  /**
   * Arguments passed to the signing command.
   *
   * @remarks `{path}` is required and is replaced with the executable path.
   * `{target}` and `{kind}` are also replaced when present.
   */
  args: readonly string[];
  /**
   * Artifact kinds to sign.
   *
   * @defaultValue Signs all supported Windows artifact kinds.
   */
  targets?: readonly MuonWindowsCodeSigningTarget[];
}

/**
 * Resolved Windows code signing command.
 */
export interface ResolvedMuonWindowsCodeSigning {
  /** Executable or script used to sign one file. */
  command: string;
  /** Arguments passed to the signing command. */
  args: readonly string[];
  /** Artifact kinds to sign. */
  targets: readonly MuonWindowsCodeSigningTarget[];
}

interface WindowsCodeSigningExecution {
  /** Resolved signing command. */
  codeSigning: ResolvedMuonWindowsCodeSigning | undefined;
  /** Artifact kind being signed. */
  kind: MuonWindowsCodeSigningTarget;
  /** Public muon target identifier. */
  target: string;
  /** Executable path being signed. */
  path: string;
  /** Child process working directory. */
  cwd: string;
  /** Child process environment. */
  environment: NodeJS.ProcessEnv;
}

const placeholderPath = "{path}";
const placeholderTarget = "{target}";
const placeholderKind = "{kind}";

const allWindowsCodeSigningTargets = [
  "runtime",
  "launcher",
  "nsisInstaller",
  "nsisUninstaller",
] as const satisfies readonly MuonWindowsCodeSigningTarget[];

const isJsonObject = (value: unknown): value is JsonObject =>
  typeof value === "object" && value !== null && !Array.isArray(value);

const isStringArray = (value: unknown): value is readonly string[] =>
  Array.isArray(value) && value.every((entry) => typeof entry === "string");

const isWindowsCodeSigningTarget = (
  value: string,
): value is MuonWindowsCodeSigningTarget =>
  allWindowsCodeSigningTargets.includes(value as MuonWindowsCodeSigningTarget);

const readWindowsCodeSigningTargets = (
  value: unknown,
  label: string,
): readonly MuonWindowsCodeSigningTarget[] => {
  if (value === undefined) {
    return allWindowsCodeSigningTargets;
  }
  if (!isStringArray(value)) {
    throw new Error(`${label}.targets must be an array of strings.`);
  }
  if (value.length === 0) {
    throw new Error(`${label}.targets must not be empty.`);
  }
  const targets: MuonWindowsCodeSigningTarget[] = [];
  for (const target of value) {
    if (!isWindowsCodeSigningTarget(target)) {
      throw new Error(`Unsupported ${label}.targets entry: ${target}`);
    }
    if (!targets.includes(target)) {
      targets.push(target);
    }
  }
  return targets;
};

/**
 * Validates Windows code signing options.
 */
export const validateMuonWindowsCodeSigningOptions = (
  value: unknown,
  label: string,
): MuonWindowsCodeSigningOptions => {
  if (!isJsonObject(value)) {
    throw new Error(`${label} must be an object.`);
  }
  if (typeof value.command !== "string" || value.command.trim() === "") {
    throw new Error(`${label}.command must be a non-empty string.`);
  }
  if (!isStringArray(value.args)) {
    throw new Error(`${label}.args must be an array of strings.`);
  }
  if (!value.args.some((arg) => arg.includes(placeholderPath))) {
    throw new Error(`${label}.args must contain ${placeholderPath}.`);
  }

  return {
    command: value.command.trim(),
    args: value.args,
    targets: readWindowsCodeSigningTargets(value.targets, label),
  };
};

/**
 * Returns true when a value is valid Windows code signing options.
 */
export const isMuonWindowsCodeSigningOptions = (
  value: unknown,
): value is MuonWindowsCodeSigningOptions => {
  try {
    validateMuonWindowsCodeSigningOptions(value, "windowsCodeSigning");
    return true;
  } catch {
    return false;
  }
};

/**
 * Resolves Windows code signing options from API options and muon config.
 */
export const resolveMuonWindowsCodeSigning = (input: {
  muonConfig: JsonObject;
  options: false | MuonWindowsCodeSigningOptions | undefined;
}): ResolvedMuonWindowsCodeSigning | undefined => {
  if (input.options === false) {
    return undefined;
  }
  if (input.options !== undefined) {
    return validateMuonWindowsCodeSigningOptions(
      input.options,
      "windowsCodeSigning",
    ) as ResolvedMuonWindowsCodeSigning;
  }

  const windows = input.muonConfig.windows;
  if (windows === undefined) {
    return undefined;
  }
  if (!isJsonObject(windows)) {
    throw new Error("muon.json windows must be an object when present.");
  }
  const codeSigning = windows.codeSigning;
  if (codeSigning === undefined) {
    return undefined;
  }
  return validateMuonWindowsCodeSigningOptions(
    codeSigning,
    "muon.json windows.codeSigning",
  ) as ResolvedMuonWindowsCodeSigning;
};

/**
 * Removes build-only Windows code signing settings from runtime config.
 */
export const stripBuildOnlyWindowsCodeSigningConfig = (
  sourceConfig: JsonObject,
): JsonObject => {
  const sourceWindows = sourceConfig.windows;
  if (!isJsonObject(sourceWindows)) {
    return sourceConfig;
  }

  const windowsConfig: JsonObject = {};
  for (const [key, value] of Object.entries(sourceWindows)) {
    if (key !== "codeSigning") {
      windowsConfig[key] = value;
    }
  }

  const output: JsonObject = {};
  for (const [key, value] of Object.entries(sourceConfig)) {
    if (key !== "windows") {
      output[key] = value;
    }
  }
  if (Object.keys(windowsConfig).length > 0) {
    output.windows = windowsConfig;
  }
  return output;
};

/**
 * Applies supported placeholders to signing command arguments.
 */
export const createWindowsCodeSigningArgs = (input: {
  args: readonly string[];
  kind: MuonWindowsCodeSigningTarget;
  target: string;
  path: string;
}): string[] =>
  input.args.map((arg) =>
    arg
      .replaceAll(placeholderPath, input.path)
      .replaceAll(placeholderTarget, input.target)
      .replaceAll(placeholderKind, input.kind),
  );

/**
 * Returns true when a resolved signing command includes an artifact kind.
 */
export const includesWindowsCodeSigningTarget = (
  codeSigning: ResolvedMuonWindowsCodeSigning | undefined,
  kind: MuonWindowsCodeSigningTarget,
): codeSigning is ResolvedMuonWindowsCodeSigning =>
  codeSigning !== undefined && codeSigning.targets.includes(kind);

/**
 * Runs the configured Windows code signing command for one executable.
 */
export const signWindowsExecutable = async (
  input: WindowsCodeSigningExecution,
): Promise<void> => {
  if (!includesWindowsCodeSigningTarget(input.codeSigning, input.kind)) {
    return;
  }
  const args = createWindowsCodeSigningArgs({
    args: input.codeSigning.args,
    kind: input.kind,
    target: input.target,
    path: input.path,
  });
  const child = spawn(input.codeSigning.command, args, {
    cwd: input.cwd,
    env: input.environment,
    stdio: ["ignore", "pipe", "pipe"],
  });
  let stderr = "";
  child.stderr.setEncoding("utf8");
  child.stderr.on("data", (chunk: string) => {
    stderr += chunk;
  });
  const exitCode = await new Promise<number>((resolvePromise, reject) => {
    child.once("error", reject);
    child.once("close", (code) => {
      resolvePromise(code ?? 1);
    });
  });
  if (exitCode !== 0) {
    throw new Error(
      `${input.codeSigning.command} failed while signing ${input.kind} ${input.path} with exit code ${exitCode}: ${stderr.trim()}`,
    );
  }
};
