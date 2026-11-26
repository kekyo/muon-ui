import { createServiceServer, type ServiceServer } from './service-server.ts';
import { createServiceHttp, type ServiceHttpClient } from './service-http.ts';
import { parseServiceInterface } from './service-interface.ts';
import {
  createServiceFiles,
  type ServiceFileSystem,
  type ServiceIoBridge,
} from './service-io.ts';
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

const startReasons = [
  'acquire',
  'app-start',
  'login-autostart',
  'autostart-enabled',
  'development-update',
] as const;
const stopReasons = [
  'reference-released',
  'app-exit',
  'session-ending',
  'development-update',
  'development-exit',
  'execution-timeout',
  'host-stop',
] as const;

/** The demand that started this generation, fixed until it stops. */
export type ServiceStartReason = (typeof startReasons)[number];
/** The first termination request accepted by the host for this generation. */
export type ServiceStopReason = (typeof stopReasons)[number];

/** Host observation passed to the runtime at the beginning of shutdown. */
export interface ServiceStopRequest {
  /** Why this generation is stopping. */ readonly reason: ServiceStopReason;
  /** Additional platform information when available. */ readonly detail?: string;
  /** Remaining cleanup time in milliseconds when the host has a deadline. */ readonly remainingMs?: number;
}

/** Information passed to the author's cleanup handler. */
export interface ServiceStopInfo extends ServiceStopRequest {
  /** Cancels cleanup I/O when the host deadline expires. */ readonly signal: ServiceCancellationSignal;
}

/** Immutable host inputs for one logical service generation. */
export interface ServiceRuntimeInput {
  /** Why the host started this generation. */ readonly startReason: ServiceStartReason;
  /** Configured logical service name. */ readonly id: string;
  /** Positive decimal uint64 generation. */ readonly generation: string;
  /** Application-owned configuration strings. */ readonly config: Readonly<
    Record<string, string>
  >;
  /** Absolute service-private directory. */ readonly dataDirectory: string;
}

/** Invocation-scoped cancellation; no page owner or native pointer is exposed. */
export interface ServiceInvocationContext {
  /** Host-assigned positive decimal uint64 invocation identity. */ readonly id: string;
  /** Aborted when cancellation or service stop begins. */ readonly signal: ServiceCancellationSignal;
}

/** Android foreground purposes accepted by the service configuration. */
export type AndroidForegroundServiceType =
  | 'camera'
  | 'connectedDevice'
  | 'dataSync'
  | 'health'
  | 'location'
  | 'mediaPlayback'
  | 'mediaProcessing'
  | 'mediaProjection'
  | 'microphone'
  | 'phoneCall'
  | 'remoteMessaging'
  | 'shortService'
  | 'specialUse'
  | 'systemExempted';

/** Android operations belonging to the current service generation. */
export interface ServiceAndroid {
  /**
   * Replace the active foreground purposes of an on-demand Android service.
   * @param types - Currently used types from the service declaration. An empty list removes its notification.
   * @returns Completion after Android accepts the change.
   * @throws A permission-denied error when the host, declaration or Android permissions prevent promotion.
   */
  readonly setForegroundServiceTypes: (
    types: readonly AndroidForegroundServiceType[]
  ) => Promise<void>;
}

/** Author-facing environment for one service instance. @typeParam Definition - Shared schema. */
export interface ServiceStartContext<
  Definition extends ServiceInterface,
> extends ServiceRuntimeInput {
  /** Generation-scoped Android service operations. */ readonly android: ServiceAndroid;
  /** Keep accepted work running after the last page releases its reference.
   * @returns An idempotent release function for this job.
   * @remarks Available on supporting QuickJS hosts. Call after startup and release
   * when the job finishes. The grant applies only to this generation.
   */
  readonly retainWork?: () => () => void;
  /** Local listeners and connections owned by this generation. */ readonly server: ServiceServer;
  /** Outbound HTTP and HTTPS with per-origin permissions. */ readonly http: ServiceHttpClient;
  /** Permission-checked files, private to the service unless explicitly shared. */ readonly fs: ServiceFileSystem;
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
   * @param info - First accepted reason and cancellation signal for cleanup.
   * @returns Completion after every author-owned resource is released.
   */
  readonly stop: (info: ServiceStopInfo) => void | Promise<void>;
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
  /** Change transient work demand for this generation.
   * @param active - Whether a job still needs the runtime.
   */
  readonly setWorkActive?: (active: boolean) => void;
  /** Performs permission-checked I/O for this generation. */ readonly io?: ServiceIoBridge;
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
  /** Cancels asynchronous initialization when demand is withdrawn during startup. */
  readonly cancelStart: () => void;
  /**
   * Closes admission, cancels active calls, drains them, then awaits author cleanup.
   * @param request - First accepted reason, platform detail and remaining time.
   * @returns The shared shutdown completion.
   * @remarks This adapter never fabricates completion for pending author promises.
   * The embedding host owns deadlines and runtime interruption.
   */
  readonly stop: (request: ServiceStopRequest) => Promise<void>;
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
  'permission-denied',
  'not-found',
  'io-error',
  'invalid-input',
  'timeout',
  'size-limit',
  'tls-error',
  'network-error',
  'redirect-limit',
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
  let activeJobs = 0;
  const setWorkActive = bridge.setWorkActive;
  const retainWork = () => {
    if (phase !== 'running' || !setWorkActive)
      throw failure('unavailable', 'The service cannot accept background work');
    if (activeJobs === 0) setWorkActive(true);
    activeJobs++;
    let released = false;
    return () => {
      if (released) return;
      released = true;
      if (--activeJobs === 0 && phase === 'running') setWorkActive(false);
    };
  };
  const operations = new Set<{ cancel: () => void; done: Promise<void> }>();
  let cleanupSignal: ServiceCancellationSignal | undefined;
  const io: ServiceIoBridge = async (name, parameters, body, signal) => {
    if (
      signal.aborted ||
      phase === 'failed' ||
      phase === 'stopped' ||
      (source.signal.aborted && (!cleanupSignal || signal !== cleanupSignal))
    )
      throw failure('cancelled', 'Service I/O was cancelled');
    if (!bridge.io)
      throw failure('permission-denied', 'Service I/O is unavailable');
    if (body.byteLength > 1024 * 1024)
      throw failure('size-limit', 'Service I/O body exceeds 1 MiB');
    if (operations.size >= 64)
      throw failure('resource-exhausted', 'Too many service I/O operations');
    const operation = cancellation();
    const drained = deferred<void>();
    const pending = { cancel: operation.abort, done: drained.promise };
    operations.add(pending);
    try {
      signal.addEventListener('abort', operation.abort, { once: true });
      return await bridge.io(name, parameters, body, operation.signal);
    } finally {
      signal.removeEventListener('abort', operation.abort);
      operation.clear();
      operations.delete(pending);
      drained.resolve();
    }
  };
  const drainIo = async () => {
    const pending = [...operations];
    for (const operation of pending) operation.cancel();
    await Promise.all(pending.map((operation) => operation.done));
  };
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
        if (!startReasons.includes(input.startReason))
          throw new TypeError('Invalid service start reason');
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
          startReason: input.startReason,
          dataDirectory: text(input.dataDirectory),
          config,
          signal: source.signal,
          ...(setWorkActive ? { retainWork } : {}),
          android: Object.freeze({
            setForegroundServiceTypes: async (
              types: readonly AndroidForegroundServiceType[]
            ) => {
              await io(
                'android.setForegroundServiceTypes',
                { types: [...types] },
                new Uint8Array(),
                source.signal
              );
            },
          }),
          server: createServiceServer(io, source.signal),
          http: createServiceHttp(io, source.signal),
          fs: createServiceFiles(io, source.signal),
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
        if (phase === 'starting')
          phase = source.signal.aborted ? 'stopping' : 'running';
        started.resolve();
      } catch (error) {
        phase = 'failed';
        source.abort();
        await drainIo();
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
  const stop: ServiceRuntime['stop'] = (request) => {
    if (shutdown) return shutdown;
    if (
      !request ||
      !stopReasons.includes(request.reason) ||
      (request.remainingMs !== undefined &&
        (!Number.isSafeInteger(request.remainingMs) ||
          request.remainingMs < 0 ||
          request.remainingMs > 2147483647))
    )
      return Promise.reject(new TypeError('Invalid service stop request'));
    const { reason, detail, remainingMs } = request;
    const deadline =
      remainingMs === undefined ? undefined : Date.now() + remainingMs;
    const cleanup = cancellation();
    const stopped = deferred<void>();
    shutdown = stopped.promise;
    let outcome: { error: unknown } | undefined;
    let timer: ReturnType<typeof setTimeout> | undefined;
    if (remainingMs !== undefined) {
      try {
        timer = setTimeout(cleanup.abort, remainingMs);
      } catch {
        // QuickJS imposes a per-instance timer quota. Cleanup still runs once.
        cleanup.abort();
      }
    }
    phase = 'stopping';
    source.abort();
    for (const operation of operations) operation.cancel();
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
        await drainIo();
        if (instance && typeof instance.stop === 'function') {
          const left =
            deadline === undefined
              ? undefined
              : Math.max(0, Math.min(remainingMs!, deadline - Date.now()));
          if (left === 0) cleanup.abort();
          cleanupSignal = cleanup.signal;
          await instance.stop(
            Object.freeze({
              reason,
              signal: cleanup.signal,
              ...(detail === undefined ? {} : { detail }),
              ...(left === undefined ? {} : { remainingMs: left }),
            })
          );
        }
        instance = undefined;
        phase = 'stopped';
        source.clear();
      } catch (error) {
        phase = 'failed';
        outcome = { error };
      } finally {
        cleanupSignal = undefined;
        if (timer !== undefined) clearTimeout(timer);
        cleanup.abort();
        await drainIo();
      }
      if (outcome) stopped.reject(outcome.error);
      else stopped.resolve();
    };
    void run();
    return shutdown;
  };
  const cancelStart = () => {
    if (phase === 'starting' || phase === 'new') {
      source.abort();
      for (const operation of operations) operation.cancel();
    }
  };
  return Object.freeze({ start, invoke, cancel, cancelStart, stop });
};
