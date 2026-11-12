import { execFile } from 'node:child_process';
import { mkdtemp, rm, writeFile } from 'node:fs/promises';
import { createRequire } from 'node:module';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';
import { expect, it } from 'vitest';

const execute = promisify(execFile);
const compiler = createRequire(import.meta.url).resolve('typescript/bin/tsc');

it.each(['ES2022', 'DOM', 'Node'] as const)(
  'types the shipped services client with %s globals',
  async (environment) => {
    const directory = await mkdtemp(join(tmpdir(), 'muon-service-types-'));
    try {
      await writeFile(
        join(directory, 'tsconfig.json'),
        JSON.stringify({
          compilerOptions: {
            target: 'ES2022',
            module: 'ESNext',
            moduleResolution: 'Bundler',
            strict: true,
            noEmit: true,
            skipLibCheck: false,
            lib: environment === 'DOM' ? ['ES2022', 'DOM'] : ['ES2022'],
            types: environment === 'Node' ? ['node'] : [],
            typeRoots: [
              fileURLToPath(
                new URL('../../node_modules/@types', import.meta.url)
              ),
            ],
          },
          files: ['consumer.ts'],
        })
      );
      await writeFile(
        join(directory, 'consumer.ts'),
        `
      import { bindServiceClient, createServiceClient, defineServiceInterface, defineService, createServiceRuntime, type ServiceCancellationSignal } from ${JSON.stringify(fileURLToPath(new URL('../dist/types/services.js', import.meta.url)))};
      const definition = defineServiceInterface({ schemaVersion: 1, methods: {
        ping: { parameters: [], returns: 'uint64', errors: [] }
      }, events: {} });
      const client = createServiceClient(definition, async () => null);
      const signal: ServiceCancellationSignal = ${environment !== 'ES2022' ? 'new AbortController().signal' : '{ aborted: false, addEventListener: () => {}, removeEventListener: () => {} }'};
      const result: Promise<bigint> = client.call('ping', [], { signal });
      void result;
      const service = defineService({ interface: definition, start: context => ({
        methods: { ping: call => {
          const signal: ServiceCancellationSignal = call.signal;
          const generation: string = context.generation;
          // @ts-expect-error configuration is immutable
          context.config.changed = 'bad';
          void signal; void generation; return 1n;
        } }, stop: async reason => { const message: string = reason; void message; }
      }) });
      const runtime = createServiceRuntime(service, {
        callback: async () => ({ status: 'ok', value: '1' }), emit: () => {}
      });
      const started: Promise<void> = runtime.start({id:'worker',generation:'1',config:{},dataDirectory:'/private'});
      void started;
      // @ts-expect-error method return type is derived from the schema
      defineService({interface: definition, start: () => ({methods:{ping:()=> 'wrong'},stop:()=>{}})});

      ${
        environment === 'DOM'
          ? `
      import ${JSON.stringify(fileURLToPath(new URL('../../ui/muon.d.ts', import.meta.url)))};
      declare const reference: MuonServiceHandle;
      const bound = bindServiceClient(definition, reference);
      const pageResult: Promise<bigint> = bound.call('ping', [], { signal: new AbortController().signal });
      // @ts-expect-error ping has no arguments
      void bound.call('ping', [1]);
      // @ts-expect-error no undeclared method can be called
      void bound.call('missing', []);
      void pageResult;
      `
          : ''
      }
    `
      );
      const result = await (async () => {
        try {
          return await execute(process.execPath, [
            compiler,
            '-p',
            join(directory, 'tsconfig.json'),
          ]);
        } catch (error) {
          const detail = error as Error & { stdout?: string; stderr?: string };
          throw new Error(
            `${detail.message}\n${detail.stdout ?? ''}${detail.stderr ?? ''}`
          );
        }
      })();
      expect(result.stdout).toBe('');
      expect(result.stderr).toBe('');
    } finally {
      await rm(directory, { recursive: true, force: true });
    }
  }
);
