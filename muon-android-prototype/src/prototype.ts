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
  installMuonWebViewCapabilityBridge(client);

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
      client.dispose();
    },
    { once: true }
  );
}
