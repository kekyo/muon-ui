import { execFile } from 'node:child_process';
import { mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { promisify } from 'node:util';
import { expect, it } from 'vitest';

it('authors and packs a plugin using only an installed SDK tarball outside the repository', async () => {
  const execute = promisify(execFile);
  const root = await mkdtemp(join(tmpdir(), 'muon-installed-sdk-'));
  try {
    const packed = JSON.parse(
      (
        await execute('npm', ['pack', '--json', '--pack-destination', root], {
          cwd: resolve('.'),
        })
      ).stdout
    );
    const application = join(root, 'application');
    await mkdir(application);
    await writeFile(
      join(application, 'package.json'),
      JSON.stringify({ name: 'independent-author', private: true })
    );
    await execute(
      'npm',
      [
        'install',
        '--ignore-scripts',
        '--no-audit',
        '--no-fund',
        join(root, packed[0].filename),
      ],
      { cwd: application }
    );
    const cli = join(application, 'node_modules/muon-plugin-sdk/dist/cli.mjs');
    await execute(
      process.execPath,
      [cli, 'init', 'plugin', '--name', 'independent'],
      { cwd: application }
    );
    await execute(
      process.execPath,
      [
        cli,
        'build',
        'plugin/muon-plugin.build.json',
        '--target',
        'linux-amd64',
        '--out',
        'package',
      ],
      { cwd: application }
    );
    const directory = join(application, 'package');
    await writeFile(
      join(directory, 'package.json'),
      JSON.stringify({
        name: 'independent-plugin',
        version: '1.0.0',
        types: './types/index.d.ts',
        exports: { './muon-plugin.json': './muon-plugin.json' },
        files: ['muon-plugin.json', 'native', 'types'],
      })
    );
    const plugin = JSON.parse(
      (
        await execute('npm', ['pack', '--json', '--pack-destination', root], {
          cwd: directory,
        })
      ).stdout
    );
    const consumer = join(root, 'consumer');
    await mkdir(consumer);
    await writeFile(
      join(consumer, 'package.json'),
      JSON.stringify({ name: 'independent-consumer', private: true })
    );
    await execute(
      'npm',
      [
        'install',
        '--ignore-scripts',
        '--no-audit',
        '--no-fund',
        join(root, plugin[0].filename),
      ],
      { cwd: consumer }
    );
    const manifest = join(
      consumer,
      'node_modules/independent-plugin/muon-plugin.json'
    );
    const verified = await execute(
      process.execPath,
      [cli, 'verify', manifest, '--target', 'linux-amd64'],
      { env: { PATH: '' } }
    );
    expect(verified.stdout).toContain('independent: verified linux-amd64');
    const dependencies = JSON.parse(
      await readFile(join(consumer, 'package.json'), 'utf8')
    ).dependencies;
    expect(Object.keys(dependencies)).toEqual(['independent-plugin']);
  } finally {
    await rm(root, { recursive: true, force: true });
  }
});
