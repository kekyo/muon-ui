// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile } from "node:child_process";
import { copyFile, mkdir, mkdtemp, rm, writeFile } from "node:fs/promises";
import { join, resolve } from "node:path";
import { promisify } from "node:util";
import { build } from "vite";
import { beforeAll, expect, it } from "vitest";

beforeAll(async () => {
  await mkdir("../service-host/.build", { recursive: true });
  await promisify(execFile)("flock", [
    "../service-host/.build/driver-build.lock",
    "make",
    "-C",
    "../service-host",
    ".build/muon-service-host",
    ".build/http-service.so",
    ".build/server-service.so",
  ]);
}, 600000);

it("owns a Node service independently of UI and drains asynchronous stop", async () => {
  await mkdir("../../.build", { recursive: true });
  const root = await mkdtemp(resolve("../../.build/host node driver "));
  try {
    await writeFile(
      join(root, "package.json"),
      JSON.stringify({ type: "module" }),
    );
    await build({
      configFile: false,
      logLevel: "silent",
      build: {
        outDir: root,
        emptyOutDir: false,
        minify: false,
        lib: {
          entry: {
            "server-node": resolve("../../sdk/examples/services/server.ts"),
            "http-node": resolve("../../sdk/examples/services/http.ts"),
            "node-driver": resolve("../service-host/node-driver.ts"),
            service: resolve("../../node/test/fixtures/node-service.ts"),
            lifecycle: resolve(
              "../../node/test/fixtures/node-lifecycle-service.ts",
            ),
            "disconnect-bridge": resolve(
              "../../node/test/fixtures/node-disconnect-bridge.ts",
            ),
          },
          formats: ["es"],
          fileName: (_format, entry) => entry + ".mjs",
        },
        rolldownOptions: { external: [/^node:/] },
      },
    });
    await promisify(execFile)(process.execPath, [
      "../../sdk/scripts/build-service-runtime-fixture.mjs",
      join(root, "http-quick.js"),
      "--entry",
      "../../sdk/examples/services/http.ts",
    ]);
    await promisify(execFile)(process.execPath, [
      "../../sdk/scripts/build-service-runtime-fixture.mjs",
      join(root, "server-quick.js"),
      "--entry",
      "../../sdk/examples/services/server.ts",
    ]);
    for (const name of ["localhost-cert.pem", "localhost-key.pem"])
      await copyFile(
        join("../service-host/test-assets", name),
        join(root, name),
      );
    await copyFile(
      resolve("../../node/dist/node-service-bridge.mjs"),
      join(root, "node-service-bridge.mjs"),
    );
    const result = await promisify(execFile)(process.execPath, [
      join(root, "node-driver.mjs"),
      resolve("../service-host/.build"),
      root,
    ]);
    expect(result.stdout).toContain('"nodeService":true,"passed":true');
  } finally {
    await rm(root, { recursive: true, force: true });
  }
}, 90000);
