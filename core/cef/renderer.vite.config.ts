// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { runScriptOnceToText, type FunCityLogEntry } from 'funcity';
import prettierMax from 'prettier-max';
import { defineConfig } from 'vite';
import { muonNotificationRendererBuildOptions } from '../common/build-notification-renderer.mjs';

export default defineConfig({
  plugins: [
    prettierMax(),
    {
      name: 'muon-notification-native-setup',
      async generateBundle(_options, bundle) {
        const chunks = Object.values(bundle).filter(
          (entry) => entry.type === 'chunk'
        );
        if (chunks.length !== 1)
          throw new Error('Expected one notification renderer chunk');
        const code = chunks[0]!.code;
        if (code.includes(')MUON_NOTIFY"'))
          throw new Error('Notification setup contains its C++ delimiter');
        const logs: FunCityLogEntry[] = [];
        const source = await runScriptOnceToText(
          '// Generated from the shared TypeScript notification renderer.\n#pragma once\n\nstatic constexpr char kMuonNotificationSetupScript[] = R"MUON_NOTIFY(\n{{code}}\nMuonNotificationRenderer.installMuonCefNotificationsApi(namespace, isAllowed);\n)MUON_NOTIFY";\n',
          {
            sourceId: 'muon_notification_renderer_generated.h',
            variables: new Map([['code', code]]),
            logs,
          }
        );
        if (source === undefined || logs.some((log) => log.type === 'error'))
          throw new Error(
            `Cannot render notification setup: ${JSON.stringify(logs)}`
          );
        this.emitFile({
          type: 'asset',
          fileName: 'muon_notification_renderer_generated.h',
          source,
        });
      },
    },
  ],
  publicDir: false,
  build: {
    ...muonNotificationRendererBuildOptions,
    emptyOutDir: true,
    outDir: '.build/notification-renderer',
  },
});
