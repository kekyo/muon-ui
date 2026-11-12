// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { runScriptOnceToText, type FunCityLogEntry } from 'funcity';
import prettierMax from 'prettier-max';
import { defineConfig } from 'vite';
import { muonServiceRendererBuildOptions } from '../common/build-service-renderer.mjs';

export default defineConfig({
  plugins: [
    prettierMax(),
    {
      name: 'muon-service-native-setup',
      async generateBundle(_options, bundle) {
        const chunks = Object.values(bundle).filter(
          (entry) => entry.type === 'chunk'
        );
        if (chunks.length !== 1)
          throw new Error('Expected one service renderer chunk');
        const code = chunks[0]!.code;
        if (code.includes(')MUON_SERVICE"'))
          throw new Error('Service setup contains its C++ delimiter');
        const logs: FunCityLogEntry[] = [];
        const source = await runScriptOnceToText(
          '// Generated from the shared TypeScript service renderer.\n#pragma once\n\nstatic constexpr char kMuonServiceSetupScript[] = R"MUON_SERVICE(\n{{code}}\nMuonServiceRenderer.installMuonCefServicesApi(namespace, isAllowed);\n)MUON_SERVICE";\n',
          {
            sourceId: 'muon_service_renderer_generated.h',
            variables: new Map([['code', code]]),
            logs,
          }
        );
        if (source === undefined || logs.some((log) => log.type === 'error'))
          throw new Error(
            `Cannot render service setup: ${JSON.stringify(logs)}`
          );
        this.emitFile({
          type: 'asset',
          fileName: 'muon_service_renderer_generated.h',
          source,
        });
      },
    },
  ],
  publicDir: false,
  build: {
    ...muonServiceRendererBuildOptions,
    rolldownOptions: {
      // Required formatting and type checking dominate this small bundle.
      // prettier-max reports their timings; retain output correctness checks.
      checks: { bundlerTimings: false },
    },
    emptyOutDir: true,
    outDir: '.build/service-renderer',
  },
});
