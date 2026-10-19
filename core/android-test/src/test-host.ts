// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import {
  createMuonWebViewRpcClient,
  createMuonWebViewRpcTransport,
  installMuonWebViewCapabilityBridge,
  type MuonWebViewJavaScriptBridge,
} from '../../android/renderer/webview-rpc.js';
import {
  createMuonAndroidSimpleApi,
  type MuonAndroidInsetsWatcher,
} from '../../android/renderer/android-api.js';
import { installMuonAndroidNativePluginApi } from '../../android/renderer/native-plugin-api.js';
import { readMuonAndroidRendererMetadata } from '../../android/renderer/native-plugin-metadata.js';

/** Test operations that require direct RPC client controls. */
export interface MuonAndroidTestOperations {
  /** Starts and immediately aborts a delayed native call. */
  readonly cancelDelayed: () => Promise<unknown>;
}

const app = document.querySelector<HTMLElement>('#app');
if (app === null) {
  throw new Error('Test root element was not found');
}

const bridge = Reflect.get(globalThis, 'muonAndroidRpc') as
  MuonWebViewJavaScriptBridge | undefined;

if (bridge === undefined) {
  app.textContent = 'muon Android native bridge is unavailable';
} else {
  const rendererMetadata = readMuonAndroidRendererMetadata(
    Reflect.get(globalThis, '__muon_android_plugin_metadata')
  );
  const client = createMuonWebViewRpcClient(
    createMuonWebViewRpcTransport(bridge),
    rendererMetadata
  );
  const uninstallCapabilityBridge = installMuonWebViewCapabilityBridge(client);
  const simpleApi = createMuonAndroidSimpleApi(client, {
    'muon.browser': 'browser-capability',
    'muon.environments': 'environment-capability',
    'muon.fs': 'fs-capability',
    'muon.android.systemBars': 'system-bars-capability',
    'muon.android.settings': 'settings-capability',
    'muon.android.notifications': 'android-notifications-capability',
  });
  const androidApi = {
    ...simpleApi,
    android: {
      systemBars: simpleApi.android.systemBars,
      settings: simpleApi.android.settings,
      ...(rendererMetadata.builtinFunctions.some((path) =>
        path.startsWith('muon.android.notifications.')
      )
        ? { notifications: simpleApi.android.notifications }
        : {}),
    },
  };
  const uninstallNativePluginApi = installMuonAndroidNativePluginApi(
    client,
    rendererMetadata,
    { muon: androidApi }
  );
  const operations: MuonAndroidTestOperations = {
    cancelDelayed: () => {
      const controller = new AbortController();
      const result = client.call(
        'prototype-capability',
        'prototype.delay',
        [],
        { signal: controller.signal }
      );
      controller.abort();
      return result;
    },
  };
  Reflect.set(globalThis, '__muon_android_prototype', operations);

  const heading = document.createElement('h1');
  heading.textContent = 'muon Android prototype';
  const status = document.createElement('output');
  status.id = 'rpc-status';
  status.textContent = 'Ready';
  const button = document.createElement('button');
  button.type = 'button';
  button.textContent = 'Get configuration';
  button.addEventListener('click', async () => {
    status.textContent = 'Loading';
    try {
      const result = await client.call(
        'environment-capability',
        'muon.environments.getConfigValues',
        []
      );
      status.textContent = String(result);
    } catch (error) {
      status.textContent =
        error instanceof Error ? error.message : 'Unknown RPC error';
    }
  });

  app.replaceChildren(heading, button, status);
  const bars = document.createElement('fieldset');
  const legend = document.createElement('legend');
  legend.textContent = 'Android system bars';
  const barState = document.createElement('pre');
  barState.id = 'system-bars-state';
  bars.append(legend, barState);
  const actions: ReadonlyArray<readonly [string, () => Promise<unknown>]> = [
    [
      'Show status bar',
      async () =>
        await androidApi.android.systemBars.configure({
          statusBar: { visible: true },
        }),
    ],
    [
      'Hide status bar',
      async () =>
        await androidApi.android.systemBars.configure({
          statusBar: { visible: false },
        }),
    ],
    [
      'Show navigation bar',
      async () =>
        await androidApi.android.systemBars.configure({
          navigationBar: { visible: true },
        }),
    ],
    [
      'Hide navigation bar',
      async () =>
        await androidApi.android.systemBars.configure({
          navigationBar: { visible: false },
        }),
    ],
    ['Enter fullscreen', androidApi.browser.enterFullscreen],
    ['Exit fullscreen', androidApi.browser.exitFullscreen],
    ['Toggle fullscreen', androidApi.browser.toggleFullscreen],
    [
      'Use safe area',
      async () =>
        await androidApi.android.systemBars.configure({
          contentLayout: 'safe-area',
        }),
    ],
    [
      'Draw edge to edge',
      async () =>
        await androidApi.android.systemBars.configure({
          contentLayout: 'edge-to-edge',
        }),
    ],
    [
      'Use light icons',
      async () =>
        await androidApi.android.systemBars.configure({
          statusBar: { icons: 'light' },
          navigationBar: { icons: 'light' },
        }),
    ],
    [
      'Use dark icons',
      async () =>
        await androidApi.android.systemBars.configure({
          statusBar: { icons: 'dark' },
          navigationBar: { icons: 'dark' },
        }),
    ],
    ['Read bar state', async () => {}],
  ];
  for (const [label, action] of actions) {
    const control = document.createElement('button');
    control.type = 'button';
    control.textContent = label;
    control.addEventListener('click', async () => {
      try {
        await action();
        barState.textContent = JSON.stringify(
          await androidApi.android.systemBars.getState(),
          null,
          2
        );
        insetsOutput.textContent = JSON.stringify(
          await androidApi.android.systemBars.getInsets(),
          null,
          2
        );
      } catch (error) {
        barState.textContent =
          error instanceof Error ? error.message : String(error);
      }
    });
    bars.append(control);
  }
  app.append(bars);
  const keyboardInput = document.createElement('input');
  keyboardInput.id = 'ime-input';
  keyboardInput.placeholder = 'Show the software keyboard';
  keyboardInput.setAttribute('aria-label', 'Keyboard test input');
  const edgeButton = document.createElement('button');
  edgeButton.id = 'safe-area-marker';
  edgeButton.textContent = 'Safe-area corner';
  edgeButton.style.cssText =
    'position:fixed;right:var(--muon-safe-area-right,0px);bottom:var(--muon-safe-area-bottom,0px);background:#0067a3;color:white;padding:12px;border:0';
  const insetsOutput = document.createElement('pre');
  insetsOutput.id = 'system-bars-insets';
  app.append(keyboardInput, insetsOutput, edgeButton);
  let insetsWatcher: MuonAndroidInsetsWatcher | undefined;
  const startInsets = document.createElement('button');
  startInsets.textContent = 'Start insets updates';
  const stopInsets = document.createElement('button');
  stopInsets.textContent = 'Stop insets updates';
  stopInsets.disabled = true;
  startInsets.addEventListener('click', async () => {
    startInsets.disabled = true;
    try {
      insetsWatcher = await androidApi.android.systemBars.watchInsets(
        (insets) => {
          insetsOutput.textContent = JSON.stringify(insets, null, 2);
        }
      );
      stopInsets.disabled = false;
    } catch (error) {
      startInsets.disabled = false;
      insetsOutput.textContent =
        error instanceof Error ? error.message : String(error);
    }
  });
  stopInsets.addEventListener('click', async () => {
    stopInsets.disabled = true;
    if (insetsWatcher !== undefined) await insetsWatcher.close();
    insetsWatcher = undefined;
    startInsets.disabled = false;
  });
  app.append(startInsets, stopInsets);
  const settings = document.createElement('fieldset');
  const settingsLegend = document.createElement('legend');
  settingsLegend.textContent = 'Android notification permission and settings';
  const permissionOutput = document.createElement('pre');
  permissionOutput.id = 'notification-permission-state';
  const settingsOutput = document.createElement('pre');
  settingsOutput.id = 'settings-result';
  settings.append(settingsLegend, permissionOutput, settingsOutput);
  const refreshPermission = async (): Promise<void> => {
    try {
      permissionOutput.textContent =
        androidApi.android.notifications === undefined
          ? 'Notifications are disabled in this application.'
          : JSON.stringify(
              await androidApi.android.notifications.getPermissionState(),
              null,
              2
            );
    } catch (error) {
      permissionOutput.textContent =
        error instanceof Error ? error.message : String(error);
    }
  };
  const settingActions: ReadonlyArray<
    readonly [string, () => Promise<unknown>]
  > = [
    ['Read notification permission', refreshPermission],
    [
      'Request notification permission',
      async () => {
        if (androidApi.android.notifications === undefined)
          throw new Error('Notifications are disabled');
        const result =
          await androidApi.android.notifications.requestPermission();
        await refreshPermission();
        return result;
      },
    ],
    [
      'Check settings availability',
      async () => ({
        'app-details':
          await androidApi.android.settings.isAvailable('app-details'),
        notifications:
          await androidApi.android.settings.isAvailable('notifications'),
      }),
    ],
    [
      'Open app settings',
      async () => await androidApi.android.settings.open('app-details'),
    ],
    [
      'Open notification settings',
      async () =>
        await androidApi.android.settings.open('notifications', {
          fallback: 'app-details',
        }),
    ],
  ];
  for (const [label, action] of settingActions) {
    const control = document.createElement('button');
    control.type = 'button';
    control.textContent = label;
    control.addEventListener('click', async () => {
      control.disabled = true;
      try {
        settingsOutput.textContent =
          JSON.stringify(await action(), null, 2) ?? 'Completed';
      } catch (error) {
        settingsOutput.textContent =
          error instanceof Error ? error.message : String(error);
      } finally {
        control.disabled = false;
      }
    });
    settings.append(control);
  }
  app.append(settings);
  document.addEventListener('visibilitychange', async () => {
    if (document.visibilityState === 'visible') {
      await refreshPermission();
      try {
        barState.textContent = JSON.stringify(
          await androidApi.android.systemBars.getState(),
          null,
          2
        );
      } catch (error) {
        barState.textContent = String(error);
      }
    }
  });
  try {
    insetsOutput.textContent = JSON.stringify(
      await androidApi.android.systemBars.getInsets(),
      null,
      2
    );
  } catch (error) {
    insetsOutput.textContent =
      error instanceof Error ? error.message : String(error);
  }
  window.addEventListener(
    'pagehide',
    () => {
      Reflect.deleteProperty(globalThis, '__muon_android_prototype');
      uninstallNativePluginApi();
      uninstallCapabilityBridge();
      client.dispose();
    },
    { once: true }
  );
}
