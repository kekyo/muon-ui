// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { fileURLToPath } from "node:url";
import { build, type BuildOptions } from "vite";

/** Shared self-contained bundle settings for native setup and virtual modules. */
export const muonServerRendererBuildOptions: BuildOptions = {
  minify: false,
  sourcemap: false,
  target: "es2022",
  lib: {
    entry: fileURLToPath(
      new URL("./renderer/server.ts", import.meta.url),
    ),
    name: "MuonServerRenderer",
    formats: ["iife"],
    fileName: () => "server.js",
  },
};

/**
 * Bundles the shared server facade and CEF installer without runtime imports.
 * @returns An IIFE defining MuonServerRenderer in the caller's scope.
 * @remarks Both CEF setup and capability modules consume these same source bytes.
 */
export const buildMuonServerRenderer = async (): Promise<string> => {
  const result = await build({
    configFile: false,
    logLevel: "warn",
    publicDir: false,
    build: {
      ...muonServerRendererBuildOptions,
      write: false,
    },
  });
  const outputs = Array.isArray(result) ? result : [result];
  if (outputs.length !== 1 || !("output" in outputs[0]!))
    throw new Error("Unexpected server renderer build output");
  const chunks = outputs[0].output;
  if (chunks.length !== 1 || chunks[0]?.type !== "chunk")
    throw new Error("Server renderer must be one self-contained script");
  return chunks[0].code;
};
