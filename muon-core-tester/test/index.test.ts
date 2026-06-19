// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import { resolve } from "node:path";
import { pathToFileURL } from "node:url";

import { describe, expect, it } from "vitest";

import { createXvfbCommandEnvironment } from "../../scripts/xvfb-environment.mjs";
import { createDriver, type CdpDriver } from "../src/helper.js";

interface MockSocket {
  readyState: number;
  sentMessages: string[];
  addEventListener(type: string, listener: EventListener): void;
  removeEventListener(type: string, listener: EventListener): void;
  send(message: string): void;
  close(): void;
  emit(type: string, event: unknown | undefined): void;
  message(message: unknown): void;
}

const createMockSocket = (): MockSocket => {
  const listeners = new Map<string, Set<EventListener>>();
  const socket: MockSocket = {
    readyState: 1,
    sentMessages: [],
    addEventListener: (type, listener) => {
      const typeListeners = listeners.get(type) ?? new Set<EventListener>();
      typeListeners.add(listener);
      listeners.set(type, typeListeners);
    },
    removeEventListener: (type, listener) => {
      listeners.get(type)?.delete(listener);
    },
    send: (message) => {
      socket.sentMessages.push(message);
    },
    close: () => {
      socket.readyState = 3;
      socket.emit("close", undefined);
    },
    emit: (type, event) => {
      for (const listener of Array.from(listeners.get(type) ?? [])) {
        listener((event ?? {}) as Event);
      }
    },
    message: (message) => {
      socket.emit("message", {
        data: typeof message === "string" ? message : JSON.stringify(message),
      });
    },
  };
  return socket;
};

const createMockDriver = (): { driver: CdpDriver; socket: MockSocket } => {
  const socket = createMockSocket();
  const driver = createDriver(socket as unknown as WebSocket, 5000);
  return { driver, socket };
};

const readSentMessage = (socket: MockSocket, index: number): unknown =>
  JSON.parse(socket.sentMessages[index] ?? "");

const readPromiseState = async (
  promise: Promise<unknown>,
): Promise<"pending" | "settled"> => {
  const pending = Symbol("pending");
  const result = await Promise.race([
    (async (): Promise<"settled"> => {
      try {
        await promise;
      } catch {
        // Rejections are still a settled state for this helper.
      }
      return "settled";
    })(),
    Promise.resolve(pending),
  ]);
  return result === pending ? "pending" : "settled";
};

describe("CDP helper calls", () => {
  it("loads the built ESM helper", async () => {
    const helperModule = (await import(
      pathToFileURL(resolve("dist/helper.mjs")).href
    )) as { createDriver: typeof createDriver };
    const socket = createMockSocket();
    const driver = helperModule.createDriver(
      socket as unknown as WebSocket,
      5000,
    );

    const resultPromise = driver.send("Runtime.evaluate", {
      expression: "1 + 1",
    });
    expect(readSentMessage(socket, 0)).toEqual({
      id: 1,
      method: "Runtime.evaluate",
      params: { expression: "1 + 1" },
    });

    socket.message({ id: 1, result: { value: 2 } });

    await expect(resultPromise).resolves.toEqual({ value: 2 });
    driver.close();
  });

  it("sends a raw CDP command", async () => {
    const { driver, socket } = createMockDriver();

    const resultPromise = driver.send("Runtime.evaluate", {
      expression: "1 + 1",
    });
    expect(readSentMessage(socket, 0)).toEqual({
      id: 1,
      method: "Runtime.evaluate",
      params: { expression: "1 + 1" },
    });

    socket.message({ id: 1, result: { value: 2 } });

    await expect(resultPromise).resolves.toEqual({ value: 2 });
    driver.close();
  });

  it("navigates with Page.enable and Page.navigate", async () => {
    const { driver, socket } = createMockDriver();

    const navigatePromise = driver.navigate("https://example.com/");
    expect(readSentMessage(socket, 0)).toEqual({
      id: 1,
      method: "Page.enable",
    });

    socket.message({ id: 1, result: {} });
    await expect.poll(() => socket.sentMessages.length).toBe(2);
    expect(readSentMessage(socket, 1)).toEqual({
      id: 2,
      method: "Page.navigate",
      params: { url: "https://example.com/" },
    });

    socket.message({ method: "Page.loadEventFired", params: { timestamp: 1 } });
    socket.message({
      id: 2,
      result: { frameId: "frame-1", loaderId: "loader-1" },
    });
    await new Promise((resolve) => setTimeout(resolve, 0));
    expect(await readPromiseState(navigatePromise)).toBe("pending");
    socket.message({
      method: "Page.frameNavigated",
      params: {
        frame: {
          id: "frame-1",
          loaderId: "loader-1",
          url: "https://example.com/",
        },
      },
    });
    socket.message({ method: "Page.loadEventFired", params: { timestamp: 1 } });
    socket.message({
      method: "Page.frameStoppedLoading",
      params: { frameId: "frame-1" },
    });

    await expect(navigatePromise).resolves.toBeUndefined();
    driver.close();
  });

  it("evaluates JavaScript with Runtime.evaluate", async () => {
    const { driver, socket } = createMockDriver();

    const evaluatePromise = driver.evaluate<string>("document.title");
    expect(readSentMessage(socket, 0)).toEqual({
      id: 1,
      method: "Runtime.evaluate",
      params: {
        expression: "document.title",
        returnByValue: true,
        awaitPromise: true,
      },
    });

    socket.message({ id: 1, result: { result: { value: "Hello" } } });

    await expect(evaluatePromise).resolves.toBe("Hello");
    driver.close();
  });

  it("captures screenshots with Page.captureScreenshot", async () => {
    const { driver, socket } = createMockDriver();

    const screenshotPromise = driver.screenshot();
    expect(readSentMessage(socket, 0)).toEqual({
      id: 1,
      method: "Page.captureScreenshot",
      params: {},
    });

    socket.message({
      id: 1,
      result: { data: Buffer.from("png").toString("base64") },
    });

    await expect(screenshotPromise).resolves.toEqual(
      Uint8Array.from(Buffer.from("png")),
    );
    driver.close();
  });
});

describe("Xvfb command environment", () => {
  it("removes Wayland display variables and prefers X11 backends", () => {
    const environment = createXvfbCommandEnvironment({
      DISPLAY: ":42",
      PATH: "/usr/bin",
      WAYLAND_DISPLAY: "wayland-1",
      WAYLAND_SOCKET: "7",
      XDG_SESSION_TYPE: "wayland",
    });

    expect(environment).toMatchObject({
      CLUTTER_BACKEND: "x11",
      DISPLAY: ":42",
      ELECTRON_OZONE_PLATFORM_HINT: "x11",
      GDK_BACKEND: "x11",
      LIBGL_ALWAYS_SOFTWARE: "1",
      MOZ_ENABLE_WAYLAND: "0",
      MUON_TEST_XVFB_WINDOW_MANAGER: "1",
      OZONE_PLATFORM: "x11",
      PATH: "/usr/bin",
      QT_QPA_PLATFORM: "xcb",
      SDL_VIDEODRIVER: "x11",
      XDG_SESSION_TYPE: "x11",
    });
    expect(environment).not.toHaveProperty("WAYLAND_DISPLAY");
    expect(environment).not.toHaveProperty("WAYLAND_SOCKET");
  });
});
