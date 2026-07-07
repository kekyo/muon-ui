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

import * as ts from "typescript";
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

interface PublicDeclarationDefaultValueTarget {
  filePath: string;
  parentName: string;
  memberName: string;
}

interface PublicDeclarationCallableDefaultValueTarget {
  filePath: string;
  callableName: string;
}

const publicDeclarationDefaultValueTargets: PublicDeclarationDefaultValueTarget[] =
  [
    {
      filePath: "muon.d.ts",
      parentName: "MuonBootstrapSettings",
      memberName: "cefVersionPolicy",
    },
    {
      filePath: "muon.d.ts",
      parentName: "MuonBootstrapSettings",
      memberName: "cefExactVersion",
    },
    {
      filePath: "muon.d.ts",
      parentName: "MuonBootstrapSettings",
      memberName: "catalogRefreshIntervalSeconds",
    },
    {
      filePath: "muon.d.ts",
      parentName: "MuonBootstrapSettingsPatch",
      memberName: "cefVersionPolicy",
    },
    {
      filePath: "muon.d.ts",
      parentName: "MuonBootstrapSettingsPatch",
      memberName: "cefExactVersion",
    },
    {
      filePath: "muon.d.ts",
      parentName: "MuonBootstrapSettingsPatch",
      memberName: "catalogRefreshIntervalSeconds",
    },
    {
      filePath: "muon.d.ts",
      parentName: "MuonFsOperationOptions",
      memberName: "signal",
    },
    {
      filePath: "muon.d.ts",
      parentName: "MuonFsReadFileOptions",
      memberName: "position",
    },
    {
      filePath: "muon.d.ts",
      parentName: "MuonFsReadFileOptions",
      memberName: "length",
    },
    {
      filePath: "muon.d.ts",
      parentName: "MuonFsWriteFileOptions",
      memberName: "position",
    },
    {
      filePath: "muon.d.ts",
      parentName: "MuonFsAccessOptions",
      memberName: "mode",
    },
    {
      filePath: "muon.d.ts",
      parentName: "MuonFsReadDirectoryOptions",
      memberName: "withFileTypes",
    },
    {
      filePath: "muon.d.ts",
      parentName: "MuonFsMakeDirectoryOptions",
      memberName: "recursive",
    },
    {
      filePath: "muon.d.ts",
      parentName: "MuonFsRemoveOptions",
      memberName: "recursive",
    },
    {
      filePath: "muon.d.ts",
      parentName: "MuonFsRemoveOptions",
      memberName: "force",
    },
    {
      filePath: "muon.d.ts",
      parentName: "MuonFsCopyFileOptions",
      memberName: "overwrite",
    },
    {
      filePath: "muon.d.ts",
      parentName: "MuonFsGtkDialogOptions",
      memberName: "localOnly",
    },
    {
      filePath: "muon.d.ts",
      parentName: "MuonFsGtkDialogOptions",
      memberName: "createFolders",
    },
    {
      filePath: "muon.d.ts",
      parentName: "MuonFsGtkDialogOptions",
      memberName: "mimeTypes",
    },
    {
      filePath: "muon.d.ts",
      parentName: "MuonFsWin32DialogOptions",
      memberName: "forceFilesystem",
    },
    {
      filePath: "muon.d.ts",
      parentName: "MuonFsWin32DialogOptions",
      memberName: "noDereferenceLinks",
    },
    {
      filePath: "muon.d.ts",
      parentName: "MuonFsWin32DialogOptions",
      memberName: "dontAddToRecent",
    },
    {
      filePath: "muon.d.ts",
      parentName: "MuonFsWin32DialogOptions",
      memberName: "noValidate",
    },
    {
      filePath: "muon.d.ts",
      parentName: "MuonFsWin32DialogOptions",
      memberName: "strictFileTypes",
    },
    {
      filePath: "muon.d.ts",
      parentName: "MuonFsWin32DialogOptions",
      memberName: "pathMustExist",
    },
    {
      filePath: "muon.d.ts",
      parentName: "MuonFsWin32DialogOptions",
      memberName: "fileMustExist",
    },
    {
      filePath: "muon.d.ts",
      parentName: "MuonFsDialogOptions",
      memberName: "title",
    },
    {
      filePath: "muon.d.ts",
      parentName: "MuonFsDialogOptions",
      memberName: "defaultPath",
    },
    {
      filePath: "muon.d.ts",
      parentName: "MuonFsDialogOptions",
      memberName: "buttonLabel",
    },
    {
      filePath: "muon.d.ts",
      parentName: "MuonFsDialogOptions",
      memberName: "modal",
    },
    {
      filePath: "muon.d.ts",
      parentName: "MuonFsDialogOptions",
      memberName: "showHidden",
    },
    {
      filePath: "muon.d.ts",
      parentName: "MuonFsDialogOptions",
      memberName: "filters",
    },
    {
      filePath: "muon.d.ts",
      parentName: "MuonFsDialogOptions",
      memberName: "gtk",
    },
    {
      filePath: "muon.d.ts",
      parentName: "MuonFsDialogOptions",
      memberName: "win32",
    },
    {
      filePath: "muon.d.ts",
      parentName: "MuonFsSaveDialogOptions",
      memberName: "defaultName",
    },
    {
      filePath: "muon.d.ts",
      parentName: "MuonFsSaveDialogOptions",
      memberName: "confirmOverwrite",
    },
    {
      filePath: "muon.d.ts",
      parentName: "MuonExecutorSpawnOptions",
      memberName: "args",
    },
    {
      filePath: "muon.d.ts",
      parentName: "MuonExecutorSpawnOptions",
      memberName: "cwd",
    },
    {
      filePath: "muon.d.ts",
      parentName: "MuonExecutorSpawnOptions",
      memberName: "env",
    },
    {
      filePath: "vite.d.ts",
      parentName: "MuonViteBuildOptions",
      memberName: "targets",
    },
    {
      filePath: "vite.d.ts",
      parentName: "MuonViteBuildOptions",
      memberName: "allTargets",
    },
    {
      filePath: "vite.d.ts",
      parentName: "MuonViteBuildOptions",
      memberName: "appName",
    },
    {
      filePath: "vite.d.ts",
      parentName: "MuonViteBuildOptions",
      memberName: "appId",
    },
    {
      filePath: "vite.d.ts",
      parentName: "MuonViteBuildOptions",
      memberName: "outputRoot",
    },
    {
      filePath: "vite.d.ts",
      parentName: "MuonViteBuildOptions",
      memberName: "configPath",
    },
    {
      filePath: "vite.d.ts",
      parentName: "MuonViteBuildOptions",
      memberName: "iconPath",
    },
    {
      filePath: "vite.d.ts",
      parentName: "MuonViteBuildOptions",
      memberName: "windowsResource",
    },
    {
      filePath: "vite.d.ts",
      parentName: "MuonViteBuildOptions",
      memberName: "linuxDesktop",
    },
    {
      filePath: "vite.d.ts",
      parentName: "MuonViteBuildOptions",
      memberName: "packageDirectory",
    },
    {
      filePath: "vite.d.ts",
      parentName: "MuonViteBuildOptions",
      memberName: "assetSalt",
    },
    {
      filePath: "vite.d.ts",
      parentName: "MuonVitePluginOptions",
      memberName: "muonPath",
    },
    {
      filePath: "vite.d.ts",
      parentName: "MuonVitePluginOptions",
      memberName: "cefPath",
    },
    {
      filePath: "vite.d.ts",
      parentName: "MuonVitePluginOptions",
      memberName: "stagePath",
    },
    {
      filePath: "vite.d.ts",
      parentName: "MuonVitePluginOptions",
      memberName: "open",
    },
    {
      filePath: "vite.d.ts",
      parentName: "MuonVitePluginOptions",
      memberName: "enableDebugger",
    },
    {
      filePath: "vite.d.ts",
      parentName: "MuonVitePluginOptions",
      memberName: "build",
    },
  ];

const publicDeclarationCallableDefaultValueTargets: PublicDeclarationCallableDefaultValueTarget[] =
  [
    {
      filePath: "vite.d.ts",
      callableName: "muon",
    },
  ];

const readDeclarationSourceFile = async (
  filePath: string,
): Promise<ts.SourceFile> => {
  const declarationPath = resolve(filePath);
  const source = await readFile(declarationPath, "utf8");
  return ts.createSourceFile(
    declarationPath,
    source,
    ts.ScriptTarget.Latest,
    true,
  );
};

const getMemberName = (member: ts.TypeElement): string | undefined =>
  member.name === undefined ? undefined : member.name.getText();

const hasDefaultValueTag = (node: ts.Node): boolean =>
  ts.getJSDocTags(node).some((tag) => tag.tagName.getText() === "defaultValue");

const hasDefaultValueTagOnNodeOrParents = (node: ts.Node): boolean => {
  let current: ts.Node | undefined = node;
  while (current !== undefined) {
    if (hasDefaultValueTag(current)) {
      return true;
    }
    current = current.parent;
  }
  return false;
};

const findInterfaceMember = (
  sourceFile: ts.SourceFile,
  parentName: string,
  memberName: string,
): ts.TypeElement | undefined => {
  let found: ts.TypeElement | undefined = undefined;
  const visit = (node: ts.Node): void => {
    if (found !== undefined) {
      return;
    }
    if (ts.isInterfaceDeclaration(node) && node.name.text === parentName) {
      found = node.members.find(
        (member) => getMemberName(member) === memberName,
      );
      return;
    }
    ts.forEachChild(node, visit);
  };
  visit(sourceFile);
  return found;
};

const findCallableDeclaration = (
  sourceFile: ts.SourceFile,
  callableName: string,
): ts.Node | undefined => {
  let found: ts.Node | undefined = undefined;
  const visit = (node: ts.Node): void => {
    if (found !== undefined) {
      return;
    }
    if (
      ts.isVariableDeclaration(node) &&
      ts.isIdentifier(node.name) &&
      node.name.text === callableName
    ) {
      found = node;
      return;
    }
    ts.forEachChild(node, visit);
  };
  visit(sourceFile);
  return found;
};

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

const runTypeScriptConsumer = async (
  source: string,
  types: readonly string[] = [],
): Promise<void> => {
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
          types,
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
  await mkdir(join(root, "deps", "engraver", "libengraver", "include"), {
    recursive: true,
  });
  await mkdir(join(root, "muon-builder"), { recursive: true });
  await mkdir(join(root, "muon-core"), { recursive: true });
  await mkdir(join(root, "muon-ui", "scripts"), { recursive: true });
  await writeFile(join(root, "deps", "tra-ffic", "include", "tra_ffic.h"), "");
  await writeFile(join(root, "deps", "cardio", "include", "cardio.h"), "");
  await writeFile(
    join(root, "deps", "engraver", "libengraver", "include", "engraver.h"),
    "",
  );
  await writeFile(
    join(root, "muon-builder", "package.json"),
    `${JSON.stringify({ version: "0.0.0" }, null, 2)}\n`,
  );
  await writeFile(
    join(root, "muon-core", "package.json"),
    `${JSON.stringify({ version: "0.0.0" }, null, 2)}\n`,
  );
  await writeFile(
    join(root, "muon-ui", "package.json"),
    `${JSON.stringify({ version: "1.2.3" }, null, 2)}\n`,
  );
  await writeFile(
    join(root, "muon-ui", "scripts", "stage-muon-builder.mjs"),
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
if [[ "\${1:-}" == "run" && "\${2:-}" == "generate:runtime-version-header" ]]; then
  output_path=""
  for arg in "$@"; do
    output_path="\${arg}"
  done
  mkdir -p "$(dirname "\${output_path}")"
  cat > "\${output_path}" <<'HEADER'
#ifndef MUON_CORE_VERSION_GENERATED_H
#define MUON_CORE_VERSION_GENERATED_H
#define MUON_CORE_VERSION "1.2.3"
#define MUON_CORE_GIT_COMMIT_HASH "fake-generated-hash"
#define MUON_CORE_BUILD_DATE "2026-07-07T00:00:00+09:00"
#define MUON_CORE_GIT_COMMIT_DATE "2026-07-01T00:00:00+09:00"
#endif
HEADER
fi
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
    expect(containerInvocation).toContain(
      "MUON_CORE_VERSION_HEADER=/workspace/muon-core/.build/package/muon_core_version_generated.h",
    );
    await expect(
      readFile(
        join(
          root,
          "muon-core",
          ".build",
          "package",
          "muon_core_version_generated.h",
        ),
        "utf8",
      ),
    ).resolves.toContain('#define MUON_CORE_VERSION "1.2.3"');
    await expect(readFile(npmLog, "utf8")).resolves.toContain(
      "generate:runtime-version-header",
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
    await writeExecutableScript(
      join(binDirectory, "npm"),
      `#!/usr/bin/env bash
set -euo pipefail
if [[ "\${1:-}" == "run" && "\${2:-}" == "generate:runtime-version-header" ]]; then
  output_path=""
  for arg in "$@"; do
    output_path="\${arg}"
  done
  mkdir -p "$(dirname "\${output_path}")"
  cat > "\${output_path}" <<'HEADER'
#ifndef MUON_CORE_VERSION_GENERATED_H
#define MUON_CORE_VERSION_GENERATED_H
#define MUON_CORE_VERSION "1.2.3"
#define MUON_CORE_GIT_COMMIT_HASH "fake-generated-hash"
#define MUON_CORE_BUILD_DATE "2026-07-07T00:00:00+09:00"
#define MUON_CORE_GIT_COMMIT_DATE "2026-07-01T00:00:00+09:00"
#endif
HEADER
  exit 0
fi
printf 'Unexpected fake npm invocation: %s\\n' "$*" >&2
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
    const logPath = join(root, ".deps", "package-logs", "linux-arm64.log");
    expect(execError.code).toBe(1);
    expect(output).toContain("Linux package target failed: linux-arm64");
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
    const nativeBuilderStat = await stat(
      resolve("dist/native/linux-amd64/muon-builder"),
    );
    const nativeBootstrapStat = await stat(
      resolve("dist/native/linux-amd64/muon-bootstrap"),
    );
    const nativeRuntimeHelperStat = await stat(
      resolve("dist/native/linux-amd64/muon-runtime-helper"),
    );
    const nativeDefaultIconStat = await stat(
      resolve("dist/native/muon-256.png"),
    );

    expect(packageJson.bin?.muon).toBe("./dist/cli.cjs");
    expect(cliStat.mode & 0o111).not.toBe(0);
    expect(nativeBuilderStat.mode & 0o111).not.toBe(0);
    expect(nativeBootstrapStat.mode & 0o111).not.toBe(0);
    expect(nativeRuntimeHelperStat.mode & 0o111).not.toBe(0);
    expect(nativeDefaultIconStat.size).toBeGreaterThan(0);
    await expect(exists(resolve("dist/cli.mjs"))).resolves.toBe(false);
    await expect(exists(resolve("dist/vite.d.ts"))).resolves.toBe(false);
    await expect(exists(resolve("dist/muon-builder"))).resolves.toBe(false);
    await expect(
      exists(resolve("dist", ["muon", "prepare"].join("-"))),
    ).resolves.toBe(false);
    await expect(exists(resolve("dist/muon-bootstrap"))).resolves.toBe(false);
    await expect(
      exists(resolve("dist/native/muon-bootstrap.png")),
    ).resolves.toBe(false);
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
    expect(stdout).toMatch(/\n\s+run\s+/);
    expect(stdout).not.toMatch(/\n\s+dev\s+/);
    expect(stdout).toContain("pack");
    expect(stdout).toContain("init");
    expect(stdout).toContain("prepare");
    expect(stdout).toContain("embed-config");
  });

  it("adds muon generated directories through the muon CLI init command", async () => {
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
      ".muon/\ndist-muon/\nartifacts/\n",
    );
  });

  it("appends a missing muon dist gitignore entry when the muon CLI init command is repeated", async () => {
    const root = await mkdtemp(join(tmpdir(), "muon-init-existing-"));
    cleanupDirectories.push(root);
    await writeFile(join(root, ".gitignore"), "dist*/\n.muon/\n");
    const cliPath = resolve("dist", "cli.cjs");

    await execFileAsync(process.execPath, [cliPath, "init"], {
      cwd: root,
      encoding: "utf8",
    });

    await expect(readFile(join(root, ".gitignore"), "utf8")).resolves.toBe(
      "dist*/\n.muon/\ndist-muon/\nartifacts/\n",
    );
  });

  it("adds the current muon dist gitignore entry when a legacy entry exists", async () => {
    const root = await mkdtemp(join(tmpdir(), "muon-init-legacy-existing-"));
    cleanupDirectories.push(root);
    await writeFile(join(root, ".gitignore"), "dist*/\n.muon/\ndist-muon-*/\n");
    const cliPath = resolve("dist", "cli.cjs");

    await execFileAsync(process.execPath, [cliPath, "init"], {
      cwd: root,
      encoding: "utf8",
    });

    await expect(readFile(join(root, ".gitignore"), "utf8")).resolves.toBe(
      "dist*/\n.muon/\ndist-muon-*/\ndist-muon/\nartifacts/\n",
    );
  });

  it("keeps existing muon generated gitignore entries when the muon CLI init command is repeated", async () => {
    const root = await mkdtemp(join(tmpdir(), "muon-init-generated-existing-"));
    cleanupDirectories.push(root);
    await writeFile(
      join(root, ".gitignore"),
      "dist*/\n.muon/\ndist-muon/\nartifacts/\n",
    );
    const cliPath = resolve("dist", "cli.cjs");

    await execFileAsync(process.execPath, [cliPath, "init"], {
      cwd: root,
      encoding: "utf8",
    });

    await expect(readFile(join(root, ".gitignore"), "utf8")).resolves.toBe(
      "dist*/\n.muon/\ndist-muon/\nartifacts/\n",
    );
  });

  it("publishes only public TypeScript declaration files", async () => {
    const files = await getDryRunPackageFiles();

    expect(files).toContain("muon.d.ts");
    expect(files).toContain("muon-virtual-modules.d.ts");
    expect(files).toContain("vite.d.ts");
    expect(files).not.toContain("dist/cli.mjs");
    expect(files).not.toContain("dist/cli.d.ts");
    expect(files).not.toContain("dist/index.d.ts");
    expect(files).not.toContain("dist/prepare.d.ts");
    expect(files).not.toContain("dist/vite-internals.d.ts");
    expect(files).not.toContain("dist/vite.d.ts");
    expect(files).not.toContain("dist/runtime/linux-amd64/muon-runtime.json");
    expect(files.some((file) => file.endsWith(".d.ts.map"))).toBe(false);
    expect(files).not.toContain("dist/muon-builder");
    expect(files).not.toContain(`dist/${["muon", "prepare"].join("-")}`);
    expect(files).not.toContain("dist/muon-bootstrap");
    await expect(readFile(resolve("vite.d.ts"), "utf8")).resolves.not.toContain(
      "sourceMappingURL",
    );
  });

  it("documents default values in public TypeScript declarations", async () => {
    const sourceFiles = new Map<string, ts.SourceFile>();
    const getSourceFile = async (filePath: string): Promise<ts.SourceFile> => {
      const cached = sourceFiles.get(filePath);
      if (cached !== undefined) {
        return cached;
      }
      const sourceFile = await readDeclarationSourceFile(filePath);
      sourceFiles.set(filePath, sourceFile);
      return sourceFile;
    };
    const missingTargets: string[] = [];

    for (const target of publicDeclarationDefaultValueTargets) {
      const sourceFile = await getSourceFile(target.filePath);
      const member = findInterfaceMember(
        sourceFile,
        target.parentName,
        target.memberName,
      );
      if (member === undefined || !hasDefaultValueTag(member)) {
        missingTargets.push(
          `${target.filePath}:${target.parentName}.${target.memberName}`,
        );
      }
    }
    for (const target of publicDeclarationCallableDefaultValueTargets) {
      const sourceFile = await getSourceFile(target.filePath);
      const declaration = findCallableDeclaration(
        sourceFile,
        target.callableName,
      );
      if (
        declaration === undefined ||
        !hasDefaultValueTagOnNodeOrParents(declaration)
      ) {
        missingTargets.push(`${target.filePath}:${target.callableName}()`);
      }
    }

    expect(missingTargets).toEqual([]);
  });

  it("provides muon globals through root TypeScript imports", async () => {
    await expect(
      runTypeScriptConsumer(`import "muon-ui";

const reloadResult: Promise<void> = window.muon.browser.reload();
const resetZoomResult: Promise<void> = window.muon.browser.resetZoom();
const recycleResult: Promise<void> = window.muon.browser.recycle();
const windowBoundsResult: Promise<MuonWindowBounds> = window.muon.browser.getWindowBounds();
const setWindowBoundsResult: Promise<void> = window.muon.browser.setWindowBounds({
  x: 0,
  y: 0,
  width: 800,
  height: 600,
});
const titleBarVisibilityResult: Promise<void> = window.muon.browser.setTitleBarVisibility(true);
const titleBarIconResult: Promise<void> = window.muon.browser.setTitleBarIcon("icons/app.png");
const trayIdResult: Promise<string> = window.muon.browser.createTray(
  {
    id: "main",
    icon: "icons/app.png",
    tooltip: "Ready",
    menu: [
      { id: "open", label: "Open" },
      { type: "checkbox", id: "mute", label: "Mute", checked: false },
      { type: "separator" },
    ],
  },
  (event) => {
    if (event.type === "menu") {
      const checked: boolean = event.checked;
      void checked;
    }
  },
);
const titleBarTrayIdResult: Promise<string> = window.muon.browser.createTray({
  id: "title-bar",
});
const trayMenuResult: Promise<void> = window.muon.browser.setTrayMenu(
  "main",
  [{ type: "radio", id: "mode-a", label: "Mode A", checked: true }],
);
const trayIconResult: Promise<void> = window.muon.browser.setTrayIcon("main", "icons/tray.png");
const trayTooltipResult: Promise<void> = window.muon.browser.setTrayTooltip("main", null);
const trayRemoveResult: Promise<void> = window.muon.browser.removeTray("main");
const existsResult: Promise<boolean> = window.muon.fs.exists("asset://main/file");
void reloadResult;
void resetZoomResult;
void recycleResult;
void windowBoundsResult;
void setWindowBoundsResult;
void titleBarVisibilityResult;
void titleBarIconResult;
void trayIdResult;
void titleBarTrayIdResult;
void trayMenuResult;
void trayIconResult;
void trayTooltipResult;
void trayRemoveResult;
void existsResult;
`),
    ).resolves.toBeUndefined();
    await expect(
      runTypeScriptConsumer(`import type {} from "muon-ui";

const reloadResult: Promise<void> = window.muon.browser.reload();
const resetZoomResult: Promise<void> = window.muon.browser.resetZoom();
const recycleResult: Promise<void> = window.muon.browser.recycle();
const windowBoundsResult: Promise<MuonWindowBounds> = window.muon.browser.getWindowBounds();
const setWindowBoundsResult: Promise<void> = window.muon.browser.setWindowBounds({
  x: 0,
  y: 0,
  width: 800,
  height: 600,
});
const titleBarVisibilityResult: Promise<void> = window.muon.browser.setTitleBarVisibility(false);
const clearTitleBarIconResult: Promise<void> = window.muon.browser.setTitleBarIcon(null);
const trayIdResult: Promise<string> = window.muon.browser.createTray({
  icon: "icons/app.png",
});
const trayMenuResult: Promise<void> = window.muon.browser.setTrayMenu(
  "main",
  [{ id: "open", label: "Open" }],
  (event) => {
    const trayId: string = event.trayId;
    void trayId;
  },
);
const trayIconResult: Promise<void> = window.muon.browser.setTrayIcon("main", "icons/tray.png");
const trayTooltipResult: Promise<void> = window.muon.browser.setTrayTooltip("main", "Ready");
const trayRemoveResult: Promise<void> = window.muon.browser.removeTray("main");
const existsResult: Promise<boolean> = window.muon.fs.exists("asset://main/file");
void reloadResult;
void resetZoomResult;
void recycleResult;
void windowBoundsResult;
void setWindowBoundsResult;
void titleBarVisibilityResult;
void clearTitleBarIconResult;
void trayIdResult;
void trayMenuResult;
void trayIconResult;
void trayTooltipResult;
void trayRemoveResult;
void existsResult;
`),
    ).resolves.toBeUndefined();
  });

  it("provides muon virtual modules through configured TypeScript types", async () => {
    await expect(
      runTypeScriptConsumer(
        `import { spawn } from "muon:executor";

const run = async (): Promise<void> => {
  const process = await spawn({ command: "node", args: ["--version"] });
  const result: MuonExecutorSpawnResult = await process.wait();
  void result;
};
void run;
`,
        ["muon-ui"],
      ),
    ).resolves.toBeUndefined();
  });

  it("provides the Vite plugin with separated muon and CEF paths", async () => {
    await expect(
      runTypeScriptConsumer(`import muon from "muon-ui/vite";

const defaultPlugin = muon();
const plugin = muon({
  muonPath: "../muon-core/.run/dev-linux-amd64-debug",
  cefPath: "../muon-core/.cef/cef_binary_fake_linux64_minimal",
  open: false,
  enableDebugger: false,
  build: {
    targets: ["linux-amd64"],
    iconPath: "icons/app.png",
    windowsResource: {
      iconPath: "icons/app.png",
      productName: "Package Test App",
      fileDescription: "Package test app",
      companyName: "muon Tester",
      version: "1.2.3",
      copyright: "Copyright muon Tester",
      language: 1033,
      codePage: 1200,
    },
    linuxDesktop: {
      desktopId: "com.example.PackageTest",
      name: "Package Test App",
      comment: "Package test app",
      iconPath: "icons/app.png",
      categories: ["Utility", "Development"],
      startupNotify: true,
    },
  },
});
const explicitOpenPlugin = muon({ open: true });
void defaultPlugin;
void plugin;
void explicitOpenPlugin;
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
