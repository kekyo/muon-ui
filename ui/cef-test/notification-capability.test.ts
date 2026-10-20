// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { afterEach, describe, expect, it, vi } from "vitest";
import { createMuonCapabilityModuleResolver } from "../common/capability.js";
import { installMuonCefNotificationsApi } from "../../core/cef/renderer/notifications.js";
import { resolveMuonCefNotificationFunctionPaths } from "../cef/notifications.js";
import type {
  MuonNotificationEvent,
  MuonNotificationsApi,
} from "../../core/common/renderer/notifications.js";

afterEach(() => vi.unstubAllGlobals());

describe("CEF notification capability modules", () => {
  it.each([false, true])(
    "requires enabled Linux notifications for imports: %s",
    async (enabled) => {
      const paths = resolveMuonCefNotificationFunctionPaths(
        { enabled, defaultCategory: "orders" },
        ["linux-amd64"],
      );
      const options = {
        backend: "cef" as const,
        functionPaths: paths,
        imports: [
          { sources: ["src/**"], allow: ["muon.notifications.getStatus"] },
        ],
      };
      if (!enabled) {
        expect(() =>
          createMuonCapabilityModuleResolver("/app", options),
        ).toThrow(/unavailable.*notifications/);
        return;
      }
      vi.stubGlobal("__muon_plugin_call", async () =>
        JSON.stringify({
          app: { state: "unknown", reason: "No app status API" },
          category: null,
        }),
      );
      const resolver = createMuonCapabilityModuleResolver("/app", options);
      const id = resolver.resolveId(
        "muon:notifications",
        "/app/src/main.ts",
      )!.id;
      const api = (await import(
        `data:text/javascript,${encodeURIComponent(resolver.load(id)!)}`
      )) as MuonNotificationsApi;
      await expect(api.getStatus()).resolves.toMatchObject({
        app: { state: "unknown" },
        category: null,
      });
    },
  );

  it("does not publish notification imports on an unimplemented Windows target", () => {
    const paths = resolveMuonCefNotificationFunctionPaths({ enabled: true }, [
      "windows-amd64",
    ]);
    expect(() =>
      createMuonCapabilityModuleResolver("/app", {
        backend: "cef",
        functionPaths: paths,
        imports: [{ sources: ["src/**"], allow: ["muon.notifications.*"] }],
      }),
    ).toThrow(/unavailable.*notifications/);
  });

  it("installs allowed operations over non-enumerable native functions", async () => {
    const call = vi.fn(async () => undefined);
    const namespace: Record<string, unknown> = {};
    Object.defineProperty(namespace, "__post", { value: call });
    installMuonCefNotificationsApi(namespace, (name) => name === "post");
    Object.freeze(namespace);
    expect(Object.keys(namespace)).toEqual(["post"]);
    const options = { id: "order", title: "Ready", body: "" };
    await (namespace.post as MuonNotificationsApi["post"])(options);
    expect(call).toHaveBeenCalledWith(JSON.stringify(options));
  });

  it("uses shared argument validation and decoded results with the authorized bridge", async () => {
    const state = {
      app: { state: "unknown", reason: "Unavailable" },
      category: null,
    };
    const call = vi.fn(async (_capability: string, path: string) =>
      path.endsWith("getStatus") ? JSON.stringify(state) : undefined,
    );
    vi.stubGlobal("__muon_plugin_call", call);
    const resolver = createMuonCapabilityModuleResolver("/app", {
      backend: "cef",
      functionPaths: resolveMuonCefNotificationFunctionPaths(
        { enabled: true },
        ["linux-amd64"],
      ),
      imports: [
        {
          sources: ["src/**"],
          allow: ["muon.notifications.post", "muon.notifications.getStatus"],
        },
      ],
    });
    const id = resolver.resolveId("muon:notifications", "/app/src/main.ts")!.id;
    const api = (await import(
      `data:text/javascript,${encodeURIComponent(resolver.load(id)!)}`
    )) as MuonNotificationsApi;
    expect(Object.keys(api).sort()).toEqual(["getStatus", "post"]);
    const post = {
      id: "order",
      title: "Ready",
      body: "Open order",
      data: { order: 42 },
      actions: [{ id: "default", title: "Details" }],
    };
    await api.post(post);
    expect(call).toHaveBeenLastCalledWith(
      expect.any(String),
      "muon.notifications.post",
      [JSON.stringify(post)],
    );
    await expect(api.post({ ...post, id: " " })).rejects.toThrow(/nonempty/);
    expect(call).toHaveBeenCalledTimes(1);
    await expect(api.getStatus()).resolves.toEqual(state);
    expect(call).toHaveBeenLastCalledWith(
      expect.any(String),
      "muon.notifications.getStatus",
      ["{}"],
    );
  });

  it("delivers persisted events and closes an outstanding wait without acknowledging", async () => {
    const event: MuonNotificationEvent = {
      eventId: "event-1",
      id: "order",
      type: "action",
      actionId: "default",
      data: { order: 42 },
    };
    let receive!: (event: MuonNotificationEvent) => void;
    const received = new Promise<MuonNotificationEvent>((resolve) => {
      receive = resolve;
    });
    let wait!: () => void;
    const waiting = new Promise<void>((resolve) => {
      wait = resolve;
    });
    let finish!: (value: string) => void;
    const pending = new Promise<string>((resolve) => {
      finish = resolve;
    });
    const operations: string[] = [];
    let calls = 0;
    vi.stubGlobal(
      "__muon_plugin_call",
      async (_capability: string, path: string, args: string[]) => {
        if (path.endsWith("getPendingEvents")) return JSON.stringify([event]);
        expect(path).toBe("muon.notifications.watchEvents");
        expect(args).toHaveLength(2);
        expect(args[1]).toBe("");
        const request = JSON.parse(args[0]!);
        operations.push(request.operation);
        if (request.operation === "acquire")
          return JSON.stringify({ token: "lease", cursor: 0 });
        expect(request.token).toBe("lease");
        if (request.operation === "release") {
          finish(JSON.stringify({ events: null, cursor: 1 }));
          return;
        }
        if (calls++ === 0)
          return JSON.stringify({ events: [event], cursor: 1 });
        wait();
        return await pending;
      },
    );
    const resolver = createMuonCapabilityModuleResolver("/app", {
      backend: "cef",
      functionPaths: resolveMuonCefNotificationFunctionPaths(
        { enabled: true },
        ["linux-amd64"],
      ),
      imports: [
        {
          sources: ["src/**"],
          allow: [
            "muon.notifications.watchEvents",
            "muon.notifications.getPendingEvents",
          ],
        },
      ],
    });
    const id = resolver.resolveId("muon:notifications", "/app/src/main.ts")!.id;
    const api = (await import(
      `data:text/javascript,${encodeURIComponent(resolver.load(id)!)}`
    )) as MuonNotificationsApi;
    let handleReturned = false;
    const watcher = await api.watchEvents((value) => {
      expect(handleReturned).toBe(true);
      receive(value);
    });
    handleReturned = true;
    await expect(received).resolves.toEqual(event);
    await expect(api.getPendingEvents()).resolves.toEqual([event]);
    await waiting;
    await watcher.close();
    await watcher.close();
    expect(operations).toEqual(["acquire", "next", "next", "release"]);
  });
});
