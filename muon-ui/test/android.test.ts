// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { mkdtemp, mkdir, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { afterEach, describe, expect, it, vi } from "vitest";
import { buildMuonApp, normalizeMuonBuildTarget } from "../src/build.js";
import { allMuonTargets } from "../src/targets.js";

const buildAndroid = vi.hoisted(() =>
  vi.fn(async (input) => ({
    target: "android",
    packagePath: join(input.outputDirectory, "application.apk"),
    projectDirectory: input.projectDirectory,
    variant: input.variant,
    applicationId: input.applicationId,
    versionCode: input.versionCode,
    versionName: input.versionName,
    abis: input.abis,
    signing: "debug",
  })),
);
vi.mock("../../muon-android/src/build.js", () => ({
  buildAndroidApplication: buildAndroid,
}));

const roots: string[] = [];
afterEach(async () => {
  await Promise.all(
    roots
      .splice(0)
      .map(async (root) => await rm(root, { recursive: true, force: true })),
  );
  buildAndroid.mockClear();
});
const project = async (config: unknown): Promise<string> => {
  const root = await mkdtemp(join(tmpdir(), "muon-android-config-"));
  roots.push(root);
  await mkdir(join(root, "assets"));
  await writeFile(join(root, "assets/index.html"), "<h1>Application</h1>");
  await writeFile(
    join(root, "package.json"),
    JSON.stringify({ name: "my-application", version: "2.3.4" }),
  );
  await writeFile(join(root, "muon.json"), JSON.stringify(config));
  return root;
};

describe("public Android builds", () => {
  it("accepts an explicit Android target without changing desktop defaults", () => {
    expect(normalizeMuonBuildTarget("android")).toBe("android");
    expect(allMuonTargets).not.toContain("android");
  });
  it("resolves application settings and returns an APK result without desktop runtime fields", async () => {
    const root = await project({
      android: {
        applicationId: "dev.example.app",
        label: "From config",
        versionCode: 7,
      },
      config: { channel: "production" },
    });
    const result = await buildMuonApp({
      root,
      targets: ["android"],
      android: { label: "Explicit label", abis: ["arm64-v8a"] },
    });
    expect(result.targets).toEqual([
      expect.objectContaining({
        target: "android",
        applicationId: "dev.example.app",
        versionCode: 7,
        versionName: "2.3.4",
        signing: "debug",
        packagePath: expect.stringContaining(".apk"),
      }),
    ]);
    expect(result.targets[0]).not.toHaveProperty("launcherPath");
    expect(
      (({ environment, ...input }) => input)(buildAndroid.mock.calls[0]![0]),
    ).toEqual(
      expect.objectContaining({
        label: "Explicit label",
        values: { channel: "production" },
        abis: ["arm64-v8a"],
        plugins: [],
        startPage: "https://main.asset.muon.invalid/index.html",
      }),
    );
  });
  it("translates Vite asset paths to the trusted HTTPS host", async () => {
    const root = await project({});
    await buildMuonApp({
      root,
      targets: ["android"],
      assetPrefix: "main/nested",
      browserStartPage: "asset://main/nested/index.html",
    });
    expect(
      (({ environment, ...input }) => input)(buildAndroid.mock.calls[0]![0]),
    ).toEqual(
      expect.objectContaining({
        assetPath: "nested",
        startPage: "https://main.asset.muon.invalid/nested/index.html",
        applicationId: "dev.muon.my_application",
      }),
    );
  });
  it("packages the common simple-mode built-in policy", async () => {
    const root = await project({
      plugin: {
        mode: "simple",
        pages: ["asset://main/**"],
        plugins: [
          { name: "internal", allow: ["muon.environments.getConfigValues"] },
        ],
      },
    });
    await buildMuonApp({ root, targets: ["android"] });
    expect(buildAndroid.mock.calls[0]![0].pluginAccess).toEqual({
      mode: "simple",
      enabled: true,
      internalAllow: ["muon.environments.getConfigValues"],
    });
  });
  it("disables plugin exposure when no pages are allowed", async () => {
    const root = await project({
      plugin: { mode: "simple", pages: [], plugins: [] },
    });
    await buildMuonApp({ root, targets: ["android"] });
    expect(buildAndroid.mock.calls[0]![0].pluginAccess).toEqual({
      mode: "simple",
      enabled: false,
      internalAllow: [],
    });
  });
  it("rejects a plugin definition that has no common policy", async () => {
    const root = await project({
      android: {
        plugins: [
          {
            name: "example",
            soname: "libexample.so",
            libraries: {},
            allow: ["example.*"],
          },
        ],
      },
    });
    await expect(buildMuonApp({ root, targets: ["android"] })).rejects.toThrow(
      /plugin.plugins/,
    );
  });
  it.each([
    ["https://main.asset.muon.invalid/allowed.html"],
    ["https://example.com/**"],
    ["*"],
  ])("rejects an unenforceable page policy: %s", async (page) => {
    const root = await project({ plugin: { mode: "simple", pages: [page] } });
    await expect(buildMuonApp({ root, targets: ["android"] })).rejects.toThrow(
      /plugin.pages/,
    );
    expect(buildAndroid).not.toHaveBeenCalled();
  });
  it("lets explicit common CLI metadata override configuration defaults", async () => {
    const root = await project({
      android: { applicationId: "dev.config.app", label: "Configured" },
    });
    const result = await buildMuonApp({
      root,
      targets: ["android"],
      appId: "dev.explicit.app",
      appName: "Explicit",
    });
    expect(result.targets[0]).toMatchObject({
      applicationId: "dev.explicit.app",
    });
    expect(buildAndroid.mock.calls[0]![0].label).toBe("Explicit");
  });
  it("refuses to package a copy of the signing keystore with web assets", async () => {
    const root = await project({
      android: {
        signing: {
          keystore: "release.p12",
          keyAlias: "release",
          storePasswordEnv: "MUON_TEST_SIGN_PASSWORD",
        },
      },
    });
    await writeFile(join(root, "release.p12"), "private signing key bytes");
    await writeFile(
      join(root, "assets/renamed.bin"),
      "private signing key bytes",
    );
    const previous = process.env.MUON_TEST_SIGN_PASSWORD;
    process.env.MUON_TEST_SIGN_PASSWORD = "test password";
    try {
      await expect(
        buildMuonApp({ root, targets: ["android"], androidRelease: true }),
      ).rejects.toThrow(/keystore.*web assets/i);
      expect(buildAndroid).not.toHaveBeenCalled();
    } finally {
      if (previous === undefined) delete process.env.MUON_TEST_SIGN_PASSWORD;
      else process.env.MUON_TEST_SIGN_PASSWORD = previous;
    }
  });
  it.each([
    [{ android: { fcm: true } }, /FCM.*unavailable/i],
    [{ android: { quickjs: true } }, /QuickJS.*unavailable/i],
    [{ node: { project: "server" } }, /node.project.*unavailable/],
    [{ android: { applicationId: "invalid-id" } }, /applicationId/],
    [{ android: { versionCode: 0 } }, /versionCode/],
    [{ android: { abis: ["armeabi-v7a"] } }, /abis/],
    [{ android: { permissions: ["invalid permission"] } }, /permissions/],
    [{ browser: { startPage: "https://example.com" } }, /startPage/],
    [{ config: { notAString: 1 } }, /config.*string/],
    [{ plugin: { mode: "validate" } }, /plugin.mode/],
  ])(
    "rejects unsupported or invalid configuration before running Gradle: %j",
    async (config, diagnostic) => {
      const root = await project(config);
      await expect(
        buildMuonApp({ root, targets: ["android"] }),
      ).rejects.toThrow(diagnostic);
      expect(buildAndroid).not.toHaveBeenCalled();
    },
  );
});
