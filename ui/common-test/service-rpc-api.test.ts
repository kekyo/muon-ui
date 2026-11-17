// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { expect, it, vi } from "vitest";
import { createMuonServicesApi } from "../../core/common/renderer/services.ts";

const definition = {
  schemaVersion: 1,
  methods: {
    echo: { parameters: ["uint64"], returns: "uint64", errors: [] },
  },
  events: {},
};

it("binds validated calls to each acquired reference and closes only the released handle", async () => {
  let nextReference = 0;
  const native = vi.fn(async (name: string, json: string) => {
    const request = JSON.parse(json);
    if (name === "acquire")
      return JSON.stringify({
        serviceId: "worker",
        refId: String(++nextReference),
        generation: "9007199254740993",
      });
    if (name === "release") return "null";
    expect(name).toBe("rpc");
    expect(request.serviceId).toBe("worker");
    expect(request.generation).toBe("9007199254740993");
    if (request.action === "describe") return JSON.stringify(definition);
    if (request.action === "begin") {
      expect(request.payload).toEqual({
        method: "echo",
        arguments: ["18446744073709551615"],
      });
      return '{"callId":"1"}';
    }
    if (request.action === "result")
      return '{"status":"ok","value":"18446744073709551615"}';
    expect(request.action).toBe("callbackNext");
    return '{"done":true}';
  });
  const api = createMuonServicesApi(native);
  const first = await api.acquire("worker");
  const second = await api.acquire("worker");
  expect(first.interface).toEqual(definition);
  expect(await first.call("echo", [(1n << 64n) - 1n])).toBe((1n << 64n) - 1n);
  const closing = first.release();
  expect(first.release()).toBe(closing);
  await expect(first.call("echo", [(1n << 64n) - 1n])).rejects.toMatchObject({
    code: "closed",
  });
  await closing;
  expect(await second.call("echo", [(1n << 64n) - 1n])).toBe((1n << 64n) - 1n);
  const begins = native.mock.calls.filter(
    ([name, json]) => name === "rpc" && JSON.parse(json).action === "begin",
  );
  expect(begins.map(([, json]) => JSON.parse(json).refId)).toEqual(["1", "2"]);
  await second.release();
});

it.each(["invalid-json", '{"schemaVersion":9,"methods":{},"events":{}}'])(
  "releases an acquisition when interface discovery fails: %s",
  async (schema) => {
    const native = vi.fn(async (name: string) => {
      if (name === "acquire")
        return '{"serviceId":"worker","refId":"1","generation":"1"}';
      if (name === "rpc") return schema;
      return "null";
    });
    await expect(
      createMuonServicesApi(native).acquire("worker"),
    ).rejects.toThrow();
    expect(native).toHaveBeenLastCalledWith(
      "release",
      '{"serviceId":"worker","refId":"1","generation":"1"}',
    );
  },
);
