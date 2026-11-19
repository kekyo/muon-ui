import type { ServiceCancellationSignal } from './service-connection.ts';
import type { ServiceIoBridge, ServiceIoReply } from './service-io.ts';

/** An authenticated HTTP request or WebSocket upgrade delivered to the listener. */
export interface ServiceServerRequest {
  /** HTTP method. */ readonly method: string;
  /** Absolute local request URL. */ readonly url: string;
  /** Received field pairs, preserving repeated header names. */ readonly headers: readonly (readonly [
    string,
    string,
  ])[];
  /** Owned request bytes. */ readonly body: Uint8Array;
}
/** Buffered reply from the service's HTTP handler. */
export interface ServiceServerResponse {
  /** HTTP status from 200 through 599, default 200. */ readonly status?: number;
  /** Response headers. */ readonly headers?: Readonly<Record<string, string>>;
  /** UTF-8 text or binary body, empty by default. */ readonly body?:
    string | Uint8Array;
}
/** One WebSocket owned by this listener and generation. */
export interface ServiceServerWebSocket {
  /** Listener-local connection identifier. */ readonly id: string;
  /** Negotiated subprotocol. */ readonly protocol: string;
  /** @param data Text or binary message. @returns Completion of the send. */ readonly send: (
    data: string | Uint8Array
  ) => Promise<void>;
  /** @param code WebSocket close code. @param reason UTF-8 reason. @returns Completion of the close frame. */ readonly close: (
    code?: number,
    reason?: string
  ) => Promise<void>;
}
/** WebSocket handlers invoked on the service dispatcher. */
export interface ServiceServerWebSocketHandlers {
  /** @param request Authorized upgrade. @returns False to reject, or optional negotiated subprotocol. */
  readonly accept: (
    request: ServiceServerRequest
  ) =>
    | false
    | { readonly protocol?: string }
    | Promise<false | { readonly protocol?: string }>;
  /** @param socket Newly upgraded connection. */
  readonly open?: (socket: ServiceServerWebSocket) => void | Promise<void>;
  /** @param socket Active connection. @param data UTF-8 text or binary message. */
  readonly message: (
    socket: ServiceServerWebSocket,
    data: string | Uint8Array
  ) => void | Promise<void>;
  /** @param socket Closed connection. @param event Peer close status. */
  readonly close?: (
    socket: ServiceServerWebSocket,
    event: { readonly code: number; readonly reason: string }
  ) => void | Promise<void>;
}
/** Loopback listener options. */
export interface ServiceServerOptions {
  /** Name unique to this generation. */ readonly name: string;
  /** Loopback TCP port from 1025 to 65535. */ readonly port: number;
  /** Serialized origins accepted from clients that send an Origin header. */ readonly allowedOrigins?: readonly string[];
  /** Required Bearer credential when configured. */ readonly bearerToken?: string;
  /** Signal controlling the listener's lifetime, defaulting to the generation's signal. */ readonly signal?: ServiceCancellationSignal;
  /** @param request Authorized HTTP request. @returns Response status, headers and body. */
  readonly fetch: (
    request: ServiceServerRequest
  ) => ServiceServerResponse | Promise<ServiceServerResponse>;
  /** Optional WebSocket upgrade and message handlers. */ readonly websocket?: ServiceServerWebSocketHandlers;
}
/** A listening loopback address and its release operation. */
export interface ServiceServerListener {
  /** Numeric loopback address. */ readonly host: string;
  /** Bound port. */ readonly port: number;
  /** Base HTTP URL. */ readonly url: string;
  /** Closes this listener and its active connections. */ readonly close: () => Promise<void>;
}
/** Local HTTP and WebSocket listeners owned by this service generation. */
export interface ServiceServer {
  /** @param options Port, request policy and handlers. @returns Active listener. */ readonly listen: (
    options: ServiceServerOptions
  ) => Promise<ServiceServerListener>;
}

interface ServerEvent {
  type: string;
  requestId: string;
  method: string;
  url: string;
  headers: [string, string][];
  protocol: string;
  binary: boolean;
  text: string;
  code: number;
  reason: string;
}
const content = (
  body: unknown
): { fields: Record<string, unknown>; bytes: Uint8Array } => {
  if (typeof body === 'string')
    return { fields: { text: body }, bytes: new Uint8Array() };
  if (body !== undefined && !(body instanceof Uint8Array))
    throw new TypeError('Invalid listener response body');
  return { fields: {}, bytes: body?.slice() ?? new Uint8Array() };
};

/** @internal Adapts the shared listener protocol to ES-only service handlers. */
export const createServiceServer = (
  io: ServiceIoBridge,
  lifetime: ServiceCancellationSignal
): ServiceServer => ({
  listen: async ({
    name,
    port,
    allowedOrigins,
    bearerToken,
    signal = lifetime,
    fetch,
    websocket,
  }) => {
    if (
      typeof fetch !== 'function' ||
      (websocket &&
        (typeof websocket.accept !== 'function' ||
          typeof websocket.message !== 'function'))
    )
      throw new TypeError('Invalid service listener handlers');
    const created = await io(
      'server.create',
      { options: { name, port, allowedOrigins, bearerToken } },
      new Uint8Array(),
      signal
    );
    const { id } = created.metadata as { id: string };
    let binding: string | undefined;
    let closed = false,
      closing: Promise<void> | undefined;
    const sockets = new Map<string, ServiceServerWebSocket>();
    const command = async (
      op: string,
      options: Record<string, unknown>,
      bytes?: Uint8Array
    ) =>
      await io(
        'server.' + op,
        { id, binding, ...options },
        bytes ?? new Uint8Array(),
        signal
      );
    const close = (): Promise<void> => {
      if (!closing) {
        closed = true;
        closing = (async () => {
          try {
            if (!signal.aborted) await command('close', {});
          } finally {
            sockets.clear();
          }
        })();
      }
      return closing;
    };
    const handle = async (event: ServerEvent, body: Uint8Array) => {
      const fields = { requestId: event.requestId };
      try {
        if (event.type === 'request' || event.type === 'ws_request') {
          const request = {
            method: event.method,
            url: event.url,
            headers: event.headers,
            body,
          };
          if (event.type === 'ws_request') {
            const accepted = await websocket?.accept(request);
            if (
              accepted &&
              typeof accepted === 'object' &&
              (accepted.protocol === undefined ||
                typeof accepted.protocol === 'string')
            ) {
              await command('ws_accept', {
                ...fields,
                protocol: accepted.protocol ?? '',
              });
              return;
            }
            await command('respond', { ...fields, status: 403, headers: [] });
            return;
          }
          const response = await fetch(request);
          const payload = content(response.body);
          await command(
            'respond',
            {
              ...fields,
              status: response.status ?? 200,
              headers: Object.entries(response.headers ?? {}),
              ...payload.fields,
            },
            payload.bytes
          );
          return;
        }
        if (event.type === 'cancel') {
          sockets.delete(event.requestId);
          return;
        }
        if (event.type === 'ws_close') {
          const socket = sockets.get(event.requestId);
          sockets.delete(event.requestId);
          if (socket)
            await websocket?.close?.(socket, {
              code: event.code,
              reason: event.reason,
            });
          return;
        }
        if (event.type === 'ws_open') {
          const socket: ServiceServerWebSocket = {
            id: event.requestId,
            protocol: event.protocol,
            send: async (data) => {
              const payload = content(data);
              await command(
                'ws_send',
                {
                  ...fields,
                  binary: typeof data !== 'string',
                  ...payload.fields,
                },
                payload.bytes
              );
            },
            close: async (code = 1000, reason = '') => {
              await command('ws_close', { ...fields, code, reason });
            },
          };
          sockets.set(socket.id, socket);
          await websocket?.open?.(socket);
        } else if (event.type === 'ws_message') {
          const socket = sockets.get(event.requestId);
          if (socket)
            await websocket?.message(socket, event.binary ? body : event.text);
        } else return;
        await command('ws_ack', fields);
      } catch {
        if (closed || signal.aborted) return;
        try {
          if (event.type === 'request' || event.type === 'ws_request')
            await command('respond', { ...fields, status: 500, headers: [] });
          else {
            await command('ws_close', {
              ...fields,
              code: 1011,
              reason: 'Handler failed',
            });
            await command('ws_ack', fields);
          }
        } catch {
          /* Peer or generation already closed. */
        }
      }
    };
    const pump = async () => {
      try {
        while (!closed && !signal.aborted) {
          const packet: ServiceIoReply = await command('next', {});
          if (closed || !packet.metadata) break;
          void handle(packet.metadata as ServerEvent, packet.body);
        }
      } catch {
        /* Closing the listener cancels its outstanding event wait. */
      } finally {
        if (!closed && !signal.aborted) {
          try {
            await close();
          } catch {
            /* Owner already stopped. */
          }
        }
      }
    };
    try {
      binding = (
        (await command('attach', { websocket: !!websocket })).metadata as {
          binding: string;
        }
      ).binding;
      const address = (await command('listen', {})).metadata as {
        host: string;
        port: number;
        url: string;
      };
      void pump();
      return Object.freeze({ ...address, close });
    } catch (error) {
      if (!signal.aborted) {
        try {
          await close();
        } catch {
          /* Preserve the setup error. */
        }
      }
      throw error;
    }
  },
});
