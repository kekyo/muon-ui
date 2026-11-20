// Builds the portable integration fixture used by native host-engine tests.
import { mkdir, writeFile } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { runScriptOnceToText } from 'funcity';
import { build } from 'vite';

const output = process.argv[2];
if (!output) throw new Error('A JavaScript output path is required');
const source = await runScriptOnceToText(
  `
import definition from {{entry}};
import { createServiceRuntime } from {{runtime}};
globalThis.__muonServiceInterface = definition.interface;
globalThis.__muonServiceRuntime = createServiceRuntime(definition, {
  io: globalThis.__muonServiceIo,
  callback: globalThis.__muonServiceCallback,
  emit: globalThis.__muonServiceEmit,
});
`,
  {
    sourceId: 'service-runtime-fixture.fc',
    variables: new Map([
      [
        'entry',
        JSON.stringify(
          process.argv[3] === '--entry'
            ? resolve(process.argv[4])
            : fileURLToPath(
                new URL('../test/fixtures/quickjs-service.ts', import.meta.url)
              )
        ),
      ],
      [
        'runtime',
        JSON.stringify(
          fileURLToPath(new URL('../src/services.ts', import.meta.url))
        ),
      ],
    ]),
  }
);
const entry = fileURLToPath(
  new URL('./virtual-service-runtime-fixture.ts', import.meta.url)
);
const result = await build({
  configFile: false,
  logLevel: 'warn',
  plugins: [
    {
      name: 'muon-service-runtime-fixture',
      resolveId: (id) => (id === entry ? '\0' + entry : undefined),
      load: (id) => (id === '\0' + entry ? source : undefined),
    },
  ],
  build: {
    write: false,
    target: 'es2022',
    minify: false,
    lib: { entry, name: 'MuonServiceFixture', formats: ['iife'] },
  },
});
const bundle = (Array.isArray(result) ? result[0] : result).output.find(
  (item) => item.type === 'chunk'
);
if (!bundle) throw new Error('Service fixture bundle is missing');
await mkdir(dirname(resolve(output)), { recursive: true });
await writeFile(output, bundle.code);

const header = process.argv[3] === '--entry' ? undefined : process.argv[3];
if (header) {
  if (bundle.code.includes(')muon_js"'))
    throw new Error(
      'Service fixture collides with its C++ raw string delimiter'
    );
  const contents = await runScriptOnceToText(
    '#pragma once\ninline constexpr const char kMuonQuickJsServiceFixture[] = R"muon_js({{source}})muon_js";\n',
    {
      sourceId: 'service-runtime-header.fc',
      variables: new Map([['source', bundle.code]]),
    }
  );
  await mkdir(dirname(resolve(header)), { recursive: true });
  await writeFile(header, contents);
}
