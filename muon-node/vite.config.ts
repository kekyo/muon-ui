// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import prettierMax from 'prettier-max';
import screwUp from 'screw-up';
import { defineConfig } from 'vitest/config';

export default defineConfig({
  plugins: [
    prettierMax(),
    screwUp({
      outputMetadataFile: true,
    }),
  ],
  build: {
    emptyOutDir: true,
    lib: {
      entry: 'src/cli.ts',
      formats: ['es'],
      fileName: () => 'node-bridge.mjs',
    },
    minify: false,
    sourcemap: false,
    target: 'node20',
    rolldownOptions: {
      external: [/^node:/],
      output: {
        codeSplitting: false,
      },
    },
  },
  test: {
    environment: 'node',
    fileParallelism: true,
    restoreMocks: true,
    hookTimeout: 60000,
    testTimeout: 60000,
  },
});
