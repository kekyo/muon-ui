// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import prettierMax from 'prettier-max';
import screwUp from 'screw-up';
import { defineConfig } from 'vite';

export default defineConfig({
  plugins: [prettierMax(), screwUp()],
  base: './',
  build: {
    emptyOutDir: true,
    outDir: 'dist/renderer',
    rolldownOptions: { external: [/^node:/, 'funcity'] },
    lib: {
      entry: 'renderer/entry.ts',
      formats: ['iife'],
      name: 'MuonAndroidRenderer',
      fileName: () => 'renderer.js',
    },
    minify: false,
    sourcemap: true,
    target: 'es2022',
  },
});
