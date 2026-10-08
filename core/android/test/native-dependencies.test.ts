// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile } from 'node:child_process';
import { createHash } from 'node:crypto';
import { cp, mkdir, mkdtemp, readFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { promisify } from 'node:util';
import { expect, it } from 'vitest';

const execute = promisify(execFile);

it('builds native dependencies from shallow checkouts without release tags', async () => {
  const repository = resolve('../..');
  const fixture = await mkdtemp(join(tmpdir(), 'muon-android-shallow-'));
  const project = join(fixture, 'core/android');
  const output = join(project, '.native-dependencies');
  try {
    await mkdir(join(project, 'scripts'), { recursive: true });
    await cp(
      resolve('scripts/build-native-dependencies.mjs'),
      join(project, 'scripts/build-native-dependencies.mjs')
    );
    await cp(resolve('patches'), join(project, 'patches'), {
      recursive: true,
    });
    for (const dependency of ['deps/cardio', 'deps/tra-ffic/deps/libffi']) {
      const destination = join(fixture, dependency);
      await execute('git', [
        'clone',
        '--depth=1',
        '--no-tags',
        pathToFileURL(join(repository, dependency)).href,
        destination,
      ]);
      expect(
        (await execute('git', ['-C', destination, 'tag'])).stdout.trim()
      ).toBe('');
      expect(
        (
          await execute('git', [
            '-C',
            destination,
            'rev-parse',
            '--is-shallow-repository',
          ])
        ).stdout.trim()
      ).toBe('true');
    }

    const { stdout } = await execute(
      process.execPath,
      [join(project, 'scripts/build-native-dependencies.mjs')],
      {
        cwd: fixture,
        env: { ...process.env, MUON_ANDROID_DEPENDENCY_ROOT: output },
        maxBuffer: 16 * 1024 * 1024,
      }
    );
    expect(stdout).toContain('Android native dependency build: PASS');
    const commit = (
      await execute('git', [
        '-C',
        join(fixture, 'deps/tra-ffic/deps/libffi'),
        'rev-parse',
        'HEAD',
      ])
    ).stdout.trim();
    for (const abi of ['arm64-v8a', 'x86_64']) {
      const directory = join(output, abi);
      const manifest = JSON.parse(
        await readFile(join(directory, 'manifest.json'), 'utf8')
      );
      const archive = await readFile(join(directory, 'install/lib/libffi.a'));
      expect(manifest.abi).toBe(abi);
      expect(manifest.libffi.commit).toBe(commit);
      expect(archive.subarray(0, 8).toString()).toBe('!<arch>\n');
      expect(manifest.libffi.archiveSha256).toBe(
        createHash('sha256').update(archive).digest('hex')
      );
    }
  } finally {
    await rm(fixture, { recursive: true, force: true });
  }
}, 300000);
