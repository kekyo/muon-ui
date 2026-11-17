#!/usr/bin/env node
// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

// A UI process driver using the common page API and the actual service host.
import { readFile, writeFile } from "node:fs/promises";
import { once } from "node:events";
import { createConnection } from "node:net";
import { createInterface } from "node:readline";

const override = JSON.parse(await readFile(process.argv.at(-1), "utf8"));
const { createMuonServicesApi } = await import(process.env.MUON_TEST_RENDERER);
const socket = createConnection(override.serviceHost.endpoint);
await once(socket, "connect");
const waiting = new Map();
let sequence = 0;
createInterface({ input: socket }).on("line", (line) => {
  const reply = JSON.parse(line);
  const completion = waiting.get(reply.requestId);
  if (!completion) return;
  waiting.delete(reply.requestId);
  if (reply.error) completion.reject(new Error(reply.error.message));
  else completion.resolve(reply.result);
});
socket.on("close", () => {
  for (const job of waiting.values())
    job.reject(new Error("Page disconnected"));
  waiting.clear();
});
const api = createMuonServicesApi(async (op, json) => {
  const requestId = String(++sequence);
  const response = new Promise((resolve, reject) => {
    waiting.set(requestId, { resolve, reject });
  });
  socket.write(JSON.stringify({ ...JSON.parse(json), op, requestId }) + "\n");
  return JSON.stringify(await response);
});
try {
  const service = await api.acquire("worker");
  const value = await service.call("greet", ["service"]);
  await service.release();
  await writeFile(
    process.env.MUON_TEST_RESULT,
    JSON.stringify({ status: "ok", value }),
  );
} finally {
  socket.destroy();
}
process.exitCode = Number(process.env.MUON_TEST_EXIT ?? 0);
