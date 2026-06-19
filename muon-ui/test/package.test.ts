// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import { execFile } from "node:child_process";
import {
  access,
  chmod,
  copyFile,
  mkdir,
  mkdtemp,
  readFile,
  rm,
  stat,
  symlink,
  writeFile,
} from "node:fs/promises";
import { createRequire } from "node:module";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { promisify } from "node:util";

import { afterEach, describe, expect, it } from "vitest";

const execFileAsync = promisify(execFile);
const require = createRequire(import.meta.url);
const tscPath = require.resolve("typescript/bin/tsc");
const cleanupDirectories: string[] = [];

interface NpmPackEntry {
  files: {
    path: string;
  }[];
}

const createConsumerProject = async (): Promise<string> => {
  const root = await mkdtemp(join(tmpdir(), "muon-consumer-"));
  cleanupDirectories.push(root);
  await mkdir(join(root, "node_modules"), { recursive: true });
  await symlink(resolve("."), join(root, "node_modules", "muon-ui"), "dir");
  await writeFile(
    join(root, "package.json"),
    `${JSON.stringify({ name: "muon-consumer", type: "module" }, null, 2)}\n`,
  );
  return root;
};

const runTypeScriptConsumer = async (source: string): Promise<void> => {
  const root = await createConsumerProject();
  await mkdir(join(root, "src"), { recursive: true });
  await writeFile(join(root, "src", "main.ts"), source);
  await writeFile(
    join(root, "tsconfig.json"),
    `${JSON.stringify(
      {
        compilerOptions: {
          target: "ES2022",
          module: "ESNext",
          moduleResolution: "Bundler",
          lib: ["ES2022", "DOM"],
          types: [],
          strict: true,
          noEmit: true,
        },
        include: ["src/**/*.ts"],
      },
      null,
      2,
    )}\n`,
  );
  await execFileAsync(
    process.execPath,
    [tscPath, "--project", "tsconfig.json"],
    {
      cwd: root,
    },
  );
};

const runJavaScriptConsumer = async (
  source: string,
  extension: "cjs" | "mjs",
): Promise<void> => {
  const root = await createConsumerProject();
  const scriptPath = join(root, `main.${extension}`);
  await writeFile(scriptPath, source);
  await execFileAsync(process.execPath, [scriptPath], { cwd: root });
};

const exists = async (path: string): Promise<boolean> => {
  try {
    await access(path);
    return true;
  } catch {
    return false;
  }
};

const getDryRunPackageFiles = async (): Promise<string[]> => {
  const { stdout } = await execFileAsync(
    "npm",
    ["pack", "--dry-run", "--ignore-scripts", "--json"],
    {
      cwd: resolve("."),
      encoding: "utf8",
    },
  );
  const [pack] = JSON.parse(stdout) as [NpmPackEntry];
  return pack.files.map((file) => file.path).sort();
};

const writeExecutableScript = async (
  path: string,
  source: string,
): Promise<void> => {
  await writeFile(path, source);
  await chmod(path, 0o755);
};

const createFakePackageBuildRoot = async (): Promise<string> => {
  const root = await mkdtemp(join(tmpdir(), "muon-package-build-"));
  cleanupDirectories.push(root);
  await copyFile(
    resolve("..", "build_package.sh"),
    join(root, "build_package.sh"),
  );
  await mkdir(join(root, "deps", "tra-ffic", "include"), { recursive: true });
  await mkdir(join(root, "deps", "cardio", "include"), { recursive: true });
  await mkdir(join(root, "muon-prepare"), { recursive: true });
  await mkdir(join(root, "muon-core"), { recursive: true });
  await mkdir(join(root, "muon-ui", "scripts"), { recursive: true });
  await writeFile(join(root, "deps", "tra-ffic", "include", "tra_ffic.h"), "");
  await writeFile(join(root, "deps", "cardio", "include", "cardio.h"), "");
  await writeFile(
    join(root, "muon-prepare", "package.json"),
    `${JSON.stringify({ version: "0.0.0" }, null, 2)}\n`,
  );
  await writeFile(
    join(root, "muon-core", "package.json"),
    `${JSON.stringify({ version: "0.0.0" }, null, 2)}\n`,
  );
  await writeFile(
    join(root, "muon-ui", "scripts", "stage-muon-prepare.mjs"),
    "process.exit(0);\n",
  );
  await writeExecutableScript(
    join(root, "muon-core", "build_yyjson.sh"),
    "#!/usr/bin/env bash\nset -euo pipefail\n",
  );
  await writeExecutableScript(
    join(root, "muon-core", "build_miniz.sh"),
    "#!/usr/bin/env bash\nset -euo pipefail\n",
  );
  return root;
};

afterEach(async () => {
  for (const directory of cleanupDirectories.splice(0)) {
    await rm(directory, { recursive: true, force: true });
  }
});

describe("muon-ui package root export", () => {
  it("uses screw-up package generation through the shared package builder", async () => {
    const rootPackageJson = JSON.parse(
      await readFile(resolve("..", "package.json"), "utf8"),
    ) as { scripts?: Record<string, string> };
    const packageJson = JSON.parse(
      await readFile(resolve("package.json"), "utf8"),
    ) as { name?: string; private?: boolean; scripts?: Record<string, string> };

    expect(packageJson.name).toBe("muon-ui");
    expect(packageJson.private).toBeUndefined();
    expect(rootPackageJson.scripts?.pack).toBe("bash ./build_package.sh");
    expect(packageJson.scripts?.pack).toBe("bash ../build_package.sh");
    expect(packageJson.scripts).not.toHaveProperty("prepack");

    const rootHelp = await execFileAsync(
      "npm",
      ["run", "pack", "--", "--help"],
      {
        cwd: resolve(".."),
        encoding: "utf8",
      },
    );
    const workspaceHelp = await execFileAsync(
      "npm",
      ["run", "pack", "--workspace", "muon-ui", "--", "--help"],
      {
        cwd: resolve(".."),
        encoding: "utf8",
      },
    );

    expect(rootHelp.stdout).toContain("Usage: ./build_package.sh");
    expect(workspaceHelp.stdout).toContain("Usage: ./build_package.sh");
  });

  it("uses vendored native dependency checkouts for package builds by default", async () => {
    const root = await createFakePackageBuildRoot();
    const binDirectory = join(root, "bin");
    const containerLog = join(root, "container.log");
    const npmLog = join(root, "npm.log");
    await mkdir(binDirectory, { recursive: true });
    await writeExecutableScript(
      join(binDirectory, "fake-container"),
      `#!/usr/bin/env bash
set -euo pipefail
if [[ "\${1:-}" == "image" && "\${2:-}" == "exists" ]]; then
  exit 0
fi
if [[ "\${1:-}" == "run" ]]; then
  printf '%s\\n' "$@" >> "\${MUON_FAKE_CONTAINER_LOG:?}"
  exit 0
fi
printf 'Unexpected fake container invocation: %s\\n' "$*" >&2
exit 1
`,
    );
    await writeExecutableScript(
      join(binDirectory, "npm"),
      `#!/usr/bin/env bash
set -euo pipefail
printf '%s\\n' "$*" >> "\${MUON_FAKE_NPM_LOG:?}"
`,
    );
    await writeExecutableScript(
      join(binDirectory, "readelf"),
      `#!/usr/bin/env bash
set -euo pipefail
printf '  Class:                             ELF64\\n'
printf '  Machine:                           Advanced Micro Devices X86-64\\n'
`,
    );

    await execFileAsync(
      "bash",
      [
        "build_package.sh",
        "--target",
        "dist",
        "--arch",
        "amd64",
        "--jobs",
        "1",
      ],
      {
        cwd: root,
        encoding: "utf8",
        env: {
          ...process.env,
          PATH: `${binDirectory}:${process.env.PATH ?? ""}`,
          CONTAINER_ENGINE: join(binDirectory, "fake-container"),
          MUON_FAKE_CONTAINER_LOG: containerLog,
          MUON_FAKE_NPM_LOG: npmLog,
        },
      },
    );

    const containerInvocation = await readFile(containerLog, "utf8");
    expect(containerInvocation).toContain(
      `${join(root, "deps", "tra-ffic")}:/workspace-deps/tra-ffic:ro,Z`,
    );
    expect(containerInvocation).toContain(
      `${join(root, "deps", "cardio")}:/workspace-deps/cardio:ro,Z`,
    );
  });

  it("reports the failed Linux package target and log path", async () => {
    const root = await createFakePackageBuildRoot();
    const binDirectory = join(root, "bin");
    await mkdir(binDirectory, { recursive: true });
    await writeExecutableScript(
      join(binDirectory, "fake-container"),
      `#!/usr/bin/env bash
set -euo pipefail
if [[ "\${1:-}" == "image" && "\${2:-}" == "exists" ]]; then
  exit 0
fi
if [[ "\${1:-}" == "run" ]]; then
  if [[ " $* " == *"MUON_PACKAGE_ARCH=arm64"* ]]; then
    printf 'fake arm64 failure\\n' >&2
    exit 42
  fi
  printf 'fake container success\\n'
  exit 0
fi
printf 'Unexpected fake container invocation: %s\\n' "$*" >&2
exit 1
`,
    );

    let failure: unknown;
    try {
      await execFileAsync(
        "bash",
        [
          "build_package.sh",
          "--target",
          "dist",
          "--arch",
          "amd64,arm64",
          "--jobs",
          "2",
        ],
        {
          cwd: root,
          encoding: "utf8",
          env: {
            ...process.env,
            PATH: `${binDirectory}:${process.env.PATH ?? ""}`,
            CONTAINER_ENGINE: join(binDirectory, "fake-container"),
          },
        },
      );
    } catch (error) {
      failure = error;
    }

    expect(failure).toBeInstanceOf(Error);
    const execError = failure as Error & {
      code?: number;
      stderr?: string;
      stdout?: string;
    };
    const output = `${execError.stdout ?? ""}\n${execError.stderr ?? ""}`;
    const logPath = join(root, ".deps", "package-logs", "linuxarm64.log");
    expect(execError.code).toBe(1);
    expect(output).toContain("Linux package target failed: linuxarm64");
    expect(output).toContain("exit code 42");
    expect(output).toContain(logPath);
    await expect(readFile(logPath, "utf8")).resolves.toContain(
      "fake arm64 failure",
    );
  });

  it("publishes the muon CLI bin", async () => {
    const packageJson = JSON.parse(
      await readFile(resolve("package.json"), "utf8"),
    ) as { bin?: Record<string, string> };
    const cliStat = await stat(resolve("dist/cli.cjs"));
    const nativePrepareStat = await stat(
      resolve("dist/native/linux64/muon-prepare"),
    );
    const nativeBootstrapStat = await stat(
      resolve("dist/native/linux64/muon-bootstrap"),
    );

    expect(packageJson.bin?.muon).toBe("./dist/cli.cjs");
    expect(cliStat.mode & 0o111).not.toBe(0);
    expect(nativePrepareStat.mode & 0o111).not.toBe(0);
    expect(nativeBootstrapStat.mode & 0o111).not.toBe(0);
    await expect(exists(resolve("dist/cli.mjs"))).resolves.toBe(false);
    await expect(exists(resolve("dist/vite.d.ts"))).resolves.toBe(false);
    await expect(exists(resolve("dist/muon-prepare"))).resolves.toBe(false);
    await expect(exists(resolve("dist/muon-bootstrap"))).resolves.toBe(false);
  });

  it("shows help from the muon CLI bin", async () => {
    const { stdout, stderr } = await execFileAsync(
      process.execPath,
      [resolve("dist/cli.cjs"), "--help"],
      { encoding: "utf8" },
    );

    expect(stderr).toBe("");
    expect(stdout).toContain("Usage: muon [options] [command]");
    expect(stdout).toContain("build");
    expect(stdout).toContain("init");
    expect(stdout).toContain("prepare");
    expect(stdout).toContain("embed-config");
  });

  it("adds the Muon staging directory through the muon CLI init command", async () => {
    const root = await mkdtemp(join(tmpdir(), "muon-init-"));
    cleanupDirectories.push(root);
    const cliPath = resolve("dist", "cli.cjs");
    const { stdout, stderr } = await execFileAsync(
      process.execPath,
      [cliPath, "init"],
      {
        cwd: root,
        encoding: "utf8",
      },
    );

    expect(stderr).toBe("");
    expect(stdout).toContain(".gitignore");
    await expect(readFile(join(root, ".gitignore"), "utf8")).resolves.toBe(
      ".muon/\n",
    );
  });

  it("keeps an existing Muon gitignore entry when the muon CLI init command is repeated", async () => {
    const root = await mkdtemp(join(tmpdir(), "muon-init-existing-"));
    cleanupDirectories.push(root);
    await writeFile(join(root, ".gitignore"), "dist/\n.muon/\n");
    const cliPath = resolve("dist", "cli.cjs");

    await execFileAsync(process.execPath, [cliPath, "init"], {
      cwd: root,
      encoding: "utf8",
    });

    await expect(readFile(join(root, ".gitignore"), "utf8")).resolves.toBe(
      "dist/\n.muon/\n",
    );
  });

  it("publishes only public TypeScript declaration files", async () => {
    const files = await getDryRunPackageFiles();

    expect(files).toContain("muon.d.ts");
    expect(files).toContain("vite.d.ts");
    expect(files).not.toContain("dist/cli.mjs");
    expect(files).not.toContain("dist/cli.d.ts");
    expect(files).not.toContain("dist/index.d.ts");
    expect(files).not.toContain("dist/prepare.d.ts");
    expect(files).not.toContain("dist/vite-internals.d.ts");
    expect(files).not.toContain("dist/vite.d.ts");
    expect(files).not.toContain("dist/runtime/linux64/muon-runtime.json");
    expect(files.some((file) => file.endsWith(".d.ts.map"))).toBe(false);
    expect(files).not.toContain("dist/muon-prepare");
    expect(files).not.toContain("dist/muon-bootstrap");
    await expect(readFile(resolve("vite.d.ts"), "utf8")).resolves.not.toContain(
      "sourceMappingURL",
    );
  });

  it("provides Muon globals through root TypeScript imports", async () => {
    await expect(
      runTypeScriptConsumer(`import "muon-ui";

const reloadResult: Promise<void> = window.muon.browser.reload();
const resetZoomResult: Promise<void> = window.muon.browser.resetZoom();
const existsResult: Promise<boolean> = window.muon.fs.exists("asset://main/file");
void reloadResult;
void resetZoomResult;
void existsResult;
`),
    ).resolves.toBeUndefined();
    await expect(
      runTypeScriptConsumer(`import type {} from "muon-ui";

const reloadResult: Promise<void> = window.muon.browser.reload();
const resetZoomResult: Promise<void> = window.muon.browser.resetZoom();
const existsResult: Promise<boolean> = window.muon.fs.exists("asset://main/file");
void reloadResult;
void resetZoomResult;
void existsResult;
`),
    ).resolves.toBeUndefined();
  });

  it("provides the Vite plugin with separated muon and CEF paths", async () => {
    await expect(
      runTypeScriptConsumer(`import muon from "muon-ui/vite";

const defaultPlugin = muon();
const plugin = muon({
  muonPath: "../muon-core/.run/dev-linux64-debug",
  cefPath: "../muon-core/.cef/cef_binary_fake_linux64_minimal",
  enableDebugger: false,
  build: {
    targets: ["linux-amd64"],
  },
});
void defaultPlugin;
void plugin;
`),
    ).resolves.toBeUndefined();
  });

  it("resolves the root package import at runtime", async () => {
    await expect(
      runJavaScriptConsumer(`import "muon-ui";\n`, "mjs"),
    ).resolves.toBeUndefined();
    await expect(
      runJavaScriptConsumer(`require("muon-ui");\n`, "cjs"),
    ).resolves.toBeUndefined();
  });
});
