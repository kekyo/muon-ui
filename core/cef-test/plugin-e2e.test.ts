// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { env } from "node:process";

const group = env.MUON_CEF_E2E_GROUP ?? "all";
if (!["all", "application", "runtime"].includes(group)) {
  throw new Error(`Unknown CEF E2E group: ${group}`);
}
if (group !== "runtime") {
  await import("./plugin-e2e/app-network.js");
  await import("./plugin-e2e/browser-background.js");
  await import("./plugin-e2e/title-bar.js");
}
if (group !== "application") {
  await import("./plugin-e2e/runtime-api.js");
}
if (group !== "runtime") {
  await import("./plugin-e2e/plugin-interop.js");
  await import("./plugin-e2e/native-dialog.js");
}
