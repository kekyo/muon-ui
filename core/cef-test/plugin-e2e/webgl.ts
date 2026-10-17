// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";
import { expect, it } from "vitest";

import { startDebugMuon, stopMuon } from "./shared.js";
import { requireWindowsRemoteContext } from "./windows-context.js";

it("renders WebGL2 with the default Windows GPU subprocess", async () => {
  const running = await startDebugMuon([]);
  const context = requireWindowsRemoteContext();
  const client = new Client({ name: "muon-webgl", version: "1.0.0" });
  const transport = new StdioClientTransport({
    command: "npx",
    args: [
      "--no-install",
      "@playwright/mcp",
      "--cdp-endpoint",
      `http://${context.cdpHost}:${context.cdpPort}`,
    ],
  });
  try {
    await client.connect(transport);
    // Reading pixels checks the rendered result as well as context creation.
    // No GPU process or rendering switches are added to this launch.
    const result = await client.callTool({
      name: "browser_evaluate",
      arguments: {
        function: `() => {
          const canvas = document.createElement('canvas');
          canvas.width = canvas.height = 1;
          const gl = canvas.getContext('webgl2');
          if (!gl) return null;
          gl.clearColor(0, 1, 0, 1);
          gl.clear(gl.COLOR_BUFFER_BIT);
          const pixel = new Uint8Array(4);
          gl.readPixels(0, 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, pixel);
          return Array.from(pixel);
        }`,
      },
    });
    expect(result.isError).not.toBe(true);
    const text = (result.content as { type: string; text: string }[])
      .filter((item) => item.type === "text")
      .map((item) => item.text)
      .join("\n");
    const match = /### Result\n([\s\S]*?)(?:\n###|$)/.exec(text);
    expect(match, text).not.toBeNull();
    expect(JSON.parse(match![1]!)).toEqual([0, 255, 0, 255]);
  } finally {
    try {
      await client.close();
      await transport.close();
    } finally {
      await stopMuon(running, undefined);
    }
  }
  expect(running.stderr).not.toContain("GPU process exited unexpectedly");
});
