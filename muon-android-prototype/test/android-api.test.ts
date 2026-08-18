// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { describe, expect, it, vi } from 'vitest';

import {
  createMuonAndroidSimpleApi,
  expandMuonAndroidFunctionAllows,
  muonAndroidBuiltinFunctionPaths,
} from '../src/android-api.js';
import type { MuonWebViewRpcClient } from '../src/webview-rpc.js';

const expectedFunctionPaths = [
  'muon.browser.reload',
  'muon.browser.toggleFullscreen',
  'muon.browser.enterFullscreen',
  'muon.browser.exitFullscreen',
  'muon.browser.zoomIn',
  'muon.browser.zoomOut',
  'muon.browser.resetZoom',
  'muon.browser.close',
  'muon.environments.getVariables',
  'muon.environments.getConfigValues',
  'muon.environments.getProcessId',
  'muon.environments.getRuntimeInfo',
] as const;

describe('muon Android API metadata', () => {
  it('contains exactly the APIs implemented for Android', () => {
    expect(muonAndroidBuiltinFunctionPaths).toEqual(expectedFunctionPaths);
  });

  it('expands wildcard allows without exposing unsupported desktop APIs', () => {
    expect(
      expandMuonAndroidFunctionAllows([
        'muon.browser.*',
        'muon.environments.getProcessId',
      ])
    ).toEqual([
      'muon.browser.reload',
      'muon.browser.toggleFullscreen',
      'muon.browser.enterFullscreen',
      'muon.browser.exitFullscreen',
      'muon.browser.zoomIn',
      'muon.browser.zoomOut',
      'muon.browser.resetZoom',
      'muon.browser.close',
      'muon.environments.getProcessId',
    ]);
    expect(() =>
      expandMuonAndroidFunctionAllows(['muon.browser.hardReload'])
    ).toThrow(
      'Muon function is unavailable for Android: muon.browser.hardReload'
    );
  });

  it('creates only supported simple-mode functions', async () => {
    const call = vi.fn<MuonWebViewRpcClient['call']>().mockResolvedValue(null);
    const client: MuonWebViewRpcClient = {
      call,
      dispose: () => {},
      getPendingCallCount: () => 0,
    };
    const api = createMuonAndroidSimpleApi(client, {
      'muon.browser': 'browser-capability',
      'muon.environments': 'environment-capability',
    });

    expect(Object.keys(api)).toEqual(['browser', 'environments']);
    expect(Object.keys(api.browser)).toEqual([
      'reload',
      'toggleFullscreen',
      'enterFullscreen',
      'exitFullscreen',
      'zoomIn',
      'zoomOut',
      'resetZoom',
      'close',
    ]);
    expect(Reflect.has(api.browser, 'hardReload')).toBe(false);
    expect(Reflect.has(api, 'launcher')).toBe(false);
    expect(Reflect.has(api, 'executor')).toBe(false);
    expect(Reflect.has(api, 'fs')).toBe(false);

    await expect(api.browser.reload()).resolves.toBeUndefined();
    expect(call).toHaveBeenCalledWith(
      'browser-capability',
      'muon.browser.reload',
      []
    );

    call.mockResolvedValueOnce('{"channel":"android"}');
    await expect(api.environments.getConfigValues()).resolves.toEqual({
      channel: 'android',
    });
    expect(call).toHaveBeenLastCalledWith(
      'environment-capability',
      'muon.environments.getConfigValues',
      []
    );
  });
});
