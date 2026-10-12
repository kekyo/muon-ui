// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import prettierMax from 'prettier-max';
import screwUp from 'screw-up';
import { configDefaults, defineConfig } from 'vitest/config';

export default defineConfig({
  plugins: [prettierMax(), screwUp()],
  base: './',
  build: {
    emptyOutDir: true,
    minify: false,
    sourcemap: true,
    target: 'es2022',
  },
  test: {
    environment: 'node',
    exclude: [
      ...configDefaults.exclude,
      'android/.generated/**',
      'android/.native-dependencies/**',
    ],
    fileParallelism: true,
    restoreMocks: true,
    testTimeout: 60000,
  },
});
