// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { describe, expect, it, vi } from 'vitest';
import { createMuonNotificationsApi } from '../common/renderer/notifications.js';
import { createMuonAndroidNotificationsApi } from '../android/renderer/notification-permission-api.js';
import { createMuonAndroidSettingsApi } from '../android/renderer/settings-api.js';
import type { MuonWebViewRpcClient } from '../android/renderer/webview-rpc.js';

describe('local notification APIs', () => {
  it('accepts 64 JSON containers and rejects a 65th empty container before dispatch', async () => {
    const call = vi.fn();
    const api = createMuonNotificationsApi(call);
    let data: unknown = [];
    for (let depth = 1; depth < 64; depth++) data = [data];
    await api.post({ id: 'depth', title: 'T', body: 'B', data: data as never });
    await expect(
      api.post({ id: 'depth', title: 'T', body: 'B', data: [data] as never })
    ).rejects.toThrow(/64/);
    expect(call).toHaveBeenCalledTimes(1);
  });
  it('rejects a null channel description rather than silently replacing it', async () => {
    const call = vi.fn<MuonWebViewRpcClient['call']>().mockResolvedValue('{}');
    const api = createMuonAndroidNotificationsApi(
      { call, dispose: () => {}, getPendingCallCount: () => 0 },
      'android'
    );
    await expect(
      api.ensureChannel({
        id: 'orders',
        name: 'Orders',
        importance: 'default',
        description: null,
      } as never)
    ).rejects.toThrow(/description/);
    expect(call).not.toHaveBeenCalled();
  });
  it('posts JSON data and cancels an exact stable ID through the supplied capability', async () => {
    const call = vi.fn(async () => undefined);
    const api = createMuonNotificationsApi(call);
    const notification = {
      id: 'order/42',
      title: 'Order ready',
      body: 'Collect it',
      category: 'orders',
      data: { order: 42 },
    };
    await api.post(notification);
    await api.cancel('order/42');
    expect(call.mock.calls).toEqual([
      ['post', [JSON.stringify(notification)]],
      ['cancel', ['order/42']],
    ]);
  });
  it('keeps capability support separate from permission and category status', async () => {
    const capabilities = {
      interaction: { state: 'unsupported', reason: 'events-unavailable' },
    };
    const status = {
      app: { state: 'enabled', reason: null },
      category: { id: 'orders', state: 'disabled', reason: 'channel-disabled' },
    };
    const call = vi
      .fn()
      .mockResolvedValueOnce(JSON.stringify(capabilities))
      .mockResolvedValueOnce(JSON.stringify(status))
      .mockResolvedValueOnce(JSON.stringify({ ...status, category: null }));
    const api = createMuonNotificationsApi(call);
    expect(await api.getCapabilities()).toEqual(capabilities);
    expect(await api.getStatus('orders')).toEqual(status);
    expect((await api.getStatus()).category).toBeNull();
    expect(call.mock.calls).toEqual([
      ['getCapabilities', []],
      ['getStatus', ['{"category":"orders"}']],
      ['getStatus', ['{}']],
    ]);
  });
  it.each([
    { id: '', title: 'T', body: 'B' },
    { id: 'x', title: '', body: 'B' },
    { id: 'x', title: 'T', body: 'B', category: '' },
    { id: 'x', title: 'T', body: 'B', actions: [] },
    { id: 'x', title: 'T', body: 'B', data: { missing: undefined } },
    { id: 'x', title: 'T', body: 'B', data: Number.NaN },
    { id: 'x', title: 'T', body: 'B', data: new Date() },
    { id: 'x', title: 'T', body: 'B', data: 'あ'.repeat(6000) },
  ])(
    'rejects lossy or unsupported notification arguments: %j',
    async (value) => {
      const call = vi.fn();
      await expect(
        createMuonNotificationsApi(call).post(value as never)
      ).rejects.toThrow();
      expect(call).not.toHaveBeenCalled();
    }
  );
  it('rejects a circular payload while allowing repeated non-circular values', async () => {
    const circular: Record<string, unknown> = {};
    circular.self = circular;
    const call = vi.fn();
    const api = createMuonNotificationsApi(call);
    await expect(
      api.post({ id: 'x', title: 'T', body: 'B', data: circular as never })
    ).rejects.toThrow();
    const shared = { value: 1 };
    await api.post({ id: 'x', title: 'T', body: 'B', data: [shared, shared] });
    expect(call).toHaveBeenCalledTimes(1);
  });
  it('creates and reads a channel without claiming its requested importance is effective', async () => {
    const effective = {
      id: 'orders',
      name: 'Orders',
      description: 'Order status',
      importance: 'low',
      enabled: true,
    };
    const call = vi
      .fn<MuonWebViewRpcClient['call']>()
      .mockResolvedValue(JSON.stringify(effective));
    const api = createMuonAndroidNotificationsApi(
      { call, dispose: () => {}, getPendingCallCount: () => 0 },
      'android'
    );
    const channel = {
      id: 'orders',
      name: 'Orders',
      description: 'Order status',
      importance: 'high' as const,
    };
    expect(await api.ensureChannel(channel)).toEqual(effective);
    expect(await api.getChannel('orders')).toEqual(effective);
    expect(call.mock.calls).toEqual([
      [
        'android',
        'muon.android.notifications.ensureChannel',
        [JSON.stringify(channel)],
      ],
      ['android', 'muon.android.notifications.getChannel', ['orders']],
    ]);
  });
  it('opens channel settings with a channel-specific typed target', async () => {
    const target = {
      type: 'notification-channel' as const,
      channelId: 'orders',
    };
    const result = { requestedTarget: target, target, fallbackUsed: false };
    const call = vi
      .fn<MuonWebViewRpcClient['call']>()
      .mockResolvedValue(JSON.stringify(result));
    const api = createMuonAndroidSettingsApi(
      { call, dispose: () => {}, getPendingCallCount: () => 0 },
      'settings'
    );
    expect(await api.open(target)).toEqual(result);
    expect(call).toHaveBeenCalledWith(
      'settings',
      'muon.android.settings.open',
      ['{"target":"notification-channel","channelId":"orders"}']
    );
  });
});
