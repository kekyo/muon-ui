import { execFile } from 'node:child_process';
import {
  chmod,
  mkdir,
  mkdtemp,
  readFile,
  rm,
  writeFile,
} from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { promisify } from 'node:util';
import { afterEach, expect, it } from 'vitest';
import {
  buildPlugin,
  createPluginProject,
  getNativeBuildEnvironment,
  inspectPluginBinary,
  readPluginArtifact,
} from '../src/index.js';

const execute = promisify(execFile);
const directories: string[] = [];
afterEach(async () => {
  for (const directory of directories.splice(0))
    await rm(directory, { recursive: true, force: true });
});

it('builds the generated Makefile directly with the exported SDK environment', async () => {
  const root = await mkdtemp(join(tmpdir(), 'muon-manual-make-'));
  directories.push(root);
  await createPluginProject({ directory: root, name: 'direct', language: 'c' });
  await execute('make', ['-C', root], {
    env: await getNativeBuildEnvironment({ target: 'linux-amd64' }),
  });
  const binary = await readFile(
    join(root, '.muon-plugin/linux-amd64/direct.so')
  );
  expect(inspectPluginBinary(binary, 'linux-amd64').exports).toContain(
    'muon_init_plugin'
  );
});

it.each(['c', 'c++'] as const)(
  'builds %s projects with Make and independent CMake, and invokes their ABI',
  async (language) => {
    const root = await mkdtemp(join(tmpdir(), 'muon-author-'));
    directories.push(root);
    // The same host calls both C and C++ plugins, including their stop callback.
    await writeFile(
      join(root, 'driver.cpp'),
      `#include "muon_plugin_api.h"
#include <cardio.h>
#include <dlfcn.h>
static int result = -1;
static bool stopped = false;
static void done(const void* value, const char* error) { result = error ? -1 : *(const int32_t*)value; }
static void stop_done(void*) { stopped = true; }
int main(int argc, char** argv) {
  (void)argc; cardio::dispatcher_host dispatcher;
  void* handle = dlopen(argv[1], RTLD_NOW); if (!handle) return 10;
  auto init = (muon_init_plugin_func)dlsym(handle, "muon_init_plugin"); if (!init) return 11;
  const auto* plugin = init(nullptr);
  ((void (*)(muon_completion_func, int32_t))plugin->namespaces[0]->functions[0]->native_func)(done, 41);
  dispatcher.park();
  if (plugin->stop) { plugin->stop(stop_done, nullptr); dispatcher.park(); } else { stopped = true; }
  dlclose(handle); return result == 42 && stopped ? 0 : 12;
}
`
    );
    const sdk = resolve('.');
    const pc = await execute(
      'pkg-config',
      ['--cflags', '--libs', 'muon-plugin-cardio'],
      {
        env: {
          ...process.env,
          PKG_CONFIG_PATH: join(sdk, 'native/linux-amd64/lib/pkgconfig'),
        },
      }
    );
    await execute('g++', [
      '-std=c++20',
      join(root, 'driver.cpp'),
      '-o',
      join(root, 'driver'),
      ...pc.stdout.trim().split(/\s+/),
      '-ldl',
    ]);
    for (const buildSystem of ['make', 'cmake'] as const) {
      const source = join(root, buildSystem);
      await createPluginProject({
        directory: source,
        name: 'example',
        language,
        buildSystem,
      });
      if (language === 'c' && buildSystem === 'make') {
        await writeFile(join(source, 'step.h'), '#define INCREMENT_STEP 1\n');
        const code = join(source, 'plugin.c');
        await writeFile(
          code,
          '#include "step.h"\n' +
            (await readFile(code, 'utf8')).replace(
              'value + 1',
              'value + INCREMENT_STEP'
            )
        );
      }
      const manifestPath = await buildPlugin({
        configPath: join(source, 'muon-plugin.build.json'),
        target: 'linux-amd64',
        outputDirectory: join(root, 'outputs', buildSystem),
      });
      const artifact = await readPluginArtifact(manifestPath, ['linux-amd64']);
      expect(artifact.manifest.functions).toEqual(['muon.example.increment']);
      expect(artifact.manifest.types).toEqual(['types/index.d.ts']);
      await execute(
        join(root, 'driver'),
        [artifact.targets['linux-amd64']!.library],
        {
          env: {
            ...process.env,
            LD_LIBRARY_PATH: join(sdk, 'native/linux-amd64/lib'),
          },
        }
      );
      if (language === 'c' && buildSystem === 'make') {
        await writeFile(join(source, 'step.h'), '#define INCREMENT_STEP 2\n');
        await buildPlugin({
          configPath: join(source, 'muon-plugin.build.json'),
          target: 'linux-amd64',
          outputDirectory: join(root, 'outputs', buildSystem),
        });
        await expect(
          execute(
            join(root, 'driver'),
            [artifact.targets['linux-amd64']!.library],
            {
              env: {
                ...process.env,
                LD_LIBRARY_PATH: join(sdk, 'native/linux-amd64/lib'),
              },
            }
          )
        ).rejects.toMatchObject({ code: 12 });
      }
      await writeFile(artifact.targets['linux-amd64']!.library, 'corrupt');
      await expect(
        readPluginArtifact(manifestPath, ['linux-amd64'])
      ).rejects.toThrow(/SHA-256/);
    }
  }
);

it('rejects unavailable targets and incomplete toolchains before running the build', async () => {
  const root = await mkdtemp(join(tmpdir(), 'muon-author-'));
  directories.push(root);
  await createPluginProject({ directory: root, name: 'local', language: 'c' });
  const configPath = join(root, 'muon-plugin.build.json');
  const config = JSON.parse(await readFile(configPath, 'utf8'));
  config.targets = ['linux-amd64'];
  await writeFile(configPath, JSON.stringify(config));
  await expect(
    buildPlugin({ configPath, target: 'windows-amd64' })
  ).rejects.toThrow(/does not support/);
  await expect(
    buildPlugin({
      configPath,
      target: 'linux-amd64',
      environment: { PATH: '' },
    })
  ).rejects.toThrow(/sudo apt install[\s\S]*pkg-config/);
});

it('uses a custom build with vendor sources without writing into the source tree', async () => {
  const root = await mkdtemp(join(tmpdir(), 'muon-vendor-'));
  directories.push(root);
  await createPluginProject({
    directory: root,
    name: 'vendor_example',
    language: 'c',
  });
  const external = join(root, 'upstream.c');
  await writeFile(
    external,
    'int upstream_increment(int value) { return value + 1; }\n'
  );
  const sourcePath = join(root, 'plugin.c');
  await writeFile(
    sourcePath,
    (await readFile(sourcePath, 'utf8'))
      .replace(
        '#include <stddef.h>',
        '#include <stddef.h>\nextern int upstream_increment(int value);'
      )
      .replace('value + 1', 'upstream_increment(value)')
  );
  const configPath = join(root, 'muon-plugin.build.json');
  const config = JSON.parse(await readFile(configPath, 'utf8'));
  await mkdir(join(root, 'build-tools'));
  await writeFile(
    join(root, 'build-tools/compile'),
    '#!/bin/sh\nexec gcc "$@"\n'
  );
  await chmod(join(root, 'build-tools/compile'), 0o755);
  config.build = {
    system: 'custom',
    directory: 'build-tools',
    commands: [
      {
        command: './compile',
        args: [
          '-std=c99',
          '-fPIC',
          '-shared',
          '-I{{sdk}}/include',
          '{{source}}/plugin.c',
          '{{source}}/upstream.c',
          '-Wl,-soname,vendor_example.so',
          '-o',
          '{{output}}',
        ],
      },
    ],
  };
  await writeFile(configPath, JSON.stringify(config));
  const report = await execute(process.execPath, [
    resolve('dist/cli.mjs'),
    'doctor',
    '--config',
    configPath,
    '--target',
    'linux-amd64',
    '--json',
  ]);
  expect(JSON.parse(report.stdout)).toEqual({ ready: true, issues: [] });
  const original = await readFile(external);
  const manifest = await buildPlugin({
    configPath,
    target: 'linux-amd64',
    outputDirectory: join(root, 'output'),
  });
  const artifact = await readPluginArtifact(manifest, ['linux-amd64']);
  expect(artifact.targets['linux-amd64']!.binary.exports).toContain(
    'muon_init_plugin'
  );
  expect(await readFile(external)).toEqual(original);
  await expect(readPluginArtifact(manifest, ['windows-amd64'])).rejects.toThrow(
    /does not support/
  );
});

it('provides CLI diagnostics and an explicit environment usable by custom build tools', async () => {
  const cli = resolve('dist/cli.mjs');
  const report = await execute(cli, [
    'doctor',
    '--target',
    'linux-amd64',
    '--language',
    'c',
    '--json',
  ]);
  expect(JSON.parse(report.stdout)).toEqual({ ready: true, issues: [] });
  const environment = JSON.parse(
    (await execute(process.execPath, [cli, 'env', '--target', 'windows-amd64']))
      .stdout
  );
  expect(environment.CC).toBe('x86_64-w64-mingw32-gcc');
  expect(environment.PKG_CONFIG_LIBDIR).toBe('');
  expect(environment.MUON_SDK).toBe(resolve('.'));
});
