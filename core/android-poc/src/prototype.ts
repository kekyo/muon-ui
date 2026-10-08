// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import {
  createMuonAndroidJavaScriptRuntimeClient,
  createMuonAndroidJavaScriptRuntimeTransport,
  type MuonAndroidJavaScriptBridge,
} from './android-javascript-runtime.js';
import {
  createMuonWebViewRpcClient,
  createMuonWebViewRpcTransport,
  installMuonWebViewCapabilityBridge,
  type MuonWebViewJavaScriptBridge,
} from '../../android/renderer/webview-rpc.js';
import { createMuonAndroidSimpleApi } from '../../android/renderer/android-api.js';
import { installMuonAndroidNativePluginApi } from '../../android/renderer/native-plugin-api.js';
import { readMuonAndroidRendererMetadata } from '../../android/renderer/native-plugin-metadata.js';

const app = document.querySelector<HTMLElement>('#app');
if (app === null) {
  throw new Error('Prototype root element was not found');
}

const bridge = Reflect.get(globalThis, 'muonAndroidRpc') as
  MuonWebViewJavaScriptBridge | undefined;
const javaScriptRuntimeBridge = Reflect.get(
  globalThis,
  'muonAndroidJavaScriptRuntime'
) as MuonAndroidJavaScriptBridge | undefined;

if (bridge === undefined || javaScriptRuntimeBridge === undefined) {
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
  const javaScriptRuntimeClient = createMuonAndroidJavaScriptRuntimeClient(
    createMuonAndroidJavaScriptRuntimeTransport(javaScriptRuntimeBridge)
  );
  const androidApi = {
    ...createMuonAndroidSimpleApi(client, {
      'muon.browser': 'browser-capability',
      'muon.environments': 'environment-capability',
      'muon.fs': 'fs-capability',
    }),
    node: javaScriptRuntimeClient,
  };
  const uninstallNativePluginApi = installMuonAndroidNativePluginApi(
    client,
    rendererMetadata,
    { muon: androidApi }
  );

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
  window.addEventListener(
    'pagehide',
    () => {
      uninstallNativePluginApi();
      uninstallCapabilityBridge();
      javaScriptRuntimeClient.dispose();
      client.dispose();
    },
    { once: true }
  );
}
