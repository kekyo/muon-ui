// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import {
  mkdir,
  mkdtemp,
  readFile,
  rm,
  symlink,
  writeFile,
} from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { expect, it } from 'vitest';
import {
  createServiceRuntime,
  defineService,
  type ServiceFileSystem,
} from 'muon-plugin-sdk/services';
import { createNodeServiceIo } from '../src/service-io.ts';

it('restores the value saved at shutdown, confines paths, and preserves files on failed replacement', async () => {
  const root = await mkdtemp(join(tmpdir(), 'muon-service-files-'));
  const data = join(root, 'data');
  const shared = join(root, 'shared');
  try {
    await mkdir(data);
    await mkdir(shared);
    await writeFile(join(shared, 'external'), 'outside');
    await symlink(shared, join(data, 'linked'), 'dir');
    const permissions = {
      fs: {
        data: 'read-write',
        assets: { initial: Buffer.from('初期値').toString('base64') },
        directories: { shared: { path: shared, access: 'read' } },
      },
    };
    let files!: ServiceFileSystem;
    const service = defineService({
      interface: { schemaVersion: 1, methods: {}, events: {} },
      start: (context) => {
        files = context.fs;
        return {
          methods: {},
          stop: async (info) => {
            expect(context.signal.aborted).toBe(true);
            await context.fs.writeText('state', '更新😀', {
              signal: info.signal,
            });
          },
        };
      },
    });
    const launch = async (generation: string) => {
      const runtime = createServiceRuntime(service, {
        callback: async () => {
          throw new Error('Unexpected callback');
        },
        emit: () => {},
        io: createNodeServiceIo(data, permissions),
      });
      await runtime.start({
        id: 'files',
        generation,
        config: {},
        dataDirectory: data,
        startReason: 'acquire',
      });
      return runtime;
    };
    const first = await launch('1');
    expect(await files.readText('initial', { area: 'assets' })).toBe('初期値');
    expect(await files.readText('external', { area: 'shared' })).toBe(
      'outside'
    );
    await expect(
      files.writeText('initial', 'wrong', { area: 'assets' })
    ).rejects.toMatchObject({ code: 'permission-denied' });
    await expect(
      files.writeText('external', 'wrong', { area: 'shared' })
    ).rejects.toMatchObject({ code: 'permission-denied' });
    await expect(files.readText('../shared/external')).rejects.toMatchObject({
      code: 'permission-denied',
    });
    await expect(
      files.writeText('linked/external', 'wrong')
    ).rejects.toMatchObject({ code: 'permission-denied' });
    await files.mkdir('nested');
    await files.writeBytes('nested/raw', new Uint8Array([0, 128, 255]));
    await files.rename('nested/raw', 'nested/value');
    expect(await files.list('nested')).toEqual(['value']);
    const cancelled = new AbortController();
    cancelled.abort();
    await expect(
      files.writeBytes('nested/value', new Uint8Array([1]), {
        signal: cancelled.signal,
      })
    ).rejects.toMatchObject({ code: 'cancelled' });
    expect(await files.readBytes('nested/value')).toEqual(
      new Uint8Array([0, 128, 255])
    );
    expect(await readFile(join(shared, 'external'), 'utf8')).toBe('outside');
    const old = files;
    await first.stop({ reason: 'app-exit', remainingMs: 10000 });
    const second = await launch('2');
    expect(await files.readText('state')).toBe('更新😀');
    await expect(old.writeText('state', 'stale')).rejects.toMatchObject({
      code: 'cancelled',
    });
    await second.stop({ reason: 'reference-released' });
  } finally {
    await rm(root, { recursive: true, force: true });
  }
});
