import { execFile } from 'node:child_process';
import { mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { promisify } from 'node:util';
import { expect, it } from 'vitest';
import {
  createPluginProject,
  packagePlugin,
  readPluginArtifact,
} from '../src/index.js';
import { validatePluginLibraryDependencies } from '../src/dependencies.js';

it.each([
  ['linux-amd64', 'ld-linux-x86-64.so.2'],
  ['linux-armhf', 'ld-linux-armhf.so.3'],
  ['linux-arm64', 'ld-linux-aarch64.so.1'],
] as const)('uses the host dynamic loader for %s plugins', (target, loader) => {
  const info = {
    exports: ['muon_init_plugin'],
    needed: [loader],
    soname: undefined,
    runpath: [],
  };
  expect(() =>
    validatePluginLibraryDependencies([{ path: 'plugin.so', info }], target)
  ).not.toThrow();
  expect(() =>
    validatePluginLibraryDependencies([{ path: loader, info }], target)
  ).toThrow(/reserved/);
  expect(() =>
    validatePluginLibraryDependencies(
      [{ path: 'plugin.so', info: { ...info, needed: ['ld-foreign.so'] } }],
      target
    )
  ).toThrow(/unpackaged/);
});

it('loads an ordinary shared dependency and rejects missing or reserved runtime libraries', async () => {
  const root = await mkdtemp(join(tmpdir(), 'muon-dependencies-'));
  const execute = promisify(execFile);
  try {
    await createPluginProject({
      directory: root,
      name: 'example',
      language: 'c',
    });
    await writeFile(
      join(root, 'dependency.c'),
      'int vendor_increment(int value) { return value + 1; }\n'
    );
    const dependency = join(root, 'libvendor.so');
    await execute('gcc', [
      '-shared',
      '-fPIC',
      join(root, 'dependency.c'),
      '-Wl,-soname,libvendor.so',
      '-o',
      dependency,
    ]);
    const source = join(root, 'plugin.c');
    await writeFile(
      source,
      (await readFile(source, 'utf8'))
        .replace(
          '#include <stddef.h>',
          '#include <stddef.h>\nextern int vendor_increment(int value);'
        )
        .replace('value + 1', 'vendor_increment(value)')
    );
    const library = join(root, 'example.so');
    await execute('gcc', [
      '-shared',
      '-fPIC',
      '-I' + resolve('include'),
      source,
      '-L' + root,
      '-lvendor',
      '-Wl,-soname,example.so',
      '-Wl,-rpath,$ORIGIN',
      '-o',
      library,
    ]);
    const options = {
      outputDirectory: join(root, 'packaged'),
      name: 'example',
      functions: ['muon.example.increment'],
      targets: { 'linux-amd64': { library, language: 'c' as const } },
    };
    await expect(packagePlugin(options)).rejects.toThrow(
      /unpackaged.*libvendor/
    );
    await expect(
      readFile(join(options.outputDirectory, 'muon-plugin.json'))
    ).rejects.toMatchObject({ code: 'ENOENT' });
    const manifest = await packagePlugin({
      ...options,
      targets: {
        'linux-amd64': {
          ...options.targets['linux-amd64'],
          dependencies: [dependency],
        },
      },
    });
    const artifact = await readPluginArtifact(manifest, ['linux-amd64']);
    await writeFile(
      join(root, 'driver.c'),
      '#include "muon_plugin_api.h"\n#include <dlfcn.h>\nstatic int result;\nstatic void done(const void* value, const char* error) { result = error ? -1 : *(const int32_t*)value; }\nint main(int argc, char** argv) { (void)argc; void* h = dlopen(argv[1], RTLD_NOW); if (!h) return 10; muon_init_plugin_func init = (muon_init_plugin_func)dlsym(h, "muon_init_plugin"); const muon_plugin_metadata* p = init(0); ((void (*)(muon_completion_func, int32_t))p->namespaces[0]->functions[0]->native_func)(done, 41); dlclose(h); return result == 42 ? 0 : 12; }\n'
    );
    await execute('gcc', [
      '-std=c99',
      '-I' + resolve('include'),
      join(root, 'driver.c'),
      '-ldl',
      '-o',
      join(root, 'driver'),
    ]);
    await execute(
      join(root, 'driver'),
      [artifact.targets['linux-amd64']!.library],
      { env: { PATH: '' } }
    );
    await expect(
      packagePlugin({
        ...options,
        targets: {
          'linux-amd64': {
            library,
            language: 'c',
            dependencies: [
              dependency,
              resolve('native/linux-amd64/lib/libcardio.so'),
            ],
          },
        },
      })
    ).rejects.toThrow(/reserved/);
  } finally {
    await rm(root, { recursive: true, force: true });
  }
});
