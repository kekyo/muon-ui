// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

/** Options for a page-owned loopback HTTP server. */
export interface ServerOptions {
  /** Nonempty server name, unique within the owning page. */
  readonly name: string;
  /** Explicit listening port between 1025 and 65535. */
  readonly port: number;
  /** Numeric loopback address; defaults to 127.0.0.1. */
  readonly host?: '127.0.0.1';
  /**
   * Secret required as a Bearer token on application requests when configured.
   * @remarks Uses RFC 6750 b64token syntax, with 1 to 4096 ASCII characters.
   * CORS preflight requests are checked separately without the token.
   * @defaultValue No Bearer authentication.
   */
  readonly bearerToken?: string;
  /**
   * Exact http, https or asset origins permitted to access this server.
   * @remarks Wildcards, opaque "null" origins, paths, queries and fragments
   * are rejected. Entries are passed unchanged for native validation.
   * @defaultValue An empty list; requests without Origin remain permitted.
   */
  readonly allowedOrigins?: readonly string[];
}

/** Handlers attached to the current page. */
export interface ServerHandlers {
  /** Handles one HTTP request; response bodies are streamed with backpressure.
   * The request signal aborts on detected transport failure, detach or closure.
   */
  readonly fetch: (request: Request) => Response | Promise<Response>;
  /** Optional RFC 6455 WebSocket upgrade and connection handlers. */
  readonly websocket?: ServerWebSocketHandlers;
}

/** A native WebSocket accepted by the current binding. */
export interface ServerWebSocket {
  /** Opaque connection identifier. */
  readonly id: string;
  /** Accepted upgrade request, with credentials removed; its signal aborts on close. */
  readonly request: Request;
  /** Negotiated subprotocol, or an empty string. */
  readonly protocol: string;
  /** Sends one message of at most 1 MiB; await before sending another.
   * Resolves after OS acceptance, without implying peer receipt.
   */
  readonly send: (data: string | Uint8Array) => Promise<void>;
  /** Sends a close frame; does not wait for the peer's reply.
   * Codes: 1000..1011 except 1004/1005/1006, or 3000..4999.
   * The UTF-8 reason is limited to 123 bytes. Defaults to 1000 and empty.
   */
  readonly close: (code?: number, reason?: string) => Promise<void>;
}

/** Observed connection closure; 1006 means no valid peer close arrived. */
export interface ServerWebSocketCloseEvent {
  /** Peer close code, 1005 for no code, or 1006 for abnormal closure. */
  readonly code: number;
  /** Peer close reason; empty for abnormal closure. */
  readonly reason: string;
  /** Whether both close frames were exchanged successfully. */
  readonly wasClean: boolean;
}

/** Page callbacks for authenticated WebSocket connections. */
export interface ServerWebSocketHandlers {
  /** Accepts an upgrade with an optional offered protocol, or rejects with 403.
   * Native Host, Origin and Bearer checks run first. Private credentials are
   * removed from the request headers. Errors produce a generic HTTP 500.
   */
  readonly accept: (
    request: Request
  ) =>
    | false
    | { readonly protocol?: string }
    | Promise<false | { readonly protocol?: string }>;
  /** Runs after the handshake is sent, before the first message callback. */
  readonly open?: (socket: ServerWebSocket) => void | Promise<void>;
  /** Handles text or binary; callbacks are awaited in connection order.
   * Other connections continue independently. Throwing closes with 1011.
   */
  readonly message?: (
    socket: ServerWebSocket,
    data: string | Uint8Array
  ) => void | Promise<void>;
  /** Runs once after closure; detach and server close report abnormal closure. */
  readonly close?: (
    socket: ServerWebSocket,
    event: ServerWebSocketCloseEvent
  ) => void | Promise<void>;
}

/** Address of a listening server. */
export interface ServerAddress {
  /** HTTP base URL, without a trailing slash. */
  readonly url: string;
  /** Numeric loopback address. */
  readonly host: string;
  /** Listening port. */
  readonly port: number;
}

/** Page handler attachment, independent of the server listener. */
export interface ServerBinding {
  /** Detaches handlers and aborts their pending requests; safe to repeat. */
  readonly detach: () => Promise<void>;
}

/** A page-owned native server, initially not listening. */
export interface Server {
  /** Native server identifier. */
  readonly id: string;
  /** Name supplied when creating the server. */
  readonly name: string;
  /** Attaches one handler set; detach it before attaching another set. */
  readonly attach: (handlers: ServerHandlers) => Promise<ServerBinding>;
  /** Starts accepting requests; an address conflict permits a later retry. */
  readonly listen: () => Promise<ServerAddress>;
  /** Permanently closes the server and aborts pending requests; safe to repeat. */
  readonly close: () => Promise<void>;
}

interface NativePacket {
  readonly metadata: unknown;
  readonly body: ArrayBuffer;
}

interface NativeRequest {
  readonly type: 'request' | 'ws_request';
  readonly requestId: string;
  readonly method: string;
  readonly url: string;
  readonly headers: [string, string][];
}

interface PendingRequest {
  readonly controller: AbortController;
  reader: ReadableStreamDefaultReader<Uint8Array> | undefined;
  upgradeRequest: Request | undefined;
}

interface BindingState {
  readonly id: string;
  readonly requests: Map<string, PendingRequest>;
  readonly websockets: Map<string, WebSocketState>;
  readonly detach: () => Promise<void>;
  active: boolean;
}

interface WebSocketState {
  readonly socket: ServerWebSocket;
  readonly notifyClose: (event: ServerWebSocketCloseEvent) => Promise<void>;
  closed: boolean;
}

interface NativeWebSocketEvent {
  readonly type: 'ws_open' | 'ws_message' | 'ws_close';
  readonly requestId: string;
  readonly protocol: string;
  readonly binary: boolean;
  readonly code: number;
  readonly reason: string;
  readonly wasClean: boolean;
}

const decodePacket = (buffer: ArrayBuffer): NativePacket => {
  if (!(buffer instanceof ArrayBuffer) || buffer.byteLength < 4)
    throw new Error('Invalid native server packet');
  const length = new DataView(buffer).getUint32(0);
  if (length > buffer.byteLength - 4)
    throw new Error('Invalid native server metadata length');
  return {
    metadata: JSON.parse(
      new TextDecoder().decode(new Uint8Array(buffer, 4, length))
    ),
    body: buffer.slice(4 + length),
  };
};

const abortBinding = (binding: BindingState): void => {
  binding.active = false;
  for (const request of binding.requests.values()) request.controller.abort();
  for (const socket of binding.websockets.values()) {
    queueMicrotask(
      async () =>
        await socket.notifyClose({ code: 1006, reason: '', wasClean: false })
    );
  }
};

const preserveReceivedHeaders = (
  request: Request,
  headers: Headers
): Request => {
  // Browser Request construction filters outbound-only forbidden headers such
  // as Host and Origin. This is an inbound request already checked by native
  // policy, so expose the received Headers and preserve them when cloning.
  // Keep the standard clone operation for body, signal and consumed-body rules.
  const clone = request.clone.bind(request);
  return Object.defineProperties(request, {
    headers: { value: headers },
    clone: {
      value: () => preserveReceivedHeaders(clone(), new Headers(headers)),
    },
  });
};

/**
 * Adapts the single authorized native operation to the public server API.
 * @param call - Native dispatch accepting metadata, bytes and an owner placeholder.
 * @returns The page-owned HTTP server factory.
 * @remarks The browser replaces the owner placeholder with the authenticated
 * renderer owner. Request handlers run concurrently. Response bodies are read
 * incrementally, awaiting native acceptance before sending more bytes.
 */
export const createMuonCefServerApi = (
  call: (json: string, body: ArrayBuffer, owner: string) => Promise<ArrayBuffer>
): { readonly createServer: (options: ServerOptions) => Promise<Server> } => {
  const dispatch = async (
    metadata: Record<string, unknown>,
    body: ArrayBuffer
  ): Promise<NativePacket> =>
    decodePacket(await call(JSON.stringify(metadata), body, ''));

  return {
    createServer: async (options) => {
      if (
        !options ||
        typeof options.name !== 'string' ||
        !options.name.trim() ||
        options.name.includes('\0')
      )
        throw new TypeError('Server name must be nonempty and contain no NUL');
      if (
        !Number.isInteger(options.port) ||
        options.port < 1025 ||
        options.port > 65535
      )
        throw new RangeError('Server port must be between 1025 and 65535');
      const host = options.host ?? '127.0.0.1';
      if (host !== '127.0.0.1')
        throw new TypeError('Server host must be a numeric loopback address');
      const created = (
        await dispatch(
          {
            operation: 'create',
            options: {
              name: options.name,
              port: options.port,
              host,
              bearerToken: options.bearerToken,
              allowedOrigins: options.allowedOrigins,
            },
          },
          new ArrayBuffer(0)
        )
      ).metadata as { id: string; name: string };
      let closed = false;
      let closing: Promise<void> | undefined;
      let attaching = false;
      let current: BindingState | undefined;
      const requireOpen = (): void => {
        if (closed) throw new Error('Server is closed');
      };
      const operation = async (
        name: string,
        fields: Record<string, unknown>,
        body: ArrayBuffer
      ): Promise<NativePacket> =>
        await dispatch({ operation: name, id: created.id, ...fields }, body);

      const handleRequest = async (
        binding: BindingState,
        handlers: ServerHandlers,
        event: NativeRequest,
        body: ArrayBuffer,
        pending: PendingRequest
      ): Promise<void> => {
        const signal = pending.controller.signal;
        const requestFields = {
          binding: binding.id,
          requestId: event.requestId,
        };
        let responding = false;
        let completed = false;
        let upgraded = false;
        // Cancellation must unblock a response reader even if its producer never
        // completes. Cancelling the stream cannot cancel arbitrary handler work.
        const cancelReader = async (): Promise<void> => {
          try {
            await pending.reader?.cancel();
          } catch {
            /* The reader may already be closed. */
          }
        };
        signal.addEventListener('abort', cancelReader, { once: true });
        try {
          if (!binding.active || signal.aborted) return;
          const request = preserveReceivedHeaders(
            new Request(event.url, {
              method: event.method,
              headers: event.headers,
              signal,
              ...(event.method === 'GET' || event.method === 'HEAD'
                ? {}
                : { body }),
            }),
            new Headers(event.headers)
          );
          if (event.type === 'ws_request') {
            const accepted = await handlers.websocket!.accept(request);
            if (!binding.active || signal.aborted) return;
            if (accepted === false) {
              responding = true;
              await operation(
                'respond',
                { ...requestFields, status: 403, headers: [] },
                new ArrayBuffer(0)
              );
            } else {
              if (
                !accepted ||
                typeof accepted !== 'object' ||
                (accepted.protocol !== undefined &&
                  typeof accepted.protocol !== 'string')
              )
                throw new TypeError(
                  'WebSocket accept must return false or protocol options'
                );
              // Native validation failures still permit an HTTP error response;
              // the native operation owns the transition to upgraded transport.
              pending.upgradeRequest = request;
              await operation(
                'ws_accept',
                { ...requestFields, protocol: accepted.protocol ?? '' },
                new ArrayBuffer(0)
              );
              responding = true;
              upgraded = true;
            }
            completed = true;
            return;
          }
          const response = await handlers.fetch(request);
          if (!(response instanceof Response) || response.status < 200)
            throw new TypeError('Server handler must return an HTTP Response');
          if (!binding.active || signal.aborted) {
            try {
              await response.body?.cancel();
            } catch {
              /* The body may already be consumed. */
            }
            return;
          }
          const headers: [string, string][] = [];
          response.headers.forEach((value, name) =>
            headers.push([name, value])
          );
          const fields = { ...requestFields, status: response.status, headers };
          if (response.body) pending.reader = response.body.getReader();
          if (
            !pending.reader ||
            event.method === 'HEAD' ||
            response.status === 204 ||
            response.status === 205 ||
            response.status === 304
          ) {
            responding = true;
            await operation('respond', fields, new ArrayBuffer(0));
            completed = true;
            await cancelReader();
            return;
          }

          // Treat an attempted start as committed: it may have written headers
          // before reporting failure, so a later HTTP 500 would corrupt the wire.
          responding = true;
          await operation('start', fields, new ArrayBuffer(0));
          while (binding.active && !signal.aborted) {
            const chunk = await pending.reader.read();
            if (!binding.active || signal.aborted) return;
            if (chunk.done) {
              await operation('finish', requestFields, new ArrayBuffer(0));
              completed = true;
              return;
            }
            if (!(chunk.value instanceof Uint8Array))
              throw new TypeError('Server response streams must contain bytes');
            for (
              let offset = 0;
              offset < chunk.value.byteLength;
              offset += 65536
            ) {
              if (!binding.active || signal.aborted) return;
              const bytes = new Uint8Array(
                chunk.value.subarray(offset, offset + 65536)
              );
              await operation('write', requestFields, bytes.buffer);
            }
          }
        } catch {
          if (!responding && binding.active && !signal.aborted) {
            try {
              responding = true;
              await operation(
                'respond',
                {
                  ...requestFields,
                  status: 500,
                  headers: [['content-type', 'text/plain;charset=UTF-8']],
                },
                new TextEncoder().encode('Internal Server Error').buffer
              );
              completed = true;
            } catch {
              /* The request may have disconnected during the fallback. */
            }
          }
        } finally {
          if (responding && !completed) {
            try {
              await operation('abort', requestFields, new ArrayBuffer(0));
            } catch {
              /* A disconnect or detach may already have removed the request. */
            }
          }
          if (!completed) await cancelReader();
          const reader = pending.reader;
          pending.reader = undefined;
          reader?.releaseLock();
          signal.removeEventListener('abort', cancelReader);
          if (!upgraded) binding.requests.delete(event.requestId);
        }
      };

      const handleWebSocketEvent = async (
        binding: BindingState,
        handlers: ServerWebSocketHandlers,
        event: NativeWebSocketEvent,
        body: ArrayBuffer
      ): Promise<void> => {
        const fields = { binding: binding.id, requestId: event.requestId };
        let state = binding.websockets.get(event.requestId);
        if (event.type === 'ws_close') {
          await state?.notifyClose({
            code: event.code,
            reason: event.reason,
            wasClean: event.wasClean,
          });
          return;
        }
        try {
          if (!binding.active) return;
          if (event.type === 'ws_open') {
            const pending = binding.requests.get(event.requestId);
            if (!pending?.upgradeRequest)
              throw new Error('WebSocket upgrade request is unavailable');
            let sending = false;
            let closingSocket: Promise<void> | undefined;
            const requireSocket = (): void => {
              if (!binding.active || state!.closed || closingSocket)
                throw new Error('WebSocket is closed');
            };
            const socket: ServerWebSocket = {
              id: event.requestId,
              request: pending.upgradeRequest,
              protocol: event.protocol,
              send: async (data) => {
                requireSocket();
                if (sending) throw new Error('WebSocket is already sending');
                if (typeof data !== 'string' && !(data instanceof Uint8Array))
                  throw new TypeError(
                    'WebSocket data must be string or Uint8Array'
                  );
                const binary = typeof data !== 'string';
                const bytes =
                  typeof data === 'string'
                    ? new TextEncoder().encode(data)
                    : new Uint8Array(data);
                if (bytes.byteLength > 1024 * 1024)
                  throw new RangeError('WebSocket message exceeds 1 MiB');
                sending = true;
                try {
                  await operation(
                    'ws_send',
                    { ...fields, binary },
                    bytes.buffer
                  );
                } finally {
                  sending = false;
                }
              },
              close: async (code = 1000, reason = '') => {
                if (!closingSocket) {
                  requireSocket();
                  closingSocket = (async () => {
                    await operation(
                      'ws_close',
                      { ...fields, code, reason },
                      new ArrayBuffer(0)
                    );
                  })();
                }
                const closingOperation = closingSocket;
                try {
                  await closingOperation;
                } catch (error) {
                  // Rejected arguments have not closed the native socket.
                  // Permit correction; transport failures remain native errors.
                  if (closingSocket === closingOperation)
                    closingSocket = undefined;
                  throw error;
                }
              },
            };
            state = {
              socket,
              closed: false,
              notifyClose: async (event) => {
                if (state!.closed) return;
                state!.closed = true;
                pending.controller.abort();
                binding.requests.delete(socket.id);
                binding.websockets.delete(socket.id);
                try {
                  await handlers.close?.(socket, event);
                } catch {
                  /* A terminal callback cannot alter native closure. */
                }
              },
            };
            binding.websockets.set(event.requestId, state);
            await handlers.open?.(socket);
          } else {
            if (!state || state.closed) return;
            await handlers.message?.(
              state.socket,
              event.binary
                ? new Uint8Array(body)
                : new TextDecoder('utf-8', { fatal: true }).decode(body)
            );
          }
        } catch {
          try {
            await operation(
              'ws_close',
              { ...fields, code: 1011, reason: 'Handler failed' },
              new ArrayBuffer(0)
            );
          } catch {
            try {
              await operation('abort', fields, new ArrayBuffer(0));
            } catch {
              /* Native ownership may already have ended. */
            }
          }
        } finally {
          try {
            await operation('ws_ack', fields, new ArrayBuffer(0));
          } catch {
            /* Disconnect or detach may have removed this event. */
          }
        }
      };

      const pump = async (
        binding: BindingState,
        handlers: ServerHandlers
      ): Promise<void> => {
        try {
          while (binding.active && !closed) {
            const packet = await operation(
              'next',
              { binding: binding.id },
              new ArrayBuffer(0)
            );
            if (!binding.active || closed) break;
            const event = packet.metadata as
              | NativeRequest
              | NativeWebSocketEvent
              | { type: 'disconnect'; requestId: string }
              | null;
            if (!event) break;
            if (event.type === 'disconnect') {
              binding.requests.get(event.requestId)?.controller.abort();
            } else if (
              event.type === 'ws_open' ||
              event.type === 'ws_message' ||
              event.type === 'ws_close'
            ) {
              queueMicrotask(
                async () =>
                  await handleWebSocketEvent(
                    binding,
                    handlers.websocket!,
                    event,
                    packet.body
                  )
              );
            } else if (
              event.type === 'request' ||
              event.type === 'ws_request'
            ) {
              const pending: PendingRequest = {
                controller: new AbortController(),
                reader: undefined,
                upgradeRequest: undefined,
              };
              binding.requests.set(event.requestId, pending);
              // Keep receiving disconnects and other requests while this handler
              // awaits application work. This is dispatch, not a polling timer.
              queueMicrotask(
                async () =>
                  await handleRequest(
                    binding,
                    handlers,
                    event,
                    packet.body,
                    pending
                  )
              );
            }
          }
        } catch {
          // Release a failed native wait before allowing a replacement binding.
          try {
            await binding.detach();
          } catch {
            /* Context teardown may already have released it. */
          }
        } finally {
          abortBinding(binding);
          if (current === binding) current = undefined;
        }
      };

      return {
        id: created.id,
        name: created.name,
        attach: async (handlers) => {
          requireOpen();
          if (!handlers || typeof handlers.fetch !== 'function')
            throw new TypeError('Server handlers must provide fetch');
          if (
            handlers.websocket &&
            (typeof handlers.websocket.accept !== 'function' ||
              ['open', 'message', 'close'].some((name) => {
                const value =
                  handlers.websocket![name as 'open' | 'message' | 'close'];
                return value !== undefined && typeof value !== 'function';
              }))
          )
            throw new TypeError('Invalid WebSocket handlers');
          if (attaching || current)
            throw new Error('Server already has an attached handler');
          attaching = true;
          try {
            const result = (
              await operation(
                'attach',
                { websocket: !!handlers.websocket },
                new ArrayBuffer(0)
              )
            ).metadata as { binding: string };
            requireOpen();
            let detaching: Promise<void> | undefined;
            const binding: BindingState = {
              id: result.binding,
              active: true,
              requests: new Map(),
              websockets: new Map(),
              detach: async () => {
                if (!detaching)
                  detaching = (async () => {
                    abortBinding(binding);
                    if (closed) await closing;
                    else
                      await operation(
                        'detach',
                        { binding: binding.id },
                        new ArrayBuffer(0)
                      );
                    if (current === binding) current = undefined;
                  })();
                await detaching;
              },
            };
            current = binding;
            queueMicrotask(async () => await pump(binding, handlers));
            return { detach: binding.detach };
          } finally {
            attaching = false;
          }
        },
        listen: async () => {
          requireOpen();
          if (!current?.active)
            throw new Error(
              'Server requires an attached handler before listening'
            );
          return (
            await operation(
              'listen',
              { binding: current.id },
              new ArrayBuffer(0)
            )
          ).metadata as ServerAddress;
        },
        close: async () => {
          if (!closing)
            closing = (async () => {
              closed = true;
              if (current) abortBinding(current);
              await operation('close', {}, new ArrayBuffer(0));
              current = undefined;
            })();
          await closing;
        },
      };
    },
  };
};

/**
 * Installs the permitted server factory over the private native dispatcher.
 * @param namespace - Built-in native server namespace.
 * @param isAllowed - Tests the application policy for createServer.
 * @returns After the public factory is installed.
 */
export const installMuonCefServerApi = (
  namespace: Record<string, unknown>,
  isAllowed: (name: string) => boolean
): void => {
  const dispatch = namespace.__dispatch;
  if (!isAllowed('createServer')) return;
  if (typeof dispatch !== 'function')
    throw new Error('Native server dispatcher is unavailable');
  const api = createMuonCefServerApi(
    async (json, body, owner) => await dispatch(json, body, owner)
  );
  Object.defineProperty(namespace, 'createServer', {
    value: api.createServer,
    enumerable: true,
  });
};
