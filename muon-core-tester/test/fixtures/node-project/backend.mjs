// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { writeFileSync } from "node:fs";

import "./process-marker.mjs";

let completeObservedCallbackFailure = () => undefined;
const observedCallbackFailure = new Promise((resolve) => {
  completeObservedCallbackFailure = resolve;
});

export const processId = () => process.pid;

export const descriptorValue = "descriptor";

export const undefinedExport = undefined;

export const echo = (value) => value;

export const copyBuffer = (value) => Buffer.from(value);

export const invokeCallback = async (callback, value) => await callback(value);

export const currentWorkingDirectory = () => process.cwd();

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

export const unsupportedExport = { unsupported: true };

export const installExitMarker = (path) => {
  process.once("exit", () => {
    writeFileSync(path, "exited\n", "utf8");
  });
  return true;
};

export const crash = () => {
  process.exit(23);
};
