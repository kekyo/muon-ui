// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { describe, expect, it, vi } from 'vitest';
import {
  createMuonNotificationsApi,
  type MuonNotificationEvent,
  type MuonNotificationWatcher,
} from '../common/renderer/notifications.js';
import { createMuonAndroidSimpleApi } from '../android/renderer/android-api.js';
import type { MuonWebViewRpcClient } from '../android/renderer/webview-rpc.js';

describe('durable notification event facade', () => {
  it('reads pending events without acknowledging them and explicitly acknowledges one stable event ID', async () => {
    const event = {
      eventId: 'revision-1',
      id: 'order',
      type: 'activate',
      actionId: null,
      data: { order: 42 },
    };
    const call = vi.fn().mockResolvedValueOnce(JSON.stringify([event]));
    const api = createMuonNotificationsApi(call);
    expect(await api.getPendingEvents()).toEqual([event]);
    expect(call).toHaveBeenCalledTimes(1);
    await api.acknowledgeEvent(event.eventId);
    expect(call.mock.calls).toEqual([
      ['getPendingEvents', []],
      ['acknowledgeEvent', ['revision-1']],
    ]);
    await expect(api.acknowledgeEvent(' ')).rejects.toThrow(/eventId/);
    expect(call).toHaveBeenCalledTimes(2);
  });

  it('serializes named launch actions without interpreting their IDs as reserved default actions', async () => {
    const call = vi.fn();
    const options = {
      id: 'order',
      title: 'Ready',
      body: '',
      actions: [
        { id: 'default', title: 'View order' },
        { id: 'archive', title: 'Archive' },
      ],
    };
    await createMuonNotificationsApi(call).post(options);
    expect(call).toHaveBeenCalledWith('post', [JSON.stringify(options)]);
  });

  it.each([
    null,
    [{ id: 'x', title: '' }],
    [{ id: 'x', title: 'X', extra: true }],
    [
      { id: 'x', title: 'X' },
      { id: 'x', title: 'Other' },
    ],
    Array.from({ length: 4 }, (_, index) => ({
      id: String(index),
      title: 'Action',
    })),
    [{ id: 'x', title: 'X'.repeat(129) }],
  ])('rejects invalid actions before dispatch: %j', async (actions) => {
    const call = vi.fn();
    await expect(
      createMuonNotificationsApi(call).post({
        id: 'order',
        title: 'Ready',
        body: '',
        actions,
      } as never)
    ).rejects.toThrow();
    expect(call).not.toHaveBeenCalled();
  });

  it('returns a watcher before delivering a batch and permits closing from its first listener', async () => {
    const first: MuonNotificationEvent = {
      eventId: 'one',
      id: 'order',
      type: 'activate',
      actionId: null,
      data: null,
    };
    const second: MuonNotificationEvent = {
      eventId: 'two',
      id: 'other',
      type: 'action',
      actionId: 'open',
      data: {},
    };
    const call = vi.fn(async (name: string, args: readonly unknown[]) => {
      expect(name).toBe('watchEvents');
      const request = JSON.parse(args[0] as string);
      if (request.operation === 'acquire')
        return JSON.stringify({ token: 'lease', cursor: 10 });
      if (request.operation === 'release') return undefined;
      expect(request).toEqual({
        operation: 'next',
        token: 'lease',
        afterCursor: 10,
      });
      return JSON.stringify({ events: [first, second], cursor: 12 });
    });
    let finish!: () => void;
    const finished = new Promise<void>((resolve) => {
      finish = resolve;
    });
    const received: MuonNotificationEvent[] = [];
    let watcher: MuonNotificationWatcher | undefined;
    watcher = await createMuonNotificationsApi(call).watchEvents(
      async (event) => {
        received.push(event);
        expect(watcher).toBeDefined();
        await watcher!.close();
        finish();
      }
    );
    await finished;
    await watcher.close();
    expect(received).toEqual([first]);
    expect(
      call.mock.calls.filter(
        ([, args]) => JSON.parse(args[0] as string).operation === 'release'
      )
    ).toHaveLength(1);
  });

  it('cancels an outstanding native event wait through the Android facade when the watcher closes', async () => {
    let nextReady!: () => void;
    const ready = new Promise<void>((resolve) => {
      nextReady = resolve;
    });
    let cancelled = false;
    const call = vi.fn<MuonWebViewRpcClient['call']>(
      async (capability, path, args, options) => {
        expect(capability).toBe('notifications');
        expect(path).toBe('muon.notifications.watchEvents');
        const request = JSON.parse(args[0] as string);
        if (request.operation === 'acquire')
          return JSON.stringify({ token: 'lease', cursor: 0 });
        if (request.operation === 'release') return undefined;
        return await new Promise((_resolve, reject) => {
          options?.signal?.addEventListener(
            'abort',
            () => {
              cancelled = true;
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
        'muon.android.settings': 'settings',
        'muon.android.notifications': 'android',
        'muon.notifications': 'notifications',
      }
    );
    const listener = vi.fn();
    const watcher = await api.notifications.watchEvents(listener);
    await ready;
    await watcher.close();
    expect(cancelled).toBe(true);
    expect(listener).not.toHaveBeenCalled();
  });

  it('continues after a listener error without acknowledging either event', async () => {
    const logged = vi.spyOn(console, 'error').mockImplementation(() => {});
    const events: MuonNotificationEvent[] = [
      {
        eventId: 'one',
        id: 'first',
        type: 'activate',
        actionId: null,
        data: null,
      },
      {
        eventId: 'two',
        id: 'second',
        type: 'activate',
        actionId: null,
        data: null,
      },
    ];
    const call = vi.fn(async (_name: string, args: readonly unknown[]) => {
      const request = JSON.parse(args[0] as string);
      if (request.operation === 'acquire')
        return JSON.stringify({ token: 'lease', cursor: 0 });
      if (request.operation === 'release') return undefined;
      return JSON.stringify({ events, cursor: 2 });
    });
    let finish!: () => void;
    const finished = new Promise<void>((resolve) => {
      finish = resolve;
    });
    const received: string[] = [];
    const watcher = await createMuonNotificationsApi(call).watchEvents(
      async (event) => {
        received.push(event.id);
        if (event.id === 'first') throw new Error('Handler failed');
        await watcher.close();
        finish();
      }
    );
    try {
      await finished;
      expect(received).toEqual(['first', 'second']);
      expect(logged).toHaveBeenCalledWith(
        'Muon notification listener failed',
        expect.any(Error)
      );
      expect(call.mock.calls.every(([name]) => name === 'watchEvents')).toBe(
        true
      );
    } finally {
      await watcher.close();
      logged.mockRestore();
    }
  });
});
