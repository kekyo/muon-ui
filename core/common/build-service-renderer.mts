// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { fileURLToPath } from 'node:url';
import { build, type BuildOptions } from 'vite';

/** Bundle shared services and the CEF installer without runtime imports. */
export const muonServiceRendererBuildOptions: BuildOptions = {
  minify: false,
  sourcemap: false,
  target: 'es2022',
  lib: {
    entry: fileURLToPath(new URL('../cef/renderer/services.ts', import.meta.url)),
    name: 'MuonServiceRenderer',
    formats: ['iife'],
    fileName: () => 'services.js',
  },
};

/**
 * Builds the service facade used by native setup and capability imports.
 * @returns One IIFE defining MuonServiceRenderer in the caller's scope.
 */
export const buildMuonServiceRenderer = async (): Promise<string> => {
  const result = await build({
    configFile: false,
    logLevel: 'warn',
    publicDir: false,
    build: { ...muonServiceRendererBuildOptions, write: false },
  });
  const outputs = Array.isArray(result) ? result : [result];
  if (outputs.length !== 1 || !('output' in outputs[0]!))
    throw new Error('Unexpected service renderer build output');
  const chunks = outputs[0].output;
  if (chunks.length !== 1 || chunks[0]?.type !== 'chunk')
    throw new Error('Service renderer must be one self-contained script');
  return chunks[0].code;
};
