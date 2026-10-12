// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { describe, expect, it } from 'vitest';

import {
  createMuonAndroidJavaScriptRuntimeClient,
  type MuonAndroidJavaScriptRuntimeMessageHandler,
  type MuonAndroidJavaScriptRuntimeTransport,
} from '../src/android-javascript-runtime.js';

interface TestTransport {
  readonly transport: MuonAndroidJavaScriptRuntimeTransport;
  readonly sent: string[];
  readonly receive: (
    message: Readonly<Record<string, unknown>>
  ) => Promise<void>;
}

const createTestTransport = (): TestTransport => {
  const sent: string[] = [];
  let handler: MuonAndroidJavaScriptRuntimeMessageHandler | undefined;
  return {
    transport: {
      send: (message) => {
        sent.push(message);
      },
      setMessageHandler: (value) => {
        handler = value;
      },
    },
    sent,
    receive: async (message) => {
      if (handler === undefined) {
        throw new Error(
          'The Android JavaScript runtime handler is unavailable'
        );
      }
      await handler(JSON.stringify(message));
    },
  };
};

const parseSent = (
  testTransport: TestTransport,
  index: number
): Readonly<Record<string, unknown>> =>
  JSON.parse(testTransport.sent[index] ?? '') as Readonly<
    Record<string, unknown>
  >;

describe('muon Android JavaScript runtime client', () => {
  it('creates independent runtimes and callable module facades', async () => {
    const testTransport = createTestTransport();
    const client = createMuonAndroidJavaScriptRuntimeClient(
      testTransport.transport
    );

    const firstPromise = client.createNode();
    const secondPromise = client.createNode();
    expect(parseSent(testTransport, 0)).toEqual({
      version: 1,
      type: 'create',
      requestId: 1,
    });
    expect(parseSent(testTransport, 1)).toEqual({
      version: 1,
      type: 'create',
      requestId: 2,
    });

    await testTransport.receive({
      version: 1,
      type: 'created',
      requestId: 1,
      runtimeId: 'runtime-a',
      engine: { name: 'quickjs', version: '2026-06-04' },
    });
    await testTransport.receive({
      version: 1,
      type: 'created',
      requestId: 2,
      runtimeId: 'runtime-b',
      engine: { name: 'quickjs', version: '2026-06-04' },
    });
    const first = await firstPromise;
    const second = await secondPromise;
    expect(first).not.toBe(second);

    const importedPromise = first.importModule('.');
    expect(parseSent(testTransport, 2)).toEqual({
      version: 1,
      type: 'runtimeMessage',
      runtimeId: 'runtime-a',
      message: {
        kind: 'request',
        id: 'request-1',
        command: 'importModule',
        params: { specifier: '.' },
      },
    });
    await testTransport.receive({
      version: 1,
      type: 'runtimeMessage',
      runtimeId: 'runtime-a',
      message: {
        kind: 'response',
        id: 'request-1',
        ok: true,
        value: {
          moduleId: 'module-1',
          descriptor: {
            exports: [
              { name: 'answer', kind: 'primitive', value: 42 },
              { name: 'echo', kind: 'function' },
            ],
          },
        },
      },
    });
    const imported = await importedPromise;
    expect(imported.answer).toBe(42);
    expect(Object.keys(imported)).toEqual(['answer', 'echo']);

    const echoedPromise = imported.echo?.('value');
    expect(parseSent(testTransport, 3)).toEqual({
      version: 1,
      type: 'runtimeMessage',
      runtimeId: 'runtime-a',
      message: {
        kind: 'request',
        id: 'request-2',
        command: 'call',
        params: {
          moduleId: 'module-1',
          exportName: 'echo',
          arguments: ['value'],
        },
      },
    });
    await testTransport.receive({
      version: 1,
      type: 'runtimeMessage',
      runtimeId: 'runtime-a',
      message: {
        kind: 'response',
        id: 'request-2',
        ok: true,
        value: 'value',
      },
    });
    await expect(echoedPromise).resolves.toBe('value');

    const firstRelease = first.release();
    expect(parseSent(testTransport, 4)).toMatchObject({
      runtimeId: 'runtime-a',
      message: { id: 'request-3', command: 'shutdown' },
    });
    await testTransport.receive({
      version: 1,
      type: 'runtimeMessage',
      runtimeId: 'runtime-a',
      message: {
        kind: 'response',
        id: 'request-3',
        ok: true,
        value: { shutdown: true },
      },
    });
    await firstRelease;
    const secondRelease = second.release();
    await testTransport.receive({
      version: 1,
      type: 'runtimeMessage',
      runtimeId: 'runtime-b',
      message: {
        kind: 'response',
        id: 'request-4',
        ok: true,
        value: { shutdown: true },
      },
    });
    await secondRelease;
    client.dispose();
  });

  it('round-trips bridge values and asynchronous callbacks', async () => {
    const testTransport = createTestTransport();
    const client = createMuonAndroidJavaScriptRuntimeClient(
      testTransport.transport
    );
    const nodePromise = client.createNode();
    await testTransport.receive({
      version: 1,
      type: 'created',
      requestId: 1,
      runtimeId: 'runtime-values',
      engine: { name: 'quickjs', version: '2026-06-04' },
    });
    const node = await nodePromise;
    const modulePromise = node.importModule('.');
    await testTransport.receive({
      version: 1,
      type: 'runtimeMessage',
      runtimeId: 'runtime-values',
      message: {
        kind: 'response',
        id: 'request-1',
        ok: true,
        value: {
          moduleId: 'module-values',
          descriptor: {
            exports: [{ name: 'invoke', kind: 'function' }],
          },
        },
      },
    });
    const module = await modulePromise;
    const source = Uint8Array.from([99, 3, 1, 4, 88]);
    const resultPromise = module.invoke?.(
      0x7fff_ffff_ffff_ffffn,
      source.subarray(1, 4),
      async (value: unknown) => ({ received: String(value) })
    );
    const call = parseSent(testTransport, 2);
    expect(call).toMatchObject({
      runtimeId: 'runtime-values',
      message: {
        command: 'call',
        params: {
          arguments: [
            { kind: 'i64', value: '9223372036854775807' },
            { kind: 'buffer', data: 'AwEE' },
            { kind: 'function', handle: 'callback-1' },
          ],
        },
      },
    });

    await testTransport.receive({
      version: 1,
      type: 'runtimeMessage',
      runtimeId: 'runtime-values',
      message: {
        kind: 'callback',
        id: 'remote-callback-1',
        handle: 'callback-1',
        arguments: [{ kind: 'u64', value: '18446744073709551615' }],
      },
    });
    expect(parseSent(testTransport, 3)).toEqual({
      version: 1,
      type: 'runtimeMessage',
      runtimeId: 'runtime-values',
      message: {
        kind: 'callbackResult',
        id: 'remote-callback-1',
        ok: true,
        value: { kind: 'json', value: { received: '18446744073709551615' } },
        error: null,
      },
    });

    await testTransport.receive({
      version: 1,
      type: 'runtimeMessage',
      runtimeId: 'runtime-values',
      message: {
        kind: 'response',
        id: 'request-2',
        ok: true,
        value: { kind: 'buffer', data: 'BQkC' },
      },
    });
    const result = await resultPromise;
    expect(result).toBeInstanceOf(Uint8Array);
    expect(Array.from(result as Uint8Array)).toEqual([5, 9, 2]);
  });

  it('releases module handles and rejects work after runtime disconnect', async () => {
    const testTransport = createTestTransport();
    const client = createMuonAndroidJavaScriptRuntimeClient(
      testTransport.transport
    );
    const nodePromise = client.createNode();
    await testTransport.receive({
      version: 1,
      type: 'created',
      requestId: 1,
      runtimeId: 'runtime-release',
      engine: { name: 'quickjs', version: '2026-06-04' },
    });
    const node = await nodePromise;
    const modulePromise = node.importModule('.');
    await testTransport.receive({
      version: 1,
      type: 'runtimeMessage',
      runtimeId: 'runtime-release',
      message: {
        kind: 'response',
        id: 'request-1',
        ok: true,
        value: {
          moduleId: 'module-release',
          descriptor: {
            exports: [{ name: 'pending', kind: 'function' }],
          },
        },
      },
    });
    const module = await modulePromise;

    const releasePromise = module.$release();
    await testTransport.receive({
      version: 1,
      type: 'runtimeMessage',
      runtimeId: 'runtime-release',
      message: {
        kind: 'response',
        id: 'request-2',
        ok: true,
        value: { released: true },
      },
    });
    await expect(releasePromise).resolves.toBeUndefined();
    await expect(module.pending?.()).rejects.toThrow(
      'JavaScript module handle has been released'
    );

    const secondModulePromise = node.importModule('.');
    await testTransport.receive({
      version: 1,
      type: 'runtimeMessage',
      runtimeId: 'runtime-release',
      message: {
        kind: 'response',
        id: 'request-3',
        ok: true,
        value: {
          moduleId: 'module-pending',
          descriptor: {
            exports: [{ name: 'pending', kind: 'function' }],
          },
        },
      },
    });
    const secondModule = await secondModulePromise;
    const pending = secondModule.pending?.();
    await testTransport.receive({
      version: 1,
      type: 'runtimeClosed',
      runtimeId: 'runtime-release',
      error: 'QuickJS Service process disconnected',
    });
    await expect(pending).rejects.toThrow(
      'QuickJS Service process disconnected'
    );
    await expect(node.importModule('.')).rejects.toThrow(
      'QuickJS Service process disconnected'
    );
  });
});
