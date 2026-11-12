import { parseServiceInterface } from './service-interface.ts';
import type {
  ServiceArguments,
  ServiceCallbackType,
  ServiceInterface,
  ServiceMethod,
  ServiceType,
  ServiceValue,
} from './service-interface.ts';
import {
  decodeServiceArguments,
  decodeServiceValue,
  encodeServiceArguments,
  encodeServiceValue,
} from './service-values.ts';
import type { ServiceCallback } from './service-values.ts';

/** RPC operations carried over an already authenticated service reference. */
export type ServiceTransportAction =
  | 'begin'
  | 'result'
  | 'cancel'
  | 'callbackNext'
  | 'callbackResult'
  | 'subscribe'
  | 'eventNext'
  | 'unsubscribe';
/**
 * Exchanges one decoded JSON request and response with the service broker.
 * @param action - Service operation, including held event/callback reads.
 * @param payload - Operation-specific JSON fields; reference authentication is external.
 * @returns Decoded JSON response; disconnection rejects every outstanding request.
 */
export type ServiceTransport = (
  action: ServiceTransportAction,
  payload: Readonly<Record<string, unknown>>
) => Promise<unknown>;
/** A terminal service or subscription failure. */
export interface ServiceFailure {
  /** Stable application or framework error code. */
  readonly code: string;
  /** Human-readable diagnostic. */
  readonly message: string;
}
/** Optional cancellation of a single invocation. */
export interface ServiceCallOptions {
  /** Aborting rejects the public call while its native completion is drained. */
  readonly signal?: AbortSignal;
}
/** One non-owning FIFO event subscription. */
export interface ServiceSubscription {
  /** Always resolves; failures and overflow are reported as a value. */
  readonly closed: Promise<ServiceFailure | undefined>;
  /**
   * Stops delivery and releases the subscription; repeated calls share a promise.
   * @returns Completion of unsubscribe and local delivery cleanup.
   */
  readonly close: () => Promise<void>;
}
/** Service calls and subscriptions tied to an externally owned service reference. */
export interface ServiceConnection {
  /**
   * Calls a declared method with typed runtime validation and scoped callbacks.
   * @param method - Declared method name.
   * @param args - Ordered arguments; 64-bit integers use bigint.
   * @param options - Optional cancellation of this call.
   * @returns Decoded result, or rejection carrying the service error code.
   */
  readonly call: (
    method: string,
    args: readonly unknown[],
    options?: ServiceCallOptions
  ) => Promise<unknown>;
  /**
   * Subscribes to a declared event; listeners run sequentially in delivery order.
   * @param event - Declared event name.
   * @param listener - Receives each decoded value; rejection closes the subscription.
   * @returns A subscription whose closed promise reports failures as values.
   */
  readonly subscribe: (
    event: string,
    listener: (value: unknown) => void | Promise<void>
  ) => Promise<ServiceSubscription>;
  /**
   * Cancels calls and closes subscriptions, including requests awaiting their first ACK.
   * @returns Completion of local cleanup and pending transport operations.
   */
  readonly close: () => Promise<void>;
}

/**
 * Statically typed calls and events for one literal service interface.
 * @typeParam Definition - Complete literal method and event signatures.
 */
export interface ServiceClient<Definition extends ServiceInterface> {
  /**
   * Invokes a declared method using the argument and result types in Definition.
   * @typeParam Method - A declared method name.
   * @param method - Method to invoke.
   * @param args - Arguments matching the method's signature.
   * @param options - Optional cancellation of this call.
   * @returns The signature's decoded result.
   */
  readonly call: <Method extends keyof Definition['methods'] & string>(
    method: Method,
    args: ServiceArguments<Definition['methods'][Method]['parameters']>,
    options?: ServiceCallOptions
  ) => Promise<ServiceValue<Definition['methods'][Method]['returns']>>;
  /**
   * Delivers declared event values sequentially until the subscription closes.
   * @typeParam Event - A declared event name.
   * @param event - Event to subscribe to.
   * @param listener - Receives decoded values in FIFO order.
   * @returns A non-owning subscription with explicit terminal status.
   */
  readonly subscribe: <Event extends keyof Definition['events'] & string>(
    event: Event,
    listener: (
      value: ServiceValue<Definition['events'][Event]>
    ) => void | Promise<void>
  ) => Promise<ServiceSubscription>;
  /**
   * Cancels pending calls and subscriptions without releasing the external reference.
   * @returns Completion of local cleanup and pending transport operations.
   */
  readonly close: () => Promise<void>;
}

const deferred = <T>() => {
  let resolve!: (value: T) => void;
  let reject!: (error: unknown) => void;
  const promise = new Promise<T>((yes, no) => {
    resolve = yes;
    reject = no;
  });
  return { promise, resolve, reject };
};
const failure = (code: string, message: string): Error & ServiceFailure =>
  Object.assign(new Error(message), { code });
const record = (value: unknown): Record<string, unknown> => {
  if (!value || typeof value !== 'object' || Array.isArray(value))
    throw failure('invalid-result', 'Invalid service response');
  return value as Record<string, unknown>;
};
const identity = (value: unknown): string => {
  if (
    typeof value !== 'string' ||
    !/^[1-9][0-9]{0,19}$/.test(value) ||
    BigInt(value) > 18446744073709551615n
  )
    throw failure('invalid-result', 'Invalid service identity');
  return value;
};
const readFailure = (value: unknown): ServiceFailure => {
  const item = record(value);
  if (
    typeof item.code !== 'string' ||
    !item.code ||
    typeof item.message !== 'string'
  )
    throw failure('invalid-result', 'Invalid service failure');
  return { code: item.code, message: item.message };
};
const disconnected = (error: unknown): ServiceFailure => ({
  code: 'disconnected',
  message:
    error instanceof Error ? error.message : 'Service transport disconnected',
});
const resultValue = (type: ServiceType, value: unknown): unknown => {
  const result = record(value);
  if (result.status === 'ok')
    return decodeServiceValue(type, result.value, undefined);
  if (result.status === 'error') {
    const error = readFailure(result);
    throw failure(error.code, error.message);
  }
  throw failure('invalid-result', 'Invalid service result envelope');
};

interface Call {
  id: string | undefined;
  cancelled: boolean;
  ended: boolean;
  cancellation: Promise<void> | undefined;
  readonly result: ReturnType<typeof deferred<unknown>>;
  readonly terminal: ReturnType<typeof deferred<void>>;
  readonly finished: ReturnType<typeof deferred<void>>;
  readonly callbacks: Map<
    string,
    { callback: ServiceCallback; signature: ServiceCallbackType }
  >;
  activeCallbacks: number;
}
interface Subscription {
  id: string | undefined;
  stopping: boolean;
  ended: boolean;
  closing: Promise<void> | undefined;
  readonly terminal: ReturnType<typeof deferred<void>>;
  readonly finished: ReturnType<typeof deferred<void>>;
  readonly closed: ReturnType<typeof deferred<ServiceFailure | undefined>>;
}

/**
 * Creates a browser-safe service client over an authenticated RPC transport.
 * @param definition - Shared interface metadata, validated and copied on creation.
 * @param transport - Request/response adapter bound to one owner, reference, and generation.
 * @returns Independent calls and subscriptions, without acquiring another lifetime reference.
 * @remarks Close cancels calls and releases subscriptions. It drains transport
 * operations but never waits for arbitrary user callback or listener promises.
 */
export const createServiceConnection = (
  definition: ServiceInterface,
  transport: ServiceTransport
): ServiceConnection => {
  const schema = parseServiceInterface(definition);
  const calls = new Set<Call>();
  const subscriptions = new Set<Subscription>();
  let closing: Promise<void> | undefined;
  let closed = false;

  const cancelRemote = (call: Call): Promise<void> => {
    if (!call.id || call.ended) return Promise.resolve();
    if (!call.cancellation)
      call.cancellation = (async () => {
        try {
          await transport('cancel', { callId: call.id });
        } catch {
          /* The result request observes disconnection too. */
        }
      })();
    return call.cancellation;
  };
  const cancel = (call: Call): void => {
    if (call.cancelled || call.ended) return;
    call.cancelled = true;
    call.callbacks.clear();
    call.terminal.resolve();
    call.result.reject(
      failure('cancelled', 'Service invocation was cancelled')
    );
  };
  const performCallback = async (
    call: Call,
    item: Record<string, unknown>
  ): Promise<void> => {
    const callbackId = identity(item.callbackId);
    const token = identity(item.token);
    const handler = call.callbacks.get(token);
    let result: unknown;
    if (!handler || call.cancelled || call.ended)
      result = {
        status: 'error',
        code: 'cancelled',
        message: 'Callback expired',
      };
    else if (call.activeCallbacks >= 32)
      result = {
        status: 'error',
        code: 'resource-exhausted',
        message: 'Too many concurrent callbacks',
      };
    else {
      ++call.activeCallbacks;
      try {
        const args = decodeServiceArguments(
          handler.signature.parameters,
          item.arguments,
          undefined
        );
        const work = (async () => {
          try {
            const value = await handler.callback(...args);
            return {
              status: 'ok',
              value: encodeServiceValue(
                handler.signature.returns,
                value,
                undefined
              ),
            };
          } catch (error) {
            return {
              status: 'error',
              code: 'callback-failed',
              message:
                error instanceof Error ? error.message : 'Page callback failed',
            };
          }
        })();
        result = await Promise.race([work, call.terminal.promise]);
      } catch (error) {
        result = {
          status: 'error',
          code: 'callback-failed',
          message:
            error instanceof Error
              ? error.message
              : 'Invalid callback arguments',
        };
      } finally {
        --call.activeCallbacks;
      }
    }
    if (!call.cancelled && !call.ended)
      await transport('callbackResult', {
        callId: call.id,
        callbackId,
        result,
      });
  };
  const pumpCallbacks = async (call: Call): Promise<void> => {
    const running = new Map<object, Promise<void>>();
    try {
      // Cancellation still permits the first held read, pairing the broker's
      // per-call channel with a terminal response after begin has acknowledged.
      for (;;) {
        const response = await Promise.race([
          transport('callbackNext', { callId: call.id }),
          call.terminal.promise,
        ]);
        if (response === undefined || call.ended || call.cancelled) break;
        const item = record(response);
        if (item.done === true) break;
        if (item.done !== false)
          throw failure('invalid-result', 'Invalid callback response');
        if (running.size >= 64)
          throw failure(
            'resource-exhausted',
            'Callback replies are not draining'
          );
        const key = {};
        const task = (async () => {
          try {
            await performCallback(call, item);
          } catch (error) {
            call.result.reject(error);
            cancel(call);
            await cancelRemote(call);
          } finally {
            running.delete(key);
          }
        })();
        running.set(key, task);
        // Delivery is rearmed immediately; a callback can await another service
        // invocation whose callbacks must be read before this one can finish.
      }
    } catch (error) {
      call.result.reject(error);
      cancel(call);
      await cancelRemote(call);
    } finally {
      await Promise.all(running.values());
    }
  };
  const runCall = async (
    call: Call,
    method: string,
    signature: ServiceMethod,
    args: readonly unknown[],
    signal: AbortSignal | undefined
  ): Promise<void> => {
    const abort = (): void => {
      cancel(call);
    };
    let pump: Promise<void> | undefined;
    try {
      signal?.addEventListener('abort', abort, { once: true });
      if (signal?.aborted) cancel(call);
      if (call.cancelled && !call.id) return;
      const response = record(
        await transport('begin', { method, arguments: args })
      );
      call.id = identity(response.callId);
      const result = (async () => {
        try {
          return {
            ok: true as const,
            value: await transport('result', { callId: call.id }),
          };
        } catch (error) {
          return { ok: false as const, error };
        }
      })();
      pump = pumpCallbacks(call);
      if (call.cancelled || closed) {
        cancel(call);
        await cancelRemote(call);
      }
      // An abort can arrive after the result request is pending. Wake a separate
      // cancellation task without removing the result request from the transport.
      const cancellation = (async () => {
        await call.terminal.promise;
        if (call.cancelled) await cancelRemote(call);
      })();
      try {
        const response = await result;
        if (!response.ok) throw response.error;
        call.result.resolve(resultValue(signature.returns, response.value));
      } finally {
        call.ended = true;
        call.callbacks.clear();
        call.terminal.resolve();
        await cancellation;
      }
    } catch (error) {
      call.result.reject(error);
    } finally {
      call.ended = true;
      call.callbacks.clear();
      call.terminal.resolve();
      try {
        signal?.removeEventListener('abort', abort);
      } catch {
        // Foreign signal implementations cannot prevent call/channel cleanup.
      }
      if (pump) await pump;
      if (call.cancellation) await call.cancellation;
      calls.delete(call);
      call.finished.resolve();
    }
  };

  const stopSubscription = (item: Subscription): Promise<void> => {
    item.stopping = true;
    item.terminal.resolve();
    if (!item.closing)
      item.closing = (async () => {
        if (item.id && !item.ended) {
          try {
            await transport('unsubscribe', { subscriptionId: item.id });
          } catch (error) {
            item.closed.resolve(disconnected(error));
          }
        }
      })();
    return item.closing;
  };
  const runSubscription = async (
    item: Subscription,
    type: ServiceType,
    listener: (value: unknown) => void | Promise<void>
  ): Promise<void> => {
    try {
      while (!item.stopping) {
        let response: unknown;
        try {
          response = await Promise.race([
            transport('eventNext', { subscriptionId: item.id }),
            item.terminal.promise,
          ]);
        } catch (error) {
          item.closed.resolve(disconnected(error));
          break;
        }
        if (item.stopping || response === undefined) break;
        const value = record(response);
        if (value.done === true) {
          item.closed.resolve(
            value.error === undefined ? undefined : readFailure(value.error)
          );
          break;
        }
        if (value.done !== false)
          throw failure('invalid-result', 'Invalid event response');
        const payload = decodeServiceValue(type, value.value, undefined);
        try {
          await Promise.race([
            (async () => {
              await listener(payload);
            })(),
            item.terminal.promise,
          ]);
        } catch (error) {
          item.closed.resolve({
            code: 'listener-failed',
            message:
              error instanceof Error ? error.message : 'Event listener failed',
          });
          await stopSubscription(item);
        }
      }
    } catch (error) {
      item.closed.resolve({
        code: 'invalid-result',
        message:
          error instanceof Error ? error.message : 'Invalid event response',
      });
      await stopSubscription(item);
    } finally {
      item.ended = true;
      item.closed.resolve(undefined);
      subscriptions.delete(item);
      item.finished.resolve();
    }
  };

  return {
    call: async (method, args, options) => {
      if (closed) throw failure('closed', 'Service connection is closed');
      const signature = schema.methods[method];
      if (!signature) throw new TypeError('Unknown service method');
      const call: Call = {
        id: undefined,
        cancelled: false,
        ended: false,
        cancellation: undefined,
        result: deferred<unknown>(),
        terminal: deferred<void>(),
        finished: deferred<void>(),
        callbacks: new Map(),
        activeCallbacks: 0,
      };
      let token = 0;
      const encoded = encodeServiceArguments(
        signature.parameters,
        args,
        (callback, callbackType) => {
          if (++token > 1024) throw new TypeError('Too many input callbacks');
          const id = String(token);
          call.callbacks.set(id, { callback, signature: callbackType });
          return id;
        }
      );
      calls.add(call);
      const execution = runCall(
        call,
        method,
        signature,
        encoded,
        options?.signal
      );
      try {
        return await call.result.promise;
      } finally {
        // The internal execution retains the result request until terminal even
        // when the public promise has already rejected due to cancellation.
        if (call.ended) await execution;
      }
    },
    subscribe: async (event, listener) => {
      if (closed) throw failure('closed', 'Service connection is closed');
      const type = schema.events[event];
      if (!type || typeof listener !== 'function')
        throw new TypeError('Invalid service subscription');
      const item: Subscription = {
        id: undefined,
        stopping: false,
        ended: false,
        closing: undefined,
        terminal: deferred<void>(),
        finished: deferred<void>(),
        closed: deferred<ServiceFailure | undefined>(),
      };
      subscriptions.add(item);
      try {
        item.id = identity(
          record(await transport('subscribe', { event })).subscriptionId
        );
        if (closed || item.stopping) {
          item.closing = undefined;
          await stopSubscription(item);
        }
        const execution = runSubscription(item, type, listener);
        let closePromise: Promise<void> | undefined;
        return {
          closed: item.closed.promise,
          close: () => {
            if (!closePromise)
              closePromise = (async () => {
                await stopSubscription(item);
                await execution;
              })();
            return closePromise;
          },
        };
      } catch (error) {
        item.ended = true;
        item.closed.resolve(disconnected(error));
        subscriptions.delete(item);
        item.finished.resolve();
        throw error;
      }
    },
    close: () => {
      if (!closing) {
        closed = true;
        for (const call of calls) cancel(call);
        closing = (async () => {
          const stops = [...subscriptions].map(stopSubscription);
          await Promise.all([...calls].map((call) => call.finished.promise));
          await Promise.all(
            [...subscriptions].map((item) => item.finished.promise)
          );
          await Promise.all(stops);
        })();
      }
      return closing;
    },
  };
};

/**
 * Creates a statically typed client using a shared literal service interface.
 * @typeParam Definition - Method and event definitions with literal type information.
 * @param definition - Interface created with defineServiceInterface or an equivalent literal.
 * @param transport - Authenticated request/response adapter for one service reference.
 * @returns The same runtime validation and lifecycle as createServiceConnection.
 */
export const createServiceClient = <const Definition extends ServiceInterface>(
  definition: Definition,
  transport: ServiceTransport
): ServiceClient<Definition> =>
  createServiceConnection(
    definition,
    transport
  ) as unknown as ServiceClient<Definition>;
