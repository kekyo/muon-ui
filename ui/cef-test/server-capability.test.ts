// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { afterEach, describe, expect, it, vi } from "vitest";
import { createMuonCapabilityModuleResolver } from "../common/capability.ts";
import {
  createMuonCefServerApi,
  installMuonCefServerApi,
} from "../../core/cef/renderer/server.ts";

const packet = (metadata: unknown, body: Uint8Array): ArrayBuffer => {
  const json = new TextEncoder().encode(JSON.stringify(metadata));
  const bytes = new Uint8Array(4 + json.length + body.length);
  new DataView(bytes.buffer).setUint32(0, json.length);
  bytes.set(json, 4);
  bytes.set(body, 4 + json.length);
  return bytes.buffer;
};

afterEach(() => vi.unstubAllGlobals());

describe("CEF server capability modules", () => {
  it.each(["simple", "validate"])(
    "passes the configured Bearer and exact origins through %s mode",
    async (mode) => {
      const options = {
        name: "configured",
        port: 8765,
        bearerToken: "test-token.A_+/-~==",
        allowedOrigins: [
          "asset://main",
          "http://localhost:5173",
          "https://app.example",
        ] as const,
      };
      let received: unknown;
      const dispatch = async (json: string): Promise<ArrayBuffer> => {
        received = JSON.parse(json).options;
        return packet(
          { id: "configured", name: options.name },
          new Uint8Array(),
        );
      };
      let create: ReturnType<typeof createMuonCefServerApi>["createServer"];
      if (mode === "simple") {
        const namespace: Record<string, unknown> = {};
        Object.defineProperty(namespace, "__dispatch", { value: dispatch });
        installMuonCefServerApi(namespace, (name) => name === "createServer");
        create = namespace.createServer as typeof create;
      } else {
        vi.stubGlobal(
          "__muon_plugin_call",
          async (_capability: string, path: string, args: unknown[]) => {
            expect(path).toBe("muon.server.createServer");
            return await dispatch(args[0] as string);
          },
        );
        const resolver = createMuonCapabilityModuleResolver("/app", {
          imports: [
            { sources: ["src/**"], allow: ["muon.server.createServer"] },
          ],
        });
        const id = resolver.resolveId("muon:server", "/app/src/main.ts")!.id;
        const api = await import(
          `data:text/javascript,${encodeURIComponent(resolver.load(id)!)}`
        );
        create = api.createServer;
      }
      await create(options);
      expect(received).toEqual({ ...options, host: "127.0.0.1" });
    },
  );

  it.each([
    { bearerToken: "" },
    { bearerToken: "test-token with spaces" },
    { allowedOrigins: ["https://app.example/"] },
    { allowedOrigins: ["null"] },
  ])(
    "preserves configured policy values for authoritative native validation: %j",
    async (policy) => {
      const options = { name: "invalid-policy", port: 8765, ...policy };
      let received: unknown;
      const api = createMuonCefServerApi(async (json) => {
        received = JSON.parse(json).options;
        throw new Error("Invalid server policy");
      });
      await expect(api.createServer(options)).rejects.toThrow(
        "Invalid server policy",
      );
      expect(received).toEqual({ ...options, host: "127.0.0.1" });
    },
  );

  it("separates creation, binding, listening and closure while adapting Fetch bytes", async () => {
    const operations: string[] = [];
    let next = 0;
    let releaseWait!: (value: ArrayBuffer) => void;
    const wait = new Promise<ArrayBuffer>((resolve) => {
      releaseWait = resolve;
    });
    let finish!: (value: {
      status: number;
      headers: [string, string][];
      body: number[];
    }) => void;
    const response = new Promise<{
      status: number;
      headers: [string, string][];
      body: number[];
    }>((resolve) => {
      finish = resolve;
    });
    vi.stubGlobal(
      "__muon_plugin_call",
      async (_capability: string, path: string, args: unknown[]) => {
        expect(path).toBe("muon.server.createServer");
        expect(args[2]).toBe("");
        const request = JSON.parse(args[0] as string);
        operations.push(request.operation);
        if (request.operation === "create") {
          expect(request.options).toEqual({
            name: "automation",
            port: 8765,
            host: "127.0.0.1",
          });
          return packet(
            { id: "server-1", name: "automation" },
            new Uint8Array(),
          );
        }
        expect(request.id).toBe("server-1");
        if (request.operation === "attach")
          return packet({ binding: "binding-1" }, new Uint8Array());
        if (request.operation === "listen") {
          expect(request.binding).toBe("binding-1");
          return packet(
            { url: "http://127.0.0.1:8765", host: "127.0.0.1", port: 8765 },
            new Uint8Array(),
          );
        }
        if (request.operation === "next") {
          expect(request.binding).toBe("binding-1");
          if (next++ === 0)
            return packet(
              {
                type: "request",
                requestId: "request-1",
                method: "POST",
                url: "http://127.0.0.1:8765/echo",
                headers: [["content-type", "application/octet-stream"]],
              },
              new Uint8Array([0, 255, 3]),
            );
          return await wait;
        }
        if (request.operation === "respond")
          finish({
            status: request.status,
            headers: request.headers,
            body: [...new Uint8Array(args[1] as ArrayBuffer)],
          });
        if (request.operation === "detach" || request.operation === "close")
          releaseWait(packet(null, new Uint8Array()));
        return packet(null, new Uint8Array());
      },
    );
    const resolver = createMuonCapabilityModuleResolver("/app", {
      imports: [{ sources: ["src/**"], allow: ["muon.server.createServer"] }],
    });
    const id = resolver.resolveId("muon:server", "/app/src/main.ts")!.id;
    const api = await import(
      `data:text/javascript,${encodeURIComponent(resolver.load(id)!)}`
    );
    const server = await api.createServer({ name: "automation", port: 8765 });
    expect(operations).toEqual(["create"]);
    const binding = await server.attach({
      fetch: async (request: Request) => {
        expect(request.method).toBe("POST");
        expect(request.headers.get("content-type")).toBe(
          "application/octet-stream",
        );
        return new Response(await request.arrayBuffer(), {
          status: 201,
          headers: { "x-result": "echo" },
        });
      },
    });
    await expect(server.listen()).resolves.toMatchObject({ port: 8765 });
    await expect(response).resolves.toMatchObject({
      status: 201,
      body: [0, 255, 3],
      headers: expect.arrayContaining([["x-result", "echo"]]),
    });
    await binding.detach();
    await binding.detach();
    await server.close();
    await server.close();
    expect(
      operations.filter((operation) => operation === "detach"),
    ).toHaveLength(1);
    expect(
      operations.filter((operation) => operation === "close"),
    ).toHaveLength(1);
    await expect(server.listen()).rejects.toThrow(/closed/i);
  });

  it("does not expose the CEF server to Android", () => {
    expect(() =>
      createMuonCapabilityModuleResolver("/app", {
        backend: "android",
        imports: [{ sources: ["src/**"], allow: ["muon.server.createServer"] }],
      }),
    ).toThrow(/Android/);
  });

  it("exports only createServer for a wildcard server capability", async () => {
    vi.stubGlobal("__muon_plugin_call", vi.fn());
    const resolver = createMuonCapabilityModuleResolver("/app", {
      imports: [{ sources: ["src/**"], allow: ["muon.server.*"] }],
    });
    const id = resolver.resolveId("muon:server", "/app/src/main.ts")!.id;
    const api = await import(
      `data:text/javascript,${encodeURIComponent(resolver.load(id)!)}`
    );
    expect(Object.keys(api)).toEqual(["createServer"]);
  });

  it("installs only the allowed factory over a hidden native dispatcher", async () => {
    const dispatch = vi.fn(async () =>
      packet({ id: "simple", name: "simple" }, new Uint8Array()),
    );
    const namespace: Record<string, unknown> = {};
    Object.defineProperty(namespace, "__dispatch", { value: dispatch });
    installMuonCefServerApi(namespace, () => false);
    expect(Object.keys(namespace)).toEqual([]);
    installMuonCefServerApi(namespace, (name) => name === "createServer");
    expect(Object.keys(namespace)).toEqual(["createServer"]);
    const create = namespace.createServer as ReturnType<
      typeof createMuonCefServerApi
    >["createServer"];
    await expect(create({ name: "simple", port: 8765 })).resolves.toMatchObject(
      { id: "simple" },
    );
    expect(dispatch).toHaveBeenCalledWith(
      expect.any(String),
      expect.any(ArrayBuffer),
      "",
    );
  });

  it.each([
    { name: "", port: 8765 },
    { name: "invalid-port", port: 0 },
    { name: "invalid-port", port: 65536 },
    { name: "invalid-host", port: 8765, host: "0.0.0.0" },
  ])(
    "rejects invalid server options before native dispatch: %j",
    async (options) => {
      const dispatch = vi.fn();
      const api = createMuonCefServerApi(dispatch);
      await expect(
        api.createServer(options as Parameters<typeof api.createServer>[0]),
      ).rejects.toThrow();
      expect(dispatch).not.toHaveBeenCalled();
    },
  );

  it.each(["exception", "oversized body"])(
    "returns a generic 500 for a handler %s",
    async (failure) => {
      let releaseWait!: (value: ArrayBuffer) => void;
      const wait = new Promise<ArrayBuffer>((resolve) => {
        releaseWait = resolve;
      });
      let received!: (value: { status: number; text: string }) => void;
      const response = new Promise<{ status: number; text: string }>(
        (resolve) => {
          received = resolve;
        },
      );
      let next = 0;
      const api = createMuonCefServerApi(async (json, body) => {
        const request = JSON.parse(json);
        if (request.operation === "create")
          return packet({ id: "server", name: "server" }, new Uint8Array());
        if (request.operation === "attach")
          return packet({ binding: "binding" }, new Uint8Array());
        if (request.operation === "next") {
          if (next++ === 0)
            return packet(
              {
                type: "request",
                requestId: "request",
                method: "GET",
                url: "http://127.0.0.1:8765/",
                headers: [],
              },
              new Uint8Array(),
            );
          return await wait;
        }
        if (request.operation === "respond")
          received({
            status: request.status,
            text: new TextDecoder().decode(body),
          });
        if (request.operation === "close")
          releaseWait(packet(null, new Uint8Array()));
        return packet(null, new Uint8Array());
      });
      const server = await api.createServer({ name: "server", port: 8765 });
      await server.attach({
        fetch: () => {
          if (failure === "exception")
            throw new Error("sensitive application error");
          return new Response(new Uint8Array(1024 * 1024 + 1));
        },
      });
      expect(await response).toEqual({
        status: 500,
        text: "Internal Server Error",
      });
      await server.close();
    },
  );

  it("serves another request while one handler waits and aborts only the disconnected request", async () => {
    let releaseEvent!: (value: ArrayBuffer) => void;
    let completeSlow!: (value: Response) => void;
    let finishFast!: () => void;
    let finishAbort!: () => void;
    const fast = new Promise<void>((resolve) => {
      finishFast = resolve;
    });
    const aborted = new Promise<void>((resolve) => {
      finishAbort = resolve;
    });
    const responses: string[] = [];
    let next = 0;
    const api = createMuonCefServerApi(async (json) => {
      const operation = JSON.parse(json);
      if (operation.operation === "create")
        return packet({ id: "server", name: "server" }, new Uint8Array());
      if (operation.operation === "attach")
        return packet({ binding: "binding" }, new Uint8Array());
      if (operation.operation === "next") {
        if (next < 2) {
          const requestId = next++ === 0 ? "slow" : "fast";
          return packet(
            {
              type: "request",
              requestId,
              method: "GET",
              url: `http://127.0.0.1:8765/${requestId}`,
              headers: [],
            },
            new Uint8Array(),
          );
        }
        return await new Promise<ArrayBuffer>((resolve) => {
          releaseEvent = resolve;
        });
      }
      if (operation.operation === "respond") {
        responses.push(operation.requestId);
        if (operation.requestId === "fast") finishFast();
      }
      if (operation.operation === "close")
        releaseEvent(packet(null, new Uint8Array()));
      return packet(null, new Uint8Array());
    });
    const server = await api.createServer({ name: "server", port: 8765 });
    await server.attach({
      fetch: async (request) => {
        if (new URL(request.url).pathname === "/fast")
          return new Response("fast");
        request.signal.addEventListener("abort", finishAbort, { once: true });
        return await new Promise<Response>((resolve) => {
          completeSlow = resolve;
        });
      },
    });
    await fast;
    releaseEvent(
      packet({ type: "disconnect", requestId: "slow" }, new Uint8Array()),
    );
    await aborted;
    completeSlow(new Response("late"));
    await server.close();
    expect(responses).toEqual(["fast"]);
  });

  it.each(["disconnect", "detach", "close"])(
    "cancels a pending response body on %s",
    async (action) => {
      let releaseEvent!: (value: ArrayBuffer) => void;
      let startRead!: () => void;
      let finishCancel!: () => void;
      const reading = new Promise<void>((resolve) => {
        startRead = resolve;
      });
      const cancelled = new Promise<void>((resolve) => {
        finishCancel = resolve;
      });
      let signal: AbortSignal | undefined;
      const responses: string[] = [];
      let next = 0;
      const api = createMuonCefServerApi(async (json) => {
        const operation = JSON.parse(json);
        if (operation.operation === "create")
          return packet({ id: "server", name: "server" }, new Uint8Array());
        if (operation.operation === "attach")
          return packet({ binding: "binding" }, new Uint8Array());
        if (operation.operation === "next") {
          if (next++ === 0)
            return packet(
              {
                type: "request",
                requestId: "request",
                method: "GET",
                url: "http://127.0.0.1:8765/",
                headers: [],
              },
              new Uint8Array(),
            );
          return await new Promise<ArrayBuffer>((resolve) => {
            releaseEvent = resolve;
          });
        }
        if (operation.operation === "respond")
          responses.push(operation.requestId);
        if (operation.operation === "close" || operation.operation === "detach")
          releaseEvent(packet(null, new Uint8Array()));
        return packet(null, new Uint8Array());
      });
      const server = await api.createServer({ name: "server", port: 8765 });
      const binding = await server.attach({
        fetch: (request) => {
          signal = request.signal;
          return new Response(
            new ReadableStream<Uint8Array>({
              pull: startRead,
              cancel: finishCancel,
            }),
          );
        },
      });
      await reading;
      if (action === "disconnect")
        releaseEvent(
          packet(
            { type: "disconnect", requestId: "request" },
            new Uint8Array(),
          ),
        );
      else if (action === "detach") await binding.detach();
      else await server.close();
      await cancelled;
      expect(signal?.aborted).toBe(true);
      await server.close();
      expect(responses).toEqual([]);
    },
  );

  it("can retry listening after an occupied port and replace detached handlers", async () => {
    let releaseEvent: ((value: ArrayBuffer) => void) | undefined;
    let attachments = 0;
    let attempts = 0;
    const api = createMuonCefServerApi(async (json) => {
      const operation = JSON.parse(json);
      if (operation.operation === "create")
        return packet({ id: "server", name: "server" }, new Uint8Array());
      if (operation.operation === "attach")
        return packet({ binding: String(++attachments) }, new Uint8Array());
      if (operation.operation === "next")
        return await new Promise<ArrayBuffer>((resolve) => {
          releaseEvent = resolve;
        });
      if (operation.operation === "listen") {
        expect(operation.binding).toBe(String(attachments));
        if (attempts++ === 0) throw new Error("Port already in use");
        return packet(
          { url: "http://127.0.0.1:8765", host: "127.0.0.1", port: 8765 },
          new Uint8Array(),
        );
      }
      if (operation.operation === "detach" || operation.operation === "close")
        releaseEvent?.(packet(null, new Uint8Array()));
      return packet(null, new Uint8Array());
    });
    const server = await api.createServer({ name: "server", port: 8765 });
    await expect(server.listen()).rejects.toThrow(/attached/);
    const binding = await server.attach({ fetch: () => new Response("first") });
    await expect(
      server.attach({ fetch: () => new Response("duplicate") }),
    ).rejects.toThrow(/attached/);
    await expect(server.listen()).rejects.toThrow(/Port/);
    await expect(server.listen()).resolves.toMatchObject({ port: 8765 });
    await binding.detach();
    await server.attach({ fetch: () => new Response("replacement") });
    expect(attachments).toBe(2);
    await server.close();
  });
});
