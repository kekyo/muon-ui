// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { mkdtemp, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { pathToFileURL } from "node:url";
import { afterEach, expect, it, vi } from "vitest";
import { runMuonBuildSequence } from "../common/build-sequence.js";

const build = vi.hoisted(() =>
  vi.fn(async (options) => ({
    root: options.root,
    appName: "app",
    appId: "app",
    targets: [],
  })),
);
vi.mock("../common/build.js", () => ({
  buildMuonApp: build,
  resolveMuonNodeProjectForBuildConfig: async () => undefined,
}));
const directories: string[] = [];
afterEach(async () => {
  for (const directory of directories.splice(0))
    await rm(directory, { recursive: true, force: true });
  build.mockClear();
  Reflect.deleteProperty(globalThis, "__muon_plugin_call");
});

it.each([true, false])(
  "carries the bundled Android capability into CLI packaging (target in Vite: %s)",
  async (configuredTarget) => {
    const root = await mkdtemp(join(tmpdir(), "muon-android-sequence-"));
    directories.push(root);
    await writeFile(
      join(root, "package.json"),
      JSON.stringify({ name: "app", type: "module" }),
    );
    await writeFile(
      join(root, "main.ts"),
      'export { getConfigValues } from "muon:environments";',
    );
    await writeFile(
      join(root, "vite.config.mts"),
      `
import muon from ${JSON.stringify(new URL("../dist/vite.mjs", import.meta.url).href)};
export default {
  build: { target: 'esnext', minify: false, lib: { entry: 'main.ts', formats: ['es'], fileName: () => 'app.mjs' } },
  plugins: [muon({
    ${configuredTarget ? "build: { targets: ['android'] }," : ""}
    pluginAccess: { mode: 'validate', plugins: [{ name: 'internal', imports: [{ sources: ['main.ts'], allow: ['muon.environments.getConfigValues'] }] }] }
  })]
};`,
    );
    await runMuonBuildSequence({ root, targets: ["android"] });
    const policy = build.mock.calls[0]![0].runtimePluginConfig;
    expect(policy?.mode).toBe("validate");
    expect(policy.capabilities).toHaveLength(1);
    const calls: unknown[][] = [];
    Reflect.set(
      globalThis,
      "__muon_plugin_call",
      async (...args: unknown[]) => {
        calls.push(args);
        return { channel: "cli" };
      },
    );
    const module = await import(pathToFileURL(join(root, "dist/app.mjs")).href);
    await expect(module.getConfigValues()).resolves.toEqual({ channel: "cli" });
    expect(calls).toEqual([
      [policy.capabilities[0].id, "muon.environments.getConfigValues", []],
    ]);
  },
);
