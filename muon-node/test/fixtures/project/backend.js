// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { runInNewContext } from 'node:vm';

export const answer = 42;

export const undefinedExport = undefined;

export const bigintExport = 123n;

export const bufferExport = Buffer.from([1, 2, 3]);

export const echo = async (value) => value;

export const echoI64 = async (value) => value;

export const echoU64 = async (value) => value;

export const copyBuffer = async (value) => Buffer.from(value);

export const returnUint8Array = async () => {
  const storage = Uint8Array.from([255, 0, 1, 2, 254]);
  return storage.subarray(1, 4);
};

export const returnDataView = async () => {
  const storage = Uint8Array.from([255, 3, 4, 5, 254]);
  return new DataView(storage.buffer, 1, 3);
};

export const returnOversizedBuffer = async () => Buffer.alloc(12 * 1024 * 1024);

export const returnOversizedJsonValue = async () => ({
  data: 'x'.repeat(16 * 1024 * 1024),
});

export const returnFakeArrayBuffer = async () =>
  Object.create(ArrayBuffer.prototype);

export const returnCrossRealmArrayBuffer = async () =>
  runInNewContext('Uint8Array.from([0, 1, 127, 255]).buffer');

export const invokeCallback = async (callback, value) => await callback(value);

export const returnObject = async () => ({
  nested: {
    enabled: true,
  },
  values: [1, 'two', null, { leaf: false }],
});

export const objectExport = {
  unsupported: true,
};

export const arrayExport = ['unsupported'];

export const mutateJsonValue = async (value) => {
  value.changed = true;
  value.values.push('node');
  return value;
};

export const returnSharedJsonValue = async () => {
  const shared = {
    source: 'shared',
  };
  return {
    first: shared,
    second: shared,
  };
};

export const returnNullPrototypeObject = async () => {
  const value = Object.create(null);
  value.supported = true;
  return value;
};

export const inspectPrototypeProperty = async (value) => ({
  hasOwnPrototypeProperty: Object.hasOwn(value, '__proto__'),
  prototypeProperty: value['__proto__'],
  objectPrototypePolluted: Object.prototype.muonNodePolluted === true,
});

export const returnInvalidJsonValue = async (kind) => {
  switch (kind) {
    case 'cycle': {
      const value = {};
      value.self = value;
      return value;
    }
    case 'sparse-array': {
      const value = [];
      value.length = 3;
      value[0] = 1;
      value[2] = 3;
      return value;
    }
    case 'array-property': {
      const value = [1, 2, 3];
      value.extra = true;
      return value;
    }
    case 'custom-prototype': {
      const value = Object.create({
        inherited: true,
      });
      value.own = true;
      return value;
    }
    case 'date':
      return new Date('2026-01-01T00:00:00.000Z');
    case 'map':
      return new Map([['key', 'value']]);
    case 'set':
      return new Set(['value']);
    case 'accessor': {
      const value = {};
      Object.defineProperty(value, 'accessor', {
        enumerable: true,
        get: () => {
          throw new Error('The accessor must not be invoked.');
        },
      });
      return value;
    }
    case 'symbol-property': {
      const value = {};
      value[Symbol('key')] = 'value';
      return value;
    }
    case 'non-enumerable': {
      const value = {};
      Object.defineProperty(value, 'hidden', {
        enumerable: false,
        value: true,
      });
      return value;
    }
    case 'nested-undefined':
      return { value: undefined };
    case 'nested-bigint':
      return { value: 1n };
    case 'nested-buffer':
      return { value: Buffer.from([1, 2, 3]) };
    case 'nested-function':
      return { value: () => undefined };
    case 'nested-symbol':
      return { value: Symbol('value') };
    case 'nan':
      return { value: Number.NaN };
    case 'infinity':
      return { value: Number.POSITIVE_INFINITY };
    case 'negative-zero':
      return { value: -0 };
    case 'proxy':
      return new Proxy(
        {},
        {
          ownKeys: () => {
            throw new Error('The proxy cannot be inspected.');
          },
        }
      );
    default:
      throw new Error(`Unknown invalid JSON value fixture: ${kind}`);
  }
};
