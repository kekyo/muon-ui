// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { afterEach, describe, expect, it } from "vitest";
import { createMuonCapabilityModuleResolver } from "../src/capability.js";

const previous = Object.getOwnPropertyDescriptor(
  globalThis,
  "__muon_plugin_call",
);
afterEach(() => {
  if (previous === undefined)
    Reflect.deleteProperty(globalThis, "__muon_plugin_call");
  else Object.defineProperty(globalThis, "__muon_plugin_call", previous);
});

describe("Android capability modules", () => {
  it("exports only Android browser functions for a wildcard", async () => {
    Reflect.set(globalThis, "__muon_plugin_call", async () => undefined);
    const resolver = createMuonCapabilityModuleResolver("/app", {
      backend: "android",
      imports: [{ sources: ["src/**"], allow: ["muon.browser.*"] }],
    });
    const id = resolver.resolveId("muon:browser", "/app/src/main.ts")!.id;
    const module = await import(
      `data:text/javascript,${encodeURIComponent(resolver.load(id)!)}`
    );
    expect(Object.keys(module).sort()).toEqual([
      "close",
      "enterFullscreen",
      "exitFullscreen",
      "reload",
      "resetZoom",
      "toggleFullscreen",
      "zoomIn",
      "zoomOut",
    ]);
    await module.reload();
  });

  it("preserves the Android bridge result for environment calls", async () => {
    Reflect.set(globalThis, "__muon_plugin_call", async () => ({
      channel: "android",
    }));
    const resolver = createMuonCapabilityModuleResolver("/app", {
      backend: "android",
      imports: [
        { sources: ["src/**"], allow: ["muon.environments.getConfigValues"] },
      ],
    });
    const id = resolver.resolveId("muon:environments", "/app/src/main.ts")!.id;
    const module = await import(
      `data:text/javascript,${encodeURIComponent(resolver.load(id)!)}`
    );
    await expect(module.getConfigValues()).resolves.toEqual({
      channel: "android",
    });
  });

  it.each([
    "muon.browser.hardReload",
    "muon.fs.dialogs.*",
    "muon.node.**",
    "muon.executor.spawn",
  ])("rejects unavailable Android functions at build time: %s", (path) => {
    expect(() =>
      createMuonCapabilityModuleResolver("/app", {
        backend: "android",
        imports: [{ sources: ["src/**"], allow: [path] }],
      }),
    ).toThrow(/Android/);
  });
});
