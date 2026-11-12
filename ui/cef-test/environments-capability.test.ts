// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { afterEach, expect, it, vi } from "vitest";
import { createMuonCapabilityModuleResolver } from "../common/capability.ts";

afterEach(() => vi.unstubAllGlobals());

it("returns UI autostart booleans and an unsupported result through capability imports", async () => {
  let response = "false";
  vi.stubGlobal("__muon_plugin_call", async () => response);
  const resolver = createMuonCapabilityModuleResolver("/app", {
    backend: "cef",
    imports: [
      { sources: ["main.ts"], allow: ["muon.environments.getAutostart"] },
    ],
  });
  const id = resolver.resolveId("muon:environments", "/app/main.ts")!.id;
  const api = await import(
    "data:text/javascript," + encodeURIComponent(resolver.load(id)!)
  );
  expect(await api.getAutostart()).toBe(false);
  response = "true";
  expect(await api.getAutostart()).toBe(true);
  response = "null";
  expect(await api.getAutostart()).toBeUndefined();
});
