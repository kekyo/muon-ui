// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { readFile, writeFile } from 'node:fs/promises';
import {
  defineService,
  defineServiceInterface,
} from 'muon-plugin-sdk/services';

export default defineService({
  interface: defineServiceInterface({
    schemaVersion: 1,
    methods: {
      pid: { parameters: [], returns: 'int32', errors: [] },
      crash: { parameters: [], returns: 'void', errors: [] },
    },
    events: {},
  }),
  start: async (context) => {
    await writeFile(context.config.pid!, String(process.pid));
    const mode = await readFile(context.config.control!, 'utf8');
    if (mode === 'start-failure') throw new Error('Author startup rejected');
    if (mode === 'start-timeout') await new Promise<void>(() => {});
    return {
      methods: {
        pid: () => process.pid,
        crash: () => {
          process.exit(23);
        },
      },
      stop: async () => {
        const mode = await readFile(context.config.control!, 'utf8');
        if (mode === 'stop-timeout') await new Promise<void>(() => {});
        if (mode === 'stop-crash') process.exit(24);
        if (mode === 'stop-wait') {
          const response = await fetch(
            context.config.origin + '/lifecycle-stop'
          );
          await response.text();
        }
        await writeFile(context.config.marker!, 'lifecycle cleanup completed');
      },
    };
  },
});
