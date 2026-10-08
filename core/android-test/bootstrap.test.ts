import { describe, expect, it } from 'vitest';
import { bootstrapMuonAndroid } from '../android/renderer/bootstrap.js';
import { muonAndroidBuiltinFunctionPaths } from '../android/renderer/android-api.js';
import type { MuonWebViewJavaScriptBridge } from '../android/renderer/webview-rpc.js';

describe('packaged Android renderer', () => {
  it('adapts public validate calls while preserving their capability id', async () => {
    const sent: string[] = [];
    const bridge: MuonWebViewJavaScriptBridge = {
      onmessage: null,
      postMessage: (message) => {
        sent.push(String(message));
      },
    };
    const target: Record<PropertyKey, unknown> = {};
    const dispose = bootstrapMuonAndroid(
      bridge,
      {
        version: 1,
        contextId: 1,
        mode: 'validate',
        namespaces: [],
        functions: [],
        builtinFunctions: ['muon.environments.getConfigValues'],
      },
      target
    );
    expect(target.muon).toBeUndefined();
    const call = target.__muon_plugin_call as (
      id: string,
      path: string,
      args: unknown[]
    ) => Promise<unknown>;
    const result = call(
      'cap-generated',
      'muon.environments.getConfigValues',
      []
    );
    const request = JSON.parse(sent[0]!);
    expect(request.capabilityId).toBe('cap-generated');
    bridge.onmessage?.({
      data: JSON.stringify({
        version: 1,
        type: 'result',
        callId: request.callId,
        success: true,
        value: '{"channel":"android"}',
      }),
    });
    await expect(result).resolves.toEqual({ channel: 'android' });
    dispose();
  });
  it('publishes the basic API before app code and completes calls without QuickJS', async () => {
    const sent: string[] = [];
    const bridge: MuonWebViewJavaScriptBridge = {
      onmessage: null,
      postMessage: (value) => {
        sent.push(String(value));
      },
    };
    const target: Record<PropertyKey, unknown> = {};
    const dispose = bootstrapMuonAndroid(
      bridge,
      {
        version: 1,
        contextId: 1,
        mode: 'simple',
        builtinFunctions: muonAndroidBuiltinFunctionPaths,
        namespaces: [],
        functions: [],
      },
      target
    );
    const api = target.muon as {
      environments: { getConfigValues(): Promise<unknown> };
    };
    expect(Object.keys(api).sort()).toEqual(['browser', 'environments', 'fs']);
    const result = api.environments.getConfigValues();
    const call = JSON.parse(sent[0]!);
    expect(call.functionPath).toBe('muon.environments.getConfigValues');
    bridge.onmessage?.({
      data: JSON.stringify({
        version: 1,
        type: 'result',
        callId: call.callId,
        success: true,
        value: '{"channel":"consumer"}',
      }),
    });
    await expect(result).resolves.toEqual({ channel: 'consumer' });
    dispose();
    expect(target.muon).toBeUndefined();
  });
  it('exposes only the configured built-in functions', () => {
    const target: Record<PropertyKey, unknown> = {};
    const dispose = bootstrapMuonAndroid(
      { onmessage: null, postMessage: () => {} },
      {
        version: 1,
        contextId: 1,
        mode: 'simple',
        namespaces: [],
        functions: [],
        builtinFunctions: ['muon.environments.getConfigValues'],
      },
      target
    );
    expect(Object.keys(target.muon as object)).toEqual(['environments']);
    expect(
      Object.keys((target.muon as { environments: object }).environments)
    ).toEqual(['getConfigValues']);
    dispose();
  });
});
