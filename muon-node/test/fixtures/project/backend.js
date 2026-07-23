// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

export const answer = 42;

export const undefinedExport = undefined;

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

export const invokeCallback = async (callback, value) => await callback(value);

export const returnObject = async () => ({
  unsupported: true,
});

export const objectExport = {
  unsupported: true,
};
