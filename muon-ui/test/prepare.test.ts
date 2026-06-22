// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import { execFile } from "node:child_process";
import { createHash } from "node:crypto";
import {
  access,
  chmod,
  copyFile,
  mkdir,
  mkdtemp,
  readdir,
  readFile,
  rm,
  stat,
  utimes,
  writeFile,
} from "node:fs/promises";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { promisify } from "node:util";

import {
  afterAll,
  afterEach,
  beforeAll,
  describe,
  expect,
  it,
  vi,
} from "vitest";

import { getDefaultMuonPrepareTarget, runMuonPrepare } from "../src/prepare.js";
import { embedMuonConfigInBootstrapFile } from "../src/embed-config.js";
import {
  buildTestMuonPrepare,
  createRuntimeInfoHeader,
} from "./test-muon-prepare.js";

const execFileAsync = promisify(execFile);
const cleanupDirectories: string[] = [];
const suiteCleanupDirectories: string[] = [];
let prepareExecutablePath = "";
let bootstrapExecutablePath = "";
let embeddedCefArchive:
  | {
      archivePath: string;
      fileName: string;
      sha1: string;
      size: number;
    }
  | undefined = undefined;

interface PrepareFixture {
  projectPath: string;
  muonPath: string;
  cefPath: string;
  cacheDir: string;
  catalogPath: string;
  archiveFileName: string;
  stageDir: string;
}

interface FakeCefArchiveOptions {
  archiveFileName?: string;
  archiveRootName?: string;
  apiHash?: string;
  libcefContent?: string;
}

interface CatalogVersion {
  version: string;
  chromiumVersion: string;
  archive: {
    archivePath: string;
    fileName: string;
    sha1: string;
    size: number;
  };
  channel?: string;
  lastModified?: string;
}

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

const createFakeCefArchive = async (
  createDirectory: (
    prefix: string,
  ) => Promise<string> = createTemporaryDirectory,
  options: FakeCefArchiveOptions = {},
): Promise<{
  archivePath: string;
  fileName: string;
  sha1: string;
  size: number;
}> => {
  const root = await createDirectory("muon-prepare-cef-");
  const entry = join(
    root,
    options.archiveRootName ?? "cef_binary_fake_linux64_minimal",
  );
  const apiHash = options.apiHash ?? "fake-api-hash";
  await mkdir(join(entry, "Release"), { recursive: true });
  await mkdir(join(entry, "Resources", "locales"), { recursive: true });
  await mkdir(join(entry, "include"), { recursive: true });
  await mkdir(join(entry, "cmake"), { recursive: true });
  await mkdir(join(entry, "libcef_dll_wrapper"), { recursive: true });
  await writeFile(
    join(entry, "Release", "libcef.so"),
    options.libcefContent ?? "cef library\n",
  );
  await writeFile(join(entry, "Resources", "icudtl.dat"), "cef data\n");
  await writeFile(join(entry, "Resources", "locales", "en-US.pak"), "locale\n");
  await writeFile(
    join(entry, "include", "cef_api_versions.h"),
    `#define CEF_API_HASH_14700 "windows-${apiHash}" "mac-${apiHash}" "${apiHash}"\n`,
  );
  await writeFile(join(entry, "cmake", "cef-config.cmake"), "cmake\n");
  await writeFile(
    join(entry, "libcef_dll_wrapper", "CMakeLists.txt"),
    "wrapper\n",
  );
  const fileName = options.archiveFileName ?? "cef.tar.bz2";
  const archivePath = join(root, fileName);
  await execFileAsync("tar", [
    "-cjf",
    archivePath,
    "-C",
    root,
    options.archiveRootName ?? "cef_binary_fake_linux64_minimal",
  ]);
  const content = await readFile(archivePath);
  return {
    archivePath,
    fileName,
    sha1: createHash("sha1").update(content).digest("hex"),
    size: (await stat(archivePath)).size,
  };
};

const getEmbeddedCefArchive = (): {
  archivePath: string;
  fileName: string;
  sha1: string;
  size: number;
} => {
  if (embeddedCefArchive === undefined) {
    throw new Error("Embedded CEF archive fixture is not initialized.");
  }
  return embeddedCefArchive;
};

beforeAll(async () => {
  embeddedCefArchive = await createFakeCefArchive(
    createSuiteTemporaryDirectory,
  );
  const executableName =
    process.platform === "win32" ? "muon-core.exe" : "muon-core";
  const buildRoot = await createSuiteTemporaryDirectory("muon-prepare-native-");
  const binaries = await buildTestMuonPrepare(
    buildRoot,
    createRuntimeInfoHeader({
      archiveFileName: embeddedCefArchive.fileName,
      archiveUrl: embeddedCefArchive.archivePath,
      archiveSha1: embeddedCefArchive.sha1,
      archiveSize: embeddedCefArchive.size,
      executableName,
      corePayload: [executableName, "plugins"],
    }),
  );
  prepareExecutablePath = binaries.prepareExecutablePath;
  bootstrapExecutablePath = binaries.bootstrapExecutablePath;
});

afterAll(async () => {
  for (const directory of suiteCleanupDirectories.splice(0)) {
    await rm(directory, { recursive: true, force: true });
  }
});

const createFakeCefDirectory = async (
  shape: "releaseResources" | "flat",
): Promise<string> => {
  const cefPath = await createTemporaryDirectory("muon-prepare-cef-dir-");
  if (shape === "releaseResources") {
    await mkdir(join(cefPath, "Release"), { recursive: true });
    await mkdir(join(cefPath, "Resources", "locales"), { recursive: true });
    await writeFile(join(cefPath, "Release", "libcef.so"), "cef library\n");
    await writeFile(join(cefPath, "Resources", "icudtl.dat"), "cef data\n");
    await writeFile(
      join(cefPath, "Resources", "locales", "en-US.pak"),
      "locale\n",
    );
  } else {
    await mkdir(join(cefPath, "locales"), { recursive: true });
    await writeFile(join(cefPath, "libcef.so"), "cef library\n");
    await writeFile(join(cefPath, "icudtl.dat"), "cef data\n");
    await writeFile(join(cefPath, "locales", "en-US.pak"), "locale\n");
  }
  return cefPath;
};

const createPrepareFixture = async (
  sha1Override: string | undefined = undefined,
  catalogVersions: readonly CatalogVersion[] | undefined = undefined,
): Promise<PrepareFixture> => {
  const muonPath = await createTemporaryDirectory("muon-prepare-muon-");
  const cefPath = await createFakeCefDirectory("releaseResources");
  const cacheDir = await createTemporaryDirectory("muon-prepare-cache-");
  const sourceDir = await createTemporaryDirectory("muon-prepare-source-");
  const projectPath = await createTemporaryDirectory("muon-prepare-project-");
  const stageDir = join(projectPath, ".muon", "linux64");
  const cef = getEmbeddedCefArchive();
  const versions: readonly CatalogVersion[] = catalogVersions ?? [
    {
      version: "fake-cef",
      chromiumVersion: "100.0.0.0",
      archive: cef,
      channel: "stable",
      lastModified: "2026-01-01T00:00:00.000Z",
    },
  ];
  for (const version of versions) {
    await copyFile(
      version.archive.archivePath,
      join(sourceDir, version.archive.fileName),
    );
  }
  const catalogPath = join(sourceDir, "source-catalog.json");
  await mkdir(join(muonPath, "plugins"), { recursive: true });
  await writeFile(join(muonPath, "muon-core"), "core v1\n");
  await chmod(join(muonPath, "muon-core"), 0o755);
  await writeFile(join(muonPath, "plugins", "plugin.txt"), "plugin\n");
  await mkdir(join(muonPath, "assets"), { recursive: true });
  await writeFile(join(muonPath, "assets", "app.txt"), "app asset\n");
  await writeFile(join(muonPath, "muon.json"), "{}\n");
  await writeFile(
    catalogPath,
    `${JSON.stringify(
      {
        linux64: {
          versions: versions.map((version) => ({
            cef_version: version.version,
            channel: version.channel ?? "stable",
            chromium_version: version.chromiumVersion,
            files: [
              {
                last_modified:
                  version.lastModified ?? "2026-01-01T00:00:00.000Z",
                name: version.archive.fileName,
                sha1:
                  version.archive.fileName === cef.fileName
                    ? (sha1Override ?? version.archive.sha1)
                    : version.archive.sha1,
                size: version.archive.size,
                type: "minimal",
              },
            ],
          })),
        },
      },
      null,
      2,
    )}\n`,
  );
  return {
    projectPath,
    muonPath,
    cefPath,
    cacheDir,
    catalogPath,
    archiveFileName: cef.fileName,
    stageDir,
  };
};

const writeBootstrapIni = async (
  runtimePath: string,
  content: string,
): Promise<void> => {
  await writeFile(join(runtimePath, "muon-bootstrap.ini"), content);
};

const findCachedFile = async (
  root: string,
  fileName: string,
): Promise<string | undefined> => {
  for (const entry of await readdir(root, { withFileTypes: true })) {
    const path = join(root, entry.name);
    if (entry.isDirectory()) {
      const child = await findCachedFile(path, fileName);
      if (child !== undefined) {
        return child;
      }
    } else if (entry.isFile() && entry.name === fileName) {
      return path;
    }
  }
  return undefined;
};

const prepareFixture = async (
  fixture: PrepareFixture,
  options: {
    cefPath: string | undefined;
    stageDir: string | undefined;
    force: boolean;
    quiet: boolean;
  } = {
    cefPath: fixture.cefPath,
    stageDir: fixture.stageDir,
    force: false,
    quiet: false,
  },
) =>
  await runMuonPrepare({
    muonPath: fixture.muonPath,
    cefPath: options.cefPath,
    stageDir: options.stageDir,
    target: "linux64",
    cacheDir: fixture.cacheDir,
    force: options.force,
    quiet: options.quiet,
    prepareExecutablePath,
    environment: {
      ...process.env,
      MUON_CEF_CATALOG_URL: fixture.catalogPath,
    },
    cwd: process.cwd(),
  });

const requireStagePath = (
  result: Awaited<ReturnType<typeof prepareFixture>>,
): string => {
  expect(result.stagePath).toBeDefined();
  return result.stagePath ?? "";
};

const listCacheEntries = async (root: string): Promise<string[]> => {
  const entries: string[] = [];
  const visit = async (directory: string, prefix: string): Promise<void> => {
    for (const entry of await readdir(directory, { withFileTypes: true })) {
      const relative = prefix === "" ? entry.name : `${prefix}/${entry.name}`;
      entries.push(relative);
      if (entry.isDirectory()) {
        await visit(join(directory, entry.name), relative);
      }
    }
  };
  await visit(root, "");
  return entries.sort();
};

afterEach(async () => {
  for (const directory of cleanupDirectories.splice(0)) {
    await rm(directory, { recursive: true, force: true });
  }
});

describe("muon prepare target resolution", () => {
  it("resolves supported Node platforms to Muon runtime targets", () => {
    expect(getDefaultMuonPrepareTarget("linux", "x64")).toBe("linux64");
    expect(getDefaultMuonPrepareTarget("linux", "arm")).toBe("linuxarm");
    expect(getDefaultMuonPrepareTarget("linux", "arm64")).toBe("linuxarm64");
    expect(getDefaultMuonPrepareTarget("win32", "ia32")).toBe("windows32");
    expect(getDefaultMuonPrepareTarget("win32", "x64")).toBe("windows64");
  });

  it("rejects unsupported Linux i686 targets", () => {
    expect(() => getDefaultMuonPrepareTarget("linux", "ia32")).toThrow(
      "Unsupported Muon prepare target: platform=linux, arch=ia32",
    );
  });
});

describe("muon-prepare", () => {
  it("stages explicit CEF Release/Resources files and the whole muon directory", async () => {
    const fixture = await createPrepareFixture();

    const result = await prepareFixture(fixture);
    const stagePath = requireStagePath(result);

    expect(result.stagePath).toBe(fixture.stageDir);
    expect(result.muonPath).toBe(fixture.muonPath);
    expect(result.cefPath).toBe(fixture.cefPath);
    await expect(access(join(stagePath, "muon-core"))).resolves.toBeUndefined();
    await expect(
      access(join(stagePath, "icudtl.dat")),
    ).resolves.toBeUndefined();
    await expect(
      access(join(stagePath, "locales", "en-US.pak")),
    ).resolves.toBeUndefined();
    await expect(access(join(stagePath, "libcef.so"))).resolves.toBeUndefined();
    await expect(
      access(join(stagePath, "plugins", "plugin.txt")),
    ).resolves.toBeUndefined();
    await expect(access(join(stagePath, "muon.json"))).resolves.toBeUndefined();
    await expect(
      access(join(stagePath, "assets", "app.txt")),
    ).resolves.toBeUndefined();
  });

  it("downloads CEF when cefPath is omitted and stages the prepared cache", async () => {
    const fixture = await createPrepareFixture();

    const result = await prepareFixture(fixture, {
      cefPath: undefined,
      stageDir: fixture.stageDir,
      force: false,
      quiet: false,
    });
    const stagePath = requireStagePath(result);

    expect(result.stagePath).toBe(fixture.stageDir);
    expect(result.cefPath).not.toBe(fixture.cefPath);
    expect(result.cefPath.endsWith("cef.tar.bz2")).toBe(true);
    expect(result.cefPath).toBe(
      join(fixture.cacheDir, "artifacts", fixture.archiveFileName),
    );
    await expect(
      findCachedFile(fixture.cacheDir, fixture.archiveFileName),
    ).resolves.toBe(result.cefPath);
    await expect(access(result.cefPath)).resolves.toBeUndefined();
    await expect(
      access(join(fixture.cacheDir, "catalog.json")),
    ).rejects.toThrow();
    await expect(listCacheEntries(fixture.cacheDir)).resolves.toEqual([
      "artifacts",
      `artifacts/${fixture.archiveFileName}`,
    ]);
    await expect(access(join(stagePath, "libcef.so"))).resolves.toBeUndefined();
    await expect(
      access(join(stagePath, "icudtl.dat")),
    ).resolves.toBeUndefined();
    await expect(
      access(join(stagePath, "plugins", "plugin.txt")),
    ).resolves.toBeUndefined();
  });

  it("uses the embedded tested CEF artifact without refreshing the catalog", async () => {
    const fixture = await createPrepareFixture();
    await writeBootstrapIni(
      fixture.muonPath,
      `[cef]
versionPolicy=tested
catalogRefreshIntervalSeconds=604800
lastCatalogUpdateUnix=0
`,
    );

    const result = await prepareFixture(fixture, {
      cefPath: undefined,
      stageDir: fixture.stageDir,
      force: false,
      quiet: false,
    });

    expect(result.cefPath).toBe(
      join(fixture.cacheDir, "artifacts", fixture.archiveFileName),
    );
    await expect(
      access(join(fixture.cacheDir, "catalog.json")),
    ).rejects.toThrow();
    await expect(
      readFile(join(result.stagePath ?? "", "libcef.so"), "utf8"),
    ).resolves.toBe("cef library\n");
  });

  it("selects the latest same-major CEF artifact with a matching API hash", async () => {
    const latestSameMajor = await createFakeCefArchive(
      createTemporaryDirectory,
      {
        archiveFileName: "cef-same-major-latest.tar.bz2",
        archiveRootName: "cef_binary_fake-cef.2_linux64_minimal",
        libcefContent: "same major latest cef library\n",
      },
    );
    const latestOtherMajor = await createFakeCefArchive(
      createTemporaryDirectory,
      {
        archiveFileName: "cef-other-major-latest.tar.bz2",
        archiveRootName: "cef_binary_other.1_linux64_minimal",
        libcefContent: "other major latest cef library\n",
      },
    );
    const fixture = await createPrepareFixture(undefined, [
      {
        version: "other.1",
        chromiumVersion: "200.0.0.0",
        archive: latestOtherMajor,
      },
      {
        version: "fake-cef.2",
        chromiumVersion: "102.0.0.0",
        archive: latestSameMajor,
      },
      {
        version: "fake-cef.1",
        chromiumVersion: "101.0.0.0",
        archive: getEmbeddedCefArchive(),
      },
    ]);
    await writeBootstrapIni(
      fixture.muonPath,
      `[cef]
versionPolicy=same-major-latest
catalogRefreshIntervalSeconds=604800
lastCatalogUpdateUnix=0
`,
    );

    const result = await prepareFixture(fixture, {
      cefPath: undefined,
      stageDir: fixture.stageDir,
      force: false,
      quiet: false,
    });

    expect(result.cefPath).toBe(
      join(fixture.cacheDir, "artifacts", latestSameMajor.fileName),
    );
    await expect(
      readFile(join(result.stagePath ?? "", "libcef.so"), "utf8"),
    ).resolves.toBe("same major latest cef library\n");
  });

  it("skips incompatible CEF artifacts for compat-latest", async () => {
    const incompatible = await createFakeCefArchive(createTemporaryDirectory, {
      archiveFileName: "cef-incompatible-latest.tar.bz2",
      archiveRootName: "cef_binary_incompatible_linux64_minimal",
      apiHash: "different-api-hash",
      libcefContent: "incompatible cef library\n",
    });
    const compatible = await createFakeCefArchive(createTemporaryDirectory, {
      archiveFileName: "cef-compatible-latest.tar.bz2",
      archiveRootName: "cef_binary_compatible_linux64_minimal",
      libcefContent: "compatible cef library\n",
    });
    const fixture = await createPrepareFixture(undefined, [
      {
        version: "newest-incompatible",
        chromiumVersion: "200.0.0.0",
        archive: incompatible,
      },
      {
        version: "newest-compatible",
        chromiumVersion: "199.0.0.0",
        archive: compatible,
      },
    ]);
    await writeBootstrapIni(
      fixture.muonPath,
      `[cef]
versionPolicy=compat-latest
catalogRefreshIntervalSeconds=604800
lastCatalogUpdateUnix=0
`,
    );

    const result = await prepareFixture(fixture, {
      cefPath: undefined,
      stageDir: fixture.stageDir,
      force: false,
      quiet: false,
    });

    expect(result.cefPath).toBe(
      join(fixture.cacheDir, "artifacts", compatible.fileName),
    );
    await expect(
      readFile(join(result.stagePath ?? "", "libcef.so"), "utf8"),
    ).resolves.toBe("compatible cef library\n");
  });

  it("rejects exact CEF artifacts with an incompatible API hash", async () => {
    const incompatible = await createFakeCefArchive(createTemporaryDirectory, {
      archiveFileName: "cef-exact-incompatible.tar.bz2",
      archiveRootName: "cef_binary_exact_incompatible_linux64_minimal",
      apiHash: "different-api-hash",
    });
    const fixture = await createPrepareFixture(undefined, [
      {
        version: "exact-incompatible",
        chromiumVersion: "150.0.0.0",
        archive: incompatible,
      },
    ]);
    await writeBootstrapIni(
      fixture.muonPath,
      `[cef]
versionPolicy=exact
exactVersion=exact-incompatible
catalogRefreshIntervalSeconds=604800
lastCatalogUpdateUnix=0
`,
    );

    await expect(
      prepareFixture(fixture, {
        cefPath: undefined,
        stageDir: fixture.stageDir,
        force: false,
        quiet: false,
      }),
    ).rejects.toThrow(/API hash/);
  });

  it("falls back to the embedded CEF artifact when the catalog is unavailable", async () => {
    const fixture = await createPrepareFixture();

    const result = await runMuonPrepare({
      muonPath: fixture.muonPath,
      cefPath: undefined,
      stageDir: fixture.stageDir,
      target: "linux64",
      cacheDir: fixture.cacheDir,
      force: false,
      quiet: false,
      prepareExecutablePath,
      environment: {
        ...process.env,
        MUON_CEF_CATALOG_URL: join(fixture.projectPath, "missing-catalog.json"),
      },
      cwd: process.cwd(),
    });
    const stagePath = requireStagePath(result);

    expect(result.stagePath).toBe(fixture.stageDir);
    expect(result.cefPath).toBe(
      join(fixture.cacheDir, "artifacts", fixture.archiveFileName),
    );
    await expect(access(join(stagePath, "libcef.so"))).resolves.toBeUndefined();
    await expect(
      access(join(stagePath, "icudtl.dat")),
    ).resolves.toBeUndefined();
  });

  it("prepares a full CEF root for muon-core builds from the catalog cache", async () => {
    const fixture = await createPrepareFixture();
    const outputDir = join(fixture.projectPath, ".cef", "cef_binary_fake");

    const { stdout } = await execFileAsync(
      prepareExecutablePath,
      [
        "buildtime",
        "--version",
        "fake-cef",
        "--target",
        "linux64",
        "--output-dir",
        outputDir,
        "--cache-dir",
        fixture.cacheDir,
        "--json",
      ],
      {
        encoding: "utf8",
        env: {
          ...process.env,
          MUON_CEF_CATALOG_URL: fixture.catalogPath,
        },
      },
    );

    const result = JSON.parse(stdout) as {
      cefPath: string;
      archivePath: string;
      version: string;
      target: string;
      distribution: string;
      artifact: {
        fileName: string;
        url: string;
        sha1: string;
        size: number;
      };
    };
    expect(result.cefPath).toBe(outputDir);
    expect(result.archivePath).toBe(
      join(fixture.cacheDir, "artifacts", fixture.archiveFileName),
    );
    expect(result.version).toBe("fake-cef");
    expect(result.target).toBe("linux64");
    expect(result.distribution).toBe("minimal");
    expect(result.artifact.fileName).toBe(fixture.archiveFileName);
    await expect(
      access(join(outputDir, "Release", "libcef.so")),
    ).resolves.toBeUndefined();
    await expect(
      access(join(outputDir, "Resources", "locales", "en-US.pak")),
    ).resolves.toBeUndefined();
    await expect(
      access(join(outputDir, "include", "cef_api_versions.h")),
    ).resolves.toBeUndefined();
    await expect(
      access(join(outputDir, "cmake", "cef-config.cmake")),
    ).resolves.toBeUndefined();
    await expect(
      access(join(outputDir, "libcef_dll_wrapper", "CMakeLists.txt")),
    ).resolves.toBeUndefined();
    await expect(listCacheEntries(fixture.cacheDir)).resolves.toEqual([
      "artifacts",
      `artifacts/${fixture.archiveFileName}`,
      "catalog.json",
    ]);
  });

  it("rebuilds an existing buildtime output without a ready marker", async () => {
    const fixture = await createPrepareFixture();
    const outputDir = join(fixture.projectPath, ".cef", "cef_binary_fake");
    await mkdir(outputDir, { recursive: true });
    await writeFile(join(outputDir, "stale.txt"), "stale\n");

    const { stdout } = await execFileAsync(
      prepareExecutablePath,
      [
        "buildtime",
        "--version",
        "fake-cef",
        "--target",
        "linux64",
        "--output-dir",
        outputDir,
        "--cache-dir",
        fixture.cacheDir,
        "--json",
      ],
      {
        encoding: "utf8",
        env: {
          ...process.env,
          MUON_CEF_CATALOG_URL: fixture.catalogPath,
        },
      },
    );

    const result = JSON.parse(stdout) as { cefPath: string };
    expect(result.cefPath).toBe(outputDir);
    await expect(
      access(join(outputDir, "Release", "libcef.so")),
    ).resolves.toBeUndefined();
    await expect(
      access(join(outputDir, ".muon-cef-ready.json")),
    ).resolves.toBeUndefined();
    await expect(access(join(outputDir, "stale.txt"))).rejects.toThrow();
  });

  it("keeps the artifact cache valid for concurrent CEF build prepares", async () => {
    const fixture = await createPrepareFixture();
    const firstOutputDir = join(fixture.projectPath, ".cef", "first");
    const secondOutputDir = join(fixture.projectPath, ".cef", "second");

    const results = await Promise.all(
      [firstOutputDir, secondOutputDir].map(async (outputDir) => {
        const { stdout } = await execFileAsync(
          prepareExecutablePath,
          [
            "buildtime",
            "--version",
            "fake-cef",
            "--target",
            "linux64",
            "--output-dir",
            outputDir,
            "--cache-dir",
            fixture.cacheDir,
            "--json",
          ],
          {
            encoding: "utf8",
            env: {
              ...process.env,
              MUON_CEF_CATALOG_URL: fixture.catalogPath,
            },
          },
        );
        return JSON.parse(stdout) as { archivePath: string };
      }),
    );
    const first = results[0];
    const second = results[1];
    if (first === undefined || second === undefined) {
      throw new Error("Expected two CEF prepare results.");
    }

    expect(first.archivePath).toBe(second.archivePath);
    await expect(
      access(join(firstOutputDir, "include", "cef_api_versions.h")),
    ).resolves.toBeUndefined();
    await expect(
      access(join(secondOutputDir, "include", "cef_api_versions.h")),
    ).resolves.toBeUndefined();
    await expect(listCacheEntries(fixture.cacheDir)).resolves.toEqual([
      "artifacts",
      `artifacts/${fixture.archiveFileName}`,
      "catalog.json",
    ]);
  });

  it("rejects the removed flat native CLI form", async () => {
    const fixture = await createPrepareFixture();

    await expect(
      execFileAsync(
        prepareExecutablePath,
        ["--muon-path", fixture.muonPath, "--json"],
        {
          encoding: "utf8",
        },
      ),
    ).rejects.toMatchObject({
      stderr: expect.stringContaining("Usage: muon-prepare"),
    });
  });

  it("writes progress messages while downloading and staging", async () => {
    const fixture = await createPrepareFixture();

    const { stdout, stderr } = await execFileAsync(
      prepareExecutablePath,
      [
        "runtime",
        "--muon-path",
        fixture.muonPath,
        "--stage-dir",
        fixture.stageDir,
        "--target",
        "linux64",
        "--cache-dir",
        fixture.cacheDir,
        "--json",
      ],
      {
        encoding: "utf8",
        env: {
          ...process.env,
          MUON_CEF_CATALOG_URL: fixture.catalogPath,
        },
      },
    );

    const result = JSON.parse(stdout) as { stagePath: string };
    const lines = stderr.trim().split(/\r?\n/);
    expect(result.stagePath).toBe(fixture.stageDir);
    expect(lines[0]).toMatch(/^muon-prepare: .+-[0-9a-f]+: Started\.$/);
    expect(stderr).toContain(
      "Downloading CEF binary: version=fake-cef target=linux64 distribution=minimal",
    );
    expect(stderr).toContain(
      "CEF binary downloaded: version=fake-cef target=linux64 distribution=minimal",
    );
    expect(stderr).toContain(
      "CEF files copied to staging: version=fake-cef files=3",
    );
    expect(stderr).toContain("Muon files copied to staging: files=4");
  });

  it("does not write progress messages when native quiet mode is enabled", async () => {
    const fixture = await createPrepareFixture();

    const { stdout, stderr } = await execFileAsync(
      prepareExecutablePath,
      [
        "runtime",
        "--muon-path",
        fixture.muonPath,
        "--stage-dir",
        fixture.stageDir,
        "--target",
        "linux64",
        "--cache-dir",
        fixture.cacheDir,
        "-q",
        "--json",
      ],
      {
        encoding: "utf8",
        env: {
          ...process.env,
          MUON_CEF_CATALOG_URL: fixture.catalogPath,
        },
      },
    );

    const result = JSON.parse(stdout) as { stagePath: string };
    expect(result.stagePath).toBe(fixture.stageDir);
    expect(stderr).toBe("");
  });

  it("forwards progress messages from the TypeScript wrapper", async () => {
    const fixture = await createPrepareFixture();
    const chunks: string[] = [];
    const write = vi.spyOn(process.stderr, "write").mockImplementation(((
      chunk: string | Uint8Array,
    ) => {
      chunks.push(
        typeof chunk === "string" ? chunk : Buffer.from(chunk).toString("utf8"),
      );
      return true;
    }) as typeof process.stderr.write);
    try {
      await prepareFixture(fixture, {
        cefPath: undefined,
        stageDir: fixture.stageDir,
        force: false,
        quiet: false,
      });
    } finally {
      write.mockRestore();
    }

    const stderr = chunks.join("");
    expect(stderr).toContain(
      "Downloading CEF binary: version=fake-cef target=linux64 distribution=minimal",
    );
    expect(stderr).toContain("Muon files copied to staging: files=4");
  });

  it("does not forward progress messages from the TypeScript wrapper in quiet mode", async () => {
    const fixture = await createPrepareFixture();
    const chunks: string[] = [];
    const write = vi.spyOn(process.stderr, "write").mockImplementation(((
      chunk: string | Uint8Array,
    ) => {
      chunks.push(
        typeof chunk === "string" ? chunk : Buffer.from(chunk).toString("utf8"),
      );
      return true;
    }) as typeof process.stderr.write);
    try {
      await prepareFixture(fixture, {
        cefPath: undefined,
        stageDir: fixture.stageDir,
        force: false,
        quiet: true,
      });
    } finally {
      write.mockRestore();
    }

    expect(chunks).toEqual([]);
  });

  it("adds .muon to gitignore when staging under the project .muon directory", async () => {
    const fixture = await createPrepareFixture();

    await prepareFixture(fixture);

    await expect(
      readFile(join(fixture.projectPath, ".gitignore"), "utf8"),
    ).resolves.toBe(".muon/\n");
  });

  it("does not duplicate an existing .muon gitignore entry", async () => {
    const fixture = await createPrepareFixture();
    await writeFile(join(fixture.projectPath, ".gitignore"), "dist/\n.muon/\n");

    await prepareFixture(fixture);

    await expect(
      readFile(join(fixture.projectPath, ".gitignore"), "utf8"),
    ).resolves.toBe("dist/\n.muon/\n");
  });

  it("stages a flat CEF cache directory", async () => {
    const fixture = await createPrepareFixture();
    const flatCefPath = await createFakeCefDirectory("flat");

    const result = await prepareFixture(fixture, {
      cefPath: flatCefPath,
      stageDir: fixture.stageDir,
      force: false,
      quiet: false,
    });
    const stagePath = requireStagePath(result);

    expect(result.cefPath).toBe(flatCefPath);
    await expect(access(join(stagePath, "libcef.so"))).resolves.toBeUndefined();
    await expect(
      access(join(stagePath, "locales", "en-US.pak")),
    ).resolves.toBeUndefined();
  });

  it("rejects a CEF archive with a SHA1 mismatch", async () => {
    const fixture = await createPrepareFixture(
      "0000000000000000000000000000000000000000",
      [
        {
          version: "fake-cef-download",
          chromiumVersion: "100.0.0.0",
          archive: getEmbeddedCefArchive(),
        },
      ],
    );
    await writeBootstrapIni(
      fixture.muonPath,
      `[cef]
versionPolicy=exact
exactVersion=fake-cef-download
catalogRefreshIntervalSeconds=604800
lastCatalogUpdateUnix=0
`,
    );

    await expect(
      prepareFixture(fixture, {
        cefPath: undefined,
        stageDir: fixture.stageDir,
        force: false,
        quiet: false,
      }),
    ).rejects.toThrow(/sha1 mismatch/);
  });

  it("returns the same staged runtime for concurrent calls", async () => {
    const fixture = await createPrepareFixture();

    const results = await Promise.all([
      prepareFixture(fixture),
      prepareFixture(fixture),
    ]);

    expect(results[0]?.stagePath).toBe(results[1]?.stagePath);
    await expect(
      access(join(results[0]?.stagePath ?? "", ".muon-ready.json")),
    ).resolves.toBeUndefined();
  });

  it("reuses the staged runtime while its ready marker matches", async () => {
    const fixture = await createPrepareFixture();
    const first = await prepareFixture(fixture);
    const stagePath = requireStagePath(first);
    await writeFile(join(stagePath, "cached-only.txt"), "cached\n");

    const second = await prepareFixture(fixture);

    expect(second.stagePath).toBe(stagePath);
    expect(second.cacheHit).toBe(true);
    await expect(
      readFile(join(stagePath, "cached-only.txt"), "utf8"),
    ).resolves.toBe("cached\n");
  });

  it("reuses the staged runtime when source mtimes change without content changes", async () => {
    const fixture = await createPrepareFixture();
    const first = await prepareFixture(fixture);
    const stagePath = requireStagePath(first);
    await writeFile(join(stagePath, "cached-only.txt"), "cached\n");
    const sourcePath = join(fixture.muonPath, "muon-core");
    const timestamp = new Date("2030-01-01T00:00:00.000Z");
    await utimes(sourcePath, timestamp, timestamp);

    const second = await prepareFixture(fixture);

    expect(second.stagePath).toBe(stagePath);
    expect(second.cacheHit).toBe(true);
    await expect(
      readFile(join(stagePath, "cached-only.txt"), "utf8"),
    ).resolves.toBe("cached\n");
  });

  it("rebuilds the staged runtime when source content changes with the same mtime", async () => {
    const fixture = await createPrepareFixture();
    const sourcePath = join(fixture.muonPath, "muon-core");
    const timestamp = new Date("2024-01-01T00:00:00.000Z");
    await utimes(sourcePath, timestamp, timestamp);
    const first = await prepareFixture(fixture);
    const stagePath = requireStagePath(first);
    await writeFile(join(stagePath, "cached-only.txt"), "cached\n");
    await writeFile(sourcePath, "core v2\n");
    await chmod(sourcePath, 0o755);
    await utimes(sourcePath, timestamp, timestamp);

    const second = await prepareFixture(fixture);

    expect(second.stagePath).toBe(stagePath);
    expect(second.cacheHit).toBe(false);
    await expect(readFile(join(stagePath, "muon-core"), "utf8")).resolves.toBe(
      "core v2\n",
    );
    await expect(access(join(stagePath, "cached-only.txt"))).rejects.toThrow();
  });

  it("rebuilds the staging directory when force is enabled", async () => {
    const fixture = await createPrepareFixture();
    const first = await prepareFixture(fixture);
    await writeFile(join(fixture.muonPath, "muon-core"), "core v2\n");
    await chmod(join(fixture.muonPath, "muon-core"), 0o755);

    const second = await prepareFixture(fixture, {
      cefPath: fixture.cefPath,
      stageDir: fixture.stageDir,
      force: true,
      quiet: false,
    });

    expect(second.stagePath).toBe(first.stagePath);
    const stagePath = requireStagePath(second);
    await expect(readFile(join(stagePath, "muon-core"), "utf8")).resolves.toBe(
      "core v2\n",
    );
  });

  it("is available through the muon CLI prepare command", async () => {
    const fixture = await createPrepareFixture();
    const cliPath = resolve("dist", "cli.cjs");
    const { stdout } = await execFileAsync(
      process.execPath,
      [
        cliPath,
        "prepare",
        "--muon-path",
        fixture.muonPath,
        "--cef-path",
        fixture.cefPath,
        "--stage-dir",
        fixture.stageDir,
        "--target",
        "linux64",
        "--cache-dir",
        fixture.cacheDir,
        "--json",
      ],
      {
        encoding: "utf8",
        env: {
          ...process.env,
          MUON_CEF_CATALOG_URL: fixture.catalogPath,
          MUON_PREPARE_PATH: prepareExecutablePath,
        },
      },
    );

    const result = JSON.parse(stdout) as { stagePath: string };
    await expect(
      access(join(result.stagePath, "muon-core")),
    ).resolves.toBeUndefined();
  });

  it("does not write progress messages from the muon CLI in quiet mode", async () => {
    const fixture = await createPrepareFixture();
    const cliPath = resolve("dist", "cli.cjs");
    const { stdout, stderr } = await execFileAsync(
      process.execPath,
      [
        cliPath,
        "prepare",
        "--muon-path",
        fixture.muonPath,
        "--stage-dir",
        fixture.stageDir,
        "--target",
        "linux64",
        "--cache-dir",
        fixture.cacheDir,
        "--quiet",
        "--json",
      ],
      {
        encoding: "utf8",
        env: {
          ...process.env,
          MUON_CEF_CATALOG_URL: fixture.catalogPath,
          MUON_PREPARE_PATH: prepareExecutablePath,
        },
      },
    );

    const result = JSON.parse(stdout) as { stagePath: string };
    expect(result.stagePath).toBe(fixture.stageDir);
    expect(stderr).toBe("");
  });

  it("bootstraps an in-place portable runtime and forwards the core exit code", async () => {
    const fixture = await createPrepareFixture();
    const outputDirectory = await createTemporaryDirectory(
      "muon-bootstrap-output-",
    );
    const escapedOutput = outputDirectory.replaceAll("'", "'\\''");
    await writeFile(
      join(fixture.muonPath, "muon-core"),
      `#!/usr/bin/env bash
set -euo pipefail
printf '%s\\n' "$@" > '${escapedOutput}/args.txt'
pwd > '${escapedOutput}/cwd.txt'
exit 17
`,
    );
    await chmod(join(fixture.muonPath, "muon-core"), 0o755);
    const appBootstrapPath = join(fixture.muonPath, "myapp");
    await copyFile(bootstrapExecutablePath, appBootstrapPath);
    await chmod(appBootstrapPath, 0o755);

    await expect(
      execFileAsync(appBootstrapPath, ["--alpha", "two words"], {
        cwd: fixture.projectPath,
        encoding: "utf8",
        env: {
          ...process.env,
          MUON_CACHE_DIR: fixture.cacheDir,
          MUON_CEF_CATALOG_URL: fixture.catalogPath,
        },
      }),
    ).rejects.toMatchObject({ code: 17 });

    await expect(
      readFile(join(outputDirectory, "args.txt"), "utf8"),
    ).resolves.toBe("--alpha\ntwo words\n");
    await expect(
      readFile(join(outputDirectory, "cwd.txt"), "utf8"),
    ).resolves.toBe(`${fixture.muonPath}\n`);
    await expect(
      access(join(fixture.muonPath, "libcef.so")),
    ).resolves.toBeUndefined();
    await expect(
      access(join(fixture.muonPath, "icudtl.dat")),
    ).resolves.toBeUndefined();
    await expect(
      access(join(fixture.muonPath, "locales", "en-US.pak")),
    ).resolves.toBeUndefined();
  });

  it("uses embedded defaultVersionPolicy when bootstrap ini omits versionPolicy", async () => {
    const compatible = await createFakeCefArchive(createTemporaryDirectory, {
      archiveFileName: "cef-bootstrap-compatible.tar.bz2",
      archiveRootName: "cef_binary_bootstrap_compatible_linux64_minimal",
      libcefContent: "bootstrap compatible cef library\n",
    });
    const fixture = await createPrepareFixture(undefined, [
      {
        version: "bootstrap-compatible",
        chromiumVersion: "199.0.0.0",
        archive: compatible,
      },
      {
        version: "fake-cef",
        chromiumVersion: "100.0.0.0",
        archive: getEmbeddedCefArchive(),
      },
    ]);
    await writeBootstrapIni(
      fixture.muonPath,
      `[cef]
catalogRefreshIntervalSeconds=604800
lastCatalogUpdateUnix=0
`,
    );
    const configPath = join(fixture.projectPath, "muon.json");
    await writeFile(
      configPath,
      `{ "bootstrap": { "defaultVersionPolicy": "compat-latest" } }\n`,
    );
    const outputDirectory = await createTemporaryDirectory(
      "muon-bootstrap-output-",
    );
    const escapedOutput = outputDirectory.replaceAll("'", "'\\''");
    await writeFile(
      join(fixture.muonPath, "muon-core"),
      `#!/usr/bin/env bash
set -euo pipefail
pwd > '${escapedOutput}/cwd.txt'
exit 17
`,
    );
    await chmod(join(fixture.muonPath, "muon-core"), 0o755);
    const appBootstrapPath = join(fixture.muonPath, "myapp");
    await embedMuonConfigInBootstrapFile({
      bootstrapPath: bootstrapExecutablePath,
      configPath,
      outputPath: appBootstrapPath,
    });
    await chmod(appBootstrapPath, 0o755);

    await expect(
      execFileAsync(appBootstrapPath, [], {
        cwd: fixture.projectPath,
        encoding: "utf8",
        env: {
          ...process.env,
          MUON_CACHE_DIR: fixture.cacheDir,
          MUON_CEF_CATALOG_URL: fixture.catalogPath,
        },
      }),
    ).rejects.toMatchObject({ code: 17 });

    await expect(
      readFile(join(fixture.muonPath, "libcef.so"), "utf8"),
    ).resolves.toBe("bootstrap compatible cef library\n");
    await expect(
      readFile(join(fixture.muonPath, "muon-bootstrap.ini"), "utf8"),
    ).resolves.not.toContain("versionPolicy=");
  });

  it("rejects an invalid embedded defaultVersionPolicy during bootstrap", async () => {
    const fixture = await createPrepareFixture();
    const configPath = join(fixture.projectPath, "muon.json");
    await writeFile(
      configPath,
      `{ "bootstrap": { "defaultVersionPolicy": "invalid" } }\n`,
    );
    const appBootstrapPath = join(fixture.muonPath, "myapp");
    await embedMuonConfigInBootstrapFile({
      bootstrapPath: bootstrapExecutablePath,
      configPath,
      outputPath: appBootstrapPath,
    });
    await chmod(appBootstrapPath, 0o755);

    await expect(
      execFileAsync(appBootstrapPath, [], {
        cwd: fixture.projectPath,
        encoding: "utf8",
        env: {
          ...process.env,
          MUON_CACHE_DIR: fixture.cacheDir,
          MUON_CEF_CATALOG_URL: fixture.catalogPath,
        },
      }),
    ).rejects.toThrow(/defaultVersionPolicy|CEF version policy/);
  });
});
