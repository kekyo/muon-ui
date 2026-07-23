// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile } from "node:child_process";
import { createReadStream } from "node:fs";
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
import { basename, dirname, join, resolve } from "node:path";
import { pathToFileURL } from "node:url";
import { promisify } from "node:util";
import { inflateRawSync } from "node:zlib";

import { afterEach, describe, expect, it } from "vitest";
import { createTarExtractor } from "tar-vern";

import {
  createMuonLauncherEmbeddedConfigSlot,
  createMuonEmbeddedConfigSlot,
} from "../src/embed-config.js";
import { packMuonApp } from "../src/pack.js";
import { createWindowsIconBufferFromPngData } from "../src/windows-icon.js";
import type { MuonBuildTarget } from "../src/build.js";

const execFileAsync = promisify(execFile);
const cleanupDirectories: string[] = [];

const runMuonCli = async (
  root: string,
  args: readonly string[],
  environment: NodeJS.ProcessEnv = process.env,
): Promise<{ stderr: string; stdout: string }> => {
  const result = await execFileAsync(
    process.execPath,
    [resolve("dist", "cli.cjs"), ...args],
    {
      cwd: root,
      env: environment,
      maxBuffer: 10 * 1024 * 1024,
    },
  );
  return {
    stderr: result.stderr,
    stdout: result.stdout,
  };
};

const createTemporaryDirectory = async (prefix: string): Promise<string> => {
  const directory = await mkdtemp(join(tmpdir(), prefix));
  cleanupDirectories.push(directory);
  return directory;
};

const exists = async (path: string): Promise<boolean> => {
  try {
    await access(path);
    return true;
  } catch {
    return false;
  }
};

const createFakeSigningCommand = async (
  binDirectory: string,
  logDirectory: string,
  name: string,
): Promise<{ command: string; args: readonly string[]; logPath: string }> => {
  const scriptPath = join(binDirectory, name);
  const logPath = join(logDirectory, `${name}.log`);
  await writeExecutable(
    scriptPath,
    [
      "#!/usr/bin/env bash",
      "set -euo pipefail",
      `${JSON.stringify(process.execPath)} --input-type=module - "$@" <<'EOF'`,
      "import { appendFileSync } from 'node:fs';",
      "const [kind, target, path] = process.argv.slice(2);",
      `appendFileSync(${JSON.stringify(logPath)}, JSON.stringify({ kind, target, path }) + "\\n");`,
      "EOF",
      "",
    ].join("\n"),
  );
  return {
    command: name,
    args: ["{kind}", "{target}", "{path}"],
    logPath,
  };
};

const readSigningLog = async (
  logPath: string,
): Promise<{ kind: string; target: string; path: string }[]> => {
  if (!(await exists(logPath))) {
    return [];
  }
  const content = await readFile(logPath, "utf8");
  return content
    .trim()
    .split("\n")
    .filter((line) => line.length > 0)
    .map(
      (line) =>
        JSON.parse(line) as { kind: string; target: string; path: string },
    );
};

const writeExecutable = async (
  path: string,
  content: Buffer | string,
): Promise<void> => {
  await mkdir(join(path, ".."), { recursive: true });
  await writeFile(path, content);
  await chmod(path, 0o755);
};

const fakeTargetDescriptors: Record<
  MuonBuildTarget,
  {
    runtimeExecutableName: string;
    uiLibraryName: string;
    cardioLibraryName: string;
    launcherExecutableName: string;
    runtimeHelperExecutableName?: string;
  }
> = {
  "linux-amd64": {
    runtimeExecutableName: "muon-core",
    uiLibraryName: "libmuon-ui.so",
    cardioLibraryName: "libcardio.so",
    launcherExecutableName: "muon-launcher",
    runtimeHelperExecutableName: "muon-runtime-helper",
  },
  "linux-armhf": {
    runtimeExecutableName: "muon-core",
    uiLibraryName: "libmuon-ui.so",
    cardioLibraryName: "libcardio.so",
    launcherExecutableName: "muon-launcher",
    runtimeHelperExecutableName: "muon-runtime-helper",
  },
  "linux-arm64": {
    runtimeExecutableName: "muon-core",
    uiLibraryName: "libmuon-ui.so",
    cardioLibraryName: "libcardio.so",
    launcherExecutableName: "muon-launcher",
    runtimeHelperExecutableName: "muon-runtime-helper",
  },
  "windows-i686": {
    runtimeExecutableName: "muon-core.exe",
    uiLibraryName: "libmuon-ui.dll",
    cardioLibraryName: "libcardio.dll",
    launcherExecutableName: "muon-launcher.exe",
  },
  "windows-amd64": {
    runtimeExecutableName: "muon-core.exe",
    uiLibraryName: "libmuon-ui.dll",
    cardioLibraryName: "libcardio.dll",
    launcherExecutableName: "muon-launcher.exe",
  },
};

const createFakeMuonPackageDist = async (
  root: string,
  targets: readonly MuonBuildTarget[],
): Promise<string> => {
  const packageDirectory = join(root, "package-dist");
  for (const target of targets) {
    const descriptor = fakeTargetDescriptors[target];
    const runtimeDirectory = join(packageDirectory, "runtime", target);
    const nativeDirectory = join(packageDirectory, "native", target);
    await mkdir(runtimeDirectory, { recursive: true });
    await mkdir(nativeDirectory, { recursive: true });
    await writeExecutable(
      join(runtimeDirectory, descriptor.runtimeExecutableName),
      Buffer.concat([
        Buffer.from("core prefix\n"),
        createMuonEmbeddedConfigSlot(),
        Buffer.from("\ncore suffix\n"),
      ]),
    );
    await writeFile(join(runtimeDirectory, descriptor.uiLibraryName), "ui\n");
    await writeFile(
      join(runtimeDirectory, descriptor.cardioLibraryName),
      "cardio\n",
    );
    const nodePluginFileName = target.startsWith("windows-")
      ? "node.dll"
      : "node.so";
    await mkdir(join(runtimeDirectory, "plugins"), { recursive: true });
    await writeFile(
      join(runtimeDirectory, "plugins", nodePluginFileName),
      `${target} node plugin\n`,
    );
    await writeFile(
      join(runtimeDirectory, "plugins", "node-bridge.mjs"),
      "export const bridgeFixture = true;\n",
    );
    await writeFile(
      join(
        runtimeDirectory,
        target.startsWith("windows-") ? "node.exe" : "node",
      ),
      "node executable must not be copied\n",
    );
    await writeFile(
      join(runtimeDirectory, "node-runtime.tar.xz"),
      "node archive must not be copied\n",
    );
    await writeFile(join(runtimeDirectory, "CREDITS.md"), "notices\n");
    await writeExecutable(
      join(nativeDirectory, descriptor.launcherExecutableName),
      Buffer.concat([
        Buffer.from("launcher prefix\n"),
        createMuonLauncherEmbeddedConfigSlot(),
        Buffer.from("\nlauncher suffix\n"),
      ]),
    );
    if (descriptor.runtimeHelperExecutableName !== undefined) {
      await writeExecutable(
        join(nativeDirectory, descriptor.runtimeHelperExecutableName),
        Buffer.concat([
          Buffer.from("helper prefix\n"),
          createMuonLauncherEmbeddedConfigSlot(),
          Buffer.from("\nhelper suffix\n"),
        ]),
      );
    }
  }
  return packageDirectory;
};

const writeViteProject = async (
  root: string,
  packageDirectory: string,
  buildTargets: readonly string[],
  base: string | undefined = undefined,
  options: {
    buildOptions?: Record<string, unknown>;
    muonConfig?: Record<string, unknown>;
    packageJson?: Record<string, unknown>;
  } = {},
): Promise<void> => {
  const vitePluginUrl = pathToFileURL(resolve("dist", "vite.mjs")).href;
  await writeFile(
    join(root, "package.json"),
    `${JSON.stringify(
      {
        name: "@scope/packed-sample",
        version: "1.2.3",
        description: "Packed sample",
        author: "muon Tester",
        type: "module",
        ...(options.packageJson ?? {}),
      },
      null,
      2,
    )}\n`,
  );
  await writeFile(
    join(root, "index.html"),
    '<!doctype html><title>packed</title><script type="module" src="/src/main.ts"></script>',
  );
  await mkdir(join(root, "src"), { recursive: true });
  await writeFile(
    join(root, "src", "main.ts"),
    "document.body.textContent = 'packed';\n",
  );
  await writeFile(
    join(root, "muon.json"),
    `${JSON.stringify(
      options.muonConfig ?? { network: { allow: ["asset://main/**"] } },
      null,
      2,
    )}\n`,
  );
  await writeFile(
    join(root, "vite.config.mjs"),
    [
      `import muon from ${JSON.stringify(vitePluginUrl)};`,
      "export default {",
      ...(base === undefined ? [] : [`  base: ${JSON.stringify(base)},`]),
      "  build: { outDir: 'web-dist' },",
      "  plugins: [",
      `    muon({ build: ${JSON.stringify({
        targets: buildTargets,
        packageDirectory,
        ...(options.buildOptions ?? {}),
      })} }),`,
      "  ],",
      "};",
    ].join("\n"),
  );
};

const writeViteProjectWithoutMuonPlugin = async (
  root: string,
): Promise<void> => {
  await writeFile(
    join(root, "package.json"),
    `${JSON.stringify(
      {
        name: "plain-vite-packed-sample",
        version: "1.2.3",
        description: "Plain Vite packed sample",
        author: "muon Tester",
        type: "module",
      },
      null,
      2,
    )}\n`,
  );
  await mkdir(join(root, "assets"), { recursive: true });
  await writeFile(
    join(root, "assets", "index.html"),
    "<!doctype html><title>plain assets</title>",
  );
  await writeFile(
    join(root, "index.html"),
    '<!doctype html><title>plain vite</title><script type="module" src="/src/main.ts"></script>',
  );
  await mkdir(join(root, "src"), { recursive: true });
  await writeFile(
    join(root, "src", "main.ts"),
    "document.body.textContent = 'plain vite';\n",
  );
  await writeFile(
    join(root, "vite.config.mjs"),
    [
      "import { writeFileSync } from 'node:fs';",
      "export default {",
      "  build: { outDir: 'web-dist' },",
      "  plugins: [{",
      "    name: 'vite-build-marker',",
      "    closeBundle() {",
      "      writeFileSync(new URL('vite-build-marker.txt', import.meta.url), 'built\\n');",
      "    },",
      "  }],",
      "};",
    ].join("\n"),
  );
};

const writeViteProjectWithMuonBuildDisabled = async (
  root: string,
): Promise<void> => {
  const vitePluginUrl = pathToFileURL(resolve("dist", "vite.mjs")).href;
  await writeFile(
    join(root, "package.json"),
    `${JSON.stringify(
      {
        name: "disabled-packed-sample",
        version: "1.2.3",
        description: "Disabled packed sample",
        author: "muon Tester",
        type: "module",
      },
      null,
      2,
    )}\n`,
  );
  await writeFile(
    join(root, "index.html"),
    '<!doctype html><title>disabled vite</title><script type="module" src="/src/main.ts"></script>',
  );
  await mkdir(join(root, "src"), { recursive: true });
  await writeFile(
    join(root, "src", "main.ts"),
    "document.body.textContent = 'disabled vite';\n",
  );
  await writeFile(
    join(root, "vite.config.mjs"),
    [
      `import muon from ${JSON.stringify(vitePluginUrl)};`,
      "export default {",
      "  build: { outDir: 'web-dist' },",
      "  plugins: [muon({ build: false })],",
      "};",
    ].join("\n"),
  );
};

const readZipEntryNames = async (archivePath: string): Promise<string[]> => {
  const content = await readFile(archivePath);
  const endSignature = 0x06054b50;
  let endOffset = -1;
  for (let offset = content.length - 22; offset >= 0; offset -= 1) {
    if (content.readUInt32LE(offset) === endSignature) {
      endOffset = offset;
      break;
    }
  }
  if (endOffset < 0) {
    throw new Error("ZIP end of central directory was not found.");
  }
  const entryCount = content.readUInt16LE(endOffset + 10);
  const centralDirectoryOffset = content.readUInt32LE(endOffset + 16);
  const names: string[] = [];
  let cursor = centralDirectoryOffset;
  for (let index = 0; index < entryCount; index += 1) {
    if (content.readUInt32LE(cursor) !== 0x02014b50) {
      throw new Error("ZIP central directory entry is invalid.");
    }
    const nameLength = content.readUInt16LE(cursor + 28);
    const extraLength = content.readUInt16LE(cursor + 30);
    const commentLength = content.readUInt16LE(cursor + 32);
    names.push(content.toString("utf8", cursor + 46, cursor + 46 + nameLength));
    cursor += 46 + nameLength + extraLength + commentLength;
  }
  return names.sort();
};

const readZipTextEntry = async (
  archivePath: string,
  entryName: string,
): Promise<string> => {
  return (await readZipBinaryEntry(archivePath, entryName)).toString("utf8");
};

const readZipBinaryEntry = async (
  archivePath: string,
  entryName: string,
): Promise<Buffer> => {
  const content = await readFile(archivePath);
  const endSignature = 0x06054b50;
  let endOffset = -1;
  for (let offset = content.length - 22; offset >= 0; offset -= 1) {
    if (content.readUInt32LE(offset) === endSignature) {
      endOffset = offset;
      break;
    }
  }
  if (endOffset < 0) {
    throw new Error("ZIP end of central directory was not found.");
  }
  const entryCount = content.readUInt16LE(endOffset + 10);
  const centralDirectoryOffset = content.readUInt32LE(endOffset + 16);
  let cursor = centralDirectoryOffset;
  for (let index = 0; index < entryCount; index += 1) {
    const method = content.readUInt16LE(cursor + 10);
    const compressedSize = content.readUInt32LE(cursor + 20);
    const nameLength = content.readUInt16LE(cursor + 28);
    const extraLength = content.readUInt16LE(cursor + 30);
    const commentLength = content.readUInt16LE(cursor + 32);
    const localHeaderOffset = content.readUInt32LE(cursor + 42);
    const name = content.toString(
      "utf8",
      cursor + 46,
      cursor + 46 + nameLength,
    );
    if (name === entryName) {
      const localNameLength = content.readUInt16LE(localHeaderOffset + 26);
      const localExtraLength = content.readUInt16LE(localHeaderOffset + 28);
      const dataStart =
        localHeaderOffset + 30 + localNameLength + localExtraLength;
      const data = content.subarray(dataStart, dataStart + compressedSize);
      return method === 0 ? data : inflateRawSync(data);
    }
    cursor += 46 + nameLength + extraLength + commentLength;
  }
  throw new Error(`ZIP entry was not found: ${entryName}`);
};

const readTarGzEntryNames = async (archivePath: string): Promise<string[]> => {
  const names: string[] = [];
  for await (const entry of createTarExtractor(
    createReadStream(archivePath),
    "gzip",
  )) {
    names.push(entry.path);
  }
  return names.sort();
};

const readTarGzTextEntry = async (
  archivePath: string,
  entryName: string,
): Promise<string> => {
  return (await readTarGzBinaryEntry(archivePath, entryName)).toString("utf8");
};

const readTarGzBinaryEntry = async (
  archivePath: string,
  entryName: string,
): Promise<Buffer> => {
  for await (const entry of createTarExtractor(
    createReadStream(archivePath),
    "gzip",
  )) {
    if (entry.kind === "file" && entry.path === entryName) {
      return await entry.getContent("buffer");
    }
  }
  throw new Error(`tar.gz entry was not found: ${entryName}`);
};

const writeFakeTool = async (
  binDirectory: string,
  name: string,
  script: string,
): Promise<void> => {
  await writeExecutable(join(binDirectory, name), script);
};

const createWindowsIconFixture = async (iconPath: string): Promise<void> => {
  await mkdir(dirname(iconPath), { recursive: true });
  await writeFile(
    iconPath,
    Buffer.from(
      "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg==",
      "base64",
    ),
  );
};

const createFakePackagingToolEnvironment = async (
  root: string,
): Promise<NodeJS.ProcessEnv> => {
  const binDirectory = join(root, "bin");
  await mkdir(binDirectory, { recursive: true });
  await writeFakeTool(
    binDirectory,
    "dpkg-deb",
    `#!/usr/bin/env bash
set -euo pipefail
args=("$@")
output_path="\${args[$((\${#args[@]} - 1))]}"
mkdir -p "$(dirname "$output_path")"
printf 'deb\\n' > "$output_path"
`,
  );
  await writeFakeTool(
    binDirectory,
    "makensis",
    `#!/usr/bin/env bash
set -euo pipefail
script_path="\${1:?}"
output_path="$(sed -n 's/^OutFile "\\(.*\\)"$/\\1/p' "$script_path")"
mkdir -p "$(dirname "$output_path")"
printf 'nsis\\n' > "$output_path"
`,
  );
  return {
    ...process.env,
    PATH: `${binDirectory}:${process.env.PATH ?? ""}`,
  };
};

const allFakePackageTargets = [
  "linux-amd64",
  "linux-armhf",
  "linux-arm64",
  "windows-i686",
  "windows-amd64",
] as const satisfies readonly MuonBuildTarget[];

const getArtifactSummary = (
  artifacts: readonly { type: string; target: string; path: string }[],
): string[] =>
  artifacts
    .map(
      (artifact) =>
        `${artifact.type}:${artifact.target}:${basename(artifact.path)}`,
    )
    .sort();

afterEach(async () => {
  for (const directory of cleanupDirectories.splice(0)) {
    await rm(directory, { recursive: true, force: true });
  }
});

describe("muon pack", () => {
  it("builds Vite output once and packages the configured Linux target as a tar.gz archive", async () => {
    const root = await createTemporaryDirectory("muon-pack-tar-gz-");
    const packageDirectory = await createFakeMuonPackageDist(root, [
      "linux-amd64",
    ]);
    await writeViteProject(root, packageDirectory, ["linux-amd64"]);

    const result = await packMuonApp({
      root,
      types: ["tar.gz"],
    });

    const [artifact] = result.artifacts;
    expect(result.targets.map((target) => target.target)).toEqual([
      "linux-amd64",
    ]);
    expect(artifact?.type).toBe("tar.gz");
    expect(artifact?.target).toBe("linux-amd64");
    expect(artifact?.path).toBe(
      join(root, "artifacts", "packed-sample-1.2.3-linux-amd64.tar.gz"),
    );
    await expect(exists(join(root, "dist-muon/linux-amd64"))).resolves.toBe(
      true,
    );
    const entries = await readTarGzEntryNames(artifact?.path ?? "");
    expect(entries).toContain("packed-sample/linux-amd64/assets.zip");
    expect(entries).toContain("packed-sample/linux-amd64/muon-install.json");
    expect(entries).not.toContain("dist-muon/linux-amd64/assets.zip");
    expect(entries).not.toContain("dist-muon/linux-amd64/muon-install.json");
    expect(entries).not.toContain("packed-sample/linux-amd64/libcef.so");
    await expect(
      readTarGzTextEntry(
        artifact?.path ?? "",
        "packed-sample/linux-amd64/CREDITS.md",
      ),
    ).resolves.toBe("notices\n");
    await expect(
      readTarGzTextEntry(
        artifact?.path ?? "",
        "packed-sample/linux-amd64/muon-install.json",
      ),
    ).resolves.toBe(
      `${JSON.stringify(
        {
          type: "portable",
          runtimeMode: "in-place",
        },
        undefined,
        2,
      )}\n`,
    );
    const portableCore = (
      await readTarGzBinaryEntry(
        artifact?.path ?? "",
        "packed-sample/linux-amd64/muon-core",
      )
    ).toString("utf8");
    expect(portableCore).toContain("profilePath");
    expect(portableCore).toContain("profile");
  });

  it("packages an opted-in Node project and host bridge without a Node executable", async () => {
    const root = await createTemporaryDirectory("muon-pack-node-project-");
    const packageDirectory = await createFakeMuonPackageDist(root, [
      "linux-amd64",
    ]);
    const backendDirectory = join(root, "backend");
    const dependencyDirectory = join(
      backendDirectory,
      "node_modules",
      "fixture-dependency",
    );
    await mkdir(dependencyDirectory, { recursive: true });
    await writeFile(
      join(backendDirectory, "package.json"),
      `${JSON.stringify(
        {
          name: "packed-node-project",
          type: "module",
          main: "./main.js",
        },
        null,
        2,
      )}\n`,
    );
    await writeFile(
      join(backendDirectory, "main.js"),
      [
        'import dependency from "fixture-dependency";',
        "process.stdout.write(`${dependency}\\n`);",
        "",
      ].join("\n"),
    );
    await writeFile(
      join(dependencyDirectory, "package.json"),
      `${JSON.stringify(
        {
          name: "fixture-dependency",
          type: "module",
          exports: "./index.js",
        },
        null,
        2,
      )}\n`,
    );
    await writeFile(
      join(dependencyDirectory, "index.js"),
      'export default "packed dependency";\n',
    );
    await writeViteProject(root, packageDirectory, ["linux-amd64"], undefined, {
      muonConfig: {
        node: {
          project: "./backend",
        },
      },
    });

    const result = await packMuonApp({
      root,
      types: ["tar.gz"],
    });

    const artifactPath = result.artifacts[0]?.path ?? "";
    const entries = await readTarGzEntryNames(artifactPath);
    const entryRoot = "packed-sample/linux-amd64";
    expect(entries).toContain(`${entryRoot}/plugins/node.so`);
    expect(entries).toContain(`${entryRoot}/plugins/node-bridge.mjs`);
    expect(entries).toContain(`${entryRoot}/node-project/package.json`);
    expect(entries).toContain(`${entryRoot}/node-project/main.js`);
    expect(entries).toContain(
      `${entryRoot}/node-project/node_modules/fixture-dependency/index.js`,
    );
    expect(entries).not.toContain(`${entryRoot}/node`);
    expect(entries).not.toContain(`${entryRoot}/node.exe`);
    expect(entries).not.toContain(`${entryRoot}/node-runtime.tar.xz`);
    await expect(
      readTarGzTextEntry(artifactPath, `${entryRoot}/plugins/node-bridge.mjs`),
    ).resolves.toBe("export const bridgeFixture = true;\n");
    const stagedExecution = await execFileAsync(process.execPath, [
      join(root, "dist-muon", "linux-amd64", "node-project"),
    ]);
    expect(stagedExecution.stdout).toBe("packed dependency\n");
    const assetEntries = await readZipEntryNames(
      join(root, "dist-muon", "linux-amd64", "assets.zip"),
    );
    expect(
      assetEntries.some((entry) => entry.startsWith("node-project/")),
    ).toBe(false);
  });

  it.each([
    ["the pack build root", ".muon/pack/backend"],
    ["the deb artifact work tree", "artifacts/deb/backend"],
    ["the NSIS artifact work tree", "artifacts/nsis/backend"],
  ])(
    "rejects a Node project inside %s before cleanup removes it",
    async (_label, projectPath) => {
      const root = await createTemporaryDirectory(
        "muon-pack-node-cleanup-source-",
      );
      const packageDirectory = await createFakeMuonPackageDist(root, [
        "linux-amd64",
      ]);
      const backendDirectory = join(root, ...projectPath.split("/"));
      const markerPath = join(backendDirectory, "source-marker.txt");
      await mkdir(backendDirectory, { recursive: true });
      await writeFile(
        join(backendDirectory, "package.json"),
        `${JSON.stringify({ name: "pack-cleanup-source", type: "module" })}\n`,
      );
      await writeFile(markerPath, "preserve source\n");
      await writeViteProject(
        root,
        packageDirectory,
        ["linux-amd64"],
        undefined,
        {
          muonConfig: {
            node: {
              project: backendDirectory,
            },
          },
        },
      );

      await expect(
        packMuonApp({
          root,
          types: ["tar.gz"],
        }),
      ).rejects.toThrow(
        "muon Node project and pack cleanup directory must not overlap",
      );
      await expect(readFile(markerPath, "utf8")).resolves.toBe(
        "preserve source\n",
      );
    },
  );

  it("applies the shared Vite preflight before pack deletes an outDir-overlapping Node project", async () => {
    const root = await createTemporaryDirectory(
      "muon-pack-node-outdir-source-",
    );
    const packageDirectory = await createFakeMuonPackageDist(root, [
      "linux-amd64",
    ]);
    const backendDirectory = join(root, "web-dist", "backend");
    const markerPath = join(backendDirectory, "source-marker.txt");
    await writeViteProject(root, packageDirectory, ["linux-amd64"], undefined, {
      buildOptions: {
        configPath: "settings/plugin-muon.json",
      },
    });
    await mkdir(join(root, "settings"), { recursive: true });
    await mkdir(backendDirectory, { recursive: true });
    await writeFile(
      join(backendDirectory, "package.json"),
      `${JSON.stringify({ name: "pack-outdir-source", type: "module" })}\n`,
    );
    await writeFile(markerPath, "preserve source\n");
    await writeFile(
      join(root, "settings", "plugin-muon.json"),
      `${JSON.stringify({ node: { project: backendDirectory } }, null, 2)}\n`,
    );

    await expect(
      packMuonApp({
        root,
        types: ["tar.gz"],
      }),
    ).rejects.toThrow("muon Node project");
    await expect(readFile(markerPath, "utf8")).resolves.toBe(
      "preserve source\n",
    );
  });

  it("writes build and packaging progress from the muon pack CLI while keeping JSON stdout", async () => {
    const root = await createTemporaryDirectory("muon-pack-cli-progress-");
    const packageDirectory = await createFakeMuonPackageDist(root, [
      "linux-amd64",
    ]);
    const environment = await createFakePackagingToolEnvironment(root);
    await writeViteProject(root, packageDirectory, ["linux-amd64"]);

    const result = await runMuonCli(
      root,
      ["pack", "--type", "deb", "--json"],
      environment,
    );
    const parsed = JSON.parse(result.stdout) as {
      artifacts: readonly { path: string; target: string; type: string }[];
    };
    const artifactPath = join(
      root,
      "artifacts",
      "packed-sample-1.2.3-amd64.deb",
    );

    expect(parsed.artifacts).toEqual([
      {
        path: artifactPath,
        target: "linux-amd64",
        type: "deb",
      },
    ]);
    expect(result.stderr).toContain("Building distributions");
    expect(result.stderr).toContain("Running Vite build");
    expect(result.stderr).toContain("Building muon target linux-amd64 (1/1)");
    expect(result.stderr).toContain("Packaging deb linux-amd64 (1/1)");
    expect(result.stderr).toContain("Running dpkg-deb");
    expect(result.stderr).toContain(`Wrote ${artifactPath}`);
  });

  it("warns and skips CLI package artifacts that require unavailable external tools", async () => {
    const root = await createTemporaryDirectory("muon-pack-cli-missing-tools-");
    const packageDirectory = await createFakeMuonPackageDist(root, [
      "linux-amd64",
      "windows-amd64",
    ]);
    const emptyPathDirectory = join(root, "empty-path");
    await mkdir(emptyPathDirectory, { recursive: true });
    await writeViteProject(root, packageDirectory, [
      "linux-amd64",
      "windows-amd64",
    ]);

    const result = await runMuonCli(
      root,
      ["pack", "--target", "amd64", "--json"],
      {
        ...process.env,
        PATH: emptyPathDirectory,
      },
    );
    const parsed = JSON.parse(result.stdout) as {
      artifacts: readonly { path: string; target: string; type: string }[];
    };

    expect(getArtifactSummary(parsed.artifacts)).toEqual([
      "tar.gz:linux-amd64:packed-sample-1.2.3-linux-amd64.tar.gz",
      "zip:windows-amd64:packed-sample-1.2.3-windows-amd64.zip",
    ]);
    expect(result.stderr).toContain(
      "Warning: dpkg-deb is not available; skipping deb package for linux-amd64.",
    );
    expect(result.stderr).toContain(
      "Warning: makensis is not available; skipping nsis package for windows-amd64.",
    );
  });

  it("packages Vite output under the configured base path when the muon plugin controls pack", async () => {
    const root = await createTemporaryDirectory("muon-pack-vite-base-");
    const packageDirectory = await createFakeMuonPackageDist(root, [
      "linux-amd64",
    ]);
    await writeViteProject(
      root,
      packageDirectory,
      ["linux-amd64"],
      "/sample-base/",
    );

    const result = await packMuonApp({
      root,
      types: ["tar.gz"],
    });

    const [target] = result.targets;
    const entries = await readZipEntryNames(
      join(root, "dist-muon/linux-amd64", "assets.zip"),
    );
    expect(entries).toContain("main/sample-base/index.html");
    expect(
      entries.some((entry) => entry.startsWith("main/sample-base/assets/")),
    ).toBe(true);
    expect(entries).not.toContain("main/index.html");
    expect(target?.embeddedConfig.browser).toEqual({
      initialTitleBarIcon: "asset://main/.muon/app-icon.png",
      startPage: "asset://main/sample-base/index.html",
    });
  });

  it("overrides explicit browser profile paths for portable tar.gz artifacts", async () => {
    const root = await createTemporaryDirectory("muon-pack-portable-profile-");
    const packageDirectory = await createFakeMuonPackageDist(root, [
      "linux-amd64",
    ]);
    await writeViteProject(root, packageDirectory, ["linux-amd64"]);
    await writeFile(
      join(root, "muon.json"),
      `${JSON.stringify(
        {
          browser: {
            profilePath: "profiles/custom",
          },
          network: {
            allow: ["asset://main/**"],
          },
        },
        null,
        2,
      )}\n`,
    );

    const result = await packMuonApp({
      root,
      types: ["tar.gz"],
    });

    const [artifact] = result.artifacts;
    const portableCore = (
      await readTarGzBinaryEntry(
        artifact?.path ?? "",
        "packed-sample/linux-amd64/muon-core",
      )
    ).toString("utf8");
    expect(portableCore).toContain("profilePath");
    expect(portableCore).toContain("profile");
    expect(portableCore).not.toContain("profiles/custom");
  });

  it("copies package.json files into package distributions while excluding the Vite asset output", async () => {
    const root = await createTemporaryDirectory("muon-pack-package-files-");
    const packageDirectory = await createFakeMuonPackageDist(root, [
      "linux-amd64",
    ]);
    await writeFile(join(root, "README.md"), "read me\n");
    await writeFile(join(root, "LICENSE"), "license\n");
    await writeViteProject(root, packageDirectory, ["linux-amd64"], undefined, {
      packageJson: {
        files: ["web-dist", "README.md", "LICENSE"],
      },
    });

    const result = await packMuonApp({
      root,
      types: ["tar.gz"],
    });

    const [artifact] = result.artifacts;
    const entries = await readTarGzEntryNames(artifact?.path ?? "");
    expect(entries).toContain("packed-sample/linux-amd64/README.md");
    expect(entries).toContain("packed-sample/linux-amd64/LICENSE");
    expect(entries).not.toContain(
      "packed-sample/linux-amd64/web-dist/index.html",
    );
    expect(entries).not.toContain("dist-muon/linux-amd64/README.md");
    expect(entries).not.toContain("dist-muon/linux-amd64/LICENSE");
    await expect(
      readTarGzTextEntry(
        artifact?.path ?? "",
        "packed-sample/linux-amd64/README.md",
      ),
    ).resolves.toBe("read me\n");
    await expect(
      readTarGzTextEntry(
        artifact?.path ?? "",
        "packed-sample/linux-amd64/LICENSE",
      ),
    ).resolves.toBe("license\n");
  });

  it("prefers muon Vite plugin distribution files over package.json files", async () => {
    const root = await createTemporaryDirectory(
      "muon-pack-plugin-distribution-files-",
    );
    const packageDirectory = await createFakeMuonPackageDist(root, [
      "linux-amd64",
    ]);
    await writeFile(join(root, "README.md"), "read me\n");
    await writeFile(join(root, "NOTICE.md"), "notice\n");
    await writeViteProject(root, packageDirectory, ["linux-amd64"], undefined, {
      buildOptions: {
        distributionFiles: ["NOTICE.md"],
      },
      packageJson: {
        files: ["README.md"],
      },
    });

    const result = await packMuonApp({
      root,
      types: ["tar.gz"],
    });

    const [artifact] = result.artifacts;
    const entries = await readTarGzEntryNames(artifact?.path ?? "");
    expect(entries).toContain("packed-sample/linux-amd64/NOTICE.md");
    expect(entries).not.toContain("packed-sample/linux-amd64/README.md");
    expect(entries).not.toContain("dist-muon/linux-amd64/NOTICE.md");
    await expect(
      readTarGzTextEntry(
        artifact?.path ?? "",
        "packed-sample/linux-amd64/NOTICE.md",
      ),
    ).resolves.toBe("notice\n");
  });

  it("uses an empty muon Vite plugin distribution file list as an override", async () => {
    const root = await createTemporaryDirectory(
      "muon-pack-empty-distribution-files-",
    );
    const packageDirectory = await createFakeMuonPackageDist(root, [
      "linux-amd64",
    ]);
    await writeFile(join(root, "README.md"), "read me\n");
    await writeViteProject(root, packageDirectory, ["linux-amd64"], undefined, {
      buildOptions: {
        distributionFiles: [],
      },
      packageJson: {
        files: ["README.md"],
      },
    });

    const result = await packMuonApp({
      root,
      types: ["tar.gz"],
    });

    const [artifact] = result.artifacts;
    await expect(
      readTarGzEntryNames(artifact?.path ?? ""),
    ).resolves.not.toContain("packed-sample/linux-amd64/README.md");
  });

  it("packages non-Vite assets without running Vite when no muon plugin is configured", async () => {
    const root = await createTemporaryDirectory("muon-pack-no-muon-plugin-");
    const packageDirectory = await createFakeMuonPackageDist(root, [
      "linux-amd64",
    ]);
    await writeViteProjectWithoutMuonPlugin(root);

    const result = await packMuonApp({
      root,
      packageDirectory,
      targets: ["linux-amd64"],
      types: ["tar.gz"],
    });

    const [artifact] = result.artifacts;
    expect(artifact?.type).toBe("tar.gz");
    await expect(exists(join(root, "vite-build-marker.txt"))).resolves.toBe(
      false,
    );
    await expect(
      readZipTextEntry(
        join(root, "dist-muon/linux-amd64", "assets.zip"),
        "index.html",
      ),
    ).resolves.toBe("<!doctype html><title>plain assets</title>");
  });

  it("normalizes the tgz package type alias to tar.gz", async () => {
    const root = await createTemporaryDirectory("muon-pack-tgz-");
    const packageDirectory = await createFakeMuonPackageDist(root, [
      "linux-amd64",
    ]);
    await writeViteProject(root, packageDirectory, ["linux-amd64"]);

    const result = await packMuonApp({
      root,
      types: ["tgz"],
    });

    const [artifact] = result.artifacts;
    expect(artifact?.type).toBe("tar.gz");
    expect(artifact?.path).toBe(
      join(root, "artifacts", "packed-sample-1.2.3-linux-amd64.tar.gz"),
    );
  });

  it("packages the configured Windows target as a ZIP", async () => {
    const root = await createTemporaryDirectory("muon-pack-zip-");
    const packageDirectory = await createFakeMuonPackageDist(root, [
      "windows-amd64",
    ]);
    await writeViteProject(root, packageDirectory, ["windows-amd64"]);

    const result = await packMuonApp({
      root,
      types: ["zip"],
    });

    const [artifact] = result.artifacts;
    expect(result.targets.map((target) => target.target)).toEqual([
      "windows-amd64",
    ]);
    expect(artifact?.type).toBe("zip");
    expect(artifact?.target).toBe("windows-amd64");
    expect(artifact?.path).toBe(
      join(root, "artifacts", "packed-sample-1.2.3-windows-amd64.zip"),
    );
    const entries = await readZipEntryNames(artifact?.path ?? "");
    expect(entries).toContain("packed-sample/windows-amd64/assets.zip");
    expect(entries).toContain("packed-sample/windows-amd64/muon-install.json");
    expect(entries).not.toContain("dist-muon/windows-amd64/assets.zip");
    expect(entries).not.toContain("dist-muon/windows-amd64/muon-install.json");
    expect(entries).not.toContain("packed-sample/windows-amd64/libcef.dll");
    await expect(
      readZipTextEntry(
        artifact?.path ?? "",
        "packed-sample/windows-amd64/CREDITS.md",
      ),
    ).resolves.toBe("notices\n");
    await expect(
      readZipTextEntry(
        artifact?.path ?? "",
        "packed-sample/windows-amd64/muon-install.json",
      ),
    ).resolves.toBe(
      `${JSON.stringify(
        {
          type: "portable",
          runtimeMode: "in-place",
        },
        undefined,
        2,
      )}\n`,
    );
    const portableCore = (
      await readZipBinaryEntry(
        artifact?.path ?? "",
        "packed-sample/windows-amd64/muon-core.exe",
      )
    ).toString("utf8");
    expect(portableCore).toContain("profilePath");
    expect(portableCore).toContain("profile");
  });

  it("rejects package builds when the Vite muon plugin disables muon builds", async () => {
    const root = await createTemporaryDirectory("muon-pack-disabled-");
    const packageDirectory = await createFakeMuonPackageDist(root, [
      "linux-amd64",
    ]);
    await writeViteProjectWithMuonBuildDisabled(root);

    await expect(
      packMuonApp({
        root,
        packageDirectory,
        targets: ["linux-amd64"],
        types: ["tar.gz"],
      }),
    ).rejects.toThrow("muon build is disabled by muon({ build: false })");
  });

  it("creates a deb package tree and invokes dpkg-deb for Linux targets", async () => {
    const root = await createTemporaryDirectory("muon-pack-deb-");
    const packageDirectory = await createFakeMuonPackageDist(root, [
      "linux-amd64",
    ]);
    const binDirectory = join(root, "bin");
    const logPath = join(root, "dpkg-deb.log");
    await mkdir(binDirectory, { recursive: true });
    await mkdir(join(root, "artifacts", "deb"), { recursive: true });
    await writeFile(join(root, "artifacts", "deb", "stale"), "stale\n");
    await writeFakeTool(
      binDirectory,
      "dpkg-deb",
      `#!/usr/bin/env bash
set -euo pipefail
printf '%s\\n' "$@" > '${logPath}'
args=("$@")
package_root="\${args[$((\${#args[@]} - 2))]}"
output_path="\${args[$((\${#args[@]} - 1))]}"
find "$package_root" -type f | sort > '${root}/deb-files.txt'
mkdir -p "$(dirname "$output_path")"
printf 'deb\\n' > "$output_path"
`,
    );
    await writeViteProject(root, packageDirectory, ["linux-amd64"]);

    const result = await packMuonApp({
      root,
      types: ["deb"],
      environment: {
        ...process.env,
        PATH: `${binDirectory}:${process.env.PATH ?? ""}`,
      },
    });

    const [artifact] = result.artifacts;
    expect(artifact?.type).toBe("deb");
    expect(artifact?.path).toBe(
      join(root, "artifacts", "packed-sample-1.2.3-amd64.deb"),
    );
    await expect(readFile(artifact?.path ?? "", "utf8")).resolves.toBe("deb\n");
    await expect(readFile(logPath, "utf8")).resolves.toBe(
      `--root-owner-group\n--build\n${join(root, ".muon", "pack", "deb", "packed-sample-linux-amd64")}\n${artifact?.path}\n`,
    );
    await expect(readdir(join(root, "artifacts"))).resolves.toEqual([
      "packed-sample-1.2.3-amd64.deb",
    ]);
    await expect(exists(join(root, "artifacts", "deb"))).resolves.toBe(false);
    await expect(
      readFile(join(root, "deb-files.txt"), "utf8"),
    ).resolves.toContain("/usr/bin/packed-sample");
    await expect(
      readFile(join(root, "deb-files.txt"), "utf8"),
    ).resolves.toContain(
      "/usr/lib/packed-sample/dist-muon/linux-amd64/packed-sample",
    );
    await expect(
      readFile(join(root, "deb-files.txt"), "utf8"),
    ).resolves.toContain(
      "/usr/lib/packed-sample/dist-muon/linux-amd64/muon-install.json",
    );
    await expect(
      readFile(join(root, "deb-files.txt"), "utf8"),
    ).resolves.toContain("/usr/share/applications/scope.packed-sample.desktop");
    await expect(
      readFile(join(root, "deb-files.txt"), "utf8"),
    ).resolves.toContain(
      "/usr/share/icons/hicolor/256x256/apps/scope.packed-sample.png",
    );
    const packageRoot = join(
      root,
      ".muon",
      "pack",
      "deb",
      "packed-sample-linux-amd64",
    );
    await expect(
      readFile(join(packageRoot, "DEBIAN", "control"), "utf8"),
    ).resolves.toBe(
      [
        "Package: packed-sample",
        "Version: 1.2.3",
        "Architecture: amd64",
        "Maintainer: muon Tester",
        [
          "Depends: libc6",
          "libgcc-s1",
          "libstdc++6",
          "libffi8",
          "libexpat1",
          "libdbus-1-3",
          "libglib2.0-0t64 | libglib2.0-0",
          "libgtk-3-0t64 | libgtk-3-0",
          "libatk-bridge2.0-0t64 | libatk-bridge2.0-0",
          "libatk1.0-0t64 | libatk1.0-0",
          "libatspi2.0-0t64 | libatspi2.0-0",
          "libcups2t64 | libcups2",
          "libnspr4",
          "libnss3",
          "libgbm1",
          "libasound2t64 | libasound2",
          "libudev1",
          "libx11-6",
          "libxcb1",
          "libxcomposite1",
          "libxdamage1",
          "libxext6",
          "libxfixes3",
          "libxi6",
          "libxkbcommon0",
          "libxrandr2",
          "libcairo2",
          "libpango-1.0-0",
        ].join(", "),
        "Description: Packed sample",
        "",
      ].join("\n"),
    );
    await expect(
      readFile(
        join(
          packageRoot,
          "usr",
          "share",
          "applications",
          "scope.packed-sample.desktop",
        ),
        "utf8",
      ),
    ).resolves.toBe(
      [
        "[Desktop Entry]",
        "Type=Application",
        "Name=@scope/packed-sample",
        "Comment=Packed sample",
        'Exec="/usr/bin/packed-sample" --muon-launch-from=normal',
        "TryExec=/usr/bin/packed-sample",
        "Icon=scope.packed-sample",
        "Terminal=false",
        "Categories=Utility;",
        "StartupNotify=true",
        "StartupWMClass=scope.packed-sample",
        "X-muon-Managed=true",
        "",
      ].join("\n"),
    );
    await expect(
      readFile(
        join(
          packageRoot,
          "usr",
          "lib",
          "packed-sample",
          "dist-muon/linux-amd64",
          "muon-install.json",
        ),
        "utf8",
      ),
    ).resolves.toBe(
      `${JSON.stringify(
        {
          type: "deb",
          packageName: "packed-sample",
          launcherPath: "/usr/bin/packed-sample",
        },
        undefined,
        2,
      )}\n`,
    );
  });

  it("keeps deb metadata separate from portable tar.gz metadata in mixed Linux packaging", async () => {
    const root = await createTemporaryDirectory("muon-pack-mixed-linux-");
    const packageDirectory = await createFakeMuonPackageDist(root, [
      "linux-amd64",
    ]);
    await writeViteProject(root, packageDirectory, ["linux-amd64"]);

    const result = await packMuonApp({
      root,
      types: ["tgz", "deb"],
      environment: await createFakePackagingToolEnvironment(root),
    });

    const tarArtifact = result.artifacts.find(
      (artifact) => artifact.type === "tar.gz",
    );
    const debArtifact = result.artifacts.find(
      (artifact) => artifact.type === "deb",
    );
    expect(tarArtifact?.path).toBe(
      join(root, "artifacts", "packed-sample-1.2.3-linux-amd64.tar.gz"),
    );
    expect(debArtifact?.path).toBe(
      join(root, "artifacts", "packed-sample-1.2.3-amd64.deb"),
    );
    await expect(
      readTarGzTextEntry(
        tarArtifact?.path ?? "",
        "packed-sample/linux-amd64/muon-install.json",
      ),
    ).resolves.toBe(
      `${JSON.stringify(
        {
          type: "portable",
          runtimeMode: "in-place",
        },
        undefined,
        2,
      )}\n`,
    );
    await expect(
      readFile(
        join(
          root,
          ".muon",
          "pack",
          "deb",
          "packed-sample-linux-amd64",
          "usr",
          "lib",
          "packed-sample",
          "dist-muon/linux-amd64",
          "muon-install.json",
        ),
        "utf8",
      ),
    ).resolves.toBe(
      `${JSON.stringify(
        {
          type: "deb",
          packageName: "packed-sample",
          launcherPath: "/usr/bin/packed-sample",
        },
        undefined,
        2,
      )}\n`,
    );
  });

  it("creates a setuid deb package tree with system runtime metadata", async () => {
    const root = await createTemporaryDirectory("muon-pack-deb-setuid-");
    const packageDirectory = await createFakeMuonPackageDist(root, [
      "linux-amd64",
    ]);
    const binDirectory = join(root, "bin");
    const logPath = join(root, "dpkg-deb.log");
    await mkdir(binDirectory, { recursive: true });
    await writeFakeTool(
      binDirectory,
      "dpkg-deb",
      `#!/usr/bin/env bash
set -euo pipefail
printf '%s\\n' "$@" > '${logPath}'
args=("$@")
output_path="\${args[$((\${#args[@]} - 1))]}"
mkdir -p "$(dirname "$output_path")"
printf 'deb\\n' > "$output_path"
`,
    );
    await writeViteProject(root, packageDirectory, ["linux-amd64"]);

    const result = await packMuonApp({
      root,
      types: ["deb"],
      linuxSandbox: "setuid",
      environment: {
        ...process.env,
        PATH: `${binDirectory}:${process.env.PATH ?? ""}`,
      },
    });

    const [artifact] = result.artifacts;
    expect(artifact?.type).toBe("deb");
    await expect(readFile(logPath, "utf8")).resolves.toBe(
      `--root-owner-group\n--build\n${join(root, ".muon", "pack", "deb", "packed-sample-linux-amd64")}\n${artifact?.path}\n`,
    );

    const packageRoot = join(
      root,
      ".muon",
      "pack",
      "deb",
      "packed-sample-linux-amd64",
    );
    const installedDist = join(
      packageRoot,
      "usr",
      "lib",
      "packed-sample",
      "dist-muon/linux-amd64",
    );
    await expect(
      readFile(join(installedDist, "muon-install.json"), "utf8"),
    ).resolves.toBe(
      `${JSON.stringify(
        {
          type: "deb",
          packageName: "packed-sample",
          launcherPath: "/usr/bin/packed-sample",
          runtimeMode: "system-setuid",
          systemRuntimePath:
            "/var/lib/muon/apps/packed-sample/linux-amd64/runtime",
          privilegedPreparePath:
            "/usr/lib/packed-sample/dist-muon/linux-amd64/muon-runtime-helper",
        },
        undefined,
        2,
      )}\n`,
    );
    const helperStats = await stat(join(installedDist, "muon-runtime-helper"));
    expect(helperStats.mode & 0o7777).toBe(0o4755);
    await expect(
      readFile(join(packageRoot, "DEBIAN", "postinst"), "utf8"),
    ).resolves.toContain('chmod 4755 "$helper"');
    await expect(
      readFile(join(packageRoot, "DEBIAN", "postrm"), "utf8"),
    ).resolves.toContain('rm -rf "/var/lib/muon/apps/packed-sample"');
  });

  it("rejects setuid sandbox mode outside Linux deb packaging", async () => {
    const root = await createTemporaryDirectory("muon-pack-deb-setuid-reject-");
    const packageDirectory = await createFakeMuonPackageDist(
      root,
      allFakePackageTargets,
    );
    await writeViteProject(root, packageDirectory, allFakePackageTargets);

    await expect(
      packMuonApp({
        root,
        types: ["tar.gz"],
        targets: ["linux-amd64"],
        linuxSandbox: "setuid",
      }),
    ).rejects.toThrow("--linux-sandbox=setuid is supported only");

    await expect(
      packMuonApp({
        root,
        types: ["deb", "tar.gz"],
        targets: ["linux-amd64"],
        linuxSandbox: "setuid",
      }),
    ).rejects.toThrow("--linux-sandbox=setuid is supported only");

    await expect(
      packMuonApp({
        root,
        types: ["nsis"],
        targets: ["windows-amd64"],
        linuxSandbox: "setuid",
      }),
    ).rejects.toThrow("--linux-sandbox=setuid is supported only");
  });

  it("creates an NSIS script and invokes makensis for Windows targets", async () => {
    const root = await createTemporaryDirectory("muon-pack-nsis-");
    const packageDirectory = await createFakeMuonPackageDist(root, [
      "windows-amd64",
    ]);
    const binDirectory = join(root, "bin");
    const logPath = join(root, "makensis.log");
    await mkdir(binDirectory, { recursive: true });
    await mkdir(join(root, "artifacts", "nsis"), { recursive: true });
    await writeFile(join(root, "artifacts", "nsis", "stale.nsi"), "stale\n");
    await writeFakeTool(
      binDirectory,
      "makensis",
      `#!/usr/bin/env bash
set -euo pipefail
printf '%s\\n' "$@" > '${logPath}'
script_path="\${1:?}"
output_path="$(sed -n 's/^OutFile "\\(.*\\)"$/\\1/p' "$script_path")"
mkdir -p "$(dirname "$output_path")"
printf 'nsis\\n' > "$output_path"
`,
    );
    const iconPath = join(root, "icons", "setup.png");
    const generatedIconPath = join(
      root,
      ".muon",
      "pack",
      "nsis",
      "packed-sample-windows-amd64.ico",
    );
    await createWindowsIconFixture(iconPath);
    await writeViteProject(root, packageDirectory, ["windows-amd64"]);
    await writeFile(
      join(root, "muon.json"),
      `${JSON.stringify(
        {
          iconPath: "icons/setup.png",
          windows: {
            resource: {
              productName: "NSIS Product",
              fileDescription: "NSIS Installer",
              companyName: "NSIS Company",
              version: "9.8.7",
              copyright: "Copyright NSIS",
            },
          },
        },
        null,
        2,
      )}\n`,
    );

    const result = await packMuonApp({
      root,
      types: ["nsis"],
      environment: {
        ...process.env,
        PATH: `${binDirectory}:${process.env.PATH ?? ""}`,
      },
    });

    const [artifact] = result.artifacts;
    expect(artifact?.type).toBe("nsis");
    expect(artifact?.path).toBe(
      join(root, "artifacts", "packed-sample-1.2.3-amd64-setup.exe"),
    );
    await expect(readFile(artifact?.path ?? "", "utf8")).resolves.toBe(
      "nsis\n",
    );
    await expect(readFile(logPath, "utf8")).resolves.toBe(
      `${join(root, ".muon", "pack", "nsis", "packed-sample-windows-amd64.nsi")}\n`,
    );
    await expect(readdir(join(root, "artifacts"))).resolves.toEqual([
      "packed-sample-1.2.3-amd64-setup.exe",
    ]);
    await expect(exists(join(root, "artifacts", "nsis"))).resolves.toBe(false);
    const nsisScript = await readFile(
      join(root, ".muon", "pack", "nsis", "packed-sample-windows-amd64.nsi"),
      "utf8",
    );
    expect(nsisScript).toContain('Name "packed-sample (amd64)"');
    expect(nsisScript).toContain(
      'InstallDir "$LOCALAPPDATA\\Programs\\packed-sample-amd64"',
    );
    expect(nsisScript).toContain("RequestExecutionLevel user");
    expect(nsisScript).toContain(`Icon "${generatedIconPath}"`);
    expect(nsisScript).toContain(`UninstallIcon "${generatedIconPath}"`);
    await expect(readFile(generatedIconPath)).resolves.toEqual(
      await createWindowsIconBufferFromPngData(
        await readFile(iconPath),
        iconPath,
      ),
    );
    expect(nsisScript).toContain('VIProductVersion "9.8.7.0"');
    expect(nsisScript).toContain('VIFileVersion "9.8.7.0"');
    expect(nsisScript).toContain(
      'VIAddVersionKey /LANG=1033 "CompanyName" "NSIS Company"',
    );
    expect(nsisScript).toContain(
      'VIAddVersionKey /LANG=1033 "FileDescription" "NSIS Installer"',
    );
    expect(nsisScript).toContain(
      'VIAddVersionKey /LANG=1033 "ProductName" "NSIS Product"',
    );
    expect(nsisScript).toContain(
      'VIAddVersionKey /LANG=1033 "LegalCopyright" "Copyright NSIS"',
    );
    expect(nsisScript).toContain("ShowInstDetails nevershow");
    expect(nsisScript).toContain("AutoCloseWindow true");
    expect(nsisScript).toContain("Page instfiles");
    expect(
      nsisScript.split("\n").filter((line) => line.startsWith("Page ")),
    ).toEqual(["Page instfiles"]);
    expect(nsisScript).toContain(
      '  CreateShortCut "$SMPROGRAMS\\packed-sample (amd64).lnk" "$INSTDIR\\packed-sample.exe"',
    );
    expect(nsisScript).toContain(
      '  WriteRegStr HKCU "Software\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Uninstall\\\\scope.packed-sample.amd64" "DisplayName" "packed-sample (amd64)"',
    );
    expect(nsisScript).toContain(
      '  Delete "$SMPROGRAMS\\packed-sample (amd64).lnk"',
    );
    expect(nsisScript).toContain(
      '  DeleteRegKey HKCU "Software\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Uninstall\\\\scope.packed-sample.amd64"',
    );
    expect(nsisScript).toContain(
      '  RMDir /r "$LOCALAPPDATA\\scope.packed-sample.amd64"',
    );
    expect(nsisScript).toContain("Function .onInstSuccess");
    expect(nsisScript).toContain("  IfSilent +3");
    expect(nsisScript).toContain('  SetOutPath "$INSTDIR"');
    expect(nsisScript).toContain(
      '  Exec "$\\"$INSTDIR\\packed-sample.exe$\\""',
    );
    expect(nsisScript).toContain("FunctionEnd");
  });

  it("signs Windows distribution executables and NSIS artifacts from muon config", async () => {
    const root = await createTemporaryDirectory("muon-pack-windows-signing-");
    const packageDirectory = await createFakeMuonPackageDist(root, [
      "windows-amd64",
    ]);
    const binDirectory = join(root, "bin");
    const uninstallerPath = join(root, "fake-uninstall.exe");
    await mkdir(binDirectory, { recursive: true });
    const signer = await createFakeSigningCommand(
      binDirectory,
      root,
      "fake-sign",
    );
    await writeFakeTool(
      binDirectory,
      "makensis",
      `#!/usr/bin/env bash
set -euo pipefail
script_path="\${1:?}"
output_path="$(sed -n 's/^OutFile "\\(.*\\)"$/\\1/p' "$script_path")"
finalize_command="$(sed -n "s/^!finalize '\\(.*\\)' = 0$/\\1/p" "$script_path")"
uninst_finalize_command="$(sed -n "s/^!uninstfinalize '\\(.*\\)' = 0$/\\1/p" "$script_path")"
mkdir -p "$(dirname "$output_path")"
printf 'uninstaller\\n' > '${uninstallerPath}'
printf 'nsis\\n' > "$output_path"
if [ -n "$uninst_finalize_command" ]; then
  eval "\${uninst_finalize_command//%1/${uninstallerPath}}"
fi
if [ -n "$finalize_command" ]; then
  eval "\${finalize_command//%1/$output_path}"
fi
`,
    );
    await writeViteProject(root, packageDirectory, ["windows-amd64"]);
    await writeFile(
      join(root, "muon.json"),
      `${JSON.stringify(
        {
          windows: {
            codeSigning: {
              command: signer.command,
              args: signer.args,
            },
          },
        },
        null,
        2,
      )}\n`,
    );

    const result = await packMuonApp({
      root,
      types: ["nsis"],
      packageVersion: "4.5.6",
      environment: {
        ...process.env,
        PATH: `${binDirectory}:${process.env.PATH ?? ""}`,
      },
    });

    const [artifact] = result.artifacts;
    const log = await readSigningLog(signer.logPath);
    expect(log).toEqual([
      {
        kind: "runtime",
        target: "windows-amd64",
        path: join(root, "dist-muon/windows-amd64", "muon-core.exe"),
      },
      {
        kind: "launcher",
        target: "windows-amd64",
        path: join(root, "dist-muon/windows-amd64", "packed-sample.exe"),
      },
      {
        kind: "runtime",
        target: "windows-amd64",
        path: join(root, "dist-muon/windows-amd64", "muon-core.exe"),
      },
      {
        kind: "launcher",
        target: "windows-amd64",
        path: join(root, "dist-muon/windows-amd64", "packed-sample.exe"),
      },
      {
        kind: "nsisUninstaller",
        target: "windows-amd64",
        path: uninstallerPath,
      },
      {
        kind: "nsisInstaller",
        target: "windows-amd64",
        path: artifact?.path ?? "",
      },
    ]);
    const nsisScript = await readFile(
      join(root, ".muon", "pack", "nsis", "packed-sample-windows-amd64.nsi"),
      "utf8",
    );
    expect(nsisScript).toContain("!finalize");
    expect(nsisScript).toContain("!uninstfinalize");
  });

  it("uses the package version override as the NSIS Windows resource version fallback", async () => {
    const root = await createTemporaryDirectory(
      "muon-pack-nsis-package-version-",
    );
    const packageDirectory = await createFakeMuonPackageDist(root, [
      "windows-amd64",
    ]);
    await writeViteProject(root, packageDirectory, ["windows-amd64"]);

    const result = await packMuonApp({
      root,
      types: ["nsis"],
      packageVersion: "4.5.6",
      environment: await createFakePackagingToolEnvironment(root),
    });

    const [artifact] = result.artifacts;
    expect(artifact?.path).toBe(
      join(root, "artifacts", "packed-sample-4.5.6-amd64-setup.exe"),
    );
    const nsisScript = await readFile(
      join(root, ".muon", "pack", "nsis", "packed-sample-windows-amd64.nsi"),
      "utf8",
    );
    expect(nsisScript).toContain('"DisplayVersion" "4.5.6"');
    expect(nsisScript).toContain('VIProductVersion "4.5.6.0"');
    expect(nsisScript).toContain('VIFileVersion "4.5.6.0"');
    expect(nsisScript).toContain(
      'VIAddVersionKey /LANG=1033 "FileVersion" "4.5.6"',
    );
    expect(nsisScript).toContain(
      'VIAddVersionKey /LANG=1033 "ProductVersion" "4.5.6"',
    );
  });

  it("keeps explicit Windows resource versions ahead of the package version override", async () => {
    const root = await createTemporaryDirectory(
      "muon-pack-nsis-windows-version-precedence-",
    );
    const packageDirectory = await createFakeMuonPackageDist(root, [
      "windows-amd64",
    ]);
    await writeViteProject(root, packageDirectory, ["windows-amd64"]);
    await writeFile(
      join(root, "muon.json"),
      `${JSON.stringify(
        {
          windows: {
            resource: {
              version: "9.8.7",
            },
          },
        },
        null,
        2,
      )}\n`,
    );

    const result = await packMuonApp({
      root,
      types: ["nsis"],
      packageVersion: "4.5.6",
      environment: await createFakePackagingToolEnvironment(root),
    });

    const [artifact] = result.artifacts;
    expect(artifact?.path).toBe(
      join(root, "artifacts", "packed-sample-4.5.6-amd64-setup.exe"),
    );
    const nsisScript = await readFile(
      join(root, ".muon", "pack", "nsis", "packed-sample-windows-amd64.nsi"),
      "utf8",
    );
    expect(nsisScript).toContain('"DisplayVersion" "4.5.6"');
    expect(nsisScript).toContain('VIProductVersion "9.8.7.0"');
    expect(nsisScript).toContain('VIFileVersion "9.8.7.0"');
    expect(nsisScript).toContain(
      'VIAddVersionKey /LANG=1033 "FileVersion" "9.8.7"',
    );
    expect(nsisScript).toContain(
      'VIAddVersionKey /LANG=1033 "ProductVersion" "9.8.7"',
    );
  });

  it("packages only Windows targets when NSIS is requested without explicit targets", async () => {
    const root = await createTemporaryDirectory("muon-pack-nsis-targets-");
    const packageDirectory = await createFakeMuonPackageDist(
      root,
      allFakePackageTargets,
    );
    await writeViteProject(root, packageDirectory, allFakePackageTargets);

    const result = await packMuonApp({
      root,
      types: ["nsis"],
      environment: await createFakePackagingToolEnvironment(root),
    });

    expect(result.targets.map((target) => target.target)).toEqual([
      "windows-i686",
      "windows-amd64",
    ]);
    expect(getArtifactSummary(result.artifacts)).toEqual([
      "nsis:windows-amd64:packed-sample-1.2.3-amd64-setup.exe",
      "nsis:windows-i686:packed-sample-1.2.3-i686-setup.exe",
    ]);
    const i686Script = await readFile(
      join(root, ".muon", "pack", "nsis", "packed-sample-windows-i686.nsi"),
      "utf8",
    );
    const amd64Script = await readFile(
      join(root, ".muon", "pack", "nsis", "packed-sample-windows-amd64.nsi"),
      "utf8",
    );
    expect(i686Script).toContain('Name "packed-sample (i686)"');
    expect(i686Script).toContain(
      'InstallDir "$LOCALAPPDATA\\Programs\\packed-sample-i686"',
    );
    expect(i686Script).toContain(
      "Software\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Uninstall\\\\scope.packed-sample.i686",
    );
    expect(i686Script).toContain(
      '  RMDir /r "$LOCALAPPDATA\\scope.packed-sample.i686"',
    );
    expect(amd64Script).toContain('Name "packed-sample (amd64)"');
    expect(amd64Script).toContain(
      'InstallDir "$LOCALAPPDATA\\Programs\\packed-sample-amd64"',
    );
    expect(amd64Script).toContain(
      "Software\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Uninstall\\\\scope.packed-sample.amd64",
    );
    expect(amd64Script).toContain(
      '  RMDir /r "$LOCALAPPDATA\\scope.packed-sample.amd64"',
    );
    await expect(exists(join(root, "dist-muon/linux-amd64"))).resolves.toBe(
      false,
    );
  });

  it("packages only Linux targets when deb is requested without explicit targets", async () => {
    const root = await createTemporaryDirectory("muon-pack-deb-targets-");
    const packageDirectory = await createFakeMuonPackageDist(
      root,
      allFakePackageTargets,
    );
    await writeViteProject(root, packageDirectory, allFakePackageTargets);

    const result = await packMuonApp({
      root,
      types: ["deb"],
      environment: await createFakePackagingToolEnvironment(root),
    });

    expect(result.targets.map((target) => target.target)).toEqual([
      "linux-amd64",
      "linux-armhf",
      "linux-arm64",
    ]);
    expect(getArtifactSummary(result.artifacts)).toEqual([
      "deb:linux-amd64:packed-sample-1.2.3-amd64.deb",
      "deb:linux-arm64:packed-sample-1.2.3-arm64.deb",
      "deb:linux-armhf:packed-sample-1.2.3-armhf.deb",
    ]);
    await expect(exists(join(root, "dist-muon/windows-amd64"))).resolves.toBe(
      false,
    );
  });

  it("uses every valid target and package type combination by default", async () => {
    const root = await createTemporaryDirectory("muon-pack-default-");
    const packageDirectory = await createFakeMuonPackageDist(
      root,
      allFakePackageTargets,
    );
    await writeViteProject(root, packageDirectory, allFakePackageTargets);

    const result = await packMuonApp({
      root,
      environment: await createFakePackagingToolEnvironment(root),
    });

    expect(result.targets.map((target) => target.target)).toEqual([
      "linux-amd64",
      "linux-armhf",
      "linux-arm64",
      "windows-i686",
      "windows-amd64",
    ]);
    expect(getArtifactSummary(result.artifacts)).toEqual([
      "deb:linux-amd64:packed-sample-1.2.3-amd64.deb",
      "deb:linux-arm64:packed-sample-1.2.3-arm64.deb",
      "deb:linux-armhf:packed-sample-1.2.3-armhf.deb",
      "nsis:windows-amd64:packed-sample-1.2.3-amd64-setup.exe",
      "nsis:windows-i686:packed-sample-1.2.3-i686-setup.exe",
      "tar.gz:linux-amd64:packed-sample-1.2.3-linux-amd64.tar.gz",
      "tar.gz:linux-arm64:packed-sample-1.2.3-linux-arm64.tar.gz",
      "tar.gz:linux-armhf:packed-sample-1.2.3-linux-armhf.tar.gz",
      "zip:windows-amd64:packed-sample-1.2.3-windows-amd64.zip",
      "zip:windows-i686:packed-sample-1.2.3-windows-i686.zip",
    ]);
  });

  it("packages valid combinations for complete and partial target selectors", async () => {
    const cases: {
      selector: string;
      expectedTargets: readonly MuonBuildTarget[];
      expectedArtifacts: readonly string[];
    }[] = [
      {
        selector: "windows-i686",
        expectedTargets: ["windows-i686"],
        expectedArtifacts: [
          "nsis:windows-i686:packed-sample-1.2.3-i686-setup.exe",
          "zip:windows-i686:packed-sample-1.2.3-windows-i686.zip",
        ],
      },
      {
        selector: "windows",
        expectedTargets: ["windows-i686", "windows-amd64"],
        expectedArtifacts: [
          "nsis:windows-amd64:packed-sample-1.2.3-amd64-setup.exe",
          "nsis:windows-i686:packed-sample-1.2.3-i686-setup.exe",
          "zip:windows-amd64:packed-sample-1.2.3-windows-amd64.zip",
          "zip:windows-i686:packed-sample-1.2.3-windows-i686.zip",
        ],
      },
      {
        selector: "linux",
        expectedTargets: ["linux-amd64", "linux-armhf", "linux-arm64"],
        expectedArtifacts: [
          "deb:linux-amd64:packed-sample-1.2.3-amd64.deb",
          "deb:linux-arm64:packed-sample-1.2.3-arm64.deb",
          "deb:linux-armhf:packed-sample-1.2.3-armhf.deb",
          "tar.gz:linux-amd64:packed-sample-1.2.3-linux-amd64.tar.gz",
          "tar.gz:linux-arm64:packed-sample-1.2.3-linux-arm64.tar.gz",
          "tar.gz:linux-armhf:packed-sample-1.2.3-linux-armhf.tar.gz",
        ],
      },
      {
        selector: "amd64",
        expectedTargets: ["linux-amd64", "windows-amd64"],
        expectedArtifacts: [
          "deb:linux-amd64:packed-sample-1.2.3-amd64.deb",
          "nsis:windows-amd64:packed-sample-1.2.3-amd64-setup.exe",
          "tar.gz:linux-amd64:packed-sample-1.2.3-linux-amd64.tar.gz",
          "zip:windows-amd64:packed-sample-1.2.3-windows-amd64.zip",
        ],
      },
      {
        selector: "arm64",
        expectedTargets: ["linux-arm64"],
        expectedArtifacts: [
          "deb:linux-arm64:packed-sample-1.2.3-arm64.deb",
          "tar.gz:linux-arm64:packed-sample-1.2.3-linux-arm64.tar.gz",
        ],
      },
    ];

    for (const entry of cases) {
      const root = await createTemporaryDirectory(
        `muon-pack-target-${entry.selector}-`,
      );
      const packageDirectory = await createFakeMuonPackageDist(
        root,
        allFakePackageTargets,
      );
      await writeViteProject(root, packageDirectory, allFakePackageTargets);

      const result = await packMuonApp({
        root,
        targets: [entry.selector],
        environment: await createFakePackagingToolEnvironment(root),
      });

      expect(result.targets.map((target) => target.target)).toEqual(
        entry.expectedTargets,
      );
      expect(getArtifactSummary(result.artifacts)).toEqual([
        ...entry.expectedArtifacts,
      ]);
    }
  });

  it("rejects package requests with no valid type and target combination", async () => {
    const root = await createTemporaryDirectory("muon-pack-invalid-");
    const packageDirectory = await createFakeMuonPackageDist(root, [
      "linux-amd64",
    ]);
    await writeViteProject(root, packageDirectory, ["linux-amd64"]);

    await expect(
      packMuonApp({
        root,
        types: ["nsis"],
        targets: ["linux"],
      }),
    ).rejects.toThrow("No valid muon pack target and type combinations");
  });

  it("rejects zip packages for Linux targets", async () => {
    const root = await createTemporaryDirectory("muon-pack-linux-zip-");
    const packageDirectory = await createFakeMuonPackageDist(root, [
      "linux-amd64",
    ]);
    await writeViteProject(root, packageDirectory, ["linux-amd64"]);

    await expect(
      packMuonApp({
        root,
        types: ["zip"],
        targets: ["linux"],
      }),
    ).rejects.toThrow("No valid muon pack target and type combinations");
  });

  it("rejects tar.gz packages for Windows targets", async () => {
    const root = await createTemporaryDirectory("muon-pack-windows-tar-gz-");
    const packageDirectory = await createFakeMuonPackageDist(root, [
      "windows-amd64",
    ]);
    await writeViteProject(root, packageDirectory, ["windows-amd64"]);

    await expect(
      packMuonApp({
        root,
        types: ["tar.gz"],
        targets: ["windows"],
      }),
    ).rejects.toThrow("No valid muon pack target and type combinations");
  });

  it("rejects unknown pack target selectors", async () => {
    const root = await createTemporaryDirectory("muon-pack-unknown-target-");
    const packageDirectory = await createFakeMuonPackageDist(root, [
      "linux-amd64",
    ]);
    await writeViteProject(root, packageDirectory, ["linux-amd64"]);

    await expect(
      packMuonApp({
        root,
        types: ["zip"],
        targets: ["linux64"],
      }),
    ).rejects.toThrow("Unsupported muon pack target selector: linux64");
  });

  it("rejects empty explicit package types", async () => {
    const root = await createTemporaryDirectory("muon-pack-empty-type-");
    const packageDirectory = await createFakeMuonPackageDist(root, [
      "linux-amd64",
    ]);
    await writeViteProject(root, packageDirectory, ["linux-amd64"]);

    await expect(
      packMuonApp({
        root,
        types: [],
      }),
    ).rejects.toThrow("Specify at least one package type with --type");
  });

  it("requires package version metadata", async () => {
    const root = await createTemporaryDirectory("muon-pack-metadata-");
    const packageDirectory = await createFakeMuonPackageDist(root, [
      "linux-amd64",
    ]);
    await writeViteProject(root, packageDirectory, ["linux-amd64"]);
    const packageJson = JSON.parse(
      await readFile(join(root, "package.json"), "utf8"),
    ) as Record<string, unknown>;
    delete packageJson.version;
    await writeFile(
      join(root, "package.json"),
      `${JSON.stringify(packageJson)}\n`,
    );

    await expect(
      packMuonApp({
        root,
        types: ["zip"],
      }),
    ).rejects.toThrow("package.json version is required");
  });
});
