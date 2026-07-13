// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import { execFile } from "node:child_process";
import { mkdir, mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { promisify } from "node:util";

import { afterEach, describe, expect, it } from "vitest";

import {
  createMuonBootstrapEmbeddedConfigSlot,
  createMuonEmbeddedConfigSlot,
  embedMuonConfigInBootstrapFile,
  embedMuonConfigInCoreFile,
  findMuonBootstrapEmbeddedConfigSlot,
  findMuonEmbeddedConfigSlot,
  muonEmbeddedConfigSlotSize,
} from "../src/embed-config.js";

const execFileAsync = promisify(execFile);
const cleanupDirectories: string[] = [];

const createTemporaryDirectory = async (): Promise<string> => {
  const directory = await mkdtemp(join(tmpdir(), "muon-embed-config-"));
  cleanupDirectories.push(directory);
  return directory;
};

const createFakeCore = async (directory: string): Promise<string> => {
  const corePath = join(directory, "muon-core");
  const slot = createMuonEmbeddedConfigSlot();
  await writeFile(
    corePath,
    Buffer.concat([
      Buffer.from("fake executable prefix\n"),
      slot,
      Buffer.from("\nfake executable suffix\n"),
    ]),
  );
  return corePath;
};

const createFakeBootstrap = async (directory: string): Promise<string> => {
  const bootstrapPath = join(directory, "muon-bootstrap");
  const slot = createMuonBootstrapEmbeddedConfigSlot();
  await writeFile(
    bootstrapPath,
    Buffer.concat([
      Buffer.from("fake bootstrap prefix\n"),
      slot,
      Buffer.from("\nfake bootstrap suffix\n"),
    ]),
  );
  return bootstrapPath;
};

const writeConfig = async (
  directory: string,
  content: string,
): Promise<string> => {
  const configPath = join(directory, "muon.json");
  await writeFile(configPath, content);
  return configPath;
};

afterEach(async () => {
  for (const directory of cleanupDirectories.splice(0)) {
    await rm(directory, { recursive: true, force: true });
  }
});

describe("muon embedded config", () => {
  it("embeds JSON5 config into the fixed muon-core slot", async () => {
    const directory = await createTemporaryDirectory();
    const corePath = await createFakeCore(directory);
    const assetSignature =
      "202122232425262728292A2B2C2D2E2F303132333435363738393A3B3C3D3E3F";
    const pluginSignature =
      "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
    const configPath = await writeConfig(
      directory,
      `{
        // JSON5 input must be accepted by the packaging CLI.
        asset: {
          sourcePath: 'assets.zip',
          signature: '${assetSignature}',
          salt: '0A10ff',
        },
        browser: {
          startPage: 'https://example.com/app',
          backgroundColor: '#123abc',
        },
        plugin: {
          plugins: [
            {
              name: 'foobar',
              allow: ['foobar.*'],
              signature: '${pluginSignature}',
              salt: 'deadbeef',
            },
          ],
        },
      }\n`,
    );

    const result = await embedMuonConfigInCoreFile({
      corePath,
      configPath,
      outputPath: undefined,
    });
    const content = await readFile(corePath);
    const payload = content.subarray(
      result.slotOffset,
      result.slotOffset + result.payloadSize,
    );

    expect(result.outputPath).toBe(corePath);
    expect(result.replaced).toBe(false);
    expect(result.payloadSize).toBeGreaterThan(0);
    expect(result.slotOffset).toBe("fake executable prefix\n".length);
    expect(() => findMuonEmbeddedConfigSlot(content)).toThrow("found 0");
    expect(payload.indexOf(Buffer.from(assetSignature, "utf8"))).toBe(-1);
    expect(payload.indexOf(Buffer.from("0A10ff"))).toBe(-1);
    expect(payload.indexOf(Buffer.from(pluginSignature, "utf8"))).toBe(-1);
    expect(payload.indexOf(Buffer.from("deadbeef"))).toBe(-1);
    expect(
      payload.indexOf(Buffer.from(assetSignature, "hex")),
    ).toBeGreaterThanOrEqual(0);
    expect(
      payload.indexOf(Buffer.from([0x0a, 0x10, 0xff])),
    ).toBeGreaterThanOrEqual(0);
    expect(
      payload.indexOf(Buffer.from(pluginSignature, "hex")),
    ).toBeGreaterThanOrEqual(0);
    expect(
      payload.indexOf(Buffer.from([0xde, 0xad, 0xbe, 0xef])),
    ).toBeGreaterThanOrEqual(0);
    const emptySlot = createMuonEmbeddedConfigSlot();
    const tail = content.subarray(
      result.slotOffset + result.payloadSize,
      result.slotOffset + muonEmbeddedConfigSlotSize,
    );
    const emptyTail = emptySlot.subarray(result.payloadSize);
    expect(tail.equals(emptyTail)).toBe(false);
    expect(content.length).toBe(
      "fake executable prefix\n".length +
        muonEmbeddedConfigSlotSize +
        "\nfake executable suffix\n".length,
    );
  });

  it("rejects embedding into an already embedded core", async () => {
    const directory = await createTemporaryDirectory();
    const corePath = await createFakeCore(directory);
    const firstConfigPath = await writeConfig(
      directory,
      `{"browser":{"startPage":"https://first.example/app"}}\n`,
    );
    await embedMuonConfigInCoreFile({
      corePath,
      configPath: firstConfigPath,
      outputPath: undefined,
    });
    const firstContent = await readFile(corePath);
    const secondConfigPath = await writeConfig(
      directory,
      `{"browser":{"startPage":"https://second.example/app"}}\n`,
    );

    await expect(
      embedMuonConfigInCoreFile({
        corePath,
        configPath: secondConfigPath,
        outputPath: undefined,
      }),
    ).rejects.toThrow("found 0");
    expect((await readFile(corePath)).equals(firstContent)).toBe(true);
  });

  it("fails when the encoded config exceeds the fixed slot", async () => {
    const directory = await createTemporaryDirectory();
    const corePath = await createFakeCore(directory);
    const configPath = await writeConfig(
      directory,
      JSON.stringify({
        network: {
          allow: Array.from(
            { length: 4000 },
            (_, index) =>
              `https://example.com/${index.toString().padStart(4, "0")}/**`,
          ),
        },
      }),
    );

    await expect(
      embedMuonConfigInCoreFile({
        corePath,
        configPath,
        outputPath: undefined,
      }),
    ).rejects.toThrow("exceeds");
  });

  it("exposes embed-config from the packaged CLI", async () => {
    const directory = await createTemporaryDirectory();
    const corePath = await createFakeCore(directory);
    const configPath = await writeConfig(
      directory,
      `{"browser":{"startPage":"https://cli.example/app"}}\n`,
    );
    const { stdout } = await execFileAsync(
      process.execPath,
      [
        resolve("dist/cli.cjs"),
        "embed-config",
        "--core-path",
        corePath,
        "--config",
        configPath,
        "--json",
      ],
      { encoding: "utf8" },
    );
    const result = JSON.parse(stdout) as { payloadSize: number };
    const content = await readFile(corePath);

    expect(result.payloadSize).toBeGreaterThan(0);
    expect(
      content.indexOf(Buffer.from("https://cli.example/app")),
    ).toBeGreaterThanOrEqual(0);
    expect(() => findMuonEmbeddedConfigSlot(content)).toThrow("found 0");
  });

  it("embeds JSON5 config into the fixed muon-bootstrap slot", async () => {
    const directory = await createTemporaryDirectory();
    const bootstrapPath = await createFakeBootstrap(directory);
    const configPath = await writeConfig(
      directory,
      `{ bootstrap: { defaultVersionPolicy: 'compat-latest' } }\n`,
    );

    const result = await embedMuonConfigInBootstrapFile({
      bootstrapPath,
      configPath,
      outputPath: undefined,
    });
    const content = await readFile(bootstrapPath);
    const payload = content.subarray(
      result.slotOffset,
      result.slotOffset + result.payloadSize,
    );

    expect(result.outputPath).toBe(bootstrapPath);
    expect(result.payloadSize).toBeGreaterThan(0);
    expect(result.slotOffset).toBe("fake bootstrap prefix\n".length);
    expect(() => findMuonBootstrapEmbeddedConfigSlot(content)).toThrow(
      "found 0",
    );
    expect(
      payload.indexOf(Buffer.from("compat-latest")),
    ).toBeGreaterThanOrEqual(0);
    const tail = content.subarray(
      result.slotOffset + result.payloadSize,
      result.slotOffset + muonEmbeddedConfigSlotSize,
    );
    expect(tail.equals(Buffer.alloc(tail.length, 0))).toBe(false);
  });

  it("writes an embedded bootstrap config to a separate output path", async () => {
    const directory = await createTemporaryDirectory();
    const bootstrapPath = await createFakeBootstrap(directory);
    const outputPath = join(directory, "patched-bootstrap");
    const configPath = await writeConfig(
      directory,
      `{ bootstrap: { defaultVersionPolicy: 'same-major-latest' } }\n`,
    );
    const original = await readFile(bootstrapPath);

    const { stdout } = await execFileAsync(
      process.execPath,
      [
        resolve("dist/cli.cjs"),
        "embed-config",
        "--bootstrap-path",
        bootstrapPath,
        "--config",
        configPath,
        "--output-bootstrap",
        outputPath,
        "--json",
      ],
      { encoding: "utf8" },
    );
    const result = JSON.parse(stdout) as {
      outputPath: string;
      payloadSize: number;
    };
    const patched = await readFile(outputPath);

    expect(result.outputPath).toBe(outputPath);
    expect(result.payloadSize).toBeGreaterThan(0);
    expect((await readFile(bootstrapPath)).equals(original)).toBe(true);
    expect(
      patched.indexOf(Buffer.from("same-major-latest")),
    ).toBeGreaterThanOrEqual(0);
    expect(() => findMuonBootstrapEmbeddedConfigSlot(patched)).toThrow(
      "found 0",
    );
  });
});
