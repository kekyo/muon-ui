// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { createServer } from 'node:http';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import type { Duplex } from 'node:stream';
import { build } from 'vite';
import { expect, it } from 'vitest';

// This driver owns the existing sidecar transport independently of a mock UI
// process. It proves the lifetime/SDK possibilities and the shutdown gap; it
// does not claim that the product has a Node service execution adapter.
it.skipIf(process.platform === 'win32').each([true, false])(
  'probes host-owned Node lifetime with explicit service cleanup=%s',
  async (awaitServiceCleanup) => {
    const directory = await mkdtemp(join(tmpdir(), 'muon-node-service-probe-'));
    const marker = join(directory, 'stopped.txt');
    await writeFile(
      join(directory, 'package.json'),
      JSON.stringify({ type: 'module' })
    );
    await build({
      configFile: false,
      logLevel: 'silent',
      build: {
        outDir: directory,
        emptyOutDir: false,
        lib: {
          entry: resolve('test/fixtures/service-probe.ts'),
          formats: ['es'],
          fileName: () => 'service.mjs',
        },
        rolldownOptions: { external: [/^node:/] },
        minify: false,
      },
    });
    const server = createServer();
    server.listen(0, '127.0.0.1');
    await once(server, 'listening');
    const address = server.address();
    if (!address || typeof address === 'string')
      throw new Error('Missing probe address');
    const origin = `http://127.0.0.1:${address.port}`;
    const environment: NodeJS.ProcessEnv = {
      ...process.env,
      MUON_NODE_FD: '3',
      MUON_NODE_TOKEN: 'service-probe',
    };
    delete environment.MUON_NODE_PIPE;
    const child = spawn(process.execPath, [resolve('dist/node-bridge.mjs')], {
      env: environment,
      stdio: ['ignore', 'ignore', 'pipe', 'pipe'],
    });
    const exited = once(child, 'exit');
    const transport = child.stdio[3] as Duplex;
    let diagnostics = '';
    child.stderr!.on('data', (chunk) => {
      diagnostics += String(chunk);
    });
    let buffered = Buffer.alloc(0);
    let sequence = 0;
    const pending = new Map<
      string,
      { resolve(value: unknown): void; reject(error: Error): void }
    >();
    transport.on('data', (chunk: Buffer) => {
      buffered = Buffer.concat([buffered, chunk]);
      while (
        buffered.length >= 4 &&
        buffered.length >= buffered.readUInt32BE(0) + 4
      ) {
        const length = buffered.readUInt32BE(0);
        const message = JSON.parse(buffered.subarray(4, length + 4).toString());
        buffered = buffered.subarray(length + 4);
        const waiter = pending.get(message.id);
        if (!waiter) continue;
        pending.delete(message.id);
        if (message.ok) waiter.resolve(message.value);
        else waiter.reject(new Error(message.error.message));
      }
    });
    transport.on('close', () => {
      for (const waiter of pending.values())
        waiter.reject(new Error('Sidecar closed before service completion'));
      pending.clear();
    });
    const request = async (
      command: string,
      params: Record<string, unknown>
    ): Promise<unknown> => {
      const id = String(++sequence);
      const response = new Promise((resolve, reject) => {
        pending.set(id, { resolve, reject });
      });
      const body = Buffer.from(
        JSON.stringify({ kind: 'request', id, command, params })
      );
      const header = Buffer.alloc(4);
      header.writeUInt32BE(body.length);
      transport.write(Buffer.concat([header, body]));
      return await response;
    };
    const ui = spawn(process.execPath, ['-e', 'process.stdin.resume()'], {
      stdio: ['pipe', 'ignore', 'ignore'],
    });
    const uiExited = once(ui, 'exit');
    try {
      await request('initialize', {
        protocol: 'node/1',
        projectRoot: directory,
        token: 'service-probe',
      });
      const imported = (await request('importModule', {
        specifier: './service.mjs',
      })) as { moduleId: string };
      const call = async (exportName: string, args: unknown[]) =>
        await request('call', {
          moduleId: imported.moduleId,
          exportName,
          arguments: args.map((value) =>
            value !== null && typeof value === 'object'
              ? { kind: 'json', value }
              : value
          ),
        });
      await call('start', [{ stopUrl: origin + '/stop', marker }]);
      const entered = once(server, 'request');
      const work = call('invoke', ['1', 'work', [origin + '/work']]);
      const [incoming, response] = await Promise.race([
        entered,
        (async () => {
          await work;
          throw new Error('Work settled before the observer barrier');
        })(),
      ]);
      expect(incoming.url).toBe('/work');
      ui.stdin!.end();
      expect((await uiExited)[0]).toBe(0);
      // The independent observer releases work only after the UI has exited.
      response.end('completed after UI exit');
      expect(await work).toEqual({
        kind: 'json',
        value: { status: 'ok', value: 'completed after UI exit' },
      });
      expect(await call('events', [])).toEqual({
        kind: 'json',
        value: [{ event: 'progress', value: 'completed after UI exit' }],
      });
      const stopping = once(server, 'request');
      const cleanup = (async () => {
        try {
          return await call('stop', []);
        } catch (error) {
          return error;
        }
      })();
      const [stopRequest, stopResponse] = await stopping;
      expect(stopRequest.url).toBe('/stop');
      if (awaitServiceCleanup) {
        stopResponse.end('allow cleanup');
        await cleanup;
        expect(await readFile(marker, 'utf8')).toBe('service stopped');
      }
      await request('shutdown', {});
      expect((await exited)[0], diagnostics).toBe(0);
      if (!awaitServiceCleanup) {
        expect(await cleanup).toBeInstanceOf(Error);
        await expect(readFile(marker)).rejects.toMatchObject({
          code: 'ENOENT',
        });
        stopResponse.destroy();
      }
    } finally {
      if (ui.exitCode === null && ui.signalCode === null) {
        ui.kill();
        await uiExited;
      }
      if (child.exitCode === null && child.signalCode === null) {
        child.kill();
        await exited;
      }
      transport.destroy();
      server.closeAllConnections();
      await new Promise<void>((resolve) => server.close(() => resolve()));
      await rm(directory, { recursive: true, force: true });
    }
  }
);
