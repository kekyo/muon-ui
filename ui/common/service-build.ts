// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { isBuiltin } from "node:module";
import { dirname, join } from "node:path";
import { runScriptOnceToText } from "funcity";

/**
 * Bundles one declared TS/JS service with its portable runtime adapter.
 * @param entry - Absolute author entry exporting a service definition.
 * @returns Self-contained ESM source, evaluated only by the service host.
 * @remarks Requires Vite 8 and the author's muon-plugin-sdk installation.
 * Node builtins and external chunks are rejected for the QuickJS runtime.
 */
export const buildMuonQuickJsService = async (
  entry: string,
): Promise<string> => {
  const vite = await import("vite");
  if (Number(vite.version.split(".")[0]) < 8)
    throw new Error("QuickJS services require Vite 8 or newer.");
  const source = await runScriptOnceToText(
    `
import definition from {{entry}};
import { createServiceRuntime } from 'muon-plugin-sdk/services';
globalThis.__muonServiceInterface = definition.interface;
globalThis.__muonServiceRuntime = createServiceRuntime(definition, {
  callback: globalThis.__muonServiceCallback,
  emit: globalThis.__muonServiceEmit,
});
`,
    {
      sourceId: "muon-quickjs-service.fc",
      variables: new Map([["entry", JSON.stringify(entry)]]),
    },
  );
  if (source === undefined)
    throw new Error("Unable to generate the QuickJS service entry.");
  const virtual = join(dirname(entry), ".muon-service-entry.ts");
  const result = await vite.build({
    configFile: false,
    root: dirname(entry),
    publicDir: false,
    logLevel: "warn",
    plugins: [
      {
        name: "muon-quickjs-service",
        enforce: "pre",
        resolveId: (id) => {
          if (isBuiltin(id) || id.startsWith("node:"))
            throw new Error(`QuickJS services cannot import ${id}.`);
          if (id === virtual) return "\0" + virtual;
          return undefined;
        },
        load: (id) => (id === "\0" + virtual ? source : undefined),
      },
    ],
    build: {
      write: false,
      target: "es2022",
      minify: false,
      modulePreload: false,
      lib: { entry: virtual, formats: ["es"] },
      rolldownOptions: {
        output: { codeSplitting: false },
        onwarn: (warning) => {
          throw new Error(`QuickJS service build: ${warning.message}`);
        },
      },
    },
  });
  const outputs = Array.isArray(result) ? result : [result];
  if (outputs.length !== 1 || !("output" in outputs[0]!))
    throw new Error(
      "QuickJS service build must produce one JavaScript bundle.",
    );
  const files = outputs[0]!.output;
  if (
    files.length !== 1 ||
    files[0]!.type !== "chunk" ||
    files[0]!.imports.length ||
    files[0]!.dynamicImports.length
  )
    throw new Error(
      "QuickJS service bundles cannot depend on external modules or assets.",
    );
  return files[0]!.code;
};
