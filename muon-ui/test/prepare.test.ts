// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

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
import { createServer, type Server } from "node:http";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { promisify } from "node:util";
import { gzipSync } from "node:zlib";

import AdmZip from "adm-zip";
import {
  createDirectoryItem,
  createFileItem,
  createTarPacker,
  type EntryItem,
} from "tar-vern";

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
import {
  createMuonLauncherEmbeddedConfigSlot,
  embedMuonConfigInLauncherFile,
  encodeMuonConfigTlv,
  findMuonLauncherEmbeddedConfigSlot,
} from "../src/embed-config.js";
import {
  buildTestMuonBuilder,
  createRuntimeInfoHeader,
} from "./test-muon-builder.js";

const execFileAsync = promisify(execFile);
const sha256HexPattern = /^[0-9a-f]{64}$/u;
const emptySha256Fingerprint = "0".repeat(64);
const futureCatalogTimestamp = 4_102_444_800;
const runtimeReadyMarkerFileName = ".muon-runtime-ready.json";
const legacyRuntimeReadyMarkerFileName = ".muon-ready.json";
const cleanupDirectories: string[] = [];
const suiteCleanupDirectories: string[] = [];
const nodeDistributionServers: FakeNodeDistributionServer[] = [];
let prepareExecutablePath = "";
let launcherExecutablePath = "";
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

interface FakeNodeArtifact {
  version: string;
  nodeTarget: string;
  catalogFile: string;
  fileName: string;
  content: Buffer;
  sha256: string;
}

interface FakeNodeRelease {
  version: string;
  lts: string | false;
  artifacts: readonly FakeNodeArtifact[];
}

interface FakeNodeDistribution {
  files: ReadonlyMap<string, Buffer>;
}

interface FakeNodeDistributionRequest {
  method: string;
  pathname: string;
}

interface FakeNodeDistributionServer {
  baseUrl: string;
  getRequests(): readonly FakeNodeDistributionRequest[];
  close(): Promise<void>;
}

interface ConcurrentRuntimeDistributionServer {
  baseUrl: string;
  getRequests(): readonly FakeNodeDistributionRequest[];
  catalogBarrierReached(): boolean;
  archiveBarrierReached(): boolean;
  close(): Promise<void>;
}

interface TestNodeRuntimeRequirement {
  required: boolean;
  engineRangeSpecified: boolean;
  engineRange: string;
  comparatorSets: readonly (readonly string[])[];
}

interface FakeTarDirectoryEntry {
  kind: "directory";
  path: string;
  mode: number;
}

interface FakeTarFileEntry {
  kind: "file";
  path: string;
  content: Buffer;
  mode: number;
  symlinkTarget: string | undefined;
}

type FakeTarEntry = FakeTarDirectoryEntry | FakeTarFileEntry;

interface FakeNodeTarArchiveOptions {
  includeLicense: boolean;
  nodeIsSymlink: boolean;
  includeNpmSymlink: boolean;
  includeTraversalEntry: boolean;
}

interface ReadyMarker {
  ready: boolean;
  muonFingerprint: string;
  cefFingerprint: string;
}

interface RuntimeReadyMarker extends ReadyMarker {
  nodeFingerprint: string;
}

const readReadyMarker = async (path: string): Promise<ReadyMarker> =>
  JSON.parse(await readFile(path, "utf8")) as ReadyMarker;

const readRuntimeReadyMarker = async (
  runtimePath: string,
): Promise<RuntimeReadyMarker> =>
  JSON.parse(
    await readFile(join(runtimePath, runtimeReadyMarkerFileName), "utf8"),
  ) as RuntimeReadyMarker;

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

const createFakeNodeArtifact = (
  version: string,
  nodeTarget = "linux-x64",
  catalogFile = "linux-x64",
  content = Buffer.from(`fake Node archive ${version} ${nodeTarget}\n`),
): FakeNodeArtifact => {
  const extension = catalogFile.endsWith("-zip") ? ".zip" : ".tar.gz";
  const fileName = `node-${version}-${nodeTarget}${extension}`;
  return {
    version,
    nodeTarget,
    catalogFile,
    fileName,
    content,
    sha256: createHash("sha256").update(content).digest("hex"),
  };
};

const fakeNodeVersion = "v24.4.0";
const createFakeNodeExecutableContent = (version: string): Buffer =>
  Buffer.from(`#!/bin/sh\nprintf '%s\\n' 'fake-node-${version}'\n`);
const fakeNodeLicenseContent = Buffer.from("Fake Node.js license\n");
const fakeTarDate = new Date("2026-01-01T00:00:00.000Z");

const createFakeTarEntryItems = async function* (
  entries: readonly FakeTarEntry[],
): AsyncGenerator<EntryItem, void, unknown> {
  for (const entry of entries) {
    const metadata = {
      mode: entry.mode,
      uname: "root",
      gname: "root",
      uid: 0,
      gid: 0,
      date: fakeTarDate,
    };
    if (entry.kind === "directory") {
      yield await createDirectoryItem(entry.path, "none", metadata);
    } else {
      yield await createFileItem(entry.path, entry.content, metadata);
    }
  }
};

const collectTarPacker = async (
  entries: readonly FakeTarEntry[],
): Promise<Buffer> => {
  const chunks: Buffer[] = [];
  for await (const chunk of createTarPacker(
    createFakeTarEntryItems(entries),
    "none",
  )) {
    chunks.push(
      Buffer.isBuffer(chunk) ? chunk : Buffer.from(chunk as Uint8Array),
    );
  }
  return Buffer.concat(chunks);
};

const readTarHeaderField = (
  header: Buffer,
  offset: number,
  length: number,
): string => {
  const field = header.subarray(offset, offset + length);
  const terminator = field.indexOf(0);
  return field
    .subarray(0, terminator === -1 ? field.length : terminator)
    .toString("utf8");
};

// tar-vern emits regular files, so zero-length placeholders are rewritten into
// POSIX symlink headers to model the links present in official Node archives.
const replaceTarEntryWithSymlink = (
  archive: Buffer,
  path: string,
  target: string,
): void => {
  if (Buffer.byteLength(target) > 100) {
    throw new Error(`Test symlink target is too long: ${target}`);
  }
  let offset = 0;
  while (offset + 512 <= archive.length) {
    const header = archive.subarray(offset, offset + 512);
    if (header.every((value) => value === 0)) {
      break;
    }
    const name = readTarHeaderField(header, 0, 100);
    const prefix = readTarHeaderField(header, 345, 155);
    const entryPath = prefix === "" ? name : `${prefix}/${name}`;
    const sizeText = readTarHeaderField(header, 124, 12).trim();
    const size = sizeText === "" ? 0 : Number.parseInt(sizeText, 8);
    if (!Number.isSafeInteger(size) || size < 0) {
      throw new Error(`Invalid generated tar size for ${entryPath}`);
    }
    if (entryPath === path) {
      header[156] = "2".charCodeAt(0);
      header.fill(0, 157, 257);
      header.write(target, 157, 100, "utf8");
      header.fill(0x20, 148, 156);
      let checksum = 0;
      for (const value of header.values()) {
        checksum += value;
      }
      const octalChecksum = checksum.toString(8).padStart(6, "0");
      if (octalChecksum.length !== 6) {
        throw new Error(`Generated tar checksum is too large for ${path}`);
      }
      header.write(octalChecksum, 148, 6, "ascii");
      header[154] = 0;
      header[155] = 0x20;
      return;
    }
    offset += 512 + Math.ceil(size / 512) * 512;
  }
  throw new Error(`Generated tar entry was not found: ${path}`);
};

const createFakeNodeTarGzArtifact = async (
  version: string,
  options: FakeNodeTarArchiveOptions,
): Promise<FakeNodeArtifact> => {
  const archiveRoot = `node-${version}-linux-x64`;
  const nodePath = `${archiveRoot}/bin/node`;
  const entries: FakeTarEntry[] = [
    { kind: "directory", path: archiveRoot, mode: 0o700 },
    { kind: "directory", path: `${archiveRoot}/bin`, mode: 0o700 },
    {
      kind: "file",
      path: nodePath,
      content: options.nodeIsSymlink
        ? Buffer.alloc(0)
        : createFakeNodeExecutableContent(version),
      mode: 0o600,
      symlinkTarget: options.nodeIsSymlink ? "../lib/node-real" : undefined,
    },
  ];
  if (options.includeLicense) {
    entries.push({
      kind: "file",
      path: `${archiveRoot}/LICENSE`,
      content: fakeNodeLicenseContent,
      mode: 0o777,
      symlinkTarget: undefined,
    });
  }
  entries.push({
    kind: "file",
    path: `${archiveRoot}/README.md`,
    content: Buffer.from("Ignored Node.js documentation\n"),
    mode: 0o666,
    symlinkTarget: undefined,
  });
  if (options.includeNpmSymlink) {
    entries.push({
      kind: "file",
      path: `${archiveRoot}/bin/npm`,
      content: Buffer.alloc(0),
      mode: 0o777,
      symlinkTarget: "../lib/node_modules/npm/bin/npm-cli.js",
    });
  }
  if (options.includeTraversalEntry) {
    entries.push({
      kind: "file",
      path: `${archiveRoot}/../../node-runtime-escape.txt`,
      content: Buffer.from("must not escape\n"),
      mode: 0o666,
      symlinkTarget: undefined,
    });
  }

  const tar = await collectTarPacker(entries);
  for (const entry of entries) {
    if (entry.kind === "file" && entry.symlinkTarget !== undefined) {
      replaceTarEntryWithSymlink(tar, entry.path, entry.symlinkTarget);
    }
  }
  return createFakeNodeArtifact(
    version,
    "linux-x64",
    "linux-x64",
    gzipSync(tar),
  );
};

const createFakeNodeZipArtifact = (): FakeNodeArtifact => {
  const archiveRoot = `node-${fakeNodeVersion}-win-x64`;
  const nodePath = `${archiveRoot}/node.exe`;
  const zip = new AdmZip();
  zip.addFile(nodePath, Buffer.from("MZfake-node.exe\n"), "", 0o777);
  zip.addFile(`${archiveRoot}/LICENSE`, fakeNodeLicenseContent, "", 0o777);
  zip.addFile(
    `${archiveRoot}/README.md`,
    Buffer.from("Ignored Node.js documentation\n"),
    "",
    0o666,
  );
  const nodeEntry = zip.getEntry(nodePath);
  if (nodeEntry === null || nodeEntry.header.method !== 8) {
    throw new Error("The fake Node.js executable must use ZIP deflate.");
  }
  return createFakeNodeArtifact(
    fakeNodeVersion,
    "win-x64",
    "win-x64-zip",
    zip.toBuffer(),
  );
};

const createFakeNodeRelease = (
  version: string,
  lts: string | false,
  artifact = createFakeNodeArtifact(version),
): FakeNodeRelease => ({
  version,
  lts,
  artifacts: [artifact],
});

const createFakeNodeDistribution = (
  releases: readonly FakeNodeRelease[],
  checksumOverrides: ReadonlyMap<string, string> = new Map(),
): FakeNodeDistribution => {
  const files = new Map<string, Buffer>();
  files.set(
    "/index.json",
    Buffer.from(
      `${JSON.stringify(
        releases.map((release) => ({
          version: release.version,
          date: "2026-01-01",
          files: release.artifacts.map((artifact) => artifact.catalogFile),
          lts: release.lts,
          security: release.version === "v24.4.0",
        })),
        null,
        2,
      )}\n`,
    ),
  );
  for (const release of releases) {
    const checksumContent =
      checksumOverrides.get(release.version) ??
      release.artifacts
        .map((artifact) => `${artifact.sha256}  ${artifact.fileName}\n`)
        .join("");
    files.set(
      `/${release.version}/SHASUMS256.txt`,
      Buffer.from(checksumContent),
    );
    for (const artifact of release.artifacts) {
      files.set(`/${release.version}/${artifact.fileName}`, artifact.content);
    }
  }
  return { files };
};

const createNodeRequirement = (
  engineRange: string,
  comparatorSets: readonly (readonly string[])[],
): TestNodeRuntimeRequirement => ({
  required: true,
  engineRangeSpecified: engineRange !== "*",
  engineRange,
  comparatorSets,
});

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
  launcherExecutablePath = binaries.launcherExecutablePath;
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

const writeLauncherIni = async (
  runtimePath: string,
  content: string,
): Promise<void> => {
  await writeFile(join(runtimePath, "muon-launcher.ini"), content);
};

const readLauncherCatalogState = async (
  runtimePath: string,
): Promise<{
  cefLastCatalogUpdateUnix: number;
  nodeLastCatalogUpdateUnix: number;
  updateRequested: boolean;
  updateRequestedAtUnix: number;
}> => {
  const values = new Map<string, string>();
  let section = "";
  const content = await readFile(
    join(runtimePath, "muon-launcher.ini"),
    "utf8",
  );
  for (const rawLine of content.split(/\r?\n/u)) {
    const line = rawLine.trim();
    const sectionMatch = /^\[([^\]]+)\]$/u.exec(line);
    if (sectionMatch !== null) {
      section = sectionMatch[1] ?? "";
      continue;
    }
    const separator = line.indexOf("=");
    if (line === "" || separator < 1) {
      continue;
    }
    values.set(
      `${section}.${line.slice(0, separator)}`,
      line.slice(separator + 1),
    );
  }
  const readInteger = (key: string): number => {
    const value = values.get(key);
    if (value === undefined || !/^\d+$/u.test(value)) {
      throw new Error(`Invalid launcher state integer: ${key}`);
    }
    return Number.parseInt(value, 10);
  };
  const requested = values.get("update.requested");
  if (requested !== "true" && requested !== "false") {
    throw new Error("Invalid launcher update.requested state.");
  }
  return {
    cefLastCatalogUpdateUnix: readInteger("cef.lastCatalogUpdateUnix"),
    nodeLastCatalogUpdateUnix: readInteger("node.lastCatalogUpdateUnix"),
    updateRequested: requested === "true",
    updateRequestedAtUnix: readInteger("update.requestedAtUnix"),
  };
};

const writeEmbeddedLauncher = async (
  fixture: PrepareFixture,
  config: Record<string, unknown>,
  launcherName = "myapp",
): Promise<string> => {
  const configPath = join(fixture.projectPath, `${launcherName}.json`);
  const appLauncherPath = join(fixture.muonPath, launcherName);
  await writeFile(configPath, `${JSON.stringify(config, null, 2)}\n`);
  await embedMuonConfigInLauncherFile({
    launcherPath: launcherExecutablePath,
    configPath,
    outputPath: appLauncherPath,
  });
  await chmod(appLauncherPath, 0o755);
  return appLauncherPath;
};

const getLinuxPortableStateRuntimePath = (
  stateHome: string,
  appId: string,
): string => join(stateHome, appId, "runtime");

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
    nodeRuntimeRequirement: undefined,
    force: options.force,
    quiet: options.quiet,
    prepareExecutablePath,
    environment: {
      ...process.env,
      MUON_CEF_CATALOG_URL: fixture.catalogPath,
    },
    cwd: process.cwd(),
  });

const prepareNodeFixture = async (
  fixture: PrepareFixture,
  nodeDistUrl: string,
  nodeRuntimeRequirement: TestNodeRuntimeRequirement | undefined,
  stageDir = fixture.stageDir,
) =>
  await runMuonPrepare({
    muonPath: fixture.muonPath,
    cefPath: fixture.cefPath,
    stageDir,
    target: "linux-amd64",
    cacheDir: fixture.cacheDir,
    force: false,
    quiet: false,
    prepareExecutablePath,
    environment: {
      ...process.env,
      MUON_NODE_DIST_URL: nodeDistUrl,
    },
    cwd: process.cwd(),
    nodeRuntimeRequirement,
  });

const requireStagePath = (
  result: Awaited<ReturnType<typeof prepareFixture>>,
): string => {
  expect(result.stagePath).toBeDefined();
  return result.stagePath ?? "";
};

const sanitizePrepareLockKey = (value: string): string =>
  value.replace(/[^A-Za-z0-9._-]/g, "_");

const buildNodeTargetHarness = async (root: string): Promise<string> => {
  const harnessPath = join(root, "prepare-node-target-harness.c");
  const executablePath = join(root, "prepare-node-target-harness");
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
  const zlibLib = join(
    prepareRoot,
    ".deps",
    "build",
    "zlib-linux64",
    "install",
    "lib",
    "libz.a",
  );
  await writeFile(
    harnessPath,
    `#include <stdio.h>
#include "prepare_node.h"

int main(int argc, char **argv) {
  MuonNodeTargetInfo info;
  if (argc != 2) {
    return 64;
  }
  if (muon_prepare_get_node_target_info(argv[1], &info) != 0) {
    return 1;
  }
  printf("%s|%s|%s|%s\\n",
         info.catalog_file,
         info.archive_target,
         info.archive_suffix,
         info.executable_relative_path);
  return 0;
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
    zlibLib,
  ]);
  return executablePath;
};

const buildNodeInstallHarness = async (root: string): Promise<string> => {
  const harnessPath = join(root, "prepare-node-install-harness.c");
  const executablePath = join(root, "prepare-node-install-harness");
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
  const zlibLib = join(
    prepareRoot,
    ".deps",
    "build",
    "zlib-linux64",
    "install",
    "lib",
    "libz.a",
  );
  await writeFile(
    harnessPath,
    `#include <stdio.h>
#include <string.h>
#include "prepare_node.h"

int main(int argc, char **argv) {
  MuonNodeArtifact artifact;
  size_t file_count = 0;
  int result;
  if (argc != 6) {
    return 64;
  }
  memset(&artifact, 0, sizeof(artifact));
  artifact.version = "v24.4.0";
  artifact.catalog_file =
      strcmp(argv[2], "windows-amd64") == 0 ? "win-x64-zip" : "linux-x64";
  artifact.archive_target = argv[4];
  artifact.file_name = argv[5];
  result = muon_prepare_install_node_runtime_progress(
      argv[1], argv[2], &artifact, argv[3], &file_count, NULL, NULL);
  if (result != 0) {
    return 1;
  }
  printf("%zu\\n", file_count);
  return 0;
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
    zlibLib,
  ]);
  return executablePath;
};

const runNodeInstallHarness = async (
  harnessPath: string,
  fixture: PrepareFixture,
  artifact: FakeNodeArtifact,
  target: string,
  runtimeRoot: string,
) => {
  const archivePath = join(fixture.projectPath, artifact.fileName);
  await writeFile(archivePath, artifact.content);
  return await execFileAsync(
    harnessPath,
    [archivePath, target, runtimeRoot, artifact.nodeTarget, artifact.fileName],
    { encoding: "utf8" },
  );
};

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
  const zlibLib = join(
    prepareRoot,
    ".deps",
    "build",
    "zlib-linux64",
    "install",
    "lib",
    "libz.a",
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
    zlibLib,
    "-pthread",
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

const buildEmbeddedNodeRuntimeHarness = async (
  root: string,
): Promise<string> => {
  const harnessPath = join(root, "embedded-node-runtime-harness.c");
  const executablePath = join(root, "embedded-node-runtime-harness");
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
  const zlibLib = join(
    prepareRoot,
    ".deps",
    "build",
    "zlib-linux64",
    "install",
    "lib",
    "libz.a",
  );
  await writeFile(
    harnessPath,
    `#include <stdio.h>

#include "launcher_config.h"

int main(void) {
  MuonNodeRuntimeRequirement requirement;
  int present = 0;
  if (muon_launcher_get_embedded_node_runtime_requirement(
          &requirement, &present) != 0) {
    return 1;
  }
  printf("%d|%d|%d|%s|%llu\\n", present, requirement.required,
         requirement.engine_range_specified,
         requirement.engine_range == NULL ? "" : requirement.engine_range,
         (unsigned long long)requirement.set_count);
  muon_prepare_free_node_runtime_requirement(&requirement);
  return 0;
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
    zlibLib,
    "-pthread",
  ]);
  return executablePath;
};

const writeEmbeddedNodeRuntimeHarness = async (
  harnessPath: string,
  payload: Uint8Array,
  outputPath: string,
): Promise<void> => {
  const content = await readFile(harnessPath);
  const slot = findMuonLauncherEmbeddedConfigSlot(content);
  const patched = Buffer.from(content);
  createMuonLauncherEmbeddedConfigSlot(payload).copy(patched, slot.offset);
  await writeFile(outputPath, patched);
  await chmod(outputPath, 0o755);
};

const encodeTestVarUint = (value: number): Buffer => {
  const bytes: number[] = [];
  let remaining = value;
  do {
    let byte = remaining & 0x7f;
    remaining = Math.floor(remaining / 128);
    if (remaining !== 0) {
      byte |= 0x80;
    }
    bytes.push(byte);
  } while (remaining !== 0);
  return Buffer.from(bytes);
};

const encodeTestRawString = (value: string): Buffer => {
  const bytes = Buffer.from(value, "utf8");
  return Buffer.concat([encodeTestVarUint(bytes.length), bytes]);
};

const encodeTestString = (value: string): Buffer =>
  Buffer.concat([Buffer.from([4]), encodeTestRawString(value)]);

const encodeTestBoolean = (value: boolean): Buffer =>
  Buffer.from([value ? 2 : 1]);

const encodeTestArray = (values: readonly Buffer[]): Buffer =>
  Buffer.concat([
    Buffer.from([6]),
    encodeTestVarUint(values.length),
    ...values,
  ]);

const encodeTestObject = (
  entries: readonly (readonly [string, Buffer])[],
): Buffer =>
  Buffer.concat([
    Buffer.from([7]),
    encodeTestVarUint(entries.length),
    ...entries.flatMap(([key, value]) => [encodeTestRawString(key), value]),
  ]);

const createDuplicateNodeRuntimeFieldPayload = (): Buffer =>
  encodeTestObject([
    [
      "launcher",
      encodeTestObject([
        [
          "nodeRuntime",
          encodeTestObject([
            ["required", encodeTestBoolean(true)],
            ["required", encodeTestBoolean(false)],
            ["engineRangeSpecified", encodeTestBoolean(false)],
            ["engineRange", encodeTestString("*")],
          ]),
        ],
      ]),
    ],
  ]);

const createDeepUnknownLauncherPayload = (): Buffer => {
  let nested: unknown = null;
  for (let depth = 0; depth < 128; depth += 1) {
    nested = [nested];
  }
  return encodeMuonConfigTlv({
    unknown: nested,
    launcher: {
      nodeRuntime: createNodeRequirement("*", [[]]),
    },
  });
};

const closeServer = async (server: Server): Promise<void> => {
  await new Promise<void>((resolveClose, rejectClose) => {
    server.close((error) => {
      if (error !== undefined) {
        rejectClose(error);
        return;
      }
      resolveClose();
    });
  });
};

const startNodeDistributionServer = async (
  distribution: FakeNodeDistribution,
): Promise<FakeNodeDistributionServer> => {
  const requests: FakeNodeDistributionRequest[] = [];
  const server = createServer((request, response) => {
    const method = request.method ?? "GET";
    const pathname = new URL(request.url ?? "/", "http://127.0.0.1").pathname;
    requests.push({ method, pathname });
    const content = distribution.files.get(pathname);
    if (content === undefined) {
      response.writeHead(404);
      response.end();
      return;
    }
    response.writeHead(200, {
      "Accept-Ranges": "bytes",
      "Content-Length": content.length,
      "Content-Type":
        pathname.endsWith(".json") || pathname.endsWith(".txt")
          ? "application/json"
          : "application/octet-stream",
    });
    if (method === "HEAD") {
      response.end();
    } else {
      response.end(content);
    }
  });
  await new Promise<void>((resolveListen, rejectListen) => {
    server.once("error", rejectListen);
    server.listen(0, "127.0.0.1", () => {
      server.off("error", rejectListen);
      resolveListen();
    });
  });
  const address = server.address();
  if (address === null || typeof address === "string") {
    await closeServer(server);
    throw new Error("Expected a TCP test server address.");
  }
  let closed = false;
  const result: FakeNodeDistributionServer = {
    baseUrl: `http://127.0.0.1:${address.port}`,
    getRequests: () => [...requests],
    close: async (): Promise<void> => {
      if (closed) {
        return;
      }
      closed = true;
      await closeServer(server);
    },
  };
  nodeDistributionServers.push(result);
  return result;
};

const startConcurrentRuntimeDistributionServer = async (
  fixture: PrepareFixture,
  distribution: FakeNodeDistribution,
): Promise<ConcurrentRuntimeDistributionServer> => {
  const requests: FakeNodeDistributionRequest[] = [];
  const files = new Map(distribution.files);
  const cefArchive = getEmbeddedCefArchive();
  const nodeArchivePath = [...distribution.files.keys()].find(
    (path) => path.endsWith(".tar.gz") || path.endsWith(".zip"),
  );
  if (nodeArchivePath === undefined) {
    throw new Error("Expected one Node runtime archive in the distribution.");
  }
  files.set("/source-catalog.json", await readFile(fixture.catalogPath));
  files.set(`/${cefArchive.fileName}`, await readFile(cefArchive.archivePath));

  const barriers = [
    {
      paths: new Set(["/source-catalog.json", "/index.json"]),
      arrived: new Set<string>(),
      pending: [] as (() => void)[],
      reached: false,
    },
    {
      paths: new Set([`/${cefArchive.fileName}`, nodeArchivePath]),
      arrived: new Set<string>(),
      pending: [] as (() => void)[],
      reached: false,
    },
  ];
  let closing = false;

  const server = createServer((request, response) => {
    const method = request.method ?? "GET";
    const pathname = new URL(request.url ?? "/", "http://127.0.0.1").pathname;
    requests.push({ method, pathname });
    const content = files.get(pathname);
    if (content === undefined) {
      response.writeHead(404);
      response.end();
      return;
    }
    const writeResponse = (): void => {
      if (closing) {
        response.writeHead(503);
        response.end();
        return;
      }
      response.writeHead(200, {
        "Accept-Ranges": "bytes",
        "Content-Length": content.length,
        "Content-Type":
          pathname.endsWith(".json") || pathname.endsWith(".txt")
            ? "application/json"
            : "application/octet-stream",
      });
      if (method === "HEAD") {
        response.end();
      } else {
        response.end(content);
      }
    };
    const barrier = barriers.find(({ paths }) => paths.has(pathname));
    if (barrier === undefined || method !== "GET") {
      writeResponse();
      return;
    }

    barrier.arrived.add(pathname);
    barrier.pending.push(writeResponse);
    if (barrier.arrived.size !== barrier.paths.size) {
      return;
    }
    barrier.reached = true;
    for (const respond of barrier.pending) {
      respond();
    }
    barrier.pending.length = 0;
  });
  await new Promise<void>((resolveListen, rejectListen) => {
    server.once("error", rejectListen);
    server.listen(0, "127.0.0.1", () => {
      server.off("error", rejectListen);
      resolveListen();
    });
  });
  const address = server.address();
  if (address === null || typeof address === "string") {
    await closeServer(server);
    throw new Error("Expected a TCP test server address.");
  }
  return {
    baseUrl: `http://127.0.0.1:${address.port}`,
    getRequests: () => [...requests],
    catalogBarrierReached: () => barriers[0]?.reached === true,
    archiveBarrierReached: () => barriers[1]?.reached === true,
    close: async (): Promise<void> => {
      closing = true;
      for (const barrier of barriers) {
        for (const respond of barrier.pending) {
          respond();
        }
        barrier.pending.length = 0;
      }
      await closeServer(server);
    },
  };
};

const startInterruptingArchiveServer = async (
  catalogPath: string,
  archiveFileName: string,
  archivePath: string,
): Promise<{
  baseUrl: string;
  server: Server;
  getArchiveRequestCount: () => number;
  getRangeRequestCount: () => number;
}> => {
  const catalogContent = await readFile(catalogPath);
  const archiveContent = await readFile(archivePath);
  const interruptSize = Math.max(1, Math.floor(archiveContent.length / 2));
  let archiveRequestCount = 0;
  let rangeRequestCount = 0;
  const server = createServer((request, response) => {
    const path = new URL(request.url ?? "/", "http://127.0.0.1").pathname;
    if (path === "/source-catalog.json") {
      response.writeHead(200, {
        "Content-Length": catalogContent.length,
        "Content-Type": "application/json",
      });
      response.end(catalogContent);
      return;
    }
    if (path !== `/${encodeURIComponent(archiveFileName)}`) {
      response.writeHead(404);
      response.end();
      return;
    }
    archiveRequestCount += 1;
    const rangeHeader = request.headers.range;
    if (rangeHeader !== undefined) {
      rangeRequestCount += 1;
    }
    if (archiveRequestCount === 1) {
      response.writeHead(200, {
        "Accept-Ranges": "bytes",
        "Content-Length": archiveContent.length,
        "Content-Type": "application/x-bzip2",
      });
      response.write(archiveContent.subarray(0, interruptSize), () => {
        response.destroy();
      });
      return;
    }
    const rangeMatch =
      rangeHeader === undefined ? null : /^bytes=(\d+)-$/.exec(rangeHeader);
    const start = rangeMatch === null ? 0 : Number(rangeMatch[1]);
    const body = archiveContent.subarray(start);
    response.writeHead(rangeMatch === null ? 200 : 206, {
      "Accept-Ranges": "bytes",
      "Content-Length": body.length,
      "Content-Range": `bytes ${start}-${archiveContent.length - 1}/${archiveContent.length}`,
      "Content-Type": "application/x-bzip2",
    });
    response.end(body);
  });
  await new Promise<void>((resolveListen, rejectListen) => {
    server.once("error", rejectListen);
    server.listen(0, "127.0.0.1", () => {
      server.off("error", rejectListen);
      resolveListen();
    });
  });
  const address = server.address();
  if (address === null || typeof address === "string") {
    await closeServer(server);
    throw new Error("Expected a TCP test server address.");
  }
  return {
    baseUrl: `http://127.0.0.1:${address.port}`,
    server,
    getArchiveRequestCount: () => archiveRequestCount,
    getRangeRequestCount: () => rangeRequestCount,
  };
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

const listDirectoryEntriesIfPresent = async (
  directory: string,
): Promise<string[]> => {
  try {
    return (await readdir(directory)).sort();
  } catch (error) {
    if (
      typeof error === "object" &&
      error !== null &&
      "code" in error &&
      error.code === "ENOENT"
    ) {
      return [];
    }
    throw error;
  }
};

afterEach(async () => {
  for (const server of nodeDistributionServers.splice(0)) {
    await server.close();
  }
  for (const directory of cleanupDirectories.splice(0)) {
    await rm(directory, { recursive: true, force: true });
  }
});

describe("muon prepare target resolution", () => {
  it("resolves supported Node platforms to muon runtime targets", () => {
    expect(getDefaultMuonPrepareTarget("linux", "x64")).toBe("linux-amd64");
    expect(getDefaultMuonPrepareTarget("linux", "arm")).toBe("linux-armhf");
    expect(getDefaultMuonPrepareTarget("linux", "arm64")).toBe("linux-arm64");
    expect(getDefaultMuonPrepareTarget("win32", "ia32")).toBe("windows-i686");
    expect(getDefaultMuonPrepareTarget("win32", "x64")).toBe("windows-amd64");
  });

  it("rejects unsupported Linux i686 targets", () => {
    expect(() => getDefaultMuonPrepareTarget("linux", "ia32")).toThrow(
      "Unsupported muon prepare target: platform=linux, arch=ia32",
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
        nodeRuntimeRequirement: undefined,
        force: false,
        quiet: true,
        prepareExecutablePath: "/tmp/muon-builder",
        environment: process.env,
        cwd: process.cwd(),
      }),
    ).rejects.toThrow("Unsupported muon prepare target: linux64");
  });
});

describe("embedded Node runtime requirement", () => {
  let harnessPath = "";

  beforeAll(async () => {
    harnessPath = await buildEmbeddedNodeRuntimeHarness(
      await createSuiteTemporaryDirectory(
        "muon-embedded-node-runtime-harness-",
      ),
    );
  });

  it("decodes a normalized requirement without reading random slot padding", async () => {
    const root = await createTemporaryDirectory(
      "muon-embedded-node-runtime-valid-",
    );
    const executablePath = join(root, "harness");
    await writeEmbeddedNodeRuntimeHarness(
      harnessPath,
      encodeMuonConfigTlv({
        launcher: {
          nodeRuntime: createNodeRequirement("*", [[]]),
        },
      }),
      executablePath,
    );

    const { stdout } = await execFileAsync(executablePath, [], {
      encoding: "utf8",
    });

    expect(stdout.trim()).toBe("1|1|0|*|1");
  });

  it.each([
    {
      name: "a missing field",
      payload: encodeMuonConfigTlv({
        launcher: {
          nodeRuntime: {
            required: true,
            engineRange: "*",
            comparatorSets: [[]],
          },
        },
      }),
    },
    {
      name: "an unknown field",
      payload: encodeMuonConfigTlv({
        launcher: {
          nodeRuntime: {
            ...createNodeRequirement("*", [[]]),
            extra: true,
          },
        },
      }),
    },
    {
      name: "a duplicate field",
      payload: createDuplicateNodeRuntimeFieldPayload(),
    },
    {
      name: "a field with the wrong type",
      payload: encodeMuonConfigTlv({
        launcher: {
          nodeRuntime: {
            ...createNodeRequirement("*", [[]]),
            engineRangeSpecified: "true",
          },
        },
      }),
    },
    {
      name: "an embedded NUL",
      payload: encodeMuonConfigTlv({
        launcher: {
          nodeRuntime: createNodeRequirement("*\u0000invalid", [[]]),
        },
      }),
    },
    {
      name: "an invalid normalized comparator",
      payload: encodeMuonConfigTlv({
        launcher: {
          nodeRuntime: createNodeRequirement("*", [["not-a-comparator"]]),
        },
      }),
    },
    {
      name: "a non-canonical varuint",
      payload: Buffer.from([7, 0x80, 0]),
    },
    {
      name: "an overflowing varuint",
      payload: Buffer.from([
        7, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 2,
      ]),
    },
    {
      name: "excessively nested unknown data",
      payload: createDeepUnknownLauncherPayload(),
    },
  ])("rejects $name", async ({ payload }) => {
    const root = await createTemporaryDirectory(
      "muon-embedded-node-runtime-invalid-",
    );
    const executablePath = join(root, "harness");
    await writeEmbeddedNodeRuntimeHarness(harnessPath, payload, executablePath);

    await expect(
      execFileAsync(executablePath, [], { encoding: "utf8" }),
    ).rejects.toMatchObject({ code: 1 });
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
    const readyMarker = await readRuntimeReadyMarker(stagePath);
    expect(readyMarker.muonFingerprint).toMatch(sha256HexPattern);
    expect(readyMarker.cefFingerprint).toMatch(sha256HexPattern);
    expect(readyMarker.nodeFingerprint).toBe(emptySha256Fingerprint);
  });

  it("does not publish a ready stage when launcher state cannot be committed", async () => {
    const fixture = await createPrepareFixture();
    const launcherStateBlocker = join(
      fixture.cefPath,
      "Release",
      "muon-launcher.ini",
    );
    await mkdir(launcherStateBlocker, { recursive: true });
    await writeFile(join(launcherStateBlocker, "blocker"), "not a file\n");

    await expect(prepareFixture(fixture)).rejects.toThrow();

    await expect(
      access(join(fixture.stageDir, runtimeReadyMarkerFileName)),
    ).rejects.toThrow();
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
      join(fixture.cacheDir, "artifacts", "cef", fixture.archiveFileName),
    );
    await expect(
      findCachedFile(fixture.cacheDir, fixture.archiveFileName),
    ).resolves.toBe(result.cefPath);
    await expect(access(result.cefPath)).resolves.toBeUndefined();
    await expect(
      access(join(fixture.cacheDir, "cef-catalog.json")),
    ).rejects.toThrow();
    await expect(listCacheEntries(fixture.cacheDir)).resolves.toEqual([
      "artifacts",
      "artifacts/cef",
      `artifacts/cef/${fixture.archiveFileName}`,
    ]);
    await expect(access(join(stagePath, "libcef.so"))).resolves.toBeUndefined();
    await expect(
      access(join(stagePath, "icudtl.dat")),
    ).resolves.toBeUndefined();
    await expect(
      access(join(stagePath, "plugins", "plugin.txt")),
    ).resolves.toBeUndefined();
    const readyMarker = await readRuntimeReadyMarker(stagePath);
    expect(readyMarker.cefFingerprint).toMatch(sha256HexPattern);
    expect(readyMarker.nodeFingerprint).toBe(emptySha256Fingerprint);
  });

  it("resumes an interrupted HTTP CEF archive download", async () => {
    const archive = await createFakeCefArchive(createTemporaryDirectory, {
      archiveFileName: "cef-http-retry.tar.bz2",
      archiveRootName: "cef_binary_http_retry_linux64_minimal",
      libcefContent: "http retry cef library\n",
    });
    const fixture = await createPrepareFixture(undefined, [
      {
        version: "http-retry-cef",
        chromiumVersion: "150.0.0.0",
        archive,
      },
    ]);
    await writeLauncherIni(
      fixture.muonPath,
      `[runtime]
catalogRefreshIntervalSeconds=0

[cef]
versionPolicy=exact
exactVersion=http-retry-cef
`,
    );
    const server = await startInterruptingArchiveServer(
      fixture.catalogPath,
      archive.fileName,
      archive.archivePath,
    );
    try {
      const result = await runMuonPrepare({
        muonPath: fixture.muonPath,
        cefPath: undefined,
        stageDir: fixture.stageDir,
        target: "linux-amd64",
        cacheDir: fixture.cacheDir,
        nodeRuntimeRequirement: undefined,
        force: false,
        quiet: false,
        prepareExecutablePath,
        environment: {
          ...process.env,
          MUON_CEF_CATALOG_URL: `${server.baseUrl}/source-catalog.json`,
        },
        cwd: process.cwd(),
      });
      const stagePath = requireStagePath(result);

      await expect(
        readFile(join(stagePath, "libcef.so"), "utf8"),
      ).resolves.toBe("http retry cef library\n");
      expect(server.getArchiveRequestCount()).toBeGreaterThanOrEqual(2);
      expect(server.getRangeRequestCount()).toBeGreaterThanOrEqual(1);
    } finally {
      await closeServer(server.server);
    }
  });

  it("uses the embedded tested CEF artifact without refreshing the catalog", async () => {
    const fixture = await createPrepareFixture();
    await writeLauncherIni(
      fixture.muonPath,
      `[runtime]
catalogRefreshIntervalSeconds=604800

[cef]
versionPolicy=tested
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
      join(fixture.cacheDir, "artifacts", "cef", fixture.archiveFileName),
    );
    await expect(
      access(join(fixture.cacheDir, "cef-catalog.json")),
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
    await writeLauncherIni(
      fixture.muonPath,
      `[runtime]
catalogRefreshIntervalSeconds=604800

[cef]
versionPolicy=same-major-latest
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
      join(fixture.cacheDir, "artifacts", "cef", latestSameMajor.fileName),
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
    await writeLauncherIni(
      fixture.muonPath,
      `[runtime]
catalogRefreshIntervalSeconds=604800

[cef]
versionPolicy=compat-latest
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
      join(fixture.cacheDir, "artifacts", "cef", compatible.fileName),
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
    await writeLauncherIni(
      fixture.muonPath,
      `[runtime]
catalogRefreshIntervalSeconds=604800

[cef]
versionPolicy=exact
exactVersion=exact-incompatible
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
      nodeRuntimeRequirement: undefined,
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
      join(fixture.cacheDir, "artifacts", "cef", fixture.archiveFileName),
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
      join(fixture.cacheDir, "artifacts", "cef", fixture.archiveFileName),
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
    const readyMarker = await readReadyMarker(
      join(outputDir, ".muon-cef-ready.json"),
    );
    expect(readyMarker.muonFingerprint).toBe(emptySha256Fingerprint);
    expect(readyMarker.cefFingerprint).toMatch(sha256HexPattern);
    await expect(listCacheEntries(fixture.cacheDir)).resolves.toEqual([
      "artifacts",
      "artifacts/cef",
      `artifacts/cef/${fixture.archiveFileName}`,
      "cef-catalog.json",
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
      "artifacts/cef",
      `artifacts/cef/${fixture.archiveFileName}`,
      "cef-catalog.json",
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
    expect(stderr).toContain("muon files copied to staging: files=4");
    expect(stderr).toContain("Starting muon...");
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

  it("writes structured JSON progress when requested by the native CLI", async () => {
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
        "--progress-json",
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
    const events = stderr
      .trim()
      .split(/\r?\n/)
      .filter((line) => line.startsWith("{"))
      .map(
        (line) =>
          JSON.parse(line) as {
            phase: string;
            status: string;
            current: number;
            total: number;
            determinate: boolean;
          },
      );
    const downloadEvent = events.find(
      (event) =>
        event.phase === "downloading" &&
        event.status === "Downloading CEF runtime..." &&
        event.determinate &&
        event.total > 0 &&
        event.current > 0,
    );
    const extractEvent = events.find(
      (event) =>
        event.phase === "installing" &&
        event.status === "Extracting CEF runtime..." &&
        event.current > 0,
    );
    const finalInstallEvent = events.find(
      (event) =>
        event.phase === "installing" &&
        event.status === "Installing CEF runtime..." &&
        !event.determinate &&
        event.current === 3 &&
        event.total === 0,
    );

    expect(result.stagePath).toBe(fixture.stageDir);
    expect(downloadEvent).toBeDefined();
    expect(extractEvent).toBeDefined();
    expect(finalInstallEvent).toBeDefined();
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
    expect(stderr).toContain("Downloading CEF runtime... 100%");
    expect(stderr).toMatch(/Installing CEF runtime\.\.\. \d+ files/u);
    expect(stderr).toContain("Starting muon...");
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
    expect(stderr).toMatch(/\r[-\\|/] Downloading CEF runtime\.\.\. \d+%/u);
    expect(stderr).toMatch(/\rInstalling CEF runtime\.\.\. \d+ files/u);
    expect(stderr).toContain("Starting muon...");
  });

  it("reports structured progress for launcher in-place CEF preparation", async () => {
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
      expect.stringContaining("|Extracting CEF runtime...|"),
    );
    expect(firstRun).toContainEqual(
      expect.stringContaining("|Installing CEF runtime...|"),
    );
    expect(firstRun).toContain("install|Installing CEF runtime...|0|3|0");
    expect(firstRun).toContainEqual(
      expect.stringContaining("|Starting muon...|"),
    );

    await expect(
      access(join(fixture.muonPath, "libcef.so")),
    ).resolves.toBeUndefined();
    const readyMarker = await readRuntimeReadyMarker(fixture.muonPath);
    expect(readyMarker.muonFingerprint).toBe(emptySha256Fingerprint);
    expect(readyMarker.cefFingerprint).toMatch(sha256HexPattern);
    expect(readyMarker.nodeFingerprint).toBe(emptySha256Fingerprint);

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

  it("adds muon generated directories to gitignore when staging under the project .muon directory", async () => {
    const fixture = await createPrepareFixture();

    await prepareFixture(fixture);

    await expect(
      readFile(join(fixture.projectPath, ".gitignore"), "utf8"),
    ).resolves.toBe(".muon/\ndist-muon/\nartifacts/\n");
  });

  it("appends a missing muon dist gitignore entry without duplicating .muon", async () => {
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

  it("adds the current muon dist gitignore entry when a legacy entry exists", async () => {
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
    await writeLauncherIni(
      fixture.muonPath,
      `[runtime]
catalogRefreshIntervalSeconds=604800

[cef]
versionPolicy=exact
exactVersion=fake-cef-download
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

  describe("Node runtime distribution", () => {
    it("maps every public muon target to its official Node distribution", async () => {
      const fixture = await createPrepareFixture();
      const harnessPath = await buildNodeTargetHarness(fixture.projectPath);
      const expectations = [
        ["linux-amd64", "linux-x64|linux-x64|.tar.gz|bin/node"],
        ["linux-armhf", "linux-armv7l|linux-armv7l|.tar.gz|bin/node"],
        ["linux-arm64", "linux-arm64|linux-arm64|.tar.gz|bin/node"],
        ["windows-i686", "win-x86-zip|win-x86|.zip|bin/node.exe"],
        ["windows-amd64", "win-x64-zip|win-x64|.zip|bin/node.exe"],
      ] as const;

      for (const [target, expected] of expectations) {
        const { stdout } = await execFileAsync(harnessPath, [target], {
          encoding: "utf8",
        });
        expect(stdout.trim()).toBe(expected);
      }
    });

    it("downloads CEF and Node runtime inputs concurrently with serialized progress events", async () => {
      const fixture = await createPrepareFixture(undefined, [
        {
          version: "fake-cef-concurrent",
          chromiumVersion: "100.0.0.0",
          archive: getEmbeddedCefArchive(),
        },
      ]);
      const artifact = await createFakeNodeTarGzArtifact(fakeNodeVersion, {
        includeLicense: true,
        nodeIsSymlink: false,
        includeNpmSymlink: false,
        includeTraversalEntry: false,
      });
      const server = await startConcurrentRuntimeDistributionServer(
        fixture,
        createFakeNodeDistribution([
          createFakeNodeRelease(fakeNodeVersion, "Krypton", artifact),
        ]),
      );
      await writeLauncherIni(
        fixture.muonPath,
        `[runtime]
catalogRefreshIntervalSeconds=604800

[cef]
versionPolicy=exact
exactVersion=fake-cef-concurrent
lastCatalogUpdateUnix=0

[node]
lastCatalogUpdateUnix=0

[update]
requested=false
requestedAtUnix=0
`,
      );

      try {
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
            "--node-runtime-requirement",
            JSON.stringify(createNodeRequirement("*", [[]])),
            "--progress-json",
            "--json",
          ],
          {
            cwd: process.cwd(),
            encoding: "utf8",
            env: {
              ...process.env,
              MUON_CEF_CATALOG_URL: `${server.baseUrl}/source-catalog.json`,
              MUON_NODE_DIST_URL: server.baseUrl,
            },
            timeout: 60_000,
          },
        );
        const result = JSON.parse(stdout) as { stagePath: string };
        expect(result.stagePath).toBe(fixture.stageDir);
        expect(server.catalogBarrierReached()).toBe(true);
        expect(server.archiveBarrierReached()).toBe(true);
        const progressLines = stderr
          .split(/\r?\n/u)
          .filter((line) => line.includes('"phase"'));
        const progressEvents = progressLines.map(
          (line) =>
            JSON.parse(line) as {
              phase: string;
              status: string;
            },
        );
        expect(progressEvents).toEqual(
          expect.arrayContaining([
            expect.objectContaining({
              phase: "downloading",
              status: "Checking for CEF updates...",
            }),
            expect.objectContaining({
              phase: "downloading",
              status: "Checking for Node.js updates...",
            }),
          ]),
        );
        await expect(
          access(join(fixture.stageDir, "libcef.so")),
        ).resolves.toBeUndefined();
        await expect(
          readFile(join(fixture.stageDir, "runtimes", "node", "bin", "node")),
        ).resolves.toEqual(createFakeNodeExecutableContent(fakeNodeVersion));
      } finally {
        await server.close();
      }
    });

    it("selects the latest matching LTS for a wildcard requirement", async () => {
      const fixture = await createPrepareFixture();
      const selectedArtifact = await createFakeNodeTarGzArtifact("v24.4.0", {
        includeLicense: true,
        nodeIsSymlink: false,
        includeNpmSymlink: false,
        includeTraversalEntry: false,
      });
      const distribution = createFakeNodeDistribution([
        createFakeNodeRelease("v26.1.0", false),
        createFakeNodeRelease("v24.2.0", "Krypton"),
        createFakeNodeRelease("v22.19.0", "Jod"),
        createFakeNodeRelease("v24.4.0", "Krypton", selectedArtifact),
      ]);
      const server = await startNodeDistributionServer(distribution);

      await prepareNodeFixture(
        fixture,
        server.baseUrl,
        createNodeRequirement("*", [[]]),
      );

      const selectedPath = join(
        fixture.cacheDir,
        "artifacts",
        "node",
        selectedArtifact.version,
        selectedArtifact.fileName,
      );
      await expect(readFile(selectedPath)).resolves.toEqual(
        selectedArtifact.content,
      );
      expect(
        server
          .getRequests()
          .filter(
            ({ method, pathname }) =>
              method === "GET" &&
              pathname ===
                `/${selectedArtifact.version}/${selectedArtifact.fileName}`,
          ),
      ).toHaveLength(1);
      expect(
        server
          .getRequests()
          .some(({ pathname }) => pathname.startsWith("/v26.1.0/")),
      ).toBe(false);
    });

    it("does not use a non-LTS fallback when package.json omits engines.node", async () => {
      const fixture = await createPrepareFixture();
      const distribution = createFakeNodeDistribution([
        createFakeNodeRelease("v25.1.0", false),
        createFakeNodeRelease("v24.9.0", false),
      ]);
      const server = await startNodeDistributionServer(distribution);
      const requirement = {
        ...createNodeRequirement("*", [[]]),
        engineRangeSpecified: false,
      };
      let message = "";

      try {
        await prepareNodeFixture(fixture, server.baseUrl, requirement);
      } catch (error) {
        message = error instanceof Error ? error.message : String(error);
      }

      expect(message).toContain(
        "No Node.js LTS release matches target linux-amd64",
      );
      expect(message).toContain("engines.node (omitted)");
    });

    it("uses a non-LTS fallback for an explicit wildcard engines.node", async () => {
      const fixture = await createPrepareFixture();
      const selectedArtifact = await createFakeNodeTarGzArtifact("v25.1.0", {
        includeLicense: true,
        nodeIsSymlink: false,
        includeNpmSymlink: false,
        includeTraversalEntry: false,
      });
      const distribution = createFakeNodeDistribution([
        createFakeNodeRelease("v24.9.0", false),
        createFakeNodeRelease("v25.1.0", false, selectedArtifact),
      ]);
      const server = await startNodeDistributionServer(distribution);
      const requirement = {
        ...createNodeRequirement("*", [[]]),
        engineRangeSpecified: true,
      };

      await prepareNodeFixture(fixture, server.baseUrl, requirement);

      await expect(
        readFile(
          join(
            fixture.cacheDir,
            "artifacts",
            "node",
            selectedArtifact.version,
            selectedArtifact.fileName,
          ),
        ),
      ).resolves.toEqual(selectedArtifact.content);
    });

    it("falls back to the latest matching non-LTS release across comparator sets", async () => {
      const fixture = await createPrepareFixture();
      const selectedArtifact = await createFakeNodeTarGzArtifact("v24.9.0", {
        includeLicense: true,
        nodeIsSymlink: false,
        includeNpmSymlink: false,
        includeTraversalEntry: false,
      });
      const distribution = createFakeNodeDistribution([
        createFakeNodeRelease("v24.2.0", "Krypton"),
        createFakeNodeRelease("v25.0.0", false),
        createFakeNodeRelease("v20.20.0", false),
        createFakeNodeRelease("v22.19.0", "Jod"),
        createFakeNodeRelease("v24.9.0", false, selectedArtifact),
        createFakeNodeRelease("v24.3.0", false),
      ]);
      const server = await startNodeDistributionServer(distribution);

      await prepareNodeFixture(
        fixture,
        server.baseUrl,
        createNodeRequirement(">=20 <21 || >=24.3 <25", [
          [">=20.0.0", "<21.0.0-0"],
          [">=24.3.0", "<25.0.0-0"],
        ]),
      );

      await expect(
        readFile(
          join(
            fixture.cacheDir,
            "artifacts",
            "node",
            selectedArtifact.version,
            selectedArtifact.fileName,
          ),
        ),
      ).resolves.toEqual(selectedArtifact.content);
      expect(
        server
          .getRequests()
          .some(({ pathname }) => pathname.startsWith("/v25.0.0/")),
      ).toBe(false);
    });

    it("reports the target and engine range when no release matches", async () => {
      const fixture = await createPrepareFixture();
      const distribution = createFakeNodeDistribution([
        createFakeNodeRelease("v24.4.0", "Krypton"),
        createFakeNodeRelease("v26.1.0", false),
      ]);
      const server = await startNodeDistributionServer(distribution);
      let message = "";

      try {
        await prepareNodeFixture(
          fixture,
          server.baseUrl,
          createNodeRequirement(">=30", [[">=30.0.0"]]),
        );
      } catch (error) {
        message = error instanceof Error ? error.message : String(error);
      }

      expect(message).toContain("linux-amd64");
      expect(message).toContain(">=30");
    });

    it.each([
      {
        name: "duplicate exact checksum lines",
        createChecksum: (artifact: FakeNodeArtifact): string => {
          const line = `${artifact.sha256}  ${artifact.fileName}\n`;
          return `${line}${line}`;
        },
      },
      {
        name: "a checksum line with one separator space",
        createChecksum: (artifact: FakeNodeArtifact): string =>
          `${artifact.sha256} ${artifact.fileName}\n`,
      },
    ])(
      "rejects $name before requesting the Node artifact",
      async ({ createChecksum }) => {
        const fixture = await createPrepareFixture();
        const artifact = createFakeNodeArtifact("v24.4.0");
        const release = createFakeNodeRelease("v24.4.0", "Krypton", artifact);
        const distribution = createFakeNodeDistribution(
          [release],
          new Map([[release.version, createChecksum(artifact)]]),
        );
        const server = await startNodeDistributionServer(distribution);

        await expect(
          prepareNodeFixture(
            fixture,
            server.baseUrl,
            createNodeRequirement("*", [[]]),
          ),
        ).rejects.toThrow();

        expect(
          server
            .getRequests()
            .filter(
              ({ method, pathname }) =>
                method === "GET" &&
                pathname === `/${artifact.version}/${artifact.fileName}`,
            ),
        ).toEqual([]);
      },
    );

    it("does not retain an artifact whose SHA-256 does not match", async () => {
      const fixture = await createPrepareFixture();
      const artifact = createFakeNodeArtifact("v24.4.0");
      const release = createFakeNodeRelease("v24.4.0", "Krypton", artifact);
      const distribution = createFakeNodeDistribution(
        [release],
        new Map([
          [release.version, `${"0".repeat(64)}  ${artifact.fileName}\n`],
        ]),
      );
      const server = await startNodeDistributionServer(distribution);
      const versionCacheDirectory = join(
        fixture.cacheDir,
        "artifacts",
        "node",
        artifact.version,
      );

      await expect(
        prepareNodeFixture(
          fixture,
          server.baseUrl,
          createNodeRequirement("*", [[]]),
        ),
      ).rejects.toThrow(/sha-?256/i);

      expect(
        await listDirectoryEntriesIfPresent(versionCacheDirectory),
      ).toEqual([]);
    });

    it("reuses the catalog, checksum, and verified artifact without a server", async () => {
      const fixture = await createPrepareFixture();
      const artifact = await createFakeNodeTarGzArtifact("v24.4.0", {
        includeLicense: true,
        nodeIsSymlink: false,
        includeNpmSymlink: false,
        includeTraversalEntry: false,
      });
      const release = createFakeNodeRelease("v24.4.0", "Krypton", artifact);
      const server = await startNodeDistributionServer(
        createFakeNodeDistribution([release]),
      );
      const requirement = createNodeRequirement("*", [[]]);

      await prepareNodeFixture(
        fixture,
        server.baseUrl,
        requirement,
        join(fixture.projectPath, ".muon", "online"),
      );

      await expect(
        access(join(fixture.cacheDir, "node-catalog.json")),
      ).resolves.toBeUndefined();
      await expect(
        readFile(
          join(
            fixture.cacheDir,
            "node-checksums",
            artifact.version,
            "SHASUMS256.txt",
          ),
          "utf8",
        ),
      ).resolves.toBe(`${artifact.sha256}  ${artifact.fileName}\n`);
      await expect(
        readFile(
          join(
            fixture.cacheDir,
            "artifacts",
            "node",
            artifact.version,
            artifact.fileName,
          ),
        ),
      ).resolves.toEqual(artifact.content);

      await server.close();
      await expect(
        prepareNodeFixture(
          fixture,
          server.baseUrl,
          requirement,
          join(fixture.projectPath, ".muon", "offline"),
        ),
      ).resolves.toMatchObject({
        stagePath: join(fixture.projectPath, ".muon", "offline"),
      });
    });

    it("does not refresh the Node catalog again within the configured interval", async () => {
      const fixture = await createPrepareFixture();
      const artifact = await createFakeNodeTarGzArtifact("v24.4.0", {
        includeLicense: true,
        nodeIsSymlink: false,
        includeNpmSymlink: false,
        includeTraversalEntry: false,
      });
      const release = createFakeNodeRelease("v24.4.0", "Krypton", artifact);
      const server = await startNodeDistributionServer(
        createFakeNodeDistribution([release]),
      );
      const requirement = createNodeRequirement("*", [[]]);

      await prepareNodeFixture(
        fixture,
        server.baseUrl,
        requirement,
        fixture.stageDir,
      );
      const second = await prepareNodeFixture(
        fixture,
        server.baseUrl,
        requirement,
        fixture.stageDir,
      );

      expect(second.cacheHit).toBe(true);
      expect(
        server
          .getRequests()
          .filter(
            ({ method, pathname }) =>
              method === "GET" && pathname === "/index.json",
          ),
      ).toHaveLength(1);
    });

    it("does not request Node metadata when no runtime requirement is provided", async () => {
      const fixture = await createPrepareFixture();
      const server = await startNodeDistributionServer(
        createFakeNodeDistribution([
          createFakeNodeRelease("v24.4.0", "Krypton"),
        ]),
      );

      await prepareNodeFixture(fixture, server.baseUrl, undefined);

      expect(server.getRequests()).toEqual([]);
      await expect(
        access(join(fixture.cacheDir, "node-catalog.json")),
      ).rejects.toThrow();
      await expect(
        access(join(fixture.cacheDir, "artifacts", "node")),
      ).rejects.toThrow();
    });

    it("does not request Node metadata when the runtime is not required", async () => {
      const fixture = await createPrepareFixture();
      const server = await startNodeDistributionServer(
        createFakeNodeDistribution([
          createFakeNodeRelease("v24.4.0", "Krypton"),
        ]),
      );

      await prepareNodeFixture(fixture, server.baseUrl, {
        ...createNodeRequirement("*", [[]]),
        required: false,
      });

      expect(server.getRequests()).toEqual([]);
      await expect(
        access(join(fixture.cacheDir, "node-catalog.json")),
      ).rejects.toThrow();
      await expect(
        access(join(fixture.cacheDir, "artifacts", "node")),
      ).rejects.toThrow();
    });

    it("clears a shared update request only after both runtime catalogs are refreshed", async () => {
      const fixture = await createPrepareFixture();
      const artifact = await createFakeNodeTarGzArtifact(fakeNodeVersion, {
        includeLicense: true,
        nodeIsSymlink: false,
        includeNpmSymlink: false,
        includeTraversalEntry: false,
      });
      const server = await startNodeDistributionServer(
        createFakeNodeDistribution([
          createFakeNodeRelease(fakeNodeVersion, "Krypton", artifact),
        ]),
      );
      const requirement = createNodeRequirement("*", [[]]);
      await prepareNodeFixture(
        fixture,
        server.baseUrl,
        requirement,
        join(fixture.projectPath, ".muon", "update-warmup"),
      );
      await copyFile(
        fixture.catalogPath,
        join(fixture.cacheDir, "cef-catalog.json"),
      );
      const nodeCatalogRequestsBefore = server
        .getRequests()
        .filter(({ pathname }) => pathname === "/index.json").length;
      await writeLauncherIni(
        fixture.muonPath,
        `[runtime]
catalogRefreshIntervalSeconds=604800

[cef]
versionPolicy=exact
exactVersion=fake-cef
lastCatalogUpdateUnix=${futureCatalogTimestamp}

[node]
lastCatalogUpdateUnix=${futureCatalogTimestamp}

[update]
requested=true
requestedAtUnix=123
`,
      );
      const sourceLauncherConfig = await readFile(
        join(fixture.muonPath, "muon-launcher.ini"),
        "utf8",
      );

      await runMuonPrepare({
        muonPath: fixture.muonPath,
        cefPath: undefined,
        stageDir: fixture.stageDir,
        target: "linux-amd64",
        cacheDir: fixture.cacheDir,
        nodeRuntimeRequirement: requirement,
        force: false,
        quiet: false,
        prepareExecutablePath,
        environment: {
          ...process.env,
          MUON_CEF_CATALOG_URL: fixture.catalogPath,
          MUON_NODE_DIST_URL: server.baseUrl,
        },
        cwd: process.cwd(),
      });

      const state = await readLauncherCatalogState(fixture.stageDir);
      expect(state.cefLastCatalogUpdateUnix).toBeGreaterThan(0);
      expect(state.cefLastCatalogUpdateUnix).toBeLessThan(
        futureCatalogTimestamp,
      );
      expect(state.nodeLastCatalogUpdateUnix).toBeGreaterThan(0);
      expect(state.nodeLastCatalogUpdateUnix).toBeLessThan(
        futureCatalogTimestamp,
      );
      expect(state.updateRequested).toBe(false);
      expect(state.updateRequestedAtUnix).toBe(0);
      await expect(
        readFile(join(fixture.muonPath, "muon-launcher.ini"), "utf8"),
      ).resolves.toBe(sourceLauncherConfig);
      expect(
        server
          .getRequests()
          .filter(({ pathname }) => pathname === "/index.json").length,
      ).toBeGreaterThan(nodeCatalogRequestsBefore);
    });

    it("retains a shared update request when one catalog refresh falls back to cache", async () => {
      const fixture = await createPrepareFixture();
      const artifact = await createFakeNodeTarGzArtifact(fakeNodeVersion, {
        includeLicense: true,
        nodeIsSymlink: false,
        includeNpmSymlink: false,
        includeTraversalEntry: false,
      });
      const server = await startNodeDistributionServer(
        createFakeNodeDistribution([
          createFakeNodeRelease(fakeNodeVersion, "Krypton", artifact),
        ]),
      );
      const requirement = createNodeRequirement("*", [[]]);

      await prepareNodeFixture(
        fixture,
        server.baseUrl,
        requirement,
        fixture.stageDir,
      );
      await copyFile(
        fixture.catalogPath,
        join(fixture.cacheDir, "cef-catalog.json"),
      );
      await server.close();
      await writeLauncherIni(
        fixture.stageDir,
        `[runtime]
catalogRefreshIntervalSeconds=604800

[cef]
versionPolicy=exact
exactVersion=fake-cef
lastCatalogUpdateUnix=${futureCatalogTimestamp}

[node]
lastCatalogUpdateUnix=${futureCatalogTimestamp}

[update]
requested=true
requestedAtUnix=123
`,
      );

      await runMuonPrepare({
        muonPath: fixture.muonPath,
        cefPath: undefined,
        stageDir: fixture.stageDir,
        target: "linux-amd64",
        cacheDir: fixture.cacheDir,
        nodeRuntimeRequirement: requirement,
        force: false,
        quiet: false,
        prepareExecutablePath,
        environment: {
          ...process.env,
          MUON_CEF_CATALOG_URL: fixture.catalogPath,
          MUON_NODE_DIST_URL: server.baseUrl,
        },
        cwd: process.cwd(),
      });

      const state = await readLauncherCatalogState(fixture.stageDir);
      expect(state.cefLastCatalogUpdateUnix).toBeGreaterThan(0);
      expect(state.cefLastCatalogUpdateUnix).toBeLessThan(
        futureCatalogTimestamp,
      );
      expect(state.nodeLastCatalogUpdateUnix).toBe(futureCatalogTimestamp);
      expect(state.updateRequested).toBe(true);
      expect(state.updateRequestedAtUnix).toBe(123);
    });
  });

  describe("Node runtime installation", () => {
    let installHarnessPath = "";

    const getInstallHarnessPath = async (): Promise<string> => {
      if (installHarnessPath === "") {
        installHarnessPath = await buildNodeInstallHarness(
          await createSuiteTemporaryDirectory("muon-node-install-harness-"),
        );
      }
      return installHarnessPath;
    };

    it("downloads and installs only the Node executable and license into the runtime namespace", async () => {
      const fixture = await createPrepareFixture();
      const artifact = await createFakeNodeTarGzArtifact(fakeNodeVersion, {
        includeLicense: true,
        nodeIsSymlink: false,
        includeNpmSymlink: true,
        includeTraversalEntry: false,
      });
      const server = await startNodeDistributionServer(
        createFakeNodeDistribution([
          createFakeNodeRelease(fakeNodeVersion, "Krypton", artifact),
        ]),
      );

      const result = await prepareNodeFixture(
        fixture,
        server.baseUrl,
        createNodeRequirement("*", [[]]),
      );
      const stagePath = requireStagePath(result);
      const nodeRoot = join(stagePath, "runtimes", "node");
      const nodePath = join(nodeRoot, "bin", "node");
      const licensePath = join(nodeRoot, "LICENSE");

      expect(await listCacheEntries(nodeRoot)).toEqual([
        "LICENSE",
        "bin",
        "bin/node",
      ]);
      await expect(readFile(nodePath)).resolves.toEqual(
        createFakeNodeExecutableContent(fakeNodeVersion),
      );
      await expect(readFile(licensePath)).resolves.toEqual(
        fakeNodeLicenseContent,
      );
      expect((await stat(nodePath)).mode & 0o777).toBe(0o755);
      expect((await stat(licensePath)).mode & 0o777).toBe(0o644);
      const { stdout } = await execFileAsync(nodePath, [], {
        encoding: "utf8",
      });
      expect(stdout.trim()).toBe("fake-node-v24.4.0");
      await expect(access(join(stagePath, "bin", "node"))).rejects.toThrow();
      expect(
        server
          .getRequests()
          .filter(
            ({ method, pathname }) =>
              method === "GET" &&
              pathname === `/${artifact.version}/${artifact.fileName}`,
          ),
      ).toHaveLength(1);
    });

    it("rebuilds a staged runtime when the selected Node artifact changes", async () => {
      const fixture = await createPrepareFixture();
      const firstArtifact = await createFakeNodeTarGzArtifact("v22.18.0", {
        includeLicense: true,
        nodeIsSymlink: false,
        includeNpmSymlink: false,
        includeTraversalEntry: false,
      });
      const secondArtifact = await createFakeNodeTarGzArtifact("v24.4.0", {
        includeLicense: true,
        nodeIsSymlink: false,
        includeNpmSymlink: false,
        includeTraversalEntry: false,
      });
      const server = await startNodeDistributionServer(
        createFakeNodeDistribution([
          createFakeNodeRelease(firstArtifact.version, "Jod", firstArtifact),
          createFakeNodeRelease(
            secondArtifact.version,
            "Krypton",
            secondArtifact,
          ),
        ]),
      );

      const first = await prepareNodeFixture(
        fixture,
        server.baseUrl,
        createNodeRequirement(firstArtifact.version.slice(1), [
          [firstArtifact.version.slice(1)],
        ]),
      );
      const stagePath = requireStagePath(first);
      const nodePath = join(stagePath, "runtimes", "node", "bin", "node");
      const firstMarker = await readRuntimeReadyMarker(stagePath);
      await writeFile(join(stagePath, "cached-only.txt"), "cached\n");

      const second = await prepareNodeFixture(
        fixture,
        server.baseUrl,
        createNodeRequirement(secondArtifact.version.slice(1), [
          [secondArtifact.version.slice(1)],
        ]),
      );
      const secondMarker = await readRuntimeReadyMarker(stagePath);

      expect(firstMarker.nodeFingerprint).toBe(firstArtifact.sha256);
      expect(second.stagePath).toBe(stagePath);
      expect(second.cacheHit).toBe(false);
      expect(secondMarker.nodeFingerprint).toBe(secondArtifact.sha256);
      expect(secondMarker.nodeFingerprint).not.toBe(
        firstMarker.nodeFingerprint,
      );
      await expect(readFile(nodePath)).resolves.toEqual(
        createFakeNodeExecutableContent(secondArtifact.version),
      );
      await expect(
        access(join(stagePath, "cached-only.txt")),
      ).rejects.toThrow();
    });

    it("extracts a deflated Windows ZIP into the normalized Node runtime layout", async () => {
      const fixture = await createPrepareFixture();
      const artifact = createFakeNodeZipArtifact();
      const runtimeRoot = join(fixture.projectPath, "windows-runtime");

      const { stdout } = await runNodeInstallHarness(
        await getInstallHarnessPath(),
        fixture,
        artifact,
        "windows-amd64",
        runtimeRoot,
      );
      const nodeRoot = join(runtimeRoot, "runtimes", "node");

      expect(stdout.trim()).toBe("2");
      expect(await listCacheEntries(nodeRoot)).toEqual([
        "LICENSE",
        "bin",
        "bin/node.exe",
      ]);
      await expect(
        readFile(join(nodeRoot, "bin", "node.exe")),
      ).resolves.toEqual(Buffer.from("MZfake-node.exe\n"));
      await expect(readFile(join(nodeRoot, "LICENSE"))).resolves.toEqual(
        fakeNodeLicenseContent,
      );
    });

    it("validates every archive path and rejects a trailing traversal entry", async () => {
      const fixture = await createPrepareFixture();
      const artifact = await createFakeNodeTarGzArtifact(fakeNodeVersion, {
        includeLicense: true,
        nodeIsSymlink: false,
        includeNpmSymlink: false,
        includeTraversalEntry: true,
      });
      const runtimeRoot = join(fixture.projectPath, "traversal-runtime");

      await expect(
        runNodeInstallHarness(
          await getInstallHarnessPath(),
          fixture,
          artifact,
          "linux-amd64",
          runtimeRoot,
        ),
      ).rejects.toThrow();

      expect(
        await listDirectoryEntriesIfPresent(
          join(runtimeRoot, "runtimes", "node"),
        ),
      ).toEqual([]);
      await expect(
        access(join(fixture.projectPath, "node-runtime-escape.txt")),
      ).rejects.toThrow();
    });

    it("rejects a symbolic link in place of the target Node executable", async () => {
      const fixture = await createPrepareFixture();
      const artifact = await createFakeNodeTarGzArtifact(fakeNodeVersion, {
        includeLicense: true,
        nodeIsSymlink: true,
        includeNpmSymlink: false,
        includeTraversalEntry: false,
      });
      const runtimeRoot = join(fixture.projectPath, "symlink-runtime");

      await expect(
        runNodeInstallHarness(
          await getInstallHarnessPath(),
          fixture,
          artifact,
          "linux-amd64",
          runtimeRoot,
        ),
      ).rejects.toThrow();
      expect(
        await listDirectoryEntriesIfPresent(
          join(runtimeRoot, "runtimes", "node"),
        ),
      ).toEqual([]);
    });

    it("rejects an archive without the Node.js license", async () => {
      const fixture = await createPrepareFixture();
      const artifact = await createFakeNodeTarGzArtifact(fakeNodeVersion, {
        includeLicense: false,
        nodeIsSymlink: false,
        includeNpmSymlink: false,
        includeTraversalEntry: false,
      });
      const runtimeRoot = join(fixture.projectPath, "missing-license-runtime");

      await expect(
        runNodeInstallHarness(
          await getInstallHarnessPath(),
          fixture,
          artifact,
          "linux-amd64",
          runtimeRoot,
        ),
      ).rejects.toThrow();
      expect(
        await listDirectoryEntriesIfPresent(
          join(runtimeRoot, "runtimes", "node"),
        ),
      ).toEqual([]);
    });
  });

  it("returns the same staged runtime for concurrent calls", async () => {
    const fixture = await createPrepareFixture();

    const results = await Promise.all([
      prepareFixture(fixture),
      prepareFixture(fixture),
    ]);

    expect(results[0]?.stagePath).toBe(results[1]?.stagePath);
    await expect(
      access(join(results[0]?.stagePath ?? "", runtimeReadyMarkerFileName)),
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

  it("ignores generated runtime directories and legacy and current ready markers in the source runtime", async () => {
    const fixture = await createPrepareFixture();
    const sourceNodePath = join(
      fixture.muonPath,
      "runtimes",
      "node",
      "bin",
      "node",
    );
    const legacyMarkerPath = join(
      fixture.muonPath,
      legacyRuntimeReadyMarkerFileName,
    );
    const currentMarkerPath = join(
      fixture.muonPath,
      runtimeReadyMarkerFileName,
    );
    const cefMarkerPath = join(fixture.muonPath, ".muon-cef-ready.json");
    await mkdir(dirname(sourceNodePath), { recursive: true });
    await writeFile(sourceNodePath, "source Node v1\n");
    await writeFile(legacyMarkerPath, "source legacy marker v1\n");
    await writeFile(currentMarkerPath, "source current marker v1\n");
    await writeFile(cefMarkerPath, "source CEF marker v1\n");

    const first = await prepareFixture(fixture);
    const stagePath = requireStagePath(first);
    await writeFile(join(stagePath, "cached-only.txt"), "cached\n");
    await writeFile(sourceNodePath, "source Node v2\n");
    await writeFile(legacyMarkerPath, "source legacy marker v2\n");
    await writeFile(currentMarkerPath, "source current marker v2\n");
    await writeFile(cefMarkerPath, "source CEF marker v2\n");

    const second = await prepareFixture(fixture);
    const readyMarker = await readRuntimeReadyMarker(stagePath);

    expect(second.stagePath).toBe(stagePath);
    expect(second.cacheHit).toBe(true);
    await expect(
      readFile(join(stagePath, "cached-only.txt"), "utf8"),
    ).resolves.toBe("cached\n");
    await expect(access(join(stagePath, "runtimes"))).rejects.toThrow();
    await expect(
      access(join(stagePath, legacyRuntimeReadyMarkerFileName)),
    ).rejects.toThrow();
    await expect(
      access(join(stagePath, ".muon-cef-ready.json")),
    ).rejects.toThrow();
    expect(readyMarker.ready).toBe(true);
    expect(readyMarker.muonFingerprint).toMatch(sha256HexPattern);
    expect(readyMarker.cefFingerprint).toMatch(sha256HexPattern);
    expect(readyMarker.nodeFingerprint).toBe(emptySha256Fingerprint);
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

  it("rejects the insecure localhost switch at the launcher boundary", async () => {
    for (const [index, argument] of [
      "--allow-insecure-localhost",
      "--allow-insecure-localhost=true",
    ].entries()) {
      const fixture = await createPrepareFixture();
      const stateHome = await createTemporaryDirectory(
        "muon-launcher-reject-state-",
      );
      const appId = `scope.reject-insecure-${index}`;
      const stateRuntimePath = getLinuxPortableStateRuntimePath(
        stateHome,
        appId,
      );
      const appLauncherPath = await writeEmbeddedLauncher(fixture, {
        launcher: { appId },
      });

      await expect(
        execFileAsync(appLauncherPath, [argument], {
          cwd: fixture.projectPath,
          encoding: "utf8",
          env: {
            ...process.env,
            MUON_CACHE_DIR: fixture.cacheDir,
            MUON_CEF_CATALOG_URL: fixture.catalogPath,
            XDG_STATE_HOME: stateHome,
          },
        }),
      ).rejects.toMatchObject({
        code: 1,
        stderr: expect.stringContaining(
          "muon-launcher: --allow-insecure-localhost is not supported",
        ),
      });
      await expect(access(stateRuntimePath)).rejects.toThrow();
    }
  });

  it("installs Node into a staged runtime from the embedded launcher requirement", async () => {
    const fixture = await createPrepareFixture();
    const stateHome = await createTemporaryDirectory(
      "muon-launcher-node-state-",
    );
    const dataHome = await createTemporaryDirectory("muon-launcher-node-data-");
    const appId = "scope.node-staged-app";
    const stateRuntimePath = getLinuxPortableStateRuntimePath(stateHome, appId);
    const artifact = await createFakeNodeTarGzArtifact(fakeNodeVersion, {
      includeLicense: true,
      nodeIsSymlink: false,
      includeNpmSymlink: false,
      includeTraversalEntry: false,
    });
    const server = await startNodeDistributionServer(
      createFakeNodeDistribution([
        createFakeNodeRelease(fakeNodeVersion, "Krypton", artifact),
      ]),
    );
    await writeFile(
      join(fixture.muonPath, "muon-core"),
      "#!/usr/bin/env bash\nexit 0\n",
    );
    await chmod(join(fixture.muonPath, "muon-core"), 0o755);
    const appLauncherPath = await writeEmbeddedLauncher(fixture, {
      launcher: {
        appId,
        nodeRuntime: createNodeRequirement("*", [[]]),
      },
    });

    await execFileAsync(appLauncherPath, [], {
      cwd: fixture.projectPath,
      encoding: "utf8",
      env: {
        ...process.env,
        MUON_CACHE_DIR: fixture.cacheDir,
        MUON_CEF_CATALOG_URL: fixture.catalogPath,
        MUON_NODE_DIST_URL: server.baseUrl,
        XDG_STATE_HOME: stateHome,
        XDG_DATA_HOME: dataHome,
      },
    });

    const nodePath = join(stateRuntimePath, "runtimes", "node", "bin", "node");
    await expect(readFile(nodePath)).resolves.toEqual(
      createFakeNodeExecutableContent(fakeNodeVersion),
    );
    const readyMarker = await readRuntimeReadyMarker(stateRuntimePath);
    expect(readyMarker.nodeFingerprint).toBe(artifact.sha256);
    expect(
      server
        .getRequests()
        .filter(
          ({ method, pathname }) =>
            method === "GET" &&
            pathname === `/${artifact.version}/${artifact.fileName}`,
        ),
    ).toHaveLength(1);
  });

  it("launchers a portable runtime in the user state directory and forwards the core exit code", async () => {
    const fixture = await createPrepareFixture();
    const stateHome = await createTemporaryDirectory("muon-launcher-state-");
    const dataHome = await createTemporaryDirectory("muon-launcher-data-");
    const appId = "scope.sample-app";
    const stateRuntimePath = getLinuxPortableStateRuntimePath(stateHome, appId);
    const desktopEntryPath = getUserDesktopEntryPath(dataHome, appId);
    const outputDirectory = await createTemporaryDirectory(
      "muon-launcher-output-",
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
    const appLauncherPath = await writeEmbeddedLauncher(fixture, {
      launcher: { appId },
    });

    await expect(
      execFileAsync(appLauncherPath, ["--alpha", "two words"], {
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
        "X-muon-Managed=true",
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

  it("launchers a portable install in place without using the user state runtime", async () => {
    const fixture = await createPrepareFixture();
    const stateHome = await createTemporaryDirectory(
      "muon-launcher-portable-state-",
    );
    const appId = "scope.portable-app";
    const stateRuntimePath = getLinuxPortableStateRuntimePath(stateHome, appId);
    const outputDirectory = await createTemporaryDirectory(
      "muon-launcher-portable-output-",
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
    await writeFile(
      join(fixture.muonPath, "muon-install.json"),
      `${JSON.stringify(
        {
          type: "portable",
          runtimeMode: "in-place",
        },
        null,
        2,
      )}\n`,
    );
    const appLauncherPath = await writeEmbeddedLauncher(fixture, {
      launcher: { appId },
    });

    await expect(
      execFileAsync(appLauncherPath, [], {
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
      readFile(join(outputDirectory, "cwd.txt"), "utf8"),
    ).resolves.toBe(`${fixture.muonPath}\n`);
    await expect(
      readFile(join(fixture.muonPath, "libcef.so"), "utf8"),
    ).resolves.toBe("cef library\n");
    await expect(access(join(stateRuntimePath, "muon-core"))).rejects.toThrow();
  });

  it("installs Node into a portable in-place runtime from the embedded launcher requirement", async () => {
    const fixture = await createPrepareFixture();
    const stateHome = await createTemporaryDirectory(
      "muon-launcher-node-portable-state-",
    );
    const dataHome = await createTemporaryDirectory(
      "muon-launcher-node-portable-data-",
    );
    const appId = "scope.node-portable-app";
    const stateRuntimePath = getLinuxPortableStateRuntimePath(stateHome, appId);
    const artifact = await createFakeNodeTarGzArtifact(fakeNodeVersion, {
      includeLicense: true,
      nodeIsSymlink: false,
      includeNpmSymlink: false,
      includeTraversalEntry: false,
    });
    const server = await startNodeDistributionServer(
      createFakeNodeDistribution([
        createFakeNodeRelease(fakeNodeVersion, "Krypton", artifact),
      ]),
    );
    await writeFile(
      join(fixture.muonPath, "muon-core"),
      "#!/usr/bin/env bash\nexit 0\n",
    );
    await chmod(join(fixture.muonPath, "muon-core"), 0o755);
    await writeFile(
      join(fixture.muonPath, "muon-install.json"),
      `${JSON.stringify(
        {
          type: "portable",
          runtimeMode: "in-place",
        },
        null,
        2,
      )}\n`,
    );
    const appLauncherPath = await writeEmbeddedLauncher(fixture, {
      launcher: {
        appId,
        nodeRuntime: createNodeRequirement("*", [[]]),
      },
    });

    await execFileAsync(appLauncherPath, [], {
      cwd: fixture.projectPath,
      encoding: "utf8",
      env: {
        ...process.env,
        MUON_CACHE_DIR: fixture.cacheDir,
        MUON_CEF_CATALOG_URL: fixture.catalogPath,
        MUON_NODE_DIST_URL: server.baseUrl,
        XDG_STATE_HOME: stateHome,
        XDG_DATA_HOME: dataHome,
      },
    });

    const nodePath = join(fixture.muonPath, "runtimes", "node", "bin", "node");
    await expect(readFile(nodePath)).resolves.toEqual(
      createFakeNodeExecutableContent(fakeNodeVersion),
    );
    await expect(access(join(stateRuntimePath, "muon-core"))).rejects.toThrow();
    const readyMarker = await readRuntimeReadyMarker(fixture.muonPath);
    expect(readyMarker.nodeFingerprint).toBe(artifact.sha256);
    expect(
      server
        .getRequests()
        .filter(
          ({ method, pathname }) =>
            method === "GET" &&
            pathname === `/${artifact.version}/${artifact.fileName}`,
        ),
    ).toHaveLength(1);
  });

  it("removes a generated Node runtime when a portable in-place launcher no longer requires it", async () => {
    const fixture = await createPrepareFixture();
    const stateHome = await createTemporaryDirectory(
      "muon-launcher-node-removal-state-",
    );
    const appId = "scope.node-removal-app";
    const artifact = await createFakeNodeTarGzArtifact(fakeNodeVersion, {
      includeLicense: true,
      nodeIsSymlink: false,
      includeNpmSymlink: false,
      includeTraversalEntry: false,
    });
    const server = await startNodeDistributionServer(
      createFakeNodeDistribution([
        createFakeNodeRelease(fakeNodeVersion, "Krypton", artifact),
      ]),
    );
    await writeFile(
      join(fixture.muonPath, "muon-core"),
      "#!/usr/bin/env bash\nexit 0\n",
    );
    await chmod(join(fixture.muonPath, "muon-core"), 0o755);
    await writeFile(
      join(fixture.muonPath, "muon-install.json"),
      `${JSON.stringify(
        {
          type: "portable",
          runtimeMode: "in-place",
        },
        null,
        2,
      )}\n`,
    );
    const appLauncherPath = await writeEmbeddedLauncher(fixture, {
      launcher: {
        appId,
        nodeRuntime: createNodeRequirement("*", [[]]),
      },
    });
    const environment = {
      ...process.env,
      MUON_CACHE_DIR: fixture.cacheDir,
      MUON_CEF_CATALOG_URL: fixture.catalogPath,
      MUON_NODE_DIST_URL: server.baseUrl,
      XDG_STATE_HOME: stateHome,
    };

    await execFileAsync(appLauncherPath, [], {
      cwd: fixture.projectPath,
      encoding: "utf8",
      env: environment,
    });
    await expect(
      access(join(fixture.muonPath, "runtimes", "node", "bin", "node")),
    ).resolves.toBeUndefined();

    await writeEmbeddedLauncher(fixture, {
      launcher: { appId },
    });
    await execFileAsync(appLauncherPath, [], {
      cwd: fixture.projectPath,
      encoding: "utf8",
      env: environment,
    });

    await expect(
      access(join(fixture.muonPath, "runtimes", "node")),
    ).rejects.toThrow();
    const readyMarker = await readRuntimeReadyMarker(fixture.muonPath);
    expect(readyMarker.nodeFingerprint).toBe(emptySha256Fingerprint);
  });

  it("invalidates the in-place ready marker before a runtime update can fail", async () => {
    const fixture = await createPrepareFixture();
    const stateHome = await createTemporaryDirectory(
      "muon-launcher-node-failure-state-",
    );
    const appId = "scope.node-failure-app";
    const firstArtifact = await createFakeNodeTarGzArtifact("v22.18.0", {
      includeLicense: true,
      nodeIsSymlink: false,
      includeNpmSymlink: false,
      includeTraversalEntry: false,
    });
    const invalidArtifact = await createFakeNodeTarGzArtifact("v24.4.0", {
      includeLicense: false,
      nodeIsSymlink: false,
      includeNpmSymlink: false,
      includeTraversalEntry: false,
    });
    const server = await startNodeDistributionServer(
      createFakeNodeDistribution([
        createFakeNodeRelease(firstArtifact.version, "Jod", firstArtifact),
        createFakeNodeRelease(
          invalidArtifact.version,
          "Krypton",
          invalidArtifact,
        ),
      ]),
    );
    await writeFile(
      join(fixture.muonPath, "muon-core"),
      "#!/usr/bin/env bash\nexit 0\n",
    );
    await chmod(join(fixture.muonPath, "muon-core"), 0o755);
    await writeFile(
      join(fixture.muonPath, "muon-install.json"),
      `${JSON.stringify(
        {
          type: "portable",
          runtimeMode: "in-place",
        },
        null,
        2,
      )}\n`,
    );
    const appLauncherPath = await writeEmbeddedLauncher(fixture, {
      launcher: {
        appId,
        nodeRuntime: createNodeRequirement(firstArtifact.version.slice(1), [
          [firstArtifact.version.slice(1)],
        ]),
      },
    });
    const environment = {
      ...process.env,
      MUON_CACHE_DIR: fixture.cacheDir,
      MUON_CEF_CATALOG_URL: fixture.catalogPath,
      MUON_NODE_DIST_URL: server.baseUrl,
      XDG_STATE_HOME: stateHome,
    };

    await execFileAsync(appLauncherPath, [], {
      cwd: fixture.projectPath,
      encoding: "utf8",
      env: environment,
    });
    await expect(
      access(join(fixture.muonPath, runtimeReadyMarkerFileName)),
    ).resolves.toBeUndefined();

    await writeEmbeddedLauncher(fixture, {
      launcher: {
        appId,
        nodeRuntime: createNodeRequirement(invalidArtifact.version.slice(1), [
          [invalidArtifact.version.slice(1)],
        ]),
      },
    });
    await expect(
      execFileAsync(appLauncherPath, [], {
        cwd: fixture.projectPath,
        encoding: "utf8",
        env: environment,
      }),
    ).rejects.toMatchObject({ code: 1 });

    await expect(
      access(join(fixture.muonPath, runtimeReadyMarkerFileName)),
    ).rejects.toThrow();
    await expect(
      readFile(join(fixture.muonPath, "runtimes", "node", "bin", "node")),
    ).resolves.toEqual(createFakeNodeExecutableContent(firstArtifact.version));
  });

  it("updates the staged runtime and desktop entry from a newer portable distribution", async () => {
    const firstFixture = await createPrepareFixture();
    const secondFixture = await createPrepareFixture();
    const stateHome = await createTemporaryDirectory("muon-launcher-state-");
    const dataHome = await createTemporaryDirectory("muon-launcher-data-");
    const appId = "scope.update-app";
    const stateRuntimePath = getLinuxPortableStateRuntimePath(stateHome, appId);
    const desktopEntryPath = getUserDesktopEntryPath(dataHome, appId);
    const outputDirectory = await createTemporaryDirectory(
      "muon-launcher-output-",
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
    const firstLauncherPath = await writeEmbeddedLauncher(firstFixture, {
      launcher: { appId },
    });

    await expect(
      execFileAsync(firstLauncherPath, [], {
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
    const secondLauncherPath = await writeEmbeddedLauncher(secondFixture, {
      launcher: { appId },
    });

    await expect(
      execFileAsync(secondLauncherPath, [], {
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
    const stateHome = await createTemporaryDirectory("muon-launcher-state-");
    const dataHome = await createTemporaryDirectory("muon-launcher-data-");
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
    const appLauncherPath = await writeEmbeddedLauncher(fixture, {
      launcher: { appId },
    });
    const env = {
      ...process.env,
      MUON_CACHE_DIR: fixture.cacheDir,
      MUON_CEF_CATALOG_URL: fixture.catalogPath,
      XDG_STATE_HOME: stateHome,
      XDG_DATA_HOME: dataHome,
    };

    await expect(
      execFileAsync(appLauncherPath, [], {
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
      execFileAsync(appLauncherPath, [], {
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
      "[Desktop Entry]\nName=Managed\nExec=/tmp/old\nX-muon-Managed=true\n",
    );
    await expect(
      execFileAsync(appLauncherPath, [], {
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
        "X-muon-Managed=true",
        "",
      ].join("\n"),
    );
  });

  it("hides launcher preparation diagnostics when a progress window is available", async () => {
    const xvfbRun = await findCommand("xvfb-run");
    if (xvfbRun === undefined) {
      return;
    }
    const fixture = await createPrepareFixture();
    const stateHome = await createTemporaryDirectory("muon-launcher-state-");
    const outputDirectory = await createTemporaryDirectory(
      "muon-launcher-output-",
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
    const appLauncherPath = await writeEmbeddedLauncher(fixture, {
      launcher: { appId: "progress.sample" },
    });

    await expect(
      execFileAsync(
        xvfbRun,
        [
          "-a",
          "node",
          resolve("..", "scripts/run-xvfb-command.mjs"),
          appLauncherPath,
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

  it("uses embedded defaultVersionPolicy when launcher ini omits versionPolicy", async () => {
    const compatible = await createFakeCefArchive(createTemporaryDirectory, {
      archiveFileName: "cef-launcher-compatible.tar.bz2",
      archiveRootName: "cef_binary_launcher_compatible_linux64_minimal",
      libcefContent: "launcher compatible cef library\n",
    });
    const fixture = await createPrepareFixture(undefined, [
      {
        version: "launcher-compatible",
        chromiumVersion: "199.0.0.0",
        archive: compatible,
      },
      {
        version: "fake-cef",
        chromiumVersion: "100.0.0.0",
        archive: getEmbeddedCefArchive(),
      },
    ]);
    await writeLauncherIni(
      fixture.muonPath,
      `[runtime]
catalogRefreshIntervalSeconds=604800

[cef]
lastCatalogUpdateUnix=0
`,
    );
    const stateHome = await createTemporaryDirectory("muon-launcher-state-");
    const appId = "compat.sample";
    const stateRuntimePath = getLinuxPortableStateRuntimePath(stateHome, appId);
    const outputDirectory = await createTemporaryDirectory(
      "muon-launcher-output-",
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
    const appLauncherPath = await writeEmbeddedLauncher(fixture, {
      launcher: {
        appId,
        defaultVersionPolicy: "compat-latest",
      },
    });

    await expect(
      execFileAsync(appLauncherPath, [], {
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
    ).resolves.toBe("launcher compatible cef library\n");
    await expect(access(join(fixture.muonPath, "libcef.so"))).rejects.toThrow();
    await expect(
      readFile(join(stateRuntimePath, "muon-launcher.ini"), "utf8"),
    ).resolves.toMatch(
      /^\[runtime\]\ncatalogRefreshIntervalSeconds=604800\n\n\[cef\]\nlastCatalogUpdateUnix=\d+\n\n\[node\]\nlastCatalogUpdateUnix=0\n\n\[update\]\nrequested=false\nrequestedAtUnix=0\n$/,
    );
  });

  it("rejects an invalid embedded defaultVersionPolicy during launcher", async () => {
    const fixture = await createPrepareFixture();
    const stateHome = await createTemporaryDirectory("muon-launcher-state-");
    const appLauncherPath = await writeEmbeddedLauncher(
      fixture,
      {
        launcher: {
          appId: "invalid-policy-app",
          defaultVersionPolicy: "invalid",
        },
      },
      "invalid-policy-app",
    );

    await expect(
      execFileAsync(appLauncherPath, [], {
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

  it("rejects an invalid embedded defaultVersionPolicy during portable in-place launcher", async () => {
    const fixture = await createPrepareFixture();
    const stateHome = await createTemporaryDirectory(
      "muon-launcher-in-place-state-",
    );
    await writeFile(
      join(fixture.muonPath, "muon-install.json"),
      `${JSON.stringify(
        {
          type: "portable",
          runtimeMode: "in-place",
        },
        null,
        2,
      )}\n`,
    );
    const appLauncherPath = await writeEmbeddedLauncher(
      fixture,
      {
        launcher: {
          appId: "invalid-in-place-policy-app",
          defaultVersionPolicy: "invalid",
        },
      },
      "invalid-in-place-policy-app",
    );

    await expect(
      execFileAsync(appLauncherPath, [], {
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
