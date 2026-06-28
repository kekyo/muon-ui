// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import { spawn } from "node:child_process";
import { constants } from "node:fs";
import { access } from "node:fs/promises";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import {
  getDefaultMuonTarget,
  normalizeMuonTarget,
  type MuonTarget,
} from "./targets.js";

/**
 * Options used to invoke the native Muon prepare helper.
 */
export interface MuonPrepareOptions {
  /**
   * Directory containing muon-core runtime files.
   */
  muonPath: string;

  /**
   * Directory containing CEF files, or a CEF archive root with Release/Resources.
   */
  cefPath: string | undefined;

  /**
   * Directory where the executable runtime is staged.
   */
  stageDir: string | undefined;

  /**
   * Public target runtime platform such as linux-amd64 or windows-amd64.
   */
  target: string | undefined;

  /**
   * Cache directory passed to muon-prepare.
   */
  cacheDir: string | undefined;

  /**
   * Rebuild an existing prepared runtime.
   */
  force: boolean;

  /**
   * Suppress progress messages from the native prepare process.
   */
  quiet: boolean;

  /**
   * Explicit native muon-prepare executable path.
   */
  prepareExecutablePath: string | undefined;

  /**
   * Environment used for the child process.
   */
  environment: NodeJS.ProcessEnv;

  /**
   * Working directory used for the child process.
   */
  cwd: string | undefined;
}

/**
 * Result returned by native muon-prepare.
 */
export interface MuonPrepareResult {
  /**
   * Directory containing the staged Muon runtime.
   */
  stagePath?: string;

  /**
   * Directory containing muon-core files used as the staging source.
   */
  muonPath: string;

  /**
   * Directory or cached archive containing CEF files used as the staging source.
   */
  cefPath: string;

  /**
   * True when an existing prepared runtime was reused.
   */
  cacheHit: boolean;
}

/**
 * Returns the default Muon prepare target for a Node platform and architecture.
 *
 * @param platform Node platform.
 * @param architecture Node architecture.
 * @returns Muon prepare target name.
 */
export const getDefaultMuonPrepareTarget = (
  platform: NodeJS.Platform,
  architecture: NodeJS.Architecture,
): MuonTarget => {
  try {
    return getDefaultMuonTarget(platform, architecture);
  } catch {
    throw new Error(
      `Unsupported Muon prepare target: platform=${platform}, arch=${architecture}`,
    );
  }
};

const getPrepareExecutableName = (platform: NodeJS.Platform): string =>
  platform === "win32" ? "muon-prepare.exe" : "muon-prepare";

const moduleDirectory =
  typeof __dirname === "string"
    ? __dirname
    : dirname(fileURLToPath(import.meta.url));

const canExecute = async (path: string): Promise<boolean> => {
  try {
    await access(path, constants.X_OK);
    return true;
  } catch {
    return false;
  }
};

const resolveMuonPrepareExecutable = async (
  options: MuonPrepareOptions,
): Promise<string> => {
  const explicit =
    options.prepareExecutablePath ?? options.environment.MUON_PREPARE_PATH;
  if (explicit !== undefined && explicit !== "") {
    return explicit;
  }
  const executableName = getPrepareExecutableName(process.platform);
  const target = getDefaultMuonPrepareTarget(process.platform, process.arch);
  const candidates = [
    join(moduleDirectory, "native", target, executableName),
    join(moduleDirectory, "..", "dist", "native", target, executableName),
  ];
  for (const candidate of candidates) {
    if (await canExecute(candidate)) {
      return candidate;
    }
  }
  return candidates[0] ?? executableName;
};

const createMuonPrepareArguments = (options: MuonPrepareOptions): string[] => {
  const args = ["runtime", "--muon-path", options.muonPath, "--json"];
  if (options.cefPath !== undefined) {
    args.push("--cef-path", options.cefPath);
  }
  if (options.stageDir !== undefined) {
    args.push("--stage-dir", options.stageDir);
  }
  if (options.target !== undefined) {
    args.push(
      "--target",
      normalizeMuonTarget(options.target, "Muon prepare target"),
    );
  }
  if (options.cacheDir !== undefined) {
    args.push("--cache-dir", options.cacheDir);
  }
  if (options.force) {
    args.push("--force");
  }
  if (options.quiet) {
    args.push("--quiet");
  }
  return args;
};

/**
 * Invokes the native muon-prepare executable and returns the prepared runtime.
 *
 * @param options Native prepare invocation options.
 * @returns Prepared runtime location.
 */
export const runMuonPrepare = async (
  options: MuonPrepareOptions,
): Promise<MuonPrepareResult> => {
  const executable = await resolveMuonPrepareExecutable(options);
  const args = createMuonPrepareArguments(options);
  const child = spawn(executable, args, {
    cwd: options.cwd,
    env: options.environment,
    stdio: ["ignore", "pipe", "pipe"],
  });
  let stdout = "";
  let stderr = "";
  child.stdout.setEncoding("utf8");
  child.stderr.setEncoding("utf8");
  child.stdout.on("data", (chunk: string) => {
    stdout += chunk;
  });
  child.stderr.on("data", (chunk: string) => {
    stderr += chunk;
    if (!options.quiet) {
      process.stderr.write(chunk);
    }
  });
  const exitCode = await new Promise<number>((resolvePromise, reject) => {
    child.on("error", reject);
    child.on("close", (code) => {
      resolvePromise(code ?? 1);
    });
  });
  if (exitCode !== 0) {
    throw new Error(
      `muon-prepare failed with exit code ${exitCode}.\n${stderr.trim()}`,
    );
  }
  const result = JSON.parse(stdout) as Partial<MuonPrepareResult>;
  if (
    (result.stagePath !== undefined && typeof result.stagePath !== "string") ||
    typeof result.muonPath !== "string" ||
    typeof result.cefPath !== "string" ||
    typeof result.cacheHit !== "boolean"
  ) {
    throw new Error(`muon-prepare returned invalid JSON: ${stdout}`);
  }
  return {
    ...(result.stagePath === undefined ? {} : { stagePath: result.stagePath }),
    muonPath: result.muonPath,
    cefPath: result.cefPath,
    cacheHit: result.cacheHit,
  };
};
