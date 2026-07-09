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
  readdir,
  readFile,
  rm,
  stat,
  writeFile,
} from "node:fs/promises";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { pathToFileURL } from "node:url";
import { promisify } from "node:util";

import AdmZip from "adm-zip";
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
import {
  createMuonBootstrapEmbeddedConfigSlot,
  createMuonEmbeddedConfigSlot,
} from "../src/embed-config.js";
import { createMuonCapabilityModuleResolver } from "../src/capability.js";
import muon, { type MuonVitePluginAccessOptions } from "../src/vite.js";
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

const writeValidatePluginMuonConfig = async (
  root: string,
  allow: string,
): Promise<void> => {
  await writeFile(
    join(root, "muon.json"),
    `${JSON.stringify(
      {
        plugin: {
          mode: "validate",
          plugins: [
            {
              name: "internal",
              imports: [
                {
                  sources: ["src/**"],
                  allow: [allow],
                },
              ],
            },
          ],
        },
      },
      null,
      2,
    )}\n`,
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

const writeFakeControlledRecyclingMuonExecutable = async (
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
project_config=''
config_index=0
previous=''
for argument in "$@"; do
  if [[ "$previous" == '-c' ]]; then
    config_index=$((config_index + 1))
    if [[ "$config_index" -eq 1 ]]; then
      project_config="$argument"
    fi
    override_config="$argument"
  fi
  previous="$argument"
done
if [[ -z "$override_config" ]]; then
  echo 'missing override config' >&2
  exit 1
fi
if [[ "$project_config" != "$override_config" && -f "$project_config" ]]; then
  cp "$project_config" "${escapedOutputDirectory}/project-$count.json"
fi
cp "$override_config" "${escapedOutputDirectory}/override-$count.json"
if [[ "$count" -eq 1 ]]; then
  touch "${escapedOutputDirectory}/ready-1"
  for _ in $(seq 1 200); do
    if [[ -f "${escapedOutputDirectory}/continue-1" ]]; then
      exit 88
    fi
    sleep 0.05
  done
  echo 'timed out waiting for recycle continuation' >&2
  exit 1
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

const writeFakeControlledRecyclingMuonSource = async (
  muonDirectory: string,
  outputDirectory: string,
): Promise<void> => {
  await writeFakeControlledRecyclingMuonExecutable(
    muonDirectory,
    outputDirectory,
  );
  await mkdir(join(muonDirectory, "plugins"), { recursive: true });
  await writeFile(join(muonDirectory, "plugins", "plugin.txt"), "plugin\n");
};

const writeFakeConfigUpdatingRecyclingMuonSource = async (
  muonDirectory: string,
  outputDirectory: string,
  configPath: string,
  nextAllow: string,
): Promise<void> => {
  await mkdir(muonDirectory, { recursive: true });
  const escapedOutputDirectory = outputDirectory.replaceAll("'", "'\\''");
  const escapedConfigPath = configPath.replaceAll("'", "'\\''");
  const nextConfig = JSON.stringify(
    {
      plugin: {
        mode: "validate",
        plugins: [
          {
            name: "internal",
            imports: [
              {
                sources: ["src/**"],
                allow: [nextAllow],
              },
            ],
          },
        ],
      },
    },
    null,
    2,
  );
  const executable = getMuonExecutablePath(muonDirectory, "linux");
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
  cat > '${escapedConfigPath}' <<'MUON_NEXT_CONFIG'
${nextConfig}
MUON_NEXT_CONFIG
  exit 88
fi
`,
  );
  await chmod(executable, 0o755);
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

const writeFakePackagedExecutable = async (
  path: string,
  slot: Buffer,
  prefix: string,
): Promise<void> => {
  await mkdir(dirname(path), { recursive: true });
  await writeFile(
    path,
    Buffer.concat([
      Buffer.from(`${prefix} prefix\n`),
      slot,
      Buffer.from(`\n${prefix} suffix\n`),
    ]),
  );
  await chmod(path, 0o755);
};

const createFakePackageDirectory = async (root: string): Promise<string> => {
  const packageDirectory = join(root, "package-dist");
  const runtimeDirectory = join(packageDirectory, "runtime", "linux-amd64");
  const nativeDirectory = join(packageDirectory, "native", "linux-amd64");
  await writeFakePackagedExecutable(
    join(runtimeDirectory, "muon-core"),
    createMuonEmbeddedConfigSlot(),
    "core",
  );
  await writeFile(join(runtimeDirectory, "libmuon-ui.so"), "ui\n");
  await writeFile(join(runtimeDirectory, "libcardio.so"), "cardio\n");
  await writeFile(join(runtimeDirectory, "CREDITS.md"), "notices\n");
  await writeFakePackagedExecutable(
    join(nativeDirectory, "muon-bootstrap"),
    createMuonBootstrapEmbeddedConfigSlot(),
    "bootstrap",
  );
  return packageDirectory;
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
  pluginAccess?: false | MuonVitePluginAccessOptions;
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

const writeRunViteApplication = async (root: string): Promise<void> => {
  await mkdir(join(root, "src"), { recursive: true });
  await writeFile(
    join(root, "index.html"),
    '<!doctype html><div id="app"></div><script type="module" src="/src/main.ts"></script>',
  );
  await writeFile(
    join(root, "src", "main.ts"),
    'document.querySelector("#app")?.append("muon run vite build");\n',
  );
};

const readRunBuiltJavaScript = async (
  root: string,
  assetBasePath = join(".muon", "run", "assets", "main", "assets"),
): Promise<string> => {
  const assetsDirectory = join(root, assetBasePath);
  const fileNames = await readdir(assetsDirectory);
  const chunks: string[] = [];
  for (const fileName of fileNames) {
    if (!fileName.endsWith(".js")) {
      continue;
    }
    chunks.push(await readFile(join(assetsDirectory, fileName), "utf8"));
  }
  return chunks.join("\n");
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

const readZipEntries = async (
  archivePath: string,
): Promise<Map<string, Buffer>> => {
  const zip = new AdmZip(await readFile(archivePath));
  return new Map(
    zip.getEntries().map((entry) => [entry.entryName, entry.getData()]),
  );
};

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

  it("launches muon when server.open is false", async () => {
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

  it("forwards native prepare phase progress while starting muon", async () => {
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
    expect(stderr).toContain("Starting muon...");
  });

  it("does not launch muon when the plugin open option is false", async () => {
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
            plugins: [
              {
                name: "internal",
                imports: [
                  {
                    sources: ["src/native/**"],
                    allow: ["muon.executor.spawn"],
                  },
                ],
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

  it("packages Vite base-path output under the matching asset URL root", async () => {
    const root = await createTemporaryDirectory("muon-vite-base-pack-");
    const packageDirectory = await createFakePackageDirectory(root);
    await writeFile(
      join(root, "package.json"),
      `${JSON.stringify({ name: "base-path-sample" }, null, 2)}\n`,
    );
    await mkdir(join(root, "src"), { recursive: true });
    await writeFile(
      join(root, "index.html"),
      '<div id="app"></div><script type="module" src="/src/main.ts"></script>',
    );
    await writeFile(
      join(root, "src", "main.ts"),
      'document.querySelector("#app")?.append("ready");\n',
    );

    await viteBuild({
      root,
      base: "/maplibre-gl-layers/",
      logLevel: "silent",
      plugins: [
        muon({
          open: false,
          build: {
            targets: ["linux-amd64"],
            packageDirectory,
            assetSalt: Buffer.from("1234", "hex"),
          },
        }),
      ],
    });

    const outputPath = join(root, "dist-muon", "linux-amd64");
    const entries = await readZipEntries(join(outputPath, "assets.zip"));
    const indexHtml = entries
      .get("main/maplibre-gl-layers/index.html")
      ?.toString("utf8");
    const embeddedCore = await readFile(join(outputPath, "muon-core"));
    expect(indexHtml).toContain("/maplibre-gl-layers/assets/");
    expect(
      [...entries.keys()].some((entry) =>
        entry.startsWith("main/maplibre-gl-layers/assets/"),
      ),
    ).toBe(true);
    expect(entries.has("main/index.html")).toBe(false);
    expect(
      embeddedCore.includes(
        Buffer.from("asset://main/maplibre-gl-layers/index.html"),
      ),
    ).toBe(true);
  });

  it("resolves validate-mode executor virtual modules from muon.json plugin imports", async () => {
    const root = await createTemporaryDirectory("muon-vite-config-capability-");
    await mkdir(join(root, "src", "native"), { recursive: true });
    await writeFile(
      join(root, "muon.json"),
      `${JSON.stringify(
        {
          plugin: {
            plugins: [
              {
                name: "internal",
                imports: [
                  {
                    sources: ["src/native/**"],
                    allow: ["muon.executor.spawn"],
                  },
                ],
              },
            ],
          },
        },
        null,
        2,
      )}\n`,
    );
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
        }),
      ],
    });

    await expect(
      access(join(root, "dist", "index.html")),
    ).resolves.toBeUndefined();
  });

  it("resolves validate-mode executor virtual modules from allowed packages", async () => {
    const root = await createTemporaryDirectory(
      "muon-vite-package-capability-",
    );
    const packageRoot = join(
      root,
      "node_modules",
      "@example",
      "trusted-muon-helper",
    );
    await mkdir(join(root, "src"), { recursive: true });
    await mkdir(packageRoot, { recursive: true });
    await writeFile(
      join(root, "muon.json"),
      `${JSON.stringify(
        {
          plugin: {
            plugins: [
              {
                name: "internal",
                imports: [
                  {
                    sources: ["src/not-used/**"],
                    packages: ["@example/trusted-muon-helper"],
                    allow: ["muon.executor.spawn"],
                  },
                ],
              },
            ],
          },
        },
        null,
        2,
      )}\n`,
    );
    await writeFile(
      join(root, "index.html"),
      '<script type="module" src="/src/main.ts"></script>',
    );
    await writeFile(
      join(root, "src", "main.ts"),
      'import { runNode } from "@example/trusted-muon-helper";\nvoid runNode;\n',
    );
    await writeFile(
      join(packageRoot, "package.json"),
      `${JSON.stringify(
        {
          name: "@example/trusted-muon-helper",
          type: "module",
          main: "./index.js",
        },
        null,
        2,
      )}\n`,
    );
    await writeFile(
      join(packageRoot, "index.js"),
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
        }),
      ],
    });

    await expect(
      access(join(root, "dist", "index.html")),
    ).resolves.toBeUndefined();
  });

  it("generates runtime plugin allow from validate-mode import rules", async () => {
    const root = await createTemporaryDirectory("muon-vite-runtime-allow-");
    const muonDirectory = await createTemporaryDirectory("muon-vite-muon-");
    const outputDirectory = await createTemporaryDirectory("muon-vite-output-");
    const cefDirectory = await writeFakeCefDirectory();
    await writeBasicViteProject(root);
    await writeFile(
      join(root, "muon.json"),
      `${JSON.stringify(
        {
          network: { allow: ["asset://main/**"] },
          plugin: {
            plugins: [
              {
                name: "internal",
                imports: [
                  {
                    sources: ["src/native/**"],
                    allow: ["muon.executor.spawn"],
                  },
                  {
                    sources: ["src/browser/**"],
                    allow: ["muon.executor.spawn", "muon.browser.reload"],
                  },
                ],
              },
            ],
          },
        },
        null,
        2,
      )}\n`,
    );
    await writeFakeMuonSource(muonDirectory, outputDirectory);
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
    try {
      await wait(() => existsSync(join(outputDirectory, "override.json")));
      const overrideConfig = JSON.parse(
        await readFile(join(outputDirectory, "override.json"), "utf8"),
      ) as {
        plugin: {
          capabilities: { allow: string[] }[];
          plugins: { name: string; allow: string[] }[];
        };
      };
      expect(overrideConfig.plugin.plugins).toEqual([
        {
          name: "internal",
          allow: ["muon.executor.spawn", "muon.browser.reload"],
        },
      ]);
      expect(
        overrideConfig.plugin.capabilities.map((entry) => entry.allow),
      ).toEqual([
        ["muon.executor.spawn"],
        ["muon.executor.spawn", "muon.browser.reload"],
      ]);
    } finally {
      await server.close();
    }
  });

  it("generates opaque capability ids shared by runtime config and virtual modules", async () => {
    const root = await createTemporaryDirectory("muon-vite-capability-id-");
    await mkdir(join(root, "src", "native"), { recursive: true });
    await mkdir(join(root, "src", "browser"), { recursive: true });

    const resolver = createMuonCapabilityModuleResolver(root, {
      imports: [
        {
          sources: ["src/native/**"],
          allow: ["muon.executor.spawn"],
          pluginName: "internal",
        },
        {
          sources: ["src/browser/**"],
          allow: ["muon.browser.reload"],
          pluginName: "internal",
        },
      ],
    });

    const runtimeConfig = resolver.getRuntimePluginConfig();
    expect(runtimeConfig.capabilities.map((entry) => entry.allow)).toEqual([
      ["muon.executor.spawn"],
      ["muon.browser.reload"],
    ]);
    const capabilityIds = runtimeConfig.capabilities.map((entry) => entry.id);
    expect(capabilityIds).toHaveLength(new Set(capabilityIds).size);
    for (const capabilityId of capabilityIds) {
      expect(capabilityId).toMatch(/^cap-[0-9a-f]{32}$/u);
    }

    const resolved = resolver.resolveId(
      "muon:executor",
      join(root, "src", "native", "executor.ts"),
    );
    expect(resolved).toBeDefined();
    const moduleSource =
      resolved === undefined ? undefined : resolver.load(resolved.id);
    expect(moduleSource).toContain(
      `__muonCall(${JSON.stringify(capabilityIds[0])}, "muon.executor.spawn"`,
    );
    expect(moduleSource).toContain("export const spawn = async (options = {})");
    expect(moduleSource).toContain("writeStdin");
    expect(moduleSource).toContain("closeStdin");
    expect(moduleSource).toContain("wait");
    expect(moduleSource).toContain("kill");
    expect(moduleSource).toContain("dispose");
  });

  it("generates executor loadLibrary virtual module wrappers", async () => {
    const root = await createTemporaryDirectory("muon-vite-adhoc-library-");
    await mkdir(join(root, "src", "native"), { recursive: true });

    const resolver = createMuonCapabilityModuleResolver(root, {
      imports: [
        {
          sources: ["src/native/**"],
          allow: ["muon.executor.loadLibrary"],
          pluginName: "internal",
        },
      ],
    });

    const runtimeConfig = resolver.getRuntimePluginConfig();
    const capabilityId = runtimeConfig.capabilities[0]?.id;
    if (capabilityId === undefined) {
      throw new Error("capability id was not generated");
    }

    const resolved = resolver.resolveId(
      "muon:executor",
      join(root, "src", "native", "library.ts"),
    );
    expect(resolved).toBeDefined();
    const moduleSource =
      resolved === undefined ? undefined : resolver.load(resolved.id);
    expect(moduleSource).toContain(
      `__muonCall(${JSON.stringify(capabilityId)}, "muon.executor.loadLibrary"`,
    );
    expect(moduleSource).toContain("export const loadLibrary = async (path)");
    expect(moduleSource).toContain("export const pointer = ");
    expect(moduleSource).toContain("export const usize = ");
    expect(moduleSource).toContain('op: "getFunction"');
    expect(moduleSource).toContain('op: "call"');
    expect(moduleSource).toContain('op: "release"');
    expect(moduleSource).toContain('Symbol.for("muon.nativePointer")');
  });

  it("generates browser context menu virtual module wrappers", async () => {
    const root = await createTemporaryDirectory("muon-vite-browser-menu-");
    await mkdir(join(root, "src", "browser"), { recursive: true });

    const resolver = createMuonCapabilityModuleResolver(root, {
      imports: [
        {
          sources: ["src/browser/**"],
          allow: [
            "muon.browser.setContextMenuItems",
            "muon.browser.clearContextMenuItems",
          ],
          pluginName: "internal",
        },
      ],
    });

    const resolved = resolver.resolveId(
      "muon:browser",
      join(root, "src", "browser", "menu.ts"),
    );
    expect(resolved).toBeDefined();
    const moduleSource =
      resolved === undefined ? undefined : resolver.load(resolved.id);
    expect(moduleSource).toContain("export const setContextMenuItems = ");
    expect(moduleSource).toContain("export const clearContextMenuItems = ");
    expect(moduleSource).toContain("__muonBrowserContextMenuHandler");
    expect(moduleSource).toContain("muon.browser.setContextMenuItems");
    expect(moduleSource).toContain("muon.browser.clearContextMenuItems");
  });

  it("generates browser tray virtual module wrappers", async () => {
    const root = await createTemporaryDirectory("muon-vite-browser-tray-");
    await mkdir(join(root, "src", "browser"), { recursive: true });

    const resolver = createMuonCapabilityModuleResolver(root, {
      imports: [
        {
          sources: ["src/browser/**"],
          allow: [
            "muon.browser.createTray",
            "muon.browser.setTrayMenu",
            "muon.browser.setTrayIcon",
            "muon.browser.setTrayTooltip",
            "muon.browser.removeTray",
          ],
          pluginName: "internal",
        },
      ],
    });

    const resolved = resolver.resolveId(
      "muon:browser",
      join(root, "src", "browser", "tray.ts"),
    );
    expect(resolved).toBeDefined();
    const moduleSource =
      resolved === undefined ? undefined : resolver.load(resolved.id);
    expect(moduleSource).toContain("export const createTray = ");
    expect(moduleSource).toContain("export const setTrayMenu = ");
    expect(moduleSource).toContain("export const setTrayIcon = ");
    expect(moduleSource).toContain("export const setTrayTooltip = ");
    expect(moduleSource).toContain("export const removeTray = ");
    expect(moduleSource).toContain("__muonBrowserTrayHandlers");
    expect(moduleSource).toContain("muon-browser-tray-event");
    expect(moduleSource).toContain("muon.browser.createTray");
    expect(moduleSource).toContain("muon.browser.setTrayMenu");
  });

  it("preserves plugin signature in generated plugin runtime config", async () => {
    const root = await createTemporaryDirectory("muon-vite-plugin-signature-");
    const muonDirectory = await createTemporaryDirectory(
      "muon-vite-plugin-signature-runtime-",
    );
    const outputDirectory = await createTemporaryDirectory(
      "muon-vite-plugin-signature-output-",
    );
    const cefDirectory = await writeFakeCefDirectory();
    await writeBasicViteProject(root);
    await writeProjectMuonConfig(root);
    await writeFakeMuonSource(muonDirectory, outputDirectory);
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
        pluginAccess: {
          plugins: [
            {
              name: "foobar",
              signature: "A9993E364706816ABA3E25717850C26C9CD0D89D",
              salt: "deadbeef",
              imports: [
                {
                  sources: ["src/native/**"],
                  allow: ["foobar.run"],
                },
              ],
            },
          ],
        },
      },
      undefined,
    );
    try {
      await wait(() => existsSync(join(outputDirectory, "override.json")));
      const overrideConfig = JSON.parse(
        await readFile(join(outputDirectory, "override.json"), "utf8"),
      ) as {
        plugin: {
          plugins: {
            name: string;
            allow: string[];
            signature?: string;
            salt?: string;
          }[];
        };
      };
      expect(overrideConfig.plugin.plugins).toEqual([
        {
          name: "foobar",
          allow: ["foobar.run"],
          signature: "A9993E364706816ABA3E25717850C26C9CD0D89D",
          salt: "deadbeef",
        },
      ]);
    } finally {
      await server.close();
    }
  });

  it("rejects validate-mode plugin entry allow", async () => {
    const root = await createTemporaryDirectory("muon-vite-parent-allow-");
    await writeBasicViteProject(root);

    await expect(
      viteBuild({
        root,
        logLevel: "silent",
        plugins: [
          muon({
            open: false,
            build: false,
            pluginAccess: {
              plugins: [
                {
                  name: "internal",
                  allow: ["muon.executor.spawn"],
                  imports: [
                    {
                      sources: ["src/native/**"],
                      allow: ["muon.executor.spawn"],
                    },
                  ],
                },
              ],
            },
          }),
        ],
      }),
    ).rejects.toThrow(
      "plugin.plugins[0].allow is only supported in simple mode",
    );
  });

  it("rejects validate-mode plugin entries without import rules", async () => {
    const root = await createTemporaryDirectory("muon-vite-missing-imports-");
    await writeBasicViteProject(root);

    await expect(
      viteBuild({
        root,
        logLevel: "silent",
        plugins: [
          muon({
            open: false,
            build: false,
            pluginAccess: {
              plugins: [
                {
                  name: "internal",
                },
              ],
            },
          }),
        ],
      }),
    ).rejects.toThrow("plugin.plugins[0].imports is required in validate mode");
  });

  it("rejects validate-mode plugin entries with empty import rules", async () => {
    const root = await createTemporaryDirectory("muon-vite-empty-imports-");
    await writeBasicViteProject(root);

    await expect(
      viteBuild({
        root,
        logLevel: "silent",
        plugins: [
          muon({
            open: false,
            build: false,
            pluginAccess: {
              plugins: [
                {
                  name: "internal",
                  imports: [],
                },
              ],
            },
          }),
        ],
      }),
    ).rejects.toThrow("plugin.plugins[0].imports must not be empty");
  });

  it("rejects validate-mode imports without allow", async () => {
    const root = await createTemporaryDirectory("muon-vite-missing-allow-");
    await writeBasicViteProject(root);

    await expect(
      viteBuild({
        root,
        logLevel: "silent",
        plugins: [
          muon({
            open: false,
            build: false,
            pluginAccess: {
              plugins: [
                {
                  name: "internal",
                  imports: [
                    {
                      sources: ["src/native/**"],
                    },
                  ],
                },
              ],
            },
          }),
        ],
      }),
    ).rejects.toThrow(
      "plugin.plugins[0].imports[0].allow is required in validate mode",
    );
  });

  it("rejects validate-mode imports with empty allow", async () => {
    const root = await createTemporaryDirectory("muon-vite-empty-allow-");
    await writeBasicViteProject(root);

    await expect(
      viteBuild({
        root,
        logLevel: "silent",
        plugins: [
          muon({
            open: false,
            build: false,
            pluginAccess: {
              plugins: [
                {
                  name: "internal",
                  imports: [
                    {
                      sources: ["src/native/**"],
                      allow: [],
                    },
                  ],
                },
              ],
            },
          }),
        ],
      }),
    ).rejects.toThrow("plugin.plugins[0].imports[0].allow must not be empty");
  });

  it("rejects validate-mode imports without sources or packages", async () => {
    const root = await createTemporaryDirectory("muon-vite-empty-import-rule-");
    await writeBasicViteProject(root);

    await expect(
      viteBuild({
        root,
        logLevel: "silent",
        plugins: [
          muon({
            open: false,
            build: false,
            pluginAccess: {
              plugins: [
                {
                  name: "internal",
                  imports: [
                    {
                      allow: ["muon.executor.spawn"],
                    },
                  ],
                },
              ],
            },
          }),
        ],
      }),
    ).rejects.toThrow("requires sources or packages");
  });

  it("rejects simple-mode plugin entries without allow", async () => {
    const root = await createTemporaryDirectory("muon-vite-simple-no-allow-");
    await writeBasicViteProject(root);

    await expect(
      viteBuild({
        root,
        logLevel: "silent",
        plugins: [
          muon({
            open: false,
            build: false,
            pluginAccess: {
              mode: "simple",
              plugins: [
                {
                  name: "internal",
                },
              ],
            },
          }),
        ],
      }),
    ).rejects.toThrow("plugin.plugins[0].allow is required in simple mode");
  });

  it("rejects simple-mode plugin entries with imports", async () => {
    const root = await createTemporaryDirectory("muon-vite-simple-imports-");
    await writeBasicViteProject(root);

    await expect(
      viteBuild({
        root,
        logLevel: "silent",
        plugins: [
          muon({
            open: false,
            build: false,
            pluginAccess: {
              mode: "simple",
              plugins: [
                {
                  name: "internal",
                  allow: ["muon.executor.spawn"],
                  imports: [
                    {
                      sources: ["src/native/**"],
                      allow: ["muon.executor.spawn"],
                    },
                  ],
                },
              ],
            },
          }),
        ],
      }),
    ).rejects.toThrow(
      "plugin.plugins[0].imports is only supported in validate mode",
    );
  });

  it("rejects pluginAccess false when muon.json only has validate imports", async () => {
    const root = await createTemporaryDirectory("muon-vite-false-validate-");
    await writeBasicViteProject(root);
    await writeFile(
      join(root, "muon.json"),
      `${JSON.stringify(
        {
          plugin: {
            plugins: [
              {
                name: "internal",
                imports: [
                  {
                    sources: ["src/native/**"],
                    allow: ["muon.executor.spawn"],
                  },
                ],
              },
            ],
          },
        },
        null,
        2,
      )}\n`,
    );

    await expect(
      viteBuild({
        root,
        logLevel: "silent",
        plugins: [
          muon({
            open: false,
            build: false,
            pluginAccess: false,
          }),
        ],
      }),
    ).rejects.toThrow("plugin.plugins[0].allow is required in simple mode");
  });

  it("uses Vite pluginAccess plugins instead of muon.json plugin imports", async () => {
    const root = await createTemporaryDirectory(
      "muon-vite-override-capability-",
    );
    await mkdir(join(root, "src", "native"), { recursive: true });
    await writeFile(
      join(root, "muon.json"),
      `${JSON.stringify(
        {
          plugin: {
            plugins: [
              {
                name: "internal",
                imports: [
                  {
                    sources: ["src/blocked/**"],
                    allow: ["muon.executor.spawn"],
                  },
                ],
              },
            ],
          },
        },
        null,
        2,
      )}\n`,
    );
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
            plugins: [
              {
                name: "internal",
                imports: [
                  {
                    sources: ["src/native/**"],
                    allow: ["muon.executor.spawn"],
                  },
                ],
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
              plugins: [
                {
                  name: "internal",
                  imports: [
                    {
                      sources: ["src/native/**"],
                      allow: ["muon.executor.spawn"],
                    },
                  ],
                },
              ],
            },
          }),
        ],
      }),
    ).rejects.toThrow("muon capability import is not allowed");
  });

  it("launches muon without server.open", async () => {
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
      };
      plugin: {
        pages: string[];
        capabilities: unknown[];
        mode: string;
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
      },
      plugin: {
        pages: [`${new URL(baseUrl ?? "").origin}/**`],
        capabilities: [],
        mode: "validate",
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

  it("refreshes the generated Vite override when muon requests recycle", async () => {
    const root = await createTemporaryDirectory("muon-vite-recycle-refresh-");
    const muonDirectory = await createTemporaryDirectory("muon-vite-muon-");
    const outputDirectory = await createTemporaryDirectory("muon-vite-output-");
    const cefDirectory = await writeFakeCefDirectory();
    await writeBasicViteProject(root);
    await mkdir(join(root, "src"), { recursive: true });
    await writeValidatePluginMuonConfig(root, "muon.browser.reload");
    await writeFakeControlledRecyclingMuonSource(
      muonDirectory,
      outputDirectory,
    );
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
    await wait(() => existsSync(join(outputDirectory, "ready-1")));

    const firstOverride = JSON.parse(
      await readFile(join(outputDirectory, "override-1.json"), "utf8"),
    ) as {
      plugin: { plugins: { name: string; allow: string[] }[] };
    };
    expect(firstOverride.plugin.plugins).toEqual([
      {
        name: "internal",
        allow: ["muon.browser.reload"],
      },
    ]);

    await writeValidatePluginMuonConfig(root, "muon.browser.recycle");
    await writeFile(join(outputDirectory, "continue-1"), "");
    await wait(() => existsSync(join(outputDirectory, "override-2.json")));

    const secondOverride = JSON.parse(
      await readFile(join(outputDirectory, "override-2.json"), "utf8"),
    ) as {
      plugin: { plugins: { name: string; allow: string[] }[] };
    };
    expect(secondOverride.plugin.plugins).toEqual([
      {
        name: "internal",
        allow: ["muon.browser.recycle"],
      },
    ]);

    await server.close();
  });

  it("adds a newly created muon config path on Vite recycle", async () => {
    const root = await createTemporaryDirectory(
      "muon-vite-recycle-add-config-",
    );
    const muonDirectory = await createTemporaryDirectory("muon-vite-muon-");
    const outputDirectory = await createTemporaryDirectory("muon-vite-output-");
    const cefDirectory = await writeFakeCefDirectory();
    await writeBasicViteProject(root);
    await writeFakeControlledRecyclingMuonSource(
      muonDirectory,
      outputDirectory,
    );
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
    await wait(() => existsSync(join(outputDirectory, "ready-1")));

    const firstArgs = (
      await readFile(join(outputDirectory, "args-1.txt"), "utf8")
    )
      .trim()
      .split("\n");
    expect(firstArgs).toHaveLength(2);

    await writeProjectMuonConfig(root);
    await writeFile(join(outputDirectory, "continue-1"), "");
    await wait(() => existsSync(join(outputDirectory, "args-2.txt")));

    const secondArgs = (
      await readFile(join(outputDirectory, "args-2.txt"), "utf8")
    )
      .trim()
      .split("\n");
    expect(secondArgs).toHaveLength(4);
    expect(secondArgs.slice(0, 3)).toEqual([
      "-c",
      join(root, "muon.json"),
      "-c",
    ]);
    expect(secondArgs[3]).toContain("muon.vite.json");

    await server.close();
  });

  it("launches both the browser and muon when server.open is true", async () => {
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
        plugin: {
          capabilities?: unknown[];
          mode: string;
        };
      };
      expect(overrideConfig.plugin.mode).toBe("simple");
      expect(overrideConfig.plugin.capabilities).toBeUndefined();
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
    expect(warnings.join("\n")).toContain("muon project config was not found");
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

  it("uses the Vite base URL as the muon start page when server.open is a string", async () => {
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
  it("builds Vite assets before direct run when a muon plugin config is present", async () => {
    const root = await createTemporaryDirectory("muon-dev-vite-assets-");
    const muonDirectory = await createTemporaryDirectory("muon-dev-muon-");
    const outputDirectory = await createTemporaryDirectory("muon-dev-output-");
    const cefDirectory = await writeFakeCefDirectory();
    await writeRunViteApplication(root);
    await writeFakeMuonSource(muonDirectory, outputDirectory);
    await writeMuonViteConfig(
      root,
      [
        'import muon from "__MUON_VITE_URL__";',
        "export default {",
        '  base: "/nested/base/",',
        '  build: { outDir: "web-dist" },',
        "  plugins: [",
        `    muon({ muonPath: ${JSON.stringify(muonDirectory)}, cefPath: ${JSON.stringify(cefDirectory)} }),`,
        "  ],",
        "};",
      ].join("\n"),
    );
    process.env.MUON_CACHE_DIR =
      await createTemporaryDirectory("muon-dev-cache-");

    const result = await runMuonCli(root, ["run", "--json"]);

    expect(result.stderr).toBe("");
    expect(result.exitCode).toBe(0);
    const devResult = JSON.parse(result.stdout) as {
      assetSourcePath: string;
      overrideConfigPath: string;
    };
    const overrideConfig = JSON.parse(
      await readFile(join(outputDirectory, "override.json"), "utf8"),
    ) as {
      asset: { sourcePath: string };
      browser: { startPage: string };
    };

    await expect(
      access(join(root, "web-dist", "index.html")),
    ).resolves.toBeUndefined();
    await expect(
      access(
        join(
          root,
          ".muon",
          "run",
          "assets",
          "main",
          "nested",
          "base",
          "index.html",
        ),
      ),
    ).resolves.toBeUndefined();
    expect(devResult.assetSourcePath).toBe(
      join(root, ".muon", "run", "assets"),
    );
    expect(overrideConfig.asset.sourcePath).toBe(devResult.assetSourcePath);
    expect(overrideConfig.browser.startPage).toBe(
      "asset://main/nested/base/index.html",
    );
    expect(await readCapturedArguments(outputDirectory)).toEqual([
      "-c",
      devResult.overrideConfigPath,
    ]);
  });

  it("uses the Vite build config path for direct run unless the CLI overrides it", async () => {
    const root = await createTemporaryDirectory("muon-dev-vite-config-path-");
    const muonDirectory = await createTemporaryDirectory("muon-dev-muon-");
    const outputDirectory = await createTemporaryDirectory("muon-dev-output-");
    const cefDirectory = await writeFakeCefDirectory();
    const pluginConfigPath = join(root, "settings", "plugin-muon.json");
    const cliConfigPath = join(root, "settings", "cli-muon.json");
    await mkdir(dirname(pluginConfigPath), { recursive: true });
    await writeFile(
      pluginConfigPath,
      `${JSON.stringify({ network: { allow: ["asset://main/**"] } }, null, 2)}\n`,
    );
    await writeFile(
      cliConfigPath,
      `${JSON.stringify({ network: { allow: ["asset://cli/**"] } }, null, 2)}\n`,
    );
    await writeRunViteApplication(root);
    await writeFakeMuonSource(muonDirectory, outputDirectory);
    await writeMuonViteConfig(
      root,
      [
        'import muon from "__MUON_VITE_URL__";',
        "export default {",
        '  build: { outDir: "web-dist" },',
        "  plugins: [",
        `    muon({ muonPath: ${JSON.stringify(muonDirectory)}, cefPath: ${JSON.stringify(cefDirectory)}, build: { configPath: "settings/plugin-muon.json" } }),`,
        "  ],",
        "};",
      ].join("\n"),
    );
    process.env.MUON_CACHE_DIR =
      await createTemporaryDirectory("muon-dev-cache-");

    const pluginResult = await runMuonCli(root, ["run", "--json"]);
    expect(pluginResult.stderr).toBe("");
    expect(pluginResult.exitCode).toBe(0);
    const pluginDevResult = JSON.parse(pluginResult.stdout) as {
      overrideConfigPath: string;
      projectConfigPath: string;
    };
    expect(pluginDevResult.projectConfigPath).toBe(pluginConfigPath);
    expect(await readCapturedArguments(outputDirectory)).toEqual([
      "-c",
      pluginConfigPath,
      "-c",
      pluginDevResult.overrideConfigPath,
    ]);

    const cliResult = await runMuonCli(root, [
      "run",
      "--config",
      "settings/cli-muon.json",
      "--json",
    ]);
    expect(cliResult.stderr).toBe("");
    expect(cliResult.exitCode).toBe(0);
    const cliDevResult = JSON.parse(cliResult.stdout) as {
      overrideConfigPath: string;
      projectConfigPath: string;
    };
    expect(cliDevResult.projectConfigPath).toBe(cliConfigPath);
    expect(await readCapturedArguments(outputDirectory)).toEqual([
      "-c",
      cliConfigPath,
      "-c",
      cliDevResult.overrideConfigPath,
    ]);
  });

  it("keeps a configured browser start page for Vite-backed direct run", async () => {
    const root = await createTemporaryDirectory("muon-dev-vite-start-page-");
    const muonDirectory = await createTemporaryDirectory("muon-dev-muon-");
    const outputDirectory = await createTemporaryDirectory("muon-dev-output-");
    const cefDirectory = await writeFakeCefDirectory();
    await writeFile(
      join(root, "muon.json"),
      `${JSON.stringify({ browser: { startPage: "asset://main/custom.html" } }, null, 2)}\n`,
    );
    await writeRunViteApplication(root);
    await writeFakeMuonSource(muonDirectory, outputDirectory);
    await writeMuonViteConfig(
      root,
      [
        'import muon from "__MUON_VITE_URL__";',
        "export default {",
        '  base: "/nested/base/",',
        '  build: { outDir: "web-dist" },',
        "  plugins: [",
        `    muon({ muonPath: ${JSON.stringify(muonDirectory)}, cefPath: ${JSON.stringify(cefDirectory)} }),`,
        "  ],",
        "};",
      ].join("\n"),
    );
    process.env.MUON_CACHE_DIR =
      await createTemporaryDirectory("muon-dev-cache-");

    const result = await runMuonCli(root, ["run", "--json"]);

    expect(result.stderr).toBe("");
    expect(result.exitCode).toBe(0);
    const devResult = JSON.parse(result.stdout) as {
      overrideConfigPath: string;
    };
    const overrideConfig = JSON.parse(
      await readFile(join(outputDirectory, "override.json"), "utf8"),
    ) as {
      browser: { startPage?: string };
    };

    expect(overrideConfig.browser.startPage).toBeUndefined();
    expect(await readCapturedArguments(outputDirectory)).toEqual([
      "-c",
      join(root, "muon.json"),
      "-c",
      devResult.overrideConfigPath,
    ]);
  });

  it("keeps Vite pluginAccess capability ids consistent with direct run config", async () => {
    const root = await createTemporaryDirectory("muon-dev-vite-capability-");
    const muonDirectory = await createTemporaryDirectory("muon-dev-muon-");
    const outputDirectory = await createTemporaryDirectory("muon-dev-output-");
    const cefDirectory = await writeFakeCefDirectory();
    await mkdir(join(root, "src", "native"), { recursive: true });
    await writeFile(
      join(root, "index.html"),
      '<script type="module" src="/src/main.ts"></script>',
    );
    await writeFile(
      join(root, "src", "main.ts"),
      'import { runNative } from "./native/executor";\nvoid runNative();\n',
    );
    await writeFile(
      join(root, "src", "native", "executor.ts"),
      [
        'import { spawn } from "muon:executor";',
        "export const runNative = async () =>",
        "  await spawn({ command: 'node', args: ['script.js'] });",
        "",
      ].join("\n"),
    );
    await writeFakeMuonSource(muonDirectory, outputDirectory);
    await writeMuonViteConfig(
      root,
      [
        'import muon from "__MUON_VITE_URL__";',
        "export default {",
        '  build: { outDir: "web-dist" },',
        "  plugins: [",
        "    muon({",
        `      muonPath: ${JSON.stringify(muonDirectory)},`,
        `      cefPath: ${JSON.stringify(cefDirectory)},`,
        "      pluginAccess: {",
        "        plugins: [",
        "          {",
        '            name: "internal",',
        "            imports: [",
        "              {",
        '                sources: ["src/native/**"],',
        '                allow: ["muon.executor.spawn"],',
        "              },",
        "            ],",
        "          },",
        "        ],",
        "      },",
        "    }),",
        "  ],",
        "};",
      ].join("\n"),
    );
    process.env.MUON_CACHE_DIR =
      await createTemporaryDirectory("muon-dev-cache-");

    const result = await runMuonCli(root, ["run", "--json"]);

    expect(result.stderr).toBe("");
    expect(result.exitCode).toBe(0);
    const overrideConfig = JSON.parse(
      await readFile(join(outputDirectory, "override.json"), "utf8"),
    ) as {
      plugin: {
        capabilities: { id: string; allow: string[] }[];
        plugins: { name: string; allow: string[] }[];
      };
    };
    const capabilityId = overrideConfig.plugin.capabilities[0]?.id;
    if (capabilityId === undefined) {
      throw new Error("capability id was not generated");
    }

    expect(overrideConfig.plugin.plugins).toEqual([
      {
        name: "internal",
        allow: ["muon.executor.spawn"],
      },
    ]);
    expect(await readRunBuiltJavaScript(root)).toContain(capabilityId);
  });

  it("regenerates Vite-backed direct run override when muon requests recycle", async () => {
    const root = await createTemporaryDirectory("muon-dev-vite-recycle-");
    const muonDirectory = await createTemporaryDirectory("muon-dev-muon-");
    const outputDirectory = await createTemporaryDirectory("muon-dev-output-");
    const cefDirectory = await writeFakeCefDirectory();
    await writeRunViteApplication(root);
    await writeValidatePluginMuonConfig(root, "muon.browser.reload");
    await writeFakeConfigUpdatingRecyclingMuonSource(
      muonDirectory,
      outputDirectory,
      join(root, "muon.json"),
      "muon.browser.recycle",
    );
    await writeMuonViteConfig(
      root,
      [
        'import muon from "__MUON_VITE_URL__";',
        "export default {",
        '  build: { outDir: "web-dist" },',
        "  plugins: [",
        `    muon({ muonPath: ${JSON.stringify(muonDirectory)}, cefPath: ${JSON.stringify(cefDirectory)} }),`,
        "  ],",
        "};",
      ].join("\n"),
    );
    process.env.MUON_CACHE_DIR =
      await createTemporaryDirectory("muon-dev-cache-");

    const result = await runMuonCli(root, ["run", "--json"]);

    expect(result.stderr).toBe("");
    expect(result.exitCode).toBe(0);
    const devResult = JSON.parse(result.stdout) as {
      exitCode: number;
      projectConfigPath: string;
    };
    expect(devResult.exitCode).toBe(0);
    expect(devResult.projectConfigPath).toBe(join(root, "muon.json"));
    await expect(
      readFile(join(outputDirectory, "recycle-count.txt"), "utf8"),
    ).resolves.toBe("2\n");

    const firstOverride = JSON.parse(
      await readFile(join(outputDirectory, "override-1.json"), "utf8"),
    ) as {
      plugin: { plugins: { name: string; allow: string[] }[] };
    };
    const secondOverride = JSON.parse(
      await readFile(join(outputDirectory, "override-2.json"), "utf8"),
    ) as {
      plugin: { plugins: { name: string; allow: string[] }[] };
    };
    expect(firstOverride.plugin.plugins).toEqual([
      {
        name: "internal",
        allow: ["muon.browser.reload"],
      },
    ]);
    expect(secondOverride.plugin.plugins).toEqual([
      {
        name: "internal",
        allow: ["muon.browser.recycle"],
      },
    ]);
  });

  it("rejects Vite-backed direct run when muon build is disabled", async () => {
    const root = await createTemporaryDirectory("muon-dev-vite-disabled-");
    const muonDirectory = await createTemporaryDirectory("muon-dev-muon-");
    const cefDirectory = await writeFakeCefDirectory();
    await writeRunViteApplication(root);
    await writeMuonViteConfig(
      root,
      [
        'import muon from "__MUON_VITE_URL__";',
        "export default {",
        "  plugins: [",
        `    muon({ muonPath: ${JSON.stringify(muonDirectory)}, cefPath: ${JSON.stringify(cefDirectory)}, build: false }),`,
        "  ],",
        "};",
      ].join("\n"),
    );

    const result = await runMuonCli(root, ["run", "--json"]);

    expect(result.exitCode).toBe(1);
    expect(result.stderr).toContain(
      "muon build is disabled by muon({ build: false })",
    );
  });

  it("launches muon directly without a Vite config", async () => {
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

  it("restarts muon when the direct run process requests recycle", async () => {
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

  it("restarts muon from the muon-core dev supervisor", async () => {
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
        `    muon({ muonPath: ${JSON.stringify(muonDirectory)}, cefPath: ${JSON.stringify(cefDirectory)}, stagePath: "custom-stage", enableDebugger: false, open: false, build: false }),`,
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
    expect(result.stderr).toContain("muon run asset source does not exist");
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

  it("restarts from the POSIX script when muon requests recycle", async () => {
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
      muonExecutablePath: "C:\\muon Runtime\\muon-core.exe",
      projectConfigPath: "C:\\My App\\muon.json",
      overrideConfigPath: "C:\\Temp Dir\\muon.vite.json",
      platform: "win32",
    });

    expect(script).toContain(
      'set "MUON_EXECUTABLE=C:\\muon Runtime\\muon-core.exe"',
    );
    expect(script).toContain(
      'set "MUON_EXECUTABLE_DIRECTORY=C:\\muon Runtime"',
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
