// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { createMuonAndroidSimpleApi } from './android-api.js';
import { installMuonAndroidNativePluginApi } from './native-plugin-api.js';
import type { MuonAndroidRendererMetadata } from './native-plugin-metadata.js';
import {
  createMuonWebViewRpcClient,
  createMuonWebViewRpcTransport,
  installMuonWebViewCapabilityBridge,
  type MuonWebViewJavaScriptBridge,
} from './webview-rpc.js';

/**
 * Installs the packaged Android API before application scripts execute.
 * @param bridge - Native WebView transport restricted to the trusted origin.
 * @param metadata - Metadata from the native context for this document.
 * @param target - Object receiving the public API and capability bridge.
 * @returns Releases the context and restores previous global properties.
 */
export const bootstrapMuonAndroid = (
  bridge: MuonWebViewJavaScriptBridge,
  metadata: MuonAndroidRendererMetadata,
  target: Record<PropertyKey, unknown>
): (() => void) => {
  const client = createMuonWebViewRpcClient(
    createMuonWebViewRpcTransport(bridge),
    metadata
  );
  const uninstallCapabilities = installMuonWebViewCapabilityBridge(
    client,
    target
  );
  const uninstallApi = installMuonAndroidNativePluginApi(
    client,
    metadata,
    {
      muon: createMuonAndroidSimpleApi(client, {
        'muon.browser': 'browser-capability',
        'muon.environments': 'environment-capability',
        'muon.fs': 'fs-capability',
      }),
    },
    target
  );
  return () => {
    uninstallApi();
    uninstallCapabilities();
    client.dispose();
  };
};
