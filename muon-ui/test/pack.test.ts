// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import { execFile } from "node:child_process";
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
import { join, resolve } from "node:path";
import { pathToFileURL } from "node:url";
import { promisify } from "node:util";
import { inflateRawSync } from "node:zlib";

import { afterEach, describe, expect, it } from "vitest";

import {
  createMuonBootstrapEmbeddedConfigSlot,
  createMuonEmbeddedConfigSlot,
} from "../src/embed-config.js";
import { packMuonApp } from "../src/pack.js";
import type { MuonBuildTarget } from "../src/build.js";

const execFileAsync = promisify(execFile);
const cleanupDirectories: string[] = [];

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
    bootstrapExecutableName: string;
  }
> = {
  linux64: {
    runtimeExecutableName: "muon-core",
    uiLibraryName: "libmuon-ui.so",
    cardioLibraryName: "libcardio.so",
    bootstrapExecutableName: "muon-bootstrap",
  },
  linuxarm: {
    runtimeExecutableName: "muon-core",
    uiLibraryName: "libmuon-ui.so",
    cardioLibraryName: "libcardio.so",
    bootstrapExecutableName: "muon-bootstrap",
  },
  linuxarm64: {
    runtimeExecutableName: "muon-core",
    uiLibraryName: "libmuon-ui.so",
    cardioLibraryName: "libcardio.so",
    bootstrapExecutableName: "muon-bootstrap",
  },
  windows32: {
    runtimeExecutableName: "muon-core.exe",
    uiLibraryName: "libmuon-ui.dll",
    cardioLibraryName: "libcardio.dll",
    bootstrapExecutableName: "muon-bootstrap.exe",
  },
  windows64: {
    runtimeExecutableName: "muon-core.exe",
    uiLibraryName: "libmuon-ui.dll",
    cardioLibraryName: "libcardio.dll",
    bootstrapExecutableName: "muon-bootstrap.exe",
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
    await writeFile(join(runtimeDirectory, "CREDITS.md"), "notices\n");
    await writeExecutable(
      join(nativeDirectory, descriptor.bootstrapExecutableName),
      Buffer.concat([
        Buffer.from("bootstrap prefix\n"),
        createMuonBootstrapEmbeddedConfigSlot(),
        Buffer.from("\nbootstrap suffix\n"),
      ]),
    );
  }
  return packageDirectory;
};

const writeViteProject = async (
  root: string,
  packageDirectory: string,
  buildTargets: readonly string[],
): Promise<void> => {
  const vitePluginUrl = pathToFileURL(resolve("dist", "vite.mjs")).href;
  await writeFile(
    join(root, "package.json"),
    `${JSON.stringify(
      {
        name: "@scope/packed-sample",
        version: "1.2.3",
        description: "Packed sample",
        author: "Muon Tester",
        type: "module",
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
    `${JSON.stringify({ network: { allow: ["asset://main/**"] } }, null, 2)}\n`,
  );
  await writeFile(
    join(root, "vite.config.mjs"),
    [
      `import muon from ${JSON.stringify(vitePluginUrl)};`,
      "export default {",
      "  build: { outDir: 'web-dist' },",
      "  plugins: [",
      `    muon({ build: { targets: ${JSON.stringify(buildTargets)}, packageDirectory: ${JSON.stringify(packageDirectory)} } }),`,
      "  ],",
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
      return (method === 0 ? data : inflateRawSync(data)).toString("utf8");
    }
    cursor += 46 + nameLength + extraLength + commentLength;
  }
  throw new Error(`ZIP entry was not found: ${entryName}`);
};

const writeFakeTool = async (
  binDirectory: string,
  name: string,
  script: string,
): Promise<void> => {
  await writeExecutable(join(binDirectory, name), script);
};

afterEach(async () => {
  for (const directory of cleanupDirectories.splice(0)) {
    await rm(directory, { recursive: true, force: true });
  }
});

describe("muon pack", () => {
  it("builds Vite output once and packages the configured target as a ZIP", async () => {
    const root = await createTemporaryDirectory("muon-pack-zip-");
    const packageDirectory = await createFakeMuonPackageDist(root, ["linux64"]);
    await writeViteProject(root, packageDirectory, ["linux-amd64"]);

    const result = await packMuonApp({
      root,
      types: ["zip"],
    });

    const [artifact] = result.artifacts;
    expect(result.targets.map((target) => target.target)).toEqual(["linux64"]);
    expect(artifact?.type).toBe("zip");
    expect(artifact?.target).toBe("linux64");
    expect(artifact?.path).toBe(
      join(root, "artifacts", "packed-sample-linux64.zip"),
    );
    await expect(exists(join(root, "dist-muon-linux-amd64"))).resolves.toBe(
      true,
    );
    await expect(readZipEntryNames(artifact?.path ?? "")).resolves.toContain(
      "dist-muon-linux-amd64/assets.zip",
    );
    await expect(
      readZipTextEntry(
        artifact?.path ?? "",
        "dist-muon-linux-amd64/CREDITS.md",
      ),
    ).resolves.toBe("notices\n");
  });

  it("creates a deb package tree and invokes dpkg-deb for Linux targets", async () => {
    const root = await createTemporaryDirectory("muon-pack-deb-");
    const packageDirectory = await createFakeMuonPackageDist(root, ["linux64"]);
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
package_root="\${2:?}"
output_path="\${3:?}"
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
      join(root, "artifacts", "packed-sample_1.2.3_amd64.deb"),
    );
    await expect(readFile(artifact?.path ?? "", "utf8")).resolves.toBe("deb\n");
    await expect(readFile(logPath, "utf8")).resolves.toBe(
      `--build\n${join(root, ".muon", "pack", "deb", "packed-sample-linux64")}\n${artifact?.path}\n`,
    );
    await expect(readdir(join(root, "artifacts"))).resolves.toEqual([
      "packed-sample_1.2.3_amd64.deb",
    ]);
    await expect(exists(join(root, "artifacts", "deb"))).resolves.toBe(false);
    await expect(
      readFile(join(root, "deb-files.txt"), "utf8"),
    ).resolves.toContain("/usr/bin/packed-sample");
    await expect(
      readFile(join(root, "deb-files.txt"), "utf8"),
    ).resolves.toContain(
      "/usr/lib/packed-sample/dist-muon-linux-amd64/packed-sample",
    );
  });

  it("creates an NSIS script and invokes makensis for Windows targets", async () => {
    const root = await createTemporaryDirectory("muon-pack-nsis-");
    const packageDirectory = await createFakeMuonPackageDist(root, [
      "windows64",
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
    await writeViteProject(root, packageDirectory, ["windows-amd64"]);

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
      join(root, "artifacts", "packed-sample-1.2.3-windows64-setup.exe"),
    );
    await expect(readFile(artifact?.path ?? "", "utf8")).resolves.toBe(
      "nsis\n",
    );
    await expect(readFile(logPath, "utf8")).resolves.toBe(
      `${join(root, ".muon", "pack", "nsis", "packed-sample-windows64.nsi")}\n`,
    );
    await expect(readdir(join(root, "artifacts"))).resolves.toEqual([
      "packed-sample-1.2.3-windows64-setup.exe",
    ]);
    await expect(exists(join(root, "artifacts", "nsis"))).resolves.toBe(false);
    await expect(
      readFile(
        join(root, ".muon", "pack", "nsis", "packed-sample-windows64.nsi"),
        "utf8",
      ),
    ).resolves.toContain("RequestExecutionLevel user");
  });

  it("rejects package types that do not support the resolved target", async () => {
    const root = await createTemporaryDirectory("muon-pack-invalid-");
    const packageDirectory = await createFakeMuonPackageDist(root, [
      "windows64",
    ]);
    await writeViteProject(root, packageDirectory, ["windows-amd64"]);

    await expect(
      packMuonApp({
        root,
        types: ["deb"],
      }),
    ).rejects.toThrow("deb packaging supports only Linux targets");
  });

  it("requires package version metadata", async () => {
    const root = await createTemporaryDirectory("muon-pack-metadata-");
    const packageDirectory = await createFakeMuonPackageDist(root, ["linux64"]);
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
