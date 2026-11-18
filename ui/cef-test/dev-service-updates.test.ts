// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { EventEmitter, once } from "node:events";
import { execFile } from "node:child_process";
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
import { createServer as createHttpServer } from "node:http";
import { createConnection, type Socket } from "node:net";
import { join, resolve } from "node:path";
import { createInterface } from "node:readline";
import { promisify } from "node:util";
import { buildService, createServiceProject } from "muon-plugin-sdk";
import { readServiceArtifact } from "muon-plugin-sdk/artifact";
import { createLogger, createServer, type ViteDevServer } from "vite";
import { expect, it, vi } from "vitest";
import { createMuonServicesApi } from "../../core/common/renderer/services.ts";
import { runMuonPrepare } from "../cef/prepare.ts";
import { startMuonViteBrowserBridge } from "../cef/vite-internals.ts";

vi.mock(import("../cef/prepare.ts"), async (original) => ({
  ...(await original()),
  runMuonPrepare: vi.fn(),
}));

it.each(["native", "dependency", "artifact", "quickjs", "node"] as const)(
  "updates %s service inputs, retains the old service after a failed build, and retries",
  async (runtime) => {
    await mkdir("../.build", { recursive: true });
    const root = await mkdtemp(resolve("../.build/vite-service-updates-"));
    const events = new EventEmitter();
    let server: ViteDevServer | undefined;
    const sockets: Socket[] = [];
    let endpoint = "";
    let diagnostics = "";
    let unblockPreparation: (() => void) | undefined;
    let uiStarts = 0;
    const observer = createHttpServer(async (request, response) => {
      let body = "";
      for await (const chunk of request) body += String(chunk);
      response.end("ok");
      uiStarts++;
      events.emit("ui", JSON.parse(body));
    });
    const next = async (name: string) => {
      try {
        return (
          await once(
            events,
            name === "update" || name === "full-reload" ? "ws:" + name : name,
            { signal: AbortSignal.timeout(15000) },
          )
        )[0];
      } catch (error) {
        throw new Error(`Waiting for ${name}: ${diagnostics}`, {
          cause: error,
        });
      }
    };
    const connect = async () => {
      const socket = createConnection(endpoint);
      sockets.push(socket);
      await once(socket, "connect");
      const pending = new Map<
        string,
        { resolve: (value: unknown) => void; reject: (error: Error) => void }
      >();
      let sequence = 0;
      createInterface({ input: socket }).on("line", (line) => {
        const message = JSON.parse(line);
        const completion = pending.get(message.requestId);
        if (!completion) return;
        pending.delete(message.requestId);
        if (message.error) completion.reject(new Error(message.error.message));
        else completion.resolve(message.result);
      });
      socket.on("close", () => {
        for (const completion of pending.values())
          completion.reject(new Error("Disconnected"));
        pending.clear();
      });
      return createMuonServicesApi(async (op, json) => {
        if (socket.destroyed) throw new Error("Disconnected");
        const requestId = String(++sequence);
        const response = new Promise<unknown>((resolve, reject) =>
          pending.set(requestId, { resolve, reject }),
        );
        socket.write(
          JSON.stringify({ ...JSON.parse(json), op, requestId }) + "\n",
        );
        return JSON.stringify(await response);
      });
    };
    try {
      const project = join(root, "worker");
      const stagePath = join(root, ".muon/linux-amd64");
      await mkdir(stagePath, { recursive: true });
      await mkdir(project);
      let definition: Record<string, unknown>;
      let changedFile: string;
      let changedText: string;
      let invalidFile: string;
      let rebuildDependency: ((value: string) => Promise<void>) | undefined;
      if (
        runtime === "native" ||
        runtime === "dependency" ||
        runtime === "artifact"
      ) {
        await createServiceProject({
          directory: project,
          name: "worker",
          language: "c",
          buildSystem: "make",
        });
        const example = await readFile(
          resolve("../sdk/examples/services/native-echo.c"),
          "utf8",
        );
        await writeFile(
          join(project, "service.c"),
          example
            .replace(
              "#include <stdlib.h>",
              '#include <stdlib.h>\n#include "greeting.h"',
            )
            .replace(
              "complete(data, &arguments[0], NULL);",
              "(void)arguments;\n  const muon_service_value result = {MUON_SERVICE_VALUE_STRING, {.string_value = {GREETING, sizeof(GREETING) - 1}}};\n  complete(data, &result, NULL);",
            ),
        );
        changedFile = join(project, "greeting.h");
        await writeFile(changedFile, '#define GREETING "Before"\n');
        changedText = '#define GREETING "After"\n';
        invalidFile = changedFile;
        definition = {
          runtime: "native",
          source: "./worker/muon-plugin.build.json",
          autostart: true,
        };
        if (runtime === "dependency") {
          const dependency = join(root, "dependency");
          await mkdir(dependency);
          changedFile = join(dependency, "libgreeting.so");
          invalidFile = changedFile;
          rebuildDependency = async (value) => {
            const source = join(dependency, "value.c");
            await writeFile(
              source,
              `const char* greeting(void) { return ${JSON.stringify(value)}; }\n`,
            );
            await promisify(execFile)("gcc", [
              "-std=c99",
              "-shared",
              "-fPIC",
              "-Wl,-soname,libgreeting.so",
              source,
              "-o",
              changedFile,
            ]);
          };
          await rebuildDependency("Before");
          await writeFile(
            join(project, "service.c"),
            example
              .replace(
                "#include <stdlib.h>",
                "#include <stdlib.h>\n#include <string.h>\nextern const char* greeting(void);",
              )
              .replace(
                "complete(data, &arguments[0], NULL);",
                "(void)arguments;\n  const char* value = greeting();\n  const muon_service_value result = {MUON_SERVICE_VALUE_STRING, {.string_value = {value, strlen(value)}}};\n  complete(data, &result, NULL);",
              ),
          );
          const buildPath = join(project, "muon-plugin.build.json");
          const build = JSON.parse(await readFile(buildPath, "utf8"));
          build.build = {
            system: "make",
            args: ["LDLIBS=-L{{source}}/../dependency -lgreeting"],
          };
          build.dependencies = {
            "linux-amd64": ["{{source}}/../dependency/libgreeting.so"],
          };
          await writeFile(buildPath, JSON.stringify(build));
        }
        if (runtime === "artifact") {
          const artifact = await buildService({
            configPath: join(project, "muon-plugin.build.json"),
            target: "linux-amd64",
            outputDirectory: join(root, "prebuilt"),
          });
          definition = { runtime: "native", artifact, autostart: true };
          invalidFile = (await readServiceArtifact(artifact, ["linux-amd64"]))
            .targets["linux-amd64"]!.library;
        }
      } else {
        await writeFile(
          join(project, "package.json"),
          JSON.stringify({ name: "update-worker", type: "module" }),
        );
        await writeFile(
          join(project, "entry.ts"),
          `import { defineService, defineServiceInterface } from "muon-plugin-sdk/services";
import { greeting } from ${JSON.stringify(runtime === "quickjs" ? "quickjs-greeting" : "./greeting.ts")};
export default defineService({ interface: defineServiceInterface({ schemaVersion: 1, methods: {
  echo: { parameters: ["string"], returns: "string", errors: [] }
}, events: {} }), start: () => ({ methods: { echo: async () => await greeting() }, stop: async () => {} }) });\n`,
        );
        invalidFile = join(project, "greeting.ts");
        if (runtime === "quickjs") {
          const dependency = join(project, "node_modules/quickjs-greeting");
          await mkdir(dependency, { recursive: true });
          await writeFile(
            join(dependency, "package.json"),
            '{"name":"quickjs-greeting","type":"module","exports":"./greeting.ts"}',
          );
          invalidFile = join(dependency, "greeting.ts");
          changedFile = invalidFile;
          await writeFile(
            changedFile,
            'export const greeting = () => "Before";\n',
          );
          changedText = 'export const greeting = () => "After";\n';
        } else {
          changedFile = join(project, "greeting.txt");
          await writeFile(changedFile, "Before");
          changedText = "After";
          await writeFile(
            invalidFile,
            'import { readFile } from "node:fs/promises";\nimport { suffix } from "muon-update-dep";\nexport const greeting = async () => await readFile(new URL("./greeting.txt", import.meta.url), "utf8") + suffix;\n',
          );
          await mkdir(join(project, "node_modules/muon-update-dep"), {
            recursive: true,
          });
          await writeFile(
            join(project, "node_modules/muon-update-dep/package.json"),
            JSON.stringify({
              name: "muon-update-dep",
              type: "module",
              exports: "./index.js",
            }),
          );
          await writeFile(
            join(project, "node_modules/muon-update-dep/index.js"),
            'export const suffix = "";\n',
          );
          await mkdir(join(stagePath, "runtimes/node/bin"), {
            recursive: true,
          });
          await mkdir(join(stagePath, "plugins"));
          await cp(process.execPath, join(stagePath, "runtimes/node/bin/node"));
          await cp(
            resolve("../node/dist/node-bridge.mjs"),
            join(stagePath, "plugins/node-bridge.mjs"),
          );
          await cp(
            resolve(
              "../core/cef/.run/test-linux-amd64-release/plugins/node.so",
            ),
            join(stagePath, "plugins/node.so"),
          );
        }
        definition = {
          runtime,
          project: "./worker",
          entry: "./entry.ts",
          autostart: true,
        };
      }
      await writeFile(
        join(root, "muon.json"),
        JSON.stringify({ services: { worker: definition } }),
      );
      await writeFile(
        join(root, "index.html"),
        '<script type="module" src="/main.ts"></script>',
      );
      await writeFile(
        join(root, "main.ts"),
        'export const value = "Before UI"; if (import.meta.hot) import.meta.hot.accept();',
      );
      for (const name of ["muon-service-host", "libcardio.so"])
        await cp(
          resolve("../core/service-host/.build", name),
          join(stagePath, name),
        );
      await writeFile(
        join(stagePath, "muon-core"),
        `#!/usr/bin/env node\nconst fs = require("node:fs");\nconst config = fs.readFileSync(process.argv.at(-1), "utf8");\n(async () => { await fetch(process.env.MUON_TEST_OBSERVER, { method: "POST", body: config }); setInterval(() => {}, 1000); })();\n`,
      );
      await chmod(join(stagePath, "muon-core"), 0o755);
      vi.mocked(runMuonPrepare).mockResolvedValue({
        stagePath,
        muonPath: stagePath,
        cefPath: stagePath,
        cacheHit: true,
      });
      observer.listen(0, "127.0.0.1");
      await once(observer, "listening");
      const address = observer.address();
      if (!address || typeof address === "string")
        throw new Error("Missing observer address");
      const logger = createLogger("silent");
      logger.warn = (message) => {
        diagnostics += message + "\n";
        events.emit("warning", message);
      };
      logger.info = (message) => {
        if (message === "muon services updated") events.emit("updated");
      };
      server = await createServer({
        root,
        configFile: false,
        customLogger: logger,
        server: {
          port: 0,
          watch: { ignored: (path) => path.split(/[\\/]/u).includes(".muon") },
        },
      });
      const send = server.ws.send.bind(server.ws);
      server.ws.send = ((message: unknown, ...rest: unknown[]) => {
        if (
          typeof message === "object" &&
          message !== null &&
          "type" in message
        )
          events.emit("ws:" + String(message.type), message);
        return Reflect.apply(send, server!.ws, [message, ...rest]);
      }) as typeof server.ws.send;
      await startMuonViteBrowserBridge({
        server,
        pluginOptions: { exitWithServer: false },
        refreshRuntimePluginConfig: async () => ({ mode: "simple" }),
        platform: "linux",
        architecture: "x64",
        environment: {
          ...process.env,
          MUON_TEST_OBSERVER: `http://127.0.0.1:${address.port}`,
        },
      });
      const ready = next("ui");
      await server.listen();
      const config = await ready;
      endpoint = config.serviceHost.endpoint;
      const original = await connect();
      const old = await original.acquire("worker");
      expect(await old.call("echo", [""]), diagnostics).toBe("Before");
      await server.transformRequest("/main.ts");
      const hmr = next("update");
      await writeFile(
        join(root, "main.ts"),
        'export const value = "After UI"; if (import.meta.hot) import.meta.hot.accept();',
      );
      await hmr;
      expect(await old.call("echo", [""])).toBe("Before");
      await original.setAutostart("worker", false);
      const updated = next("full-reload");
      if (rebuildDependency) await rebuildDependency("After");
      else await writeFile(changedFile, changedText);
      if (runtime === "artifact")
        await buildService({
          configPath: join(project, "muon-plugin.build.json"),
          target: "linux-amd64",
          outputDirectory: join(root, "prebuilt"),
        });
      await updated;
      await expect(old.call("echo", [""])).rejects.toThrow();
      const current = await connect();
      expect(await current.getState("worker")).toMatchObject({
        autostart: false,
        resident: false,
      });
      const handle = await current.acquire("worker");
      expect(await handle.call("echo", [""])).toBe("After");
      const valid = await readFile(invalidFile);
      const failed = next("warning");
      await writeFile(invalidFile, "! invalid service syntax !\n");
      expect(await failed).toContain("service update failed");
      expect(await handle.call("echo", [""])).toBe("After");
      const recovered = next("full-reload");
      await writeFile(invalidFile, valid);
      await recovered;
      const recovery = await connect();
      const final = await recovery.acquire("worker");
      expect(await final.call("echo", [""])).toBe("After");
      await final.release();
      if (runtime === "node") {
        const dependencyUpdated = next("full-reload");
        await writeFile(
          join(project, "node_modules/muon-update-dep/index.js"),
          'export const suffix = "/npm";\n',
        );
        await dependencyUpdated;
        const npmApi = await connect();
        const npmHandle = await npmApi.acquire("worker");
        expect(await npmHandle.call("echo", [""])).toBe("After/npm");
        await npmApi.setAutostart("worker", true);
        const startupFailed = next("warning");
        await writeFile(
          invalidFile,
          valid.toString() + '\nthrow new Error("author startup failure");\n',
        );
        expect(await startupFailed).toContain("author startup failure");
        await expect(npmHandle.call("echo", [""])).rejects.toThrow();
        const startupRecovered = next("full-reload");
        await writeFile(invalidFile, valid);
        await startupRecovered;
        const newApi = await connect();
        const newHandle = await newApi.acquire("worker");
        expect(await newHandle.call("echo", [""])).toBe("After/npm");
        await newHandle.release();

        // Hold runtime preparation while a second edit arrives. Both updates
        // must finish in order, without overwriting the runtime still in use.
        const gate = new Promise<void>((resolve) => {
          unblockPreparation = resolve;
        });
        vi.mocked(runMuonPrepare).mockImplementationOnce(async (options) => {
          events.emit("preparing", options.stageDir);
          await gate;
          await cp(stagePath, options.stageDir!, { recursive: true });
          return {
            stagePath: options.stageDir!,
            muonPath: options.stageDir!,
            cefPath: options.stageDir!,
            cacheHit: true,
          };
        });
        const preparing = next("preparing");
        await writeFile(
          join(project, "package.json"),
          JSON.stringify({
            name: "update-worker",
            type: "module",
            engines: { node: ">=20" },
          }),
        );
        const replacementRuntime = await preparing;
        expect(replacementRuntime).not.toBe(stagePath);
        expect(replacementRuntime).toContain("development-runtimes");
        const held = await newApi.acquire("worker");
        expect(await held.call("echo", [""])).toBe("After/npm");
        const restartedUi = next("ui");
        let updates = 0;
        const bothUpdated = new Promise<void>((resolve) => {
          events.on("updated", () => {
            if (++updates === 2) resolve();
          });
        });
        await writeFile(
          invalidFile,
          valid.toString().replace(" + suffix", ' + suffix + "/latest"'),
        );
        unblockPreparation!();
        await bothUpdated;
        const recycled = await restartedUi;
        expect(recycled.browser.profilePath).toBe(config.browser.profilePath);
        expect(vi.mocked(runMuonPrepare)).toHaveBeenCalledTimes(2);
        await expect(held.call("echo", [""])).rejects.toThrow();
        const latestApi = await connect();
        const latest = await latestApi.acquire("worker");
        expect(await latest.call("echo", [""])).toBe("After/npm/latest");

        // Closing Vite during another preparation must cancel the session and
        // reclaim the candidate without launching another UI or service host.
        const stopGate = new Promise<void>((resolve) => {
          unblockPreparation = resolve;
        });
        vi.mocked(runMuonPrepare).mockImplementationOnce(async (options) => {
          events.emit("preparing", options.stageDir);
          await stopGate;
          await cp(stagePath, options.stageDir!, { recursive: true });
          return {
            stagePath: options.stageDir!,
            muonPath: options.stageDir!,
            cefPath: options.stageDir!,
            cacheHit: true,
          };
        });
        const stoppingPrepare = next("preparing");
        await writeFile(
          join(project, "package.json"),
          JSON.stringify({
            name: "update-worker",
            type: "module",
            engines: { node: ">=20.1" },
          }),
        );
        await stoppingPrepare;
        const previousStarts = uiStarts;
        const closed = once(server.httpServer!, "close");
        const closing = server.close();
        await closed;
        unblockPreparation!();
        await closing;
        expect(uiStarts).toBe(previousStarts);
        await expect(latest.call("echo", [""])).rejects.toThrow();
        expect(
          await readdir(join(root, ".muon/development-runtimes/linux-amd64")),
        ).toEqual([]);
      } else if (runtime === "quickjs") {
        const missing = next("warning");
        await writeFile(
          invalidFile,
          'import { value } from "./nested/value.ts"; export const greeting = () => value;\n',
        );
        expect(await missing).toContain("service update failed");
        const restored = next("full-reload");
        await mkdir(join(project, "node_modules/quickjs-greeting/nested"));
        await writeFile(
          join(project, "node_modules/quickjs-greeting/nested/value.ts"),
          'export const value = "New dependency";\n',
        );
        await restored;
        const newApi = await connect();
        const newHandle = await newApi.acquire("worker");
        expect(await newHandle.call("echo", [""])).toBe("New dependency");
        await newHandle.release();
      }
      await server.close();
      expect(
        await readdir(join(root, ".muon/development/linux-amd64")),
      ).toEqual([]);
    } finally {
      unblockPreparation?.();
      for (const socket of sockets) socket.destroy();
      await server?.close();
      observer.closeAllConnections();
      await new Promise<void>((resolve) => observer.close(() => resolve()));
      await rm(root, { recursive: true, force: true });
      vi.mocked(runMuonPrepare).mockReset();
    }
  },
);
