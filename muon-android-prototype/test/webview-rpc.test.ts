// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { describe, expect, it } from 'vitest';

import {
  createMuonWebViewRpcClient,
  decodeMuonWebViewRpcBinaryFrame,
  encodeMuonWebViewRpcBinaryFrame,
  installMuonWebViewCapabilityBridge,
  type MuonWebViewRpcTransport,
} from '../src/webview-rpc.js';

describe('muon WebView RPC client', () => {
  it('matches successful and failed native results to their promises', async () => {
    const sent: Array<string | ArrayBuffer> = [];
    const receiver: {
      current: ((message: string | ArrayBuffer) => void) | undefined;
    } = { current: undefined };
    const transport: MuonWebViewRpcTransport = {
      send: (message) => sent.push(message),
      setMessageHandler: (handler) => {
        receiver.current = handler;
      },
    };
    const client = createMuonWebViewRpcClient(transport);

    const success = client.call(
      'environment-capability',
      'muon.environments.getConfigValues',
      []
    );
    expect(JSON.parse(sent[0] as string)).toEqual({
      version: 1,
      type: 'call',
      callId: 1,
      capabilityId: 'environment-capability',
      functionPath: 'muon.environments.getConfigValues',
      arguments: [],
    });
    receiver.current?.(
      JSON.stringify({
        version: 1,
        type: 'result',
        callId: 1,
        success: true,
        value: '{"channel":"android"}',
      })
    );
    await expect(success).resolves.toBe('{"channel":"android"}');

    const failure = client.call('', 'prototype.fail', []);
    receiver.current?.(
      JSON.stringify({
        version: 1,
        type: 'result',
        callId: 2,
        success: false,
        error: 'prototype failure',
      })
    );
    await expect(failure).rejects.toThrow('prototype failure');
    expect(client.getPendingCallCount()).toBe(0);
  });

  it('cancels an aborted call once and ignores its late result', async () => {
    const sent: Array<string | ArrayBuffer> = [];
    const receiver: {
      current: ((message: string | ArrayBuffer) => void) | undefined;
    } = { current: undefined };
    const transport: MuonWebViewRpcTransport = {
      send: (message) => sent.push(message),
      setMessageHandler: (handler) => {
        receiver.current = handler;
      },
    };
    const client = createMuonWebViewRpcClient(transport);
    const controller = new AbortController();

    const result = client.call('', 'prototype.delay', [], {
      signal: controller.signal,
    });
    controller.abort();
    controller.abort();

    await expect(result).rejects.toMatchObject({ name: 'AbortError' });
    expect(JSON.parse(sent[1] as string)).toEqual({
      version: 1,
      type: 'cancel',
      callId: 1,
    });
    expect(sent).toHaveLength(2);
    expect(client.getPendingCallCount()).toBe(0);
    expect(() =>
      receiver.current?.(
        JSON.stringify({
          version: 1,
          type: 'result',
          callId: 1,
          success: true,
          value: 'late',
        })
      )
    ).not.toThrow();
  });

  it('does not send a call when its signal is already aborted', async () => {
    const sent: Array<string | ArrayBuffer> = [];
    const transport: MuonWebViewRpcTransport = {
      send: (message) => sent.push(message),
      setMessageHandler: () => {},
    };
    const client = createMuonWebViewRpcClient(transport);
    const controller = new AbortController();
    controller.abort();

    await expect(
      client.call('', 'prototype.delay', [], { signal: controller.signal })
    ).rejects.toMatchObject({ name: 'AbortError' });
    expect(sent).toEqual([]);
  });

  it('transfers typed-array slices and binary results without JSON encoding', async () => {
    const sent: Array<string | ArrayBuffer> = [];
    const receiver: {
      current: ((message: string | ArrayBuffer) => void) | undefined;
    } = { current: undefined };
    const transport: MuonWebViewRpcTransport = {
      send: (message) => sent.push(message),
      setMessageHandler: (handler) => {
        receiver.current = handler;
      },
    };
    const client = createMuonWebViewRpcClient(transport);
    const source = Uint8Array.from([99, 3, 1, 4, 88]);

    const result = client.call('', 'prototype.echoBinary', [
      source.subarray(1, 4),
    ]);
    expect(JSON.parse(sent[0] as string)).toMatchObject({
      version: 1,
      type: 'call',
      callId: 1,
      arguments: [{ type: 'binary', attachment: 0, byteLength: 3 }],
    });
    const argumentFrame = decodeMuonWebViewRpcBinaryFrame(
      sent[1] as ArrayBuffer
    );
    expect(argumentFrame).toMatchObject({
      kind: 'argument',
      callId: 1,
      attachment: 0,
    });
    expect(Array.from(new Uint8Array(argumentFrame?.payload ?? []))).toEqual([
      3, 1, 4,
    ]);

    receiver.current?.(
      encodeMuonWebViewRpcBinaryFrame({
        kind: 'result',
        callId: 1,
        attachment: 0,
        payload: Uint8Array.from([5, 9, 2]).buffer,
      })
    );
    await expect(result).resolves.toBeInstanceOf(ArrayBuffer);
    expect(Array.from(new Uint8Array((await result) as ArrayBuffer))).toEqual([
      5, 9, 2,
    ]);
  });

  it('ignores malformed, unknown, and duplicate result messages', async () => {
    const receiver: {
      current: ((message: string | ArrayBuffer) => void) | undefined;
    } = { current: undefined };
    const transport: MuonWebViewRpcTransport = {
      send: () => {},
      setMessageHandler: (handler) => {
        receiver.current = handler;
      },
    };
    const client = createMuonWebViewRpcClient(transport);
    const result = client.call('', 'prototype.value', []);

    expect(() => receiver.current?.('{')).not.toThrow();
    expect(() =>
      receiver.current?.(
        JSON.stringify({
          version: 1,
          type: 'result',
          callId: 99,
          success: true,
          value: 'unknown',
        })
      )
    ).not.toThrow();
    receiver.current?.(
      JSON.stringify({
        version: 1,
        type: 'result',
        callId: 1,
        success: true,
        value: 'accepted',
      })
    );
    await expect(result).resolves.toBe('accepted');
    expect(() =>
      receiver.current?.(
        JSON.stringify({
          version: 1,
          type: 'result',
          callId: 1,
          success: true,
          value: 'duplicate',
        })
      )
    ).not.toThrow();
  });

  it('installs the capability boundary and releases pending calls on dispose', async () => {
    const sent: Array<string | ArrayBuffer> = [];
    const transport: MuonWebViewRpcTransport = {
      send: (message) => sent.push(message),
      setMessageHandler: () => {},
    };
    const client = createMuonWebViewRpcClient(transport);
    const target: Record<PropertyKey, unknown> = {};
    const uninstall = installMuonWebViewCapabilityBridge(client, target);
    const call = target.__muon_plugin_call as (
      capabilityId: string,
      functionPath: string,
      arguments_: readonly unknown[]
    ) => Promise<unknown>;

    const pending = call('capability', 'prototype.delay', []);
    uninstall();
    expect(target.__muon_plugin_call).toBeUndefined();
    client.dispose();

    await expect(pending).rejects.toThrow(
      'muon WebView RPC client was disposed'
    );
    expect(JSON.parse(sent[1] as string)).toEqual({
      version: 1,
      type: 'release',
    });
    expect(client.getPendingCallCount()).toBe(0);
  });
});
