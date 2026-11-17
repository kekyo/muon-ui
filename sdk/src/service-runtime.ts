import { parseServiceInterface } from './service-interface.ts';
import type {
  ServiceArguments,
  ServiceInterface,
  ServiceValue,
} from './service-interface.ts';
import type {
  ServiceCancellationSignal,
  ServiceFailure,
} from './service-connection.ts';
import {
  decodeServiceArguments,
  decodeServiceValue,
  encodeServiceArguments,
  encodeServiceValue,
} from './service-values.ts';

/** A JSON-compatible completion shared with native service implementations. */
export type ServiceRuntimeResult =
  | {
      /** Successful completion. */ readonly status: 'ok';
      /** Encoded return value. */ readonly value: unknown;
    }
  | {
      /** Failed completion. */ readonly status: 'error';
      /** Declared or framework code. */ readonly code: string;
      /** Human-readable diagnostic. */ readonly message: string;
    };

/** Immutable host inputs for one logical service generation. */
export interface ServiceRuntimeInput {
  /** Configured logical service name. */ readonly id: string;
  /** Positive decimal uint64 generation. */ readonly generation: string;
  /** Application-owned configuration strings. */ readonly config: Readonly<
    Record<string, string>
  >;
  /** Host-owned private directory; no filesystem API is implied. */ readonly dataDirectory: string;
}

/** Invocation-scoped cancellation; no page owner or native pointer is exposed. */
export interface ServiceInvocationContext {
  /** Host-assigned positive decimal uint64 invocation identity. */ readonly id: string;
  /** Aborted when cancellation or service stop begins. */ readonly signal: ServiceCancellationSignal;
}

/** Author-facing environment for one service instance. @typeParam Definition - Shared schema. */
export interface ServiceStartContext<
  Definition extends ServiceInterface,
> extends ServiceRuntimeInput {
  /** Aborted when service shutdown begins, including shutdown during start. */ readonly signal: ServiceCancellationSignal;
  /**
   * Publishes an event while this instance is running.
   * @param event - Declared event name.
   * @param value - Schema-compatible value, copied before host delivery.
   * @throws Error when the instance is stopping or the host rejects delivery.
   */
  readonly emit: <Event extends keyof Definition['events'] & string>(
    event: Event,
    value: ServiceValue<Definition['events'][Event]>
  ) => void;
}

/** Schema-derived service method implementations. @typeParam Definition - Shared schema. */
export type ServiceMethods<Definition extends ServiceInterface> = {
  readonly [Method in keyof Definition['methods']]: (
    context: ServiceInvocationContext,
    ...args: ServiceArguments<Definition['methods'][Method]['parameters']>
  ) =>
    | ServiceValue<Definition['methods'][Method]['returns']>
    | Promise<ServiceValue<Definition['methods'][Method]['returns']>>;
};

/** Running resources returned by an author. @typeParam Definition - Shared schema. */
export interface ServiceImplementation<Definition extends ServiceInterface> {
  /** Exactly the methods described by the shared interface. */ readonly methods: ServiceMethods<Definition>;
  /**
   * Releases instance resources after real invocations and callbacks have drained.
   * @param reason - Host-provided stop reason; the first request wins.
   * @returns Completion after every author-owned resource is released.
   */
  readonly stop: (reason: string) => void | Promise<void>;
}

/** Portable service entry point. @typeParam Definition - Shared literal schema. */
export interface ServiceDefinition<Definition extends ServiceInterface> {
  /** Interface used by page clients and the host broker. */ readonly interface: Definition;
  /**
   * Acquires resources for one logical service generation.
   * @param context - Immutable identity/configuration, cancellation and event access.
   * @returns Its methods and asynchronous cleanup.
   */
  readonly start: (
    context: ServiceStartContext<Definition>
  ) =>
    | ServiceImplementation<Definition>
    | Promise<ServiceImplementation<Definition>>;
}

/** Environment-specific delivery; all values here use the shared JSON codec. */
export interface ServiceRuntimeBridge {
  /**
   * Calls a page function within its pending parent invocation.
   * @param callId - Host invocation identity.
   * @param token - Callback token supplied in that invocation's arguments.
   * @param args - Encoded arguments, with a shared size budget.
   * @returns Encoded completion; cancellation must eventually settle host I/O.
   */
  readonly callback: (
    callId: string,
    token: string,
    args: readonly unknown[]
  ) => Promise<ServiceRuntimeResult>;
  /**
   * Enqueues an encoded event in the host's bounded delivery mechanism.
   * @param event - Declared event name.
   * @param value - Encoded event payload.
   */
  readonly emit: (event: string, value: unknown) => void;
}

/** Host-facing lifecycle and invocation adapter for one service instance. */
export interface ServiceRuntime {
  /**
   * Starts this instance once. Concurrent attempts share the first completion.
   * @param input - Validated host identity and private configuration.
   * @returns Startup completion; rejects on invalid inputs or author failure.
   */
  readonly start: (input: ServiceRuntimeInput) => Promise<void>;
  /**
   * Invokes a method and retains its resources through actual completion.
   * @param callId - Unique positive decimal uint64 identity while pending.
   * @param method - Declared method name.
   * @param args - Encoded ordered arguments.
   * @returns An encoded success/error envelope after callback delivery drains.
   */
  readonly invoke: (
    callId: string,
    method: string,
    args: readonly unknown[]
  ) => Promise<ServiceRuntimeResult>;
  /**
   * Signals cancellation and prevents new callback admission, idempotently.
   * @param callId - Invocation identity; already completed calls are ignored.
   */
  readonly cancel: (callId: string) => void;
  /**
   * Closes admission, cancels active calls, drains them, then awaits author cleanup.
   * @param reason - Host-provided stop reason; the first request wins.
   * @returns The shared shutdown completion.
   * @remarks This adapter never fabricates completion for pending author promises.
   * The embedding host owns deadlines and runtime interruption.
   */
  readonly stop: (reason: string) => Promise<void>;
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
const errorResult = (code: string, message: string): ServiceRuntimeResult => ({
  status: 'error',
  code,
  message,
});
const identity = (value: unknown): string => {
  if (
    typeof value !== 'string' ||
    !/^[1-9][0-9]{0,19}$/u.test(value) ||
    BigInt(value) > 18446744073709551615n
  )
    throw failure('invalid-result', 'Invalid service identity');
  return value;
};
const text = (value: unknown): string => {
  encodeServiceValue('string', value, undefined);
  return value as string;
};
const commonErrors = new Set([
  'cancelled',
  'unavailable',
  'invalid-result',
  'callback-failed',
  'resource-exhausted',
]);
const diagnostic = (
  error: unknown,
  declared: readonly string[]
): ServiceRuntimeResult => {
  try {
    const item = error as Partial<ServiceFailure> | null;
    const code = item?.code;
    if (
      typeof code === 'string' &&
      (commonErrors.has(code) || declared.includes(code))
    )
      return errorResult(code, text(item?.message));
  } catch {
    /* Malformed author failures are interface violations. */
  }
  return errorResult(
    'invalid-result',
    'Service completion does not match its interface'
  );
};
const cancellation = () => {
  let aborted = false;
  const listeners = new Set<() => void>();
  const signal: ServiceCancellationSignal = Object.freeze({
    get aborted() {
      return aborted;
    },
    addEventListener: (
      _type: 'abort',
      listener: () => void,
      _options: { readonly once: boolean }
    ) => {
      if (!aborted) listeners.add(listener);
    },
    removeEventListener: (_type: 'abort', listener: () => void) => {
      listeners.delete(listener);
    },
  });
  return {
    signal,
    abort: () => {
      if (aborted) return;
      aborted = true;
      const current = [...listeners];
      listeners.clear();
      // One author listener must not interrupt cancellation of the remaining work.
      for (const listener of current) {
        try {
          listener();
        } catch {}
      }
    },
    clear: () => listeners.clear(),
  };
};

/**
 * Declares a portable service with schema-derived implementation types.
 * @typeParam Definition - Literal interface retained for method/event inference.
 * @param definition - Shared interface and instance factory.
 * @returns An immutable declaration with a detached, validated interface.
 */
export const defineService = <const Definition extends ServiceInterface>(
  definition: ServiceDefinition<Definition>
): ServiceDefinition<Definition> => {
  if (!definition || typeof definition.start !== 'function')
    throw new TypeError('Service start must be callable');
  return Object.freeze({
    interface: parseServiceInterface(definition.interface) as Definition,
    start: definition.start,
  });
};

interface Invocation {
  id: string;
  source: ReturnType<typeof cancellation>;
  cancelled: boolean;
  finished: boolean;
  cancellation: ReturnType<typeof deferred<ServiceRuntimeResult>>;
  done: ReturnType<typeof deferred<ServiceRuntimeResult>>;
  callbacks: Set<Promise<void>>;
}

/**
 * Adapts a portable service to an authenticated host without Node or DOM globals.
 * @typeParam Definition - Shared literal interface.
 * @param definition - Author declaration, normally the bundle's default export.
 * @param bridge - Host callback and event delivery, with no filesystem assumptions.
 * @returns Single-generation lifecycle, cancellation, and encoded invocation methods.
 */
export const createServiceRuntime = <const Definition extends ServiceInterface>(
  definition: ServiceDefinition<Definition>,
  bridge: ServiceRuntimeBridge
): ServiceRuntime => {
  const declared = defineService(definition);
  if (
    !bridge ||
    typeof bridge.callback !== 'function' ||
    typeof bridge.emit !== 'function'
  )
    throw new TypeError('Invalid service host bridge');
  const source = cancellation();
  let phase:
    'new' | 'starting' | 'running' | 'stopping' | 'stopped' | 'failed' = 'new';
  let instance: ServiceImplementation<Definition> | undefined;
  let startup: Promise<void> | undefined, shutdown: Promise<void> | undefined;
  const calls = new Map<string, Invocation>();
  let callbacks = 0;
  const cancelCall = (call: Invocation) => {
    if (call.cancelled) return;
    call.cancelled = true;
    call.cancellation.resolve(
      errorResult('cancelled', 'Service invocation cancelled')
    );
    call.source.abort();
  };
  const start: ServiceRuntime['start'] = (input) => {
    if (phase === 'stopping' || phase === 'stopped' || phase === 'failed')
      return Promise.reject(failure('unavailable', 'Service cannot restart'));
    if (startup) return startup;
    const started = deferred<void>();
    startup = started.promise;
    phase = 'starting';
    const run = async () => {
      try {
        if (
          !input ||
          typeof input.config !== 'object' ||
          !input.config ||
          Array.isArray(input.config)
        )
          throw new TypeError('Invalid service configuration');
        const config = Object.freeze(
          Object.fromEntries(
            Object.entries(input.config).map(([key, value]) => [
              text(key),
              text(value),
            ])
          )
        );
        const context: ServiceStartContext<Definition> = Object.freeze({
          id: text(input.id),
          generation: identity(input.generation),
          dataDirectory: text(input.dataDirectory),
          config,
          signal: source.signal,
          emit: (
            event: keyof Definition['events'] & string,
            value: unknown
          ) => {
            if (phase !== 'running')
              throw failure('unavailable', 'Service event admission is closed');
            if (!Object.hasOwn(declared.interface.events, event))
              throw failure('invalid-result', 'Undeclared service event');
            const encoded = encodeServiceValue(
              declared.interface.events[event]!,
              value,
              undefined
            );
            // Author getters can synchronously request shutdown while encoding.
            if (phase !== 'running')
              throw failure('unavailable', 'Service event admission is closed');
            bridge.emit(event, encoded);
          },
        });
        instance = await declared.start(context);
        if (
          !instance ||
          typeof instance.stop !== 'function' ||
          !instance.methods ||
          Object.keys(instance.methods).length !==
            Object.keys(declared.interface.methods).length ||
          Object.keys(declared.interface.methods).some(
            (name) =>
              !Object.hasOwn(instance!.methods, name) ||
              typeof Reflect.get(instance!.methods, name) !== 'function'
          )
        )
          throw new TypeError('Service methods do not match the interface');
        if (phase === 'starting') phase = 'running';
        started.resolve();
      } catch (error) {
        phase = 'failed';
        source.abort();
        started.reject(error);
      }
    };
    void run();
    return startup;
  };
  const invoke: ServiceRuntime['invoke'] = (id, method, args) => {
    try {
      identity(id);
    } catch (error) {
      return Promise.resolve(diagnostic(error, []));
    }
    if (phase !== 'running' || !instance)
      return Promise.resolve(
        errorResult('unavailable', 'Service is not running')
      );
    if (calls.has(id))
      return Promise.resolve(
        errorResult('invalid-result', 'Duplicate service invocation identity')
      );
    if (calls.size >= 128)
      return Promise.resolve(
        errorResult('resource-exhausted', 'Too many service invocations')
      );
    if (!Object.hasOwn(declared.interface.methods, method))
      return Promise.resolve(
        errorResult('invalid-result', 'Undeclared service method')
      );
    const signature = declared.interface.methods[method]!;
    const call: Invocation = {
      id,
      source: cancellation(),
      cancelled: false,
      finished: false,
      cancellation: deferred<ServiceRuntimeResult>(),
      done: deferred<ServiceRuntimeResult>(),
      callbacks: new Set(),
    };
    calls.set(id, call);
    const run = async () => {
      let result: ServiceRuntimeResult;
      try {
        const tokens = new Set<string>();
        const values = decodeServiceArguments(
          signature.parameters,
          args,
          (token, callback) => {
            if (tokens.has(token) || tokens.size >= 1024)
              throw failure(
                'invalid-result',
                'Duplicate or excessive service callback tokens'
              );
            tokens.add(token);
            return async (...parameters: unknown[]) => {
              if (call.cancelled || call.finished || phase !== 'running')
                throw failure('cancelled', 'Callback invocation expired');
              if (call.callbacks.size >= 32 || callbacks >= 128)
                throw failure(
                  'resource-exhausted',
                  'Too many service callbacks'
                );
              const encoded = encodeServiceArguments(
                callback.parameters,
                parameters,
                undefined
              );
              // Encoding an author-provided array can reenter cancellation.
              if (call.cancelled || call.finished || phase !== 'running')
                throw failure('cancelled', 'Callback invocation expired');
              if (call.callbacks.size >= 32 || callbacks >= 128)
                throw failure(
                  'resource-exhausted',
                  'Too many service callbacks'
                );
              const drained = deferred<void>();
              call.callbacks.add(drained.promise);
              callbacks++;
              const deliver = async () => {
                try {
                  return await bridge.callback(id, token, encoded);
                } catch {
                  return errorResult(
                    'callback-failed',
                    'Page callback delivery failed'
                  );
                } finally {
                  callbacks--;
                  call.callbacks.delete(drained.promise);
                  drained.resolve();
                }
              };
              const response = await Promise.race([
                deliver(),
                call.cancellation.promise,
              ]);
              if (!response || response.status !== 'ok') {
                const checked = diagnostic(response, []);
                if (checked.status === 'error')
                  throw failure(checked.code, checked.message);
                throw failure(
                  'callback-failed',
                  'Invalid page callback completion'
                );
              }
              return decodeServiceValue(
                callback.returns,
                response.value,
                undefined
              );
            };
          }
        );
        const handler = Reflect.get(instance!.methods, method) as (
          context: ServiceInvocationContext,
          ...parameters: unknown[]
        ) => unknown;
        const value = await handler(
          Object.freeze({ id, signal: call.source.signal }),
          ...values
        );
        result = {
          status: 'ok',
          value: encodeServiceValue(signature.returns, value, undefined),
        };
      } catch (error) {
        result = diagnostic(error, signature.errors);
      }
      // Like native completion, a returned method cannot leave callbacks live.
      if (call.callbacks.size && result.status === 'ok')
        result = errorResult(
          'invalid-result',
          'Service completed before draining its callbacks'
        );
      call.finished = true;
      call.cancellation.resolve(
        errorResult('cancelled', 'Parent invocation completed')
      );
      await Promise.all([...call.callbacks]);
      if (call.cancelled)
        result = errorResult('cancelled', 'Service invocation cancelled');
      call.source.clear();
      calls.delete(id);
      call.done.resolve(result);
    };
    void run();
    return call.done.promise;
  };
  const cancel: ServiceRuntime['cancel'] = (id) => {
    identity(id);
    const call = calls.get(id);
    if (call) cancelCall(call);
  };
  const stop: ServiceRuntime['stop'] = (reason) => {
    if (shutdown) return shutdown;
    const stopped = deferred<void>();
    shutdown = stopped.promise;
    phase = 'stopping';
    source.abort();
    for (const call of calls.values()) cancelCall(call);
    const run = async () => {
      try {
        if (startup) {
          try {
            await startup;
          } catch {
            /* Failed start still permits cleanup of returned resources. */
          }
        }
        await Promise.all([...calls.values()].map((call) => call.done.promise));
        if (instance && typeof instance.stop === 'function')
          await instance.stop(reason);
        instance = undefined;
        phase = 'stopped';
        source.clear();
        stopped.resolve();
      } catch (error) {
        phase = 'failed';
        stopped.reject(error);
      }
    };
    void run();
    return shutdown;
  };
  return Object.freeze({ start, invoke, cancel, stop });
};
