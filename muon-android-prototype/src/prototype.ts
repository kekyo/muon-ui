// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import {
  createMuonWebViewRpcClient,
  createMuonWebViewRpcTransport,
  installMuonWebViewCapabilityBridge,
  type MuonWebViewJavaScriptBridge,
} from './webview-rpc.js';
import { createMuonAndroidSimpleApi } from './android-api.js';

/** Testable prototype operations that require direct RPC client controls. */
export interface MuonAndroidPrototypeOperations {
  /** Starts and immediately aborts a delayed native call. */
  readonly cancelDelayed: () => Promise<unknown>;
}

const app = document.querySelector<HTMLElement>('#app');
if (app === null) {
  throw new Error('Prototype root element was not found');
}

const bridge = Reflect.get(globalThis, 'muonAndroidRpc') as
  | MuonWebViewJavaScriptBridge
  | undefined;

if (bridge === undefined) {
  app.textContent = 'muon Android RPC bridge is unavailable';
} else {
  const client = createMuonWebViewRpcClient(
    createMuonWebViewRpcTransport(bridge)
  );
  const uninstallCapabilityBridge = installMuonWebViewCapabilityBridge(client);
  const previousMuon = Object.getOwnPropertyDescriptor(globalThis, 'muon');
  Object.defineProperty(globalThis, 'muon', {
    configurable: true,
    enumerable: true,
    writable: false,
    value: createMuonAndroidSimpleApi(client, {
      'muon.browser': 'browser-capability',
      'muon.environments': 'environment-capability',
    }),
  });
  const operations: MuonAndroidPrototypeOperations = {
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
  window.addEventListener(
    'pagehide',
    () => {
      Reflect.deleteProperty(globalThis, '__muon_android_prototype');
      if (previousMuon === undefined) {
        Reflect.deleteProperty(globalThis, 'muon');
      } else {
        Object.defineProperty(globalThis, 'muon', previousMuon);
      }
      uninstallCapabilityBridge();
      client.dispose();
    },
    { once: true }
  );
}
