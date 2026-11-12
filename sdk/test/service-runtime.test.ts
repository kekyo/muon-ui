import { expect, expectTypeOf, it } from 'vitest';
import {
  createServiceRuntime,
  defineService,
  type ServiceRuntimeResult,
} from '../src/service-runtime.ts';
import { defineServiceInterface } from '../src/service-interface.ts';

const deferred = <T>() => {
  let resolve!: (value: T) => void;
  const promise = new Promise<T>((yes) => {
    resolve = yes;
  });
  return { promise, resolve };
};
const definition = defineServiceInterface({
  schemaVersion: 1,
  methods: {
    echo: {
      parameters: ['string', 'uint64', 'bytes'],
      returns: { array: 'string' },
      errors: ['denied'],
    },
    work: {
      parameters: [{ callback: { parameters: ['string'], returns: 'string' } }],
      returns: 'string',
      errors: [],
    },
  },
  events: { progress: 'uint64' },
});
const input = {
  id: 'worker.v1',
  generation: '18446744073709551615',
  config: { hello: '日本語😀' },
  dataDirectory: '/private/worker',
};
const unavailable = async (): Promise<ServiceRuntimeResult> => {
  throw new Error('Unexpected callback');
};

it('maps Unicode, bigint, bytes and typed events through the service interface', async () => {
  const emitted: unknown[] = [];
  let stopped = '';
  const service = defineService({
    interface: definition,
    start: (context) => {
      expect(context.id).toBe(input.id);
      expect(context.generation).toBe(input.generation);
      expect(context.config).toEqual(input.config);
      expect(Object.isFrozen(context.config)).toBe(true);
      return {
        methods: {
          echo: (_call, text, integer, bytes) => {
            expectTypeOf(text).toEqualTypeOf<string>();
            expectTypeOf(integer).toEqualTypeOf<bigint>();
            expectTypeOf(bytes).toEqualTypeOf<Uint8Array>();
            context.emit('progress', integer);
            return [text, String(integer), String(bytes[0])];
          },
          work: async (_call, callback) => await callback('hello'),
        },
        stop: (reason) => {
          stopped = reason;
        },
      };
    },
  });
  const runtime = createServiceRuntime(service, {
    callback: unavailable,
    emit: (event, value) => emitted.push([event, value]),
  });
  await runtime.start(input);
  expect(
    await runtime.invoke('1', 'echo', [
      'A\0日本語😀',
      '18446744073709551615',
      '/w==',
    ])
  ).toEqual({
    status: 'ok',
    value: ['A\0日本語😀', '18446744073709551615', '255'],
  });
  expect(emitted).toEqual([['progress', '18446744073709551615']]);
  const stopping = runtime.stop('released');
  expect(runtime.stop('shutdown')).toBe(stopping);
  await stopping;
  expect(stopped).toBe('released');
  expect(
    await runtime.invoke('2', 'echo', ['late', '1', 'AA=='])
  ).toMatchObject({ status: 'error', code: 'unavailable' });
});

it('permits reentrant calls while a callback is pending and expires cached callbacks', async () => {
  let cached: ((text: string) => string | Promise<string>) | undefined;
  const runtime = createServiceRuntime(
    defineService({
      interface: definition,
      start: () => ({
        methods: {
          echo: (_call, text) => [text],
          work: async (_call, callback) => {
            cached = callback;
            return await callback('inner');
          },
        },
        stop: () => {},
      }),
    }),
    {
      callback: async (callId, token, args) => {
        expect([callId, token, args]).toEqual(['1', '7', ['inner']]);
        expect(
          await runtime.invoke('2', 'echo', ['nested', '1', 'AA=='])
        ).toEqual({ status: 'ok', value: ['nested'] });
        return { status: 'ok', value: 'returned' };
      },
      emit: () => {},
    }
  );
  await runtime.start(input);
  expect(await runtime.invoke('1', 'work', ['7'])).toEqual({
    status: 'ok',
    value: 'returned',
  });
  await expect(cached!('late')).rejects.toMatchObject({ code: 'cancelled' });
  await runtime.stop('released');
});

it('notifies cancellation immediately but drains the real handler and callback before stop', async () => {
  const entered = deferred<void>(),
    releaseHandler = deferred<void>(),
    releaseCallback = deferred<ServiceRuntimeResult>();
  let aborted = false,
    stopped = false,
    callbackFailed = false;
  const runtime = createServiceRuntime(
    defineService({
      interface: definition,
      start: () => ({
        methods: {
          echo: () => [],
          work: async (call, callback) => {
            call.signal.addEventListener(
              'abort',
              () => {
                aborted = true;
              },
              { once: true }
            );
            const result = (async () => {
              try {
                await callback('pending');
              } catch {
                callbackFailed = true;
              }
            })();
            entered.resolve();
            await releaseHandler.promise;
            await result;
            return 'late';
          },
        },
        stop: () => {
          stopped = true;
        },
      }),
    }),
    { callback: async () => await releaseCallback.promise, emit: () => {} }
  );
  await runtime.start(input);
  const result = runtime.invoke('1', 'work', ['9']);
  await entered.promise;
  runtime.cancel('1');
  expect(aborted).toBe(true);
  const stopping = runtime.stop('shutdown');
  releaseHandler.resolve();
  await Promise.resolve();
  expect(stopped).toBe(false);
  releaseCallback.resolve({
    status: 'error',
    code: 'cancelled',
    message: 'owner ended',
  });
  expect(await result).toMatchObject({ status: 'error', code: 'cancelled' });
  await stopping;
  expect(stopped).toBe(true);
  expect(callbackFailed).toBe(true);
});

it('keeps stop pending while an asynchronous start is still acquiring resources', async () => {
  const entered = deferred<void>(),
    release = deferred<void>();
  let stops = 0,
    abort = false;
  const runtime = createServiceRuntime(
    defineService({
      interface: definition,
      start: async (context) => {
        context.signal.addEventListener(
          'abort',
          () => {
            abort = true;
          },
          { once: true }
        );
        entered.resolve();
        await release.promise;
        return {
          methods: { echo: () => [], work: () => '' },
          stop: () => {
            stops++;
          },
        };
      },
    }),
    { callback: unavailable, emit: () => {} }
  );
  const started = runtime.start(input);
  await entered.promise;
  const stopped = runtime.stop('shutdown');
  expect(abort).toBe(true);
  release.resolve();
  await started;
  await stopped;
  expect(stops).toBe(1);
  await expect(runtime.start(input)).rejects.toThrow();
});

it('rejects malformed values, undeclared errors and excess callback concurrency', async () => {
  const release = deferred<ServiceRuntimeResult>(),
    admitted = deferred<void>();
  let calls = 0;
  const runtime = createServiceRuntime(
    defineService({
      interface: definition,
      start: () => ({
        methods: {
          echo: () => {
            throw Object.assign(new Error('undeclared'), { code: 'secret' });
          },
          work: async (_call, callback) => {
            const requests = Array.from({ length: 32 }, () =>
              callback('pending')
            );
            const overflow = (async () => {
              try {
                await callback('overflow');
                return false;
              } catch (error) {
                return (
                  (error as { code: string }).code === 'resource-exhausted'
                );
              }
            })();
            admitted.resolve();
            expect(await overflow).toBe(true);
            await Promise.all(requests);
            return 'drained';
          },
        },
        stop: () => {},
      }),
    }),
    {
      callback: async () => {
        calls++;
        return await release.promise;
      },
      emit: () => {},
    }
  );
  await runtime.start(input);
  expect(await runtime.invoke('1', 'echo', ['x', '01', 'AA=='])).toMatchObject({
    status: 'error',
    code: 'invalid-result',
  });
  expect(await runtime.invoke('2', 'echo', ['x', '1', 'AA=='])).toMatchObject({
    status: 'error',
    code: 'invalid-result',
  });
  const result = runtime.invoke('3', 'work', ['1']);
  await admitted.promise;
  expect(calls).toBe(32);
  release.resolve({ status: 'ok', value: 'ok' });
  expect(await result).toEqual({ status: 'ok', value: 'drained' });
  await runtime.stop('released');
});

it('bounds simultaneous invocations and admits new work after a real completion', async () => {
  const release = deferred<void>();
  const runtime = createServiceRuntime(
    defineService({
      interface: definition,
      start: () => ({
        methods: {
          echo: async () => {
            await release.promise;
            return [];
          },
          work: () => '',
        },
        stop: () => {},
      }),
    }),
    { callback: unavailable, emit: () => {} }
  );
  await runtime.start(input);
  const pending = Array.from({ length: 128 }, (_, index) =>
    runtime.invoke(String(index + 1), 'echo', ['x', '1', 'AA=='])
  );
  expect(await runtime.invoke('129', 'echo', ['x', '1', 'AA=='])).toMatchObject(
    { code: 'resource-exhausted' }
  );
  expect(await runtime.invoke('1', 'echo', ['x', '1', 'AA=='])).toMatchObject({
    code: 'invalid-result',
  });
  release.resolve();
  await Promise.all(pending);
  expect(await runtime.invoke('130', 'echo', ['next', '1', 'AA=='])).toEqual({
    status: 'ok',
    value: [],
  });
  await runtime.stop('released');
});

it('refuses new callbacks after cancellation without completing the pending handler', async () => {
  const entered = deferred<void>(),
    release = deferred<void>();
  let callbackCalls = 0,
    cancelled = false;
  const runtime = createServiceRuntime(
    defineService({
      interface: definition,
      start: () => ({
        methods: {
          echo: () => [],
          work: async (call, callback) => {
            entered.resolve();
            await release.promise;
            expect(call.signal.aborted).toBe(true);
            try {
              await callback('late');
            } catch (error) {
              cancelled = (error as { code: string }).code === 'cancelled';
            }
            return 'ignored';
          },
        },
        stop: () => {},
      }),
    }),
    {
      callback: async () => {
        callbackCalls++;
        return { status: 'ok', value: 'unexpected' };
      },
      emit: () => {},
    }
  );
  await runtime.start(input);
  const result = runtime.invoke('1', 'work', ['1']);
  await entered.promise;
  runtime.cancel('1');
  runtime.cancel('1');
  release.resolve();
  expect(await result).toMatchObject({ status: 'error', code: 'cancelled' });
  expect(callbackCalls).toBe(0);
  expect(cancelled).toBe(true);
  runtime.cancel('1');
  await runtime.stop('released');
});

it('preserves declared errors and checks callback results against their signature', async () => {
  const runtime = createServiceRuntime(
    defineService({
      interface: definition,
      start: () => ({
        methods: {
          echo: () => {
            throw Object.assign(new Error('拒否😀'), { code: 'denied' });
          },
          work: async (_call, callback) => await callback('x'),
        },
        stop: () => {},
      }),
    }),
    { callback: async () => ({ status: 'ok', value: 42 }), emit: () => {} }
  );
  await runtime.start(input);
  expect(await runtime.invoke('1', 'echo', ['x', '1', 'AA=='])).toEqual({
    status: 'error',
    code: 'denied',
    message: '拒否😀',
  });
  expect(await runtime.invoke('2', 'work', ['1'])).toMatchObject({
    status: 'error',
    code: 'invalid-result',
  });
  expect(await runtime.invoke('0', 'work', ['1'])).toMatchObject({
    status: 'error',
    code: 'invalid-result',
  });
  await runtime.stop('released');
});

it('encodes large bytes and rejects aggregate argument overflow before author code', async () => {
  let called = 0;
  const schema = defineServiceInterface({
    schemaVersion: 1,
    methods: {
      copy: { parameters: ['bytes'], returns: 'bytes', errors: [] },
      pair: { parameters: ['string', 'string'], returns: 'void', errors: [] },
    },
    events: {},
  });
  const runtime = createServiceRuntime(
    defineService({
      interface: schema,
      start: () => ({
        methods: {
          copy: (_call, value) => value,
          pair: () => {
            called++;
          },
        },
        stop: () => {},
      }),
    }),
    { callback: unavailable, emit: () => {} }
  );
  await runtime.start(input);
  const bytes = new Uint8Array(1024 * 1024);
  bytes[0] = 255;
  const { encodeServiceValue } = await import('../src/service-values.ts');
  const encoded = encodeServiceValue('bytes', bytes, undefined);
  expect(await runtime.invoke('1', 'copy', [encoded])).toEqual({
    status: 'ok',
    value: encoded,
  });
  expect(
    await runtime.invoke('2', 'pair', ['a'.repeat(700000), 'b'.repeat(700000)])
  ).toMatchObject({ status: 'error', code: 'invalid-result' });
  expect(called).toBe(0);
  await runtime.stop('released');
});

it('rechecks callback admission when argument encoding reenters cancellation', async () => {
  let delivered = 0;
  const schema = defineServiceInterface({
    schemaVersion: 1,
    methods: {
      work: {
        parameters: [
          { callback: { parameters: [{ array: 'int32' }], returns: 'void' } },
        ],
        returns: 'void',
        errors: [],
      },
    },
    events: {},
  });
  const runtime = createServiceRuntime(
    defineService({
      interface: schema,
      start: () => ({
        methods: {
          work: async (_call, callback) => {
            const values = Object.defineProperty([0], '0', {
              get: () => {
                runtime.cancel('1');
                return 7;
              },
            });
            await callback(values);
          },
        },
        stop: () => {},
      }),
    }),
    {
      callback: async () => {
        delivered++;
        return { status: 'ok', value: null };
      },
      emit: () => {},
    }
  );
  await runtime.start(input);
  expect(await runtime.invoke('1', 'work', ['1'])).toMatchObject({
    code: 'cancelled',
  });
  expect(delivered).toBe(0);
  await runtime.stop('released');
});

it('rechecks event admission when value encoding reenters stop', async () => {
  let delivered = 0,
    stopped: Promise<void> | undefined;
  const schema = defineServiceInterface({
    schemaVersion: 1,
    methods: { work: { parameters: [], returns: 'void', errors: [] } },
    events: { value: { array: 'int32' } },
  });
  const runtime = createServiceRuntime(
    defineService({
      interface: schema,
      start: (context) => ({
        methods: {
          work: () => {
            const values = Object.defineProperty([0], '0', {
              get: () => {
                stopped = runtime.stop('released');
                return 7;
              },
            });
            context.emit('value', values);
          },
        },
        stop: () => {},
      }),
    }),
    {
      callback: unavailable,
      emit: () => {
        delivered++;
      },
    }
  );
  await runtime.start(input);
  expect(await runtime.invoke('1', 'work', [])).toMatchObject({
    code: 'cancelled',
  });
  expect(delivered).toBe(0);
  await stopped;
});

it('reports an early method completion only after its outstanding callback drains', async () => {
  const delivered = deferred<void>(),
    release = deferred<ServiceRuntimeResult>();
  let callback: Promise<unknown> | undefined;
  const runtime = createServiceRuntime(
    defineService({
      interface: definition,
      start: () => ({
        methods: {
          echo: () => [],
          work: (_call, notify) => {
            callback = (async () => {
              try {
                return await notify('pending');
              } catch (error) {
                return error;
              }
            })();
            return 'too-early';
          },
        },
        stop: () => {},
      }),
    }),
    {
      callback: async () => {
        delivered.resolve();
        return await release.promise;
      },
      emit: () => {},
    }
  );
  await runtime.start(input);
  const result = runtime.invoke('1', 'work', ['1']);
  await delivered.promise;
  release.resolve({ status: 'ok', value: 'completed' });
  expect(await result).toMatchObject({
    status: 'error',
    code: 'invalid-result',
  });
  await callback;
  await runtime.stop('released');
});

it('rejects duplicate or excessive callback tokens before author code', async () => {
  const schema = defineServiceInterface({
    schemaVersion: 1,
    methods: {
      work: {
        parameters: [
          { array: { callback: { parameters: [], returns: 'void' } } },
        ],
        returns: 'int32',
        errors: [],
      },
    },
    events: {},
  });
  let invoked = 0;
  const runtime = createServiceRuntime(
    defineService({
      interface: schema,
      start: () => ({
        methods: {
          work: (_call, callbacks) => {
            invoked++;
            return callbacks.length;
          },
        },
        stop: () => {},
      }),
    }),
    { callback: unavailable, emit: () => {} }
  );
  await runtime.start(input);
  expect(await runtime.invoke('1', 'work', [['1', '1']])).toMatchObject({
    code: 'invalid-result',
  });
  const tokens = Array.from({ length: 1025 }, (_, index) => String(index + 1));
  expect(await runtime.invoke('2', 'work', [tokens])).toMatchObject({
    code: 'invalid-result',
  });
  expect(invoked).toBe(0);
  expect(await runtime.invoke('3', 'work', [tokens.slice(0, 1024)])).toEqual({
    status: 'ok',
    value: 1024,
  });
  await runtime.stop('released');
});
