// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { spawn, type ChildProcess } from "node:child_process";
import { constants, writeFileSync } from "node:fs";
import { access, mkdir, mkdtemp, readFile, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { dirname, isAbsolute, join, resolve, win32 } from "node:path";
import { StringDecoder } from "node:string_decoder";
import { fileURLToPath } from "node:url";

import { parse } from "json5";
import type { ViteDevServer } from "vite";

import { ensureMuonGitignoreEntry } from "../common/gitignore.js";
import { getDefaultMuonPrepareTarget, runMuonPrepare } from "./prepare.js";
import type { MuonRuntimePluginConfig } from "../common/capability.js";
import type { MuonVitePluginOptions } from "../common/vite.js";
import { resolveMuonServices } from "../common/service-config.ts";
import type { MuonServiceInput } from "../common/service-inputs.ts";
import {
  createMuonDevelopmentServiceWatcher,
  type MuonDevelopmentServiceWatcher,
} from "./development-service-watch.ts";
import {
  startMuonDevelopmentServices,
  type MuonDevelopmentServiceConfig,
  type MuonDevelopmentServices,
} from "./development-services.ts";
import {
  assertMuonNodeHostArtifacts,
  assertMuonNodeProjectStagingIsSafe,
  createMuonNodeRuntimeRequirement,
  resolveMuonNodeProject,
  stageMuonNodeProject,
  type ResolvedMuonNodeProject,
} from "./node-project.ts";

export interface MuonLaunchScriptOptions {
  muonExecutablePath: string;
  projectConfigPath: string | undefined;
  overrideConfigPath: string;
  platform: NodeJS.Platform;
}

interface MuonViteSessionOptions {
  server: ViteDevServer;
  pluginOptions: MuonVitePluginOptions;
  refreshRuntimePluginConfig: () => Promise<MuonRuntimePluginConfig>;
  platform: NodeJS.Platform;
  architecture: NodeJS.Architecture;
  environment: NodeJS.ProcessEnv;
}

interface MuonRuntimePaths {
  temporaryDirectory: string;
  overrideConfigPath: string;
}

interface MuonProjectConfigResolution {
  path: string | undefined;
  config: Record<string, unknown> | undefined;
}

interface MuonOverrideConfig {
  serviceHost: MuonDevelopmentServiceConfig["serviceHost"];
  config?: Record<string, string>;
  node?: {
    project: string;
  };
  cdp?: {
    enable: true;
  };
  browser: {
    profilePath: string;
    startPage: string;
    titleBarType?: "native" | "muon";
    initialTitleBarVisibility?: boolean;
    keybind?: {
      devtools: "f12";
      recycle: "ctrl+f12";
    };
  };
  plugin: MuonRuntimePluginConfig & {
    pages: string[];
  };
  network: {
    allow: string[];
    localAccess?: {
      allowInsecureLocalhost: boolean;
    };
  };
}

/**
 * Options for resolving a muon runtime directory used by the Vite plugin.
 */
export interface MuonRuntimePathOptions {
  /**
   * Vite project root used to resolve explicit relative paths.
   */
  root: string;

  /**
   * Public muon runtime target such as linux-amd64 or windows-amd64.
   */
  target: string;

  /**
   * Explicit custom muon-core runtime path.
   */
  muonPath: string | undefined;

  /**
   * Directory containing the packaged muon JavaScript files.
   *
   * @remarks This is injectable for tests. Production code uses the module directory.
   */
  packageDirectory?: string;
}

const resolveFromRoot = (root: string, path: string): string =>
  isAbsolute(path) ? path : resolve(root, path);

const moduleDirectory =
  typeof __dirname === "string"
    ? __dirname
    : dirname(fileURLToPath(import.meta.url));
const defaultProjectConfigFileNames = ["muon.json5", "muon.jsonc", "muon.json"];
const muonRecycleExitCode = 88;
const muonDiagnosticLimit = 64 * 1024;

interface MuonProcessResult {
  code: number | null;
  signal: NodeJS.Signals | null;
  diagnostic: string;
  error: Error | undefined;
}

const appendDiagnostic = (
  diagnostic: Buffer<ArrayBufferLike>,
  chunk: Buffer<ArrayBufferLike>,
): Buffer<ArrayBufferLike> => {
  if (chunk.byteLength >= muonDiagnosticLimit) {
    return Buffer.from(chunk.subarray(chunk.byteLength - muonDiagnosticLimit));
  }
  const retainedLength = Math.max(0, muonDiagnosticLimit - chunk.byteLength);
  return Buffer.concat([
    diagnostic.subarray(Math.max(0, diagnostic.byteLength - retainedLength)),
    chunk,
  ]);
};

const decodeTrailingUtf8 = (value: Buffer<ArrayBufferLike>): string => {
  let start = 0;
  while (start < value.byteLength && (value[start]! & 0xc0) === 0x80) {
    start += 1;
  }
  return value.subarray(start).toString("utf8");
};

const getMuonExitDescription = ({ code, signal }: MuonProcessResult): string =>
  signal === null ? `exit code ${code ?? 1}` : `signal ${signal}`;

const reportMuonFailure = (
  server: ViteDevServer,
  executablePath: string,
  result: MuonProcessResult,
): void => {
  const diagnostic = result.diagnostic.trim();
  const error = result.error === undefined ? "" : `: ${result.error.message}`;
  server.config.logger.warn(
    `muon runtime failed: ${executablePath}: ${getMuonExitDescription(result)}${error}${
      diagnostic === "" ? "" : `\n${diagnostic}`
    }`,
  );
};

/**
 * Resolves the muon-core runtime directory used by the Vite plugin.
 *
 * @param options Runtime path resolution inputs.
 * @returns Absolute or package-relative runtime directory path.
 */
export const resolveMuonRuntimePath = ({
  root,
  target,
  muonPath,
  packageDirectory = moduleDirectory,
}: MuonRuntimePathOptions): string =>
  muonPath === undefined
    ? join(packageDirectory, "runtime", target)
    : resolveFromRoot(root, muonPath);

export const getMuonExecutablePath = (
  runtimePath: string,
  platform: NodeJS.Platform,
): string =>
  join(runtimePath, platform === "win32" ? "muon-core.exe" : "muon-core");

const quotePosix = (value: string): string =>
  `'${value.replaceAll("'", "'\\''")}'`;

const getPlatformDirectoryName = (
  path: string,
  platform: NodeJS.Platform,
): string => (platform === "win32" ? win32.dirname(path) : dirname(path));

const getOptionalPosixValue = (value: string | undefined): string =>
  value === undefined ? "''" : quotePosix(value);

const createPosixMuonLaunchScript = ({
  muonExecutablePath,
  projectConfigPath,
  overrideConfigPath,
  platform,
}: MuonLaunchScriptOptions): string => `#!/usr/bin/env bash
set -euo pipefail
MUON_RECYCLE_EXIT_CODE=${muonRecycleExitCode}
MUON_EXECUTABLE=${quotePosix(muonExecutablePath)}
MUON_EXECUTABLE_DIRECTORY=${quotePosix(getPlatformDirectoryName(muonExecutablePath, "linux"))}
MUON_PROJECT_CONFIG=${getOptionalPosixValue(projectConfigPath)}
MUON_OVERRIDE_CONFIG=${quotePosix(overrideConfigPath)}

MUON_CONFIG_ARGS=()
if [[ -n "$MUON_PROJECT_CONFIG" ]]; then
  if [[ ! -f "$MUON_PROJECT_CONFIG" ]]; then
    echo "muon startup failed: project config does not exist: $MUON_PROJECT_CONFIG" >&2
    exit 1
  fi
  MUON_CONFIG_ARGS+=("-c" "$MUON_PROJECT_CONFIG")
fi

if [[ ! -x "$MUON_EXECUTABLE" ]]; then
  echo "muon startup failed: executable does not exist or is not executable: $MUON_EXECUTABLE" >&2
  exit 1
fi

if [[ ! -f "$MUON_OVERRIDE_CONFIG" ]]; then
  echo "muon startup failed: generated override config does not exist: $MUON_OVERRIDE_CONFIG" >&2
  exit 1
fi
MUON_CONFIG_ARGS+=("-c" "$MUON_OVERRIDE_CONFIG")

cd "$MUON_EXECUTABLE_DIRECTORY"
while true; do
  set +e
  "$MUON_EXECUTABLE" "\${MUON_CONFIG_ARGS[@]}"
  MUON_EXIT_CODE=$?
  set -e
  if [[ "$MUON_EXIT_CODE" -ne "$MUON_RECYCLE_EXIT_CODE" ]]; then
    exit "$MUON_EXIT_CODE"
  fi
done
`;

const escapeWindowsCmdValue = (value: string): string =>
  value.replaceAll("%", "%%").replaceAll("\r", "").replaceAll("\n", "");

const getOptionalWindowsCmdValue = (value: string | undefined): string =>
  value === undefined ? "" : escapeWindowsCmdValue(value);

const createWindowsMuonLaunchScript = ({
  muonExecutablePath,
  projectConfigPath,
  overrideConfigPath,
}: MuonLaunchScriptOptions): string => `@echo off
setlocal
set "MUON_RECYCLE_EXIT_CODE=${muonRecycleExitCode}"
set "MUON_EXECUTABLE=${escapeWindowsCmdValue(muonExecutablePath)}"
set "MUON_EXECUTABLE_DIRECTORY=${escapeWindowsCmdValue(getPlatformDirectoryName(muonExecutablePath, "win32"))}"
set "MUON_PROJECT_CONFIG=${getOptionalWindowsCmdValue(projectConfigPath)}"
set "MUON_OVERRIDE_CONFIG=${escapeWindowsCmdValue(overrideConfigPath)}"

if not exist "%MUON_EXECUTABLE%" (
  echo muon startup failed: executable does not exist: %MUON_EXECUTABLE% 1>&2
  exit /b 1
)

if not exist "%MUON_OVERRIDE_CONFIG%" (
  echo muon startup failed: generated override config does not exist: %MUON_OVERRIDE_CONFIG% 1>&2
  exit /b 1
)

pushd "%MUON_EXECUTABLE_DIRECTORY%"
:muon_recycle_loop
if defined MUON_PROJECT_CONFIG (
  if not exist "%MUON_PROJECT_CONFIG%" (
    echo muon startup failed: project config does not exist: %MUON_PROJECT_CONFIG% 1>&2
    popd
    exit /b 1
  )
  "%MUON_EXECUTABLE%" -c "%MUON_PROJECT_CONFIG%" -c "%MUON_OVERRIDE_CONFIG%"
) else (
  "%MUON_EXECUTABLE%" -c "%MUON_OVERRIDE_CONFIG%"
)
set "MUON_EXIT_CODE=%ERRORLEVEL%"
if "%MUON_EXIT_CODE%"=="%MUON_RECYCLE_EXIT_CODE%" goto muon_recycle_loop
popd
exit /b %MUON_EXIT_CODE%
`;

export const createMuonLaunchScript = (
  options: MuonLaunchScriptOptions,
): string =>
  options.platform === "win32"
    ? createWindowsMuonLaunchScript(options)
    : createPosixMuonLaunchScript(options);

const getBaseUrl = (server: ViteDevServer): string | undefined =>
  server.resolvedUrls?.local[0] ?? server.resolvedUrls?.network[0];

const getWebSocketOrigin = (startUrl: string): string => {
  const url = new URL(startUrl);
  if (url.protocol === "https:") {
    url.protocol = "wss:";
  } else if (url.protocol === "http:") {
    url.protocol = "ws:";
  }
  return url.origin;
};

const createMuonOverrideConfig = (
  startUrl: string,
  enableDebugger: boolean,
  allowInsecureLocalhost: boolean | undefined,
  config: Readonly<Record<string, string>> | undefined,
  runtimePluginConfig: MuonRuntimePluginConfig,
  nodeProjectPath: string | undefined,
  serviceConfig: MuonDevelopmentServiceConfig,
): MuonOverrideConfig => {
  const origin = new URL(startUrl).origin;
  return {
    serviceHost: serviceConfig.serviceHost,
    ...(config === undefined ? {} : { config: { ...config } }),
    ...(nodeProjectPath === undefined
      ? {}
      : {
          node: {
            project: nodeProjectPath,
          },
        }),
    ...(enableDebugger
      ? {
          cdp: {
            enable: true,
          },
        }
      : {}),
    browser: {
      ...serviceConfig.browser,
      startPage: startUrl,
      ...(enableDebugger
        ? {
            keybind: {
              devtools: "f12",
              recycle: "ctrl+f12",
            },
          }
        : {}),
    },
    plugin: {
      ...runtimePluginConfig,
      pages: [`${origin}/**`],
    },
    network: {
      allow: [`${origin}/**`, `${getWebSocketOrigin(startUrl)}/**`],
      ...(allowInsecureLocalhost === undefined
        ? {}
        : {
            localAccess: {
              allowInsecureLocalhost,
            },
          }),
    },
  };
};

const writeMuonOverrideConfig = (
  server: ViteDevServer,
  overrideConfigPath: string,
  pluginOptions: MuonVitePluginOptions,
  runtimePluginConfig: MuonRuntimePluginConfig,
  nodeProjectPath: string | undefined,
  serviceConfig: MuonDevelopmentServiceConfig,
): boolean => {
  const startUrl = getBaseUrl(server);
  if (startUrl === undefined) {
    server.config.logger.warn("muon Vite plugin could not resolve a Vite URL.");
    return false;
  }
  writeFileSync(
    overrideConfigPath,
    `${JSON.stringify(
      createMuonOverrideConfig(
        startUrl,
        pluginOptions.enableDebugger !== false,
        pluginOptions.allowInsecureLocalhost,
        pluginOptions.dev?.config,
        runtimePluginConfig,
        nodeProjectPath,
        serviceConfig,
      ),
      null,
      2,
    )}\n`,
  );
  return true;
};

const createRuntimePaths = async (): Promise<MuonRuntimePaths> => {
  const temporaryDirectory = await mkdtemp(join(tmpdir(), "muon-vite-"));
  return {
    temporaryDirectory,
    overrideConfigPath: join(temporaryDirectory, "muon.vite.json"),
  };
};

const fileExists = async (path: string): Promise<boolean> => {
  try {
    await access(path, constants.F_OK);
    return true;
  } catch {
    return false;
  }
};

const isJsonObject = (value: unknown): value is Record<string, unknown> =>
  typeof value === "object" && value !== null && !Array.isArray(value);

const getErrorMessage = (error: unknown): string =>
  error instanceof Error ? error.message : String(error);

const resolveProjectConfig = async (
  server: ViteDevServer,
): Promise<MuonProjectConfigResolution> => {
  for (const fileName of defaultProjectConfigFileNames) {
    const candidatePath = join(server.config.root, fileName);
    if (!(await fileExists(candidatePath))) {
      continue;
    }

    try {
      const parsed = parse(await readFile(candidatePath, "utf8"));
      if (!isJsonObject(parsed)) {
        throw new Error("muon config root must be an object");
      }
      return {
        path: candidatePath,
        config: parsed,
      };
    } catch (error) {
      throw new Error(
        `muon project config could not be read or parsed: ${candidatePath}: ${getErrorMessage(error)}`,
      );
    }
  }

  server.config.logger.warn(
    `muon project config was not found in ${server.config.root}; launching with generated Vite config only.`,
  );
  return {
    path: undefined,
    config: undefined,
  };
};

const createMuonConfigArguments = (
  projectConfigPath: string | undefined,
  overrideConfigPath: string,
): string[] => [
  ...(projectConfigPath === undefined ? [] : ["-c", projectConfigPath]),
  "-c",
  overrideConfigPath,
];

const waitForMuonProcess = async (
  muonExecutablePath: string,
  configArguments: readonly string[],
  environment: NodeJS.ProcessEnv,
  setCurrentChild: (child: ChildProcess | undefined) => void,
): Promise<MuonProcessResult> => {
  const child = spawn(muonExecutablePath, [...configArguments], {
    cwd: dirname(muonExecutablePath),
    env: environment,
    stdio: ["ignore", "pipe", "pipe"],
    windowsHide: false,
  });
  setCurrentChild(child);
  try {
    return await new Promise<MuonProcessResult>((resolvePromise) => {
      let diagnostic: Buffer<ArrayBufferLike> = Buffer.alloc(0);
      let error: Error | undefined = undefined;
      const stdoutDecoder = new StringDecoder("utf8");
      const stderrDecoder = new StringDecoder("utf8");
      const append = (source: "stdout" | "stderr", text: string): void => {
        if (text === "") {
          return;
        }
        diagnostic = appendDiagnostic(
          diagnostic,
          Buffer.from(`[${source}] ${text}`),
        );
      };
      child.stdout?.on("data", (chunk: Buffer) => {
        append("stdout", stdoutDecoder.write(chunk));
      });
      child.stderr?.on("data", (chunk: Buffer) => {
        append("stderr", stderrDecoder.write(chunk));
      });
      child.once("error", (reason: Error) => {
        error = reason;
      });
      child.once("close", (code, signal) => {
        append("stdout", stdoutDecoder.end());
        append("stderr", stderrDecoder.end());
        resolvePromise({
          code,
          signal,
          diagnostic: decodeTrailingUtf8(diagnostic),
          error,
        });
      });
    });
  } finally {
    setCurrentChild(undefined);
  }
};

export const startMuonViteBrowserBridge = async ({
  server,
  pluginOptions,
  refreshRuntimePluginConfig,
  platform,
  architecture,
  environment,
}: MuonViteSessionOptions): Promise<void> => {
  await ensureMuonGitignoreEntry(server.config.root);

  if (pluginOptions.open === false || server.httpServer === null) {
    return;
  }

  const target = getDefaultMuonPrepareTarget(platform, architecture);
  const muonPath = resolveMuonRuntimePath({
    root: server.config.root,
    target,
    muonPath: pluginOptions.muonPath,
  });
  const cefPath =
    pluginOptions.cefPath === undefined
      ? undefined
      : resolveFromRoot(server.config.root, pluginOptions.cefPath);
  const stagePath =
    pluginOptions.stagePath === undefined
      ? resolve(server.config.root, ".muon", target)
      : resolveFromRoot(server.config.root, pluginOptions.stagePath);
  const paths = await createRuntimePaths();

  let currentChild: ChildProcess | undefined = undefined;
  let closing = false;
  let serverCloseRequested = false;
  let supervisorPromise: Promise<void> | undefined = undefined;
  let serviceSession: MuonDevelopmentServices | undefined;
  let serviceWatcher: MuonDevelopmentServiceWatcher | undefined;
  let updatePromise: Promise<void> | undefined;
  let updateTimer: ReturnType<typeof setTimeout> | undefined;
  let restartRequested = false;
  let activeRuntime: string | undefined;
  let uiRuntime: string | undefined;
  let activeRequirement: string | undefined;
  let activeSettings: string | undefined;
  let activeNodeProject: ResolvedMuonNodeProject | undefined;
  let activeNodeProjectPath: string | undefined;
  let activeInputs = new Map<string, MuonServiceInput>();
  const changedPaths = new Set<string>();
  const runtimeDirectories = new Set<string>();
  const configPaths = defaultProjectConfigFileNames.map((name) =>
    resolve(server.config.root, name),
  );
  const cancellation = new AbortController();

  const serviceSettings = (project: MuonProjectConfigResolution): string =>
    JSON.stringify({
      services: project.config?.services,
      node: project.config?.node,
    });
  const collectInputs = (): Map<string, MuonServiceInput> =>
    new Map(
      configPaths.map((path) => ["file:" + path, { path, kind: "file" }]),
    );
  const prepareServices = async (
    project: MuonProjectConfigResolution,
    inputs: Map<string, MuonServiceInput>,
  ) => {
    const onInput = (input: MuonServiceInput): void => {
      inputs.set(input.kind + ":" + input.path, input);
    };
    const directory =
      project.path === undefined ? server.config.root : dirname(project.path);
    const services = await resolveMuonServices(
      project.config?.services,
      directory,
      [target],
      { root: server.config.root, environment, onInput },
    );
    const nodeProject = await resolveMuonNodeProject(project.config, directory);
    if (nodeProject)
      onInput({
        path: join(nodeProject.sourcePath, "package.json"),
        kind: "file",
      });
    const nodeProjects = [
      ...(nodeProject === undefined ? [] : [nodeProject]),
      ...services.flatMap((service) =>
        service.runtime === "node" ? [service.project] : [],
      ),
    ];
    await assertMuonNodeProjectStagingIsSafe(nodeProjects, stagePath);
    const requirement = createMuonNodeRuntimeRequirement(nodeProjects, true);
    const key = JSON.stringify([requirement, nodeProject?.sourcePath]);
    let runtime = activeRuntime;
    let allocated: string | undefined;
    let nodeProjectPath = activeNodeProjectPath;
    try {
      if (runtime === undefined || activeRequirement !== key) {
        let destination = stagePath;
        if (runtime !== undefined) {
          const base = join(
            server.config.root,
            ".muon/development-runtimes",
            target,
          );
          await mkdir(base, { recursive: true });
          allocated = await mkdtemp(join(base, "runtime-"));
          destination = allocated;
          runtimeDirectories.add(allocated);
        }
        await assertMuonNodeProjectStagingIsSafe(nodeProjects, destination);
        const prepared = await runMuonPrepare({
          muonPath,
          cefPath,
          stageDir: destination,
          target,
          cacheDir: environment.MUON_CACHE_DIR,
          nodeRuntimeRequirement: requirement,
          force: false,
          quiet: false,
          prepareExecutablePath: undefined,
          environment,
          cwd: server.config.root,
        });
        if (prepared.stagePath === undefined)
          throw new Error("muon-builder did not return a staged runtime path.");
        runtime = prepared.stagePath;
        nodeProjectPath = await stageMuonNodeProject(nodeProject, runtime);
      }
      if (nodeProjects.length)
        await assertMuonNodeHostArtifacts(runtime, platform);
      return {
        runtime,
        services,
        nodeProject,
        nodeProjectPath,
        key,
        allocated,
        onInput,
      };
    } catch (error) {
      if (allocated) {
        await rm(allocated, { recursive: true, force: true });
        runtimeDirectories.delete(allocated);
      }
      throw error;
    }
  };

  const retireRuntimes = async (): Promise<void> => {
    for (const directory of runtimeDirectories)
      if (directory !== activeRuntime && directory !== uiRuntime) {
        await rm(directory, { recursive: true, force: true });
        runtimeDirectories.delete(directory);
      }
  };

  const processUpdates = async (): Promise<void> => {
    while (!closing && changedPaths.size) {
      const changed = [...changedPaths];
      changedPaths.clear();
      const inputs = collectInputs();
      let candidate: Awaited<ReturnType<typeof prepareServices>> | undefined;
      try {
        const project = await resolveProjectConfig(server);
        const settings = serviceSettings(project);
        if (
          changed.every((path) => configPaths.includes(path)) &&
          settings === activeSettings
        )
          continue;
        candidate = await prepareServices(project, inputs);
        if (closing) return;
        const previousEndpoint = serviceSession!.config.serviceHost.endpoint;
        const previousRuntime = activeRuntime;
        await serviceSession!.update(
          candidate.runtime,
          candidate.services,
          candidate.onInput,
        );
        if (closing) return;
        activeRuntime = candidate.runtime;
        activeRequirement = candidate.key;
        activeSettings = settings;
        activeNodeProject = candidate.nodeProject;
        activeNodeProjectPath = candidate.nodeProjectPath;
        activeInputs = inputs;
        await serviceWatcher?.replace([...inputs.values()]);
        if (closing) return;
        // Adding the first host or changing Node runtimes also changes the CEF
        // process configuration. Recycle that process after services are ready.
        if (
          currentChild &&
          (previousEndpoint !== serviceSession!.config.serviceHost.endpoint ||
            previousRuntime !== activeRuntime)
        ) {
          restartRequested = true;
          currentChild.kill();
        } else server.ws.send({ type: "full-reload", path: "*" });
        server.config.logger.info("muon services updated");
        await retireRuntimes();
      } catch (error) {
        if (candidate?.allocated && candidate.runtime !== activeRuntime) {
          await rm(candidate.allocated, { recursive: true, force: true });
          runtimeDirectories.delete(candidate.allocated);
        }
        if (!closing) {
          // Keep both the current inputs and newly discovered failing inputs.
          // Creating or repairing a dependency must be enough to retry.
          activeInputs = new Map([...activeInputs, ...inputs]);
          await serviceWatcher?.replace([...activeInputs.values()]);
          const message = `muon service update failed: ${getErrorMessage(error)}`;
          server.config.logger.warn(message);
          server.ws.send({ type: "error", err: { message, stack: "" } });
        }
      }
    }
  };

  const scheduleUpdate = (path: string): void => {
    if (closing) return;
    changedPaths.add(path);
    if (updatePromise) return;
    clearTimeout(updateTimer);
    updateTimer = setTimeout(async () => {
      updateTimer = undefined;
      updatePromise = processUpdates();
      try {
        await updatePromise;
      } catch (error) {
        if (!closing)
          server.config.logger.warn(
            `muon service update failed: ${getErrorMessage(error)}`,
          );
      } finally {
        updatePromise = undefined;
      }
    }, 75);
  };

  const setCurrentChild = (child: ChildProcess | undefined): void => {
    currentChild = child;
  };

  const runSupervisor = async (): Promise<void> => {
    let result: MuonProcessResult | undefined = undefined;
    while (
      !closing &&
      (result === undefined || result.code === muonRecycleExitCode)
    ) {
      try {
        const projectConfig = await resolveProjectConfig(server);
        const runtimePluginConfig = await refreshRuntimePluginConfig();
        if (serviceSession === undefined) {
          const inputs = collectInputs();
          const candidate = await prepareServices(projectConfig, inputs);
          serviceSession = await startMuonDevelopmentServices(
            server.config.root,
            target,
            candidate.runtime,
            candidate.services,
            environment,
            cancellation.signal,
            candidate.onInput,
          );
          activeRuntime = candidate.runtime;
          activeRequirement = candidate.key;
          activeSettings = serviceSettings(projectConfig);
          activeNodeProject = candidate.nodeProject;
          activeNodeProjectPath = candidate.nodeProjectPath;
          activeInputs = inputs;
          if (!closing && server.config.server.watch !== null)
            serviceWatcher = await createMuonDevelopmentServiceWatcher(
              [...inputs.values()],
              scheduleUpdate,
              (error) =>
                server.config.logger.warn(
                  `muon service watch failed: ${getErrorMessage(error)}`,
                ),
            );
        } else {
          // A UI recycle can copy its page-owned Node project after that UI has
          // exited. The service runtime and its snapshots remain immutable.
          activeNodeProjectPath = await stageMuonNodeProject(
            activeNodeProject,
            activeRuntime!,
          );
        }
        const configWritten = writeMuonOverrideConfig(
          server,
          paths.overrideConfigPath,
          pluginOptions,
          runtimePluginConfig,
          activeNodeProjectPath,
          serviceSession.config,
        );
        if (!configWritten || closing) {
          return;
        }
        const executablePath = getMuonExecutablePath(activeRuntime!, platform);
        uiRuntime = activeRuntime;
        result = await waitForMuonProcess(
          executablePath,
          createMuonConfigArguments(
            projectConfig.path,
            paths.overrideConfigPath,
          ),
          environment,
          setCurrentChild,
        );
        uiRuntime = undefined;
        await retireRuntimes();
        if (restartRequested) {
          restartRequested = false;
          result = {
            ...result,
            code: muonRecycleExitCode,
            signal: null,
            error: undefined,
          };
        }
        if (
          !closing &&
          (result.error !== undefined ||
            result.signal !== null ||
            result.code === null ||
            (result.code !== 0 && result.code !== muonRecycleExitCode))
        ) {
          reportMuonFailure(server, executablePath, result);
          if (pluginOptions.exitWithServer !== false) {
            if (process.exitCode === undefined || process.exitCode === 0) {
              process.exitCode = 1;
            }
          }
          await serviceSession.stop();
        } else if (!closing && result.code !== muonRecycleExitCode) {
          await serviceSession.waitForExit();
        }
      } catch (error) {
        if (!closing) {
          server.config.logger.warn(
            `muon startup failed: ${getErrorMessage(error)}`,
          );
          if (pluginOptions.exitWithServer !== false) {
            if (process.exitCode === undefined || process.exitCode === 0) {
              process.exitCode = 1;
            }
          }
        }
        try {
          await serviceSession?.stop();
        } catch (cleanupError) {
          server.config.logger.warn(
            `muon service cleanup failed: ${getErrorMessage(cleanupError)}`,
          );
        }
        return;
      }
    }
  };

  let cleanupPromise: Promise<void> | undefined = undefined;
  const cleanup = async (): Promise<void> => {
    if (cleanupPromise !== undefined) {
      return cleanupPromise;
    }
    cleanupPromise = (async () => {
      closing = true;
      clearTimeout(updateTimer);
      cancellation.abort();
      currentChild?.kill();
      await serviceWatcher?.close();
      await updatePromise;
      await supervisorPromise;
      // The initial scan can finish after the first close above observed no
      // watcher. Await supervisor startup before reclaiming that late monitor.
      await serviceWatcher?.close();
      try {
        await serviceSession?.stop();
      } finally {
        for (const directory of runtimeDirectories)
          await rm(directory, { recursive: true, force: true });
        runtimeDirectories.clear();
        await rm(paths.temporaryDirectory, { recursive: true, force: true });
      }
    })();
    return cleanupPromise;
  };

  const originalClose = server.close.bind(server);
  server.close = async (): Promise<void> => {
    try {
      await originalClose();
    } finally {
      await cleanup();
    }
  };
  server.httpServer.once("close", async () => {
    try {
      await cleanup();
    } catch (error) {
      server.config.logger.warn(
        `muon could not clean up after the Vite server closed: ${getErrorMessage(error)}`,
      );
    }
  });
  server.httpServer.once("listening", async () => {
    supervisorPromise = runSupervisor();
    await supervisorPromise;
    if (
      closing ||
      serverCloseRequested ||
      pluginOptions.exitWithServer === false
    ) {
      return;
    }
    serverCloseRequested = true;
    try {
      await server.close();
    } catch (error) {
      server.config.logger.warn(
        `muon could not close the Vite server after muon exited: ${getErrorMessage(error)}`,
      );
    }
  });
};
