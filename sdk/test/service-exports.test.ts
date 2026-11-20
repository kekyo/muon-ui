import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import vm from 'node:vm';
import { build } from 'vite';
import { expect, it } from 'vitest';
import type {
  ServiceConnection,
  ServiceTransport,
} from '../src/service-connection.ts';
import type { ServiceInterface } from '../src/service-interface.ts';
import type {
  createServiceRuntime,
  defineService,
} from '../src/service-runtime.ts';

const definition = {
  schemaVersion: 1,
  methods: { ping: { parameters: [], returns: 'uint64', errors: [] } },
  events: {},
} as const;
const transport: ServiceTransport = async (action) => {
  if (action === 'begin') return { callId: '1' };
  if (action === 'result')
    return { status: 'ok', value: '18446744073709551615' };
  if (action === 'callbackNext') return { done: true };
  throw new Error(`Unexpected ${action}`);
};

it('ships working service clients in both ESM and CommonJS entries', async () => {
  const esm = await import(
    new URL('../dist/services.mjs', import.meta.url).href
  );
  const cjs = createRequire(new URL('../package.json', import.meta.url))(
    './dist/services.cjs'
  );
  for (const module of [esm, cjs] as {
    createServiceRuntime: typeof createServiceRuntime;
    defineService: typeof defineService;
    createServiceConnection: (
      definition: ServiceInterface,
      transport: ServiceTransport
    ) => ServiceConnection;
  }[]) {
    const client = module.createServiceConnection(definition, transport);
    expect(await client.call('ping', [])).toBe(18446744073709551615n);
    await client.close();
    const runtime = module.createServiceRuntime(
      module.defineService({
        interface: definition,
        start: () => ({ methods: { ping: () => 37n }, stop: () => {} }),
      }),
      {
        callback: async () => ({
          status: 'error',
          code: 'callback-failed',
          message: 'unused',
        }),
        emit: () => {},
      }
    );
    await runtime.start({
      id: 'test',
      generation: '1',
      config: {},
      startReason: 'acquire',
      dataDirectory: '/private/test',
    });
    expect(await runtime.invoke('1', 'ping', [])).toEqual({
      status: 'ok',
      value: '37',
    });
    await runtime.stop({ reason: 'reference-released' });
  }
});

it('runs the public service entry without Node or browser-only globals', async () => {
  const result = await build({
    configFile: false,
    logLevel: 'silent',
    build: {
      write: false,
      minify: false,
      target: 'es2022',
      lib: {
        entry: fileURLToPath(new URL('../src/services.ts', import.meta.url)),
        name: 'MuonServices',
        formats: ['iife'],
      },
    },
  });
  const output = (Array.isArray(result) ? result[0]! : result) as {
    output: { type: string; code?: string }[];
  };
  const chunk = output.output.find((item) => item.type === 'chunk')!;
  const invoke = vm.runInNewContext(
    `${chunk.code}\n(async () => {
    const client = MuonServices.createServiceConnection(definition, transport);
    const value = await client.call('ping', []);
    await client.close();
    return value;
  })()`,
    { definition, transport }
  );
  expect(await invoke).toBe(18446744073709551615n);
});
