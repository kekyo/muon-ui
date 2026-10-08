// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { describe, expect, it } from 'vitest';

import {
  initializeBridgePeer,
  requireSuccessValue,
  type BridgePeer,
  type WireCallbackRequest,
  type WireMessage,
  type WireResponse,
} from './support/bridge-peer.js';
import { createProjectFixture } from './support/project-fixture.js';

interface ImportedModule {
  moduleId: string;
}

interface JsonWireValue {
  kind: 'json';
  value: unknown;
}

interface MessageObservation {
  kind: 'message';
  message: WireMessage;
}

interface CompletionObservation {
  kind: 'completed';
}

interface EventLoopBarrier {
  kind: 'eventLoopBarrier';
}

const importBackend = async (peer: BridgePeer): Promise<ImportedModule> =>
  requireSuccessValue(
    await peer.request('importModule', {
      specifier: './backend.js',
    })
  ) as ImportedModule;

const call = async (
  peer: BridgePeer,
  moduleId: string,
  exportName: string,
  argumentsValue: readonly unknown[]
) =>
  await peer.request('call', {
    moduleId,
    exportName,
    arguments: argumentsValue,
  });

const jsonWireValue = (value: unknown): JsonWireValue => ({
  kind: 'json',
  value,
});

const observeMessage = async (
  operation: Promise<WireMessage>
): Promise<MessageObservation> => ({
  kind: 'message',
  message: await operation,
});

const observeCompletion = async (
  operation: Promise<void>
): Promise<CompletionObservation> => {
  await operation;
  return {
    kind: 'completed',
  };
};

// This is an event-loop ordering barrier, not a wall-clock deadline. Protocol
// work triggered by the preceding in-memory write must complete before it.
const reachNextEventLoopTurn = async (): Promise<EventLoopBarrier> => {
  await new Promise<void>((resolvePromise) => {
    setImmediate(() => {
      resolvePromise();
    });
  });
  return {
    kind: 'eventLoopBarrier',
  };
};

const findResponse = (
  messages: readonly WireMessage[],
  id: string
): WireResponse | undefined =>
  messages.find(
    (message): message is WireResponse =>
      message.kind === 'response' && message.id === id
  );

describe('muon Node wire values', () => {
  it('round-trips supported primitive values', async () => {
    const project = await createProjectFixture();
    const peer = await initializeBridgePeer(project.root);
    try {
      const backend = await importBackend(peer);
      const values = [null, false, true, 0, -42.5, 'text'] as const;
      for (const value of values) {
        expect(
          requireSuccessValue(
            await call(peer, backend.moduleId, 'echo', [value])
          )
        ).toBe(value);
      }
      const undefinedValue = {
        kind: 'undefined',
      };
      expect(
        requireSuccessValue(
          await call(peer, backend.moduleId, 'echo', [undefinedValue])
        )
      ).toEqual(undefinedValue);
    } finally {
      await peer.close();
      await project.dispose();
    }
  });

  it('round-trips signed and unsigned 64-bit tagged values', async () => {
    const project = await createProjectFixture();
    const peer = await initializeBridgePeer(project.root);
    try {
      const backend = await importBackend(peer);
      const signed = {
        kind: 'i64',
        value: '-9223372036854775808',
      };
      const unsigned = {
        kind: 'u64',
        value: '18446744073709551615',
      };

      expect(
        requireSuccessValue(
          await call(peer, backend.moduleId, 'echoI64', [signed])
        )
      ).toEqual(signed);
      expect(
        requireSuccessValue(
          await call(peer, backend.moduleId, 'echoU64', [unsigned])
        )
      ).toEqual(unsigned);
    } finally {
      await peer.close();
      await project.dispose();
    }
  });

  it('copies buffer values through a base64 tagged frame value', async () => {
    const project = await createProjectFixture();
    const peer = await initializeBridgePeer(project.root);
    try {
      const backend = await importBackend(peer);
      const buffer = {
        kind: 'buffer',
        data: Buffer.from([0, 1, 2, 127, 128, 255]).toString('base64'),
      };

      expect(
        requireSuccessValue(
          await call(peer, backend.moduleId, 'copyBuffer', [buffer])
        )
      ).toEqual(buffer);
    } finally {
      await peer.close();
      await project.dispose();
    }
  });

  it('copies Uint8Array and other ArrayBuffer view results within their view bounds', async () => {
    const project = await createProjectFixture();
    const peer = await initializeBridgePeer(project.root);
    try {
      const backend = await importBackend(peer);

      expect(
        requireSuccessValue(
          await call(peer, backend.moduleId, 'returnUint8Array', [])
        )
      ).toEqual({
        kind: 'buffer',
        data: Buffer.from([0, 1, 2]).toString('base64'),
      });
      expect(
        requireSuccessValue(
          await call(peer, backend.moduleId, 'returnDataView', [])
        )
      ).toEqual({
        kind: 'buffer',
        data: Buffer.from([3, 4, 5]).toString('base64'),
      });
    } finally {
      await peer.close();
      await project.dispose();
    }
  });

  it('copies ArrayBuffer results created in another JavaScript realm', async () => {
    const project = await createProjectFixture();
    const peer = await initializeBridgePeer(project.root);
    try {
      const backend = await importBackend(peer);

      expect(
        requireSuccessValue(
          await call(peer, backend.moduleId, 'returnCrossRealmArrayBuffer', [])
        )
      ).toEqual({
        kind: 'buffer',
        data: Buffer.from([0, 1, 127, 255]).toString('base64'),
      });
    } finally {
      await peer.close();
      await project.dispose();
    }
  });

  it('rejects objects that imitate ArrayBuffer instances', async () => {
    const project = await createProjectFixture();
    const peer = await initializeBridgePeer(project.root);
    try {
      const backend = await importBackend(peer);

      await expect(
        call(peer, backend.moduleId, 'returnFakeArrayBuffer', [])
      ).resolves.toMatchObject({
        ok: false,
        error: {
          code: 'ERR_MUON_NODE_UNSUPPORTED_VALUE',
        },
      });
    } finally {
      await peer.close();
      await project.dispose();
    }
  });

  it('rejects oversized binary and JSON responses without closing the bridge', async () => {
    const project = await createProjectFixture();
    const peer = await initializeBridgePeer(project.root);
    try {
      const backend = await importBackend(peer);

      for (const exportName of [
        'returnOversizedBuffer',
        'returnOversizedJsonValue',
      ]) {
        await expect(
          call(peer, backend.moduleId, exportName, [])
        ).resolves.toMatchObject({
          ok: false,
          error: {
            code: 'ERR_MUON_NODE_FRAME_TOO_LARGE',
          },
        });
      }
      expect(
        requireSuccessValue(
          await call(peer, backend.moduleId, 'echo', ['still-ready'])
        )
      ).toBe('still-ready');
    } finally {
      await peer.close();
      await project.dispose();
    }
  });

  it('invokes and awaits a remote function callback', async () => {
    const project = await createProjectFixture();
    const peer = await initializeBridgePeer(project.root);
    try {
      const backend = await importBackend(peer);
      const callId = peer.sendRequest('call', {
        moduleId: backend.moduleId,
        exportName: 'invokeCallback',
        arguments: [
          {
            kind: 'function',
            handle: 'renderer-callback-1',
          },
          'callback input',
        ],
      });

      const callbackMessage = await peer.nextMessage();
      expect(callbackMessage).toMatchObject({
        kind: 'callback',
        id: expect.any(String),
        handle: 'renderer-callback-1',
        arguments: ['callback input'],
      });
      const callback = callbackMessage as WireCallbackRequest;
      peer.send({
        kind: 'callbackResult',
        id: callback.id,
        ok: true,
        value: 'callback result',
      });

      expect(requireSuccessValue(await peer.nextResponse(callId))).toBe(
        'callback result'
      );
    } finally {
      await peer.close();
      await project.dispose();
    }
  });

  it('copies strict JSON values through a remote function callback', async () => {
    const project = await createProjectFixture();
    const peer = await initializeBridgePeer(project.root);
    try {
      const backend = await importBackend(peer);
      const argument = jsonWireValue({
        nested: {
          source: 'renderer',
        },
        values: [1, true, null],
      });
      const result = jsonWireValue({
        nested: {
          source: 'renderer-callback',
        },
        values: ['callback', false, null],
      });
      const callId = peer.sendRequest('call', {
        moduleId: backend.moduleId,
        exportName: 'invokeCallback',
        arguments: [
          {
            kind: 'function',
            handle: 'renderer-json-callback',
          },
          argument,
        ],
      });

      const callbackMessage = await peer.nextMessage();
      expect(callbackMessage).toMatchObject({
        kind: 'callback',
        id: expect.any(String),
        handle: 'renderer-json-callback',
        arguments: [argument],
      });
      const callback = callbackMessage as WireCallbackRequest;
      peer.send({
        kind: 'callbackResult',
        id: callback.id,
        ok: true,
        value: result,
      });

      expect(requireSuccessValue(await peer.nextResponse(callId))).toEqual(
        result
      );
    } finally {
      await peer.close();
      await project.dispose();
    }
  });

  it('allows a renderer callback to await another Node call', async () => {
    const project = await createProjectFixture();
    const peer = await initializeBridgePeer(project.root);
    try {
      const backend = await importBackend(peer);
      const outerCallId = peer.sendRequest('call', {
        moduleId: backend.moduleId,
        exportName: 'invokeCallback',
        arguments: [
          {
            kind: 'function',
            handle: 'renderer-reentrant-callback',
          },
          'outer input',
        ],
      });

      const callbackMessage = await peer.nextMessage();
      expect(callbackMessage).toMatchObject({
        kind: 'callback',
        id: expect.any(String),
        handle: 'renderer-reentrant-callback',
        arguments: ['outer input'],
      });
      const callback = callbackMessage as WireCallbackRequest;

      const nestedCallId = peer.sendRequest('call', {
        moduleId: backend.moduleId,
        exportName: 'echo',
        arguments: ['nested result'],
      });
      const nextMessageOperation = peer.nextMessage();
      const observation = await Promise.race([
        observeMessage(nextMessageOperation),
        reachNextEventLoopTurn(),
      ]);

      peer.send({
        kind: 'callbackResult',
        id: callback.id,
        ok: true,
        value: 'outer result',
      });

      const messages: WireMessage[] = [];
      if (observation.kind === 'message') {
        messages.push(observation.message);
      } else {
        messages.push(await nextMessageOperation);
      }
      while (
        findResponse(messages, nestedCallId) === undefined ||
        findResponse(messages, outerCallId) === undefined
      ) {
        messages.push(await peer.nextMessage());
      }

      expect(observation).toMatchObject({
        kind: 'message',
        message: {
          kind: 'response',
          id: nestedCallId,
          ok: true,
          value: 'nested result',
        },
      });
      expect(findResponse(messages, outerCallId)).toMatchObject({
        ok: true,
        value: 'outer result',
      });
    } finally {
      await peer.close();
      await project.dispose();
    }
  });

  it('closes without waiting for a pending Node call', async () => {
    const project = await createProjectFixture();
    const peer = await initializeBridgePeer(project.root);
    try {
      const backend = await importBackend(peer);
      peer.sendRequest('call', {
        moduleId: backend.moduleId,
        exportName: 'invokeCallback',
        arguments: [
          {
            kind: 'function',
            handle: 'renderer-pending-close',
          },
          'pending close',
        ],
      });
      await expect(peer.nextMessage()).resolves.toMatchObject({
        kind: 'callback',
        handle: 'renderer-pending-close',
        arguments: ['pending close'],
      });

      const closeOperation = peer.close();
      const observation = await Promise.race([
        observeCompletion(closeOperation),
        reachNextEventLoopTurn(),
      ]);

      await closeOperation;
      expect(observation).toEqual({
        kind: 'completed',
      });
    } finally {
      await peer.close();
      await project.dispose();
    }
  });

  it('acknowledges shutdown without waiting for a pending Node call', async () => {
    const project = await createProjectFixture();
    const peer = await initializeBridgePeer(project.root);
    try {
      const backend = await importBackend(peer);
      peer.sendRequest('call', {
        moduleId: backend.moduleId,
        exportName: 'invokeCallback',
        arguments: [
          {
            kind: 'function',
            handle: 'renderer-pending-shutdown',
          },
          'pending shutdown',
        ],
      });
      await expect(peer.nextMessage()).resolves.toMatchObject({
        kind: 'callback',
        handle: 'renderer-pending-shutdown',
        arguments: ['pending shutdown'],
      });

      const shutdownId = peer.sendRequest('shutdown', {});
      const nextMessageOperation = peer.nextMessage();
      const observation = await Promise.race([
        observeMessage(nextMessageOperation),
        reachNextEventLoopTurn(),
      ]);

      const messages: WireMessage[] = [];
      if (observation.kind === 'message') {
        messages.push(observation.message);
      } else {
        messages.push(await nextMessageOperation);
      }
      while (findResponse(messages, shutdownId) === undefined) {
        messages.push(await peer.nextMessage());
      }

      expect(observation).toMatchObject({
        kind: 'message',
        message: {
          kind: 'response',
          id: shutdownId,
          ok: true,
          value: {
            shutdown: true,
          },
        },
      });
    } finally {
      await peer.close();
      await project.dispose();
    }
  });

  it('copies strict JSON objects and arrays in arguments and results', async () => {
    const project = await createProjectFixture();
    const peer = await initializeBridgePeer(project.root);
    try {
      const backend = await importBackend(peer);
      const argument = jsonWireValue({
        nested: {
          enabled: true,
          metadata: {
            name: 'muon',
          },
        },
        values: [1, -42.5, 'text', false, null, ['nested']],
      });

      expect(
        requireSuccessValue(
          await call(peer, backend.moduleId, 'echo', [argument])
        )
      ).toEqual(argument);
      expect(
        requireSuccessValue(
          await call(peer, backend.moduleId, 'returnObject', [])
        )
      ).toEqual(
        jsonWireValue({
          nested: {
            enabled: true,
          },
          values: [1, 'two', null, { leaf: false }],
        })
      );
      expect(
        requireSuccessValue(
          await call(peer, backend.moduleId, 'returnNullPrototypeObject', [])
        )
      ).toEqual(
        jsonWireValue({
          supported: true,
        })
      );
    } finally {
      await peer.close();
      await project.dispose();
    }
  });

  it('copies a top-level JSON array through an argument and result', async () => {
    const project = await createProjectFixture();
    const peer = await initializeBridgePeer(project.root);
    try {
      const backend = await importBackend(peer);
      const value = jsonWireValue([
        'root',
        {
          nested: [1, true, null],
        },
        ['leaf'],
      ]);

      expect(
        requireSuccessValue(await call(peer, backend.moduleId, 'echo', [value]))
      ).toEqual(value);
    } finally {
      await peer.close();
      await project.dispose();
    }
  });

  it('rejects raw aggregates and malformed JSON envelopes without closing the bridge', async () => {
    const project = await createProjectFixture();
    const peer = await initializeBridgePeer(project.root);
    try {
      const backend = await importBackend(peer);
      const invalidValues = [
        {
          raw: 'object',
        },
        ['raw-array'],
        {
          kind: 'json',
        },
        {
          kind: 'json',
          value: null,
        },
        {
          kind: 'json',
          value: 'not-an-aggregate',
        },
      ] as const;

      for (const invalidValue of invalidValues) {
        await expect(
          call(peer, backend.moduleId, 'echo', [invalidValue])
        ).resolves.toMatchObject({
          ok: false,
          error: {
            code: 'ERR_MUON_NODE_UNSUPPORTED_VALUE',
          },
        });
      }
      expect(
        requireSuccessValue(
          await call(peer, backend.moduleId, 'echo', ['still-ready'])
        )
      ).toBe('still-ready');
    } finally {
      await peer.close();
      await project.dispose();
    }
  });

  it('copies JSON values before Node mutates them', async () => {
    const project = await createProjectFixture();
    const peer = await initializeBridgePeer(project.root);
    try {
      const backend = await importBackend(peer);
      const sourceValue = {
        changed: false,
        values: ['renderer'],
      };
      const argument = jsonWireValue(sourceValue);

      expect(
        requireSuccessValue(
          await call(peer, backend.moduleId, 'mutateJsonValue', [argument])
        )
      ).toEqual(
        jsonWireValue({
          changed: true,
          values: ['renderer', 'node'],
        })
      );
      expect(sourceValue).toEqual({
        changed: false,
        values: ['renderer'],
      });
    } finally {
      await peer.close();
      await project.dispose();
    }
  });

  it('copies repeated non-cyclic references as independent JSON values', async () => {
    const project = await createProjectFixture();
    const peer = await initializeBridgePeer(project.root);
    try {
      const backend = await importBackend(peer);

      const result = requireSuccessValue(
        await call(peer, backend.moduleId, 'returnSharedJsonValue', [])
      ) as JsonWireValue;
      expect(result).toEqual(
        jsonWireValue({
          first: {
            source: 'shared',
          },
          second: {
            source: 'shared',
          },
        })
      );
      const resultValue = result.value as {
        first: unknown;
        second: unknown;
      };
      expect(resultValue.first).not.toBe(resultValue.second);
    } finally {
      await peer.close();
      await project.dispose();
    }
  });

  it('treats reserved wire tag shapes inside JSON values as user data', async () => {
    const project = await createProjectFixture();
    const peer = await initializeBridgePeer(project.root);
    try {
      const backend = await importBackend(peer);
      const value = jsonWireValue({
        values: [
          {
            kind: 'i64',
            value: '1',
          },
          {
            kind: 'buffer',
            data: 'AQID',
          },
          {
            kind: 'function',
            handle: 'not-a-callback',
          },
          {
            kind: 'json',
            value: {
              nested: true,
            },
          },
        ],
      });

      expect(
        requireSuccessValue(await call(peer, backend.moduleId, 'echo', [value]))
      ).toEqual(value);
    } finally {
      await peer.close();
      await project.dispose();
    }
  });

  it('preserves a __proto__ data property without prototype pollution', async () => {
    const project = await createProjectFixture();
    const peer = await initializeBridgePeer(project.root);
    try {
      const backend = await importBackend(peer);
      const value = JSON.parse(
        '{"__proto__":{"muonNodePolluted":true},"safe":true}'
      ) as unknown;

      expect(
        requireSuccessValue(
          await call(peer, backend.moduleId, 'inspectPrototypeProperty', [
            jsonWireValue(value),
          ])
        )
      ).toEqual(
        jsonWireValue({
          hasOwnPrototypeProperty: true,
          prototypeProperty: {
            muonNodePolluted: true,
          },
          objectPrototypePolluted: false,
        })
      );
      expect(
        (Object.prototype as { muonNodePolluted?: boolean }).muonNodePolluted
      ).toBeUndefined();
    } finally {
      await peer.close();
      await project.dispose();
    }
  });

  it('rejects values outside the strict JSON subset without closing the bridge', async () => {
    const project = await createProjectFixture();
    const peer = await initializeBridgePeer(project.root);
    try {
      const backend = await importBackend(peer);
      const invalidKinds = [
        'cycle',
        'sparse-array',
        'array-property',
        'custom-prototype',
        'date',
        'map',
        'set',
        'accessor',
        'symbol-property',
        'non-enumerable',
        'nested-undefined',
        'nested-bigint',
        'nested-buffer',
        'nested-function',
        'nested-symbol',
        'nan',
        'infinity',
        'negative-zero',
        'proxy',
      ] as const;

      for (const invalidKind of invalidKinds) {
        await expect(
          call(peer, backend.moduleId, 'returnInvalidJsonValue', [invalidKind])
        ).resolves.toMatchObject({
          ok: false,
          error: {
            code: 'ERR_MUON_NODE_UNSUPPORTED_VALUE',
          },
        });
      }
      expect(
        requireSuccessValue(
          await call(peer, backend.moduleId, 'echo', ['still-ready'])
        )
      ).toBe('still-ready');
    } finally {
      await peer.close();
      await project.dispose();
    }
  });

  it('invalidates a module handle after release', async () => {
    const project = await createProjectFixture();
    const peer = await initializeBridgePeer(project.root);
    try {
      const backend = await importBackend(peer);

      await expect(
        peer.request('release', {
          kind: 'module',
          handle: backend.moduleId,
        })
      ).resolves.toMatchObject({
        ok: true,
        value: {
          released: true,
        },
      });
      await expect(
        call(peer, backend.moduleId, 'echo', ['after release'])
      ).resolves.toMatchObject({
        ok: false,
        error: {
          code: 'ERR_MUON_NODE_RELEASED_HANDLE',
        },
      });
    } finally {
      await peer.close();
      await project.dispose();
    }
  });
});
