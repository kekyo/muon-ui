import { expect, it, vi } from 'vitest';
import { defineServiceInterface } from '../src/service-interface.ts';
import { bindServiceClient } from '../src/service-binding.ts';

const definition = defineServiceInterface({
  schemaVersion: 1,
  methods: {
    echo: {
      parameters: ['uint64'],
      returns: 'uint64',
      errors: ['busy', 'missing'],
    },
  },
  events: { tick: 'int32' },
});

it('binds a compatible page handle without acquiring or releasing another reference', async () => {
  const call = vi.fn(async () => 42n);
  const close = vi.fn(async () => {});
  const subscribe = vi.fn(async () => ({
    close,
    closed: Promise.resolve(undefined),
  }));
  const reference = {
    interface: {
      ...definition,
      methods: {
        ...definition.methods,
        echo: { ...definition.methods.echo, errors: ['missing', 'busy'] },
        extra: { parameters: [], returns: 'void', errors: [] },
      },
    },
    call,
    subscribe,
  };
  const client = bindServiceClient(definition, reference);
  const result: bigint = await client.call('echo', [42n]);
  expect(result).toBe(42n);
  expect(call).toHaveBeenCalledExactlyOnceWith('echo', [42n], undefined);
  const listener = vi.fn();
  const subscription = await client.subscribe('tick', listener);
  expect(subscribe).toHaveBeenCalledExactlyOnceWith('tick', listener);
  await subscription.close();
  expect(close).toHaveBeenCalledOnce();
  expect(Object.keys(client).sort()).toEqual(['call', 'subscribe']);
});

it.each([
  { ...definition, methods: {} },
  {
    ...definition,
    methods: { echo: { ...definition.methods.echo, returns: 'string' } },
  },
  {
    ...definition,
    methods: { echo: { ...definition.methods.echo, errors: [] } },
  },
  { ...definition, events: { tick: 'string' } },
])(
  'rejects incompatible runtime definitions before invoking a method',
  (runtime) => {
    const call = vi.fn();
    const subscribe = vi.fn();
    expect(() =>
      bindServiceClient(definition, { interface: runtime, call, subscribe })
    ).toThrow('does not match');
    expect(call).not.toHaveBeenCalled();
    expect(subscribe).not.toHaveBeenCalled();
  }
);
