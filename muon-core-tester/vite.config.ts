// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import prettierMax from "prettier-max";
import screwUp from "screw-up";
import { defineConfig } from "vitest/config";

const isValgrindRun = process.env.MUON_TEST_USE_VALGRIND === "1";
const testTimeout = isValgrindRun ? 600000 : 60000;

export default defineConfig({
  plugins: [prettierMax(), screwUp()],
  build: {
    lib: {
      entry: "src/helper.ts",
      formats: ["es", "cjs"],
      fileName: (format, entryName) =>
        `${entryName}${format === "es" ? ".mjs" : ".cjs"}`,
    },
    minify: false,
    sourcemap: true,
    target: "node22",
  },
  test: {
    environment: "node",
    fileParallelism: true,
    restoreMocks: true,
    hookTimeout: testTimeout,
    testTimeout,
  },
});
