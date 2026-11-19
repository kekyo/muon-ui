// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { spawn } from 'node:child_process';
import { EventEmitter, once } from 'node:events';
import { mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { createServer } from 'node:http';
import { join, resolve } from 'node:path';
import type { Duplex } from 'node:stream';
import { build } from 'vite';
import { expect, it } from 'vitest';

it.skipIf(process.platform === 'win32').each(['stop', 'disconnect'] as const)(
  'runs the service SDK in a child and awaits asynchronous cleanup after %s',
  async (termination) => {
    await mkdir('../.build', { recursive: true });
    const root = await mkdtemp(resolve('../.build/node-service-'));
    const marker = join(root, 'stopped');
    const observer = createServer();
    observer.listen(0, '127.0.0.1');
    await once(observer, 'listening');
    const address = observer.address();
    if (!address || typeof address === 'string')
      throw new Error('Missing observer port');
    await writeFile(
      join(root, 'package.json'),
      JSON.stringify({ type: 'module' })
    );
    await build({
      configFile: false,
      logLevel: 'silent',
      build: {
        outDir: root,
        emptyOutDir: false,
        minify: false,
        lib: {
          entry: resolve('test/fixtures/node-service.ts'),
          formats: ['es'],
          fileName: () => 'service.mjs',
        },
        rolldownOptions: { external: [/^node:/] },
      },
    });
    const environment: NodeJS.ProcessEnv = {
      ...process.env,
      MUON_NODE_FD: '3',
      MUON_NODE_TOKEN: 'service-token',
    };
    delete environment.MUON_NODE_PIPE;
    const child = spawn(
      process.execPath,
      [resolve('dist/node-bridge.mjs'), '--service'],
      {
        env: environment,
        stdio: ['ignore', 'ignore', 'pipe', 'pipe'],
      }
    );
    const exited = once(child, 'close');
    const socket = child.stdio[3] as Duplex;
    const notifications = new EventEmitter();
    let diagnostic = '';
    child.stderr!.on('data', (bytes: Buffer) => {
      diagnostic += bytes.toString();
    });
    let buffered = Buffer.alloc(0);
    let sequence = 0;
    const pending = new Map<
      string,
      { resolve: (value: any) => void; reject: (error: Error) => void }
    >();
    socket.on('data', (chunk: Buffer) => {
      buffered = Buffer.concat([buffered, chunk]);
      while (
        buffered.length >= 4 &&
        buffered.length >= buffered.readUInt32BE(0) + 4
      ) {
        const length = buffered.readUInt32BE(0);
        const message = JSON.parse(buffered.subarray(4, length + 4).toString());
        buffered = buffered.subarray(length + 4);
        if (message.kind === 'response') {
          pending.get(message.id)?.resolve(message);
          pending.delete(message.id);
        } else notifications.emit(message.kind, message);
      }
    });
    socket.on('close', () => {
      for (const job of pending.values())
        job.reject(new Error('Service disconnected: ' + diagnostic));
      pending.clear();
    });
    const send = (message: unknown) => {
      const payload = Buffer.from(JSON.stringify(message));
      const header = Buffer.alloc(4);
      header.writeUInt32BE(payload.length);
      socket.write(Buffer.concat([header, payload]));
    };
    const request = async (
      command: string,
      params: Record<string, unknown>
    ) => {
      const id = String(++sequence);
      const completion = new Promise<any>((resolve, reject) => {
        pending.set(id, { resolve, reject });
      });
      send({ kind: 'request', id, command, params });
      return await completion;
    };
    try {
      const initialization = {
        protocol: 'node-service/1',
        projectRoot: root,
        entry: './service.mjs',
        id: 'worker',
        generation: '9',
        startReason: 'acquire',
        dataDirectory: root,
        config: { origin: `http://127.0.0.1:${address.port}`, marker },
      };
      expect(
        await request('initialize', { ...initialization, token: 'wrong' })
      ).toMatchObject({ ok: false, error: { code: 'ERR_MUON_NODE_AUTH' } });
      const entered = once(observer, 'request');
      const initialized = await request('initialize', {
        ...initialization,
        token: 'service-token',
      });
      expect(initialized, diagnostic).toMatchObject({
        ok: true,
        value: {
          protocol: 'node-service/1',
          interface: { methods: { status: { returns: 'string' } } },
        },
      });
      const [incoming, workResponse] = await entered;
      expect(incoming.url).toBe('/work');
      expect(
        await request('invoke', {
          callId: '1',
          method: 'integer',
          arguments: ['-9223372036854775808'],
        })
      ).toMatchObject({
        ok: true,
        value: { status: 'ok', value: '-9223372036854775808' },
      });
      expect(
        await request('invoke', {
          callId: '2',
          method: 'bytes',
          arguments: ['AAH/'],
        })
      ).toMatchObject({ ok: true, value: { status: 'ok', value: 'AAH/' } });
      const callback = once(notifications, 'callback');
      const called = request('invoke', {
        callId: '3',
        method: 'callback',
        arguments: ['input', '17'],
      });
      const [invocation] = await callback;
      expect(invocation).toMatchObject({
        callId: '3',
        token: '17',
        arguments: ['input'],
      });
      send({
        kind: 'callbackResult',
        id: invocation.id,
        result: { status: 'ok', value: 'callback reply' },
      });
      expect(await called).toMatchObject({
        ok: true,
        value: { status: 'ok', value: 'callback reply' },
      });
      const failedCallback = once(notifications, 'callback');
      const failureReply = request('invoke', {
        callId: '8',
        method: 'callback',
        arguments: ['failure', '19'],
      });
      const [failed] = await failedCallback;
      send({
        kind: 'callbackResult',
        id: failed.id,
        result: { status: 'error', code: 'callback-failed', message: '' },
      });
      expect(await failureReply).toMatchObject({
        ok: true,
        value: { status: 'error', code: 'callback-failed', message: '' },
      });
      const waiting = once(notifications, 'event');
      const canceled = request('invoke', {
        callId: '4',
        method: 'wait',
        arguments: [],
      });
      expect((await waiting)[0]).toMatchObject({
        event: 'progress',
        value: 'waiting',
        generation: '9',
      });
      expect(await request('cancel', { callId: '4' })).toMatchObject({
        ok: true,
      });
      expect(await canceled).toMatchObject({
        ok: true,
        value: { status: 'error', code: 'cancelled' },
      });
      const lateCallback = once(notifications, 'callback');
      const canceledCallback = request('invoke', {
        callId: '7',
        method: 'callback',
        arguments: ['cancel me', '18'],
      });
      const [late] = await lateCallback;
      await request('cancel', { callId: '7' });
      expect(await canceledCallback).toMatchObject({
        ok: true,
        value: { status: 'error', code: 'cancelled' },
      });
      send({
        kind: 'callbackResult',
        id: late.id,
        result: { status: 'ok', value: 'too late' },
      });
      const ui = spawn(process.execPath, ['-e', 'process.stdin.resume()'], {
        stdio: ['pipe', 'ignore', 'ignore'],
      });
      const uiExited = once(ui, 'close');
      ui.stdin!.end();
      expect((await uiExited)[0]).toBe(0);
      const progress = once(notifications, 'event');
      // Only the independent observer can complete work after the UI has exited.
      workResponse.end('finished after UI exit');
      expect((await progress)[0]).toMatchObject({
        event: 'progress',
        value: 'finished after UI exit',
        generation: '9',
      });
      expect(
        await request('invoke', {
          callId: '5',
          method: 'status',
          arguments: [],
        })
      ).toMatchObject({
        ok: true,
        value: { status: 'ok', value: 'finished after UI exit' },
      });
      const stopping = once(observer, 'request');
      const stopped =
        termination === 'stop'
          ? request('stop', { reason: 'app-exit' })
          : undefined;
      if (termination === 'disconnect') socket.end();
      const [stopRequest, stopResponse] = await stopping;
      expect(stopRequest.url).toBe('/stop');
      if (termination === 'stop')
        expect(
          await request('invoke', {
            callId: '6',
            method: 'status',
            arguments: [],
          })
        ).toMatchObject({ ok: false });
      stopResponse.end('allow cleanup');
      if (stopped)
        expect(await stopped).toMatchObject({
          ok: true,
          value: { stopped: true },
        });
      expect(await exited, diagnostic).toEqual([
        termination === 'stop' ? 0 : 1,
        null,
      ]);
      if (termination === 'disconnect')
        expect(diagnostic).toContain('Service host disconnected');
      expect(JSON.parse(await readFile(marker, 'utf8'))).toEqual([
        '9',
        'acquire',
        termination === 'stop' ? 'app-exit' : 'host-stop',
      ]);
    } finally {
      socket.destroy();
      if (child.exitCode === null && child.signalCode === null) {
        child.kill('SIGKILL');
        await exited;
      }
      observer.closeAllConnections();
      const closed = once(observer, 'close');
      observer.close();
      await closed;
      await rm(root, { recursive: true, force: true });
    }
  }
);
