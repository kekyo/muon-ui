import { describe, expect, it } from 'vitest';
import { bootstrapMuonAndroid } from '../src/renderer/bootstrap.js';
import type { MuonWebViewJavaScriptBridge } from '../src/renderer/webview-rpc.js';

describe('packaged Android renderer', () => {
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
});
