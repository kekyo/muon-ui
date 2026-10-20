// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { fileURLToPath } from "node:url";
import { build, type BuildOptions } from "vite";

/** Shared self-contained bundle settings for native setup and virtual modules. */
export const muonNotificationRendererBuildOptions: BuildOptions = {
  minify: false,
  sourcemap: false,
  target: "es2022",
  lib: {
    entry: fileURLToPath(
      new URL("../cef/renderer/notifications.ts", import.meta.url),
    ),
    name: "MuonNotificationRenderer",
    formats: ["iife"],
    fileName: () => "notifications.js",
  },
};

/**
 * Bundles the shared notification facade and CEF installer without runtime imports.
 * @returns An IIFE defining MuonNotificationRenderer in the caller's scope.
 * @remarks Both CEF setup and capability modules consume these same source bytes.
 */
export const buildMuonNotificationRenderer = async (): Promise<string> => {
  const result = await build({
    configFile: false,
    logLevel: "warn",
    publicDir: false,
    build: {
      ...muonNotificationRendererBuildOptions,
      write: false,
    },
  });
  const outputs = Array.isArray(result) ? result : [result];
  if (outputs.length !== 1 || !("output" in outputs[0]!))
    throw new Error("Unexpected notification renderer build output");
  const chunks = outputs[0].output;
  if (chunks.length !== 1 || chunks[0]?.type !== "chunk")
    throw new Error("Notification renderer must be one self-contained script");
  return chunks[0].code;
};
