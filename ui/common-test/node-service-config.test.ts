// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile } from "node:child_process";
import {
  mkdir,
  mkdtemp,
  readFile,
  rename,
  rm,
  writeFile,
} from "node:fs/promises";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { pathToFileURL } from "node:url";
import { promisify } from "node:util";
import { afterEach, expect, it } from "vitest";
import { resolveMuonServices } from "../common/service-config.ts";
import { stageMuonDesktopServices } from "../cef/services.ts";

const roots: string[] = [];
afterEach(async () => {
  for (const root of roots.splice(0))
    await rm(root, { recursive: true, force: true });
});

const createProject = async () => {
  const root = await mkdtemp(join(tmpdir(), "muon-node-service-"));
  roots.push(root);
  const source = join(root, "producer");
  const project = join(source, "worker");
  await mkdir(join(project, "src/nested"), { recursive: true });
  await writeFile(
    join(project, "package.json"),
    JSON.stringify({
      name: "worker",
      type: "module",
      engines: { node: ">=22.12" },
      dependencies: { "fixture-dependency": "1.0.0" },
    }),
  );
  const dependency = join(source, "node_modules/fixture-dependency");
  await mkdir(dependency, { recursive: true });
  await writeFile(
    join(dependency, "package.json"),
    JSON.stringify({
      name: "fixture-dependency",
      type: "module",
      exports: { ".": "./index.mjs" },
      dependencies: { "fixture-transitive": "1.0.0" },
    }),
  );
  await writeFile(
    join(dependency, "index.mjs"),
    'import value from "fixture-transitive"; export default "npm:" + value;',
  );
  const transitive = join(source, "node_modules/fixture-transitive");
  await mkdir(transitive, { recursive: true });
  await writeFile(
    join(transitive, "package.json"),
    JSON.stringify({
      name: "fixture-transitive",
      type: "module",
      exports: "./index.mjs",
    }),
  );
  await writeFile(
    join(transitive, "index.mjs"),
    'export default "transitive";',
  );
  await writeFile(join(project, "src/nested/data.txt"), "日本語 asset");
  await writeFile(
    join(project, "src/nested/read.ts"),
    `import { readFile } from "node:fs/promises";
    export const read = async (): Promise<string> => await readFile(new URL("./data.txt", import.meta.url), "utf8");`,
  );
  await writeFile(
    join(project, "src/service.ts"),
    `import value from "fixture-dependency";
    import { read } from "./nested/read.ts";
    export default async (): Promise<string> => value + ":" + await read();`,
  );
  return {
    root,
    source,
    project,
    config: {
      worker: {
        runtime: "node",
        project: "./worker",
        entry: "./src/service.ts",
        autostart: true,
        config: { role: "writer" },
      },
    },
  };
};

it("executes TS, npm dependencies and module-relative assets after relocating and deleting the source", async () => {
  const fixture = await createProject();
  const definitions = await resolveMuonServices(
    fixture.config,
    fixture.source,
    ["linux-amd64", "windows-amd64"],
  );
  const output = join(fixture.root, "output");
  await stageMuonDesktopServices(definitions, "linux-amd64", output);
  const relocated = join(fixture.root, "移動先 with spaces");
  await rename(output, relocated);
  await rm(fixture.source, { recursive: true, force: true });
  const registry = JSON.parse(
    await readFile(join(relocated, "muon-services.json"), "utf8"),
  );
  const worker = registry.services.worker;
  expect(worker).toMatchObject({
    runtime: "node",
    autostart: true,
    config: { role: "writer" },
  });
  const entry = pathToFileURL(
    resolve(relocated, worker.project, worker.entry),
  ).href;
  const result = await promisify(execFile)(
    process.execPath,
    [
      "--input-type=module",
      "-e",
      `const { default: run } = await import(${JSON.stringify(entry)}); console.log(await run());`,
    ],
    { cwd: relocated },
  );
  expect(result.stdout.trim()).toBe("npm:transitive:日本語 asset");
});

it("rejects Node services for Android before trying to build the source", async () => {
  await expect(
    resolveMuonServices(
      { worker: { runtime: "node", project: "missing", entry: "missing.ts" } },
      "/missing",
      ["linux-amd64", "android-arm64-v8a"],
    ),
  ).rejects.toThrow(/services.worker.*node.*android-arm64-v8a/);
});

it("preserves a hoisted package shared by two npm dependencies", async () => {
  const fixture = await createProject();
  await writeFile(
    join(fixture.project, "package.json"),
    JSON.stringify({
      type: "module",
      dependencies: {
        "fixture-dependency": "1.0.0",
        "fixture-reader": "1.0.0",
      },
    }),
  );
  await writeFile(
    join(fixture.source, "node_modules/fixture-transitive/index.mjs"),
    "let count = 0; export const increment = () => ++count; export const read = () => count;",
  );
  await writeFile(
    join(fixture.source, "node_modules/fixture-dependency/index.mjs"),
    'import { increment } from "fixture-transitive"; export default increment;',
  );
  await mkdir(join(fixture.source, "node_modules/fixture-reader"));
  await writeFile(
    join(fixture.source, "node_modules/fixture-reader/package.json"),
    JSON.stringify({
      name: "fixture-reader",
      type: "module",
      exports: "./index.mjs",
      dependencies: { "fixture-transitive": "1.0.0" },
    }),
  );
  await writeFile(
    join(fixture.source, "node_modules/fixture-reader/index.mjs"),
    'export { read as default } from "fixture-transitive";',
  );
  await writeFile(
    join(fixture.project, "src/service.ts"),
    'import increment from "fixture-dependency"; import read from "fixture-reader"; export default () => { increment(); return read(); };',
  );
  const definitions = await resolveMuonServices(
    fixture.config,
    fixture.source,
    ["linux-amd64"],
  );
  const output = join(fixture.root, "output");
  await stageMuonDesktopServices(definitions, "linux-amd64", output);
  await rm(fixture.source, { recursive: true, force: true });
  const worker = JSON.parse(
    await readFile(join(output, "muon-services.json"), "utf8"),
  ).services.worker;
  const entry = pathToFileURL(
    resolve(output, worker.project, worker.entry),
  ).href;
  const result = await promisify(execFile)(
    process.execPath,
    [
      "--input-type=module",
      "-e",
      `const {default: run} = await import(${JSON.stringify(entry)}); console.log(run());`,
    ],
    { cwd: output },
  );
  expect(result.stdout.trim()).toBe("1");
});

it.each(["dependencies", "optionalDependencies"])(
  "rejects an imported package that is absent from the installed %s",
  async (field) => {
    const fixture = await createProject();
    await writeFile(
      join(fixture.project, "package.json"),
      JSON.stringify({
        type: "module",
        [field]: { "missing-service-package": "1.0.0" },
      }),
    );
    await writeFile(
      join(fixture.project, "src/service.ts"),
      'import value from "missing-service-package"; export default () => value;',
    );
    const definitions = await resolveMuonServices(
      fixture.config,
      fixture.source,
      ["linux-amd64"],
    );
    await expect(
      stageMuonDesktopServices(
        definitions,
        "linux-amd64",
        join(fixture.root, "output"),
      ),
    ).rejects.toThrow(/missing-service-package.*not installed/);
  },
);

it("lets a service handle an absent optional dynamic import", async () => {
  const fixture = await createProject();
  await writeFile(
    join(fixture.project, "package.json"),
    JSON.stringify({
      type: "module",
      optionalDependencies: { "missing-service-package": "1.0.0" },
    }),
  );
  await writeFile(
    join(fixture.project, "src/service.ts"),
    'export default async () => { try { return (await import("missing-service-package")).default; } catch { return "fallback"; } };',
  );
  const definitions = await resolveMuonServices(
    fixture.config,
    fixture.source,
    ["linux-amd64"],
  );
  const output = join(fixture.root, "output");
  await stageMuonDesktopServices(definitions, "linux-amd64", output);
  const worker = JSON.parse(
    await readFile(join(output, "muon-services.json"), "utf8"),
  ).services.worker;
  const entry = pathToFileURL(
    resolve(output, worker.project, worker.entry),
  ).href;
  const result = await promisify(execFile)(
    process.execPath,
    [
      "--input-type=module",
      "-e",
      `const {default: run} = await import(${JSON.stringify(entry)}); console.log(await run());`,
    ],
    { cwd: output },
  );
  expect(result.stdout.trim()).toBe("fallback");
});

it.each([
  {},
  { entry: "../outside.ts" },
  { artifact: "native.json" },
  { plugins: [] },
])("rejects incomplete or ambiguous Node inputs %j", async (extra) => {
  const fixture = await createProject();
  const definition =
    Object.keys(extra).length === 0
      ? { runtime: "node" }
      : { ...fixture.config.worker, ...extra };
  await expect(
    resolveMuonServices({ worker: definition }, fixture.source, [
      "linux-amd64",
    ]),
  ).rejects.toThrow(/services.worker/);
});
