// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import {
  mkdir,
  mkdtemp,
  readFile,
  rm,
  symlink,
  writeFile,
} from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { runInNewContext } from "node:vm";
import { afterAll, beforeAll, expect, it } from "vitest";
import type { ServiceRuntime } from "muon-plugin-sdk/services";
import { resolveMuonServices } from "../common/service-config.ts";
import { stageMuonDesktopServices } from "../cef/services.ts";

let root: string;
beforeAll(async () => {
  root = await mkdtemp(join(tmpdir(), "muon-quickjs-config-"));
  await symlink(
    fileURLToPath(new URL("../../node_modules", import.meta.url)),
    join(root, "node_modules"),
    "dir",
  );
  await writeFile(
    join(root, "service.ts"),
    `
    import { defineService, defineServiceInterface } from 'muon-plugin-sdk/services';
    export default defineService({
      interface: defineServiceInterface({schemaVersion:1,methods:{
        echo:{parameters:['string'],returns:'string',errors:[]}
      },events:{}}),
      start: context => ({methods:{echo: (_call, text: string) => context.id + ':' + text},stop:async()=>{}})
    });
  `,
  );
});
afterAll(async () => {
  if (root) await rm(root, { recursive: true, force: true });
});

it("builds a portable TS service and stages executable source for both desktop architectures", async () => {
  const services = await resolveMuonServices(
    { worker: { runtime: "quickjs", entry: "service.ts", autostart: true } },
    root,
    ["linux-amd64", "windows-amd64"],
  );
  const definition = services[0]!;
  expect(definition.runtime).toBe("quickjs");
  if (definition.runtime !== "quickjs")
    throw new Error("Missing QuickJS service");
  for (const target of ["linux-amd64", "windows-amd64"] as const) {
    const output = join(root, target);
    await stageMuonDesktopServices(services, target, output);
    const registry = JSON.parse(
      await readFile(join(output, "muon-services.json"), "utf8"),
    );
    expect(registry.services.worker).toMatchObject({
      runtime: "quickjs",
      autostart: true,
      config: {},
    });
    const source = await readFile(
      join(output, registry.services.worker.script),
      "utf8",
    );
    const context: Record<string, unknown> = {
      __muonServiceCallback: async () => ({
        status: "error",
        code: "callback-failed",
        message: "unused",
      }),
      __muonServiceEmit: () => {},
    };
    runInNewContext(source, context);
    const runtime = context.__muonServiceRuntime as ServiceRuntime;
    await runtime.start({
      id: "worker",
      generation: "1",
      config: {},
      startReason: "acquire",
      dataDirectory: "/private",
    });
    expect(await runtime.invoke("1", "echo", ["日本語😀\0value"])).toEqual({
      status: "ok",
      value: "worker:日本語😀\0value",
    });
    await runtime.stop({ reason: "reference-released" });
  }
});

it("rejects Node builtins instead of producing a browser placeholder", async () => {
  await writeFile(
    join(root, "node-service.ts"),
    `import { readFile } from 'node:fs/promises'; export default readFile;`,
  );
  await expect(
    resolveMuonServices(
      { worker: { runtime: "quickjs", entry: "node-service.ts" } },
      root,
      ["linux-amd64"],
    ),
  ).rejects.toThrow(/QuickJS.*node:fs\/promises/);
});

it("bundles an npm dependency from the selected project and observes its build inputs", async () => {
  const project = join(root, "worker");
  const dependency = join(project, "node_modules/greeting");
  await mkdir(join(project, "src"), { recursive: true });
  await mkdir(dependency, { recursive: true });
  await writeFile(
    join(project, "package.json"),
    '{"dependencies":{"greeting":"1.0.0"}}',
  );
  await writeFile(
    join(dependency, "package.json"),
    '{"type":"module","exports":"./index.js"}',
  );
  await writeFile(
    join(dependency, "index.js"),
    'export const greet = value => "npm " + value;',
  );
  await writeFile(
    join(project, "src/service.ts"),
    'import { greet } from "greeting";\n' +
      (await readFile(join(root, "service.ts"), "utf8")).replace(
        "context.id + ':' + text",
        "greet(text)",
      ),
  );
  const observed = new Set<string>();
  const config = {
    worker: { runtime: "quickjs", project: "worker", entry: "src/service.ts" },
  };
  const [definition] = await resolveMuonServices(
    config,
    root,
    ["linux-amd64"],
    {
      root,
      environment: process.env,
      onInput: (input) => observed.add(input.path),
    },
  );
  if (definition?.runtime !== "quickjs")
    throw new Error("Missing QuickJS service");
  const context: Record<string, unknown> = {
    __muonServiceCallback: async () => {
      throw new Error("Unexpected callback");
    },
    __muonServiceEmit: () => {},
  };
  runInNewContext(definition.bundle, context);
  const service = context.__muonServiceRuntime as ServiceRuntime;
  await service.start({
    id: "worker",
    generation: "1",
    config: {},
    startReason: "acquire",
    dataDirectory: "/private",
  });
  expect(await service.invoke("1", "echo", ["test"])).toEqual({
    status: "ok",
    value: "npm test",
  });
  await service.stop({ reason: "reference-released" });
  for (const path of [
    join(project, "package.json"),
    join(project, "package-lock.json"),
    join(dependency, "package.json"),
    join(dependency, "index.js"),
  ])
    expect(observed, path).toContain(path);

  for (const [source, reason] of [
    ['import "node:fs";', "node:fs"],
    ['import "./native.node";', "native.node"],
  ] as const) {
    await writeFile(
      join(dependency, "index.js"),
      source + 'export const greet = () => "";',
    );
    await expect(
      resolveMuonServices(config, root, ["linux-amd64"]),
    ).rejects.toThrow(
      new RegExp("services\\.worker[\\s\\S]*QuickJS[\\s\\S]*" + reason),
    );
  }
});

it("requires a package manifest when an explicit QuickJS project is selected", async () => {
  await expect(
    resolveMuonServices(
      {
        worker: { runtime: "quickjs", project: ".", entry: "service.ts" },
      },
      root,
      ["linux-amd64"],
    ),
  ).rejects.toThrow(/services\.worker.*package\.json/);
});

it("rejects source bundles beyond the shared host size limit during the build", async () => {
  await writeFile(
    join(root, "large-service.ts"),
    `globalThis.payload = '${"x".repeat(8 * 1024 * 1024)}'; export default {};`,
  );
  let message = "";
  try {
    await resolveMuonServices(
      { worker: { runtime: "quickjs", entry: "large-service.ts" } },
      root,
      ["linux-amd64"],
    );
  } catch (error) {
    message = String(error);
  }
  expect(message).toMatch(/8 MiB/);
});

it.each([
  { runtime: "quickjs", entry: "service.ts", artifact: "native.json" },
  { runtime: "quickjs" },
  { runtime: "native", entry: "service.ts", artifact: "native.json" },
])("rejects ambiguous or missing runtime inputs %j", async (definition) => {
  await expect(
    resolveMuonServices({ worker: definition }, root, ["linux-amd64"]),
  ).rejects.toThrow(/services.worker/);
});

it("packages declared binary assets by logical name and observes changes to their source", async () => {
  const asset = join(root, "initial.bin");
  await writeFile(asset, new Uint8Array([0, 255]));
  const inputs: string[] = [];
  const declaration = {
    runtime: "quickjs",
    entry: "service.ts",
    io: {
      fs: {
        data: "read-write",
        assets: { "defaults/value": "initial.bin" },
      },
    },
  };
  const services = await resolveMuonServices(
    { files: declaration },
    root,
    ["linux-amd64"],
    {
      root,
      environment: process.env,
      onInput: (input) => {
        inputs.push(input.path);
      },
    },
  );
  const output = join(root, "asset-output");
  await stageMuonDesktopServices(services, "linux-amd64", output);
  const registry = JSON.parse(
    await readFile(join(output, "muon-services.json"), "utf8"),
  );
  expect(registry.services.files.io.fs).toMatchObject({
    data: "read-write",
    assets: { "defaults/value": "AP8=" },
  });
  expect(inputs).toContain(asset);
  await expect(
    resolveMuonServices(
      {
        files: {
          ...declaration,
          io: { fs: { assets: { initial: "../outside" } } },
        },
      },
      root,
      ["linux-amd64"],
    ),
  ).rejects.toThrow(/outside|escapes/iu);
});
