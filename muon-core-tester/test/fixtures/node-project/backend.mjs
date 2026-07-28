// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { existsSync, watch, writeFileSync } from "node:fs";
import { basename, dirname } from "node:path";

import "./process-marker.mjs";

let completeObservedCallbackFailure = () => undefined;
const observedCallbackFailure = new Promise((resolve) => {
  completeObservedCallbackFailure = resolve;
});
let state = 0;

export const processId = () => process.pid;

export const descriptorValue = "descriptor";

export const undefinedExport = undefined;

export const echo = (value) => value;

export const copyBuffer = (value) => Buffer.from(value);

export const invokeCallback = async (callback, value) => await callback(value);

export const mutateJsonValue = (value) => {
  value.nested.items[1].source = "node";
  value.nodeOnly = ["added", { accepted: true }];
  return value;
};

export const inspectPrototypeProperty = (value) => ({
  hasOwnPrototypeProperty: Object.hasOwn(value, "__proto__"),
  prototypeProperty: value["__proto__"],
  objectPrototypePolluted: Object.prototype.muonNodeE2ePolluted === true,
});

export const currentWorkingDirectory = () => process.cwd();

export const incrementState = () => {
  state += 1;
  return state;
};

export const remainPending = async (notifyStarted) => {
  await notifyStarted();
  await new Promise(() => undefined);
};

export const observeCallbackFailure = async (callback) => {
  try {
    await callback();
    return "resolved";
  } catch (error) {
    const message = String(error instanceof Error ? error.message : error);
    completeObservedCallbackFailure(message);
    return message;
  }
};

export const waitForObservedCallbackFailure = async () =>
  await observedCallbackFailure;

export const returnObject = () => ({ unsupported: true });

export const returnCircularObject = () => {
  const value = { circular: undefined };
  value.circular = value;
  return value;
};

export const unsupportedExport = { unsupported: true };

export const installExitMarker = (path) => {
  process.once("exit", () => {
    writeFileSync(path, "exited\n", "utf8");
  });
  return true;
};

export const waitForFile = async (path) => {
  if (existsSync(path)) {
    return true;
  }
  await new Promise((resolve, reject) => {
    let watcher;
    const complete = () => {
      watcher.close();
      resolve();
    };
    try {
      watcher = watch(dirname(path), (_eventType, filename) => {
        if (filename?.toString() === basename(path) && existsSync(path)) {
          complete();
        }
      });
      if (existsSync(path)) {
        complete();
      }
    } catch (error) {
      reject(error);
    }
  });
  return true;
};

export const crash = () => {
  process.exit(23);
};
