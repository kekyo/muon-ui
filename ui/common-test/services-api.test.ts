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
    if (name === "rpc") return '{"schemaVersion":1,"methods":{},"events":{}}';
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
  expect(await createMuonServicesApi(call).getState("worker")).toEqual({
    ...state,
    lastHostExit: null,
  });
  expect(call).toHaveBeenCalledExactlyOnceWith(
    "getState",
    '{"serviceId":"worker"}',
  );
});

it("commits autostart without reserving a service reference", async () => {
  let saved: (() => void) | undefined;
  const call = vi.fn(async () => {
    await new Promise<void>((resolve) => {
      saved = resolve;
    });
    return null;
  });
  const api = createMuonServicesApi(call);
  const setting = api.setAutostart("worker", false);
  expect(call).toHaveBeenCalledExactlyOnceWith(
    "setAutostart",
    '{"serviceId":"worker","enabled":false}',
  );
  saved!();
  await setting;
  await expect(
    api.setAutostart("worker", 1 as unknown as boolean),
  ).rejects.toThrow("boolean");
  expect(call).toHaveBeenCalledTimes(1);
});

it("observes initial and changed states without acquiring, then closes a pending observation", async () => {
  vi.useFakeTimers();
  try {
    const initial = {
      id: "worker",
      state: "stopped",
      generation: "0",
      revision: "0",
      autostart: false,
      resident: false,
      clientReferences: 0,
      error: "",
      lastHostExit: null,
    };
    let next: ((json: string) => void) | undefined;
    const call = vi.fn(async (name: string, json: string) => {
      expect(name).toBe("watchState");
      const request = JSON.parse(json);
      expect(request.serviceId).toBe("worker");
      if (request.operation === "acquire")
        return JSON.stringify({ token: "9007199254740993", state: initial });
      expect(request.token).toBe("9007199254740993");
      if (request.operation === "next")
        return await new Promise<string>((resolve) => {
          next = resolve;
        });
      expect(request.operation).toBe("release");
      next!(JSON.stringify({ state: null }));
      return null;
    });
    const listener = vi.fn();
    const watcher = await createMuonServicesApi(call).watchState(
      "worker",
      listener,
    );
    expect(listener).not.toHaveBeenCalled();
    await vi.advanceTimersByTimeAsync(0);
    expect(listener).toHaveBeenCalledExactlyOnceWith(initial);
    const running = {
      ...initial,
      state: "running",
      generation: "1",
      autostart: true,
      resident: true,
    };
    next!(JSON.stringify({ state: running }));
    await vi.advanceTimersByTimeAsync(0);
    expect(listener).toHaveBeenLastCalledWith(running);
    expect(listener).toHaveBeenCalledTimes(2);
    const closing = watcher.close();
    expect(watcher.close()).toBe(closing);
    await closing;
    await vi.advanceTimersByTimeAsync(0);
    expect(
      call.mock.calls.filter(
        ([, json]) => JSON.parse(json).operation === "release",
      ),
    ).toHaveLength(1);
    expect(listener).toHaveBeenCalledTimes(2);
  } finally {
    vi.useRealTimers();
  }
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
    if (name === "rpc") return '{"schemaVersion":1,"methods":{},"events":{}}';
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
