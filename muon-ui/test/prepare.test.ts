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
import { dirname, join, resolve } from "node:path";
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
  buildTestMuonBuilder,
  createRuntimeInfoHeader,
} from "./test-muon-builder.js";

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
  const root = await createDirectory("muon-builder-cef-");
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
  const buildRoot = await createSuiteTemporaryDirectory("muon-builder-native-");
  const binaries = await buildTestMuonBuilder(
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
  const cefPath = await createTemporaryDirectory("muon-builder-cef-dir-");
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
  const muonPath = await createTemporaryDirectory("muon-builder-muon-");
  const cefPath = await createFakeCefDirectory("releaseResources");
  const cacheDir = await createTemporaryDirectory("muon-builder-cache-");
  const sourceDir = await createTemporaryDirectory("muon-builder-source-");
  const projectPath = await createTemporaryDirectory("muon-builder-project-");
  const stageDir = join(projectPath, ".muon", "linux-amd64");
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

const writeEmbeddedBootstrap = async (
  fixture: PrepareFixture,
  config: Record<string, unknown>,
  launcherName = "myapp",
): Promise<string> => {
  const configPath = join(fixture.projectPath, `${launcherName}.json`);
  const appBootstrapPath = join(fixture.muonPath, launcherName);
  await writeFile(configPath, `${JSON.stringify(config, null, 2)}\n`);
  await embedMuonConfigInBootstrapFile({
    bootstrapPath: bootstrapExecutablePath,
    configPath,
    outputPath: appBootstrapPath,
  });
  await chmod(appBootstrapPath, 0o755);
  return appBootstrapPath;
};

const getLinuxPortableStateRuntimePath = (
  stateHome: string,
  appId: string,
): string => join(stateHome, appId, "linux-amd64");

const getUserDesktopEntryPath = (dataHome: string, desktopId: string): string =>
  join(dataHome, "applications", `${desktopId}.desktop`);

const writeLinuxDesktopFiles = async (
  runtimePath: string,
  desktop: {
    desktopId: string;
    name: string;
    comment: string;
    categories: readonly string[];
    startupNotify: boolean;
    iconFileName?: string;
  },
): Promise<void> => {
  const iconFileName = desktop.iconFileName ?? "muon-desktop-icon.png";
  await writeFile(
    join(runtimePath, "muon-desktop.json"),
    `${JSON.stringify(
      {
        desktopId: desktop.desktopId,
        name: desktop.name,
        comment: desktop.comment,
        categories: desktop.categories,
        startupNotify: desktop.startupNotify,
        iconFileName,
      },
      null,
      2,
    )}\n`,
  );
  await writeFile(join(runtimePath, iconFileName), "desktop icon\n");
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
    target: "linux-amd64",
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

const sanitizePrepareLockKey = (value: string): string =>
  value.replace(/[^A-Za-z0-9._-]/g, "_");

const buildProgressHarness = async (root: string): Promise<string> => {
  const harnessPath = join(root, "prepare-progress-harness.c");
  const executablePath = join(root, "prepare-progress-harness");
  const prepareRoot = resolve("..", "muon-builder");
  const bzip2Lib = join(
    prepareRoot,
    ".deps",
    "build",
    "bzip2-linux64",
    "libbz2.a",
  );
  const libarchiveLib = join(
    prepareRoot,
    ".deps",
    "build",
    "libarchive-linux64",
    "libarchive",
    "libarchive.a",
  );
  await writeFile(
    harnessPath,
    `#include <stdio.h>
#include "prepare.h"

static const char *phase_name(MuonPrepareProgressPhase phase) {
  switch (phase) {
    case MUON_PREPARE_PROGRESS_PHASE_DOWNLOADING:
      return "download";
    case MUON_PREPARE_PROGRESS_PHASE_VERIFYING:
      return "verify";
    case MUON_PREPARE_PROGRESS_PHASE_INSTALLING:
      return "install";
    case MUON_PREPARE_PROGRESS_PHASE_FINALIZING:
      return "finalize";
    case MUON_PREPARE_PROGRESS_PHASE_DONE:
      return "done";
    case MUON_PREPARE_PROGRESS_PHASE_FAILED:
      return "failed";
    default:
      return "other";
  }
}

static void on_progress(const MuonPrepareProgress *progress, void *user_data) {
  (void)user_data;
  printf("%s|%s|%d|%llu|%llu\\n",
         phase_name(progress->phase),
         progress->status == NULL ? "" : progress->status,
         progress->determinate,
         progress->current,
         progress->total);
}

int main(int argc, char **argv) {
  if (argc != 4) {
    return 64;
  }
  return muon_prepare_in_place_with_progress(
      argv[1], argv[2], argv[3], 0, 1, on_progress, NULL);
}
`,
  );
  await execFileAsync("gcc", [
    harnessPath,
    "-std=c99",
    "-Wall",
    "-Wextra",
    "-pedantic",
    "-I",
    join(prepareRoot, "src"),
    "-o",
    executablePath,
    join(dirname(prepareExecutablePath), "libmuon-builder.a"),
    libarchiveLib,
    bzip2Lib,
  ]);
  return executablePath;
};

const runProgressHarness = async (
  harnessPath: string,
  fixture: PrepareFixture,
): Promise<string[]> => {
  const { stdout } = await execFileAsync(
    harnessPath,
    [fixture.muonPath, "linux-amd64", fixture.cacheDir],
    {
      encoding: "utf8",
      env: {
        ...process.env,
        MUON_CEF_CATALOG_URL: fixture.catalogPath,
      },
    },
  );
  return stdout.trim() === "" ? [] : stdout.trim().split(/\r?\n/);
};

const findCommand = async (name: string): Promise<string | undefined> => {
  try {
    const { stdout } = await execFileAsync(
      "bash",
      ["-lc", `command -v ${name}`],
      {
        encoding: "utf8",
      },
    );
    const path = stdout.trim();
    return path === "" ? undefined : path;
  } catch {
    return undefined;
  }
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
    expect(getDefaultMuonPrepareTarget("linux", "x64")).toBe("linux-amd64");
    expect(getDefaultMuonPrepareTarget("linux", "arm")).toBe("linux-armhf");
    expect(getDefaultMuonPrepareTarget("linux", "arm64")).toBe("linux-arm64");
    expect(getDefaultMuonPrepareTarget("win32", "ia32")).toBe("windows-i686");
    expect(getDefaultMuonPrepareTarget("win32", "x64")).toBe("windows-amd64");
  });

  it("rejects unsupported Linux i686 targets", () => {
    expect(() => getDefaultMuonPrepareTarget("linux", "ia32")).toThrow(
      "Unsupported Muon prepare target: platform=linux, arch=ia32",
    );
  });

  it("rejects CEF-derived target IDs in public prepare options", async () => {
    await expect(
      runMuonPrepare({
        muonPath: "/tmp/muon-runtime",
        cefPath: undefined,
        stageDir: undefined,
        target: "linux64",
        cacheDir: undefined,
        force: false,
        quiet: true,
        prepareExecutablePath: "/tmp/muon-builder",
        environment: process.env,
        cwd: process.cwd(),
      }),
    ).rejects.toThrow("Unsupported Muon prepare target: linux64");
  });
});

describe("muon-builder", () => {
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
      target: "linux-amd64",
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
        "linux-amd64",
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
      cefTarget: string;
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
    expect(result.target).toBe("linux-amd64");
    expect(result.cefTarget).toBe("linux64");
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
        "linux-amd64",
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
            "linux-amd64",
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
      stderr: expect.stringContaining("Usage: muon-builder"),
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
        "linux-amd64",
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
    expect(lines[0]).toMatch(/^muon-builder: .+-[0-9a-f]+: Started\.$/);
    expect(stderr).toContain(
      "Downloading CEF binary: version=fake-cef target=linux-amd64 distribution=minimal",
    );
    expect(stderr).toContain(
      "CEF binary downloaded: version=fake-cef target=linux-amd64 distribution=minimal",
    );
    expect(stderr).toContain("Installing CEF runtime...");
    expect(stderr).toContain(
      "CEF files copied to staging: version=fake-cef files=3",
    );
    expect(stderr).toContain("Muon files copied to staging: files=4");
    expect(stderr).toContain("Starting Muon...");
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
        "linux-amd64",
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
      "Downloading CEF binary: version=fake-cef target=linux-amd64 distribution=minimal",
    );
    expect(stderr).toContain("Installing CEF runtime...");
    expect(stderr).toContain("Muon files copied to staging: files=4");
    expect(stderr).toContain("Starting Muon...");
  });

  it("renders TypeScript wrapper status messages with a spinner on TTY", async () => {
    const fixture = await createPrepareFixture();
    const chunks: string[] = [];
    const isTtyDescriptor = Object.getOwnPropertyDescriptor(
      process.stderr,
      "isTTY",
    );
    Object.defineProperty(process.stderr, "isTTY", {
      configurable: true,
      value: true,
    });
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
      if (isTtyDescriptor === undefined) {
        delete (process.stderr as { isTTY?: boolean }).isTTY;
      } else {
        Object.defineProperty(process.stderr, "isTTY", isTtyDescriptor);
      }
    }

    const stderr = chunks.join("");
    expect(stderr).toContain("\r- Downloading CEF binary:");
    expect(stderr).toContain("\r- Installing CEF runtime...");
    const muonCopiedIndex = stderr.indexOf(
      "Muon files copied to staging: files=4",
    );
    const cefCopiedIndex = stderr.indexOf("CEF files copied to staging:");
    expect(muonCopiedIndex).toBeGreaterThanOrEqual(0);
    expect(cefCopiedIndex).toBeGreaterThan(muonCopiedIndex);
    expect(stderr.slice(muonCopiedIndex, cefCopiedIndex)).toMatch(
      /\r[-\\|/] Installing CEF runtime\.\.\./u,
    );
    expect(stderr).toContain("Starting Muon...");
  });

  it("reports structured progress for bootstrap in-place CEF preparation", async () => {
    const fixture = await createPrepareFixture();
    const harnessPath = await buildProgressHarness(fixture.projectPath);

    const firstRun = await runProgressHarness(harnessPath, fixture);
    const firstPhases = firstRun.map((line) => line.split("|")[0] ?? "");

    expect(firstPhases).toEqual(
      expect.arrayContaining(["download", "verify", "install", "finalize"]),
    );
    expect(firstRun).toContainEqual(
      expect.stringContaining("|Downloading CEF runtime...|"),
    );
    expect(firstRun).toContainEqual(
      expect.stringContaining("|Verifying download...|"),
    );
    expect(firstRun).toContainEqual(
      expect.stringContaining("|Installing CEF runtime...|"),
    );
    expect(firstRun).toContainEqual(
      expect.stringContaining("|Starting Muon...|"),
    );

    await expect(
      access(join(fixture.muonPath, "libcef.so")),
    ).resolves.toBeUndefined();

    await expect(runProgressHarness(harnessPath, fixture)).resolves.toEqual([]);
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

  it("adds Muon generated directories to gitignore when staging under the project .muon directory", async () => {
    const fixture = await createPrepareFixture();

    await prepareFixture(fixture);

    await expect(
      readFile(join(fixture.projectPath, ".gitignore"), "utf8"),
    ).resolves.toBe(".muon/\ndist-muon/\nartifacts/\n");
  });

  it("appends a missing Muon dist gitignore entry without duplicating .muon", async () => {
    const fixture = await createPrepareFixture();
    await writeFile(
      join(fixture.projectPath, ".gitignore"),
      "dist*/\n.muon/\n",
    );

    await prepareFixture(fixture);

    await expect(
      readFile(join(fixture.projectPath, ".gitignore"), "utf8"),
    ).resolves.toBe("dist*/\n.muon/\ndist-muon/\nartifacts/\n");
  });

  it("adds the current Muon dist gitignore entry when a legacy entry exists", async () => {
    const fixture = await createPrepareFixture();
    await writeFile(
      join(fixture.projectPath, ".gitignore"),
      "dist*/\n.muon/\ndist-muon-*/\n",
    );

    await prepareFixture(fixture);

    await expect(
      readFile(join(fixture.projectPath, ".gitignore"), "utf8"),
    ).resolves.toBe("dist*/\n.muon/\ndist-muon-*/\ndist-muon/\nartifacts/\n");
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

  it("recovers an abandoned staging lock from an interrupted prepare", async () => {
    const fixture = await createPrepareFixture();
    const lockPath = join(
      dirname(fixture.stageDir),
      `.muon-stage-${sanitizePrepareLockKey(fixture.stageDir)}.lock`,
    );
    await mkdir(lockPath, { recursive: true });
    const staleTime = new Date(Date.now() - 60_000);
    await utimes(lockPath, staleTime, staleTime);

    const { stdout } = await execFileAsync(
      prepareExecutablePath,
      [
        "runtime",
        "--muon-path",
        fixture.muonPath,
        "--cef-path",
        fixture.cefPath,
        "--stage-dir",
        fixture.stageDir,
        "--target",
        "linux-amd64",
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
        },
        timeout: 10_000,
      },
    );

    const result = JSON.parse(stdout) as { stagePath: string };
    expect(result.stagePath).toBe(fixture.stageDir);
    await expect(
      access(join(fixture.stageDir, "muon-core")),
    ).resolves.toBeUndefined();
    await expect(access(lockPath)).rejects.toThrow();
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
        "linux-amd64",
        "--cache-dir",
        fixture.cacheDir,
        "--json",
      ],
      {
        encoding: "utf8",
        env: {
          ...process.env,
          MUON_CEF_CATALOG_URL: fixture.catalogPath,
          MUON_BUILDER_PATH: prepareExecutablePath,
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
        "linux-amd64",
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
          MUON_BUILDER_PATH: prepareExecutablePath,
        },
      },
    );

    const result = JSON.parse(stdout) as { stagePath: string };
    expect(result.stagePath).toBe(fixture.stageDir);
    expect(stderr).toBe("");
  });

  it("bootstraps a portable runtime in the user state directory and forwards the core exit code", async () => {
    const fixture = await createPrepareFixture();
    const stateHome = await createTemporaryDirectory("muon-bootstrap-state-");
    const dataHome = await createTemporaryDirectory("muon-bootstrap-data-");
    const appId = "scope.sample-app";
    const stateRuntimePath = getLinuxPortableStateRuntimePath(stateHome, appId);
    const desktopEntryPath = getUserDesktopEntryPath(dataHome, appId);
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
    await writeLinuxDesktopFiles(fixture.muonPath, {
      desktopId: appId,
      name: "Sample App",
      comment: "Sample comment",
      categories: ["Utility", "Development"],
      startupNotify: false,
    });
    const appBootstrapPath = await writeEmbeddedBootstrap(fixture, {
      bootstrap: { appId },
    });

    await expect(
      execFileAsync(appBootstrapPath, ["--alpha", "two words"], {
        cwd: fixture.projectPath,
        encoding: "utf8",
        env: {
          ...process.env,
          MUON_CACHE_DIR: fixture.cacheDir,
          MUON_CEF_CATALOG_URL: fixture.catalogPath,
          XDG_STATE_HOME: stateHome,
          XDG_DATA_HOME: dataHome,
        },
      }),
    ).rejects.toMatchObject({ code: 17 });

    await expect(
      readFile(join(outputDirectory, "args.txt"), "utf8"),
    ).resolves.toBe("--alpha\ntwo words\n");
    await expect(
      readFile(join(outputDirectory, "cwd.txt"), "utf8"),
    ).resolves.toBe(`${stateRuntimePath}\n`);
    await expect(
      readFile(join(stateRuntimePath, "assets", "app.txt"), "utf8"),
    ).resolves.toBe("app asset\n");
    await expect(
      readFile(join(stateRuntimePath, "libcef.so"), "utf8"),
    ).resolves.toBe("cef library\n");
    await expect(access(join(fixture.muonPath, "libcef.so"))).rejects.toThrow();
    await expect(
      access(join(fixture.muonPath, "icudtl.dat")),
    ).rejects.toThrow();
    await expect(
      access(join(fixture.muonPath, "locales", "en-US.pak")),
    ).rejects.toThrow();
    await expect(readFile(desktopEntryPath, "utf8")).resolves.toBe(
      [
        "[Desktop Entry]",
        "Type=Application",
        "Name=Sample App",
        "Comment=Sample comment",
        `Exec="${stateRuntimePath}/myapp" --muon-launch-from=normal`,
        `TryExec=${stateRuntimePath}/myapp`,
        `Icon=${stateRuntimePath}/muon-desktop-icon.png`,
        "Terminal=false",
        "Categories=Utility;Development;",
        "StartupNotify=false",
        "StartupWMClass=scope.sample-app",
        "X-Muon-Managed=true",
        "",
      ].join("\n"),
    );

    await writeFile(
      join(stateRuntimePath, "muon-core"),
      `#!/usr/bin/env bash
set -euo pipefail
printf 'state launcher\\n' > '${escapedOutput}/state-launcher.txt'
exit 19
`,
    );
    await chmod(join(stateRuntimePath, "muon-core"), 0o755);
    await expect(
      execFileAsync(join(stateRuntimePath, "myapp"), [], {
        cwd: fixture.projectPath,
        encoding: "utf8",
        env: {
          ...process.env,
          XDG_STATE_HOME: stateHome,
          XDG_DATA_HOME: dataHome,
        },
      }),
    ).rejects.toMatchObject({ code: 19 });
    await expect(
      readFile(join(outputDirectory, "state-launcher.txt"), "utf8"),
    ).resolves.toBe("state launcher\n");
  });

  it("updates the staged runtime and desktop entry from a newer portable distribution", async () => {
    const firstFixture = await createPrepareFixture();
    const secondFixture = await createPrepareFixture();
    const stateHome = await createTemporaryDirectory("muon-bootstrap-state-");
    const dataHome = await createTemporaryDirectory("muon-bootstrap-data-");
    const appId = "scope.update-app";
    const stateRuntimePath = getLinuxPortableStateRuntimePath(stateHome, appId);
    const desktopEntryPath = getUserDesktopEntryPath(dataHome, appId);
    const outputDirectory = await createTemporaryDirectory(
      "muon-bootstrap-output-",
    );
    const escapedOutput = outputDirectory.replaceAll("'", "'\\''");
    await writeFile(
      join(firstFixture.muonPath, "muon-core"),
      `#!/usr/bin/env bash
set -euo pipefail
printf 'v1\\n' > '${escapedOutput}/version.txt'
exit 17
`,
    );
    await chmod(join(firstFixture.muonPath, "muon-core"), 0o755);
    await writeLinuxDesktopFiles(firstFixture.muonPath, {
      desktopId: appId,
      name: "Update App v1",
      comment: "",
      categories: ["Utility"],
      startupNotify: true,
    });
    const firstBootstrapPath = await writeEmbeddedBootstrap(firstFixture, {
      bootstrap: { appId },
    });

    await expect(
      execFileAsync(firstBootstrapPath, [], {
        cwd: firstFixture.projectPath,
        encoding: "utf8",
        env: {
          ...process.env,
          MUON_CACHE_DIR: firstFixture.cacheDir,
          MUON_CEF_CATALOG_URL: firstFixture.catalogPath,
          XDG_STATE_HOME: stateHome,
          XDG_DATA_HOME: dataHome,
        },
      }),
    ).rejects.toMatchObject({ code: 17 });
    await expect(
      readFile(join(outputDirectory, "version.txt"), "utf8"),
    ).resolves.toBe("v1\n");

    await writeFile(
      join(secondFixture.muonPath, "muon-core"),
      `#!/usr/bin/env bash
set -euo pipefail
printf 'v2\\n' > '${escapedOutput}/version.txt'
exit 23
`,
    );
    await chmod(join(secondFixture.muonPath, "muon-core"), 0o755);
    await writeFile(
      join(secondFixture.muonPath, "assets", "app.txt"),
      "app asset v2\n",
    );
    await writeLinuxDesktopFiles(secondFixture.muonPath, {
      desktopId: appId,
      name: "Update App v2",
      comment: "",
      categories: ["Utility", "Development"],
      startupNotify: true,
    });
    const secondBootstrapPath = await writeEmbeddedBootstrap(secondFixture, {
      bootstrap: { appId },
    });

    await expect(
      execFileAsync(secondBootstrapPath, [], {
        cwd: secondFixture.projectPath,
        encoding: "utf8",
        env: {
          ...process.env,
          MUON_CACHE_DIR: secondFixture.cacheDir,
          MUON_CEF_CATALOG_URL: secondFixture.catalogPath,
          XDG_STATE_HOME: stateHome,
          XDG_DATA_HOME: dataHome,
        },
      }),
    ).rejects.toMatchObject({ code: 23 });
    await expect(
      readFile(join(outputDirectory, "version.txt"), "utf8"),
    ).resolves.toBe("v2\n");
    await expect(
      readFile(join(stateRuntimePath, "assets", "app.txt"), "utf8"),
    ).resolves.toBe("app asset v2\n");
    await expect(readFile(desktopEntryPath, "utf8")).resolves.toContain(
      "Name=Update App v2\n",
    );
    await expect(readFile(desktopEntryPath, "utf8")).resolves.toContain(
      "Categories=Utility;Development;\n",
    );
  });

  it("does not create user desktop entries for deb installs and updates existing managed entries", async () => {
    const fixture = await createPrepareFixture();
    const stateHome = await createTemporaryDirectory("muon-bootstrap-state-");
    const dataHome = await createTemporaryDirectory("muon-bootstrap-data-");
    const appId = "scope.deb-app";
    const desktopId = "scope.deb-app";
    const desktopEntryPath = getUserDesktopEntryPath(dataHome, desktopId);
    await writeFile(
      join(fixture.muonPath, "muon-core"),
      `#!/usr/bin/env bash
set -euo pipefail
exit 17
`,
    );
    await chmod(join(fixture.muonPath, "muon-core"), 0o755);
    await writeLinuxDesktopFiles(fixture.muonPath, {
      desktopId,
      name: "Deb App",
      comment: "Deb comment",
      categories: ["Utility"],
      startupNotify: true,
    });
    await writeFile(
      join(fixture.muonPath, "muon-install.json"),
      `${JSON.stringify(
        {
          type: "deb",
          packageName: "deb-app",
          launcherPath: "/usr/bin/deb-app",
        },
        null,
        2,
      )}\n`,
    );
    const appBootstrapPath = await writeEmbeddedBootstrap(fixture, {
      bootstrap: { appId },
    });
    const env = {
      ...process.env,
      MUON_CACHE_DIR: fixture.cacheDir,
      MUON_CEF_CATALOG_URL: fixture.catalogPath,
      XDG_STATE_HOME: stateHome,
      XDG_DATA_HOME: dataHome,
    };

    await expect(
      execFileAsync(appBootstrapPath, [], {
        cwd: fixture.projectPath,
        encoding: "utf8",
        env,
      }),
    ).rejects.toMatchObject({ code: 17 });
    await expect(access(desktopEntryPath)).rejects.toThrow();

    await mkdir(dirname(desktopEntryPath), { recursive: true });
    await writeFile(
      desktopEntryPath,
      "[Desktop Entry]\nName=Unmanaged\nExec=/tmp/old\n",
    );
    await expect(
      execFileAsync(appBootstrapPath, [], {
        cwd: fixture.projectPath,
        encoding: "utf8",
        env,
      }),
    ).rejects.toMatchObject({ code: 17 });
    await expect(readFile(desktopEntryPath, "utf8")).resolves.toBe(
      "[Desktop Entry]\nName=Unmanaged\nExec=/tmp/old\n",
    );

    await writeFile(
      desktopEntryPath,
      "[Desktop Entry]\nName=Managed\nExec=/tmp/old\nX-Muon-Managed=true\n",
    );
    await expect(
      execFileAsync(appBootstrapPath, [], {
        cwd: fixture.projectPath,
        encoding: "utf8",
        env,
      }),
    ).rejects.toMatchObject({ code: 17 });
    await expect(readFile(desktopEntryPath, "utf8")).resolves.toBe(
      [
        "[Desktop Entry]",
        "Type=Application",
        "Name=Deb App",
        "Comment=Deb comment",
        'Exec="/usr/bin/deb-app" --muon-launch-from=normal',
        "TryExec=/usr/bin/deb-app",
        "Icon=scope.deb-app",
        "Terminal=false",
        "Categories=Utility;",
        "StartupNotify=true",
        "StartupWMClass=scope.deb-app",
        "X-Muon-Managed=true",
        "",
      ].join("\n"),
    );
  });

  it("hides bootstrap preparation diagnostics when a progress window is available", async () => {
    const xvfbRun = await findCommand("xvfb-run");
    if (xvfbRun === undefined) {
      return;
    }
    const fixture = await createPrepareFixture();
    const stateHome = await createTemporaryDirectory("muon-bootstrap-state-");
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
    const appBootstrapPath = await writeEmbeddedBootstrap(fixture, {
      bootstrap: { appId: "progress.sample" },
    });

    await expect(
      execFileAsync(
        xvfbRun,
        [
          "-a",
          "node",
          resolve("..", "scripts/run-xvfb-command.mjs"),
          appBootstrapPath,
        ],
        {
          cwd: fixture.projectPath,
          encoding: "utf8",
          env: {
            ...process.env,
            MUON_CACHE_DIR: fixture.cacheDir,
            MUON_CEF_CATALOG_URL: fixture.catalogPath,
            XDG_STATE_HOME: stateHome,
          },
        },
      ),
    ).rejects.toMatchObject({
      code: 17,
      stderr: expect.not.stringContaining("Downloading CEF binary"),
    });
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
    const stateHome = await createTemporaryDirectory("muon-bootstrap-state-");
    const appId = "compat.sample";
    const stateRuntimePath = getLinuxPortableStateRuntimePath(stateHome, appId);
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
    const appBootstrapPath = await writeEmbeddedBootstrap(fixture, {
      bootstrap: {
        appId,
        defaultVersionPolicy: "compat-latest",
      },
    });

    await expect(
      execFileAsync(appBootstrapPath, [], {
        cwd: fixture.projectPath,
        encoding: "utf8",
        env: {
          ...process.env,
          MUON_CACHE_DIR: fixture.cacheDir,
          MUON_CEF_CATALOG_URL: fixture.catalogPath,
          XDG_STATE_HOME: stateHome,
        },
      }),
    ).rejects.toMatchObject({ code: 17 });

    await expect(
      readFile(join(stateRuntimePath, "libcef.so"), "utf8"),
    ).resolves.toBe("bootstrap compatible cef library\n");
    await expect(access(join(fixture.muonPath, "libcef.so"))).rejects.toThrow();
    await expect(
      readFile(join(stateRuntimePath, "muon-bootstrap.ini"), "utf8"),
    ).resolves.not.toContain("versionPolicy=");
  });

  it("rejects an invalid embedded defaultVersionPolicy during bootstrap", async () => {
    const fixture = await createPrepareFixture();
    const stateHome = await createTemporaryDirectory("muon-bootstrap-state-");
    const appBootstrapPath = await writeEmbeddedBootstrap(
      fixture,
      {
        bootstrap: {
          appId: "invalid-policy-app",
          defaultVersionPolicy: "invalid",
        },
      },
      "invalid-policy-app",
    );

    await expect(
      execFileAsync(appBootstrapPath, [], {
        cwd: fixture.projectPath,
        encoding: "utf8",
        env: {
          ...process.env,
          MUON_CACHE_DIR: fixture.cacheDir,
          MUON_CEF_CATALOG_URL: fixture.catalogPath,
          XDG_STATE_HOME: stateHome,
        },
      }),
    ).rejects.toThrow(/defaultVersionPolicy|CEF version policy/);
  });
});
