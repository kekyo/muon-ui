// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { resolve } from "node:path";

import prettierMax from "prettier-max";
import screwUp from "screw-up";
import { defineConfig } from "vitest/config";
import type { UserConfig } from "vite";
import { buildMuonNotificationRenderer } from "../core/common/build-notification-renderer.mjs";

import muonNodePackageJson from "../node/package.json" with { type: "json" };

export default defineConfig(async ({ mode }): Promise<UserConfig> => {
  const isCjs = mode === "cjs";
  const isAndroidHost = mode === "android-host";
  const isLinuxDependencies = mode === "linux-dependencies";

  return {
    define: {
      __MUON_NOTIFICATION_RENDERER_SOURCE__: JSON.stringify(
        await buildMuonNotificationRenderer(),
      ),
      __MUON_NODE_SUPPORTED_ENGINE_RANGE__: JSON.stringify(
        muonNodePackageJson.engines.node,
      ),
    },
    plugins: [
      prettierMax(),
      screwUp({
        outputMetadataFile: true,
        outputMetadataFilePath: "common/generated/packageMetadata.ts",
      }),
    ],
    build: {
      emptyOutDir: isCjs || isAndroidHost || isLinuxDependencies,
      outDir: isLinuxDependencies
        ? ".build/linux-dependencies"
        : isAndroidHost
          ? ".build/android-host"
          : "dist",
      lib: {
        entry: isLinuxDependencies
          ? { "linux-dependencies": "cef/linux-dependencies-cli.ts" }
          : isAndroidHost
            ? { index: "android/index.ts" }
            : isCjs
              ? {
                  cli: "common/cli.ts",
                  index: "common/index.ts",
                  vite: "common/vite.ts",
                }
              : {
                  index: "common/index.ts",
                  vite: "common/vite.ts",
                },
        formats: isLinuxDependencies
          ? ["es"]
          : isAndroidHost
            ? ["es", "cjs"]
            : [isCjs ? "cjs" : "es"],
        fileName: (format, entryName) =>
          `${entryName}${format === "es" ? ".mjs" : ".cjs"}`,
      },
      minify: false,
      sourcemap: true,
      target: "node20",
      rolldownOptions: {
        platform: "node",
        // Required formatting and type checking dominate these small bundles.
        // prettier-max reports their timings; retain output correctness checks.
        checks: { bundlerTimings: false },
        external: [
          /^node:/,
          "adm-zip",
          "commander",
          "funcity",
          "sharp",
          "tar-vern",
          "vite",
        ],
        output: {
          preserveModules: false,
        },
      },
    },
    test: {
      environment: "node",
      fileParallelism: true,
      restoreMocks: true,
      hookTimeout: 60000,
      testTimeout: 60000,
    },
    resolve: {
      alias: {
        "@muon": resolve("src"),
      },
    },
  };
});
