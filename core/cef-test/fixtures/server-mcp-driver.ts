// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import {
  Client,
  StreamableHTTPClientTransport,
} from "@modelcontextprotocol/client";
import { createMcpHandler, McpServer } from "@modelcontextprotocol/server";
import * as z from "zod/v4";
import type { Server, ServerOptions } from "../../cef/renderer/server.ts";

/** Shared application state exposed to both MCP clients and the driver page. */
export interface McpDriverState {
  /** Current text shown by the application. */
  readonly value: string;
  /** Number of successful writes. */
  readonly revision: number;
}

/** Starts an application MCP endpoint using the supplied public Muon factory.
 * @param createServer - Factory from window.muon.server or muon:server.
 * @param port - Explicit loopback port reserved by the test launcher.
 * @param bearerToken - Optional token enforced by Muon's native HTTP server.
 * @param render - Updates the visible page whenever application state changes.
 * @returns Endpoint, browser client operations, safe observations and shutdown.
 */
export const startMcpDriver = async (
  createServer: (options: ServerOptions) => Promise<Server>,
  port: number,
  bearerToken: string | undefined,
  render: (state: McpDriverState) => void,
) => {
  // createMcpHandler creates one MCP instance per request. Application state
  // belongs outside that factory, just as the visible page outlives a request.
  let state: McpDriverState = { value: "initial", revision: 0 };
  render(state);
  const observations: Array<{
    origin: string | null;
    host: string | null;
    clonedOrigin: string | null;
    clonedHost: string | null;
    protocol: string | null;
    contentType: string | null;
  }> = [];
  const eras: string[] = [];
  const handler = createMcpHandler(
    ({ era }) => {
      eras.push(era);
      const mcp = new McpServer({
        name: "muon-state-driver",
        version: "1.0.0",
      });
      mcp.registerTool(
        "read_state",
        {
          description:
            "Read the text and revision currently shown by the Muon application.",
          inputSchema: z.object({}),
          annotations: { readOnlyHint: true },
        },
        async () => ({
          content: [{ type: "text", text: JSON.stringify(state) }],
        }),
      );
      mcp.registerTool(
        "write_state",
        {
          description:
            "Set the text shown by the Muon application and return the updated state.",
          inputSchema: z.object({ value: z.string().max(4096) }),
          annotations: { destructiveHint: false },
        },
        async ({ value }) => {
          state = { value, revision: state.revision + 1 };
          render(state);
          return { content: [{ type: "text", text: JSON.stringify(state) }] };
        },
      );
      return mcp;
    },
    { responseMode: "sse" },
  );
  const server = await createServer({
    name: "mcp-state-driver",
    port,
    ...(bearerToken === undefined ? {} : { bearerToken }),
    allowedOrigins: ["asset://main"],
  });
  try {
    await server.attach({
      fetch: async (request) => {
        if (new URL(request.url).pathname !== "/mcp")
          return new Response("Not Found", { status: 404 });
        const cloned = request.clone();
        const response = await handler.fetch(request);
        // Never retain Authorization headers or tokens in test diagnostics.
        observations.push({
          origin: request.headers.get("Origin"),
          host: request.headers.get("Host"),
          clonedOrigin: cloned.headers.get("Origin"),
          clonedHost: cloned.headers.get("Host"),
          protocol: request.headers.get("MCP-Protocol-Version"),
          contentType:
            response.headers.get("Content-Type")?.split(";")[0] ?? null,
        });
        return response;
      },
    });
    const { url } = await server.listen();
    const endpoint = `${url}/mcp`;
    return {
      endpoint,
      observations,
      eras,
      callFromBrowser: async (
        name: "read_state" | "write_state",
        args: Record<string, unknown>,
      ) => {
        const client = new Client(
          { name: "muon-browser-client", version: "1.0.0" },
          {
            versionNegotiation: { mode: { pin: "2026-07-28" } },
          },
        );
        const transport = new StreamableHTTPClientTransport(new URL(endpoint), {
          requestInit: {
            headers:
              bearerToken === undefined
                ? {}
                : { Authorization: `Bearer ${bearerToken}` },
          },
        });
        try {
          await client.connect(transport);
          const result = await client.callTool({ name, arguments: args });
          if (result.isError) throw new Error("The browser MCP tool failed");
          const text = result.content.find((entry) => entry.type === "text");
          if (!text || text.type !== "text")
            throw new Error("The MCP tool returned no text");
          return {
            state: JSON.parse(text.text) as McpDriverState,
            protocol: transport.protocolVersion,
            discovered: client.getDiscoverResult() !== undefined,
          };
        } finally {
          await transport.terminateSession();
          await client.close();
        }
      },
      close: async () => {
        await server.close();
        await handler.close();
      },
    };
  } catch (error) {
    await server.close();
    await handler.close();
    throw error;
  }
};
