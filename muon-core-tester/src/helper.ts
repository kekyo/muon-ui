// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

const DEFAULT_HOST = "127.0.0.1";
const DEFAULT_PORT = 9222;
const DEFAULT_TIMEOUT_MS = 5000;
const OPEN_READY_STATE = 1;
const CLOSED_READY_STATE = 3;

/**
 * JSON-compatible value used for CDP parameters and results.
 */
export type JsonValue =
  | null
  | boolean
  | number
  | string
  | JsonValue[]
  | JsonObject;

/**
 * JSON-compatible object used for CDP parameter dictionaries.
 */
export interface JsonObject {
  [key: string]: JsonValue;
}

/**
 * Options for connecting to an existing muon CDP endpoint.
 */
export interface ConnectOptions {
  /**
   * Hostname or IP address for the remote debugging HTTP endpoint.
   *
   * @remarks Defaults to `127.0.0.1`.
   */
  host?: string;

  /**
   * Port number for the remote debugging HTTP endpoint.
   *
   * @remarks Defaults to `9222`, matching muon's default cdp port.
   */
  port?: number;

  /**
   * Timeout for discovery, WebSocket connection, and CDP commands.
   *
   * @remarks Defaults to 5000 milliseconds.
   */
  timeoutMs?: number;

  /**
   * Specific CDP target id to connect to.
   *
   * @remarks When omitted, the first page target with a WebSocket cdp URL is
   * selected.
   */
  targetId?: string;
}

/**
 * Target entry returned by the CDP `/json/list` endpoint.
 */
export interface CdpTarget {
  /**
   * CDP target id.
   */
  id: string;

  /**
   * CDP target type, such as `page`.
   */
  type: string;

  /**
   * Human-readable target title.
   */
  title: string;

  /**
   * Current target URL.
   */
  url: string;

  /**
   * WebSocket URL used to control this target.
   */
  webSocketDebuggerUrl?: string;
}

/**
 * CDP event message.
 *
 * @typeParam T - Type of the event parameter payload.
 */
export interface CdpEvent<T = unknown> {
  /**
   * CDP event method name.
   */
  method: string;

  /**
   * Event parameter payload.
   */
  params: T;
}

/**
 * Listener for CDP events emitted by a connected driver.
 *
 * @typeParam T - Type of the event parameter payload.
 */
export type CdpEventListener<T = unknown> = (
  params: T,
  event: CdpEvent<T>,
) => void;

/**
 * CDP helper for an existing muon browser target.
 */
export interface CdpDriver {
  /**
   * Sends a raw CDP command.
   *
   * @typeParam T - Expected result type.
   * @param method CDP method name.
   * @param params Optional CDP parameter dictionary.
   * @returns The CDP result payload.
   */
  send<T = unknown>(method: string, params?: JsonObject): Promise<T>;

  /**
   * Registers a CDP event listener.
   *
   * @typeParam T - Expected event parameter type.
   * @param method CDP event method name.
   * @param listener Listener called whenever the event arrives.
   * @returns Function that unregisters the listener.
   */
  on<T = unknown>(method: string, listener: CdpEventListener<T>): () => void;

  /**
   * Waits for a single CDP event.
   *
   * @typeParam T - Expected event parameter type.
   * @param method CDP event method name.
   * @param timeoutMs Optional timeout in milliseconds.
   * @returns The event parameter payload.
   */
  waitForEvent<T = unknown>(method: string, timeoutMs?: number): Promise<T>;

  /**
   * Navigates the current page and waits for the load event.
   *
   * @param url Destination URL.
   * @param timeoutMs Optional timeout in milliseconds.
   */
  navigate(url: string, timeoutMs?: number): Promise<void>;

  /**
   * Evaluates JavaScript in the page runtime.
   *
   * @typeParam T - Expected returned value type.
   * @param expression JavaScript expression or program.
   * @returns The returned JSON-serializable value.
   */
  evaluate<T = unknown>(expression: string): Promise<T>;

  /**
   * Captures a page screenshot.
   *
   * @returns PNG screenshot bytes.
   */
  screenshot(): Promise<Uint8Array>;

  /**
   * Closes the underlying WebSocket connection.
   */
  close(): void;
}

interface PendingCommand {
  resolve(value: unknown): void;
  reject(error: Error): void;
  timer: ReturnType<typeof setTimeout>;
}

interface CdpErrorPayload {
  code?: number;
  message?: string;
  data?: unknown;
}

interface CdpResponseMessage {
  id: number;
  result?: unknown;
  error?: CdpErrorPayload;
}

interface CdpEventMessage {
  method: string;
  params?: unknown;
}

type ConnectableCdpTarget = CdpTarget & { webSocketDebuggerUrl: string };

interface EventWaiter<T> {
  promise: Promise<T>;
  cancel(): void;
}

interface BufferedCdpEvent<T> {
  params: T;
  sequence: number;
}

interface BufferedEventWaiter<T> {
  matches(params: T): boolean;
  minimumSequence: number;
  resolve(event: BufferedCdpEvent<T>): void;
  reject(error: Error): void;
  timer: ReturnType<typeof setTimeout>;
}

interface BufferedEventObserver<T> {
  dispose(): void;
  waitFor(
    matches: (params: T) => boolean,
    minimumSequence: number,
    timeoutMs: number | undefined,
    description: string,
  ): Promise<BufferedCdpEvent<T>>;
}

interface PageNavigateResponse {
  errorText?: string;
  frameId?: string;
  isDownload?: boolean;
  loaderId?: string;
}

interface PageFrameNavigatedParams {
  frame?: {
    id?: string;
    loaderId?: string;
    url?: string;
  };
}

interface PageFrameStoppedLoadingParams {
  frameId?: string;
}

const isRecord = (value: unknown): value is Record<string, unknown> =>
  typeof value === "object" && value !== null;

const createEndpointUrl = (options: ConnectOptions): string => {
  const host = options.host ?? DEFAULT_HOST;
  const port = options.port ?? DEFAULT_PORT;
  return `http://${host}:${port}/json/list`;
};

const getTimeoutMs = (options: ConnectOptions): number =>
  options.timeoutMs ?? DEFAULT_TIMEOUT_MS;

const fetchJson = async (url: string, timeoutMs: number): Promise<unknown> => {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeoutMs);
  try {
    const response = await fetch(url, { signal: controller.signal });
    if (!response.ok) {
      throw new Error(`CDP target discovery failed: HTTP ${response.status}`);
    }
    return await response.json();
  } catch (error) {
    if (error instanceof Error && error.name === "AbortError") {
      throw new Error(`CDP target discovery timed out after ${timeoutMs}ms`);
    }
    throw error;
  } finally {
    clearTimeout(timer);
  }
};

const parseTarget = (value: unknown): CdpTarget => {
  if (!isRecord(value)) {
    throw new Error("CDP target entry is not an object");
  }

  const { id, type, title, url, webSocketDebuggerUrl } = value;
  if (
    typeof id !== "string" ||
    typeof type !== "string" ||
    typeof title !== "string" ||
    typeof url !== "string"
  ) {
    throw new Error("CDP target entry is missing required string fields");
  }

  const target: CdpTarget = { id, type, title, url };
  if (typeof webSocketDebuggerUrl === "string") {
    target.webSocketDebuggerUrl = webSocketDebuggerUrl;
  }
  return target;
};

const selectTarget = (
  targets: CdpTarget[],
  targetId: string | undefined,
): ConnectableCdpTarget => {
  const target =
    targetId === undefined
      ? targets.find(
          (candidate) =>
            candidate.type === "page" &&
            candidate.webSocketDebuggerUrl !== undefined,
        )
      : targets.find((candidate) => candidate.id === targetId);

  if (target === undefined) {
    throw new Error(
      targetId === undefined
        ? "No page CDP target with a WebSocket cdp URL was found"
        : `CDP target '${targetId}' was not found`,
    );
  }
  if (target.webSocketDebuggerUrl === undefined) {
    throw new Error(`CDP target '${target.id}' is missing a WebSocket URL`);
  }
  return target as ConnectableCdpTarget;
};

const formatCdpError = (error: CdpErrorPayload): string => {
  const code = error.code === undefined ? "" : ` (${error.code})`;
  return `${error.message ?? "CDP command failed"}${code}`;
};

const waitForSocketOpen = async (
  socket: WebSocket,
  timeoutMs: number,
): Promise<void> => {
  if (socket.readyState === OPEN_READY_STATE) {
    return;
  }

  await new Promise<void>((resolve, reject) => {
    const timer = setTimeout(() => {
      cleanup();
      reject(
        new Error(`CDP WebSocket connection timed out after ${timeoutMs}ms`),
      );
    }, timeoutMs);

    const cleanup = (): void => {
      clearTimeout(timer);
      socket.removeEventListener("open", handleOpen);
      socket.removeEventListener("error", handleError);
      socket.removeEventListener("close", handleClose);
    };
    const handleOpen = (): void => {
      cleanup();
      resolve();
    };
    const handleError = (): void => {
      cleanup();
      reject(new Error("CDP WebSocket connection failed"));
    };
    const handleClose = (): void => {
      cleanup();
      reject(new Error("CDP WebSocket closed before connection opened"));
    };

    socket.addEventListener("open", handleOpen);
    socket.addEventListener("error", handleError);
    socket.addEventListener("close", handleClose);
  });
};

export const createDriver = (
  socket: WebSocket,
  commandTimeoutMs: number,
): CdpDriver => {
  let nextMessageId = 1;
  let nextEventSequence = 1;
  let closed = false;
  const pendingCommands = new Map<number, PendingCommand>();
  const eventListeners = new Map<string, Set<CdpEventListener<unknown>>>();

  const rejectPendingCommands = (error: Error): void => {
    for (const pendingCommand of pendingCommands.values()) {
      clearTimeout(pendingCommand.timer);
      pendingCommand.reject(error);
    }
    pendingCommands.clear();
  };

  const dispatchEvent = (message: CdpEventMessage): void => {
    const listeners = eventListeners.get(message.method);
    if (listeners === undefined) {
      return;
    }

    const event: CdpEvent<unknown> = {
      method: message.method,
      params: message.params,
    };
    for (const listener of Array.from(listeners)) {
      listener(event.params, event);
    }
  };

  const handleResponse = (message: CdpResponseMessage): void => {
    const pendingCommand = pendingCommands.get(message.id);
    if (pendingCommand === undefined) {
      return;
    }

    pendingCommands.delete(message.id);
    clearTimeout(pendingCommand.timer);
    if (message.error !== undefined) {
      pendingCommand.reject(new Error(formatCdpError(message.error)));
      return;
    }
    pendingCommand.resolve(message.result);
  };

  const handleMessage = (event: MessageEvent): void => {
    const rawData =
      typeof event.data === "string" ? event.data : String(event.data);
    const message: unknown = JSON.parse(rawData);
    if (!isRecord(message)) {
      return;
    }

    if (typeof message.id === "number") {
      handleResponse(message as unknown as CdpResponseMessage);
      return;
    }
    if (typeof message.method === "string") {
      dispatchEvent(message as unknown as CdpEventMessage);
    }
  };

  socket.addEventListener("message", handleMessage);
  socket.addEventListener("close", () => {
    closed = true;
    rejectPendingCommands(new Error("CDP WebSocket closed"));
  });
  socket.addEventListener("error", () => {
    rejectPendingCommands(new Error("CDP WebSocket failed"));
  });

  const send = async <T = unknown>(
    method: string,
    params?: JsonObject,
  ): Promise<T> => {
    if (closed || socket.readyState === CLOSED_READY_STATE) {
      throw new Error("CDP WebSocket is closed");
    }

    const id = nextMessageId;
    nextMessageId += 1;
    const message =
      params === undefined ? { id, method } : { id, method, params };

    const result = new Promise<T>((resolve, reject) => {
      const timer = setTimeout(() => {
        pendingCommands.delete(id);
        reject(
          new Error(
            `CDP command '${method}' timed out after ${commandTimeoutMs}ms`,
          ),
        );
      }, commandTimeoutMs);
      pendingCommands.set(id, {
        resolve: (value) => resolve(value as T),
        reject,
        timer,
      });
    });

    socket.send(JSON.stringify(message));
    return await result;
  };

  const on = <T = unknown>(
    method: string,
    listener: CdpEventListener<T>,
  ): (() => void) => {
    const listeners = eventListeners.get(method) ?? new Set();
    listeners.add(listener as CdpEventListener<unknown>);
    eventListeners.set(method, listeners);

    return () => {
      listeners.delete(listener as CdpEventListener<unknown>);
      if (listeners.size === 0) {
        eventListeners.delete(method);
      }
    };
  };

  const createBufferedEventObserver = <T = unknown>(
    method: string,
  ): BufferedEventObserver<T> => {
    const events: BufferedCdpEvent<T>[] = [];
    const waiters = new Set<BufferedEventWaiter<T>>();
    const resolveMatchingWaiters = (event: BufferedCdpEvent<T>): void => {
      for (const waiter of Array.from(waiters)) {
        if (
          event.sequence >= waiter.minimumSequence &&
          waiter.matches(event.params)
        ) {
          waiters.delete(waiter);
          clearTimeout(waiter.timer);
          waiter.resolve(event);
        }
      }
    };
    const unsubscribe = on<T>(method, (params) => {
      const event = {
        params,
        sequence: nextEventSequence,
      };
      nextEventSequence += 1;
      events.push(event);
      resolveMatchingWaiters(event);
    });

    const waitFor = async (
      matches: (params: T) => boolean,
      minimumSequence: number,
      timeoutMs: number | undefined,
      description: string,
    ): Promise<BufferedCdpEvent<T>> => {
      for (const event of events) {
        if (event.sequence >= minimumSequence && matches(event.params)) {
          return event;
        }
      }

      const effectiveTimeoutMs = timeoutMs ?? commandTimeoutMs;
      return await new Promise<BufferedCdpEvent<T>>((resolve, reject) => {
        const timer = setTimeout(() => {
          waiters.delete(waiter);
          reject(
            new Error(
              `CDP event '${method}' timed out after ${effectiveTimeoutMs}ms while waiting for ${description}`,
            ),
          );
        }, effectiveTimeoutMs);
        const waiter = {
          matches,
          minimumSequence,
          resolve,
          reject,
          timer,
        };
        waiters.add(waiter);
      });
    };

    const dispose = (): void => {
      unsubscribe();
      for (const waiter of Array.from(waiters)) {
        waiters.delete(waiter);
        clearTimeout(waiter.timer);
        waiter.reject(new Error(`CDP event observer '${method}' disposed`));
      }
    };

    return { dispose, waitFor };
  };

  const createEventWaiter = <T = unknown>(
    method: string,
    timeoutMs: number | undefined,
  ): EventWaiter<T> => {
    const effectiveTimeoutMs = timeoutMs ?? commandTimeoutMs;
    let timer: ReturnType<typeof setTimeout> | undefined = undefined;
    let unsubscribe: (() => void) | undefined = undefined;
    let settled = false;

    const cancel = (): void => {
      if (settled) {
        return;
      }
      settled = true;
      if (timer !== undefined) {
        clearTimeout(timer);
      }
      unsubscribe?.();
    };

    const promise = new Promise<T>((resolve, reject) => {
      timer = setTimeout(() => {
        cancel();
        reject(
          new Error(
            `CDP event '${method}' timed out after ${effectiveTimeoutMs}ms`,
          ),
        );
      }, effectiveTimeoutMs);

      unsubscribe = on<T>(method, (params) => {
        cancel();
        resolve(params);
      });
    });

    return { promise, cancel };
  };

  const waitForEvent = async <T = unknown>(
    method: string,
    timeoutMs?: number,
  ): Promise<T> => await createEventWaiter<T>(method, timeoutMs).promise;

  const navigate = async (url: string, timeoutMs?: number): Promise<void> => {
    await send("Page.enable", undefined);
    const frameNavigated =
      createBufferedEventObserver<PageFrameNavigatedParams>(
        "Page.frameNavigated",
      );
    const frameStoppedLoading =
      createBufferedEventObserver<PageFrameStoppedLoadingParams>(
        "Page.frameStoppedLoading",
      );
    const minimumSequence = nextEventSequence;
    try {
      const response = await send<PageNavigateResponse>("Page.navigate", {
        url,
      });
      if (response.errorText !== undefined) {
        throw new Error(`Page navigation failed: ${response.errorText}`);
      }
      if (response.isDownload === true) {
        return;
      }
      const frameId = response.frameId;
      if (frameId === undefined) {
        throw new Error("Page navigation response did not include frameId");
      }
      const loaderId = response.loaderId;
      const navigatedEvent = await frameNavigated.waitFor(
        (params) =>
          params.frame?.id === frameId &&
          (loaderId === undefined
            ? params.frame.url === url
            : params.frame.loaderId === loaderId),
        minimumSequence,
        timeoutMs,
        `frame '${frameId}' to navigate to ${url}`,
      );
      await frameStoppedLoading.waitFor(
        (params) => params.frameId === frameId,
        navigatedEvent.sequence,
        timeoutMs,
        `frame '${frameId}' to stop loading`,
      );
    } finally {
      frameNavigated.dispose();
      frameStoppedLoading.dispose();
    }
  };

  const evaluate = async <T = unknown>(expression: string): Promise<T> => {
    const response = await send<{
      result?: { value?: unknown };
      exceptionDetails?: unknown;
    }>("Runtime.evaluate", {
      expression,
      returnByValue: true,
      awaitPromise: true,
    });
    if (response.exceptionDetails !== undefined) {
      throw new Error("Runtime evaluation failed");
    }
    return response.result?.value as T;
  };

  const screenshot = async (): Promise<Uint8Array> => {
    const response = await send<{ data: string }>("Page.captureScreenshot", {});
    return Uint8Array.from(Buffer.from(response.data, "base64"));
  };

  const close = (): void => {
    closed = true;
    socket.removeEventListener("message", handleMessage);
    rejectPendingCommands(new Error("CDP WebSocket closed"));
    socket.close();
  };

  return {
    send,
    on,
    waitForEvent,
    navigate,
    evaluate,
    screenshot,
    close,
  };
};

/**
 * Lists CDP targets exposed by a running muon cdp endpoint.
 *
 * @param options Connection options for the remote debugging endpoint.
 * @returns Available CDP targets.
 */
export const listCdpTargets = async (
  options: ConnectOptions = {},
): Promise<CdpTarget[]> => {
  const payload = await fetchJson(
    createEndpointUrl(options),
    getTimeoutMs(options),
  );
  if (!Array.isArray(payload)) {
    throw new Error("CDP target discovery response is not an array");
  }
  return payload.map(parseTarget);
};

/**
 * Connects to a page target exposed by a running muon cdp endpoint.
 *
 * @param options Connection options for the remote debugging endpoint.
 * @returns A driver bound to the selected page target.
 */
export const connectToMuonCdp = async (
  options: ConnectOptions = {},
): Promise<CdpDriver> => {
  const targets = await listCdpTargets(options);
  const target = selectTarget(targets, options.targetId);
  const WebSocketConstructor = globalThis.WebSocket;
  if (WebSocketConstructor === undefined) {
    throw new Error("WebSocket is not available in this Node.js runtime");
  }

  const timeoutMs = getTimeoutMs(options);
  const socket = new WebSocketConstructor(target.webSocketDebuggerUrl);
  await waitForSocketOpen(socket, timeoutMs);
  return createDriver(socket, timeoutMs);
};
