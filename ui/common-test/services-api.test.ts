// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { expect, it, vi } from "vitest";
import { createMuonServicesApi } from "../../core/common/renderer/services.ts";

it("owns distinct handles and releases each native reference exactly once", async () => {
  let next = 0;
  let finish: (() => void) | undefined;
  const call = vi.fn(async (name: string, json: string) => {
    if (name === "acquire") {
      return JSON.stringify({
        serviceId: JSON.parse(json).serviceId,
        refId: String(++next),
        generation: "9007199254740993",
      });
    }
    await new Promise<void>((resolve) => {
      finish = resolve;
    });
    return null;
  });
  const api = createMuonServicesApi(call);
  const first = await api.acquire("worker");
  const second = await api.acquire("worker");
  expect(first).toMatchObject({ id: "worker", generation: "9007199254740993" });
  expect(first).not.toHaveProperty("refId");
  const release = first.release();
  expect(first.release()).toBe(release);
  expect(call.mock.calls.filter(([name]) => name === "release")).toHaveLength(
    1,
  );
  expect(JSON.parse(call.mock.calls.at(-1)![1])).toEqual({
    serviceId: "worker",
    refId: "1",
    generation: "9007199254740993",
  });
  finish!();
  await release;
  await first.release();
  const secondRelease = second.release();
  expect(JSON.parse(call.mock.calls.at(-1)![1])).toHaveProperty("refId", "2");
  finish!();
  await secondRelease;
});

it("observes stopped service state without acquiring a reference", async () => {
  const state = {
    id: "worker",
    state: "stopped",
    generation: "0",
    revision: "0",
    autostart: false,
    resident: false,
    clientReferences: 0,
    error: "",
  };
  const call = vi.fn(async () => JSON.stringify(state));
  expect(await createMuonServicesApi(call).getState("worker")).toEqual(state);
  expect(call).toHaveBeenCalledExactlyOnceWith(
    "getState",
    '{"serviceId":"worker"}',
  );
});

it("preserves native startup failures and releases failures idempotently", async () => {
  const failure = new Error("Foreground service start rejected");
  const denied = createMuonServicesApi(async () => {
    throw failure;
  });
  await expect(denied.acquire("worker")).rejects.toBe(failure);
  let releases = 0;
  const api = createMuonServicesApi(async (name) => {
    if (name === "acquire")
      return '{"serviceId":"worker","refId":"1","generation":"1"}';
    ++releases;
    throw failure;
  });
  const handle = await api.acquire("worker");
  await expect(handle.release()).rejects.toBe(failure);
  await expect(handle.release()).rejects.toBe(failure);
  expect(releases).toBe(1);
});

it.each(["", "../worker", "bad name"])(
  "rejects invalid service identity %s before transport",
  async (id) => {
    const call = vi.fn();
    const api = createMuonServicesApi(call);
    await expect(api.acquire(id)).rejects.toThrow();
    await expect(api.getState(id)).rejects.toThrow();
    expect(call).not.toHaveBeenCalled();
  },
);

it.each([
  { serviceId: "other", refId: "1", generation: "1" },
  { serviceId: "worker", refId: 1, generation: "1" },
  { serviceId: "worker", refId: "1", generation: "0" },
])(
  "rejects mismatched or lossy native handle identities",
  async (reference) => {
    const api = createMuonServicesApi(async () => JSON.stringify(reference));
    await expect(api.acquire("worker")).rejects.toThrow(
      "Invalid native service reference",
    );
  },
);
