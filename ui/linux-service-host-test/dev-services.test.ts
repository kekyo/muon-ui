// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { once } from "node:events";
import {
  chmod,
  cp,
  mkdir,
  mkdtemp,
  readFile,
  readdir,
  rm,
  writeFile,
} from "node:fs/promises";
import { join, resolve } from "node:path";
import { pathToFileURL } from "node:url";
import { afterEach, expect, it, vi } from "vitest";
import { build, createServer } from "vite";
import { runMuonDev } from "../cef/dev.ts";
import { runMuonPrepare } from "../cef/prepare.ts";
import { startMuonDevelopmentServices } from "../cef/development-services.ts";
import { buildMuonQuickJsService } from "../common/service-build.ts";
import { startMuonViteBrowserBridge } from "../cef/vite-internals.ts";
import * as serviceWatch from "../cef/development-service-watch.ts";

// Runtime downloading is orthogonal to service preparation. The launched driver
// uses the real service host and the common page API to report its result.
vi.mock(import("../cef/prepare.ts"), async (original) => ({
  ...(await original()),
  runMuonPrepare: vi.fn(),
}));

const directories: string[] = [];
afterEach(async () => {
  for (const directory of directories.splice(0))
    await rm(directory, { recursive: true, force: true });
  vi.mocked(runMuonPrepare).mockReset();
});

it("closes service input monitors when Vite exits during their initial scan", async () => {
  const root = await mkdtemp(resolve("../.build/dev-service-watch-close-"));
  directories.push(root);
  const stagePath = join(root, ".muon/linux-amd64");
  await mkdir(stagePath, { recursive: true });
  await writeFile(
    join(root, "index.html"),
    "<title>Close during startup</title>",
  );
  vi.mocked(runMuonPrepare).mockResolvedValue({
    stagePath,
    muonPath: stagePath,
    cefPath: stagePath,
    cacheHit: true,
  });
  let scanned!: () => void;
  const scanning = new Promise<void>((resolve) => {
    scanned = resolve;
  });
  let finish!: () => void;
  const gate = new Promise<void>((resolve) => {
    finish = resolve;
  });
  let monitorClosed = false;
  let monitor: serviceWatch.MuonDevelopmentServiceWatcher | undefined;
  const createWatcher = serviceWatch.createMuonDevelopmentServiceWatcher;
  vi.spyOn(
    serviceWatch,
    "createMuonDevelopmentServiceWatcher",
  ).mockImplementationOnce(async (...args) => {
    const watcher = await createWatcher(...args);
    monitor = watcher;
    scanned();
    await gate;
    return {
      ...watcher,
      close: async () => {
        await watcher.close();
        monitorClosed = true;
      },
    };
  });
  const server = await createServer({
    root,
    configFile: false,
    logLevel: "silent",
    server: { port: 0 },
  });
  try {
    await startMuonViteBrowserBridge({
      server,
      pluginOptions: { exitWithServer: false },
      refreshRuntimePluginConfig: async () => ({ mode: "simple" }),
      platform: "linux",
      architecture: "x64",
      environment: process.env,
    });
    await server.listen();
    await scanning;
    const closed = once(server.httpServer!, "close");
    const closing = server.close();
    await closed;
    finish();
    await closing;
    expect(monitorClosed).toBe(true);
    expect(await readdir(join(root, ".muon/development/linux-amd64"))).toEqual(
      [],
    );
  } finally {
    finish();
    await server.close();
    await monitor?.close();
  }
});

it.each([
  ["run", 0, "quickjs"],
  ["vite", 0, "quickjs"],
  ["run", 7, "quickjs"],
  ["vite", 7, "quickjs"],
  ["run", 0, "node"],
  ["vite", 0, "node"],
] as const)(
  "executes a config-relative service prepared by %s and cleans up after UI exit %s (runtime=%s)",
  async (command, exitCode, runtime) => {
    await mkdir("../.build", { recursive: true });
    const root = await mkdtemp(resolve("../.build/dev-services-"));
    directories.push(root);
    const stagePath = join(root, ".muon", "linux-amd64");
    const settings = join(root, "settings");
    await mkdir(stagePath, { recursive: true });
    await mkdir(settings);
    await writeFile(
      join(settings, "package.json"),
      JSON.stringify({
        name: "service-project",
        type: "module",
        engines: { node: ">=22.12" },
      }),
    );
    await mkdir(join(root, "assets"));
    await writeFile(
      join(root, "index.html"),
      "<!doctype html><title>Services</title>",
    );
    await writeFile(
      join(settings, "worker.ts"),
      `import { defineService, defineServiceInterface } from "muon-plugin-sdk/services";
export default defineService({
  interface: defineServiceInterface({ schemaVersion: 1, methods: {
    greet: { parameters: ["string"], returns: "string", errors: [] }
  }, events: {} }),
  start: (context) => ({
    methods: { greet: (_call, name) => context.config.prefix + name },
    stop: async () => {}
  })
});\n`,
    );
    if (runtime === "node") {
      await cp(
        resolve("../sdk/examples/services/node-files.ts"),
        join(settings, "worker.ts"),
      );
      await cp(
        resolve("../sdk/examples/services/node-greeting.txt"),
        join(settings, "node-greeting.txt"),
      );
    }
    const configPath =
      command === "run" ? join(settings, "muon.json") : join(root, "muon.json");
    await writeFile(
      configPath,
      JSON.stringify({
        services: {
          worker: {
            runtime,
            ...(runtime === "node"
              ? { project: command === "run" ? "." : "./settings" }
              : {}),
            entry:
              command === "run" || runtime === "node"
                ? "./worker.ts"
                : "./settings/worker.ts",
            config: { prefix: "Hello " },
            autostart: exitCode !== 0,
          },
        },
      }),
    );
    await cp(
      resolve("cef-test/fixtures/dev-service-ui.mjs"),
      join(stagePath, "muon-core"),
    );
    await chmod(join(stagePath, "muon-core"), 0o755);
    for (const name of ["muon-service-host", "libcardio.so"])
      await cp(
        resolve("../core/service-host/.build", name),
        join(stagePath, name),
      );
    if (runtime === "node") {
      await mkdir(join(stagePath, "runtimes/node/bin"), { recursive: true });
      await cp(process.execPath, join(stagePath, "runtimes/node/bin/node"));
      await cp(
        resolve("../node/dist/node-service-bridge.mjs"),
        join(stagePath, "node-service-bridge.mjs"),
      );
    }
    vi.mocked(runMuonPrepare).mockResolvedValue({
      stagePath,
      muonPath: stagePath,
      cefPath: stagePath,
      cacheHit: true,
    });
    const resultPath = join(root, "result.json");
    await build({
      configFile: false,
      logLevel: "silent",
      build: {
        outDir: join(root, "renderer"),
        target: "es2022",
        minify: false,
        lib: {
          entry: resolve("../core/common/renderer/services.ts"),
          formats: ["es"],
          fileName: () => "services.mjs",
        },
      },
    });
    const environment = {
      ...process.env,
      MUON_TEST_RESULT: resultPath,
      MUON_TEST_EXIT: String(exitCode),
      MUON_TEST_RENDERER: pathToFileURL(join(root, "renderer", "services.mjs"))
        .href,
    };
    if (command === "run") {
      const result = await runMuonDev({
        root,
        configPath,
        assetSourcePath: "assets",
        environment,
      });
      expect(result.exitCode).toBe(exitCode);
    } else {
      const server = await createServer({
        root,
        configFile: false,
        logLevel: "silent",
        server: { port: 0 },
      });
      const closed = once(server.httpServer!, "close");
      const previousExitCode = process.exitCode;
      try {
        await startMuonViteBrowserBridge({
          server,
          pluginOptions: {},
          refreshRuntimePluginConfig: async () => ({ mode: "simple" }),
          platform: "linux",
          architecture: "x64",
          environment,
        });
        await server.listen();
        await closed;
        await server.close();
        if (exitCode !== 0) expect(process.exitCode).toBe(1);
      } finally {
        await server.close();
        process.exitCode = previousExitCode;
      }
    }
    expect(JSON.parse(await readFile(resultPath, "utf8"))).toEqual({
      status: "ok",
      value: "Hello service",
    });
    if (runtime === "node")
      expect(
        vi.mocked(runMuonPrepare).mock.calls[0]![0].nodeRuntimeRequirement,
      ).toMatchObject({ required: true, engineRange: ">=22.12" });
    expect(
      await readdir(join(root, ".muon", "development", "linux-amd64")),
    ).toEqual([]);
  },
);

it("rejects an invalid service before preparing or launching the UI", async () => {
  await mkdir("../.build", { recursive: true });
  const root = await mkdtemp(resolve("../.build/invalid-dev-service-"));
  directories.push(root);
  await mkdir(join(root, "assets"));
  await writeFile(
    join(root, "muon.json"),
    JSON.stringify({ services: { worker: { runtime: "unknown" } } }),
  );
  await expect(runMuonDev({ root, assetSourcePath: "assets" })).rejects.toThrow(
    "services.worker.runtime",
  );
  expect(runMuonPrepare).not.toHaveBeenCalled();
});

it.each([false, true])(
  "removes a failed development startup (canceled=%s)",
  async (canceled) => {
    await mkdir("../.build", { recursive: true });
    const root = await mkdtemp(resolve("../.build/failed-dev-service-"));
    directories.push(root);
    const cancellation = new AbortController();
    if (canceled) cancellation.abort();
    const bundle = await buildMuonQuickJsService(
      resolve("../sdk/test/fixtures/quickjs-service.ts"),
      [],
    );
    await expect(
      startMuonDevelopmentServices(
        root,
        "linux-amd64",
        join(root, "missing-runtime"),
        [
          {
            id: "worker",
            runtime: "quickjs",
            autostart: true,
            config: {},
            android: undefined,
            plugins: [],
            bundle,
          },
        ],
        process.env,
        cancellation.signal,
      ),
    ).rejects.toThrow(canceled ? "session ended during startup" : "ENOENT");
    expect(
      await readdir(join(root, ".muon", "development", "linux-amd64")),
    ).toEqual([]);
  },
);
