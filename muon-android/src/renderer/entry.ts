// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { bootstrapMuonAndroid } from './bootstrap.js';
import { readMuonAndroidRendererMetadata } from './native-plugin-metadata.js';
import type { MuonWebViewJavaScriptBridge } from './webview-rpc.js';

if (window === window.top) {
  const dispose = bootstrapMuonAndroid(
    Reflect.get(globalThis, 'muonAndroidRpc') as MuonWebViewJavaScriptBridge,
    readMuonAndroidRendererMetadata(
      Reflect.get(globalThis, '__muon_android_plugin_metadata')
    ),
    globalThis as unknown as Record<PropertyKey, unknown>
  );
  window.addEventListener('pagehide', dispose, { once: true });
}
