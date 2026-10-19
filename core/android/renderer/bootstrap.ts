// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import {
  createMuonAndroidSimpleApi,
  muonAndroidBuiltinFunctionPaths,
} from './android-api.js';
import { installMuonAndroidNativePluginApi } from './native-plugin-api.js';
import type { MuonAndroidRendererMetadata } from './native-plugin-metadata.js';
import {
  createMuonWebViewRpcClient,
  createMuonWebViewRpcTransport,
  installMuonWebViewCapabilityBridge,
  type MuonWebViewJavaScriptBridge,
  type MuonWebViewRpcClient,
} from './webview-rpc.js';

// Virtual modules use public arguments; platform RPC uses Android wire values.
// Reuse the simple API conversions with the capability supplied by the module.
const createCapabilityClient = (
  client: MuonWebViewRpcClient
): MuonWebViewRpcClient => ({
  ...client,
  call: async (id, path, args, options) => {
    if (
      !(muonAndroidBuiltinFunctionPaths as readonly string[]).includes(path)
    ) {
      return await client.call(id, path, args, options);
    }
    const api = createMuonAndroidSimpleApi(client, {
      'muon.browser': id,
      'muon.environments': id,
      'muon.fs': id,
      'muon.android.systemBars': id,
      'muon.android.settings': id,
      'muon.android.notifications': id,
    });
    let value: unknown = api;
    for (const segment of path.split('.').slice(1)) {
      value = Reflect.get(value as object, segment);
    }
    const call = value as (...args: readonly unknown[]) => Promise<unknown>;
    return await call(...args);
  },
});

const selectAllowedFunctions = (
  source: object,
  prefix: string,
  allowed: ReadonlySet<string>
): Record<string, unknown> => {
  const selected: Record<string, unknown> = {};
  for (const [name, value] of Object.entries(source)) {
    const path = `${prefix}.${name}`;
    if (typeof value === 'function') {
      if (allowed.has(path)) selected[name] = value;
    } else if (typeof value === 'object' && value !== null) {
      const namespace = selectAllowedFunctions(value, path, allowed);
      if (Object.keys(namespace).length > 0) selected[name] = namespace;
    }
  }
  return selected;
};

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
    metadata.mode === 'validate' ? createCapabilityClient(client) : client,
    target
  );
  const api = createMuonAndroidSimpleApi(client, {
    'muon.browser': 'browser-capability',
    'muon.environments': 'environment-capability',
    'muon.fs': 'fs-capability',
    'muon.android.systemBars': 'system-bars-capability',
    'muon.android.settings': 'settings-capability',
    'muon.android.notifications': 'android-notifications-capability',
  });
  const allowed = new Set(metadata.builtinFunctions);
  const builtin = selectAllowedFunctions(api, 'muon', allowed);
  const uninstallApi = installMuonAndroidNativePluginApi(
    client,
    metadata,
    Object.keys(builtin).length === 0 ? {} : { muon: builtin },
    target
  );
  return () => {
    uninstallApi();
    uninstallCapabilities();
    client.dispose();
  };
};
