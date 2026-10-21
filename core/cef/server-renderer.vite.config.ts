// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { runScriptOnceToText, type FunCityLogEntry } from 'funcity';
import prettierMax from 'prettier-max';
import { defineConfig } from 'vite';
import { muonServerRendererBuildOptions } from './build-server-renderer.ts';

export default defineConfig({
  plugins: [
    prettierMax(),
    {
      name: 'muon-server-native-setup',
      async generateBundle(_options, bundle) {
        const chunks = Object.values(bundle).filter(
          (entry) => entry.type === 'chunk'
        );
        if (chunks.length !== 1)
          throw new Error('Expected one server renderer chunk');
        const code = chunks[0]!.code;
        if (code.includes(')MUON_SERVER"'))
          throw new Error('Server setup contains its C++ delimiter');
        const logs: FunCityLogEntry[] = [];
        const source = await runScriptOnceToText(
          '// Generated from the shared TypeScript server renderer.\n#pragma once\n\nstatic constexpr char kMuonServerSetupScript[] = R"MUON_SERVER(\n{{code}}\nMuonServerRenderer.installMuonCefServerApi(namespace, isAllowed);\n)MUON_SERVER";\n',
          {
            sourceId: 'muon_server_renderer_generated.h',
            variables: new Map([['code', code]]),
            logs,
          }
        );
        if (source === undefined || logs.some((log) => log.type === 'error'))
          throw new Error(
            `Cannot render server setup: ${JSON.stringify(logs)}`
          );
        this.emitFile({
          type: 'asset',
          fileName: 'muon_server_renderer_generated.h',
          source,
        });
      },
    },
  ],
  publicDir: false,
  build: {
    ...muonServerRendererBuildOptions,
    rolldownOptions: {
      // Required formatting and type checking dominate this small bundle.
      // prettier-max reports their timings; retain output correctness checks.
      checks: { bundlerTimings: false },
    },
    emptyOutDir: true,
    outDir: '.build/server-renderer',
  },
});
