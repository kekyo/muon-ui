// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import { execFile } from "node:child_process";
import { existsSync } from "node:fs";
import {
  access,
  chmod,
  mkdir,
  mkdtemp,
  readFile,
  rm,
  stat,
  writeFile,
} from "node:fs/promises";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { pathToFileURL } from "node:url";
import { promisify } from "node:util";

import { delay } from "async-primitives";
import {
  afterAll,
  afterEach,
  beforeAll,
  describe,
  expect,
  it,
  vi,
} from "vitest";
import {
  build as viteBuild,
  createLogger as createViteLogger,
  createServer,
  type Logger,
  type ViteDevServer,
} from "vite";

import {
  createMuonLaunchScript,
  getMuonExecutablePath,
  resolveMuonRuntimePath,
} from "../src/vite-internals.js";
import muon from "../src/vite.js";
import {
  buildTestMuonBuilder,
  createRuntimeInfoHeader,
} from "./test-muon-builder.js";

const execFileAsync = promisify(execFile);

const originalBrowser = process.env.BROWSER;
const originalCacheDir = process.env.MUON_CACHE_DIR;
const originalPreparePath = process.env.MUON_BUILDER_PATH;
const cleanupDirectories: string[] = [];
const suiteCleanupDirectories: string[] = [];
const servers: ViteDevServer[] = [];
const fakeBrowserExecutablePaths = new Set<string>();

const wait = async (predicate: () => boolean): Promise<void> => {
  for (let index = 0; index < 100; index += 1) {
    if (predicate()) {
      return;
    }
    await delay(50);
  }
  throw new Error("Timed out waiting for condition");
};

const createCapturingLogger = (): { logger: Logger; warnings: string[] } => {
  const warnings: string[] = [];
  const logger = createViteLogger("silent");
  const originalWarn = logger.warn.bind(logger);
  logger.warn = (message, options): void => {
    warnings.push(message);
    originalWarn(message, options);
  };
  return { logger, warnings };
};

const createTemporaryDirectory = async (prefix: string): Promise<string> => {
  const directory = await mkdtemp(join(tmpdir(), prefix));
  cleanupDirectories.push(directory);
  return directory;
};

const createSuiteTemporaryDirectory = async (
  prefix: string,
): Promise<string> => {
  const directory = await mkdtemp(join(tmpdir(), prefix));
  suiteCleanupDirectories.push(directory);
  return directory;
};

beforeAll(async () => {
  const buildRoot = await createSuiteTemporaryDirectory("muon-vite-native-");
  const executableName =
    process.platform === "win32" ? "muon-core.exe" : "muon-core";
  const binaries = await buildTestMuonBuilder(
    buildRoot,
    createRuntimeInfoHeader({
      archiveFileName: "cef.tar.bz2",
      archiveUrl: join(buildRoot, "cef.tar.bz2"),
      archiveSha1: "0000000000000000000000000000000000000000",
      archiveSize: 1,
      executableName,
      corePayload: [executableName, "plugins"],
    }),
  );
  process.env.MUON_BUILDER_PATH = binaries.prepareExecutablePath;
});

afterAll(async () => {
  if (originalPreparePath === undefined) {
    delete process.env.MUON_BUILDER_PATH;
  } else {
    process.env.MUON_BUILDER_PATH = originalPreparePath;
  }
  for (const directory of suiteCleanupDirectories.splice(0)) {
    await rm(directory, { recursive: true, force: true });
  }
});

const writeBasicViteProject = async (root: string): Promise<void> => {
  await writeFile(
    join(root, "index.html"),
    "<!doctype html><title>muon vite test</title>",
  );
};

const writeProjectMuonConfig = async (root: string): Promise<void> => {
  await writeFile(
    join(root, "muon.json"),
    `${JSON.stringify({ network: { allow: ["asset://main/**"] } }, null, 2)}\n`,
  );
};

const writeFakeMuonExecutable = async (
  runtimeDirectory: string,
  outputDirectory: string,
): Promise<void> => {
  await mkdir(runtimeDirectory, { recursive: true });
  const executable = getMuonExecutablePath(runtimeDirectory, "linux");
  await writeFile(
    executable,
    `#!/usr/bin/env bash
set -euo pipefail
printf '%s\\n' "$@" > '${outputDirectory.replaceAll("'", "'\\''")}/args.txt'
pwd > '${outputDirectory.replaceAll("'", "'\\''")}/cwd.txt'
override_config=''
previous=''
for argument in "$@"; do
  if [[ "$previous" == '-c' ]]; then
    override_config="$argument"
  fi
  previous="$argument"
done
if [[ -z "$override_config" ]]; then
  echo 'missing override config' >&2
  exit 1
fi
cp "$override_config" '${outputDirectory.replaceAll("'", "'\\''")}/override.json'
`,
  );
  await chmod(executable, 0o755);
};

const writeFakeRecyclingMuonExecutable = async (
  runtimeDirectory: string,
  outputDirectory: string,
): Promise<void> => {
  await mkdir(runtimeDirectory, { recursive: true });
  const escapedOutputDirectory = outputDirectory.replaceAll("'", "'\\''");
  const executable = getMuonExecutablePath(runtimeDirectory, "linux");
  await writeFile(
    executable,
    `#!/usr/bin/env bash
set -euo pipefail
counter_file='${escapedOutputDirectory}/recycle-count.txt'
count=0
if [[ -f "$counter_file" ]]; then
  count="$(cat "$counter_file")"
fi
count=$((count + 1))
printf '%s\\n' "$count" > "$counter_file"
printf '%s\\n' "$@" > "${escapedOutputDirectory}/args-$count.txt"
printf '%s\\n' "$@" > "${escapedOutputDirectory}/args.txt"
pwd > "${escapedOutputDirectory}/cwd-$count.txt"
override_config=''
previous=''
for argument in "$@"; do
  if [[ "$previous" == '-c' ]]; then
    override_config="$argument"
  fi
  previous="$argument"
done
if [[ -z "$override_config" ]]; then
  echo 'missing override config' >&2
  exit 1
fi
cp "$override_config" "${escapedOutputDirectory}/override-$count.json"
if [[ "$count" -eq 1 ]]; then
  exit 88
fi
`,
  );
  await chmod(executable, 0o755);
};

const writeFakeRecyclingMuonSource = async (
  muonDirectory: string,
  outputDirectory: string,
): Promise<void> => {
  await writeFakeRecyclingMuonExecutable(muonDirectory, outputDirectory);
  await mkdir(join(muonDirectory, "plugins"), { recursive: true });
  await writeFile(join(muonDirectory, "plugins", "plugin.txt"), "plugin\n");
};

const writeFakeDirectRecyclingMuonExecutable = async (
  runtimeDirectory: string,
  outputDirectory: string,
): Promise<void> => {
  const executableName =
    process.platform === "win32" ? "muon-core.exe" : "muon-core";
  const executable = join(runtimeDirectory, executableName);
  const escapedOutputDirectory = outputDirectory.replaceAll("'", "'\\''");
  await mkdir(runtimeDirectory, { recursive: true });
  await writeFile(
    executable,
    `#!/usr/bin/env bash
set -euo pipefail
counter_file='${escapedOutputDirectory}/direct-recycle-count.txt'
count=0
if [[ -f "$counter_file" ]]; then
  count="$(cat "$counter_file")"
fi
count=$((count + 1))
printf '%s\\n' "$count" > "$counter_file"
pwd > "${escapedOutputDirectory}/direct-cwd-$count.txt"
if [[ "$count" -eq 1 ]]; then
  exit 88
fi
`,
  );
  await chmod(executable, 0o755);
};

const writeFakeCefDirectory = async (): Promise<string> => {
  const cefDirectory = await createTemporaryDirectory("muon-vite-cef-dir-");
  await mkdir(join(cefDirectory, "Release"), { recursive: true });
  await mkdir(join(cefDirectory, "Resources", "locales"), {
    recursive: true,
  });
  await writeFile(join(cefDirectory, "Release", "libcef.so"), "cef\n");
  await writeFile(join(cefDirectory, "Resources", "icudtl.dat"), "cef\n");
  await writeFile(
    join(cefDirectory, "Resources", "locales", "en-US.pak"),
    "locale\n",
  );
  return cefDirectory;
};

const writeFakeMuonSource = async (
  muonDirectory: string,
  outputDirectory: string,
): Promise<void> => {
  await writeFakeMuonExecutable(muonDirectory, outputDirectory);
  await mkdir(join(muonDirectory, "plugins"), { recursive: true });
  await writeFile(join(muonDirectory, "plugins", "plugin.txt"), "plugin\n");
};

const writeFakeBrowserExecutable = async (
  outputDirectory: string,
): Promise<string> => {
  const executable = join(outputDirectory, "browser.sh");
  await writeFile(
    executable,
    `#!/usr/bin/env bash
set -euo pipefail
printf '%s\\n' "$@" > '${outputDirectory.replaceAll("'", "'\\''")}/browser-args.txt'
`,
  );
  await chmod(executable, 0o755);
  fakeBrowserExecutablePaths.add(executable);
  return executable;
};

interface StartServerPluginOptions {
  muonPath: string;
  cefPath: string | undefined;
  stagePath: string | undefined;
  enableDebugger: boolean | undefined;
  open: boolean | undefined;
  pluginAccess?: false;
}

const startServer = async (
  root: string,
  pluginOptions: StartServerPluginOptions,
  serverOpen: boolean | string | undefined,
  logger?: Logger,
): Promise<ViteDevServer> => {
  if (
    serverOpen !== undefined &&
    serverOpen !== false &&
    !fakeBrowserExecutablePaths.has(process.env.BROWSER ?? "")
  ) {
    throw new Error(
      "Vite server.open tests must set BROWSER to a fake executable.",
    );
  }

  const muonPluginOptions = {
    muonPath: pluginOptions.muonPath,
    ...(pluginOptions.cefPath === undefined
      ? {}
      : { cefPath: pluginOptions.cefPath }),
    ...(pluginOptions.stagePath === undefined
      ? {}
      : { stagePath: pluginOptions.stagePath }),
    ...(pluginOptions.enableDebugger === undefined
      ? {}
      : { enableDebugger: pluginOptions.enableDebugger }),
    ...(pluginOptions.open === undefined ? {} : { open: pluginOptions.open }),
    ...(pluginOptions.pluginAccess === undefined
      ? {}
      : { pluginAccess: pluginOptions.pluginAccess }),
  };
  const server = await createServer({
    root,
    logLevel: "silent",
    ...(logger === undefined ? {} : { customLogger: logger }),
    server: {
      host: "127.0.0.1",
      port: 0,
      ...(serverOpen === undefined ? {} : { open: serverOpen }),
    },
    plugins: [muon(muonPluginOptions)],
  });
  servers.push(server);
  await server.listen();
  return server;
};

interface CommandResult {
  exitCode: number;
  stdout: string;
  stderr: string;
}

const getCliPath = (): string => resolve("dist", "cli.cjs");

const getMuonCoreDevSupervisorPath = (): string =>
  resolve("..", "muon-core", "scripts", "run-dev-supervisor.mjs");

const runMuonCli = async (
  root: string,
  args: readonly string[],
): Promise<CommandResult> => {
  try {
    const result = await execFileAsync(
      process.execPath,
      [getCliPath(), ...args],
      {
        cwd: root,
        encoding: "utf8",
        env: process.env,
      },
    );
    return {
      exitCode: 0,
      stdout: result.stdout,
      stderr: result.stderr,
    };
  } catch (error) {
    const execError = error as Error & {
      code?: number;
      stdout?: string;
      stderr?: string;
    };
    return {
      exitCode: execError.code ?? 1,
      stdout: execError.stdout ?? "",
      stderr: execError.stderr ?? "",
    };
  }
};

const writeDevAssets = async (root: string): Promise<string> => {
  const assetsPath = join(root, "assets");
  await mkdir(join(assetsPath, "main"), { recursive: true });
  await writeFile(
    join(assetsPath, "main", "index.html"),
    "<!doctype html><title>muon run test</title>",
  );
  return assetsPath;
};

const writeMuonViteConfig = async (
  root: string,
  source: string,
): Promise<void> => {
  const vitePluginUrl = pathToFileURL(resolve("dist", "vite.mjs")).href;
  await writeFile(
    join(root, "vite.config.mjs"),
    source.replaceAll("__MUON_VITE_URL__", vitePluginUrl),
  );
};

const readCapturedArguments = async (
  outputDirectory: string,
): Promise<string[]> => {
  const content = await readFile(join(outputDirectory, "args.txt"), "utf8");
  return content.trim().length === 0 ? [] : content.trim().split("\n");
};

const pathEndsWith = (value: string, suffix: string): boolean =>
  value.replaceAll("\\", "/").endsWith(suffix);

afterEach(async () => {
  for (const server of servers.splice(0)) {
    await server.close();
  }
  for (const directory of cleanupDirectories.splice(0)) {
    await rm(directory, { recursive: true, force: true });
  }
  if (originalBrowser === undefined) {
    delete process.env.BROWSER;
  } else {
    process.env.BROWSER = originalBrowser;
  }
  if (originalCacheDir === undefined) {
    delete process.env.MUON_CACHE_DIR;
  } else {
    process.env.MUON_CACHE_DIR = originalCacheDir;
  }
  fakeBrowserExecutablePaths.clear();
});

describe("muon Vite plugin", () => {
  it("uses the packaged runtime for the host target by default", () => {
    const packageDistDirectory = join("node_modules", "muon-ui", "dist");

    expect(
      resolveMuonRuntimePath({
        root: "/project",
        target: "linux-amd64",
        muonPath: undefined,
        packageDirectory: packageDistDirectory,
      }),
    ).toBe(join(packageDistDirectory, "runtime", "linux-amd64"));
  });

  it("keeps explicit muonPath for custom core builds", () => {
    expect(
      resolveMuonRuntimePath({
        root: "/project",
        target: "linux-amd64",
        muonPath: "../custom-muon-core",
        packageDirectory: join("node_modules", "muon-ui", "dist"),
      }),
    ).toBe(resolve("/custom-muon-core"));
  });

  it("ignores generated .muon staging files while preserving custom watch ignores", async () => {
    const root = await createTemporaryDirectory("muon-vite-watch-ignore-");
    await writeBasicViteProject(root);
    const watchChanges: string[] = [];
    const customIgnoredPath = join(root, "custom.tmp");
    const server = await createServer({
      root,
      logLevel: "silent",
      server: {
        host: "127.0.0.1",
        port: 0,
        watch: {
          ignored: (path: string): boolean => path === customIgnoredPath,
        },
      },
      plugins: [
        muon({ open: false }),
        {
          name: "muon-watch-change-capture",
          watchChange: (id): void => {
            watchChanges.push(id);
          },
        },
      ],
    });
    servers.push(server);
    await server.listen();

    watchChanges.length = 0;
    await writeFile(
      join(root, "index.html"),
      "<!doctype html><title>changed</title>",
    );
    await wait(() =>
      watchChanges.some((id) => pathEndsWith(id, "/index.html")),
    );

    watchChanges.length = 0;
    await writeFile(customIgnoredPath, "ignored\n");
    await delay(500);
    expect(watchChanges.some((id) => pathEndsWith(id, "/custom.tmp"))).toBe(
      false,
    );

    watchChanges.length = 0;
    const stagingFilePath = join(root, ".muon", "linux-amd64", "CREDITS.html");
    await mkdir(dirname(stagingFilePath), { recursive: true });
    await writeFile(stagingFilePath, "ignored\n");
    await delay(500);
    expect(watchChanges.some((id) => id.includes("/.muon/"))).toBe(false);
  });

  it("launches Muon when server.open is false", async () => {
    const root = await createTemporaryDirectory("muon-vite-server-closed-");
    const muonDirectory = await createTemporaryDirectory("muon-vite-muon-");
    const outputDirectory = await createTemporaryDirectory("muon-vite-output-");
    const cefDirectory = await writeFakeCefDirectory();
    await writeBasicViteProject(root);
    await writeProjectMuonConfig(root);
    await writeFakeMuonSource(muonDirectory, outputDirectory);
    process.env.BROWSER = "existing-browser";
    process.env.MUON_CACHE_DIR =
      await createTemporaryDirectory("muon-vite-cache-");

    await startServer(
      root,
      {
        muonPath: muonDirectory,
        cefPath: cefDirectory,
        stagePath: undefined,
        enableDebugger: undefined,
        open: undefined,
      },
      false,
    );
    await wait(() => existsSync(join(outputDirectory, "override.json")));

    expect(process.env.BROWSER).toBe("existing-browser");
    await expect(readFile(join(root, ".gitignore"), "utf8")).resolves.toBe(
      ".muon/\ndist-muon/\nartifacts/\n",
    );
    await expect(
      access(join(root, ".muon", "linux-amd64")),
    ).resolves.toBeUndefined();
  });

  it("forwards native prepare phase progress while starting Muon", async () => {
    const root = await createTemporaryDirectory("muon-vite-prepare-progress-");
    const muonDirectory = await createTemporaryDirectory("muon-vite-muon-");
    const outputDirectory = await createTemporaryDirectory("muon-vite-output-");
    const cefDirectory = await writeFakeCefDirectory();
    const chunks: string[] = [];
    await writeBasicViteProject(root);
    await writeProjectMuonConfig(root);
    await writeFakeMuonSource(muonDirectory, outputDirectory);
    process.env.MUON_CACHE_DIR =
      await createTemporaryDirectory("muon-vite-cache-");
    const write = vi.spyOn(process.stderr, "write").mockImplementation(((
      chunk: string | Uint8Array,
    ) => {
      chunks.push(
        typeof chunk === "string" ? chunk : Buffer.from(chunk).toString("utf8"),
      );
      return true;
    }) as typeof process.stderr.write);
    try {
      await startServer(
        root,
        {
          muonPath: muonDirectory,
          cefPath: cefDirectory,
          stagePath: undefined,
          enableDebugger: undefined,
          open: undefined,
        },
        false,
      );
      await wait(() => existsSync(join(outputDirectory, "override.json")));
    } finally {
      write.mockRestore();
    }

    const stderr = chunks.join("");
    expect(stderr).toContain("Installing CEF runtime...");
    expect(stderr).toContain("Starting Muon...");
  });

  it("does not launch Muon when the plugin open option is false", async () => {
    const root = await createTemporaryDirectory("muon-vite-closed-");
    const muonDirectory = await createTemporaryDirectory("muon-vite-muon-");
    await writeBasicViteProject(root);
    process.env.BROWSER = "existing-browser";

    await startServer(
      root,
      {
        muonPath: muonDirectory,
        cefPath: undefined,
        stagePath: undefined,
        enableDebugger: undefined,
        open: false,
      },
      undefined,
    );

    expect(process.env.BROWSER).toBe("existing-browser");
    await expect(readFile(join(root, ".gitignore"), "utf8")).resolves.toBe(
      ".muon/\ndist-muon/\nartifacts/\n",
    );
    await expect(access(join(root, ".muon"))).rejects.toThrow();
  });

  it("resolves validate-mode executor virtual modules from allowed importers", async () => {
    const root = await createTemporaryDirectory("muon-vite-capability-");
    await mkdir(join(root, "src", "native"), { recursive: true });
    await writeFile(
      join(root, "index.html"),
      '<script type="module" src="/src/main.ts"></script>',
    );
    await writeFile(
      join(root, "src", "main.ts"),
      'import { runNode } from "./native/executor";\nvoid runNode;\n',
    );
    await writeFile(
      join(root, "src", "native", "executor.ts"),
      [
        'import { spawn } from "muon:executor";',
        "export const runNode = async () =>",
        "  await spawn({ command: 'node', args: ['script.js'] });",
        "",
      ].join("\n"),
    );

    await viteBuild({
      root,
      logLevel: "silent",
      plugins: [
        muon({
          open: false,
          build: false,
          pluginAccess: {
            imports: [
              {
                from: ["src/native/**"],
                allow: ["muon.executor.spawn"],
              },
            ],
          },
        }),
      ],
    });

    await expect(
      access(join(root, "dist", "index.html")),
    ).resolves.toBeUndefined();
  });

  it("rejects validate-mode executor virtual modules from disallowed importers", async () => {
    const root = await createTemporaryDirectory("muon-vite-capability-deny-");
    await mkdir(join(root, "src"), { recursive: true });
    await writeFile(
      join(root, "index.html"),
      '<script type="module" src="/src/main.ts"></script>',
    );
    await writeFile(
      join(root, "src", "main.ts"),
      ['import { spawn } from "muon:executor";', "void spawn;", ""].join("\n"),
    );

    await expect(
      viteBuild({
        root,
        logLevel: "silent",
        plugins: [
          muon({
            open: false,
            build: false,
            pluginAccess: {
              imports: [
                {
                  from: ["src/native/**"],
                  allow: ["muon.executor.spawn"],
                },
              ],
            },
          }),
        ],
      }),
    ).rejects.toThrow("Muon capability import is not allowed");
  });

  it("launches Muon without server.open", async () => {
    const root = await createTemporaryDirectory("muon-vite-open-");
    const muonDirectory = await createTemporaryDirectory("muon-vite-muon-");
    const outputDirectory = await createTemporaryDirectory("muon-vite-output-");
    const cefDirectory = await writeFakeCefDirectory();
    await writeBasicViteProject(root);
    await writeProjectMuonConfig(root);
    await writeFakeMuonSource(muonDirectory, outputDirectory);
    process.env.BROWSER = "existing-browser";
    process.env.MUON_CACHE_DIR =
      await createTemporaryDirectory("muon-vite-cache-");

    const server = await startServer(
      root,
      {
        muonPath: muonDirectory,
        cefPath: cefDirectory,
        stagePath: undefined,
        enableDebugger: undefined,
        open: undefined,
      },
      undefined,
    );
    await wait(() => existsSync(join(outputDirectory, "override.json")));

    const stagePath = join(root, ".muon", "linux-amd64");
    const args = (await readFile(join(outputDirectory, "args.txt"), "utf8"))
      .trim()
      .split("\n");
    const cwd = await readFile(join(outputDirectory, "cwd.txt"), "utf8");
    const overrideConfigPath = args[3];
    const overrideConfig = JSON.parse(
      await readFile(join(outputDirectory, "override.json"), "utf8"),
    ) as {
      cdp: { enable: boolean };
      browser: {
        startPage: string;
        keybind: { devtools: string; recycle: string };
        plugin: {
          allow: string[];
          capabilities: unknown[];
          mode: string;
        };
      };
      network: { allow: string[] };
    };
    const baseUrl = server.resolvedUrls?.local[0];
    expect(baseUrl).toBeDefined();
    expect(args).toEqual([
      "-c",
      join(root, "muon.json"),
      "-c",
      overrideConfigPath,
    ]);
    expect(cwd).toBe(`${stagePath}\n`);
    await expect(access(join(stagePath, "libcef.so"))).resolves.toBeUndefined();
    await expect(
      access(join(stagePath, "icudtl.dat")),
    ).resolves.toBeUndefined();
    await expect(
      access(join(stagePath, "locales", "en-US.pak")),
    ).resolves.toBeUndefined();
    await expect(
      access(join(stagePath, "plugins", "plugin.txt")),
    ).resolves.toBeUndefined();
    expect(overrideConfig).toEqual({
      cdp: {
        enable: true,
      },
      browser: {
        startPage: baseUrl,
        keybind: {
          devtools: "f12",
          recycle: "ctrl+f12",
        },
        plugin: {
          allow: [`${new URL(baseUrl ?? "").origin}/**`],
          capabilities: [],
          mode: "validate",
        },
      },
      network: {
        allow: [
          `${new URL(baseUrl ?? "").origin}/**`,
          `${new URL(baseUrl ?? "").origin.replace(/^http:/, "ws:")}/**`,
        ],
      },
    });
    expect(process.env.BROWSER).toBe("existing-browser");

    await server.close();
    expect(process.env.BROWSER).toBe("existing-browser");
    await expect(access(dirname(overrideConfigPath ?? ""))).rejects.toThrow();
  });

  it("launches both the browser and Muon when server.open is true", async () => {
    const root = await createTemporaryDirectory("muon-vite-browser-and-muon-");
    const muonDirectory = await createTemporaryDirectory("muon-vite-muon-");
    const outputDirectory = await createTemporaryDirectory("muon-vite-output-");
    const cefDirectory = await writeFakeCefDirectory();
    await writeBasicViteProject(root);
    await writeProjectMuonConfig(root);
    await writeFakeMuonSource(muonDirectory, outputDirectory);
    process.env.BROWSER = await writeFakeBrowserExecutable(outputDirectory);
    process.env.MUON_CACHE_DIR =
      await createTemporaryDirectory("muon-vite-cache-");

    const server = await startServer(
      root,
      {
        muonPath: muonDirectory,
        cefPath: cefDirectory,
        stagePath: undefined,
        enableDebugger: undefined,
        open: undefined,
      },
      true,
    );
    await wait(
      () =>
        existsSync(join(outputDirectory, "override.json")) &&
        existsSync(join(outputDirectory, "browser-args.txt")),
    );

    const browserArgs = (
      await readFile(join(outputDirectory, "browser-args.txt"), "utf8")
    )
      .trim()
      .split("\n");
    const baseUrl = server.resolvedUrls?.local[0];
    expect(baseUrl).toBeDefined();
    expect(browserArgs).toContain(baseUrl);
  });

  it("uses simple plugin mode when pluginAccess is false", async () => {
    const root = await createTemporaryDirectory("muon-vite-simple-mode-");
    const muonDirectory = await createTemporaryDirectory("muon-vite-muon-");
    const outputDirectory = await createTemporaryDirectory("muon-vite-output-");
    const cefDirectory = await writeFakeCefDirectory();
    await writeBasicViteProject(root);
    await writeProjectMuonConfig(root);
    await writeFakeMuonSource(muonDirectory, outputDirectory);
    process.env.BROWSER = "existing-browser";
    process.env.MUON_CACHE_DIR =
      await createTemporaryDirectory("muon-vite-cache-");

    const server = await startServer(
      root,
      {
        muonPath: muonDirectory,
        cefPath: cefDirectory,
        stagePath: undefined,
        enableDebugger: undefined,
        open: undefined,
        pluginAccess: false,
      },
      undefined,
    );
    try {
      await wait(() => existsSync(join(outputDirectory, "override.json")));

      const overrideConfig = JSON.parse(
        await readFile(join(outputDirectory, "override.json"), "utf8"),
      ) as {
        browser: {
          plugin: {
            capabilities?: unknown[];
            mode: string;
          };
        };
      };
      expect(overrideConfig.browser.plugin.mode).toBe("simple");
      expect(overrideConfig.browser.plugin.capabilities).toBeUndefined();
    } finally {
      await server.close();
    }
  });

  it("omits the debugger override when enableDebugger is false", async () => {
    const root = await createTemporaryDirectory("muon-vite-debugger-off-");
    const muonDirectory = await createTemporaryDirectory("muon-vite-muon-");
    const outputDirectory = await createTemporaryDirectory("muon-vite-output-");
    const cefDirectory = await writeFakeCefDirectory();
    await writeBasicViteProject(root);
    await writeProjectMuonConfig(root);
    await writeFakeMuonSource(muonDirectory, outputDirectory);
    process.env.MUON_CACHE_DIR =
      await createTemporaryDirectory("muon-vite-cache-");

    await startServer(
      root,
      {
        muonPath: muonDirectory,
        cefPath: cefDirectory,
        stagePath: undefined,
        enableDebugger: false,
        open: undefined,
      },
      undefined,
    );
    await wait(() => existsSync(join(outputDirectory, "override.json")));

    const overrideConfig = JSON.parse(
      await readFile(join(outputDirectory, "override.json"), "utf8"),
    ) as {
      cdp?: unknown;
      browser: { keybind?: unknown };
    };
    expect(overrideConfig.cdp).toBeUndefined();
    expect(overrideConfig.browser.keybind).toBeUndefined();
  });

  it("starts with only the generated override config when project config is missing", async () => {
    const root = await createTemporaryDirectory("muon-vite-missing-config-");
    const muonDirectory = await createTemporaryDirectory("muon-vite-muon-");
    const outputDirectory = await createTemporaryDirectory("muon-vite-output-");
    const cefDirectory = await writeFakeCefDirectory();
    const { logger, warnings } = createCapturingLogger();
    await writeBasicViteProject(root);
    await writeFakeMuonSource(muonDirectory, outputDirectory);
    process.env.MUON_CACHE_DIR =
      await createTemporaryDirectory("muon-vite-cache-");

    await startServer(
      root,
      {
        muonPath: muonDirectory,
        cefPath: cefDirectory,
        stagePath: undefined,
        enableDebugger: undefined,
        open: undefined,
      },
      undefined,
      logger,
    );
    await wait(() => existsSync(join(outputDirectory, "override.json")));

    const args = (await readFile(join(outputDirectory, "args.txt"), "utf8"))
      .trim()
      .split("\n");
    const overrideConfigPath = args[1];
    expect(args).toEqual(["-c", overrideConfigPath]);
    expect(warnings.join("\n")).toContain("Muon project config was not found");
    expect(warnings.join("\n")).toContain("generated Vite config only");
  });

  it("warns and ignores an invalid project config during Vite dev startup", async () => {
    const root = await createTemporaryDirectory("muon-vite-invalid-config-");
    const muonDirectory = await createTemporaryDirectory("muon-vite-muon-");
    const outputDirectory = await createTemporaryDirectory("muon-vite-output-");
    const cefDirectory = await writeFakeCefDirectory();
    const { logger, warnings } = createCapturingLogger();
    await writeBasicViteProject(root);
    await writeFile(join(root, "muon.json"), "{ invalid json\n");
    await writeFakeMuonSource(muonDirectory, outputDirectory);
    process.env.MUON_CACHE_DIR =
      await createTemporaryDirectory("muon-vite-cache-");

    await startServer(
      root,
      {
        muonPath: muonDirectory,
        cefPath: cefDirectory,
        stagePath: undefined,
        enableDebugger: undefined,
        open: undefined,
      },
      undefined,
      logger,
    );
    await wait(() => existsSync(join(outputDirectory, "override.json")));

    const args = (await readFile(join(outputDirectory, "args.txt"), "utf8"))
      .trim()
      .split("\n");
    const overrideConfigPath = args[1];
    expect(args).toEqual(["-c", overrideConfigPath]);
    expect(warnings.join("\n")).toContain(join(root, "muon.json"));
    expect(warnings.join("\n")).toContain("will be ignored");
  });

  it("uses the Vite base URL as the Muon start page when server.open is a string", async () => {
    const root = await createTemporaryDirectory("muon-vite-path-");
    const muonDirectory = await createTemporaryDirectory("muon-vite-muon-");
    const outputDirectory = await createTemporaryDirectory("muon-vite-output-");
    const cefDirectory = await writeFakeCefDirectory();
    await writeBasicViteProject(root);
    await writeProjectMuonConfig(root);
    await writeFakeMuonSource(muonDirectory, outputDirectory);
    process.env.BROWSER = await writeFakeBrowserExecutable(outputDirectory);
    process.env.MUON_CACHE_DIR =
      await createTemporaryDirectory("muon-vite-cache-");

    const server = await startServer(
      root,
      {
        muonPath: muonDirectory,
        cefPath: cefDirectory,
        stagePath: undefined,
        enableDebugger: undefined,
        open: undefined,
      },
      "/path?x=1",
    );
    await wait(
      () =>
        existsSync(join(outputDirectory, "override.json")) &&
        existsSync(join(outputDirectory, "browser-args.txt")),
    );

    const overrideConfig = JSON.parse(
      await readFile(join(outputDirectory, "override.json"), "utf8"),
    ) as { browser: { startPage: string } };
    const browserArgs = (
      await readFile(join(outputDirectory, "browser-args.txt"), "utf8")
    )
      .trim()
      .split("\n");
    const baseUrl = server.resolvedUrls?.local[0];
    if (baseUrl === undefined) {
      throw new Error("Vite base URL was not resolved.");
    }
    expect(overrideConfig.browser.startPage).toBe(baseUrl);
    expect(browserArgs).toContain(new URL("/path?x=1", baseUrl).href);
  });
});

describe("muon run CLI", () => {
  it("launches Muon directly without a Vite config", async () => {
    const root = await createTemporaryDirectory("muon-dev-direct-");
    const muonDirectory = await createTemporaryDirectory("muon-dev-muon-");
    const outputDirectory = await createTemporaryDirectory("muon-dev-output-");
    const cefDirectory = await writeFakeCefDirectory();
    const assetsPath = await writeDevAssets(root);
    await writeProjectMuonConfig(root);
    await writeFakeMuonSource(muonDirectory, outputDirectory);
    process.env.MUON_CACHE_DIR =
      await createTemporaryDirectory("muon-dev-cache-");

    const result = await runMuonCli(root, [
      "run",
      "--muon-path",
      muonDirectory,
      "--cef-path",
      cefDirectory,
      "--assets",
      assetsPath,
      "--json",
    ]);

    expect(result.stderr).toBe("");
    expect(result.exitCode).toBe(0);
    const devResult = JSON.parse(result.stdout) as {
      exitCode: number;
      assetSourcePath: string;
      overrideConfigPath: string;
      projectConfigPath: string;
      stagePath: string;
    };
    const overrideConfig = JSON.parse(
      await readFile(join(outputDirectory, "override.json"), "utf8"),
    ) as {
      asset: { sourcePath: string };
      cdp: { enable: boolean };
      browser: { keybind: { devtools: string; recycle: string } };
    };

    expect(await readCapturedArguments(outputDirectory)).toEqual([
      "-c",
      join(root, "muon.json"),
      "-c",
      devResult.overrideConfigPath,
    ]);
    await expect(readFile(join(root, ".gitignore"), "utf8")).resolves.toBe(
      ".muon/\ndist-muon/\nartifacts/\n",
    );
    expect(devResult.exitCode).toBe(0);
    expect(devResult.projectConfigPath).toBe(join(root, "muon.json"));
    expect(devResult.assetSourcePath).toBe(assetsPath);
    expect(devResult.stagePath).toBe(join(root, ".muon", "linux-amd64"));
    expect(overrideConfig).toEqual({
      asset: {
        sourcePath: assetsPath,
      },
      cdp: {
        enable: true,
      },
      browser: {
        keybind: {
          devtools: "f12",
          recycle: "ctrl+f12",
        },
      },
    });
  });

  it("restarts Muon when the direct run process requests recycle", async () => {
    const root = await createTemporaryDirectory("muon-dev-recycle-");
    const muonDirectory = await createTemporaryDirectory("muon-dev-muon-");
    const outputDirectory = await createTemporaryDirectory("muon-dev-output-");
    const cefDirectory = await writeFakeCefDirectory();
    const assetsPath = await writeDevAssets(root);
    await writeProjectMuonConfig(root);
    await writeFakeRecyclingMuonSource(muonDirectory, outputDirectory);
    process.env.MUON_CACHE_DIR =
      await createTemporaryDirectory("muon-dev-cache-");

    const result = await runMuonCli(root, [
      "run",
      "--muon-path",
      muonDirectory,
      "--cef-path",
      cefDirectory,
      "--assets",
      assetsPath,
      "--json",
    ]);

    expect(result.stderr).toBe("");
    expect(result.exitCode).toBe(0);
    const devResult = JSON.parse(result.stdout) as {
      exitCode: number;
      overrideConfigPath: string;
    };
    expect(devResult.exitCode).toBe(0);
    await expect(
      readFile(join(outputDirectory, "recycle-count.txt"), "utf8"),
    ).resolves.toBe("2\n");
    expect(await readCapturedArguments(outputDirectory)).toEqual([
      "-c",
      join(root, "muon.json"),
      "-c",
      devResult.overrideConfigPath,
    ]);
  });

  it("restarts Muon from the muon-core dev supervisor", async () => {
    const runtimeDirectory = await createTemporaryDirectory(
      "muon-core-dev-runtime-",
    );
    const outputDirectory = await createTemporaryDirectory(
      "muon-core-dev-output-",
    );
    await writeFakeDirectRecyclingMuonExecutable(
      runtimeDirectory,
      outputDirectory,
    );

    const result = await execFileAsync(
      process.execPath,
      [getMuonCoreDevSupervisorPath(), runtimeDirectory],
      {
        encoding: "utf8",
      },
    );

    expect(result.stderr).toBe("");
    expect(result.stdout).toBe("");
    await expect(
      readFile(join(outputDirectory, "direct-recycle-count.txt"), "utf8"),
    ).resolves.toBe("2\n");
    await expect(
      readFile(join(outputDirectory, "direct-cwd-1.txt"), "utf8"),
    ).resolves.toBe(`${runtimeDirectory}\n`);
    await expect(
      readFile(join(outputDirectory, "direct-cwd-2.txt"), "utf8"),
    ).resolves.toBe(`${runtimeDirectory}\n`);
  });

  it("uses muon Vite plugin options when vite.config.* is present", async () => {
    const root = await createTemporaryDirectory("muon-dev-vite-config-");
    const muonDirectory = await createTemporaryDirectory("muon-dev-muon-");
    const outputDirectory = await createTemporaryDirectory("muon-dev-output-");
    const cefDirectory = await writeFakeCefDirectory();
    const assetsPath = await writeDevAssets(root);
    await writeFakeMuonSource(muonDirectory, outputDirectory);
    await writeMuonViteConfig(
      root,
      [
        'import muon from "__MUON_VITE_URL__";',
        "export default {",
        "  plugins: [",
        `    muon({ muonPath: ${JSON.stringify(muonDirectory)}, cefPath: ${JSON.stringify(cefDirectory)}, stagePath: "custom-stage", enableDebugger: false, open: false, build: { targets: ["linux-amd64"] } }),`,
        "  ],",
        "};",
      ].join("\n"),
    );
    process.env.MUON_CACHE_DIR =
      await createTemporaryDirectory("muon-dev-cache-");

    const result = await runMuonCli(root, [
      "run",
      "--assets",
      assetsPath,
      "--json",
    ]);

    expect(result.stderr).toBe("");
    expect(result.exitCode).toBe(0);
    const devResult = JSON.parse(result.stdout) as {
      stagePath: string;
      overrideConfigPath: string;
    };
    const overrideConfig = JSON.parse(
      await readFile(join(outputDirectory, "override.json"), "utf8"),
    ) as {
      asset: { sourcePath: string };
      cdp?: unknown;
      browser?: { keybind?: unknown };
    };

    expect(devResult.stagePath).toBe(join(root, "custom-stage"));
    expect(await readCapturedArguments(outputDirectory)).toEqual([
      "-c",
      devResult.overrideConfigPath,
    ]);
    expect(overrideConfig.asset.sourcePath).toBe(assetsPath);
    expect(overrideConfig.cdp).toBeUndefined();
    expect(overrideConfig.browser?.keybind).toBeUndefined();
  });

  it("lets CLI options override muon Vite plugin options", async () => {
    const root = await createTemporaryDirectory("muon-dev-cli-override-");
    const pluginMuonDirectory = await createTemporaryDirectory(
      "muon-dev-plugin-muon-",
    );
    const pluginOutputDirectory = await createTemporaryDirectory(
      "muon-dev-plugin-output-",
    );
    const cliMuonDirectory =
      await createTemporaryDirectory("muon-dev-cli-muon-");
    const cliOutputDirectory = await createTemporaryDirectory(
      "muon-dev-cli-output-",
    );
    const pluginCefDirectory = await writeFakeCefDirectory();
    const cliCefDirectory = await writeFakeCefDirectory();
    const assetsPath = await writeDevAssets(root);
    const cliStagePath = join(root, "cli-stage");
    await writeFakeMuonSource(pluginMuonDirectory, pluginOutputDirectory);
    await writeFakeMuonSource(cliMuonDirectory, cliOutputDirectory);
    await writeMuonViteConfig(
      root,
      [
        'import muon from "__MUON_VITE_URL__";',
        "export default {",
        "  plugins: [",
        `    muon({ muonPath: ${JSON.stringify(pluginMuonDirectory)}, cefPath: ${JSON.stringify(pluginCefDirectory)}, stagePath: "plugin-stage", enableDebugger: true }),`,
        "  ],",
        "};",
      ].join("\n"),
    );
    process.env.MUON_CACHE_DIR =
      await createTemporaryDirectory("muon-dev-cache-");

    const result = await runMuonCli(root, [
      "run",
      "--muon-path",
      cliMuonDirectory,
      "--cef-path",
      cliCefDirectory,
      "--stage-dir",
      cliStagePath,
      "--assets",
      assetsPath,
      "--no-debugger",
      "--json",
    ]);

    expect(result.stderr).toBe("");
    expect(result.exitCode).toBe(0);
    const devResult = JSON.parse(result.stdout) as {
      stagePath: string;
    };
    const overrideConfig = JSON.parse(
      await readFile(join(cliOutputDirectory, "override.json"), "utf8"),
    ) as {
      cdp?: unknown;
      browser?: { keybind?: unknown };
    };

    expect(devResult.stagePath).toBe(cliStagePath);
    expect(overrideConfig.cdp).toBeUndefined();
    expect(overrideConfig.browser?.keybind).toBeUndefined();
    await expect(
      access(join(pluginOutputDirectory, "override.json")),
    ).rejects.toThrow();
  });

  it("runs with defaults when vite.config.* has no muon plugin", async () => {
    const root = await createTemporaryDirectory("muon-dev-vite-no-plugin-");
    const muonDirectory = await createTemporaryDirectory("muon-dev-muon-");
    const outputDirectory = await createTemporaryDirectory("muon-dev-output-");
    const cefDirectory = await writeFakeCefDirectory();
    const assetsPath = await writeDevAssets(root);
    await writeFakeMuonSource(muonDirectory, outputDirectory);
    await writeMuonViteConfig(
      root,
      ["export default {", '  plugins: [{ name: "other-plugin" }],', "};"].join(
        "\n",
      ),
    );
    process.env.MUON_CACHE_DIR =
      await createTemporaryDirectory("muon-dev-cache-");

    const result = await runMuonCli(root, [
      "run",
      "--muon-path",
      muonDirectory,
      "--cef-path",
      cefDirectory,
      "--assets",
      assetsPath,
      "--json",
    ]);

    expect(result.stderr).toBe("");
    expect(result.exitCode).toBe(0);
    await expect(
      access(join(outputDirectory, "override.json")),
    ).resolves.toBeUndefined();
  });

  it("fails when vite.config.* contains multiple muon plugins", async () => {
    const root = await createTemporaryDirectory("muon-dev-vite-multiple-");
    await writeDevAssets(root);
    await writeMuonViteConfig(
      root,
      [
        'import muon from "__MUON_VITE_URL__";',
        "export default {",
        "  plugins: [muon(), muon()],",
        "};",
      ].join("\n"),
    );

    const result = await runMuonCli(root, ["run", "--json"]);

    expect(result.exitCode).toBe(1);
    expect(result.stderr).toContain(
      "Multiple muon() plugin definitions were found in vite.config.*.",
    );
  });

  it("fails before launch when an explicit asset source is missing", async () => {
    const root = await createTemporaryDirectory("muon-dev-missing-assets-");
    const muonDirectory = await createTemporaryDirectory("muon-dev-muon-");
    const outputDirectory = await createTemporaryDirectory("muon-dev-output-");
    const cefDirectory = await writeFakeCefDirectory();
    await writeFakeMuonSource(muonDirectory, outputDirectory);

    const result = await runMuonCli(root, [
      "run",
      "--muon-path",
      muonDirectory,
      "--cef-path",
      cefDirectory,
      "--assets",
      join(root, "missing-assets"),
      "--json",
    ]);

    expect(result.exitCode).toBe(1);
    expect(result.stderr).toContain("Muon run asset source does not exist");
    await expect(
      access(join(outputDirectory, "override.json")),
    ).rejects.toThrow();
  });
});

describe("muon launch scripts", () => {
  it("runs the POSIX script with paths containing spaces", async () => {
    const root = await createTemporaryDirectory("muon vite shell root ");
    const runtimeDirectory = join(root, "runtime dir");
    const outputDirectory = join(root, "output dir");
    const projectConfigPath = join(root, "project config", "muon.json");
    const overrideConfigPath = join(root, "override config", "muon.vite.json");
    await mkdir(dirname(projectConfigPath), { recursive: true });
    await mkdir(dirname(overrideConfigPath), { recursive: true });
    await mkdir(outputDirectory, { recursive: true });
    await writeFile(projectConfigPath, "{}\n");
    await writeFile(overrideConfigPath, "{}\n");
    await writeFakeMuonExecutable(runtimeDirectory, outputDirectory);
    const scriptPath = join(root, "launch script.sh");
    await writeFile(
      scriptPath,
      createMuonLaunchScript({
        muonExecutablePath: getMuonExecutablePath(runtimeDirectory, "linux"),
        projectConfigPath,
        overrideConfigPath,
        platform: "linux",
      }),
    );
    await chmod(scriptPath, 0o755);

    await execFileAsync(scriptPath, ["http://127.0.0.1:5173/"]);

    await expect(
      stat(join(outputDirectory, "override.json")),
    ).resolves.toBeDefined();
    await expect(
      readFile(join(outputDirectory, "cwd.txt"), "utf8"),
    ).resolves.toBe(`${runtimeDirectory}\n`);
  });

  it("restarts from the POSIX script when Muon requests recycle", async () => {
    const root = await createTemporaryDirectory("muon vite shell recycle ");
    const runtimeDirectory = join(root, "runtime dir");
    const outputDirectory = join(root, "output dir");
    const projectConfigPath = join(root, "project config", "muon.json");
    const overrideConfigPath = join(root, "override config", "muon.vite.json");
    await mkdir(dirname(projectConfigPath), { recursive: true });
    await mkdir(dirname(overrideConfigPath), { recursive: true });
    await mkdir(outputDirectory, { recursive: true });
    await writeFile(projectConfigPath, "{}\n");
    await writeFile(overrideConfigPath, "{}\n");
    await writeFakeRecyclingMuonExecutable(runtimeDirectory, outputDirectory);
    const scriptPath = join(root, "launch script.sh");
    await writeFile(
      scriptPath,
      createMuonLaunchScript({
        muonExecutablePath: getMuonExecutablePath(runtimeDirectory, "linux"),
        projectConfigPath,
        overrideConfigPath,
        platform: "linux",
      }),
    );
    await chmod(scriptPath, 0o755);

    await execFileAsync(scriptPath, ["http://127.0.0.1:5173/"]);

    await expect(
      readFile(join(outputDirectory, "recycle-count.txt"), "utf8"),
    ).resolves.toBe("2\n");
    await expect(
      readFile(join(outputDirectory, "args-2.txt"), "utf8"),
    ).resolves.toBe(`-c\n${projectConfigPath}\n-c\n${overrideConfigPath}\n`);
  });

  it("runs the POSIX script without a project config", async () => {
    const root = await createTemporaryDirectory("muon vite shell optional ");
    const runtimeDirectory = join(root, "runtime dir");
    const outputDirectory = join(root, "output dir");
    const projectConfigPath = join(root, "project config", "muon.json");
    const overrideConfigPath = join(root, "override config", "muon.vite.json");
    await mkdir(dirname(overrideConfigPath), { recursive: true });
    await mkdir(outputDirectory, { recursive: true });
    await writeFile(overrideConfigPath, "{}\n");
    await writeFakeMuonExecutable(runtimeDirectory, outputDirectory);
    const scriptPath = join(root, "launch script.sh");
    await writeFile(
      scriptPath,
      createMuonLaunchScript({
        muonExecutablePath: getMuonExecutablePath(runtimeDirectory, "linux"),
        projectConfigPath: undefined,
        overrideConfigPath,
        platform: "linux",
      }),
    );
    await chmod(scriptPath, 0o755);

    await execFileAsync(scriptPath, ["http://127.0.0.1:5173/"]);

    await expect(
      readFile(join(outputDirectory, "args.txt"), "utf8"),
    ).resolves.toBe(`-c\n${overrideConfigPath}\n`);
    await expect(
      stat(join(outputDirectory, "override.json")),
    ).resolves.toBeDefined();
  });

  it("creates a Windows script that preserves paths with spaces", () => {
    const script = createMuonLaunchScript({
      muonExecutablePath: "C:\\Muon Runtime\\muon-core.exe",
      projectConfigPath: "C:\\My App\\muon.json",
      overrideConfigPath: "C:\\Temp Dir\\muon.vite.json",
      platform: "win32",
    });

    expect(script).toContain(
      'set "MUON_EXECUTABLE=C:\\Muon Runtime\\muon-core.exe"',
    );
    expect(script).toContain(
      'set "MUON_EXECUTABLE_DIRECTORY=C:\\Muon Runtime"',
    );
    expect(script).toContain('set "MUON_PROJECT_CONFIG=C:\\My App\\muon.json"');
    expect(script).toContain(
      'set "MUON_OVERRIDE_CONFIG=C:\\Temp Dir\\muon.vite.json"',
    );
    expect(script).toContain('pushd "%MUON_EXECUTABLE_DIRECTORY%"');
    expect(script).toContain(
      '"%MUON_EXECUTABLE%" -c "%MUON_PROJECT_CONFIG%" -c "%MUON_OVERRIDE_CONFIG%"',
    );
    expect(script).toContain('"%MUON_EXECUTABLE%" -c "%MUON_OVERRIDE_CONFIG%"');
    expect(script).toContain("popd");
  });
});
