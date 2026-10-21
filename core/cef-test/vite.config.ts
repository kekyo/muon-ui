// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import prettierMax from "prettier-max";
import screwUp from "screw-up";
import { defineConfig } from "vitest/config";
import type { UserConfig } from "vite";
import { buildMuonNotificationRenderer } from "../common/build-notification-renderer.mjs";
import { buildMuonServerRenderer } from "../cef/build-server-renderer.ts";

const isValgrindRun = process.env.MUON_TEST_USE_VALGRIND === "1";
// Local launcher startup may legitimately use its 120-second runtime staging
// timeout. Keep the enclosing test alive long enough to finish cleanup.
const testTimeout = isValgrindRun ? 600000 : 180000;

export default defineConfig(async (): Promise<UserConfig> => ({
  define: {
    __MUON_SERVER_RENDERER_SOURCE__: JSON.stringify(
      await buildMuonServerRenderer(),
    ),
    __MUON_NOTIFICATION_RENDERER_SOURCE__: JSON.stringify(
      await buildMuonNotificationRenderer(),
    ),
  },
  plugins: [prettierMax(), screwUp()],
  build: {
    lib: {
      entry: "helper.ts",
      formats: ["es", "cjs"],
      fileName: (format, entryName) =>
        `${entryName}${format === "es" ? ".mjs" : ".cjs"}`,
    },
    minify: false,
    sourcemap: true,
    target: "node22",
    rolldownOptions: {
      // Required formatting and type checking dominate this small bundle.
      // prettier-max reports their timings; retain output correctness checks.
      checks: { bundlerTimings: false },
    },
  },
  test: {
    environment: "node",
    fileParallelism: true,
    restoreMocks: true,
    hookTimeout: testTimeout,
    testTimeout,
  },
}));
