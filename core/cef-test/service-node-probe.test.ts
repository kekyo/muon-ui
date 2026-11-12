// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile, spawn } from "node:child_process";
import { createHash } from "node:crypto";
import { once } from "node:events";
import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { createConnection } from "node:net";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { createInterface } from "node:readline";
import { promisify } from "node:util";
import { beforeAll, expect, it } from "vitest";

beforeAll(async () => {
  await promisify(execFile)("flock", [
    "../service-host/.build/driver-build.lock",
    "make",
    "-C",
    "../service-host",
    "all",
  ]);
}, 600000);

// A minimal adapter experiment: load the real page-side Node plugin under the
// independent service host. These diagnostics document why its existing page
// facade cannot be reused as a service adapter without changing its contract.
it.each([
  {
    methods: ["createNode"],
    diagnostic: "Node renderer owner token is unavailable",
  },
  {
    methods: [
      "createNode",
      "importModule",
      "call",
      "releaseModule",
      "releaseNode",
    ],
    diagnostic: "Plugin public wrapper is unavailable: call",
  },
])(
  "observes the Node sidecar adapter boundary: $diagnostic",
  async ({ methods, diagnostic }) => {
    const directory = await mkdtemp(join(tmpdir(), "muon-node-adapter-probe-"));
    const endpoint = join(directory, "control");
    const registry = join(directory, "services.json");
    const script = join(directory, "probe.mjs");
    const library = resolve(
      "../cef/.run/test-linux-amd64-debug/plugins/node.so",
    );
    const allow = methods.map((name) => "muon.node." + name);
    await writeFile(
      script,
      `
    import {createNode} from 'muon:node';
    globalThis.__muonServiceInterface={schemaVersion:1,methods:{},events:{}};
    globalThis.__muonServiceRuntime={async start(){await createNode()},async invoke(){},cancel(){},async stop(){}};
  `,
    );
    await writeFile(
      registry,
      JSON.stringify({
        services: {
          probe: {
            runtime: "quickjs",
            script,
            autostart: true,
            plugins: allow,
          },
        },
        plugins: [
          {
            name: "node",
            library,
            allow,
            expectedFunctions: allow,
            signature: createHash("sha256")
              .update(await readFile(library))
              .digest("hex"),
            salt: "",
            config: {
              project: directory,
              bridge: resolve("../../node/dist/node-bridge.mjs"),
              executable: process.execPath,
            },
          },
        ],
      }),
    );
    const host = spawn(
      resolve("../service-host/.build/muon-service-host"),
      [
        "--config",
        registry,
        "--endpoint",
        endpoint,
        "--data-root",
        join(directory, "data"),
      ],
      { stdio: ["ignore", "pipe", "pipe"] },
    );
    const exited = once(host, "exit");
    let stderr = "";
    host.stderr.on("data", (chunk) => {
      stderr += String(chunk);
    });
    const output = createInterface({ input: host.stdout });
    let socket: ReturnType<typeof createConnection> | undefined;
    try {
      const failed = new Promise<string>((resolve) =>
        output.on("line", (line) => {
          const value = JSON.parse(line);
          if (value.state === "failed") resolve(value.error);
        }),
      );
      expect(
        await Promise.race([
          failed,
          (async () => {
            await exited;
            throw new Error(stderr);
          })(),
        ]),
      ).toContain(diagnostic);
      socket = createConnection(endpoint);
      await once(socket, "connect");
      socket.resume();
      socket.write(
        JSON.stringify({ requestId: "shutdown", op: "shutdown" }) + "\n",
      );
      expect((await exited)[0], stderr).toBe(0);
    } finally {
      socket?.destroy();
      output.close();
      if (host.exitCode === null && host.signalCode === null) {
        host.kill();
        await exited;
      }
      await rm(directory, { recursive: true, force: true });
    }
  },
);
