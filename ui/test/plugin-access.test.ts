// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { chmod, mkdtemp, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";

import { afterEach, describe, expect, it } from "vitest";

import { resolveMuonPluginAccessOptions } from "../src/plugin-access.js";
import { collectMuonPluginFunctionPathsForAccess } from "../src/plugin-inspector.js";

const cleanupDirectories: string[] = [];
const sha256Signature =
  "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD";

const createTemporaryDirectory = async (): Promise<string> => {
  const directory = await mkdtemp(join(tmpdir(), "muon-plugin-access-"));
  cleanupDirectories.push(directory);
  return directory;
};

const writeValidateModeConfig = async (
  root: string,
  signature: string,
): Promise<void> => {
  await writeFile(
    join(root, "muon.json"),
    `${JSON.stringify(
      {
        plugin: {
          mode: "validate",
          plugins: [
            {
              name: "foobar",
              signature,
              salt: "deadbeef",
              imports: [
                {
                  sources: ["src/native/**"],
                  allow: ["foobar.run"],
                },
              ],
            },
          ],
        },
      },
      null,
      2,
    )}\n`,
  );
};

afterEach(async () => {
  for (const directory of cleanupDirectories.splice(0)) {
    await rm(directory, { recursive: true, force: true });
  }
});

describe("muon plugin access", () => {
  it("accepts and preserves a 64-character SHA-256 signature from muon.json", async () => {
    const root = await createTemporaryDirectory();
    await writeValidateModeConfig(root, sha256Signature);

    const resolved = await resolveMuonPluginAccessOptions({
      root,
      configPath: undefined,
      pluginAccess: undefined,
    });

    expect(resolved.runtimeOverlay.plugins).toEqual([
      {
        name: "foobar",
        signature: sha256Signature,
        salt: "deadbeef",
        allow: ["foobar.run"],
      },
    ]);
  });

  it("skips the native inspector when external validate imports are exact", async () => {
    const root = await createTemporaryDirectory();
    await writeValidateModeConfig(root, sha256Signature);

    const resolved = await resolveMuonPluginAccessOptions({
      root,
      configPath: undefined,
      pluginAccess: undefined,
    });

    await expect(
      collectMuonPluginFunctionPathsForAccess(resolved, {
        ...process.env,
        MUON_PLUGIN_INSPECTOR_PATH: join(root, "missing-inspector"),
      }),
    ).resolves.toEqual([]);
  });

  it("collects external wildcard import function paths through the native inspector", async () => {
    const root = await createTemporaryDirectory();
    await writeFile(
      join(root, "muon.json"),
      `${JSON.stringify(
        {
          plugin: {
            mode: "validate",
            path: "native-plugins",
            plugins: [
              {
                name: "foobar",
                imports: [
                  {
                    sources: ["src/native/**"],
                    allow: ["foobar.native.*"],
                  },
                ],
              },
            ],
          },
        },
        null,
        2,
      )}\n`,
    );
    const inspectorPath = join(root, "fake-inspector.mjs");
    await writeFile(
      inspectorPath,
      [
        "#!/usr/bin/env node",
        'import { readFile } from "node:fs/promises";',
        "const inputPath = process.argv[2];",
        'const input = JSON.parse(await readFile(inputPath, "utf8"));',
        'if (input.path.endsWith("/native-plugins") && input.plugins?.[0]?.name === "foobar") {',
        '  process.stdout.write(JSON.stringify({ plugins: [{ name: "foobar", functions: ["foobar.native.run"] }] }));',
        "} else {",
        "  process.exit(17);",
        "}",
        "",
      ].join("\n"),
    );
    await chmod(inspectorPath, 0o755);

    const resolved = await resolveMuonPluginAccessOptions({
      root,
      configPath: undefined,
      pluginAccess: undefined,
    });

    await expect(
      collectMuonPluginFunctionPathsForAccess(resolved, {
        ...process.env,
        MUON_PLUGIN_INSPECTOR_PATH: inspectorPath,
      }),
    ).resolves.toEqual(["foobar.native.run"]);
  });

  it.each([
    ["63-character", "0".repeat(63)],
    ["65-character", "0".repeat(65)],
    ["non-hexadecimal", "G".repeat(64)],
  ])(
    "rejects a %s plugin signature from Vite options",
    async (_label, signature) => {
      const root = await createTemporaryDirectory();

      await expect(
        resolveMuonPluginAccessOptions({
          root,
          configPath: undefined,
          pluginAccess: {
            mode: "simple",
            plugins: [
              {
                name: "foobar",
                signature,
                salt: "deadbeef",
                allow: ["foobar.run"],
              },
            ],
          },
        }),
      ).rejects.toThrow("64-character SHA-256 hex string");
    },
  );
});
