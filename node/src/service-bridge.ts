// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import {
  createServiceRuntime,
  defineService,
  type ServiceIoBridge,
  type ServiceIoReply,
  type ServiceDefinition,
  type ServiceInterface,
  type ServiceRuntime,
  type ServiceRuntimeInput,
  type ServiceRuntimeResult,
  type ServiceStopRequest,
} from 'muon-plugin-sdk/services';
import type { Readable, Writable } from 'node:stream';
import { createNodeServiceIo, type NodeServiceIoPolicy } from './service-io.ts';
import { createMuonNodeFrameDecoder, encodeMuonNodeFrame } from './framing.ts';
import {
  initializeMuonNodeProject,
  loadNodeServiceEntry,
} from './service-project.ts';

/** Streams and authentication inherited by a service-owned Node process. */
export interface MuonNodeServiceBridgeOptions {
  /** Framed requests from the owning service host. */ readonly input: Readable;
  /** Framed replies, callbacks, and events to the same host. */ readonly output: Writable;
  /** Secret required during initialization. */ readonly expectedToken: string;
  /** Cleanup deadline after transport loss, in milliseconds; defaults to 5000. */ readonly shutdownTimeoutMs?: number;
}

/** One logical service generation; a stopped child cannot host another one. */
export interface MuonNodeServiceBridge {
  /** Waits for explicit asynchronous stop or reports transport/protocol failure. */
  readonly waitForShutdown: () => Promise<void>;
  /** Cancels pending callbacks and awaits author cleanup within the deadline. */
  readonly close: () => Promise<void>;
}

const protocol = 'node-service/1';
const failure = (code: string, message: string) =>
  Object.assign(new Error(message), { code });
const text = (value: unknown): string => {
  if (typeof value !== 'string' || value.length === 0 || value.includes('\0'))
    throw failure(
      'ERR_MUON_NODE_PROTOCOL',
      'Expected a nonempty string without NUL'
    );
  return value;
};
const record = (value: unknown): Record<string, unknown> => {
  if (value === null || typeof value !== 'object' || Array.isArray(value))
    throw failure('ERR_MUON_NODE_PROTOCOL', 'Expected a protocol object');
  return value as Record<string, unknown>;
};
const result = (value: unknown): ServiceRuntimeResult => {
  const item = record(value);
  if (item.status === 'ok') return { status: 'ok', value: item.value };
  if (item.status === 'error' && typeof item.message === 'string')
    return {
      status: 'error',
      code: text(item.code),
      message: item.message,
    };
  throw failure('ERR_MUON_NODE_PROTOCOL', 'Invalid service callback result');
};

/**
 * Hosts the shared SDK using an authenticated, generation-owned Node connection.
 * @param options - Parent transport, project ownership, and emergency deadline.
 * @returns Lifecycle whose normal stop waits for all real SDK cleanup.
 * @remarks The native host owns startup/stop deadlines. The local close deadline
 * also prevents an orphaned child when its parent or transport disappears.
 */
export const createMuonNodeServiceBridge = (
  options: MuonNodeServiceBridgeOptions
): MuonNodeServiceBridge => {
  let runtime: ServiceRuntime | undefined;
  let identity: ServiceRuntimeInput | undefined;
  let initializing: Promise<unknown> | undefined;
  let ready = false;
  let startupCancelled = false;
  let stopping = false;
  let ended = false;
  let signaled = false;
  let stopOperation: Promise<void> | undefined;
  let closeOperation: Promise<void> | undefined;
  let callbackSequence = 0n;
  let activeRequests = 0;
  let queuedBytes = 0;
  let ioSequence = 0n;
  const ioRequests = new Map<
    string,
    { resolve: (reply: ServiceIoReply) => void; reject: (error: Error) => void }
  >();
  const disconnectIo = () => {
    for (const request of ioRequests.values())
      request.reject(failure('unavailable', 'Service I/O transport closed'));
    ioRequests.clear();
  };
  const callbacks = new Map<
    string,
    { callId: string; resolve: (value: ServiceRuntimeResult) => void }
  >();
  let resolveShutdown!: () => void;
  let rejectShutdown!: (reason: unknown) => void;
  const shutdown = new Promise<void>((resolve, reject) => {
    resolveShutdown = resolve;
    rejectShutdown = reject;
  });
  const signalShutdown = (error: unknown) => {
    if (signaled) return;
    signaled = true;
    if (error === undefined) resolveShutdown();
    else rejectShutdown(error);
  };
  const cancelCallbacks = (callId: string | undefined) => {
    for (const [id, callback] of callbacks) {
      if (callId !== undefined && callback.callId !== callId) continue;
      callbacks.delete(id);
      callback.resolve({
        status: 'error',
        code: 'cancelled',
        message: 'Service callback canceled',
      });
    }
  };
  const fail = (error: unknown) => {
    if (ended) return;
    ended = true;
    disconnectIo();
    options.input.pause();
    cancelCallbacks(undefined);
    signalShutdown(error);
  };
  const write = async (message: unknown): Promise<void> => {
    if (ended)
      throw failure('ERR_MUON_NODE_CLOSED', 'Service transport is closed');
    const frame = encodeMuonNodeFrame(message);
    if (queuedBytes + frame.length > 16 * 1024 * 1024)
      throw failure('resource-exhausted', 'Service output queue is full');
    queuedBytes += frame.length;
    try {
      await new Promise<void>((resolve, reject) => {
        options.output.write(frame, (error: Error | null | undefined) => {
          if (error) reject(error);
          else resolve();
        });
      });
    } finally {
      queuedBytes -= frame.length;
    }
  };
  const publish = async (message: unknown): Promise<void> => {
    try {
      await write(message);
    } catch (error) {
      fail(error);
    }
  };
  const stop = (request: ServiceStopRequest): Promise<void> => {
    stopping = true;
    cancelCallbacks(undefined);
    stopOperation ??= (async () => {
      // Import can still be pending before the runtime exists. Once attached,
      // stop cancels SDK startup directly instead of waiting for it first.
      if (!runtime && initializing) {
        try {
          await initializing;
        } catch {
          /* Failed initialization has no live instance. */
        }
      }
      await runtime?.stop(request);
    })();
    return stopOperation;
  };
  const hostIo: ServiceIoBridge = async (
    operation,
    parameters,
    body,
    signal
  ) => {
    if (ended || signal.aborted)
      throw failure('cancelled', 'Service I/O cancelled');
    const id = String(++ioSequence);
    const response = new Promise<ServiceIoReply>((resolve, reject) =>
      ioRequests.set(id, { resolve, reject })
    );
    const cancel = () => {
      void publish({ kind: 'ioCancel', id });
    };
    signal.addEventListener('abort', cancel, { once: true });
    try {
      // Send starts synchronously, preserving request/cancel order on the transport.
      const [, result] = await Promise.all([
        write({
          kind: 'io',
          id,
          operation,
          parameters,
          body: Buffer.from(body).toString('base64'),
        }),
        response,
      ]);
      return result;
    } finally {
      signal.removeEventListener('abort', cancel);
      ioRequests.delete(id);
    }
  };
  const initialize = async (params: Record<string, unknown>) => {
    const project = await initializeMuonNodeProject(
      params,
      options.expectedToken,
      protocol
    );
    const entry = text(params.entry);
    const imported = await loadNodeServiceEntry(project, entry);
    if (stopping || ended)
      throw failure('unavailable', 'Service startup was canceled');
    const definition = defineService(
      imported.default as ServiceDefinition<ServiceInterface>
    );
    identity = {
      startReason: text(
        params.startReason
      ) as ServiceRuntimeInput['startReason'],
      id: text(params.id),
      generation: text(params.generation),
      config: record(params.config) as Record<string, string>,
      dataDirectory: text(params.dataDirectory),
    };
    const files = createNodeServiceIo(
      identity.dataDirectory,
      record(params.io ?? {}) as NodeServiceIoPolicy
    );
    runtime = createServiceRuntime(definition, {
      io: async (op, params, body, signal) => {
        // Win32 directory handles must deny delete-sharing while names are resolved.
        const run =
          op.startsWith('fs.') && process.platform === 'linux' ? files : hostIo;
        return await run(op, params, body, signal);
      },
      callback: async (callId, token, args) => {
        if (stopping || ended)
          return {
            status: 'error',
            code: 'cancelled',
            message: 'Service is stopping',
          };
        if (callbacks.size >= 1024)
          throw failure('resource-exhausted', 'Too many service callbacks');
        const id = String(++callbackSequence);
        const completion = new Promise<ServiceRuntimeResult>((resolve) => {
          callbacks.set(id, { callId, resolve });
        });
        try {
          await write({ kind: 'callback', id, callId, token, arguments: args });
          return await completion;
        } finally {
          callbacks.delete(id);
        }
      },
      emit: (event, value) => {
        if (stopping || ended)
          throw failure('unavailable', 'Service is stopping');
        void publish({
          kind: 'event',
          serviceId: identity!.id,
          generation: identity!.generation,
          event,
          value,
        });
      },
    });
    if (startupCancelled) runtime.cancelStart();
    await runtime.start(identity);
    ready = true;
    return {
      protocol,
      nodeVersion: process.version,
      interface: definition.interface,
    };
  };
  const execute = async (
    command: string,
    params: Record<string, unknown>
  ): Promise<unknown> => {
    if (command === 'stop') {
      await stop({
        reason: text(params.reason) as ServiceStopRequest['reason'],
        ...(params.detail === undefined ? {} : { detail: text(params.detail) }),
        ...(params.remainingMs === undefined
          ? {}
          : { remainingMs: Number(params.remainingMs) }),
      });
      return { stopped: true };
    }
    if (stopping) throw failure('unavailable', 'Service is stopping');
    if (command === 'cancelStart' && initializing) {
      startupCancelled = true;
      runtime?.cancelStart();
      return {};
    }
    if (command === 'initialize') {
      if (initializing || runtime)
        throw failure(
          'ERR_MUON_NODE_ALREADY_INITIALIZED',
          'Service initialization already started'
        );
      initializing = initialize(params);
      try {
        return await initializing;
      } catch (error) {
        // Authentication/project rejection can be corrected before any author
        // runtime exists. A failed service generation itself cannot restart.
        if (!runtime) initializing = undefined;
        throw error;
      }
    }
    if (!ready || !runtime)
      throw failure('unavailable', 'Service is not running');
    const callId = text(params.callId);
    if (command === 'invoke') {
      if (!Array.isArray(params.arguments))
        throw failure(
          'ERR_MUON_NODE_PROTOCOL',
          'Service arguments must be an array'
        );
      return await runtime.invoke(
        callId,
        text(params.method),
        params.arguments
      );
    }
    if (command === 'cancel') {
      runtime.cancel(callId);
      cancelCallbacks(callId);
      return { canceled: true };
    }
    throw failure(
      'ERR_MUON_NODE_UNKNOWN_COMMAND',
      `Unknown service command ${command}`
    );
  };
  const request = async (message: Record<string, unknown>) => {
    const id = text(message.id);
    let stoppingRequest = false;
    ++activeRequests;
    try {
      let reply: unknown;
      try {
        if (activeRequests > 1024)
          throw failure('resource-exhausted', 'Too many service requests');
        const command = text(message.command);
        const value = await execute(command, record(message.params));
        stoppingRequest = command === 'stop';
        reply = { kind: 'response', id, ok: true, value };
      } catch (error) {
        const description =
          error instanceof Error ? error.message : String(error);
        const code =
          typeof (error as { code?: unknown })?.code === 'string'
            ? (error as { code: string }).code
            : 'ERR_MUON_NODE_REQUEST';
        reply = {
          kind: 'response',
          id,
          ok: false,
          error: { code, message: description },
        };
      }
      await write(reply);
      if (stoppingRequest) signalShutdown(undefined);
    } catch (error) {
      fail(error);
    } finally {
      --activeRequests;
    }
  };
  const decoder = createMuonNodeFrameDecoder((value) => {
    const message = record(value);
    if (message.kind === 'request') {
      // Validate before launching the asynchronous operation so malformed
      // identities cannot escape as detached promise rejections.
      text(message.id);
      void request(message);
    } else if (message.kind === 'ioResult') {
      const pending = ioRequests.get(text(message.id));
      if (!pending) return;
      if (message.error) {
        const error = record(message.error);
        pending.reject(failure(text(error.code), text(error.message)));
      } else if (typeof message.body === 'string') {
        pending.resolve({
          metadata: message.metadata,
          body: Uint8Array.from(Buffer.from(message.body, 'base64')),
        });
      } else
        throw failure('ERR_MUON_NODE_PROTOCOL', 'Invalid service I/O response');
    } else if (message.kind === 'callbackResult') {
      const id = text(message.id);
      if (!/^[1-9][0-9]{0,19}$/u.test(id) || BigInt(id) > callbackSequence)
        throw failure('ERR_MUON_NODE_PROTOCOL', 'Unknown service callback');
      const callback = callbacks.get(id);
      if (!callback) return; // Late replies to canceled callbacks are harmless.
      const completion = result(message.result);
      callbacks.delete(id);
      callback.resolve(completion);
    } else throw failure('ERR_MUON_NODE_PROTOCOL', 'Unknown service message');
  });
  const data = (chunk: unknown) => {
    try {
      decoder.write(chunk);
    } catch (error) {
      fail(error);
    }
  };
  const end = () => {
    try {
      decoder.finish();
    } catch (error) {
      fail(error);
      return;
    }
    ended = true;
    disconnectIo();
    cancelCallbacks(undefined);
    signalShutdown(
      stopping
        ? undefined
        : failure('ERR_MUON_NODE_CLOSED', 'Service host disconnected')
    );
  };
  options.input.on('data', data);
  options.input.on('end', end);
  options.input.on('error', fail);
  options.output.on('error', fail);
  return {
    waitForShutdown: async () => {
      await shutdown;
    },
    close: async () => {
      closeOperation ??= (async () => {
        ended = true;
        disconnectIo();
        options.input.off('data', data);
        options.input.off('end', end);
        options.input.off('error', fail);
        let timer: ReturnType<typeof setTimeout> | undefined;
        try {
          await Promise.race([
            stop({ reason: 'host-stop', detail: 'Service transport closed' }),
            new Promise<never>((_resolve, reject) => {
              timer = setTimeout(
                () =>
                  reject(
                    failure(
                      'ERR_MUON_NODE_STOP_TIMEOUT',
                      'Service cleanup exceeded its deadline'
                    )
                  ),
                options.shutdownTimeoutMs ?? 5000
              );
            }),
          ]);
        } finally {
          clearTimeout(timer);
          options.output.off('error', fail);
        }
      })();
      await closeOperation;
    },
  };
};
