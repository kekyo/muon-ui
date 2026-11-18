// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { writeFile } from 'node:fs/promises';
import {
  createServiceRuntime,
  defineService,
  defineServiceInterface,
} from 'muon-plugin-sdk/services';

const notifications: { event: string; value: unknown }[] = [];
const runtime = createServiceRuntime(
  defineService({
    interface: defineServiceInterface({
      schemaVersion: 1,
      methods: {
        work: { parameters: ['string'], returns: 'string', errors: [] },
      },
      events: { progress: 'string' },
    }),
    start: (context) => ({
      methods: {
        work: async (_call, url) => {
          const response = await fetch(url);
          const value = await response.text();
          context.emit('progress', value);
          return value;
        },
      },
      stop: async () => {
        const response = await fetch(context.config.stopUrl!);
        await response.text();
        await writeFile(context.config.marker!, 'service stopped');
      },
    }),
  }),
  {
    callback: async () => {
      throw new Error('This feasibility probe has no callbacks');
    },
    emit: (event, value) => {
      notifications.push({ event, value });
    },
  }
);

/** Starts the common SDK runtime inside the existing Node sidecar. */
export const start = async (config: Record<string, string>) =>
  await runtime.start({
    id: 'probe',
    generation: '1',
    startReason: 'acquire',
    dataDirectory: process.cwd(),
    config,
  });
/** Invokes a service method through the common encoded SDK contract. */
export const invoke = async (id: string, method: string, args: unknown[]) =>
  await runtime.invoke(id, method, args);
/** Returns observed notifications for this small compatibility probe. */
export const events = () => notifications.splice(0);
/** Waits for real author cleanup before the outer bridge may be shut down. */
export const stop = async () => await runtime.stop({ reason: 'app-exit' });
