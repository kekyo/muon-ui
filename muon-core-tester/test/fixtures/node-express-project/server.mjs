// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { once } from "node:events";

import express from "express";

const app = express();

let server = undefined;
let startOperation = undefined;
let stopOperation = undefined;

const requirePort = () => {
  const address = server?.address();
  if (
    address === null ||
    address === undefined ||
    typeof address === "string"
  ) {
    throw new Error("Express did not expose its allocated TCP port");
  }
  return address.port;
};

app.get("/muon-node-e2e/status/:value", (request, response) => {
  response.set("Access-Control-Allow-Origin", "*");
  response.json({
    framework: "express",
    processId: process.pid,
    value: request.params.value,
  });
});

const listen = async () => {
  const pendingServer = app.listen(0, "127.0.0.1");
  server = pendingServer;
  try {
    await once(pendingServer, "listening");
    return requirePort();
  } catch (error) {
    if (server === pendingServer) {
      server = undefined;
    }
    throw error;
  }
};

/**
 * Starts the Express server on an operating-system-assigned loopback port.
 *
 * @returns {Promise<number>} The listening TCP port.
 */
export const startServer = async () => {
  if (stopOperation !== undefined) {
    await stopOperation;
  }
  if (server?.listening) {
    return requirePort();
  }
  if (startOperation === undefined) {
    startOperation = listen();
  }
  try {
    return await startOperation;
  } finally {
    startOperation = undefined;
  }
};

const close = async () => {
  const activeServer = server;
  if (activeServer === undefined || !activeServer.listening) {
    server = undefined;
    return undefined;
  }
  const closed = once(activeServer, "close");
  activeServer.close();
  await closed;
  if (server === activeServer) {
    server = undefined;
  }
  return undefined;
};

/**
 * Stops the Express server after all active connections have closed.
 *
 * @returns {Promise<undefined>} Resolves after the listener closes.
 */
export const stopServer = async () => {
  if (startOperation !== undefined) {
    await startOperation;
  }
  if (stopOperation === undefined) {
    stopOperation = close();
  }
  try {
    return await stopOperation;
  } finally {
    stopOperation = undefined;
  }
};

/**
 * Reports whether the Express server is accepting connections.
 *
 * @returns {boolean} True while the server is listening.
 */
export const isListening = () => server?.listening === true;

/**
 * Gets the executable path used to launch this Node sidecar.
 *
 * @returns {string} Absolute Node executable path.
 */
export const executablePath = () => process.execPath;

/**
 * Gets the process identifier of the Node sidecar.
 *
 * @returns {number} The Node sidecar process identifier.
 */
export const processId = () => process.pid;
