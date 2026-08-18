// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import prettierMax from 'prettier-max';
import screwUp from 'screw-up';
import { defineConfig } from 'vitest/config';

export default defineConfig({
  plugins: [prettierMax(), screwUp()],
  build: {
    emptyOutDir: true,
    minify: false,
    sourcemap: true,
    target: 'es2022',
  },
  test: {
    environment: 'node',
    fileParallelism: true,
    restoreMocks: true,
    testTimeout: 60000,
  },
});
