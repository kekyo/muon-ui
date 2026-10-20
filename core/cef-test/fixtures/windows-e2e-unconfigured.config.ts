// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { defineConfig } from "vitest/config";
import baseConfig from "../vite.config.js";

export default defineConfig(async (environment) => {
  const config = await baseConfig(environment);
  return {
    ...config,
    test: {
      ...config.test,
      setupFiles: ["fixtures/windows-e2e-unconfigured-setup.ts"],
    },
  };
});
