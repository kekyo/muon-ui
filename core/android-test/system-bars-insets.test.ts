// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { describe, expect, it, vi } from 'vitest';
import { createMuonAndroidSimpleApi } from '../android/renderer/android-api.js';
import type { MuonWebViewRpcClient } from '../android/renderer/webview-rpc.js';
import {
  createMuonAndroidSystemBarsApi,
  type MuonAndroidInsetsWatcher,
} from '../android/renderer/system-bars-api.js';

describe('Android insets subscriptions', () => {
  it('returns the handle before the first listener so it can close immediately', async () => {
    let delivered!: (watcher: MuonAndroidInsetsWatcher | undefined) => void;
    const firstDelivery = new Promise<MuonAndroidInsetsWatcher | undefined>(
      (resolve) => {
        delivered = resolve;
      }
    );
    const call = vi.fn<MuonWebViewRpcClient['call']>(
      async (_id, _path, args, options) => {
        const request = JSON.parse(args[0] as string);
        if (request.operation === 'acquire')
          return JSON.stringify({ token: 'lease', insets: { revision: 1 } });
        if (request.operation === 'release') return null;
        return await new Promise((_resolve, reject) => {
          options?.signal?.addEventListener(
            'abort',
            () => reject(new Error('Cancelled')),
            { once: true }
          );
        });
      }
    );
    const api = createMuonAndroidSystemBarsApi(
      { call, dispose: () => {}, getPendingCallCount: () => 0 },
      'bars'
    );
    let watcher: MuonAndroidInsetsWatcher | undefined;
    watcher = await api.watchInsets(async () => {
      const returned = watcher;
      if (returned !== undefined) await returned.close();
      delivered(returned);
    });
    try {
      expect(await firstDelivery).toBe(watcher);
      expect(
        call.mock.calls.map(
          (entry) => JSON.parse(entry[2][0] as string).operation
        )
      ).toEqual(['acquire', 'release']);
    } finally {
      await watcher.close();
    }
  });

  it('delivers the initial snapshot and changed CSS values and permits closing inside the listener', async () => {
    const snapshots: unknown[] = [];
    const initial = {
      revision: 1,
      unit: 'css-px',
      contentSafeArea: { top: 12.5, right: 0, bottom: 24, left: 0 },
    };
    const changed = {
      ...initial,
      revision: 2,
      contentSafeArea: { ...initial.contentSafeArea, top: 6.25 },
    };
    let notifyNext!: (value: unknown) => void;
    let nextReady!: () => void;
    const ready = new Promise<void>((resolve) => {
      nextReady = resolve;
    });
    let listenerFinished!: () => void;
    const finished = new Promise<void>((resolve) => {
      listenerFinished = resolve;
    });
    const call = vi.fn<MuonWebViewRpcClient['call']>(
      async (_capability, path, args) => {
        expect(path).toBe('muon.android.systemBars.watchInsets');
        const request = JSON.parse(args[0] as string);
        if (request.operation === 'acquire')
          return JSON.stringify({ token: 'lease', insets: initial });
        expect(request.token).toBe('lease');
        if (request.operation === 'release') return null;
        expect(request.operation).toBe('next');
        expect(request.afterRevision).toBe(1);
        return await new Promise((resolve) => {
          notifyNext = resolve;
          nextReady();
        });
      }
    );
    const api = createMuonAndroidSimpleApi(
      { call, dispose: () => {}, getPendingCallCount: () => 0 },
      {
        'muon.browser': 'browser',
        'muon.environments': 'environments',
        'muon.fs': 'fs',
        'muon.android.systemBars': 'bars',
      }
    );
    const watch = api.android.systemBars.watchInsets;
    let watcher: { close(): Promise<void> };
    watcher = await watch(async (value) => {
      snapshots.push(value);
      if (snapshots.length === 2) {
        await watcher.close();
        listenerFinished();
      }
    });
    await ready;
    expect(snapshots).toEqual([initial]);
    notifyNext(JSON.stringify({ insets: changed }));
    await finished;
    await watcher.close();
    expect(snapshots).toEqual([initial, changed]);
    expect(
      call.mock.calls.filter(
        (entry) => JSON.parse(entry[2][0] as string).operation === 'release'
      )
    ).toHaveLength(1);
  });

  it('cancels a pending event wait when closed instead of waiting for another inset change', async () => {
    let nextReady!: () => void;
    const ready = new Promise<void>((resolve) => {
      nextReady = resolve;
    });
    let aborted = false;
    const call = vi.fn<MuonWebViewRpcClient['call']>(
      async (_capability, _path, args, options) => {
        const request = JSON.parse(args[0] as string);
        if (request.operation === 'acquire')
          return JSON.stringify({ token: 'lease', insets: { revision: 1 } });
        if (request.operation === 'release') return null;
        return await new Promise((_resolve, reject) => {
          options?.signal?.addEventListener(
            'abort',
            () => {
              aborted = true;
              reject(new Error('Cancelled'));
            },
            { once: true }
          );
          nextReady();
        });
      }
    );
    const api = createMuonAndroidSimpleApi(
      { call, dispose: () => {}, getPendingCallCount: () => 0 },
      {
        'muon.browser': 'browser',
        'muon.environments': 'environments',
        'muon.fs': 'fs',
        'muon.android.systemBars': 'bars',
      }
    );
    const watch = api.android.systemBars.watchInsets;
    const watcher = await watch(() => {});
    await ready;
    await watcher.close();
    expect(aborted).toBe(true);
  });
});
