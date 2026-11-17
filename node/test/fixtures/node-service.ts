// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { writeFile } from 'node:fs/promises';
import {
  defineService,
  defineServiceInterface,
} from 'muon-plugin-sdk/services';

export default defineService({
  interface: defineServiceInterface({
    schemaVersion: 1,
    methods: {
      status: { parameters: [], returns: 'string', errors: [] },
      integer: { parameters: ['int64'], returns: 'int64', errors: [] },
      bytes: { parameters: ['bytes'], returns: 'bytes', errors: [] },
      callback: {
        parameters: [
          'string',
          { callback: { parameters: ['string'], returns: 'string' } },
        ],
        returns: 'string',
        errors: [],
      },
      wait: { parameters: [], returns: 'void', errors: [] },
    },
    events: { progress: 'string' },
  }),
  start: (context) => {
    let status = 'pending';
    const work = (async () => {
      const response = await fetch(context.config.origin + '/work');
      status = await response.text();
      context.emit('progress', status);
    })();
    return {
      methods: {
        status: () => status,
        integer: (_call, value) => value,
        bytes: (_call, value) => value,
        callback: async (_call, value, callback) => await callback(value),
        wait: async (call) => {
          context.emit('progress', 'waiting');
          await new Promise<void>((resolve) => {
            if (call.signal.aborted) resolve();
            else call.signal.addEventListener('abort', resolve, { once: true });
          });
        },
      },
      stop: async () => {
        await work;
        const response = await fetch(context.config.origin + '/stop');
        await response.text();
        await writeFile(context.config.marker!, 'cleanup completed');
      },
    };
  },
});
