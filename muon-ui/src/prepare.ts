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
import {
  createMuonProgressRenderer,
  type MuonProgressEvent,
} from "./progress.js";

/**
 * Options used to invoke the native Muon builder helper.
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
   * Cache directory passed to muon-builder.
   */
  cacheDir: string | undefined;

  /**
   * Rebuild an existing prepared runtime.
   */
  force: boolean;

  /**
   * Suppress progress messages from the native builder process.
   */
  quiet: boolean;

  /**
   * Explicit native muon-builder executable path.
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
 * Options used to update Windows PE resources through the native Muon helper.
 */
export interface MuonPrepareResourceUpdateOptions {
  /**
   * Input PE executable path.
   */
  inputPath: string;

  /**
   * Engraver update JSON path.
   */
  updatesJsonPath: string;

  /**
   * Output PE executable path.
   *
   * @remarks This must be different from `inputPath`.
   */
  outputPath: string;

  /**
   * Suppress progress messages from the native builder process.
   */
  quiet: boolean;

  /**
   * Explicit native muon-builder executable path.
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
 * Result returned by native muon-builder.
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

const getBuilderExecutableName = (platform: NodeJS.Platform): string =>
  platform === "win32" ? "muon-builder.exe" : "muon-builder";

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

const resolveMuonBuilderExecutable = async (
  options: Pick<MuonPrepareOptions, "prepareExecutablePath" | "environment">,
): Promise<string> => {
  const explicit =
    options.prepareExecutablePath ?? options.environment.MUON_BUILDER_PATH;
  if (explicit !== undefined && explicit !== "") {
    return explicit;
  }
  const executableName = getBuilderExecutableName(process.platform);
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
  if (!options.quiet) {
    args.push("--progress-json");
  }
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

interface MuonPrepareStderrForwarder {
  write(chunk: string): void;
  flush(): void;
}

const createPlainStderrForwarder = (): MuonPrepareStderrForwarder => ({
  write: (chunk): void => {
    process.stderr.write(chunk);
  },
  flush: (): void => {},
});

const parseProgressJsonLine = (line: string): MuonProgressEvent | undefined => {
  const trimmed = line.trim();
  if (!trimmed.startsWith("{")) {
    return undefined;
  }
  let parsed: unknown;
  try {
    parsed = JSON.parse(trimmed);
  } catch {
    return undefined;
  }
  if (typeof parsed !== "object" || parsed === null) {
    return undefined;
  }
  const event = parsed as Record<string, unknown>;
  if (typeof event.phase !== "string" || typeof event.status !== "string") {
    return undefined;
  }
  const current =
    typeof event.current === "number" && Number.isFinite(event.current)
      ? event.current
      : undefined;
  const total =
    typeof event.total === "number" && Number.isFinite(event.total)
      ? event.total
      : undefined;
  const determinate =
    typeof event.determinate === "boolean" ? event.determinate : undefined;
  return {
    phase: event.phase,
    status: event.status,
    ...(current === undefined ? {} : { current }),
    ...(total === undefined ? {} : { total }),
    ...(determinate === undefined ? {} : { determinate }),
  };
};

const createProgressJsonStderrForwarder = (): MuonPrepareStderrForwarder => {
  let pending = "";
  const renderer = createMuonProgressRenderer();

  const writeLine = (line: string): void => {
    const event = parseProgressJsonLine(line);
    if (event !== undefined) {
      renderer.report(event);
    }
  };

  return {
    write: (chunk): void => {
      pending += chunk;
      for (;;) {
        const newlineIndex = pending.indexOf("\n");
        if (newlineIndex < 0) {
          break;
        }
        const line = pending.slice(0, newlineIndex + 1);
        pending = pending.slice(newlineIndex + 1);
        writeLine(line);
      }
    },
    flush: (): void => {
      if (pending.length > 0) {
        const line = pending;
        pending = "";
        writeLine(line);
      }
      renderer.flush();
    },
  };
};

const runMuonPrepareCommand = async (
  options: Pick<MuonPrepareOptions, "prepareExecutablePath" | "environment"> & {
    args: readonly string[];
    cwd: string | undefined;
    quiet: boolean;
    parseProgressJson: boolean;
  },
): Promise<string> => {
  const executable = await resolveMuonBuilderExecutable(options);
  const child = spawn(executable, [...options.args], {
    cwd: options.cwd,
    env: options.environment,
    stdio: ["ignore", "pipe", "pipe"],
  });
  let stdout = "";
  let stderr = "";
  child.stdout.setEncoding("utf8");
  child.stderr.setEncoding("utf8");
  const stderrForwarder = options.quiet
    ? undefined
    : options.parseProgressJson
      ? createProgressJsonStderrForwarder()
      : createPlainStderrForwarder();
  child.stdout.on("data", (chunk: string) => {
    stdout += chunk;
  });
  child.stderr.on("data", (chunk: string) => {
    stderr += chunk;
    stderrForwarder?.write(chunk);
  });
  const exitCode = await (async (): Promise<number> => {
    try {
      return await new Promise<number>((resolvePromise, reject) => {
        child.on("error", reject);
        child.on("close", (code) => {
          resolvePromise(code ?? 1);
        });
      });
    } finally {
      stderrForwarder?.flush();
    }
  })();
  if (exitCode !== 0) {
    throw new Error(
      `muon-builder failed with exit code ${exitCode}.\n${stderr.trim()}`,
    );
  }
  return stdout;
};

/**
 * Invokes the native muon-builder executable and returns the prepared runtime.
 *
 * @param options Native prepare invocation options.
 * @returns Prepared runtime location.
 */
export const runMuonPrepare = async (
  options: MuonPrepareOptions,
): Promise<MuonPrepareResult> => {
  const args = createMuonPrepareArguments(options);
  const stdout = await runMuonPrepareCommand({
    prepareExecutablePath: options.prepareExecutablePath,
    environment: options.environment,
    cwd: options.cwd,
    quiet: options.quiet,
    parseProgressJson: !options.quiet,
    args,
  });
  const result = JSON.parse(stdout) as Partial<MuonPrepareResult>;
  if (
    (result.stagePath !== undefined && typeof result.stagePath !== "string") ||
    typeof result.muonPath !== "string" ||
    typeof result.cefPath !== "string" ||
    typeof result.cacheHit !== "boolean"
  ) {
    throw new Error(`muon-builder returned invalid JSON: ${stdout}`);
  }
  return {
    ...(result.stagePath === undefined ? {} : { stagePath: result.stagePath }),
    muonPath: result.muonPath,
    cefPath: result.cefPath,
    cacheHit: result.cacheHit,
  };
};

/**
 * Invokes the native muon-builder executable to write Windows PE resources.
 *
 * @param options Resource update invocation options.
 */
export const runMuonPrepareResourceUpdate = async (
  options: MuonPrepareResourceUpdateOptions,
): Promise<void> => {
  await runMuonPrepareCommand({
    prepareExecutablePath: options.prepareExecutablePath,
    environment: options.environment,
    cwd: options.cwd,
    quiet: options.quiet,
    parseProgressJson: false,
    args: [
      "resource",
      "--input",
      options.inputPath,
      "--updates-json",
      options.updatesJsonPath,
      "--output",
      options.outputPath,
      ...(options.quiet ? ["--quiet"] : []),
    ],
  });
};
