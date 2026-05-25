// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import { chmod, mkdtemp, rm, writeFile } from "node:fs/promises";
import { writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, isAbsolute, join, resolve, win32 } from "node:path";
import { fileURLToPath } from "node:url";

import type { ViteDevServer } from "vite";

import { getDefaultMuonPrepareTarget, runMuonPrepare } from "./prepare.js";
import type { MuonVitePluginOptions } from "./vite.js";

export interface MuonLaunchScriptOptions {
  muonExecutablePath: string;
  projectConfigPath: string;
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
  projectConfigPath: string;
  muonExecutablePath: string;
}

interface MuonOverrideConfig {
  browser: {
    startPage: string;
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

const getServerOpenValue = (
  server: ViteDevServer,
): boolean | string | false => {
  const open = server.config.server.open;
  return open === true || typeof open === "string" ? open : false;
};

const resolveFromRoot = (root: string, path: string): string =>
  isAbsolute(path) ? path : resolve(root, path);

const moduleDirectory =
  typeof __dirname === "string"
    ? __dirname
    : dirname(fileURLToPath(import.meta.url));

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

const createPosixMuonLaunchScript = ({
  muonExecutablePath,
  projectConfigPath,
  overrideConfigPath,
}: MuonLaunchScriptOptions): string => `#!/usr/bin/env bash
set -euo pipefail
MUON_EXECUTABLE=${quotePosix(muonExecutablePath)}
MUON_EXECUTABLE_DIRECTORY=${quotePosix(getPlatformDirectoryName(muonExecutablePath, "linux"))}
MUON_PROJECT_CONFIG=${quotePosix(projectConfigPath)}
MUON_OVERRIDE_CONFIG=${quotePosix(overrideConfigPath)}

if [[ ! -f "$MUON_PROJECT_CONFIG" ]]; then
  echo "Muon startup failed: project config does not exist: $MUON_PROJECT_CONFIG" >&2
  exit 1
fi

if [[ ! -x "$MUON_EXECUTABLE" ]]; then
  echo "Muon startup failed: executable does not exist or is not executable: $MUON_EXECUTABLE" >&2
  exit 1
fi

if [[ ! -f "$MUON_OVERRIDE_CONFIG" ]]; then
  echo "Muon startup failed: generated override config does not exist: $MUON_OVERRIDE_CONFIG" >&2
  exit 1
fi

cd "$MUON_EXECUTABLE_DIRECTORY"
exec "$MUON_EXECUTABLE" -c "$MUON_PROJECT_CONFIG" -c "$MUON_OVERRIDE_CONFIG"
`;

const escapeWindowsCmdValue = (value: string): string =>
  value.replaceAll("%", "%%").replaceAll("\r", "").replaceAll("\n", "");

const createWindowsMuonLaunchScript = ({
  muonExecutablePath,
  projectConfigPath,
  overrideConfigPath,
}: MuonLaunchScriptOptions): string => `@echo off
setlocal
set "MUON_EXECUTABLE=${escapeWindowsCmdValue(muonExecutablePath)}"
set "MUON_EXECUTABLE_DIRECTORY=${escapeWindowsCmdValue(getPlatformDirectoryName(muonExecutablePath, "win32"))}"
set "MUON_PROJECT_CONFIG=${escapeWindowsCmdValue(projectConfigPath)}"
set "MUON_OVERRIDE_CONFIG=${escapeWindowsCmdValue(overrideConfigPath)}"

if not exist "%MUON_PROJECT_CONFIG%" (
  echo Muon startup failed: project config does not exist: %MUON_PROJECT_CONFIG% 1>&2
  exit /b 1
)

if not exist "%MUON_EXECUTABLE%" (
  echo Muon startup failed: executable does not exist: %MUON_EXECUTABLE% 1>&2
  exit /b 1
)

if not exist "%MUON_OVERRIDE_CONFIG%" (
  echo Muon startup failed: generated override config does not exist: %MUON_OVERRIDE_CONFIG% 1>&2
  exit /b 1
)

pushd "%MUON_EXECUTABLE_DIRECTORY%"
"%MUON_EXECUTABLE%" -c "%MUON_PROJECT_CONFIG%" -c "%MUON_OVERRIDE_CONFIG%"
set "MUON_EXIT_CODE=%ERRORLEVEL%"
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

const getStartUrl = (
  server: ViteDevServer,
  openValue: boolean | string,
): string | undefined => {
  const baseUrl = getBaseUrl(server);
  if (baseUrl === undefined) {
    return undefined;
  }
  return typeof openValue === "string"
    ? new URL(openValue, baseUrl).href
    : baseUrl;
};

const getWebSocketOrigin = (startUrl: string): string => {
  const url = new URL(startUrl);
  if (url.protocol === "https:") {
    url.protocol = "wss:";
  } else if (url.protocol === "http:") {
    url.protocol = "ws:";
  }
  return url.origin;
};

const createMuonOverrideConfig = (startUrl: string): MuonOverrideConfig => {
  const origin = new URL(startUrl).origin;
  return {
    browser: {
      startPage: startUrl,
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
  openValue: boolean | string,
  overrideConfigPath: string,
): void => {
  const startUrl = getStartUrl(server, openValue);
  if (startUrl === undefined) {
    server.config.logger.warn("Muon Vite plugin could not resolve a Vite URL.");
    return;
  }
  writeFileSync(
    overrideConfigPath,
    `${JSON.stringify(createMuonOverrideConfig(startUrl), null, 2)}\n`,
  );
};

const createRuntimePaths = async (
  server: ViteDevServer,
  stagePath: string,
  platform: NodeJS.Platform,
): Promise<MuonRuntimePaths> => {
  const temporaryDirectory = await mkdtemp(join(tmpdir(), "muon-vite-"));
  return {
    temporaryDirectory,
    launchScriptPath: join(
      temporaryDirectory,
      getLaunchScriptFileName(platform),
    ),
    overrideConfigPath: join(temporaryDirectory, "muon.vite.json"),
    projectConfigPath: join(server.config.root, "muon.json"),
    muonExecutablePath: getMuonExecutablePath(stagePath, platform),
  };
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

export const startMuonViteBrowserBridge = async ({
  server,
  pluginOptions,
  platform,
  architecture,
  environment,
}: MuonViteSessionOptions): Promise<void> => {
  const openValue = getServerOpenValue(server);
  if (openValue === false || server.httpServer === null) {
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
  );
  await writeLaunchScript(paths, platform);
  const previousBrowser = environment.BROWSER;
  environment.BROWSER = paths.launchScriptPath;

  let cleanupPromise: Promise<void> | undefined = undefined;
  const cleanup = async (): Promise<void> => {
    if (cleanupPromise !== undefined) {
      return cleanupPromise;
    }
    cleanupPromise = (async () => {
      if (previousBrowser === undefined) {
        delete environment.BROWSER;
      } else {
        environment.BROWSER = previousBrowser;
      }
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
    writeMuonOverrideConfig(server, openValue, paths.overrideConfigPath);
  });
};
