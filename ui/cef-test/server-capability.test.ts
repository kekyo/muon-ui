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

const signal = () => {
  let resolve!: () => void;
  const promise = new Promise<void>((complete) => {
    resolve = complete;
  });
  return { promise, resolve };
};

const responseDriver = async (
  fetch: (request: Request) => Response | Promise<Response>,
  receive: (
    metadata: Record<string, unknown>,
    body: ArrayBuffer,
  ) => Promise<void>,
  method: string,
) => {
  let next = 0;
  let releaseEvent!: (value: ArrayBuffer) => void;
  const api = createMuonCefServerApi(async (json, body) => {
    const metadata = JSON.parse(json);
    if (metadata.operation === "create")
      return packet({ id: "server", name: "server" }, new Uint8Array());
    if (metadata.operation === "attach")
      return packet({ binding: "binding" }, new Uint8Array());
    if (metadata.operation === "next") {
      if (next++ === 0)
        return packet(
          {
            type: "request",
            requestId: "request",
            method,
            url: "http://127.0.0.1:8765/events",
            headers: [],
          },
          new Uint8Array(),
        );
      return await new Promise<ArrayBuffer>((resolve) => {
        releaseEvent = resolve;
      });
    }
    if (metadata.operation === "close" || metadata.operation === "detach")
      releaseEvent?.(packet(null, new Uint8Array()));
    else {
      expect(metadata.id).toBe("server");
      expect(metadata.binding).toBe("binding");
      expect(metadata.requestId).toBe("request");
      await receive(metadata, body);
    }
    return packet(null, new Uint8Array());
  });
  const server = await api.createServer({ name: "server", port: 8765 });
  const binding = await server.attach({ fetch });
  return {
    server,
    binding,
    disconnect: () =>
      releaseEvent(
        packet({ type: "disconnect", requestId: "request" }, new Uint8Array()),
      ),
  };
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
    let responseStatus = 0;
    let responseHeaders: [string, string][] = [];
    const responseBody: number[] = [];
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
                headers: [
                  ["content-type", "application/octet-stream"],
                  ["host", "127.0.0.1:8765"],
                  ["origin", "asset://main"],
                ],
              },
              new Uint8Array([0, 255, 3]),
            );
          return await wait;
        }
        if (request.operation === "start") {
          responseStatus = request.status;
          responseHeaders = request.headers;
        }
        if (request.operation === "write")
          responseBody.push(...new Uint8Array(args[1] as ArrayBuffer));
        if (request.operation === "finish")
          finish({
            status: responseStatus,
            headers: responseHeaders,
            body: responseBody,
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
        const cloned = request.clone();
        for (const received of [request, cloned]) {
          expect(received.headers.get("host")).toBe("127.0.0.1:8765");
          expect(received.headers.get("origin")).toBe("asset://main");
        }
        cloned.headers.set("x-clone-only", "changed");
        expect(request.headers.has("x-clone-only")).toBe(false);
        expect([...new Uint8Array(await cloned.arrayBuffer())]).toEqual([
          0, 255, 3,
        ]);
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

  it.each(["exception", "invalid response"])(
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
          return {} as Response;
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
      if (operation.operation === "finish") {
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

  it("sends headers before waiting for the producer and delivers bytes before stream completion", async () => {
    const started = signal();
    const allowRead = signal();
    const reading = signal();
    const written = signal();
    const finished = signal();
    const operations: string[] = [];
    const chunks: number[][] = [];
    let producer!: ReadableStreamDefaultController<Uint8Array>;
    const driver = await responseDriver(
      () =>
        new Response(
          new ReadableStream<Uint8Array>(
            {
              start: (controller) => {
                producer = controller;
              },
              pull: () => {
                reading.resolve();
              },
            },
            { highWaterMark: 0 },
          ),
          {
            headers: {
              "content-type": "text/event-stream",
              "mcp-session-id": "session",
            },
          },
        ),
      async (metadata, body) => {
        const operation = String(metadata.operation);
        operations.push(operation);
        if (operation === "start") {
          expect(metadata.headers).toEqual(
            expect.arrayContaining([
              ["content-type", "text/event-stream"],
              ["mcp-session-id", "session"],
            ]),
          );
          started.resolve();
          await allowRead.promise;
        }
        if (operation === "write") {
          chunks.push([...new Uint8Array(body)]);
          written.resolve();
        }
        if (operation === "finish") finished.resolve();
      },
      "GET",
    );
    try {
      await Promise.race([started.promise, reading.promise]);
      expect(operations).toEqual(["start"]);
      allowRead.resolve();
      await reading.promise;
      producer.enqueue(new Uint8Array([0, 255, 13, 10]));
      await written.promise;
      expect(chunks).toEqual([[0, 255, 13, 10]]);
      expect(operations).toEqual(["start", "write"]);
      producer.close();
      await finished.promise;
      expect(operations).toEqual(["start", "write", "finish"]);
    } finally {
      allowRead.resolve();
      await driver.server.close();
    }
  });

  it("streams responses larger than 1 MiB in ordered writes of at most 64 KiB", async () => {
    const data = Uint8Array.from(
      { length: 1024 * 1024 + 257 },
      (_, index) => index * 17 + index / 251,
    );
    const completed = signal();
    const operations: string[] = [];
    const chunks: Uint8Array[] = [];
    const driver = await responseDriver(
      () => new Response(data),
      async (metadata, body) => {
        const operation = String(metadata.operation);
        operations.push(operation);
        if (operation === "write") chunks.push(new Uint8Array(body));
        if (operation === "finish" || operation === "respond")
          completed.resolve();
      },
      "GET",
    );
    try {
      await completed.promise;
      expect(operations[0]).toBe("start");
      expect(operations.at(-1)).toBe("finish");
      expect(chunks.map((chunk) => chunk.length)).toEqual([
        ...Array(16).fill(65536),
        257,
      ]);
      const received = new Uint8Array(data.length);
      let offset = 0;
      for (const chunk of chunks) {
        received.set(chunk, offset);
        offset += chunk.length;
      }
      expect(received).toEqual(data);
    } finally {
      await driver.server.close();
    }
  });

  it("awaits every native write before sending the next slice or reading the next producer chunk", async () => {
    const writing = signal();
    const releaseWrite = signal();
    const finished = signal();
    const chunks: Uint8Array[] = [];
    let pulls = 0;
    const first = Uint8Array.from(
      { length: 2 * 65536 + 3 },
      (_, index) => index % 251,
    );
    const driver = await responseDriver(
      () =>
        new Response(
          new ReadableStream<Uint8Array>(
            {
              pull: (controller) => {
                pulls++;
                if (pulls === 1) controller.enqueue(first);
                else {
                  controller.enqueue(new Uint8Array([254]));
                  controller.close();
                }
              },
            },
            { highWaterMark: 0 },
          ),
        ),
      async (metadata, body) => {
        if (metadata.operation === "write") {
          chunks.push(new Uint8Array(body));
          if (chunks.length === 1) {
            writing.resolve();
            await releaseWrite.promise;
          }
        }
        if (metadata.operation === "finish") finished.resolve();
      },
      "GET",
    );
    try {
      await writing.promise;
      expect(pulls).toBe(1);
      expect(chunks).toHaveLength(1);
      releaseWrite.resolve();
      await finished.promise;
      expect(pulls).toBe(2);
      expect(chunks.map((chunk) => chunk.length)).toEqual([65536, 65536, 3, 1]);
      expect(chunks[0]).toEqual(first.slice(0, 65536));
      expect(chunks[1]).toEqual(first.slice(65536, 2 * 65536));
      expect(chunks[2]).toEqual(first.slice(2 * 65536));
      expect(chunks[3]).toEqual(new Uint8Array([254]));
    } finally {
      releaseWrite.resolve();
      await driver.server.close();
    }
  });

  it.each(["start", "read", "write", "finish"])(
    "aborts an incomplete response after %s fails without replacing it with HTTP 500",
    async (failure) => {
      const aborted = signal();
      const cancelled = signal();
      const operations: string[] = [];
      let pulls = 0;
      const driver = await responseDriver(
        () =>
          new Response(
            new ReadableStream<Uint8Array>(
              {
                pull: (controller) => {
                  if (failure === "read")
                    throw new Error("sensitive producer error");
                  if (pulls++ === 0)
                    controller.enqueue(new Uint8Array([1, 2, 3]));
                  else controller.close();
                },
                cancel: () => cancelled.resolve(),
              },
              { highWaterMark: 0 },
            ),
          ),
        async (metadata) => {
          const operation = String(metadata.operation);
          operations.push(operation);
          if (operation === failure) throw new Error("native send failed");
          if (operation === "abort" || operation === "respond")
            aborted.resolve();
        },
        "GET",
      );
      try {
        await aborted.promise;
        if (failure === "start" || failure === "write") {
          await cancelled.promise;
          expect(pulls).toBe(failure === "start" ? 0 : 1);
        }
        expect(operations[0]).toBe("start");
        expect(operations.at(-1)).toBe("abort");
        expect(operations).not.toContain("respond");
        if (failure !== "finish") expect(operations).not.toContain("finish");
      } finally {
        await driver.server.close();
      }
    },
  );

  it.each(["disconnect", "detach"])(
    "cancels the producer while a native write waits on %s",
    async (action) => {
      const writing = signal();
      const releaseWrite = signal();
      const cancelled = signal();
      const aborted = signal();
      const operations: string[] = [];
      let pulls = 0;
      const driver = await responseDriver(
        () =>
          new Response(
            new ReadableStream<Uint8Array>(
              {
                pull: (controller) => {
                  pulls++;
                  controller.enqueue(new Uint8Array([1, 2, 3]));
                },
                cancel: () => {
                  cancelled.resolve();
                },
              },
              { highWaterMark: 0 },
            ),
          ),
        async (metadata) => {
          const operation = String(metadata.operation);
          operations.push(operation);
          if (operation === "write") {
            writing.resolve();
            await releaseWrite.promise;
          }
          if (operation === "abort") aborted.resolve();
        },
        "GET",
      );
      try {
        await writing.promise;
        if (action === "disconnect") driver.disconnect();
        else await driver.binding.detach();
        await cancelled.promise;
        expect(pulls).toBe(1);
        releaseWrite.resolve();
        await aborted.promise;
        expect(operations).toEqual(["start", "write", "abort"]);
      } finally {
        releaseWrite.resolve();
        await driver.server.close();
      }
    },
  );

  it.each([
    { method: "HEAD", status: 200, body: true },
    { method: "GET", status: 200, body: false },
    { method: "GET", status: 204, body: false },
    { method: "GET", status: 205, body: false },
    { method: "GET", status: 304, body: false },
  ])(
    "uses an empty response without consuming a body for $method $status (body=$body)",
    async (testCase) => {
      const completed = signal();
      const cancelled = signal();
      const pulled = vi.fn();
      const operations: string[] = [];
      const driver = await responseDriver(
        () =>
          new Response(
            testCase.body
              ? new ReadableStream<Uint8Array>(
                  { pull: pulled, cancel: () => cancelled.resolve() },
                  { highWaterMark: 0 },
                )
              : null,
            { status: testCase.status },
          ),
        async (metadata, body) => {
          operations.push(String(metadata.operation));
          expect(metadata.operation).toBe("respond");
          expect(metadata.status).toBe(testCase.status);
          expect(body.byteLength).toBe(0);
          completed.resolve();
        },
        testCase.method,
      );
      try {
        await completed.promise;
        if (testCase.body) await cancelled.promise;
        expect(operations).toEqual(["respond"]);
        expect(pulled).not.toHaveBeenCalled();
      } finally {
        await driver.server.close();
      }
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
