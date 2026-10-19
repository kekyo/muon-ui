// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { describe, expect, it, vi } from 'vitest';
import { createMuonAndroidSimpleApi } from '../android/renderer/android-api.js';
import type { MuonWebViewRpcClient } from '../android/renderer/webview-rpc.js';

describe('Android settings and notification permissions', () => {
  it('opens only the requested app screen, with an explicit fallback and structured result', async () => {
    const result = {
      requestedTarget: 'notifications',
      target: 'app-details',
      fallbackUsed: true,
    };
    const call = vi
      .fn<MuonWebViewRpcClient['call']>()
      .mockResolvedValueOnce(false)
      .mockResolvedValueOnce(JSON.stringify(result));
    const api = createMuonAndroidSimpleApi(
      { call, dispose: () => {}, getPendingCallCount: () => 0 },
      {
        'muon.browser': 'browser',
        'muon.environments': 'environment',
        'muon.fs': 'fs',
        'muon.android.systemBars': 'bars',
        'muon.android.settings': 'settings',
        'muon.android.notifications': 'notifications',
      }
    );
    await expect(
      api.android.settings.isAvailable('notifications')
    ).resolves.toBe(false);
    await expect(
      api.android.settings.open('notifications', { fallback: 'app-details' })
    ).resolves.toEqual(result);
    expect(call.mock.calls).toEqual([
      [
        'settings',
        'muon.android.settings.isAvailable',
        ['{"target":"notifications"}'],
      ],
      [
        'settings',
        'muon.android.settings.open',
        ['{"target":"notifications","fallback":"app-details"}'],
      ],
    ]);
  });

  it('preserves permission observations and the limited request outcome', async () => {
    const state = {
      runtimePermission: 'denied',
      notificationsEnabled: false,
      requestedBefore: true,
      shouldShowRationale: false,
      settingsSuggested: true,
    };
    const result = { state, result: 'interrupted' };
    const call = vi
      .fn<MuonWebViewRpcClient['call']>()
      .mockResolvedValueOnce(JSON.stringify(state))
      .mockResolvedValueOnce(JSON.stringify(result));
    const api = createMuonAndroidSimpleApi(
      { call, dispose: () => {}, getPendingCallCount: () => 0 },
      {
        'muon.browser': 'browser',
        'muon.environments': 'environment',
        'muon.fs': 'fs',
        'muon.android.systemBars': 'bars',
        'muon.android.settings': 'settings',
        'muon.android.notifications': 'notifications',
      }
    );
    await expect(
      api.android.notifications.getPermissionState()
    ).resolves.toEqual(state);
    await expect(
      api.android.notifications.requestPermission()
    ).resolves.toEqual(result);
    expect(call.mock.calls.map((entry) => entry.slice(0, 2))).toEqual([
      ['notifications', 'muon.android.notifications.getPermissionState'],
      ['notifications', 'muon.android.notifications.requestPermission'],
    ]);
  });

  it.each([
    ['wifi', undefined],
    ['app-details', null],
    ['notifications', { fallback: 'wifi' }],
    ['notifications', { unrecognized: true }],
    ['notifications', []],
  ])(
    'rejects malformed settings arguments before native dispatch: %j',
    async (target, options) => {
      const call = vi.fn<MuonWebViewRpcClient['call']>();
      const api = createMuonAndroidSimpleApi(
        { call, dispose: () => {}, getPendingCallCount: () => 0 },
        {
          'muon.browser': 'browser',
          'muon.environments': 'environment',
          'muon.fs': 'fs',
          'muon.android.systemBars': 'bars',
          'muon.android.settings': 'settings',
          'muon.android.notifications': 'notifications',
        }
      );
      await expect(
        api.android.settings.open(target as never, options as never)
      ).rejects.toThrow();
      expect(call).not.toHaveBeenCalled();
    }
  );
});
