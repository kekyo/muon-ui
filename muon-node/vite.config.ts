// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import prettierMax from 'prettier-max';
import screwUp from 'screw-up';
import { defineConfig } from 'vitest/config';

import packageJson from './package.json' with { type: 'json' };

export default defineConfig({
  define: {
    __MUON_NODE_SUPPORTED_ENGINE_RANGE__: JSON.stringify(
      packageJson.engines.node
    ),
  },
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
