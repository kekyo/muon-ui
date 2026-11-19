import { expect, it } from 'vitest';
import { createServiceRuntime, defineService } from '../src/service-runtime.ts';

it('opens a generation-owned listener, dispatches binary HTTP to its handler and closes it on stop', async () => {
  const records: string[] = [];
  let sent!: () => void;
  const response = new Promise<void>((resolve) => {
    sent = resolve;
  });
  let event = true;
  const runtime = createServiceRuntime(
    defineService({
      interface: { schemaVersion: 1, methods: {}, events: {} },
      start: async (context) => {
        await context.server.listen({
          name: 'test',
          port: 12480,
          fetch: async (request) => {
            expect(request.method).toBe('POST');
            return { status: 202, body: request.body };
          },
        });
        return { methods: {}, stop: () => {} };
      },
    }),
    {
      callback: async () => {
        throw new Error('Unexpected callback');
      },
      emit: () => {},
      io: async (op, _params, body, signal) => {
        records.push(op);
        let metadata: unknown = null;
        if (op === 'server.create') metadata = { id: '1' };
        if (op === 'server.attach') metadata = { binding: '2' };
        if (op === 'server.listen')
          metadata = {
            host: '127.0.0.1',
            port: 12480,
            url: 'http://127.0.0.1:12480',
          };
        if (op === 'server.next') {
          if (event) {
            event = false;
            return {
              metadata: {
                type: 'request',
                requestId: '3',
                method: 'POST',
                url: 'http://127.0.0.1:12480/',
                headers: [],
              },
              body: new Uint8Array([0, 255]),
            };
          }
          if (!signal.aborted)
            await new Promise<void>((resolve) =>
              signal.addEventListener('abort', resolve, { once: true })
            );
        }
        if (op === 'server.respond') {
          expect(_params.status).toBe(202);
          expect(Array.from(body)).toEqual([0, 255]);
          sent();
        }
        return { metadata, body: new Uint8Array() };
      },
    }
  );
  await runtime.start({
    id: 'server',
    generation: '1',
    config: {},
    dataDirectory: '/private',
    startReason: 'acquire',
  });
  await response;
  await runtime.stop({ reason: 'reference-released' });
  expect(records.slice(0, 3)).toEqual([
    'server.create',
    'server.attach',
    'server.listen',
  ]);
});
