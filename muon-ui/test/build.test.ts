// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

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
import { join } from "node:path";
import { inflateRawSync } from "node:zlib";

import { afterEach, describe, expect, it } from "vitest";
import { build as viteBuild } from "vite";

import {
  createMuonBootstrapEmbeddedConfigSlot,
  createMuonEmbeddedConfigSlot,
  findMuonBootstrapEmbeddedConfigSlot,
  findMuonEmbeddedConfigSlot,
} from "../src/embed-config.js";
import { buildMuonApp } from "../src/build.js";
import muon from "../src/vite.js";

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

const createFakeMuonPackageDist = async (root: string): Promise<string> => {
  const packageDirectory = join(root, "package-dist");
  const runtimeDirectory = join(packageDirectory, "runtime", "linux64");
  const nativeDirectory = join(packageDirectory, "native", "linux64");
  await mkdir(runtimeDirectory, { recursive: true });
  await mkdir(nativeDirectory, { recursive: true });
  await writeExecutable(
    join(runtimeDirectory, "muon-core"),
    createMuonEmbeddedConfigSlot(),
    "core",
  );
  await writeFile(join(runtimeDirectory, "libmuon-ui.so"), "ui\n");
  await writeFile(join(runtimeDirectory, "libcardio.so"), "cardio\n");
  await writeFile(
    join(runtimeDirectory, "THIRD_PARTY_NOTICES.md"),
    "notices\n",
  );
  await writeFile(join(runtimeDirectory, "libcef.so"), "cef\n");
  await writeFile(join(runtimeDirectory, "muon.json"), "{}\n");
  await mkdir(join(runtimeDirectory, "assets"), { recursive: true });
  await writeFile(join(runtimeDirectory, "assets", "debug.txt"), "debug\n");
  await writeExecutable(
    join(nativeDirectory, "muon-bootstrap"),
    createMuonBootstrapEmbeddedConfigSlot(),
    "bootstrap",
  );
  return packageDirectory;
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
          asset: { from: "./old-assets" },
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
      assetSalt: salt,
    });

    const [target] = result.targets;
    expect(target?.target).toBe("linux64");
    expect(target?.outputPath).toBe(join(root, "dist-linux-amd64"));
    expect(target?.launcherPath).toBe(
      join(root, "dist-linux-amd64", "muon-sample"),
    );
    await expect(
      readFile(join(root, "dist-linux-amd64", "muon-core")),
    ).resolves.toBeDefined();
    await expect(
      readFile(join(root, "dist-linux-amd64", "libmuon-ui.so"), "utf8"),
    ).resolves.toBe("ui\n");
    await expect(
      readFile(join(root, "dist-linux-amd64", "libcardio.so"), "utf8"),
    ).resolves.toBe("cardio\n");
    await expect(
      readFile(
        join(root, "dist-linux-amd64", "THIRD_PARTY_NOTICES.md"),
        "utf8",
      ),
    ).resolves.toBe("notices\n");
    await expect(
      exists(join(root, "dist-linux-amd64", "muon.json")),
    ).resolves.toBe(false);
    await expect(
      exists(join(root, "dist-linux-amd64", "assets")),
    ).resolves.toBe(false);
    await expect(
      exists(join(root, "dist-linux-amd64", "muon-prepare")),
    ).resolves.toBe(false);
    await expect(
      exists(join(root, "dist-linux-amd64", "libcef.so")),
    ).resolves.toBe(false);

    const archivePath = join(root, "dist-linux-amd64", "assets.zip");
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
      from: "./assets.zip",
      signature: target?.asset.signature,
      salt: "deadbeef",
    });
    const embeddedCore = await readFile(
      join(root, "dist-linux-amd64", "muon-core"),
    );
    const embeddedLauncher = await readFile(
      join(root, "dist-linux-amd64", "muon-sample"),
    );
    expect(() => findMuonEmbeddedConfigSlot(embeddedCore)).toThrow("found 0");
    expect(() => findMuonBootstrapEmbeddedConfigSlot(embeddedLauncher)).toThrow(
      "found 0",
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

    const outputPath = join(root, "dist-linux-amd64");
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
});
