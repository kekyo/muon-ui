import { mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { afterEach, expect, it } from 'vitest';
import { resolveAndroidPlugins } from '../src/plugins.js';

const roots: string[] = [];
afterEach(async () => {
  await Promise.all(
    roots
      .splice(0)
      .map(async (root) => await rm(root, { recursive: true, force: true }))
  );
});
it.each(['internal', 'name.with.dots', 'name-with-dashes'])(
  'rejects names the runtime cannot register: %s',
  async (name) => {
    await expect(
      resolveAndroidPlugins(
        [{ name, soname: 'libexample.so', allow: ['*'], libraries: {} }],
        ['arm64-v8a'],
        '/tmp'
      )
    ).rejects.toThrow(/plugin name/);
  }
);
it('requires an explicit function policy and all selected ABI libraries', async () => {
  await expect(
    resolveAndroidPlugins(
      [{ name: 'example', soname: 'libexample.so', allow: [], libraries: {} }],
      ['arm64-v8a'],
      '/tmp'
    )
  ).rejects.toThrow(/allow/);
  await expect(
    resolveAndroidPlugins(
      [
        {
          name: 'example',
          soname: 'libexample.so',
          allow: ['*'],
          libraries: {},
        },
      ],
      ['arm64-v8a'],
      '/tmp'
    )
  ).rejects.toThrow(/arm64-v8a/);
});
it('rejects a library that cannot be loaded as an Android ELF shared library', async () => {
  const root = await mkdtemp(join(tmpdir(), 'muon-invalid-plugin-'));
  roots.push(root);
  await writeFile(join(root, 'libexample.so'), 'not an ELF');
  await expect(
    resolveAndroidPlugins(
      [
        {
          name: 'example',
          soname: 'libexample.so',
          allow: ['*'],
          libraries: { 'arm64-v8a': 'libexample.so' },
        },
      ],
      ['arm64-v8a'],
      root
    )
  ).rejects.toThrow(/ELF/);
});
it('prevents replacement of Muon runtime libraries', async () => {
  await expect(
    resolveAndroidPlugins(
      [
        {
          name: 'example',
          soname: 'libcardio.so',
          allow: ['*'],
          libraries: { 'arm64-v8a': 'libcardio.so' },
        },
      ],
      ['arm64-v8a'],
      '/tmp'
    )
  ).rejects.toThrow(/reserved/);
});
