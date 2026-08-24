/* Packaged ES module used to verify the Android QuickJS sidecar. */

import path, { basename } from 'node:path';
import pathAlias from 'path';
import EventEmitter, { once as onceEvent } from 'node:events';
import { Buffer } from 'node:buffer';
import {
  clearImmediate,
  clearInterval,
  clearTimeout,
  setImmediate,
  setInterval,
  setTimeout,
} from 'node:timers';
import { setTimeout as setPromiseTimeout } from 'node:timers/promises';

let counter = 0;

export const answer = 42;

export const increment = () => {
  counter += 1;
  return counter;
};

export const echo = async (value) => value;

export const invokeCallback = async (value, callback) => await callback(value);

export const importedPathBasename = (value) => {
  if (path !== pathAlias || basename(value) !== pathAlias.basename(value)) {
    throw new Error(
      'The packaged module did not receive one shared path module'
    );
  }
  return basename(value);
};

export const exerciseRuntimePrimitives = async () => {
  const eventValues = [];
  const emitter = new EventEmitter();
  const persistentListener = function (value) {
    eventValues.push(`on:${value}:${this === emitter}`);
  };
  emitter.prependListener('data', (value) => eventValues.push(`pre:${value}`));
  emitter.on('data', persistentListener);
  emitter.once('data', (value) => eventValues.push(`once:${value}`));
  const firstEmit = emitter.emit('data', 'first');
  emitter.removeListener('data', persistentListener);
  const secondEmit = emitter.emit('data', 'second');
  emitter.removeAllListeners('data');
  const emptyEmit = emitter.emit('data', 'third');

  const pendingEvent = onceEvent(emitter, 'ready');
  setTimeout((value) => emitter.emit('ready', value), 2, 'event-ready');
  const [awaitedEvent] = await pendingEvent;

  const buffer = Buffer.concat([
    Buffer.from('muon✓'),
    Buffer.from('21', 'hex'),
  ]);

  let cancelledTimeoutCalled = false;
  const cancelledTimeout = setTimeout(() => {
    cancelledTimeoutCalled = true;
  }, 2);
  clearTimeout(cancelledTimeout);
  const timeoutValue = await new Promise((resolve) => {
    setTimeout(resolve, 2, 'timeout-ready');
  });

  let cancelledImmediateCalled = false;
  const cancelledImmediate = setImmediate(() => {
    cancelledImmediateCalled = true;
  });
  clearImmediate(cancelledImmediate);
  const immediateValue = await new Promise((resolve) => {
    setImmediate(resolve, 'immediate-ready');
  });

  const intervalCount = await new Promise((resolve) => {
    let count = 0;
    const interval = setInterval(() => {
      count += 1;
      if (count === 2) {
        clearInterval(interval);
        resolve(count);
      }
    }, 2);
  });

  const zeroDelayIntervalCount = await new Promise((resolve) => {
    let count = 0;
    const interval = setInterval(async () => {
      count += 1;
      if (count === 100) {
        clearInterval(interval);
        resolve(count);
        return;
      }
      await Promise.resolve();
      clearInterval(interval);
      resolve(count);
    }, 0);
  });

  const referenceTimer = setTimeout(() => {}, 50);
  const initiallyReferenced = referenceTimer.hasRef();
  referenceTimer.unref();
  const referencedAfterUnref = referenceTimer.hasRef();
  referenceTimer.ref();
  const referencedAfterRef = referenceTimer.hasRef();
  clearTimeout(referenceTimer);

  const abortController = new AbortController();
  const abortedTimer = setPromiseTimeout(1000, 'late', {
    signal: abortController.signal,
  });
  setTimeout(() => abortController.abort(), 2);
  let abortName = '';
  let abortCode = '';
  try {
    await abortedTimer;
  } catch (error) {
    abortName = error.name;
    abortCode = error.code;
  }

  const staticSignal = AbortSignal.abort('static-reason');
  let staticReason = '';
  try {
    staticSignal.throwIfAborted();
  } catch (error) {
    staticReason = String(error);
  }

  const combinedController = new AbortController();
  const combinedSignal = AbortSignal.any([combinedController.signal]);
  combinedController.abort('combined-reason');

  const timeoutSignal = AbortSignal.timeout(2);
  const timeoutReasonName = await new Promise((resolve) => {
    timeoutSignal.addEventListener(
      'abort',
      () => resolve(timeoutSignal.reason.name),
      { once: true }
    );
  });

  await setPromiseTimeout(5);
  return {
    events: {
      values: eventValues,
      firstEmit,
      secondEmit,
      emptyEmit,
      awaitedEvent,
    },
    buffer: {
      text: buffer.toString(),
      hex: buffer.toString('hex'),
      base64: buffer.toString('base64'),
      byteLength: Buffer.byteLength('muon✓'),
      isBuffer: Buffer.isBuffer(buffer),
      isUint8Array: buffer instanceof Uint8Array,
      equalsCopy: buffer.equals(Buffer.from(buffer)),
    },
    timers: {
      timeoutValue,
      immediateValue,
      intervalCount,
      zeroDelayIntervalCount,
      cancelledTimeoutCalled,
      cancelledImmediateCalled,
      initiallyReferenced,
      referencedAfterUnref,
      referencedAfterRef,
    },
    abort: {
      abortName,
      abortCode,
      staticReason,
      combinedReason: combinedSignal.reason,
      timeoutReasonName,
    },
  };
};

export const exhaustMemory = () => {
  const blocks = [];
  while (true) {
    blocks.push(new Uint8Array(1024 * 1024));
  }
};

export const spin = () => {
  while (true) {
    // The runtime interrupt handler must terminate runaway application code.
  }
};

globalThis.__muonBackendModule = Object.freeze({
  answer,
  increment,
  echo,
  invokeCallback,
  importedPathBasename,
  exerciseRuntimePrimitives,
  exhaustMemory,
  spin,
});
