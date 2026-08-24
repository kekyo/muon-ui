// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

/** Receives one message from the Android JavaScript runtime host. */
export type MuonAndroidJavaScriptRuntimeMessageHandler = (
  message: string
) => Promise<void>;

/** Supplies the WebView-to-Service channel used by Android JavaScript runtimes. */
export interface MuonAndroidJavaScriptRuntimeTransport {
  /** Sends one UTF-8 JSON message to the Android host. */
  readonly send: (message: string) => void;

  /** Replaces or removes the host-message handler. */
  readonly setMessageHandler: (
    handler: MuonAndroidJavaScriptRuntimeMessageHandler | undefined
  ) => void;
}

/** Event delivered by the object installed with `addWebMessageListener`. */
export interface MuonAndroidJavaScriptBridgeMessageEvent {
  /** Contains one UTF-8 JSON bridge message. */
  readonly data: string;
}

/** JavaScript object injected by the Android JavaScript runtime host. */
export interface MuonAndroidJavaScriptBridge {
  /** Sends one UTF-8 JSON bridge message. */
  readonly postMessage: (message: string) => void;

  /** Receives messages posted by the Android host. */
  onmessage:
    | ((event: MuonAndroidJavaScriptBridgeMessageEvent) => Promise<void>)
    | null;
}

/** A module imported into one Android JavaScript runtime. */
export type MuonAndroidJavaScriptModule = Readonly<
  Record<string, any> & {
    /** Releases the remote module handle. */
    readonly $release: () => Promise<void>;
  }
>;

/** One independently isolated Android JavaScript runtime. */
export interface MuonAndroidJavaScriptRuntimeInstance {
  /** Imports a packaged ES module or supported host module. */
  readonly importModule: (
    specifier: string
  ) => Promise<MuonAndroidJavaScriptModule>;

  /** Releases this runtime and every module imported through it. */
  readonly release: () => Promise<void>;
}

/** Creates and owns Android JavaScript runtime instances. */
export interface MuonAndroidJavaScriptRuntimeClient {
  /** Creates an independent runtime in the private Android Service process. */
  readonly createNode: () => Promise<MuonAndroidJavaScriptRuntimeInstance>;

  /** Releases local bridge state and rejects all outstanding operations. */
  readonly dispose: () => void;

  /** Returns the number of live logical runtimes. */
  readonly getRuntimeCount: () => number;
}

interface PendingOperation<T> {
  readonly resolve: (value: T) => void;
  readonly reject: (reason: Error) => void;
}

interface RuntimeRequestOperation extends PendingOperation<unknown> {
  readonly callbackHandles: readonly string[];
}

interface RuntimeState {
  readonly id: string;
  readonly pending: Map<string, RuntimeRequestOperation>;
  readonly callbacks: Map<
    string,
    (...arguments_: readonly unknown[]) => unknown
  >;
  readonly modules: Set<ModuleState>;
  status: 'active' | 'releasing' | 'released' | 'failed';
  failure: Error | undefined;
  releaseOperation: Promise<void> | undefined;
}

interface ModuleState {
  readonly runtime: RuntimeState;
  readonly moduleId: string;
  released: boolean;
  releaseOperation: Promise<void> | undefined;
}

interface ExportDescriptor {
  readonly name: string;
  readonly kind: 'function' | 'primitive';
  readonly value: unknown;
}

interface EncodedArgument {
  readonly value: unknown;
  readonly callbackHandle: string | undefined;
}

interface WireError extends Error {
  code: string;
}

const protocolVersion = 1;
const signed64Minimum = -(1n << 63n);
const signed64Maximum = (1n << 63n) - 1n;
const unsigned64Maximum = (1n << 64n) - 1n;
const reservedModuleExportNames = new Set(['$release', 'then']);

const isRecord = (value: unknown): value is Record<string, unknown> =>
  typeof value === 'object' && value !== null && !Array.isArray(value);

const createWireError = (code: string, message: string): WireError => {
  const error = new Error(message) as WireError;
  error.code = code;
  return error;
};

const requireString = (value: unknown, description: string): string => {
  if (typeof value !== 'string' || value.length === 0) {
    throw createWireError(
      'ERR_MUON_JS_PROTOCOL',
      `${description} must be a non-empty string`
    );
  }
  return value;
};

const requirePositiveInteger = (
  value: unknown,
  description: string
): number => {
  if (typeof value !== 'number' || !Number.isSafeInteger(value) || value <= 0) {
    throw createWireError(
      'ERR_MUON_JS_PROTOCOL',
      `${description} must be a positive integer`
    );
  }
  return value;
};

const getBinaryBytes = (value: unknown): Uint8Array | undefined => {
  if (value instanceof ArrayBuffer) {
    return new Uint8Array(value.slice(0));
  }
  if (ArrayBuffer.isView(value)) {
    const source = new Uint8Array(
      value.buffer,
      value.byteOffset,
      value.byteLength
    );
    return Uint8Array.from(source);
  }
  return undefined;
};

const encodeBase64 = (value: Uint8Array): string => {
  let encoded = '';
  // Every non-final chunk is divisible by three, so independently encoded
  // chunks can be concatenated without introducing base64 padding mid-stream.
  const chunkLength = 0x7ffe;
  for (let offset = 0; offset < value.length; offset += chunkLength) {
    const chunk = value.subarray(offset, offset + chunkLength);
    let binary = '';
    for (const byte of chunk) {
      binary += String.fromCharCode(byte);
    }
    encoded += btoa(binary);
  }
  return encoded;
};

const decodeBase64 = (value: unknown): Uint8Array => {
  if (typeof value !== 'string') {
    throw createWireError(
      'ERR_MUON_JS_UNSUPPORTED_VALUE',
      'buffer values must contain base64 data'
    );
  }
  let binary: string;
  try {
    binary = atob(value);
  } catch {
    throw createWireError(
      'ERR_MUON_JS_UNSUPPORTED_VALUE',
      'buffer contains invalid base64 data'
    );
  }
  const result = new Uint8Array(binary.length);
  for (let index = 0; index < binary.length; index += 1) {
    result[index] = binary.charCodeAt(index);
  }
  if (encodeBase64(result) !== value) {
    throw createWireError(
      'ERR_MUON_JS_UNSUPPORTED_VALUE',
      'buffer contains non-canonical base64 data'
    );
  }
  return result;
};

const normalizeStrictJson = (
  value: unknown,
  ancestors: Set<object>
): unknown => {
  if (
    value === null ||
    typeof value === 'boolean' ||
    typeof value === 'string'
  ) {
    return value;
  }
  if (typeof value === 'number') {
    if (!Number.isFinite(value) || Object.is(value, -0)) {
      throw createWireError(
        'ERR_MUON_JS_UNSUPPORTED_VALUE',
        'JSON containers support only finite numbers other than negative zero'
      );
    }
    return value;
  }
  if (typeof value !== 'object') {
    throw createWireError(
      'ERR_MUON_JS_UNSUPPORTED_VALUE',
      'JSON containers can contain only JSON-compatible values'
    );
  }
  if (ancestors.has(value)) {
    throw createWireError(
      'ERR_MUON_JS_UNSUPPORTED_VALUE',
      'JSON containers cannot contain cycles'
    );
  }

  ancestors.add(value);
  try {
    if (Array.isArray(value)) {
      if (Object.getPrototypeOf(value) !== Array.prototype) {
        throw createWireError(
          'ERR_MUON_JS_UNSUPPORTED_VALUE',
          'JSON arrays must use Array.prototype'
        );
      }
      const result: unknown[] = [];
      for (let index = 0; index < value.length; index += 1) {
        if (!Object.prototype.hasOwnProperty.call(value, index)) {
          throw createWireError(
            'ERR_MUON_JS_UNSUPPORTED_VALUE',
            'JSON arrays must be dense'
          );
        }
        result.push(normalizeStrictJson(value[index], ancestors));
      }
      if (Reflect.ownKeys(value).length !== value.length + 1) {
        throw createWireError(
          'ERR_MUON_JS_UNSUPPORTED_VALUE',
          'JSON arrays cannot contain additional properties'
        );
      }
      return result;
    }

    const prototype = Object.getPrototypeOf(value);
    if (prototype !== Object.prototype && prototype !== null) {
      throw createWireError(
        'ERR_MUON_JS_UNSUPPORTED_VALUE',
        'JSON objects must use Object.prototype or a null prototype'
      );
    }
    const result: Record<string, unknown> = {};
    for (const key of Reflect.ownKeys(value)) {
      if (typeof key !== 'string') {
        throw createWireError(
          'ERR_MUON_JS_UNSUPPORTED_VALUE',
          'JSON objects cannot contain symbol properties'
        );
      }
      const descriptor = Object.getOwnPropertyDescriptor(value, key);
      if (
        descriptor === undefined ||
        !descriptor.enumerable ||
        !Object.prototype.hasOwnProperty.call(descriptor, 'value')
      ) {
        throw createWireError(
          'ERR_MUON_JS_UNSUPPORTED_VALUE',
          `JSON object property ${JSON.stringify(key)} must be enumerable data`
        );
      }
      result[key] = normalizeStrictJson(descriptor.value, ancestors);
    }
    return result;
  } finally {
    ancestors.delete(value);
  }
};

const encodeScalarWireValue = (value: unknown): unknown => {
  if (value === undefined) {
    return { kind: 'undefined' };
  }
  if (
    value === null ||
    typeof value === 'boolean' ||
    typeof value === 'string'
  ) {
    return value;
  }
  if (typeof value === 'number') {
    if (!Number.isFinite(value) || Object.is(value, -0)) {
      throw createWireError(
        'ERR_MUON_JS_UNSUPPORTED_VALUE',
        'Only finite numbers other than negative zero can cross the bridge'
      );
    }
    return value;
  }
  if (typeof value === 'bigint') {
    if (value >= signed64Minimum && value <= signed64Maximum) {
      return { kind: 'i64', value: value.toString() };
    }
    if (value >= 0n && value <= unsigned64Maximum) {
      return { kind: 'u64', value: value.toString() };
    }
    throw createWireError(
      'ERR_MUON_JS_UNSUPPORTED_VALUE',
      'The bigint value is outside the i64/u64 range'
    );
  }
  const binary = getBinaryBytes(value);
  if (binary !== undefined) {
    return { kind: 'buffer', data: encodeBase64(binary) };
  }
  throw createWireError(
    'ERR_MUON_JS_UNSUPPORTED_VALUE',
    'Only primitive, i64, u64, and buffer values can cross the bridge'
  );
};

const encodeWireValue = (value: unknown): unknown => {
  if (value !== null && typeof value === 'object') {
    const binary = getBinaryBytes(value);
    if (binary === undefined) {
      return {
        kind: 'json',
        value: normalizeStrictJson(value, new Set<object>()),
      };
    }
  }
  return encodeScalarWireValue(value);
};

const decodeInteger = (value: Record<string, unknown>): bigint => {
  const kind = value.kind;
  const decimal = value.value;
  if (
    (kind !== 'i64' && kind !== 'u64') ||
    typeof decimal !== 'string' ||
    !/^(?:0|-?[1-9][0-9]*)$/.test(decimal)
  ) {
    throw createWireError(
      'ERR_MUON_JS_UNSUPPORTED_VALUE',
      'Integer tags must contain a canonical decimal string'
    );
  }
  const decoded = BigInt(decimal);
  const inRange =
    kind === 'i64'
      ? decoded >= signed64Minimum && decoded <= signed64Maximum
      : decoded >= 0n && decoded <= unsigned64Maximum;
  if (!inRange) {
    throw createWireError(
      'ERR_MUON_JS_UNSUPPORTED_VALUE',
      `${kind} is outside its supported range`
    );
  }
  return decoded;
};

const decodeWireValue = (value: unknown): unknown => {
  if (
    value === null ||
    typeof value === 'boolean' ||
    typeof value === 'string'
  ) {
    return value;
  }
  if (typeof value === 'number') {
    if (!Number.isFinite(value) || Object.is(value, -0)) {
      throw createWireError(
        'ERR_MUON_JS_UNSUPPORTED_VALUE',
        'Only finite numbers other than negative zero can cross the bridge'
      );
    }
    return value;
  }
  if (!isRecord(value)) {
    throw createWireError(
      'ERR_MUON_JS_UNSUPPORTED_VALUE',
      'Wire values must be tagged objects or supported primitives'
    );
  }
  if (value.kind === 'undefined') {
    return undefined;
  }
  if (value.kind === 'i64' || value.kind === 'u64') {
    return decodeInteger(value);
  }
  if (value.kind === 'buffer') {
    return decodeBase64(value.data);
  }
  if (value.kind === 'json') {
    return normalizeStrictJson(value.value, new Set<object>());
  }
  throw createWireError(
    'ERR_MUON_JS_UNSUPPORTED_VALUE',
    'The runtime returned an unknown wire value'
  );
};

const parseExportDescriptors = (
  value: unknown
): readonly ExportDescriptor[] => {
  if (!isRecord(value) || !Array.isArray(value.exports)) {
    throw createWireError(
      'ERR_MUON_JS_PROTOCOL',
      'The runtime returned an invalid module descriptor'
    );
  }
  const names = new Set<string>();
  return value.exports.map((entry) => {
    if (!isRecord(entry)) {
      throw createWireError(
        'ERR_MUON_JS_PROTOCOL',
        'The runtime returned an invalid export descriptor'
      );
    }
    const name = requireString(entry.name, 'Module export name');
    if (reservedModuleExportNames.has(name) || names.has(name)) {
      throw createWireError(
        'ERR_MUON_JS_PROTOCOL',
        `The runtime returned reserved or duplicate export ${name}`
      );
    }
    names.add(name);
    if (entry.kind !== 'function' && entry.kind !== 'primitive') {
      throw createWireError(
        'ERR_MUON_JS_PROTOCOL',
        `The runtime returned an unknown export kind for ${name}`
      );
    }
    return {
      name,
      kind: entry.kind,
      value: entry.value,
    };
  });
};

/** Creates a transport from the object injected by AndroidX WebKit. */
export const createMuonAndroidJavaScriptRuntimeTransport = (
  bridge: MuonAndroidJavaScriptBridge
): MuonAndroidJavaScriptRuntimeTransport => {
  let handler: MuonAndroidJavaScriptRuntimeMessageHandler | undefined;
  bridge.onmessage = async (event) => {
    const current = handler;
    if (current !== undefined) {
      await current(event.data);
    }
  };
  return {
    send: (message) => bridge.postMessage(message),
    setMessageHandler: (value) => {
      handler = value;
      if (value === undefined) {
        bridge.onmessage = null;
      }
    },
  };
};

/**
 * Creates the renderer-side facade for QuickJS runtimes hosted by Android.
 *
 * @param transport - Trusted WebView channel connected to the private Service.
 * @returns A client exposing the existing `createNode()` interaction model.
 */
export const createMuonAndroidJavaScriptRuntimeClient = (
  transport: MuonAndroidJavaScriptRuntimeTransport
): MuonAndroidJavaScriptRuntimeClient => {
  const pendingCreates = new Map<
    number,
    PendingOperation<MuonAndroidJavaScriptRuntimeInstance>
  >();
  const runtimes = new Map<string, RuntimeState>();
  let nextCreateRequestId = 1;
  let nextRuntimeRequestId = 1;
  let nextCallbackHandle = 1;
  let disposed = false;

  const send = (message: Readonly<Record<string, unknown>>): void => {
    if (disposed) {
      throw new Error('Android JavaScript runtime client was disposed');
    }
    transport.send(JSON.stringify({ version: protocolVersion, ...message }));
  };

  const getRuntimeFailure = (runtime: RuntimeState): Error =>
    runtime.failure ??
    new Error(
      runtime.status === 'released'
        ? 'JavaScript runtime has been released'
        : 'JavaScript runtime is being released'
    );

  const rejectRuntime = (runtime: RuntimeState, error: Error): void => {
    if (runtime.status === 'released' || runtime.status === 'failed') {
      return;
    }
    runtime.status = 'failed';
    runtime.failure = error;
    for (const pending of runtime.pending.values()) {
      pending.reject(error);
    }
    runtime.pending.clear();
    runtime.callbacks.clear();
    for (const module of runtime.modules) {
      module.released = true;
    }
    runtime.modules.clear();
    runtimes.delete(runtime.id);
  };

  const encodeArgument = (
    runtime: RuntimeState,
    value: unknown
  ): EncodedArgument => {
    if (typeof value !== 'function') {
      return { value: encodeWireValue(value), callbackHandle: undefined };
    }
    const handle = `callback-${nextCallbackHandle}`;
    nextCallbackHandle += 1;
    runtime.callbacks.set(
      handle,
      value as (...arguments_: readonly unknown[]) => unknown
    );
    return {
      value: { kind: 'function', handle },
      callbackHandle: handle,
    };
  };

  const sendRuntimeRequest = (
    runtime: RuntimeState,
    command: string,
    params: Readonly<Record<string, unknown>>,
    callbackHandles: readonly string[]
  ): Promise<unknown> => {
    if (runtime.status !== 'active') {
      return Promise.reject(getRuntimeFailure(runtime));
    }
    const requestId = `request-${nextRuntimeRequestId}`;
    nextRuntimeRequestId += 1;
    const result = new Promise<unknown>((resolve, reject) => {
      runtime.pending.set(requestId, {
        resolve,
        reject,
        callbackHandles,
      });
    });
    try {
      send({
        type: 'runtimeMessage',
        runtimeId: runtime.id,
        message: {
          kind: 'request',
          id: requestId,
          command,
          params,
        },
      });
    } catch (error) {
      runtime.pending.delete(requestId);
      for (const handle of callbackHandles) {
        runtime.callbacks.delete(handle);
      }
      throw error;
    }
    return result;
  };

  const createModuleFacade = (
    runtime: RuntimeState,
    value: unknown
  ): MuonAndroidJavaScriptModule => {
    if (!isRecord(value)) {
      throw createWireError(
        'ERR_MUON_JS_PROTOCOL',
        'The runtime returned an invalid imported module'
      );
    }
    const moduleId = requireString(value.moduleId, 'Module id');
    const descriptors = parseExportDescriptors(value.descriptor);
    const state: ModuleState = {
      runtime,
      moduleId,
      released: false,
      releaseOperation: undefined,
    };
    runtime.modules.add(state);
    const facade: Record<string, any> = Object.create(null) as Record<
      string,
      any
    >;

    for (const descriptor of descriptors) {
      if (descriptor.kind === 'primitive') {
        Object.defineProperty(facade, descriptor.name, {
          configurable: false,
          enumerable: true,
          writable: false,
          value: decodeWireValue(descriptor.value),
        });
        continue;
      }
      Object.defineProperty(facade, descriptor.name, {
        configurable: false,
        enumerable: true,
        writable: false,
        value: async (...arguments_: readonly unknown[]) => {
          if (state.released || state.releaseOperation !== undefined) {
            throw new Error('JavaScript module handle has been released');
          }
          if (runtime.status !== 'active') {
            throw getRuntimeFailure(runtime);
          }
          const encoded = arguments_.map((argument) =>
            encodeArgument(runtime, argument)
          );
          const handles = encoded
            .map((argument) => argument.callbackHandle)
            .filter((handle): handle is string => handle !== undefined);
          try {
            const result = await sendRuntimeRequest(
              runtime,
              'call',
              {
                moduleId,
                exportName: descriptor.name,
                arguments: encoded.map((argument) => argument.value),
              },
              handles
            );
            return decodeWireValue(result);
          } finally {
            for (const handle of handles) {
              runtime.callbacks.delete(handle);
            }
          }
        },
      });
    }

    const release = async (): Promise<void> => {
      if (state.released) {
        return;
      }
      if (runtime.status !== 'active') {
        state.released = true;
        runtime.modules.delete(state);
        return;
      }
      if (state.releaseOperation === undefined) {
        state.releaseOperation = (async () => {
          await sendRuntimeRequest(
            runtime,
            'release',
            { kind: 'module', handle: moduleId },
            []
          );
          state.released = true;
          runtime.modules.delete(state);
        })();
      }
      await state.releaseOperation;
    };
    Object.defineProperty(facade, '$release', {
      configurable: false,
      enumerable: false,
      writable: false,
      value: release,
    });
    return Object.freeze(facade) as MuonAndroidJavaScriptModule;
  };

  const createRuntimeFacade = (
    runtime: RuntimeState
  ): MuonAndroidJavaScriptRuntimeInstance => {
    const release = async (): Promise<void> => {
      if (runtime.status === 'released') {
        return;
      }
      if (runtime.status === 'failed') {
        throw getRuntimeFailure(runtime);
      }
      if (runtime.releaseOperation === undefined) {
        runtime.status = 'releasing';
        runtime.releaseOperation = (async () => {
          const requestId = `request-${nextRuntimeRequestId}`;
          nextRuntimeRequestId += 1;
          const result = new Promise<unknown>((resolve, reject) => {
            runtime.pending.set(requestId, {
              resolve,
              reject,
              callbackHandles: [],
            });
          });
          try {
            send({
              type: 'runtimeMessage',
              runtimeId: runtime.id,
              message: {
                kind: 'request',
                id: requestId,
                command: 'shutdown',
                params: {},
              },
            });
            await result;
            runtime.status = 'released';
            for (const module of runtime.modules) {
              module.released = true;
            }
            runtime.modules.clear();
            runtimes.delete(runtime.id);
          } catch (error) {
            runtime.status = 'failed';
            runtime.failure =
              error instanceof Error ? error : new Error(String(error));
            runtimes.delete(runtime.id);
            throw error;
          }
        })();
      }
      await runtime.releaseOperation;
    };
    const facade: MuonAndroidJavaScriptRuntimeInstance = {
      importModule: async (specifier) => {
        if (typeof specifier !== 'string' || specifier.length === 0) {
          throw new TypeError(
            'JavaScript module specifier must be a non-empty string'
          );
        }
        const imported = await sendRuntimeRequest(
          runtime,
          'importModule',
          { specifier },
          []
        );
        return createModuleFacade(runtime, imported);
      },
      release,
    };
    if (typeof Symbol.asyncDispose === 'symbol') {
      Object.defineProperty(facade, Symbol.asyncDispose, {
        configurable: false,
        enumerable: false,
        writable: false,
        value: release,
      });
    }
    return Object.freeze(facade);
  };

  const handleCallback = async (
    runtime: RuntimeState,
    message: Record<string, unknown>
  ): Promise<void> => {
    const callbackId = requireString(message.id, 'Callback id');
    const handle = requireString(message.handle, 'Callback handle');
    const callback = runtime.callbacks.get(handle);
    if (callback === undefined) {
      send({
        type: 'runtimeMessage',
        runtimeId: runtime.id,
        message: {
          kind: 'callbackResult',
          id: callbackId,
          ok: false,
          value: null,
          error: {
            code: 'ERR_MUON_JS_UNKNOWN_CALLBACK',
            message: 'JavaScript callback handle has been released',
          },
        },
      });
      return;
    }
    if (!Array.isArray(message.arguments)) {
      throw createWireError(
        'ERR_MUON_JS_PROTOCOL',
        'Callback arguments must be an array'
      );
    }
    try {
      const value = await callback(...message.arguments.map(decodeWireValue));
      send({
        type: 'runtimeMessage',
        runtimeId: runtime.id,
        message: {
          kind: 'callbackResult',
          id: callbackId,
          ok: true,
          value: encodeWireValue(value),
          error: null,
        },
      });
    } catch (error) {
      const wireError =
        error instanceof Error ? error : new Error(String(error));
      send({
        type: 'runtimeMessage',
        runtimeId: runtime.id,
        message: {
          kind: 'callbackResult',
          id: callbackId,
          ok: false,
          value: null,
          error: {
            code:
              'code' in wireError && typeof wireError.code === 'string'
                ? wireError.code
                : 'ERR_MUON_JS_CALLBACK',
            message: wireError.message,
          },
        },
      });
    }
  };

  const handleRuntimeMessage = async (
    runtime: RuntimeState,
    value: unknown
  ): Promise<void> => {
    if (!isRecord(value)) {
      throw createWireError(
        'ERR_MUON_JS_PROTOCOL',
        'Runtime message must be an object'
      );
    }
    if (value.kind === 'callback') {
      await handleCallback(runtime, value);
      return;
    }
    if (value.kind !== 'response') {
      throw createWireError(
        'ERR_MUON_JS_PROTOCOL',
        'Runtime message kind is unsupported'
      );
    }
    const id = requireString(value.id, 'Response id');
    const pending = runtime.pending.get(id);
    if (pending === undefined) {
      return;
    }
    runtime.pending.delete(id);
    for (const handle of pending.callbackHandles) {
      runtime.callbacks.delete(handle);
    }
    if (value.ok === true) {
      pending.resolve(value.value);
      return;
    }
    const errorValue = value.error;
    if (!isRecord(errorValue)) {
      pending.reject(
        createWireError(
          'ERR_MUON_JS_PROTOCOL',
          'Failed runtime response omitted its error'
        )
      );
      return;
    }
    pending.reject(
      createWireError(
        requireString(errorValue.code, 'Runtime error code'),
        requireString(errorValue.message, 'Runtime error message')
      )
    );
  };

  const handleMessage = async (source: string): Promise<void> => {
    let value: unknown;
    try {
      value = JSON.parse(source) as unknown;
    } catch {
      return;
    }
    if (!isRecord(value) || value.version !== protocolVersion) {
      return;
    }
    if (value.type === 'created') {
      const requestId = requirePositiveInteger(
        value.requestId,
        'Create request id'
      );
      const pending = pendingCreates.get(requestId);
      if (pending === undefined) {
        return;
      }
      pendingCreates.delete(requestId);
      const runtimeId = requireString(value.runtimeId, 'Runtime id');
      if (runtimes.has(runtimeId)) {
        pending.reject(
          createWireError(
            'ERR_MUON_JS_PROTOCOL',
            `Duplicate runtime id ${runtimeId}`
          )
        );
        return;
      }
      const engine = value.engine;
      if (
        !isRecord(engine) ||
        typeof engine.name !== 'string' ||
        typeof engine.version !== 'string'
      ) {
        pending.reject(
          createWireError(
            'ERR_MUON_JS_PROTOCOL',
            'Created runtime omitted engine identity'
          )
        );
        return;
      }
      const runtime: RuntimeState = {
        id: runtimeId,
        pending: new Map(),
        callbacks: new Map(),
        modules: new Set(),
        status: 'active',
        failure: undefined,
        releaseOperation: undefined,
      };
      runtimes.set(runtimeId, runtime);
      pending.resolve(createRuntimeFacade(runtime));
      return;
    }
    if (value.type === 'createFailed') {
      const requestId = requirePositiveInteger(
        value.requestId,
        'Create request id'
      );
      const pending = pendingCreates.get(requestId);
      if (pending !== undefined) {
        pendingCreates.delete(requestId);
        pending.reject(
          new Error(
            typeof value.error === 'string'
              ? value.error
              : 'Android JavaScript runtime creation failed'
          )
        );
      }
      return;
    }
    const runtimeId = requireString(value.runtimeId, 'Runtime id');
    const runtime = runtimes.get(runtimeId);
    if (runtime === undefined) {
      return;
    }
    if (value.type === 'runtimeMessage') {
      await handleRuntimeMessage(runtime, value.message);
      return;
    }
    if (value.type === 'runtimeClosed') {
      rejectRuntime(
        runtime,
        new Error(
          typeof value.error === 'string'
            ? value.error
            : 'Android JavaScript runtime disconnected'
        )
      );
    }
  };

  transport.setMessageHandler(handleMessage);

  return {
    createNode: () => {
      if (disposed) {
        return Promise.reject(
          new Error('Android JavaScript runtime client was disposed')
        );
      }
      const requestId = nextCreateRequestId;
      nextCreateRequestId += 1;
      const result = new Promise<MuonAndroidJavaScriptRuntimeInstance>(
        (resolve, reject) => {
          pendingCreates.set(requestId, { resolve, reject });
        }
      );
      send({ type: 'create', requestId });
      return result;
    },
    dispose: () => {
      if (disposed) {
        return;
      }
      const error = new Error('Android JavaScript runtime client was disposed');
      for (const pending of pendingCreates.values()) {
        pending.reject(error);
      }
      pendingCreates.clear();
      for (const runtime of runtimes.values()) {
        rejectRuntime(runtime, error);
      }
      runtimes.clear();
      transport.send(
        JSON.stringify({ version: protocolVersion, type: 'dispose' })
      );
      transport.setMessageHandler(undefined);
      disposed = true;
    },
    getRuntimeCount: () => runtimes.size,
  };
};
