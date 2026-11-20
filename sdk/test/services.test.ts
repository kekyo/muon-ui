import { execFile } from 'node:child_process';
import { mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { promisify } from 'node:util';
import { afterEach, expect, it } from 'vitest';
import {
  buildPlugin,
  buildService,
  createPluginProject,
  createServiceProject,
  packagePlugin,
  packageService,
  readPluginArtifact,
  readServiceArtifact,
} from '../src/index.js';
import {
  validatePluginLibraryDependencies,
  validateServiceLibraryDependencies,
} from '../src/dependencies.js';

const execute = promisify(execFile);
const directories: string[] = [];
afterEach(async () => {
  for (const directory of directories.splice(0))
    await rm(directory, { recursive: true, force: true });
});

it.each(['c', 'c++'] as const)(
  'builds %s service factories with Make and CMake and owns separate instances',
  async (language) => {
    const root = await mkdtemp(join(tmpdir(), 'muon-service-author-'));
    directories.push(root);
    const driver = join(root, 'driver');
    await execute('gcc', [
      '-std=c99',
      '-Wall',
      '-Wextra',
      '-Werror',
      '-I' + resolve('include'),
      resolve('test/fixtures/service-driver.c'),
      '-ldl',
      '-o',
      driver,
    ]);
    for (const buildSystem of ['make', 'cmake'] as const) {
      const source = join(root, buildSystem);
      await createServiceProject({
        directory: source,
        name: 'worker',
        language,
        buildSystem,
      });
      const configPath = join(source, 'muon-plugin.build.json');
      await expect(
        buildPlugin({ configPath, target: 'linux-amd64' })
      ).rejects.toThrow(/kind/);
      const manifest = await buildService({
        configPath,
        target: 'linux-amd64',
      });
      const artifact = await readServiceArtifact(manifest, ['linux-amd64']);
      expect(artifact.manifest).toMatchObject({
        kind: 'service',
        abiVersion: 3,
      });
      expect(artifact.manifest).not.toHaveProperty('functions');
      const binary = artifact.targets['linux-amd64']!;
      expect(binary.binary.exports).toContain('muon_get_service_factory');
      expect(binary.binary.exports).not.toContain('muon_init_plugin');
      await execute(driver, [binary.library], {
        env: {
          ...process.env,
          LD_LIBRARY_PATH: resolve('native/linux-amd64/lib'),
        },
      });
      // A second build must preserve the service kind while reading previous outputs.
      await buildService({ configPath, target: 'linux-amd64' });
      await expect(
        readPluginArtifact(manifest, ['linux-amd64'])
      ).rejects.toThrow(/kind/);
      await expect(
        packagePlugin({
          outputDirectory: join(root, 'wrong-entry'),
          name: 'worker',
          functions: ['muon.worker.run'],
          targets: { 'linux-amd64': { library: binary.library, language } },
        })
      ).rejects.toThrow(/muon_init_plugin/);
      await writeFile(binary.library, 'corrupt');
      await expect(
        readServiceArtifact(manifest, ['linux-amd64'])
      ).rejects.toThrow(/SHA-256/);
    }
  }
);

it('keeps plugin entry and function requirements separate from service artifacts', async () => {
  const root = await mkdtemp(join(tmpdir(), 'muon-service-kind-'));
  directories.push(root);
  await createPluginProject({
    directory: root,
    name: 'example',
    language: 'c',
  });
  const configPath = join(root, 'muon-plugin.build.json');
  await expect(
    buildService({ configPath, target: 'linux-amd64' })
  ).rejects.toThrow(/kind/);
  const manifest = await buildPlugin({ configPath, target: 'linux-amd64' });
  const plugin = await readPluginArtifact(manifest, ['linux-amd64']);
  const serviceSource = join(root, 'service-source');
  await createServiceProject({
    directory: serviceSource,
    name: 'example',
    language: 'c',
  });
  await expect(
    buildService({
      configPath: join(serviceSource, 'muon-plugin.build.json'),
      target: 'linux-amd64',
      outputDirectory: join(root, '.muon-plugin'),
    })
  ).rejects.toThrow(/kind/);
  await expect(
    readPluginArtifact(manifest, ['linux-amd64'])
  ).resolves.toBeDefined();
  await expect(readServiceArtifact(manifest, ['linux-amd64'])).rejects.toThrow(
    /kind/
  );
  await expect(
    packageService({
      kind: 'service',
      outputDirectory: join(root, 'service'),
      name: 'example',
      targets: {
        'linux-amd64': {
          library: plugin.targets['linux-amd64']!.library,
          language: 'c',
        },
      },
    })
  ).rejects.toThrow(/muon_get_service_factory/);
  const metadata = JSON.parse(await readFile(manifest, 'utf8'));
  for (const functions of [undefined, []]) {
    await writeFile(manifest, JSON.stringify({ ...metadata, functions }));
    await expect(readPluginArtifact(manifest, ['linux-amd64'])).rejects.toThrow(
      /catalog|path arrays/
    );
  }
  await writeFile(
    manifest,
    JSON.stringify({ ...metadata, kind: 'service', abiVersion: 3 })
  );
  await expect(readServiceArtifact(manifest, ['linux-amd64'])).rejects.toThrow(
    /functions/
  );
  await writeFile(manifest, JSON.stringify({ ...metadata, kind: 'unknown' }));
  await expect(readPluginArtifact(manifest, ['linux-amd64'])).rejects.toThrow(
    /kind/
  );
});

it.each([
  ['linux-amd64', 'libmuon-ui.so'],
  ['linux-amd64', 'libcef.so'],
  ['windows-amd64', 'libmuon-ui.dll'],
  ['windows-i686', 'libcef.dll'],
  ['android-arm64-v8a', 'libmuon_android_rpc.so'],
] as const)(
  'does not assume the service host provides %s dependency %s',
  (target, dependency) => {
    const libraries = [
      {
        path: 'worker.so',
        info: {
          exports: [],
          needed: [dependency],
          soname: undefined,
          runpath: [],
        },
      },
    ];
    expect(() =>
      validatePluginLibraryDependencies(libraries, target)
    ).not.toThrow();
    expect(() => validateServiceLibraryDependencies(libraries, target)).toThrow(
      /unpackaged/
    );
  }
);

it('validates dependencies of a packaged service against its own host profile', async () => {
  const root = await mkdtemp(join(tmpdir(), 'muon-service-dependency-'));
  directories.push(root);
  await createServiceProject({
    directory: root,
    name: 'worker',
    language: 'c',
  });
  await writeFile(
    join(root, 'dependency.c'),
    'int vendor_work(void) { return 1; }\n'
  );
  const dependency = join(root, 'libmuon-ui.so');
  await execute('gcc', [
    '-shared',
    '-fPIC',
    join(root, 'dependency.c'),
    '-Wl,-soname,libmuon-ui.so',
    '-o',
    dependency,
  ]);
  const library = join(root, 'worker.so');
  await execute('gcc', [
    '-std=c99',
    '-shared',
    '-fPIC',
    '-I' + resolve('include'),
    join(root, 'service.c'),
    '-L' + root,
    '-Wl,--no-as-needed',
    '-lmuon-ui',
    '-Wl,-soname,worker.so',
    '-Wl,-rpath,$ORIGIN',
    '-o',
    library,
  ]);
  const options = {
    kind: 'service' as const,
    outputDirectory: join(root, 'packaged'),
    name: 'worker',
  };
  await expect(
    packageService({
      ...options,
      targets: { 'linux-amd64': { library, language: 'c' } },
    })
  ).rejects.toThrow(/unpackaged.*libmuon-ui/);
  const manifest = await packageService({
    ...options,
    targets: {
      'linux-amd64': { library, language: 'c', dependencies: [dependency] },
    },
  });
  const artifact = await readServiceArtifact(manifest, ['linux-amd64']);
  expect(artifact.targets['linux-amd64']!.dependencies).toHaveLength(1);
});

it('authors, builds, packages and verifies a service through the CLI and package exports', async () => {
  const root = await mkdtemp(join(tmpdir(), 'muon-service-cli-'));
  directories.push(root);
  const cli = resolve('dist/cli.mjs');
  const source = join(root, 'source');
  await execute(process.execPath, [
    cli,
    'init',
    source,
    '--name',
    'worker',
    '--language',
    'c',
    '--kind',
    'service',
  ]);
  const built = await execute(process.execPath, [
    cli,
    'build',
    join(source, 'muon-plugin.build.json'),
    '--target',
    'linux-amd64',
  ]);
  const artifact = await readServiceArtifact(built.stdout.trim(), [
    'linux-amd64',
  ]);
  const inputs = join(root, 'package-inputs.json');
  await writeFile(
    inputs,
    JSON.stringify({
      kind: 'service',
      outputDirectory: join(root, 'packaged'),
      name: 'worker',
      targets: {
        'linux-amd64': {
          library: artifact.targets['linux-amd64']!.library,
          language: 'c',
        },
      },
    })
  );
  const packaged = await execute(process.execPath, [cli, 'package', inputs], {
    env: { PATH: '' },
  });
  const manifest = packaged.stdout.trim();
  const verified = await execute(
    process.execPath,
    [cli, 'verify', manifest, '--target', 'linux-amd64'],
    { env: { PATH: '' } }
  );
  expect(verified.stdout).toContain('worker: verified linux-amd64');
  for (const extension of ['mjs', 'cjs']) {
    const script = `const sdk = await import(${JSON.stringify(resolve('dist/artifact.' + extension))}); const artifact = await sdk.readServiceArtifact(${JSON.stringify(manifest)}, ['linux-amd64']); process.stdout.write(artifact.manifest.kind);`;
    expect(
      (
        await execute(process.execPath, ['--input-type=module', '-e', script], {
          env: { PATH: '' },
        })
      ).stdout
    ).toBe('service');
  }
});
