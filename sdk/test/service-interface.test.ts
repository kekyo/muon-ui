import { describe, expect, it } from 'vitest';
import {
  defineServiceInterface,
  parseServiceInterface,
} from '../src/service-interface.ts';
import {
  decodeServiceValue,
  encodeServiceValue,
} from '../src/service-values.ts';

const contract = {
  schemaVersion: 1,
  methods: {
    collect: {
      parameters: [
        'uint64',
        { callback: { parameters: ['string'], returns: 'boolean' } },
      ],
      returns: { array: 'bytes' },
      errors: ['source_missing'],
    },
  },
  events: { progress: 'uint32' },
} as const;

describe('service interface metadata', () => {
  it('retains a detached immutable contract with typed methods and events', () => {
    const input = structuredClone(contract);
    const value = defineServiceInterface(input);
    expect(value).toEqual(contract);
    expect(Object.isFrozen(value.methods.collect.parameters)).toBe(true);
    Reflect.set(input.events, 'progress', 'string');
    expect(value.events.progress).toBe('uint32');
  });

  it.each([
    { ...contract, schemaVersion: 2 },
    { ...contract, extra: true },
    {
      ...contract,
      methods: { bad: { parameters: ['void'], returns: 'void', errors: [] } },
    },
    {
      ...contract,
      methods: { bad: { parameters: [], returns: 'pointer', errors: [] } },
    },
    {
      ...contract,
      methods: {
        bad: {
          parameters: [],
          returns: { callback: { parameters: [], returns: 'void' } },
          errors: [],
        },
      },
    },
    { ...contract, events: { bad: { array: 'void' } } },
    {
      ...contract,
      events: { bad: { callback: { parameters: [], returns: 'void' } } },
    },
    {
      ...contract,
      methods: {
        bad: {
          parameters: Array(33).fill('string'),
          returns: 'void',
          errors: [],
        },
      },
    },
    {
      ...contract,
      methods: {
        bad: {
          parameters: [],
          returns: 'void',
          errors: ['duplicate', 'duplicate'],
        },
      },
    },
    { ...contract, methods: { 'invalid/name': contract.methods.collect } },
  ])('rejects unsupported or ambiguous definitions', (value) => {
    expect(() => parseServiceInterface(value)).toThrow(TypeError);
  });

  it('rejects nested callback signatures while allowing callbacks in method input arrays', () => {
    const callback = { callback: { parameters: [], returns: 'void' } } as const;
    expect(() =>
      parseServiceInterface({
        ...contract,
        methods: {
          call: {
            parameters: [{ array: callback }],
            returns: 'void',
            errors: [],
          },
        },
      })
    ).not.toThrow();
    expect(() =>
      parseServiceInterface({
        ...contract,
        methods: {
          call: {
            parameters: [
              { callback: { parameters: [callback], returns: 'void' } },
            ],
            returns: 'void',
            errors: [],
          },
        },
      })
    ).toThrow(TypeError);
  });

  it('bounds each array payload independently of callback signatures', () => {
    let input: unknown = {
      callback: { parameters: ['string'], returns: 'void' },
    };
    for (let depth = 0; depth < 16; ++depth) input = { array: input };
    const definition = {
      ...contract,
      methods: {
        call: { parameters: [input], returns: 'void', errors: [] },
      },
    };
    expect(() => parseServiceInterface(definition)).not.toThrow();
    definition.methods.call.parameters = [{ array: input }];
    expect(() => parseServiceInterface(definition)).toThrow(TypeError);
  });
});

describe('portable service values', () => {
  it('preserves integer limits, negative zero, subnormal numbers and binary data through JSON', () => {
    const cases = [
      ['int64', -(1n << 63n)],
      ['uint64', (1n << 64n) - 1n],
      ['float64', -0],
      ['float64', Number.MIN_VALUE],
      ['float64', Number.MAX_VALUE],
      ['int32', -2147483648],
      ['uint32', 4294967295],
      ['boolean', true],
      ['string', '日本語\0😀'],
      ['bytes', new Uint8Array([0, 1, 127, 128, 255])],
      [{ array: 'uint64' }, [0n, (1n << 64n) - 1n]],
      ['void', undefined],
    ] as const;
    for (const [type, value] of cases) {
      const wire = JSON.parse(
        JSON.stringify(encodeServiceValue(type, value, undefined))
      );
      expect(decodeServiceValue(type, wire, undefined)).toEqual(value);
    }
  });

  it('retains callback identities through the owning invocation only', () => {
    const type = {
      callback: { parameters: ['string'], returns: 'boolean' },
    } as const;
    const callback = () => true;
    const wire = encodeServiceValue(type, callback, (value, signature) => {
      expect(value).toBe(callback);
      expect(signature).toEqual(type.callback);
      return '18446744073709551615';
    });
    expect(
      decodeServiceValue(type, wire, (token) => {
        expect(token).toBe('18446744073709551615');
        return callback;
      })
    ).toBe(callback);
    expect(() => encodeServiceValue(type, callback, undefined)).toThrow(
      TypeError
    );
    expect(() => decodeServiceValue(type, wire, undefined)).toThrow(TypeError);
  });

  it.each([
    ['int32', 2147483648],
    ['uint32', -1],
    ['int64', 1n << 63n],
    ['uint64', -1n],
    ['int64', 10],
    ['float64', NaN],
    ['float64', Infinity],
    ['boolean', 1],
    ['string', '\ud800'],
    ['bytes', [1, 2]],
    ['void', null],
  ] as const)('rejects invalid outgoing %s values', (type, value) => {
    expect(() => encodeServiceValue(type, value, undefined)).toThrow(TypeError);
  });

  it.each([
    ['int64', '01'],
    ['int64', '-0'],
    ['uint64', '18446744073709551616'],
    ['float64', 'Infinity'],
    ['float64', ' 1'],
    ['float64', 1],
    ['float64', '1e-999'],
    ['bytes', 'AB=='],
    ['bytes', 'AAB='],
    ['bytes', 'YQ'],
    ['bytes', '===='],
    ['boolean', 'true'],
    ['string', '\udfff'],
    ['void', false],
  ] as const)('rejects invalid incoming %s values', (type, value) => {
    expect(() => decodeServiceValue(type, value, undefined)).toThrow(TypeError);
  });
});
