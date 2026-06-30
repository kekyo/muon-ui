// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import { execFile } from "node:child_process";
import { createHash } from "node:crypto";
import {
  access,
  chmod,
  mkdir,
  mkdtemp,
  readFile,
  rm,
  writeFile,
} from "node:fs/promises";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { pathToFileURL } from "node:url";
import { promisify } from "node:util";
import { inflateRawSync } from "node:zlib";

import { afterEach, describe, expect, it } from "vitest";
import { build as viteBuild } from "vite";

import {
  createMuonBootstrapEmbeddedConfigSlot,
  createMuonEmbeddedConfigSlot,
  findMuonBootstrapEmbeddedConfigSlot,
  findMuonEmbeddedConfigSlot,
} from "../src/embed-config.js";
import { buildMuonApp, type MuonBuildTarget } from "../src/build.js";
import { createWindowsIconBufferFromPngData } from "../src/windows-icon.js";
import muon from "../src/vite.js";

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
  slot: Buffer,
  prefix: string,
): Promise<void> => {
  await mkdir(join(path, ".."), { recursive: true });
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

const fakePackageTargetDescriptors: Record<
  MuonBuildTarget,
  {
    runtimeExecutableName: string;
    uiLibraryName: string;
    cardioLibraryName: string;
    bootstrapExecutableName: string;
    mingwRuntimeFiles: readonly string[];
  }
> = {
  "linux-amd64": {
    runtimeExecutableName: "muon-core",
    uiLibraryName: "libmuon-ui.so",
    cardioLibraryName: "libcardio.so",
    bootstrapExecutableName: "muon-bootstrap",
    mingwRuntimeFiles: [],
  },
  "linux-armhf": {
    runtimeExecutableName: "muon-core",
    uiLibraryName: "libmuon-ui.so",
    cardioLibraryName: "libcardio.so",
    bootstrapExecutableName: "muon-bootstrap",
    mingwRuntimeFiles: [],
  },
  "linux-arm64": {
    runtimeExecutableName: "muon-core",
    uiLibraryName: "libmuon-ui.so",
    cardioLibraryName: "libcardio.so",
    bootstrapExecutableName: "muon-bootstrap",
    mingwRuntimeFiles: [],
  },
  "windows-i686": {
    runtimeExecutableName: "muon-core.exe",
    uiLibraryName: "libmuon-ui.dll",
    cardioLibraryName: "libcardio.dll",
    bootstrapExecutableName: "muon-bootstrap.exe",
    mingwRuntimeFiles: [
      "libgcc_s_dw2-1.dll",
      "libstdc++-6.dll",
      "libwinpthread-1.dll",
    ],
  },
  "windows-amd64": {
    runtimeExecutableName: "muon-core.exe",
    uiLibraryName: "libmuon-ui.dll",
    cardioLibraryName: "libcardio.dll",
    bootstrapExecutableName: "muon-bootstrap.exe",
    mingwRuntimeFiles: [
      "libgcc_s_seh-1.dll",
      "libstdc++-6.dll",
      "libwinpthread-1.dll",
    ],
  },
};

const createFakeMuonPackageDistForTargets = async (
  root: string,
  targets: readonly MuonBuildTarget[],
): Promise<string> => {
  const packageDirectory = join(root, "package-dist");
  for (const target of targets) {
    const descriptor = fakePackageTargetDescriptors[target];
    const runtimeDirectory = join(packageDirectory, "runtime", target);
    const nativeDirectory = join(packageDirectory, "native", target);
    await mkdir(runtimeDirectory, { recursive: true });
    await mkdir(nativeDirectory, { recursive: true });
    await writeExecutable(
      join(runtimeDirectory, descriptor.runtimeExecutableName),
      createMuonEmbeddedConfigSlot(),
      "core",
    );
    await writeFile(join(runtimeDirectory, descriptor.uiLibraryName), "ui\n");
    await writeFile(
      join(runtimeDirectory, descriptor.cardioLibraryName),
      "cardio\n",
    );
    for (const fileName of descriptor.mingwRuntimeFiles) {
      await writeFile(
        join(runtimeDirectory, fileName),
        `${target} ${fileName}\n`,
      );
    }
    await writeFile(join(runtimeDirectory, "CREDITS.md"), "notices\n");
    await writeFile(join(runtimeDirectory, "libcef.so"), "cef\n");
    await writeFile(join(runtimeDirectory, "muon.json"), "{}\n");
    await mkdir(join(runtimeDirectory, "assets"), { recursive: true });
    await writeFile(join(runtimeDirectory, "assets", "debug.txt"), "debug\n");
    await writeExecutable(
      join(nativeDirectory, descriptor.bootstrapExecutableName),
      createMuonBootstrapEmbeddedConfigSlot(),
      "bootstrap",
    );
  }
  return packageDirectory;
};

const createFakeMuonPackageDist = async (root: string): Promise<string> =>
  await createFakeMuonPackageDistForTargets(root, ["linux-amd64"]);

interface WindowsIconBuildProject {
  root: string;
  packageDirectory: string;
}

const createWindowsIconBuildProject = async (
  prefix: string,
  packageName: string,
): Promise<WindowsIconBuildProject> => {
  const root = await createTemporaryDirectory(prefix);
  const packageDirectory = await createFakeMuonPackageDist(root);
  await writeFile(
    join(root, "package.json"),
    `${JSON.stringify({ name: packageName }, null, 2)}\n`,
  );
  await mkdir(join(root, "assets"), { recursive: true });
  await writeFile(join(root, "assets", "index.html"), "<!doctype html>");
  return { root, packageDirectory };
};

const writeWindowsIconPngFixture = async (iconPath: string): Promise<void> => {
  await mkdir(dirname(iconPath), { recursive: true });
  await writeFile(
    iconPath,
    Buffer.from(
      "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg==",
      "base64",
    ),
  );
};

const createWindowsResourceFixture = async (
  pePath: string,
  icoPath: string,
  overlayPath: string | undefined = undefined,
): Promise<void> => {
  await execFileAsync(process.execPath, [
    resolve(
      "..",
      "muon-prepare",
      "scripts",
      "create-windows-resource-fixture.mjs",
    ),
    pePath,
    icoPath,
    ...(overlayPath === undefined ? [] : [overlayPath]),
  ]);
};

const assertWindowsIcon = async (
  executablePath: string,
  iconPath: string,
): Promise<void> => {
  await execFileAsync(process.execPath, [
    resolve("..", "muon-prepare", "scripts", "assert-windows-icon.mjs"),
    executablePath,
    iconPath,
  ]);
};

const assertWindowsVersion = async (
  executablePath: string,
  expectations: readonly string[],
): Promise<void> => {
  await execFileAsync(process.execPath, [
    resolve("..", "muon-prepare", "scripts", "assert-windows-version.mjs"),
    executablePath,
    ...expectations,
  ]);
};

const getCliPath = (): string => resolve("dist", "cli.cjs");

const getVitePluginUrl = (): string =>
  pathToFileURL(resolve("dist", "vite.mjs")).href;

const runMuonCli = async (
  root: string,
  args: readonly string[],
): Promise<{ stderr: string; stdout: string }> => {
  const result = await execFileAsync(
    process.execPath,
    [getCliPath(), ...args],
    {
      cwd: root,
      env: process.env,
      maxBuffer: 10 * 1024 * 1024,
    },
  );
  return {
    stderr: result.stderr,
    stdout: result.stdout,
  };
};

const writeViteMuonBuildProject = async (
  root: string,
  buildExpression: string,
): Promise<void> => {
  await writeFile(
    join(root, "package.json"),
    `${JSON.stringify(
      {
        name: "vite-cli-build-sample",
        type: "module",
      },
      null,
      2,
    )}\n`,
  );
  await writeFile(
    join(root, "index.html"),
    '<!doctype html><title>vite cli app</title><script type="module" src="/src/main.ts"></script>',
  );
  await mkdir(join(root, "src"), { recursive: true });
  await writeFile(
    join(root, "src", "main.ts"),
    'document.body.textContent = "vite cli build";\n',
  );
  await writeFile(
    join(root, "muon.json"),
    `${JSON.stringify({ network: { allow: ["asset://main/**"] } }, null, 2)}\n`,
  );
  await writeFile(
    join(root, "vite.config.mjs"),
    [
      `import muon from ${JSON.stringify(getVitePluginUrl())};`,
      "export default {",
      "  build: { outDir: 'web-dist' },",
      "  plugins: [",
      `    muon({ build: ${buildExpression} }),`,
      "  ],",
      "};",
    ].join("\n"),
  );
};

const readZipEntries = async (
  archivePath: string,
): Promise<Map<string, Buffer>> => {
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
  const entries = new Map<string, Buffer>();
  let cursor = centralDirectoryOffset;
  for (let index = 0; index < entryCount; index += 1) {
    if (content.readUInt32LE(cursor) !== 0x02014b50) {
      throw new Error("ZIP central directory entry is invalid.");
    }
    const method = content.readUInt16LE(cursor + 10);
    const compressedSize = content.readUInt32LE(cursor + 20);
    const uncompressedSize = content.readUInt32LE(cursor + 24);
    const nameLength = content.readUInt16LE(cursor + 28);
    const extraLength = content.readUInt16LE(cursor + 30);
    const commentLength = content.readUInt16LE(cursor + 32);
    const localHeaderOffset = content.readUInt32LE(cursor + 42);
    const name = content.toString(
      "utf8",
      cursor + 46,
      cursor + 46 + nameLength,
    );
    const localNameLength = content.readUInt16LE(localHeaderOffset + 26);
    const localExtraLength = content.readUInt16LE(localHeaderOffset + 28);
    const dataStart =
      localHeaderOffset + 30 + localNameLength + localExtraLength;
    const data = content.subarray(dataStart, dataStart + compressedSize);
    const inflated =
      method === 0
        ? Buffer.from(data)
        : method === 8
          ? inflateRawSync(data)
          : undefined;
    if (inflated === undefined) {
      throw new Error(`Unsupported ZIP compression method: ${method}`);
    }
    if (inflated.length !== uncompressedSize) {
      throw new Error(`Unexpected ZIP entry size: ${name}`);
    }
    entries.set(name, inflated);
    cursor += 46 + nameLength + extraLength + commentLength;
  }
  return entries;
};

afterEach(async () => {
  for (const directory of cleanupDirectories.splice(0)) {
    await rm(directory, { recursive: true, force: true });
  }
});

describe("muon build", () => {
  it("creates a CEF-free target distribution with embedded config and signed assets", async () => {
    const root = await createTemporaryDirectory("muon-build-app-");
    const packageDirectory = await createFakeMuonPackageDist(root);
    await writeFile(
      join(root, "package.json"),
      `${JSON.stringify({ name: "@scope/muon-sample" }, null, 2)}\n`,
    );
    await mkdir(join(root, "assets", "main"), { recursive: true });
    await writeFile(
      join(root, "assets", "main", "index.html"),
      "<!doctype html><title>muon app</title>",
    );
    await writeFile(
      join(root, "muon.json"),
      `${JSON.stringify(
        {
          asset: { sourcePath: "./old-assets" },
          network: { allow: ["asset://main/**"] },
        },
        null,
        2,
      )}\n`,
    );
    const salt = Buffer.from([0xde, 0xad, 0xbe, 0xef]);

    const result = await buildMuonApp({
      root,
      packageDirectory,
      targets: ["linux-amd64"],
      assetSourcePath: "assets",
      assetSalt: salt,
    });

    const [target] = result.targets;
    expect(target?.target).toBe("linux-amd64");
    expect(target?.outputPath).toBe(join(root, "dist-muon-linux-amd64"));
    expect(target?.launcherPath).toBe(
      join(root, "dist-muon-linux-amd64", "muon-sample"),
    );
    await expect(
      readFile(join(root, "dist-muon-linux-amd64", "muon-core")),
    ).resolves.toBeDefined();
    await expect(
      readFile(join(root, "dist-muon-linux-amd64", "libmuon-ui.so"), "utf8"),
    ).resolves.toBe("ui\n");
    await expect(
      readFile(join(root, "dist-muon-linux-amd64", "libcardio.so"), "utf8"),
    ).resolves.toBe("cardio\n");
    await expect(
      readFile(join(root, "dist-muon-linux-amd64", "CREDITS.md"), "utf8"),
    ).resolves.toBe("notices\n");
    await expect(
      exists(join(root, "dist-muon-linux-amd64", "THIRD_PARTY_NOTICES.md")),
    ).resolves.toBe(false);
    await expect(
      exists(join(root, "dist-muon-linux-amd64", "muon.json")),
    ).resolves.toBe(false);
    await expect(
      exists(join(root, "dist-muon-linux-amd64", "assets")),
    ).resolves.toBe(false);
    await expect(
      exists(join(root, "dist-muon-linux-amd64", "muon-prepare")),
    ).resolves.toBe(false);
    await expect(
      exists(join(root, "dist-muon-linux-amd64", "libcef.so")),
    ).resolves.toBe(false);

    const archivePath = join(root, "dist-muon-linux-amd64", "assets.zip");
    const archiveContent = await readFile(archivePath);
    const entries = await readZipEntries(archivePath);
    expect(entries.get("main/index.html")?.toString("utf8")).toBe(
      "<!doctype html><title>muon app</title>",
    );
    expect(target?.asset.signature).toBe(
      createHash("sha1").update(archiveContent).update(salt).digest("hex"),
    );
    expect(target?.asset.salt).toBe("deadbeef");
    expect(target?.embeddedConfig.asset).toEqual({
      sourcePath: "./assets.zip",
      signature: target?.asset.signature,
      salt: "deadbeef",
    });
    expect(target?.embeddedConfig.bootstrap).toEqual({
      appId: "scope.muon-sample",
    });
    const embeddedCore = await readFile(
      join(root, "dist-muon-linux-amd64", "muon-core"),
    );
    const embeddedLauncher = await readFile(
      join(root, "dist-muon-linux-amd64", "muon-sample"),
    );
    expect(() => findMuonEmbeddedConfigSlot(embeddedCore)).toThrow("found 0");
    expect(() => findMuonBootstrapEmbeddedConfigSlot(embeddedLauncher)).toThrow(
      "found 0",
    );
  });

  it("builds every supported target by default when targets are omitted", async () => {
    const root = await createTemporaryDirectory("muon-build-default-all-");
    const packageDirectory = await createFakeMuonPackageDistForTargets(root, [
      "linux-amd64",
      "linux-armhf",
      "linux-arm64",
      "windows-i686",
      "windows-amd64",
    ]);
    await writeFile(
      join(root, "package.json"),
      `${JSON.stringify({ name: "default-targets-sample" }, null, 2)}\n`,
    );
    await mkdir(join(root, "assets"), { recursive: true });
    await writeFile(join(root, "assets", "index.html"), "<!doctype html>");

    const result = await buildMuonApp({
      root,
      packageDirectory,
      assetSalt: Buffer.from([0x0a, 0x11]),
    });

    expect(result.targets.map((target) => target.target)).toEqual([
      "linux-amd64",
      "linux-armhf",
      "linux-arm64",
      "windows-i686",
      "windows-amd64",
    ]);
    await expect(
      exists(
        join(root, "dist-muon-windows-i686", "default-targets-sample.exe"),
      ),
    ).resolves.toBe(true);
    await expect(
      exists(
        join(root, "dist-muon-windows-amd64", "default-targets-sample.exe"),
      ),
    ).resolves.toBe(true);
  });

  it("copies MinGW runtime DLLs into Windows target distributions", async () => {
    const root = await createTemporaryDirectory("muon-build-windows-runtime-");
    const packageDirectory = await createFakeMuonPackageDistForTargets(root, [
      "windows-amd64",
    ]);
    await writeFile(
      join(root, "package.json"),
      `${JSON.stringify({ name: "windows-runtime-sample" }, null, 2)}\n`,
    );
    await mkdir(join(root, "assets"), { recursive: true });
    await writeFile(join(root, "assets", "index.html"), "<!doctype html>");

    await buildMuonApp({
      root,
      packageDirectory,
      targets: ["windows-amd64"],
      assetSalt: Buffer.from([0x0b, 0x22]),
    });

    const outputPath = join(root, "dist-muon-windows-amd64");
    await expect(
      readFile(join(outputPath, "libgcc_s_seh-1.dll"), "utf8"),
    ).resolves.toBe("windows-amd64 libgcc_s_seh-1.dll\n");
    await expect(
      readFile(join(outputPath, "libstdc++-6.dll"), "utf8"),
    ).resolves.toBe("windows-amd64 libstdc++-6.dll\n");
    await expect(
      readFile(join(outputPath, "libwinpthread-1.dll"), "utf8"),
    ).resolves.toBe("windows-amd64 libwinpthread-1.dll\n");
  });

  it("updates Windows launcher icon and version resources from muon config metadata", async () => {
    const root = await createTemporaryDirectory("muon-build-windows-resource-");
    const packageDirectory = await createFakeMuonPackageDistForTargets(root, [
      "windows-amd64",
    ]);
    const iconPath = join(root, "icons", "app.png");
    const seedIconPath = join(root, "icons", "seed.ico");
    const expectedIconPath = join(root, "icons", "expected.ico");
    const bootstrapSlotPath = join(root, "bootstrap-slot.bin");
    await writeFile(bootstrapSlotPath, createMuonBootstrapEmbeddedConfigSlot());
    await createWindowsResourceFixture(
      join(packageDirectory, "native", "windows-amd64", "muon-bootstrap.exe"),
      seedIconPath,
      bootstrapSlotPath,
    );
    await writeWindowsIconPngFixture(iconPath);
    await writeFile(
      join(root, "package.json"),
      `${JSON.stringify(
        {
          name: "windows-resource-sample",
          version: "1.2.3",
          description: "Package description",
          author: "Package Author",
        },
        null,
        2,
      )}\n`,
    );
    await writeFile(
      join(root, "muon.json"),
      `${JSON.stringify(
        {
          windows: {
            resource: {
              iconPath: "icons/app.png",
              productName: "Muon Config Product",
              fileDescription: "Muon Config Description",
              companyName: "Muon Config Company",
              version: "7.8.9",
              copyright: "Copyright Muon Config",
            },
          },
        },
        null,
        2,
      )}\n`,
    );
    await mkdir(join(root, "assets"), { recursive: true });
    await writeFile(join(root, "assets", "index.html"), "<!doctype html>");

    const previousPreparePath = process.env.MUON_PREPARE_PATH;
    process.env.MUON_PREPARE_PATH = resolve(
      "dist",
      "native",
      "linux-amd64",
      "muon-prepare",
    );
    let result: Awaited<ReturnType<typeof buildMuonApp>> | undefined =
      undefined;
    try {
      result = await buildMuonApp({
        root,
        packageDirectory,
        targets: ["windows-amd64"],
        assetSalt: Buffer.from([0x0c, 0x33]),
      });
    } finally {
      if (previousPreparePath === undefined) {
        delete process.env.MUON_PREPARE_PATH;
      } else {
        process.env.MUON_PREPARE_PATH = previousPreparePath;
      }
    }

    expect(result).toBeDefined();
    const [target] = result?.targets ?? [];
    const launcherPath = join(
      root,
      "dist-muon-windows-amd64",
      "windows-resource-sample.exe",
    );
    expect(target?.launcherPath).toBe(launcherPath);
    expect(
      (target?.embeddedConfig as Record<string, unknown> | undefined)?.windows,
    ).toBeUndefined();
    await writeFile(
      expectedIconPath,
      await createWindowsIconBufferFromPngData(
        await readFile(iconPath),
        iconPath,
      ),
    );
    await assertWindowsIcon(launcherPath, expectedIconPath);
    await assertWindowsVersion(launcherPath, [
      "--file-version",
      "7.8.9.0",
      "--product-version",
      "7.8.9.0",
      "CompanyName=Muon Config Company",
      "FileDescription=Muon Config Description",
      "FileVersion=7.8.9",
      "ProductName=Muon Config Product",
      "ProductVersion=7.8.9",
      "LegalCopyright=Copyright Muon Config",
    ]);
  });

  it("rejects Windows resource icon paths that are not PNG files", async () => {
    const { root, packageDirectory } = await createWindowsIconBuildProject(
      "muon-build-windows-icon-",
      "windows-icon-sample",
    );
    await mkdir(join(root, "icons"), { recursive: true });
    await writeFile(join(root, "icons", "app.ico"), "ico\n");

    await expect(
      buildMuonApp({
        root,
        packageDirectory,
        targets: ["linux-amd64"],
        windowsResource: {
          iconPath: "icons/app.ico",
        },
      }),
    ).rejects.toThrow("Windows resource icon must be a .png file");
  });

  it("rejects missing Windows resource icon PNG files", async () => {
    const { root, packageDirectory } = await createWindowsIconBuildProject(
      "muon-build-missing-windows-icon-",
      "missing-windows-icon-sample",
    );
    const iconPath = join(root, "icons", "missing.png");

    await expect(
      buildMuonApp({
        root,
        packageDirectory,
        targets: ["linux-amd64"],
        windowsResource: {
          iconPath: "icons/missing.png",
        },
      }),
    ).rejects.toThrow(`Windows resource icon does not exist: ${iconPath}`);
  });

  it("rejects invalid Windows resource icon PNG files", async () => {
    const { root, packageDirectory } = await createWindowsIconBuildProject(
      "muon-build-invalid-windows-icon-",
      "invalid-windows-icon-sample",
    );
    await mkdir(join(root, "icons"), { recursive: true });
    await writeFile(
      join(root, "icons", "app.png"),
      Buffer.from("89504e470d0a1a0a00000000", "hex"),
    );

    await expect(
      buildMuonApp({
        root,
        packageDirectory,
        targets: ["linux-amd64"],
        windowsResource: {
          iconPath: "icons/app.png",
        },
      }),
    ).rejects.toThrow("Windows resource icon must be a valid PNG");
  });

  it("rejects CEF-derived target IDs in public build options", async () => {
    const root = await createTemporaryDirectory("muon-build-old-target-");
    const packageDirectory = await createFakeMuonPackageDist(root);
    await writeFile(
      join(root, "package.json"),
      `${JSON.stringify({ name: "old-target-sample" }, null, 2)}\n`,
    );
    await mkdir(join(root, "assets"), { recursive: true });
    await writeFile(join(root, "assets", "index.html"), "<!doctype html>");

    await expect(
      buildMuonApp({
        root,
        packageDirectory,
        targets: ["linux64"],
      }),
    ).rejects.toThrow("Unsupported muon build target: linux64");
  });

  it("runs the Vite-backed build sequence from the muon build CLI", async () => {
    const root = await createTemporaryDirectory("muon-build-cli-vite-");
    const packageDirectory = await createFakeMuonPackageDist(root);
    await writeViteMuonBuildProject(
      root,
      `{
        packageDirectory: ${JSON.stringify(packageDirectory)},
        targets: ['linux-amd64'],
        outputRoot: 'release',
        appName: 'plugin-app',
        appId: 'com.example.plugin-app'
      }`,
    );

    const result = await runMuonCli(root, ["build", "--json"]);
    const parsed = JSON.parse(result.stdout) as {
      appId: string;
      appName: string;
      targets: readonly { outputPath: string; target: string }[];
    };

    expect(parsed.appName).toBe("plugin-app");
    expect(parsed.appId).toBe("com.example.plugin-app");
    expect(
      parsed.targets.map((target) => ({
        outputPath: target.outputPath,
        target: target.target,
      })),
    ).toEqual([
      {
        outputPath: join(root, "release", "dist-muon-linux-amd64"),
        target: "linux-amd64",
      },
    ]);
    const entries = await readZipEntries(
      join(root, "release", "dist-muon-linux-amd64", "assets.zip"),
    );
    expect(entries.has("main/index.html")).toBe(true);
    expect(
      [...entries.keys()].some((entry) => entry.startsWith("main/assets/")),
    ).toBe(true);
  });

  it("lets muon build CLI options override Vite plugin build options", async () => {
    const root = await createTemporaryDirectory("muon-build-cli-override-");
    const pluginPackageDirectory = await createFakeMuonPackageDistForTargets(
      root,
      ["windows-amd64"],
    );
    const cliPackageDirectory = await createFakeMuonPackageDistForTargets(
      root,
      ["linux-amd64"],
    );
    await writeViteMuonBuildProject(
      root,
      `{
        packageDirectory: ${JSON.stringify(pluginPackageDirectory)},
        targets: ['windows-amd64'],
        outputRoot: 'plugin-release',
        appName: 'plugin-app',
        appId: 'com.example.plugin-app'
      }`,
    );

    const result = await runMuonCli(root, [
      "build",
      "--json",
      "--target",
      "linux-amd64",
      "--out-dir",
      "cli-release",
      "--name",
      "cli-app",
      "--app-id",
      "com.example.cli-app",
      "--package-directory",
      cliPackageDirectory,
    ]);
    const parsed = JSON.parse(result.stdout) as {
      appId: string;
      appName: string;
      targets: readonly { outputPath: string; target: string }[];
    };

    expect(parsed.appName).toBe("cli-app");
    expect(parsed.appId).toBe("com.example.cli-app");
    expect(
      parsed.targets.map((target) => ({
        outputPath: target.outputPath,
        target: target.target,
      })),
    ).toEqual([
      {
        outputPath: join(root, "cli-release", "dist-muon-linux-amd64"),
        target: "linux-amd64",
      },
    ]);
    await expect(
      exists(join(root, "plugin-release", "dist-muon-windows-amd64")),
    ).resolves.toBe(false);
    await expect(
      exists(join(root, "cli-release", "dist-muon-linux-amd64", "cli-app")),
    ).resolves.toBe(true);
  });

  it("rejects muon build when the Vite plugin disables Muon builds", async () => {
    const root = await createTemporaryDirectory("muon-build-cli-disabled-");
    await writeViteMuonBuildProject(root, "false");

    await expect(runMuonCli(root, ["build"])).rejects.toThrow(
      "Muon build is disabled by muon({ build: false })",
    );
  });

  it("packages Vite output under asset://main/ during vite build", async () => {
    const root = await createTemporaryDirectory("muon-build-vite-");
    const packageDirectory = await createFakeMuonPackageDist(root);
    await writeFile(
      join(root, "package.json"),
      `${JSON.stringify(
        {
          name: "vite-muon-sample",
          type: "module",
          dependencies: {},
          devDependencies: {},
        },
        null,
        2,
      )}\n`,
    );
    await writeFile(
      join(root, "index.html"),
      '<!doctype html><title>vite app</title><script type="module" src="/src/main.ts"></script>',
    );
    await mkdir(join(root, "src"), { recursive: true });
    await writeFile(
      join(root, "src", "main.ts"),
      'document.body.textContent = "vite muon";\n',
    );
    await writeFile(
      join(root, "muon.json"),
      `${JSON.stringify({ network: { allow: ["asset://main/**"] } }, null, 2)}\n`,
    );

    await viteBuild({
      root,
      logLevel: "silent",
      build: {
        outDir: "web-dist",
      },
      plugins: [
        muon({
          build: {
            packageDirectory,
            targets: ["linux-amd64"],
            assetSalt: Buffer.from([0xca, 0xfe]),
          },
        }),
      ],
    });

    const outputPath = join(root, "dist-muon-linux-amd64");
    const entries = await readZipEntries(join(outputPath, "assets.zip"));
    expect(entries.has("main/index.html")).toBe(true);
    expect(
      [...entries.keys()].some((entry) => entry.startsWith("main/assets/")),
    ).toBe(true);
    await expect(exists(join(outputPath, "muon.json"))).resolves.toBe(false);
    await expect(exists(join(outputPath, "libcef.so"))).resolves.toBe(false);
    const embeddedCore = await readFile(join(outputPath, "muon-core"));
    expect(() => findMuonEmbeddedConfigSlot(embeddedCore)).toThrow("found 0");
  });

  it("builds with an empty config when project config is missing", async () => {
    const root = await createTemporaryDirectory("muon-build-missing-config-");
    const packageDirectory = await createFakeMuonPackageDist(root);
    await writeFile(
      join(root, "package.json"),
      `${JSON.stringify({ name: "missing-config-sample" }, null, 2)}\n`,
    );
    await mkdir(join(root, "assets"), { recursive: true });
    await writeFile(join(root, "assets", "index.html"), "<!doctype html>");

    const result = await buildMuonApp({
      root,
      packageDirectory,
      targets: ["linux-amd64"],
      assetSalt: Buffer.from([0x12, 0x34]),
    });

    const [target] = result.targets;
    expect(target?.embeddedConfig).toEqual({
      asset: {
        sourcePath: "./assets.zip",
        signature: target?.asset.signature,
        salt: "1234",
      },
      bootstrap: {
        appId: "missing-config-sample",
      },
    });
  });

  it("uses muon config asset.sourcePath relative to the config directory as the non-Vite asset source", async () => {
    const root = await createTemporaryDirectory("muon-build-config-assets-");
    const packageDirectory = await createFakeMuonPackageDist(root);
    await writeFile(
      join(root, "package.json"),
      `${JSON.stringify({ name: "config-assets-sample" }, null, 2)}\n`,
    );
    await mkdir(join(root, "settings", "web", "main"), { recursive: true });
    await writeFile(
      join(root, "settings", "web", "main", "index.html"),
      "<!doctype html><title>config asset app</title>",
    );
    await writeFile(
      join(root, "settings", "muon.json"),
      `${JSON.stringify(
        {
          asset: { sourcePath: "./web" },
          network: { allow: ["asset://main/**"] },
        },
        null,
        2,
      )}\n`,
    );

    const result = await buildMuonApp({
      root,
      packageDirectory,
      targets: ["linux-amd64"],
      configPath: "settings/muon.json",
      assetSalt: Buffer.from([0x56, 0x78]),
    });

    const [target] = result.targets;
    const archivePath = join(root, "dist-muon-linux-amd64", "assets.zip");
    const archiveContent = await readFile(archivePath);
    const entries = await readZipEntries(archivePath);
    expect(entries.get("main/index.html")?.toString("utf8")).toBe(
      "<!doctype html><title>config asset app</title>",
    );
    expect(target?.asset.signature).toBe(
      createHash("sha1")
        .update(archiveContent)
        .update(Buffer.from([0x56, 0x78]))
        .digest("hex"),
    );
    expect(target?.embeddedConfig.asset).toEqual({
      sourcePath: "./assets.zip",
      signature: target?.asset.signature,
      salt: "5678",
    });
  });

  it("copies a muon.json asset.sourcePath ZIP file into the generated distribution", async () => {
    const root = await createTemporaryDirectory("muon-build-config-zip-");
    const packageDirectory = await createFakeMuonPackageDist(root);
    await writeFile(
      join(root, "package.json"),
      `${JSON.stringify({ name: "config-zip-sample" }, null, 2)}\n`,
    );
    await mkdir(join(root, "packed"), { recursive: true });
    const sourceZipPath = join(root, "packed", "web.zip");
    const sourceZip = Buffer.from([
      0x50, 0x4b, 0x05, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    ]);
    await writeFile(sourceZipPath, sourceZip);
    await writeFile(
      join(root, "muon.json"),
      `${JSON.stringify(
        {
          asset: { sourcePath: "./packed/web.zip" },
          network: { allow: ["asset://main/**"] },
        },
        null,
        2,
      )}\n`,
    );

    const result = await buildMuonApp({
      root,
      packageDirectory,
      targets: ["linux-amd64"],
      assetSalt: Buffer.from([0x9a, 0xbc]),
    });

    const [target] = result.targets;
    const archivePath = join(root, "dist-muon-linux-amd64", "assets.zip");
    await expect(readFile(archivePath)).resolves.toEqual(sourceZip);
    expect(target?.asset.entryCount).toBe(0);
    expect(target?.asset.signature).toBe(
      createHash("sha1")
        .update(sourceZip)
        .update(Buffer.from([0x9a, 0xbc]))
        .digest("hex"),
    );
    expect(target?.embeddedConfig.asset).toEqual({
      sourcePath: "./assets.zip",
      signature: target?.asset.signature,
      salt: "9abc",
    });
  });

  it("reports the project config path when default config parsing fails", async () => {
    const root = await createTemporaryDirectory("muon-build-invalid-config-");
    const packageDirectory = await createFakeMuonPackageDist(root);
    await writeFile(
      join(root, "package.json"),
      `${JSON.stringify({ name: "invalid-config-sample" }, null, 2)}\n`,
    );
    await mkdir(join(root, "assets"), { recursive: true });
    await writeFile(join(root, "assets", "index.html"), "<!doctype html>");
    await writeFile(join(root, "muon.json"), "{ invalid json\n");

    await expect(
      buildMuonApp({
        root,
        packageDirectory,
        targets: ["linux-amd64"],
      }),
    ).rejects.toThrow(join(root, "muon.json"));
  });

  it("reports the explicit config path when --config input is missing", async () => {
    const root = await createTemporaryDirectory("muon-build-explicit-config-");
    const packageDirectory = await createFakeMuonPackageDist(root);
    const configPath = join(root, "missing-muon.json");
    await writeFile(
      join(root, "package.json"),
      `${JSON.stringify({ name: "explicit-config-sample" }, null, 2)}\n`,
    );
    await mkdir(join(root, "assets"), { recursive: true });
    await writeFile(join(root, "assets", "index.html"), "<!doctype html>");

    await expect(
      buildMuonApp({
        root,
        packageDirectory,
        targets: ["linux-amd64"],
        configPath,
      }),
    ).rejects.toThrow(`Muon config file does not exist: ${configPath}`);
  });
});
