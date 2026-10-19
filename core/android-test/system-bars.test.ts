// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { describe, expect, it, vi } from 'vitest';
import { createMuonAndroidSimpleApi } from '../android/renderer/android-api.js';
import type { MuonWebViewRpcClient } from '../android/renderer/webview-rpc.js';

describe('Android system bar API', () => {
  it('configures layout and icon appearance independently of fullscreen visibility', async () => {
    const call = vi.fn<MuonWebViewRpcClient['call']>().mockResolvedValue(null);
    const api = createMuonAndroidSimpleApi(
      { call, dispose: () => {}, getPendingCallCount: () => 0 },
      {
        'muon.browser': 'browser-capability',
        'muon.environments': 'environment-capability',
        'muon.fs': 'fs-capability',
        'muon.android.systemBars': 'system-bars-capability',
      }
    );
    const options = {
      contentLayout: 'edge-to-edge',
      statusBar: { icons: 'light' },
      navigationBar: { icons: 'dark' },
    } as const;
    await api.android.systemBars.configure(options);
    expect(call).toHaveBeenCalledWith(
      'system-bars-capability',
      'muon.android.systemBars.configure',
      [JSON.stringify(options)]
    );
  });
  it('sends a partial visibility configuration and decodes requested and actual state', async () => {
    const state = {
      requested: {
        statusBar: { visible: false },
        navigationBar: { visible: true },
      },
      actual: {
        statusBar: { visible: true },
        navigationBar: { visible: null },
      },
    };
    const call = vi
      .fn<MuonWebViewRpcClient['call']>()
      .mockResolvedValueOnce(null)
      .mockResolvedValueOnce(JSON.stringify(state));
    const api = createMuonAndroidSimpleApi(
      { call, dispose: () => {}, getPendingCallCount: () => 0 },
      {
        'muon.browser': 'browser-capability',
        'muon.environments': 'environment-capability',
        'muon.fs': 'fs-capability',
        'muon.android.systemBars': 'system-bars-capability',
      }
    );
    await api.android.systemBars.configure({ statusBar: { visible: false } });
    expect(call).toHaveBeenNthCalledWith(
      1,
      'system-bars-capability',
      'muon.android.systemBars.configure',
      ['{"statusBar":{"visible":false}}']
    );
    await expect(api.android.systemBars.getState()).resolves.toEqual(state);
    expect(call).toHaveBeenNthCalledWith(
      2,
      'system-bars-capability',
      'muon.android.systemBars.getState',
      []
    );
  });

  it.each([
    null,
    [],
    { statusBar: false },
    { statusBar: { visible: 'false' } },
    { navigationBar: { visible: null } },
    { navigationBar: { unknown: true } },
    { unknown: true },
    { contentLayout: 'fit' },
    { statusBar: { icons: 'automatic' } },
  ])(
    'rejects unsupported or malformed settings before native dispatch: %j',
    async (options) => {
      const call = vi.fn<MuonWebViewRpcClient['call']>();
      const api = createMuonAndroidSimpleApi(
        { call, dispose: () => {}, getPendingCallCount: () => 0 },
        {
          'muon.browser': 'browser-capability',
          'muon.environments': 'environment-capability',
          'muon.fs': 'fs-capability',
          'muon.android.systemBars': 'system-bars-capability',
        }
      );
      await expect(
        api.android.systemBars.configure(options as never)
      ).rejects.toThrow();
      expect(call).not.toHaveBeenCalled();
    }
  );
});
