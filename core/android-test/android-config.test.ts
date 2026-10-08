// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { describe, expect, it } from 'vitest';

import { validateMuonAndroidConfig } from '../android/renderer/android-config.js';

describe('muon Android configuration', () => {
  it('accepts supported values and warns about unenforced desktop policies', () => {
    expect(
      validateMuonAndroidConfig({
        iconPath: 'icons/app.png',
        config: { channel: 'test' },
        asset: { storage: 'assets' },
        browser: {
          startPage: 'https://main.asset.muon.invalid/index.html',
          initialWindowState: 'fullscreen',
          backgroundColor: 'system',
          contextMenu: { mode: 'disabled' },
          keybind: {},
          allowUnsafeJavaScriptParentAccess: [],
        },
        plugin: {
          mode: 'validate',
          pages: ['https://main.asset.muon.invalid/**'],
          plugins: [
            {
              name: 'internal',
              allow: ['muon.environments.getConfigValues'],
              config: { source: 'android' },
            },
          ],
        },
        network: {
          allow: ['https://example.test/**'],
          authorizedOrigin: ['https://example.test'],
          localAccess: { allowInsecureLocalhost: false },
        },
        cdp: { enable: false },
        node: {},
      })
    ).toEqual({
      warnings: [
        'muon.json network policy is not enforced by the Android WebView backend.',
      ],
    });
  });

  it('accepts a minimal shared configuration without warnings', () => {
    expect(validateMuonAndroidConfig({})).toEqual({ warnings: [] });
  });
  it.each(['*', 'https://external.example/**', 'asset://main/allowed.html'])(
    'rejects a page rule that Android cannot enforce: %s',
    (page) => {
      expect(() =>
        validateMuonAndroidConfig({ plugin: { pages: [page] } })
      ).toThrow('plugin.pages');
    }
  );

  it.each([
    {
      name: 'browser.profilePath',
      config: { browser: { profilePath: 'profiles/main' } },
      diagnostic: 'browser.profilePath',
    },
    {
      name: 'browser.profile',
      config: { browser: { profile: 'profiles/main' } },
      diagnostic: 'browser.profile',
    },
    {
      name: 'non-mobile initial window state',
      config: { browser: { initialWindowState: 'maximized' } },
      diagnostic: 'browser.initialWindowState',
    },
    {
      name: 'desktop title bar type',
      config: { browser: { titleBarType: 'native' } },
      diagnostic: 'browser.titleBarType',
    },
    {
      name: 'desktop title bar visibility',
      config: { browser: { initialTitleBarVisibility: true } },
      diagnostic: 'browser.initialTitleBarVisibility',
    },
    {
      name: 'desktop title bar icon',
      config: { browser: { initialTitleBarIcon: 'icons/app.png' } },
      diagnostic: 'browser.initialTitleBarIcon',
    },
    {
      name: 'custom context menu',
      config: { browser: { contextMenu: { mode: 'custom' } } },
      diagnostic: 'browser.contextMenu.mode',
    },
    {
      name: 'keyboard shortcuts',
      config: { browser: { keybind: { reload: 'Ctrl+R' } } },
      diagnostic: 'browser.keybind',
    },
    {
      name: 'unsafe parent access',
      config: {
        browser: {
          allowUnsafeJavaScriptParentAccess: ['https://example.test/**'],
        },
      },
      diagnostic: 'browser.allowUnsafeJavaScriptParentAccess',
    },
    {
      name: 'CEF developer protocol',
      config: { cdp: { enable: true } },
      diagnostic: 'cdp.enable',
    },
    {
      name: 'Node sidecar project',
      config: { node: { project: 'node-project' } },
      diagnostic: 'node.project',
    },
    {
      name: 'runtime plugin search path',
      config: { plugin: { path: 'plugins' } },
      diagnostic: 'plugin.path',
    },
    {
      name: 'external plugin signature',
      config: {
        plugin: {
          plugins: [{ name: 'native', signature: '00'.repeat(32) }],
        },
      },
      diagnostic: 'plugin.plugins[0].signature',
    },
    {
      name: 'external plugin signature salt',
      config: {
        plugin: { plugins: [{ name: 'native', salt: '00'.repeat(16) }] },
      },
      diagnostic: 'plugin.plugins[0].salt',
    },
  ])('rejects $name', ({ config, diagnostic }) => {
    expect(() => validateMuonAndroidConfig(config)).toThrow(diagnostic);
  });

  it('rejects malformed configuration boundaries', () => {
    expect(() => validateMuonAndroidConfig(null)).toThrow(
      'muon.json must be an object'
    );
    expect(() =>
      validateMuonAndroidConfig({ browser: 'not-an-object' })
    ).toThrow('muon.json browser must be an object');
  });
});
