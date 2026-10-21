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
}

/** Handlers attached to the current page. */
export interface ServerHandlers {
  /** Handles one HTTP request; its signal aborts when the request is cancelled. */
  readonly fetch: (request: Request) => Response | Promise<Response>;
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
  readonly type: 'request';
  readonly requestId: string;
  readonly method: string;
  readonly url: string;
  readonly headers: [string, string][];
}

interface PendingRequest {
  readonly controller: AbortController;
  reader: ReadableStreamDefaultReader<Uint8Array> | undefined;
}

interface BindingState {
  readonly id: string;
  readonly requests: Map<string, PendingRequest>;
  readonly detach: () => Promise<void>;
  active: boolean;
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
};

const readResponse = async (
  response: Response,
  pending: PendingRequest
): Promise<ArrayBuffer> => {
  if (!response.body) return new ArrayBuffer(0);
  const reader = response.body.getReader();
  pending.reader = reader;
  const chunks: Uint8Array[] = [];
  let length = 0;
  try {
    while (!pending.controller.signal.aborted) {
      const chunk = await reader.read();
      if (chunk.done) break;
      if (chunk.value.byteLength > 1024 * 1024 - length) {
        await reader.cancel();
        throw new RangeError('Server response body exceeds 1 MiB');
      }
      chunks.push(chunk.value);
      length += chunk.value.byteLength;
    }
    const bytes = new Uint8Array(length);
    let offset = 0;
    for (const chunk of chunks) {
      bytes.set(chunk, offset);
      offset += chunk.byteLength;
    }
    return bytes.buffer;
  } finally {
    pending.reader = undefined;
    reader.releaseLock();
  }
};

/**
 * Adapts the single authorized native operation to the public server API.
 * @param call - Native dispatch accepting metadata, bytes and an owner placeholder.
 * @returns The page-owned HTTP server factory.
 * @remarks The browser replaces the owner placeholder with the authenticated
 * renderer owner. Request handlers run concurrently; bodies are buffered in this
 * initial HTTP implementation, including Responses constructed from streams.
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
            options: { name: options.name, port: options.port, host },
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
          let response: Response;
          let bytes: ArrayBuffer;
          try {
            const request = new Request(event.url, {
              method: event.method,
              headers: event.headers,
              signal,
              ...(event.method === 'GET' || event.method === 'HEAD'
                ? {}
                : { body }),
            });
            response = await handlers.fetch(request);
            if (!(response instanceof Response) || response.status < 200)
              throw new TypeError(
                'Server handler must return an HTTP Response'
              );
            if (!binding.active || signal.aborted) {
              try {
                await response.body?.cancel();
              } catch {
                /* The body may already be consumed. */
              }
              return;
            }
            bytes = await readResponse(response, pending);
          } catch {
            response = new Response('Internal Server Error', { status: 500 });
            bytes = await response.arrayBuffer();
          }
          if (!binding.active || signal.aborted) return;
          const headers: [string, string][] = [];
          response.headers.forEach((value, name) =>
            headers.push([name, value])
          );
          await operation(
            'respond',
            {
              binding: binding.id,
              requestId: event.requestId,
              status: response.status,
              headers,
            },
            bytes
          );
        } catch {
          // A disconnect or detach can invalidate a response after the local
          // signal check. Native ownership checks decide whether it is accepted.
        } finally {
          signal.removeEventListener('abort', cancelReader);
          binding.requests.delete(event.requestId);
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
              NativeRequest | { type: 'disconnect'; requestId: string } | null;
            if (!event) break;
            if (event.type === 'disconnect') {
              binding.requests.get(event.requestId)?.controller.abort();
            } else {
              const pending: PendingRequest = {
                controller: new AbortController(),
                reader: undefined,
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
          if (attaching || current)
            throw new Error('Server already has an attached handler');
          attaching = true;
          try {
            const result = (await operation('attach', {}, new ArrayBuffer(0)))
              .metadata as { binding: string };
            requireOpen();
            let detaching: Promise<void> | undefined;
            const binding: BindingState = {
              id: result.binding,
              active: true,
              requests: new Map(),
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
