// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { spawn } from "node:child_process";
import { once } from "node:events";
import { mkdtemp, rm, writeFile } from "node:fs/promises";
import { createServer } from "node:net";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";

import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { InMemoryTransport } from "@modelcontextprotocol/sdk/inMemory.js";
import { createConnection } from "@playwright/mcp";
import { expect, it } from "vitest";

it("preserves filesystem results across repeated asynchronous completions", async () => {
  // The same public API exercise also runs against the distribution baseline.
  const runtime = resolve(
    process.env.MUON_TEST_LINUX_RUNTIME ?? "../cef/.run/test-linux-amd64-debug",
  );
  const directory = await mkdtemp(join(tmpdir(), "muon-linux-baseline-"));
  const listener = createServer();
  listener.listen(0, "127.0.0.1");
  await once(listener, "listening");
  const address = listener.address();
  if (address === null || typeof address === "string")
    throw new Error("Cannot allocate the baseline test CDP port");
  const port = address.port;
  listener.close();
  await once(listener, "close");
  const config = join(directory, "muon.json");
  await writeFile(
    config,
    JSON.stringify({
      network: { allow: ["asset://main/**"] },
      plugin: {
        mode: "simple",
        path: join(directory, "plugins"),
        pages: ["asset://main/**"],
        plugins: [{ name: "internal", allow: ["muon.fs.**"] }],
      },
      browser: { profilePath: join(directory, "profile") },
      cdp: { enable: true, port },
    }),
  );
  const child = spawn(join(runtime, "muon-core"), ["-c", config], {
    cwd: runtime,
    stdio: ["ignore", "pipe", "pipe"],
  });
  let diagnostics = "";
  const exited = once(child, "close");
  const ready = new Promise<void>((resolveReady, rejectReady) => {
    child.once("error", rejectReady);
    child.once("close", (code, signal) => {
      rejectReady(new Error(`Muon exited: ${code ?? signal}\n${diagnostics}`));
    });
    child.stdout.on("data", (data: Buffer) => {
      diagnostics += data.toString();
    });
    child.stderr.on("data", (data: Buffer) => {
      diagnostics += data.toString();
      if (diagnostics.includes("DevTools listening on")) resolveReady();
    });
  });
  const client = new Client({ name: "muon-linux-baseline-test", version: "1" });
  const mcp = await createConnection({
    browser: { cdpEndpoint: `http://127.0.0.1:${port}` },
    outputDir: join(directory, "playwright"),
  });
  const [clientTransport, serverTransport] =
    InMemoryTransport.createLinkedPair();
  const call = async (name: string, args: Record<string, unknown>) => {
    const result = await client.callTool({ name, arguments: args });
    const content = result.content as Array<{ type: string; text?: string }>;
    const text = content
      .filter((item) => item.type === "text")
      .map((item) => item.text ?? "")
      .join("\n");
    if (result.isError) throw new Error(`${text}\n${diagnostics}`);
    return text;
  };
  try {
    await ready;
    await mcp.connect(serverTransport);
    await client.connect(clientTransport);
    const tabs = await call("browser_tabs", { action: "list" });
    const index = tabs.match(/- (\d+):[^\n]*asset:\/\//)?.[1];
    expect(index, tabs.slice(-2000)).toBeDefined();
    await call("browser_tabs", { action: "select", index: Number(index) });
    const result = await call("browser_evaluate", {
      function: `async () => {
        const root = ${JSON.stringify(directory)};
        const values = ["", "short", "abcdefghijklmnop", "日本語の内容", "x".repeat(4096)];
        for (let i = 0; i < values.length; i++) {
          const path = root + "/file-" + i;
          const moved = path + ".moved";
          await window.muon.fs.writeTextFile(path, values[i], "utf8");
          await window.muon.fs.rename(path, moved);
          for (let repetition = 0; repetition < 3; repetition++) {
            const actual = await window.muon.fs.readTextFile(moved, "utf8");
            if (actual !== values[i]) throw new Error("Read result changed: " + i);
          }
          const link = path + ".link";
          await window.muon.fs.symlink(moved, link);
          if (await window.muon.fs.readlink(link) !== moved)
            throw new Error("Symbolic link result changed");
        }
        return "baseline roundtrip success";
      }`,
    });
    expect(result).toContain("baseline roundtrip success");
  } finally {
    child.kill("SIGTERM");
    await exited;
    await client.close();
    await mcp.close();
    await rm(directory, { recursive: true, force: true });
  }
});
