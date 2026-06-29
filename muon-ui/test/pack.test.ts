// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

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
import { basename, join, resolve } from "node:path";
import { pathToFileURL } from "node:url";
import { promisify } from "node:util";
import { inflateRawSync } from "node:zlib";

import { afterEach, describe, expect, it } from "vitest";
import { createTarExtractor } from "tar-vern";

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
  "linux-amd64": {
    runtimeExecutableName: "muon-core",
    uiLibraryName: "libmuon-ui.so",
    cardioLibraryName: "libcardio.so",
    bootstrapExecutableName: "muon-bootstrap",
  },
  "linux-armhf": {
    runtimeExecutableName: "muon-core",
    uiLibraryName: "libmuon-ui.so",
    cardioLibraryName: "libcardio.so",
    bootstrapExecutableName: "muon-bootstrap",
  },
  "linux-arm64": {
    runtimeExecutableName: "muon-core",
    uiLibraryName: "libmuon-ui.so",
    cardioLibraryName: "libcardio.so",
    bootstrapExecutableName: "muon-bootstrap",
  },
  "windows-i686": {
    runtimeExecutableName: "muon-core.exe",
    uiLibraryName: "libmuon-ui.dll",
    cardioLibraryName: "libcardio.dll",
    bootstrapExecutableName: "muon-bootstrap.exe",
  },
  "windows-amd64": {
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
        author: "Muon Tester",
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
        author: "Muon Tester",
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
  for await (const entry of createTarExtractor(
    createReadStream(archivePath),
    "gzip",
  )) {
    if (entry.kind === "file" && entry.path === entryName) {
      return await entry.getContent("string");
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

const createWindowsIconFixture = async (
  root: string,
  iconPath: string,
): Promise<void> => {
  await execFileAsync(process.execPath, [
    resolve(
      "..",
      "muon-prepare",
      "scripts",
      "create-windows-resource-fixture.mjs",
    ),
    join(root, "fixture.exe"),
    iconPath,
  ]);
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
output_path="\${3:?}"
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
    await expect(exists(join(root, "dist-muon-linux-amd64"))).resolves.toBe(
      true,
    );
    await expect(readTarGzEntryNames(artifact?.path ?? "")).resolves.toContain(
      "dist-muon-linux-amd64/assets.zip",
    );
    await expect(
      readTarGzTextEntry(
        artifact?.path ?? "",
        "dist-muon-linux-amd64/CREDITS.md",
      ),
    ).resolves.toBe("notices\n");
  });

  it("packages non-Vite assets without running Vite when no Muon plugin is configured", async () => {
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
        join(root, "dist-muon-linux-amd64", "assets.zip"),
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
    await expect(readZipEntryNames(artifact?.path ?? "")).resolves.toContain(
      "dist-muon-windows-amd64/assets.zip",
    );
    await expect(
      readZipTextEntry(
        artifact?.path ?? "",
        "dist-muon-windows-amd64/CREDITS.md",
      ),
    ).resolves.toBe("notices\n");
  });

  it("rejects package builds when the Vite Muon plugin disables Muon builds", async () => {
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
    ).rejects.toThrow("Muon build is disabled by muon({ build: false })");
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
      join(root, "artifacts", "packed-sample-1.2.3-amd64.deb"),
    );
    await expect(readFile(artifact?.path ?? "", "utf8")).resolves.toBe("deb\n");
    await expect(readFile(logPath, "utf8")).resolves.toBe(
      `--build\n${join(root, ".muon", "pack", "deb", "packed-sample-linux-amd64")}\n${artifact?.path}\n`,
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
      "/usr/lib/packed-sample/dist-muon-linux-amd64/packed-sample",
    );
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
    const iconPath = join(root, "icons", "setup.ico");
    await createWindowsIconFixture(root, iconPath);
    await writeViteProject(root, packageDirectory, ["windows-amd64"]);
    await writeFile(
      join(root, "muon.json"),
      `${JSON.stringify(
        {
          windows: {
            resource: {
              iconPath: "icons/setup.ico",
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
    expect(nsisScript).toContain("RequestExecutionLevel user");
    expect(nsisScript).toContain(`Icon "${iconPath}"`);
    expect(nsisScript).toContain(`UninstallIcon "${iconPath}"`);
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
    expect(nsisScript).toContain("Function .onInstSuccess");
    expect(nsisScript).toContain("  IfSilent +3");
    expect(nsisScript).toContain('  SetOutPath "$INSTDIR"');
    expect(nsisScript).toContain(
      '  Exec "$\\"$INSTDIR\\packed-sample.exe$\\""',
    );
    expect(nsisScript).toContain("FunctionEnd");
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
    await expect(exists(join(root, "dist-muon-linux-amd64"))).resolves.toBe(
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
    await expect(exists(join(root, "dist-muon-windows-amd64"))).resolves.toBe(
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
