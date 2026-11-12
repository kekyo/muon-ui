// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { afterEach, expect, it, vi } from "vitest";
import { createMuonCapabilityModuleResolver } from "../common/capability.ts";
import type { MuonServicesApi } from "../../core/common/renderer/services.ts";
import { installMuonCefServicesApi } from "../../core/cef/renderer/services.ts";
import { resolveMuonPluginAccessOptions } from "../common/plugin-access.ts";
import { mkdtemp, rm } from "node:fs/promises";
import { join } from "node:path";
import { tmpdir } from "node:os";

afterEach(() => vi.unstubAllGlobals());

it.each(["android", "cef"] as const)(
  "provides service handles through %s capability imports",
  async (backend) => {
    const call = vi.fn(
      async (_capability: string, path: string, args: readonly unknown[]) => {
        expect(typeof args[0]).toBe("string");
        return path.endsWith("acquire")
          ? '{"serviceId":"worker","refId":"1","generation":"1"}'
          : null;
      },
    );
    vi.stubGlobal("__muon_plugin_call", call);
    const resolver = createMuonCapabilityModuleResolver("/app", {
      backend,
      imports: [{ sources: ["src/**"], allow: ["muon.services.acquire"] }],
    });
    const id = resolver.resolveId("muon:services", "/app/src/main.ts")!.id;
    const api = (await import(
      "data:text/javascript," + encodeURIComponent(resolver.load(id)!)
    )) as MuonServicesApi;
    expect(Object.keys(api)).toEqual(["acquire"]);
    const handle = await api.acquire("worker");
    expect(handle.id).toBe("worker");
    await handle.release();
    expect(call).toHaveBeenLastCalledWith(
      expect.any(String),
      "muon.services.release",
      backend === "cef"
        ? ['{"serviceId":"worker","refId":"1","generation":"1"}', ""]
        : ['{"serviceId":"worker","refId":"1","generation":"1"}'],
    );
    expect(resolver.getRuntimePluginConfig().capabilities[0]!.allow).toEqual([
      "muon.services.acquire",
      "muon.services.release",
    ]);
  },
);

it("does not give a state observer acquisition or release permission", () => {
  const resolver = createMuonCapabilityModuleResolver("/app", {
    imports: [{ sources: ["src/**"], allow: ["muon.services.getState"] }],
  });
  expect(resolver.getRuntimePluginConfig().capabilities[0]!.allow).toEqual([
    "muon.services.getState",
  ]);
});

it.each(["android", "cef"] as const)(
  "exports only service management operations authorized for %s",
  async (backend) => {
    const call = vi.fn(async () => null);
    vi.stubGlobal("__muon_plugin_call", call);
    const resolver = createMuonCapabilityModuleResolver("/app", {
      backend,
      imports: [
        {
          sources: ["src/**"],
          allow: ["muon.services.setAutostart", "muon.services.watchState"],
        },
      ],
    });
    const id = resolver.resolveId("muon:services", "/app/src/main.ts")!.id;
    const api = (await import(
      "data:text/javascript," + encodeURIComponent(resolver.load(id)!)
    )) as MuonServicesApi;
    expect(Object.keys(api).sort()).toEqual(["setAutostart", "watchState"]);
    await api.setAutostart("worker", false);
    expect(call).toHaveBeenCalledExactlyOnceWith(
      expect.any(String),
      "muon.services.setAutostart",
      backend === "cef"
        ? ['{"serviceId":"worker","enabled":false}', ""]
        : ['{"serviceId":"worker","enabled":false}'],
    );
    expect(resolver.getRuntimePluginConfig().capabilities[0]!.allow).toEqual([
      "muon.services.setAutostart",
      "muon.services.watchState",
    ]);
  },
);

it("installs only allowed CEF methods while retaining handle release", async () => {
  const released = vi.fn(async () => null);
  const namespace: Record<string, unknown> = {};
  Object.defineProperties(namespace, {
    __acquire: {
      value: async () => '{"serviceId":"worker","refId":"2","generation":"3"}',
    },
    __release: { value: released },
  });
  installMuonCefServicesApi(namespace, (name) => name === "acquire");
  expect(Object.keys(namespace)).toEqual(["acquire"]);
  const handle = await (namespace.acquire as MuonServicesApi["acquire"])(
    "worker",
  );
  await handle.release();
  expect(released).toHaveBeenCalledExactlyOnceWith(
    '{"serviceId":"worker","refId":"2","generation":"3"}',
    "",
  );
});

it.each(["simple", "validate"] as const)(
  "includes the internal release dependency in %s plugin policy",
  async (mode) => {
    const root = await mkdtemp(join(tmpdir(), "muon-service-policy-"));
    try {
      const resolved = await resolveMuonPluginAccessOptions({
        root,
        configPath: undefined,
        pluginAccess: {
          mode,
          plugins: [
            {
              name: "internal",
              ...(mode === "simple"
                ? { allow: ["muon.services.acquire"] }
                : {
                    imports: [
                      { sources: ["src/**"], allow: ["muon.services.acquire"] },
                    ],
                  }),
            },
          ],
        },
      });
      expect(resolved.runtimeOverlay.plugins![0]!.allow).toEqual([
        "muon.services.acquire",
        "muon.services.release",
      ]);
    } finally {
      await rm(root, { recursive: true, force: true });
    }
  },
);
