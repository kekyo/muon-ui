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
  'muon.fs.readFile',
  'muon.fs.writeFile',
  'muon.fs.readTextFile',
  'muon.fs.writeTextFile',
  'muon.fs.stat',
  'muon.fs.lstat',
  'muon.fs.exists',
  'muon.fs.access',
  'muon.fs.readdir',
  'muon.fs.mkdir',
  'muon.fs.rm',
  'muon.fs.unlink',
  'muon.fs.rmdir',
  'muon.fs.rename',
  'muon.fs.copyFile',
  'muon.fs.appendFile',
  'muon.fs.appendTextFile',
  'muon.fs.truncate',
  'muon.fs.realpath',
  'muon.fs.readlink',
  'muon.fs.symlink',
  'muon.fs.watch',
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
      'muon.fs': 'fs-capability',
    });

    expect(Object.keys(api)).toEqual(['browser', 'environments', 'fs']);
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
    expect(Object.keys(api.fs)).toEqual(
      expectedFunctionPaths
        .filter((path) => path.startsWith('muon.fs.'))
        .map((path) => path.slice('muon.fs.'.length))
    );
    expect(Reflect.has(api.fs, 'dialogs')).toBe(false);

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

  it('preserves filesystem values while separating cancellation options', async () => {
    const binaryResult = new Uint8Array([4, 5, 6]).buffer;
    const call = vi
      .fn<MuonWebViewRpcClient['call']>()
      .mockResolvedValueOnce(undefined)
      .mockResolvedValueOnce(binaryResult)
      .mockResolvedValueOnce(
        '{"type":"file","size":3,"mtimeMs":10,"readonly":false}'
      );
    const client: MuonWebViewRpcClient = {
      call,
      dispose: () => {},
      getPendingCallCount: () => 0,
    };
    const api = createMuonAndroidSimpleApi(client, {
      'muon.browser': 'browser-capability',
      'muon.environments': 'environment-capability',
      'muon.fs': 'fs-capability',
    });
    const controller = new AbortController();
    const source = new Uint8Array([1, 2, 3]);

    await expect(
      api.fs.writeFile('/data/value.bin', source, {
        position: 4,
        signal: controller.signal,
      })
    ).resolves.toBeUndefined();
    expect(call).toHaveBeenLastCalledWith(
      'fs-capability',
      'muon.fs.writeFile',
      ['/data/value.bin', source, { position: 4 }],
      { signal: controller.signal }
    );

    await expect(
      api.fs.readFile('/data/value.bin', {
        position: 1,
        length: 2,
        signal: controller.signal,
      })
    ).resolves.toBe(binaryResult);
    expect(call).toHaveBeenLastCalledWith(
      'fs-capability',
      'muon.fs.readFile',
      ['/data/value.bin', { position: 1, length: 2 }],
      { signal: controller.signal }
    );

    const stats = await api.fs.stat('/data/value.bin');
    expect(stats).toMatchObject({
      type: 'file',
      size: 3,
      mtimeMs: 10,
      readonly: false,
    });
    expect(stats.isFile()).toBe(true);
    expect(stats.isDirectory()).toBe(false);
    expect(stats.isSymbolicLink()).toBe(false);
  });

  it('rejects Android-inapplicable symbolic link types before RPC', async () => {
    const call = vi.fn<MuonWebViewRpcClient['call']>();
    const client: MuonWebViewRpcClient = {
      call,
      dispose: () => {},
      getPendingCallCount: () => 0,
    };
    const api = createMuonAndroidSimpleApi(client, {
      'muon.browser': 'browser-capability',
      'muon.environments': 'environment-capability',
      'muon.fs': 'fs-capability',
    });

    await expect(
      api.fs.symlink('target', '/data/link', 'junction')
    ).rejects.toThrow('junction symbolic links are unavailable on Android');
    expect(call).not.toHaveBeenCalled();
  });

  it('can close a watcher from an asynchronous change listener', async () => {
    vi.useFakeTimers();
    try {
      let snapshotCount = 0;
      let released = false;
      const call = vi.fn<MuonWebViewRpcClient['call']>(
        async (_capabilityId, functionPath, arguments_) => {
          expect(functionPath).toBe('muon.fs.watch');
          const request = arguments_[0] as Readonly<Record<string, unknown>>;
          if (request.operation === 'acquire') {
            return '{"token":"watch-token"}';
          }
          if (request.operation === 'release') {
            released = true;
            return '{"released":true}';
          }
          snapshotCount += 1;
          return JSON.stringify({
            root: {
              type: 'directory',
              size: 0,
              mtimeMs: 1,
              readonly: false,
            },
            entries:
              snapshotCount === 1
                ? []
                : [
                    {
                      name: 'value.txt',
                      type: 'file',
                      size: 1,
                      mtimeMs: 2,
                      readonly: false,
                    },
                  ],
          });
        }
      );
      const client: MuonWebViewRpcClient = {
        call,
        dispose: () => {},
        getPendingCallCount: () => 0,
      };
      const api = createMuonAndroidSimpleApi(client, {
        'muon.browser': 'browser-capability',
        'muon.environments': 'environment-capability',
        'muon.fs': 'fs-capability',
      });
      let closeWatcher: (() => Promise<void>) | undefined;
      let listenerCompleted = false;
      const watcher = await api.fs.watch('/data', async (event) => {
        if (event.filename !== 'value.txt' || closeWatcher === undefined) {
          return;
        }
        await closeWatcher();
        listenerCompleted = true;
      });
      closeWatcher = watcher.close;

      vi.advanceTimersByTime(100);
      for (let index = 0; index < 20; index += 1) {
        await Promise.resolve();
      }

      expect(listenerCompleted).toBe(true);
      expect(released).toBe(true);
    } finally {
      vi.useRealTimers();
    }
  });
});
