import { describe, expect, expectTypeOf, it } from 'vitest';
import {
  createServiceClient,
  createServiceConnection,
} from '../src/service-connection.ts';
import {
  decodeServiceArguments,
  encodeServiceArguments,
} from '../src/service-values.ts';

const deferred = <T>() => {
  let resolve!: (value: T) => void;
  let reject!: (error: unknown) => void;
  const promise = new Promise<T>((yes, no) => {
    resolve = yes;
    reject = no;
  });
  return { promise, resolve, reject };
};
const fixture = () => {
  const requests: {
    action: string;
    payload: Record<string, unknown>;
    response: ReturnType<typeof deferred<unknown>>;
    taken: boolean;
  }[] = [];
  let changed = deferred<void>();
  const transport = async (
    action: string,
    payload: Readonly<Record<string, unknown>>
  ) => {
    const response = deferred<unknown>();
    requests.push({ action, payload, response, taken: false });
    const previous = changed;
    changed = deferred<void>();
    previous.resolve();
    return await response.promise;
  };
  const next = async (action: string) => {
    for (;;) {
      const request = requests.find(
        (item) => !item.taken && item.action === action
      );
      if (request) {
        request.taken = true;
        return request;
      }
      await changed.promise;
    }
  };
  return { transport, next, requests };
};
const definition = {
  schemaVersion: 1,
  methods: {
    work: {
      parameters: [
        'uint64',
        'bytes',
        { callback: { parameters: ['uint64'], returns: 'uint64' } },
      ],
      returns: 'bytes',
      errors: ['unavailable'],
    },
    ping: { parameters: [], returns: 'void', errors: [] },
  },
  events: { progress: 'uint64' },
} as const;

describe('service connection', () => {
  it.each([
    {},
    {
      aborted: false,
      addEventListener: () => {
        throw new Error('listener registration failed');
      },
      removeEventListener: () => {},
    },
    {
      get aborted(): boolean {
        throw new Error('signal state failed');
      },
      addEventListener: () => {},
      removeEventListener: () => {},
    },
  ])(
    'settles invalid cancellation signals without retaining the call',
    async (signal) => {
      const peer = fixture();
      const connection = createServiceConnection(definition, peer.transport);
      await expect(
        connection.call('ping', [], {
          signal: signal as unknown as AbortSignal,
        })
      ).rejects.toBeInstanceOf(Error);
      await connection.close();
      expect(peer.requests).toHaveLength(0);
    },
    1000
  );

  it('completes cleanup even when a signal rejects listener removal', async () => {
    const peer = fixture();
    const connection = createServiceConnection(definition, peer.transport);
    const signal = {
      aborted: false,
      addEventListener: () => {},
      removeEventListener: () => {
        throw new Error('listener removal failed');
      },
    } as unknown as AbortSignal;
    const call = connection.call('ping', [], { signal });
    (await peer.next('begin')).response.resolve({ callId: '1' });
    (await peer.next('result')).response.resolve({ status: 'ok', value: null });
    (await peer.next('callbackNext')).response.resolve({ done: true });
    await call;
    await connection.close();
  }, 1000);

  it('exposes literal argument and result types through the typed client', async () => {
    const peer = fixture();
    const client = createServiceClient(definition, peer.transport);
    const call = client.call('work', [
      1n,
      new Uint8Array(),
      async (value) => {
        expectTypeOf(value).toEqualTypeOf<bigint>();
        return value;
      },
    ]);
    expectTypeOf(call).toEqualTypeOf<Promise<Uint8Array>>();
    (await peer.next('begin')).response.resolve({ callId: '1' });
    (await peer.next('result')).response.resolve({
      status: 'ok',
      value: 'AQ==',
    });
    (await peer.next('callbackNext')).response.resolve({ done: true });
    expect(await call).toEqual(new Uint8Array([1]));
    await client.close();
  });
  it('copies typed arguments and preserves uint64, binary results and callback scope', async () => {
    const peer = fixture();
    const connection = createServiceConnection(definition, peer.transport);
    const call = connection.call('work', [
      18446744073709551615n,
      new Uint8Array([0, 255]),
      async (value: unknown) => value,
    ]);
    const begin = await peer.next('begin');
    expect(begin.payload).toEqual({
      method: 'work',
      arguments: ['18446744073709551615', 'AP8=', '1'],
    });
    begin.response.resolve({ callId: '9' });
    const result = await peer.next('result');
    const callback = await peer.next('callbackNext');
    callback.response.resolve({
      done: false,
      callbackId: '2',
      token: '1',
      arguments: ['18446744073709551615'],
    });
    const reply = await peer.next('callbackResult');
    expect(reply.payload).toEqual({
      callId: '9',
      callbackId: '2',
      result: { status: 'ok', value: '18446744073709551615' },
    });
    reply.response.resolve(null);
    const done = await peer.next('callbackNext');
    result.response.resolve({ status: 'ok', value: 'AAH/' });
    done.response.resolve({ done: true });
    expect(await call).toEqual(new Uint8Array([0, 1, 255]));
    await connection.close();
  });

  it('rearms callback delivery while a callback awaits a nested service call', async () => {
    const peer = fixture();
    const connection = createServiceConnection(definition, peer.transport);
    const entered = deferred<void>();
    const call = connection.call('work', [
      1n,
      new Uint8Array(),
      async () => {
        entered.resolve();
        await connection.call('ping', []);
        return 3n;
      },
    ]);
    (await peer.next('begin')).response.resolve({ callId: '1' });
    const outerResult = await peer.next('result');
    (await peer.next('callbackNext')).response.resolve({
      done: false,
      callbackId: '1',
      token: '1',
      arguments: ['2'],
    });
    await entered.promise;
    (await peer.next('begin')).response.resolve({ callId: '2' });
    const rearmedOuter = await peer.next('callbackNext');
    expect(rearmedOuter.payload.callId).toBe('1');
    const innerResult = await peer.next('result');
    const innerCallbacks = await peer.next('callbackNext');
    expect(innerCallbacks.payload.callId).toBe('2');
    innerResult.response.resolve({ status: 'ok', value: null });
    innerCallbacks.response.resolve({ done: true });
    const reply = await peer.next('callbackResult');
    expect(reply.payload.result).toEqual({ status: 'ok', value: '3' });
    reply.response.resolve(null);
    outerResult.response.resolve({ status: 'ok', value: '' });
    rearmedOuter.response.resolve({ done: true });
    await call;
    await connection.close();
  });

  it.each(['before', 'after'] as const)(
    'cancels %s begin ACK while internally collecting the result',
    async (order) => {
      const peer = fixture();
      const connection = createServiceConnection(definition, peer.transport);
      const controller = new AbortController();
      const rejected = expect(
        connection.call('ping', [], { signal: controller.signal })
      ).rejects.toMatchObject({ code: 'cancelled' });
      const begin = await peer.next('begin');
      if (order === 'before') controller.abort();
      begin.response.resolve({ callId: '1' });
      const result = await peer.next('result');
      if (order === 'after') controller.abort();
      const cancel = await peer.next('cancel');
      expect(cancel.payload.callId).toBe('1');
      cancel.response.resolve(null);
      await rejected;
      result.response.resolve({
        status: 'error',
        code: 'cancelled',
        message: 'cancelled',
      });
      (await peer.next('callbackNext')).response.resolve({ done: true });
      await connection.close();
    }
  );

  it('close includes a pending begin and does not wait for an unresponsive user callback', async () => {
    const peer = fixture();
    const connection = createServiceConnection(definition, peer.transport);
    const entered = deferred<void>();
    const never = deferred<bigint>();
    const rejected = expect(
      connection.call('work', [
        1n,
        new Uint8Array(),
        async () => {
          entered.resolve();
          return await never.promise;
        },
      ])
    ).rejects.toMatchObject({ code: 'cancelled' });
    (await peer.next('begin')).response.resolve({ callId: '1' });
    const result = await peer.next('result');
    (await peer.next('callbackNext')).response.resolve({
      done: false,
      callbackId: '1',
      token: '1',
      arguments: ['2'],
    });
    await entered.promise;
    const pending = expect(connection.call('ping', [])).rejects.toMatchObject({
      code: 'cancelled',
    });
    const begin = await peer.next('begin');
    const closed = connection.close();
    expect(connection.close()).toBe(closed);
    begin.response.resolve({ callId: '2' });
    (await peer.next('cancel')).response.resolve(null);
    (await peer.next('cancel')).response.resolve(null);
    result.response.resolve({
      status: 'error',
      code: 'cancelled',
      message: '',
    });
    (await peer.next('result')).response.resolve({
      status: 'error',
      code: 'cancelled',
      message: '',
    });
    (await peer.next('callbackNext')).response.resolve({ done: true });
    (await peer.next('callbackNext')).response.resolve({ done: true });
    await Promise.all([closed, rejected, pending]);
    expect(
      peer.requests.filter((item) => item.action === 'callbackResult')
    ).toHaveLength(0);
    never.resolve(1n);
  });

  it('preserves event FIFO and reports overflow without rejecting closed', async () => {
    const peer = fixture();
    const connection = createServiceConnection(definition, peer.transport);
    const values: bigint[] = [];
    const subscribed = connection.subscribe('progress', (value) => {
      values.push(value as bigint);
    });
    (await peer.next('subscribe')).response.resolve({ subscriptionId: '1' });
    const subscription = await subscribed;
    (await peer.next('eventNext')).response.resolve({
      done: false,
      value: '1',
    });
    (await peer.next('eventNext')).response.resolve({
      done: false,
      value: '2',
    });
    (await peer.next('eventNext')).response.resolve({
      done: true,
      error: { code: 'overflow', message: 'queue full' },
    });
    expect(await subscription.closed).toEqual({
      code: 'overflow',
      message: 'queue full',
    });
    expect(values).toEqual([1n, 2n]);
    await connection.close();
  });

  it('reports event transport disconnection and closes an in-flight subscribe', async () => {
    const peer = fixture();
    const connection = createServiceConnection(definition, peer.transport);
    const subscribed = connection.subscribe('progress', () => {});
    (await peer.next('subscribe')).response.resolve({ subscriptionId: '1' });
    const subscription = await subscribed;
    (await peer.next('eventNext')).response.reject(new Error('broken pipe'));
    expect(await subscription.closed).toMatchObject({ code: 'disconnected' });
    const pending = connection.subscribe('progress', () => {});
    const begin = await peer.next('subscribe');
    const closing = connection.close();
    begin.response.resolve({ subscriptionId: '2' });
    (await peer.next('unsubscribe')).response.resolve(null);
    expect(await (await pending).closed).toBeUndefined();
    await closing;
  });

  it('releases a subscription when a malformed event fails decoding', async () => {
    const peer = fixture();
    const connection = createServiceConnection(definition, peer.transport);
    const pending = connection.subscribe('progress', () => {});
    (await peer.next('subscribe')).response.resolve({ subscriptionId: '1' });
    const subscription = await pending;
    (await peer.next('eventNext')).response.resolve({
      done: false,
      value: '01',
    });
    expect(await subscription.closed).toMatchObject({ code: 'invalid-result' });
    (await peer.next('unsubscribe')).response.resolve(null);
    await subscription.close();
    await connection.close();
  });

  it('rejects shared argument byte and node budgets across parameter boundaries', () => {
    const text = 'x'.repeat(600_000);
    expect(() =>
      encodeServiceArguments(['string', 'string'], [text, text], undefined)
    ).toThrow(TypeError);
    expect(() =>
      decodeServiceArguments(['string', 'string'], [text, text], undefined)
    ).toThrow(TypeError);
    expect(() =>
      encodeServiceArguments(
        [{ array: 'uint32' }, { array: 'uint32' }],
        [Array(40_000).fill(0), Array(40_000).fill(0)],
        undefined
      )
    ).toThrow(TypeError);
  });

  it('bounds concurrent callbacks and keeps reading after rejecting excess work', async () => {
    const peer = fixture();
    const connection = createServiceConnection(definition, peer.transport);
    const blocked = deferred<bigint>();
    const call = connection.call('work', [
      1n,
      new Uint8Array(),
      async () => await blocked.promise,
    ]);
    (await peer.next('begin')).response.resolve({ callId: '1' });
    const result = await peer.next('result');
    for (let index = 1; index <= 33; ++index)
      (await peer.next('callbackNext')).response.resolve({
        done: false,
        callbackId: String(index),
        token: '1',
        arguments: ['1'],
      });
    const excess = await peer.next('callbackResult');
    expect(excess.payload.result).toMatchObject({
      status: 'error',
      code: 'resource-exhausted',
    });
    excess.response.resolve(null);
    const next = await peer.next('callbackNext');
    next.response.resolve({ done: true });
    result.response.resolve({ status: 'ok', value: '' });
    await call;
    await connection.close();
    blocked.resolve(0n);
  });

  it.each(['0', '01', '18446744073709551616', 1])(
    'rejects invalid call identity %s before opening channels',
    async (callId) => {
      const peer = fixture();
      const connection = createServiceConnection(definition, peer.transport);
      const call = expect(connection.call('ping', [])).rejects.toMatchObject({
        code: 'invalid-result',
      });
      (await peer.next('begin')).response.resolve({ callId });
      await call;
      await connection.close();
      expect(peer.requests.map((request) => request.action)).toEqual(['begin']);
    }
  );

  it('preserves declared application failures and ignores late abort after success', async () => {
    const peer = fixture();
    const connection = createServiceConnection(definition, peer.transport);
    const controller = new AbortController();
    const call = expect(
      connection.call('work', [1n, new Uint8Array(), () => 1n], {
        signal: controller.signal,
      })
    ).rejects.toMatchObject({ code: 'unavailable', message: 'source missing' });
    (await peer.next('begin')).response.resolve({ callId: '1' });
    (await peer.next('result')).response.resolve({
      status: 'error',
      code: 'unavailable',
      message: 'source missing',
    });
    (await peer.next('callbackNext')).response.resolve({ done: true });
    await call;
    controller.abort();
    await connection.close();
    expect(peer.requests.some((request) => request.action === 'cancel')).toBe(
      false
    );
  });

  it('does not begin an already aborted call or deliver an event after subscription close', async () => {
    const peer = fixture();
    const connection = createServiceConnection(definition, peer.transport);
    const controller = new AbortController();
    controller.abort();
    await expect(
      connection.call('ping', [], { signal: controller.signal })
    ).rejects.toMatchObject({ code: 'cancelled' });
    expect(peer.requests).toHaveLength(0);
    const received: unknown[] = [];
    const pending = connection.subscribe('progress', (value) => {
      received.push(value);
    });
    (await peer.next('subscribe')).response.resolve({ subscriptionId: '1' });
    const subscription = await pending;
    const event = await peer.next('eventNext');
    const closing = subscription.close();
    expect(subscription.close()).toBe(closing);
    event.response.resolve({ done: false, value: '9' });
    (await peer.next('unsubscribe')).response.resolve(null);
    await closing;
    expect(await subscription.closed).toBeUndefined();
    expect(received).toEqual([]);
    await connection.close();
  });
});
