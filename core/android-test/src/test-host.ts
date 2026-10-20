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
import type {
  MuonNotificationEvent,
  MuonNotificationWatcher,
} from '../../common/renderer/notifications.js';

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
    'muon.notifications': 'notifications-capability',
  });
  const androidApi = {
    browser: simpleApi.browser,
    environments: simpleApi.environments,
    fs: simpleApi.fs,
    ...(rendererMetadata.builtinFunctions.some((path) =>
      path.startsWith('muon.notifications.')
    )
      ? { notifications: simpleApi.notifications }
      : {}),
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
  const notifications = document.createElement('fieldset');
  const notificationsLegend = document.createElement('legend');
  notificationsLegend.textContent = 'Native local notifications';
  const notificationOutput = document.createElement('pre');
  notificationOutput.id = 'notification-result';
  notifications.append(notificationsLegend, notificationOutput);
  const notificationEvents = document.createElement('pre');
  notificationEvents.id = 'notification-events';
  const notificationRoute = document.createElement('output');
  notificationRoute.id = 'notification-route';
  const receivedEvents = new Map<string, MuonNotificationEvent>();
  let notificationWatcher: MuonNotificationWatcher | undefined;
  const displayEvent = (event: MuonNotificationEvent): void => {
    if (receivedEvents.has(event.eventId)) return;
    receivedEvents.set(event.eventId, event);
    notificationEvents.textContent = JSON.stringify(
      [...receivedEvents.values()],
      null,
      2
    );
    // Routing is idempotent. Actual business mutations should use eventId as
    // their transaction's deduplication key before acknowledging the event.
    notificationRoute.textContent = `Notification ${event.id}: ${event.actionId ?? 'open'} ${JSON.stringify(event.data)}`;
  };
  notifications.append(notificationRoute, notificationEvents);
  const notificationActions: ReadonlyArray<
    readonly [string, () => Promise<unknown>]
  > = [
    [
      'Post notification',
      async () =>
        await androidApi.notifications!.post({
          id: 'driver-order',
          title: 'Muon order',
          body: 'Order is preparing',
          data: { order: 42 },
          actions: [{ id: 'view-order', title: 'View order' }],
        }),
    ],
    [
      'Update notification',
      async () =>
        await androidApi.notifications!.post({
          id: 'driver-order',
          title: 'Muon order updated',
          body: 'Order is ready',
          data: { order: 42, updated: true },
          actions: [{ id: 'view-order', title: 'View updated order' }],
        }),
    ],
    [
      'Post second notification',
      async () =>
        await androidApi.notifications!.post({
          id: 'driver-other',
          title: 'Muon other',
          body: 'Separate notification',
          category: 'quiet',
        }),
    ],
    [
      'Cancel notification',
      async () => await androidApi.notifications!.cancel('driver-order'),
    ],
    [
      'Cancel second notification',
      async () => await androidApi.notifications!.cancel('driver-other'),
    ],
    [
      'Read notification status',
      async () => ({
        app: await androidApi.notifications!.getStatus(),
        general: await androidApi.notifications!.getStatus('general'),
      }),
    ],
    [
      'Read notification capabilities',
      async () => ({
        common: await androidApi.notifications!.getCapabilities(),
        android: await androidApi.android.notifications!.getCapabilities(),
      }),
    ],
    [
      'Read notification channel',
      async () => await androidApi.android.notifications!.getChannel('general'),
    ],
    [
      'Create notification channel',
      async () =>
        await androidApi.android.notifications!.ensureChannel({
          id: 'dynamic',
          name: 'Dynamic notifications',
          description: 'Created explicitly by the driver',
          importance: 'low',
        }),
    ],
    [
      'Post dynamic notification',
      async () =>
        await androidApi.notifications!.post({
          id: 'driver-dynamic',
          title: 'Muon dynamic',
          body: 'Uses an explicitly created channel',
          category: 'dynamic',
        }),
    ],
    [
      'Open channel settings',
      async () =>
        await androidApi.android.settings.open({
          type: 'notification-channel',
          channelId: 'general',
        }),
    ],
    [
      'Read pending notification events',
      async () => {
        const events = await androidApi.notifications!.getPendingEvents();
        for (const event of events) displayEvent(event);
        return events;
      },
    ],
    [
      'Watch notification events',
      async () => {
        if (notificationWatcher === undefined)
          notificationWatcher =
            await androidApi.notifications!.watchEvents(displayEvent);
        // Register first, then read pending events. displayEvent deduplicates
        // events that appear through both paths without consuming either path.
        for (const event of await androidApi.notifications!.getPendingEvents())
          displayEvent(event);
        return 'Watching';
      },
    ],
    [
      'Stop watching notification events',
      async () => {
        await notificationWatcher?.close();
        notificationWatcher = undefined;
        return 'Stopped; pending events are retained';
      },
    ],
    [
      'Acknowledge displayed notification events',
      async () => {
        for (const event of receivedEvents.values())
          await androidApi.notifications!.acknowledgeEvent(event.eventId);
        return await androidApi.notifications!.getPendingEvents();
      },
    ],
  ];
  for (const [label, action] of notificationActions) {
    const control = document.createElement('button');
    control.type = 'button';
    control.textContent = label;
    control.disabled = androidApi.notifications === undefined;
    control.addEventListener('click', async () => {
      control.disabled = true;
      try {
        notificationOutput.textContent =
          JSON.stringify(await action(), null, 2) ?? 'Completed';
      } catch (error) {
        notificationOutput.textContent =
          error instanceof Error ? error.message : String(error);
      } finally {
        control.disabled = false;
      }
    });
    notifications.append(control);
  }
  app.append(notifications);
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
