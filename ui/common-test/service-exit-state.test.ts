// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { expect, it, vi } from "vitest";
import { createMuonServicesApi } from "../../core/common/renderer/services.ts";

const state = {
  id: "worker",
  state: "running",
  generation: "1",
  revision: "0",
  autostart: true,
  resident: true,
  clientReferences: 0,
  error: "",
};
const exit = {
  platform: "android",
  processName: "example.app:muon_services",
  pid: 123,
  timestampMs: "1794520000000",
  reason: "user-requested",
  reasonCode: 10,
  status: 0,
  description: "force stop 日本語😀",
};

it("represents unavailable platform history without changing current state", async () => {
  for (const native of [state, { ...state, lastHostExit: null }]) {
    const api = createMuonServicesApi(async () => JSON.stringify(native));
    expect(await api.getState("worker")).toEqual({
      ...state,
      lastHostExit: null,
    });
  }
});

it("preserves immutable host exit history separately from current failures", async () => {
  const api = createMuonServicesApi(async () =>
    JSON.stringify({ ...state, lastHostExit: exit }),
  );
  const result = await api.getState("worker");
  expect(result).toEqual({ ...state, lastHostExit: exit });
  expect(Object.isFrozen(result.lastHostExit)).toBe(true);
});

it("validates historical metadata before exposing a state observation", async () => {
  for (const invalid of [
    false,
    {},
    { ...exit, platform: "linux" },
    { ...exit, pid: -1 },
    { ...exit, timestampMs: "1.5" },
    { ...exit, timestampMs: "18446744073709551616" },
    { ...exit, reasonCode: 1.5 },
    { ...exit, description: false },
  ]) {
    const api = createMuonServicesApi(async () =>
      JSON.stringify({ ...state, lastHostExit: invalid }),
    );
    await expect(api.getState("worker")).rejects.toThrow("exit");
  }
});

it("uses the same historical record for non-owning state watches", async () => {
  vi.useFakeTimers();
  try {
    let finish: ((value: unknown) => void) | undefined;
    const api = createMuonServicesApi(async (_name, json) => {
      const operation = JSON.parse(json).operation;
      if (operation === "acquire")
        return JSON.stringify({
          token: "1",
          state: { ...state, lastHostExit: exit },
        });
      if (operation === "next")
        return await new Promise((resolve) => {
          finish = resolve;
        });
      finish?.(JSON.stringify({ state: null }));
      return "{}";
    });
    const observed = vi.fn();
    const watcher = await api.watchState("worker", observed);
    await vi.advanceTimersByTimeAsync(0);
    expect(observed).toHaveBeenCalledExactlyOnceWith({
      ...state,
      lastHostExit: exit,
    });
    expect(Object.isFrozen(observed.mock.calls[0]![0].lastHostExit)).toBe(true);
    await watcher.close();
  } finally {
    vi.useRealTimers();
  }
});
