// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { expect, it } from "vitest";
import {
  resolveAndroidServices,
  getAndroidServicePermissions,
} from "../android/services.ts";

it("resolves QuickJS services for both Android ABIs without native artifacts", async () => {
  const services = await resolveAndroidServices(
    [
      {
        id: "worker.v1",
        runtime: "quickjs",
        bundle: "globalThis.ready = true;",
        autostart: true,
        config: { role: "background" },
        android: {
          foregroundServiceType: "specialUse",
          specialUseSubtype: "User selected work",
          notification: {
            channelId: "worker",
            title: "Worker",
            text: "Running",
          },
        },
      },
    ],
    ["arm64-v8a", "x86_64"],
    [],
  );
  expect(services).toHaveLength(1);
  expect(services[0]).toMatchObject({
    id: "worker.v1",
    runtime: "quickjs",
    autostart: true,
    bundle: "globalThis.ready = true;",
    asset: "muon/services/worker.v1.js",
    config: { role: "background" },
  });
  expect(getAndroidServicePermissions(services)).toContain(
    "android.permission.FOREGROUND_SERVICE_SPECIAL_USE",
  );
});
