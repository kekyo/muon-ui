// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import { resolve } from "node:path";

import prettierMax from "prettier-max";
import screwUp from "screw-up";
import { defineConfig } from "vitest/config";

export default defineConfig(({ mode }) => {
  const isCjs = mode === "cjs";

  return {
    plugins: [
      prettierMax(),
      screwUp({
        outputMetadataFile: true,
      }),
    ],
    build: {
      emptyOutDir: isCjs,
      lib: {
        entry: isCjs
          ? {
              cli: "src/cli.ts",
              index: "src/index.ts",
              vite: "src/vite.ts",
            }
          : {
              index: "src/index.ts",
              vite: "src/vite.ts",
            },
        formats: [isCjs ? "cjs" : "es"],
        fileName: (format, entryName) =>
          `${entryName}${format === "es" ? ".mjs" : ".cjs"}`,
      },
      minify: false,
      sourcemap: true,
      target: "node20",
      rolldownOptions: {
        external: [/^node:/, "adm-zip", "commander", "vite"],
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
