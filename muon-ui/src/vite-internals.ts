// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import { spawn } from "node:child_process";
import { constants, writeFileSync } from "node:fs";
import {
  access,
  chmod,
  mkdtemp,
  readFile,
  rm,
  writeFile,
} from "node:fs/promises";
import { tmpdir } from "node:os";
import { dirname, isAbsolute, join, resolve, win32 } from "node:path";
import { fileURLToPath } from "node:url";

import { parse } from "json5";
import type { ViteDevServer } from "vite";

import { ensureMuonGitignoreEntry } from "./gitignore.js";
import { getDefaultMuonPrepareTarget, runMuonPrepare } from "./prepare.js";
import type { MuonVitePluginOptions } from "./vite.js";

export interface MuonLaunchScriptOptions {
  muonExecutablePath: string;
  projectConfigPath: string | undefined;
  overrideConfigPath: string;
  platform: NodeJS.Platform;
}

interface MuonViteSessionOptions {
  server: ViteDevServer;
  pluginOptions: MuonVitePluginOptions;
  platform: NodeJS.Platform;
  architecture: NodeJS.Architecture;
  environment: NodeJS.ProcessEnv;
}

interface MuonRuntimePaths {
  temporaryDirectory: string;
  launchScriptPath: string;
  overrideConfigPath: string;
  projectConfigPath: string | undefined;
  muonExecutablePath: string;
}

interface MuonOverrideConfig {
  cdp?: {
    enable: true;
  };
  browser: {
    startPage: string;
    titleBarType?: "native" | "muon";
    initialTitleBarVisibility?: boolean;
    keybind?: {
      devtools: "f12";
    };
    plugin: {
      allow: string[];
    };
  };
  network: {
    allow: string[];
  };
}

/**
 * Options for resolving a Muon runtime directory used by the Vite plugin.
 */
export interface MuonRuntimePathOptions {
  /**
   * Vite project root used to resolve explicit relative paths.
   */
  root: string;

  /**
   * Muon runtime target such as linux64, linuxarm, linuxarm64, windows32, or windows64.
   */
  target: string;

  /**
   * Explicit custom muon-core runtime path.
   */
  muonPath: string | undefined;

  /**
   * Directory containing the packaged Muon JavaScript files.
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

const getLaunchScriptFileName = (platform: NodeJS.Platform): string =>
  platform === "win32" ? "open-muon.cmd" : "open-muon.sh";

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
    echo "Muon startup failed: project config does not exist: $MUON_PROJECT_CONFIG" >&2
    exit 1
  fi
  MUON_CONFIG_ARGS+=("-c" "$MUON_PROJECT_CONFIG")
fi

if [[ ! -x "$MUON_EXECUTABLE" ]]; then
  echo "Muon startup failed: executable does not exist or is not executable: $MUON_EXECUTABLE" >&2
  exit 1
fi

if [[ ! -f "$MUON_OVERRIDE_CONFIG" ]]; then
  echo "Muon startup failed: generated override config does not exist: $MUON_OVERRIDE_CONFIG" >&2
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
  echo Muon startup failed: executable does not exist: %MUON_EXECUTABLE% 1>&2
  exit /b 1
)

if not exist "%MUON_OVERRIDE_CONFIG%" (
  echo Muon startup failed: generated override config does not exist: %MUON_OVERRIDE_CONFIG% 1>&2
  exit /b 1
)

pushd "%MUON_EXECUTABLE_DIRECTORY%"
:muon_recycle_loop
if defined MUON_PROJECT_CONFIG (
  if not exist "%MUON_PROJECT_CONFIG%" (
    echo Muon startup failed: project config does not exist: %MUON_PROJECT_CONFIG% 1>&2
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
): MuonOverrideConfig => {
  const origin = new URL(startUrl).origin;
  return {
    ...(enableDebugger
      ? {
          cdp: {
            enable: true,
          },
        }
      : {}),
    browser: {
      startPage: startUrl,
      ...(enableDebugger
        ? {
            keybind: {
              devtools: "f12",
            },
          }
        : {}),
      plugin: {
        allow: [`${origin}/**`],
      },
    },
    network: {
      allow: [`${origin}/**`, `${getWebSocketOrigin(startUrl)}/**`],
    },
  };
};

const writeMuonOverrideConfig = (
  server: ViteDevServer,
  overrideConfigPath: string,
  enableDebugger: boolean,
): boolean => {
  const startUrl = getBaseUrl(server);
  if (startUrl === undefined) {
    server.config.logger.warn("Muon Vite plugin could not resolve a Vite URL.");
    return false;
  }
  writeFileSync(
    overrideConfigPath,
    `${JSON.stringify(createMuonOverrideConfig(startUrl, enableDebugger), null, 2)}\n`,
  );
  return true;
};

const createRuntimePaths = async (
  server: ViteDevServer,
  stagePath: string,
  platform: NodeJS.Platform,
  projectConfigPath: string | undefined,
): Promise<MuonRuntimePaths> => {
  const temporaryDirectory = await mkdtemp(join(tmpdir(), "muon-vite-"));
  return {
    temporaryDirectory,
    launchScriptPath: join(
      temporaryDirectory,
      getLaunchScriptFileName(platform),
    ),
    overrideConfigPath: join(temporaryDirectory, "muon.vite.json"),
    projectConfigPath,
    muonExecutablePath: getMuonExecutablePath(stagePath, platform),
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

const resolveProjectConfigPath = async (
  server: ViteDevServer,
): Promise<string | undefined> => {
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
      return candidatePath;
    } catch (error) {
      server.config.logger.warn(
        `Muon project config will be ignored because it could not be read or parsed: ${candidatePath}: ${getErrorMessage(error)}`,
      );
      return undefined;
    }
  }

  server.config.logger.warn(
    `Muon project config was not found in ${server.config.root}; launching with generated Vite config only.`,
  );
  return undefined;
};

const writeLaunchScript = async (
  paths: MuonRuntimePaths,
  platform: NodeJS.Platform,
): Promise<void> => {
  await writeFile(
    paths.launchScriptPath,
    createMuonLaunchScript({
      muonExecutablePath: paths.muonExecutablePath,
      projectConfigPath: paths.projectConfigPath,
      overrideConfigPath: paths.overrideConfigPath,
      platform,
    }),
  );
  if (platform !== "win32") {
    await chmod(paths.launchScriptPath, 0o700);
  }
};

const quoteWindowsCommandArgument = (value: string): string =>
  `"${value.replaceAll('"', '\\"')}"`;

const launchMuon = (
  paths: MuonRuntimePaths,
  platform: NodeJS.Platform,
  server: ViteDevServer,
): void => {
  const command = platform === "win32" ? "cmd.exe" : paths.launchScriptPath;
  const args =
    platform === "win32"
      ? ["/d", "/s", "/c", quoteWindowsCommandArgument(paths.launchScriptPath)]
      : [];
  const child = spawn(command, args, {
    detached: true,
    stdio: "ignore",
    windowsHide: false,
  });
  child.once("error", (error) => {
    server.config.logger.warn(`Muon startup failed: ${getErrorMessage(error)}`);
  });
  child.unref();
};

export const startMuonViteBrowserBridge = async ({
  server,
  pluginOptions,
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
  const preparedRuntime = await runMuonPrepare({
    muonPath,
    cefPath,
    stageDir: stagePath,
    target,
    cacheDir: environment.MUON_CACHE_DIR,
    force: false,
    quiet: false,
    prepareExecutablePath: undefined,
    environment,
    cwd: server.config.root,
  });
  if (preparedRuntime.stagePath === undefined) {
    throw new Error("muon-prepare did not return a staged runtime path.");
  }
  const paths = await createRuntimePaths(
    server,
    preparedRuntime.stagePath,
    platform,
    await resolveProjectConfigPath(server),
  );
  await writeLaunchScript(paths, platform);

  let cleanupPromise: Promise<void> | undefined = undefined;
  const cleanup = async (): Promise<void> => {
    if (cleanupPromise !== undefined) {
      return cleanupPromise;
    }
    cleanupPromise = (async () => {
      await rm(paths.temporaryDirectory, { recursive: true, force: true });
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
  server.httpServer.once("close", () => {
    void cleanup();
  });
  server.httpServer.once("listening", () => {
    const configWritten = writeMuonOverrideConfig(
      server,
      paths.overrideConfigPath,
      pluginOptions.enableDebugger !== false,
    );
    if (configWritten) {
      launchMuon(paths, platform, server);
    }
  });
};
