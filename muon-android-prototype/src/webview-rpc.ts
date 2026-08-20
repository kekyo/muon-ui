// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import type {
  MuonAndroidRendererMetadata,
  MuonNativeFunctionMetadata,
  MuonNativeTypeMetadata,
} from './native-plugin-metadata.js';

/** A message exchanged between JavaScript and the Android WebView host. */
export type MuonWebViewRpcMessage = string | ArrayBuffer;

/** Receives a message from the Android WebView host. */
export type MuonWebViewRpcMessageHandler = (
  message: MuonWebViewRpcMessage
) => void;

/** Supplies the platform-specific message channel used by the RPC client. */
export interface MuonWebViewRpcTransport {
  /** Sends one text or binary message to the Android WebView host. */
  readonly send: (message: MuonWebViewRpcMessage) => void;

  /** Replaces or removes the handler for messages received from the host. */
  readonly setMessageHandler: (
    handler: MuonWebViewRpcMessageHandler | undefined
  ) => void;
}

/** Options controlling one WebView RPC call. */
export interface MuonWebViewRpcCallOptions {
  /** Cancels the native RPC call when aborted. */
  readonly signal?: AbortSignal;
}

/** The kind of payload represented by a WebView RPC binary frame. */
export type MuonWebViewRpcBinaryFrameKind = 'argument' | 'result';

/** Values required to encode a WebView RPC binary frame. */
export interface MuonWebViewRpcBinaryFrame {
  /** Identifies whether the payload is a call argument or a call result. */
  readonly kind: MuonWebViewRpcBinaryFrameKind;

  /** Identifies the RPC call owning the payload. */
  readonly callId: number;

  /** Identifies this payload within the owning message. */
  readonly attachment: number;

  /** Contains the payload bytes. */
  readonly payload: ArrayBuffer;
}

/** Provides asynchronous calls over a WebView RPC transport. */
export interface MuonWebViewRpcClient {
  /**
   * Calls one native capability function.
   *
   * @param capabilityId - Capability token supplied by the generated module.
   * @param functionPath - Fully-qualified native function path.
   * @param arguments_ - Function arguments.
   * @param options - Optional cancellation settings.
   * @returns The native result.
   */
  readonly call: (
    capabilityId: string,
    functionPath: string,
    arguments_: readonly unknown[],
    options?: MuonWebViewRpcCallOptions
  ) => Promise<unknown>;

  /** Releases the JavaScript context and rejects all outstanding calls. */
  readonly dispose: () => void;

  /** Returns the number of calls awaiting a native result. */
  readonly getPendingCallCount: () => number;
}

/** The message event emitted by an object injected with addWebMessageListener. */
export interface MuonWebViewJavaScriptMessageEvent {
  /** Contains text or binary data sent by the Android host. */
  readonly data: MuonWebViewRpcMessage;
}

/** The JavaScript object injected by AndroidX WebKit. */
export interface MuonWebViewJavaScriptBridge {
  /** Sends text or binary data to the Android host. */
  readonly postMessage: (message: MuonWebViewRpcMessage) => void;

  /** Receives replies posted by the Android host. */
  onmessage: ((event: MuonWebViewJavaScriptMessageEvent) => void) | null;
}

interface PendingCall {
  readonly resolve: (value: unknown) => void;
  readonly reject: (reason: unknown) => void;
  readonly signal: AbortSignal | undefined;
  readonly returnType: MuonNativeTypeMetadata | undefined;
  abortListener: (() => void) | undefined;
}

interface EncodedArguments {
  readonly value: readonly unknown[];
  readonly attachments: readonly ArrayBuffer[];
}

const binaryHeaderLength = 16;
const binaryMagic = [0x4d, 0x52, 0x50, 0x43] as const;
const protocolVersion = 1;
const maximumCallId = 0x7fffffff;

const isRecord = (value: unknown): value is Record<string, unknown> =>
  typeof value === 'object' && value !== null && !Array.isArray(value);

const isUnsignedInteger = (value: unknown): value is number =>
  typeof value === 'number' &&
  Number.isInteger(value) &&
  value >= 0 &&
  value <= 0xffffffff;

const copyArrayBufferView = (value: ArrayBufferView): ArrayBuffer => {
  const copy = new Uint8Array(value.byteLength);
  copy.set(new Uint8Array(value.buffer, value.byteOffset, value.byteLength));
  return copy.buffer;
};

const encodeArgumentValue = (
  value: unknown,
  attachments: ArrayBuffer[],
  ancestors: Set<object>
): unknown => {
  let payload: ArrayBuffer | undefined;
  if (value instanceof ArrayBuffer) {
    payload = value.slice(0);
  } else if (ArrayBuffer.isView(value)) {
    payload = copyArrayBufferView(value);
  }

  if (payload !== undefined) {
    const attachment = attachments.length;
    attachments.push(payload);
    return {
      type: 'binary',
      attachment,
      byteLength: payload.byteLength,
    };
  }

  if (Array.isArray(value)) {
    if (ancestors.has(value)) {
      throw new TypeError('WebView RPC arguments cannot contain cycles');
    }
    ancestors.add(value);
    const encoded = value.map((element) =>
      encodeArgumentValue(element, attachments, ancestors)
    );
    ancestors.delete(value);
    return encoded;
  }

  if (isRecord(value)) {
    if (ancestors.has(value)) {
      throw new TypeError('WebView RPC arguments cannot contain cycles');
    }
    ancestors.add(value);
    const encoded: Record<string, unknown> = {};
    for (const [key, element] of Object.entries(value)) {
      encoded[key] = encodeArgumentValue(element, attachments, ancestors);
    }
    ancestors.delete(value);
    return encoded;
  }

  return value;
};

const encodeArguments = (arguments_: readonly unknown[]): EncodedArguments => {
  const attachments: ArrayBuffer[] = [];
  const ancestors = new Set<object>();
  const value = arguments_.map((argument) =>
    encodeArgumentValue(argument, attachments, ancestors)
  );
  return { value, attachments };
};

const encodeInt64Argument = (value: unknown, unsigned: boolean): string => {
  if (typeof value !== 'number') {
    throw new TypeError(`expected ${unsigned ? 'u64' : 'i64'}`);
  }
  const truncated = Number.isFinite(value) ? Math.trunc(value) : 0;
  const bits = BigInt.asUintN(64, BigInt(truncated));
  return unsigned ? bits.toString() : BigInt.asIntN(64, bits).toString();
};

const encodeNumberArgument = (
  value: unknown,
  type: MuonNativeTypeMetadata['type']
): number => {
  if (typeof value !== 'number' || !Number.isFinite(value)) {
    throw new TypeError(`expected ${type}`);
  }
  const integerRanges: Readonly<
    Partial<Record<MuonNativeTypeMetadata['type'], readonly [number, number]>>
  > = {
    i8: [-128, 127],
    u8: [0, 255],
    i16: [-32768, 32767],
    u16: [0, 65535],
    i32: [-2147483648, 2147483647],
    u32: [0, 4294967295],
  };
  const range = integerRanges[type];
  if (
    range !== undefined &&
    (!Number.isInteger(value) || value < range[0] || value > range[1])
  ) {
    throw new TypeError(`expected ${type}`);
  }
  if (
    type === 'f32' &&
    (value < -3.4028234663852886e38 || value > 3.4028234663852886e38)
  ) {
    throw new TypeError('expected f32');
  }
  return value;
};

const encodeNativeArgumentValue = (
  value: unknown,
  type: MuonNativeTypeMetadata,
  attachments: ArrayBuffer[]
): unknown => {
  switch (type.type) {
    case 'bool':
      if (typeof value !== 'boolean') {
        throw new TypeError('expected bool');
      }
      return value;
    case 'i8':
    case 'u8':
    case 'i16':
    case 'u16':
    case 'i32':
    case 'u32':
    case 'f32':
    case 'f64':
      return encodeNumberArgument(value, type.type);
    case 'i64':
      return encodeInt64Argument(value, false);
    case 'u64':
      return encodeInt64Argument(value, true);
    case 'pointer':
      if (value === null || value === undefined) {
        return null;
      }
      if (
        typeof value !== 'number' ||
        !Number.isFinite(value) ||
        !Number.isInteger(value) ||
        value < 0 ||
        value >= 2 ** 64
      ) {
        throw new TypeError('expected pointer');
      }
      return value;
    case 'string':
      if (value === null || value === undefined) {
        return null;
      }
      if (typeof value !== 'string' || value.includes('\0')) {
        throw new TypeError('expected string');
      }
      return value;
    case 'buffer_view': {
      let payload: ArrayBuffer;
      if (value instanceof ArrayBuffer) {
        payload = value.slice(0);
      } else if (ArrayBuffer.isView(value)) {
        payload = copyArrayBufferView(value);
      } else {
        throw new TypeError('expected buffer_view');
      }
      const attachment = attachments.length;
      attachments.push(payload);
      return { type: 'binary', attachment, byteLength: payload.byteLength };
    }
    case 'function':
      if (value === null || value === undefined) {
        return null;
      }
      throw new TypeError('expected function');
    case 'void':
      throw new TypeError('void arguments are unavailable');
  }
};

const encodeNativeArguments = (
  function_: MuonNativeFunctionMetadata,
  arguments_: readonly unknown[]
): EncodedArguments => {
  if (arguments_.length !== function_.args.length) {
    throw new TypeError(
      `Invalid argument count for ${function_.namespace}.${function_.publicName}`
    );
  }
  const attachments: ArrayBuffer[] = [];
  const value = arguments_.map((argument, index) => {
    try {
      return encodeNativeArgumentValue(
        argument,
        function_.args[index]!,
        attachments
      );
    } catch (error) {
      const diagnostic =
        error instanceof Error ? error.message : 'invalid value';
      throw new TypeError(`Invalid argument ${index}: ${diagnostic}`);
    }
  });
  return { value, attachments };
};

const decodeNativeResult = (
  value: unknown,
  type: MuonNativeTypeMetadata
): unknown => {
  if (type.type === 'void') {
    return undefined;
  }
  if (type.type === 'i64' || type.type === 'u64') {
    if (typeof value !== 'string') {
      throw new TypeError('Android native 64-bit result is invalid');
    }
    const parsed = BigInt(value);
    return Number(
      type.type === 'i64'
        ? BigInt.asIntN(64, parsed)
        : BigInt.asIntN(64, BigInt.asUintN(64, parsed))
    );
  }
  if (type.type === 'string') {
    if (value !== null && typeof value !== 'string') {
      throw new TypeError('Android native string result is invalid');
    }
    return value;
  }
  if (type.type === 'bool') {
    if (typeof value !== 'boolean') {
      throw new TypeError('Android native bool result is invalid');
    }
    return value;
  }
  if (type.type === 'function' || type.type === 'buffer_view') {
    throw new TypeError('Android native result transport is invalid');
  }
  if (typeof value !== 'number' || !Number.isFinite(value)) {
    throw new TypeError('Android native numeric result is invalid');
  }
  return value;
};

const createAbortError = (): DOMException =>
  new DOMException('The operation was aborted', 'AbortError');

/**
 * Encodes one binary attachment into the transport-independent RPC frame.
 *
 * @param frame - Frame metadata and payload.
 * @returns A new buffer containing the frame header and payload.
 */
export const encodeMuonWebViewRpcBinaryFrame = (
  frame: MuonWebViewRpcBinaryFrame
): ArrayBuffer => {
  if (
    !Number.isInteger(frame.callId) ||
    frame.callId <= 0 ||
    frame.callId > maximumCallId
  ) {
    throw new RangeError('WebView RPC callId is out of range');
  }
  if (!isUnsignedInteger(frame.attachment)) {
    throw new RangeError('WebView RPC attachment is out of range');
  }

  const encoded = new ArrayBuffer(
    binaryHeaderLength + frame.payload.byteLength
  );
  const bytes = new Uint8Array(encoded);
  bytes.set(binaryMagic, 0);
  bytes[4] = protocolVersion;
  bytes[5] = frame.kind === 'argument' ? 1 : 2;
  const view = new DataView(encoded);
  view.setUint32(8, frame.callId);
  view.setUint32(12, frame.attachment);
  bytes.set(new Uint8Array(frame.payload), binaryHeaderLength);
  return encoded;
};

/**
 * Decodes and validates one transport-independent RPC binary frame.
 *
 * @param message - Candidate frame received from the WebView channel.
 * @returns The decoded frame, or `undefined` when the message is malformed.
 */
export const decodeMuonWebViewRpcBinaryFrame = (
  message: ArrayBuffer
): MuonWebViewRpcBinaryFrame | undefined => {
  if (message.byteLength < binaryHeaderLength) {
    return undefined;
  }

  const bytes = new Uint8Array(message);
  if (
    bytes[0] !== binaryMagic[0] ||
    bytes[1] !== binaryMagic[1] ||
    bytes[2] !== binaryMagic[2] ||
    bytes[3] !== binaryMagic[3] ||
    bytes[4] !== protocolVersion ||
    (bytes[5] !== 1 && bytes[5] !== 2) ||
    bytes[6] !== 0 ||
    bytes[7] !== 0
  ) {
    return undefined;
  }

  const view = new DataView(message);
  const callId = view.getUint32(8);
  if (callId === 0 || callId > maximumCallId) {
    return undefined;
  }

  return {
    kind: bytes[5] === 1 ? 'argument' : 'result',
    callId,
    attachment: view.getUint32(12),
    payload: message.slice(binaryHeaderLength),
  };
};

/**
 * Adapts an AndroidX WebKit JavaScript object to the RPC transport contract.
 *
 * @param bridge - Object injected by `WebViewCompat.addWebMessageListener`.
 * @returns A transport backed by that object.
 */
export const createMuonWebViewRpcTransport = (
  bridge: MuonWebViewJavaScriptBridge
): MuonWebViewRpcTransport => {
  let handler: MuonWebViewRpcMessageHandler | undefined = undefined;
  return {
    send: (message) => bridge.postMessage(message),
    setMessageHandler: (nextHandler) => {
      handler = nextHandler;
      bridge.onmessage =
        nextHandler === undefined
          ? null
          : (event) => {
              handler?.(event.data);
            };
    },
  };
};

/**
 * Creates a JavaScript RPC client for an Android WebView message channel.
 *
 * @param transport - Platform-specific bidirectional message transport.
 * @returns The RPC client bound to the transport.
 */
export const createMuonWebViewRpcClient = (
  transport: MuonWebViewRpcTransport,
  rendererMetadata?: MuonAndroidRendererMetadata
): MuonWebViewRpcClient => {
  const pendingCalls = new Map<number, PendingCall>();
  const functionsByPath = new Map<string, MuonNativeFunctionMetadata>(
    (rendererMetadata?.functions ?? []).map(
      (function_) =>
        [`${function_.namespace}.${function_.publicName}`, function_] as const
    )
  );
  let nextCallId = 1;
  let disposed = false;

  const detachAbortListener = (pending: PendingCall): void => {
    if (pending.signal !== undefined && pending.abortListener !== undefined) {
      pending.signal.removeEventListener('abort', pending.abortListener);
      pending.abortListener = undefined;
    }
  };

  const takePending = (callId: number): PendingCall | undefined => {
    const pending = pendingCalls.get(callId);
    if (pending === undefined) {
      return undefined;
    }
    pendingCalls.delete(callId);
    detachAbortListener(pending);
    return pending;
  };

  const handleTextMessage = (message: string): void => {
    let parsed: unknown;
    try {
      parsed = JSON.parse(message) as unknown;
    } catch {
      return;
    }
    if (
      !isRecord(parsed) ||
      parsed.version !== protocolVersion ||
      parsed.type !== 'result' ||
      !Number.isInteger(parsed.callId) ||
      typeof parsed.callId !== 'number' ||
      parsed.callId <= 0 ||
      parsed.callId > maximumCallId ||
      typeof parsed.success !== 'boolean'
    ) {
      return;
    }

    const pending = takePending(parsed.callId);
    if (pending === undefined) {
      return;
    }
    if (parsed.success) {
      try {
        if (pending.returnType === undefined) {
          pending.resolve(parsed.value);
        } else {
          if (parsed.valueType !== pending.returnType.type) {
            throw new TypeError('Android native result type is invalid');
          }
          pending.resolve(decodeNativeResult(parsed.value, pending.returnType));
        }
      } catch (error) {
        pending.reject(error);
      }
    } else {
      pending.reject(
        new Error(
          typeof parsed.error === 'string'
            ? parsed.error
            : 'Native WebView RPC call failed'
        )
      );
    }
  };

  const handleBinaryMessage = (message: ArrayBuffer): void => {
    const frame = decodeMuonWebViewRpcBinaryFrame(message);
    if (frame === undefined || frame.kind !== 'result') {
      return;
    }
    const pending = takePending(frame.callId);
    if (pending === undefined) {
      return;
    }
    if (
      pending.returnType !== undefined &&
      pending.returnType.type !== 'buffer_view'
    ) {
      pending.reject(new TypeError('Unexpected Android native binary result'));
      return;
    }
    pending.resolve(frame.payload);
  };

  transport.setMessageHandler((message) => {
    if (disposed) {
      return;
    }
    if (typeof message === 'string') {
      handleTextMessage(message);
    } else {
      handleBinaryMessage(message);
    }
  });

  const call = (
    capabilityId: string,
    functionPath: string,
    arguments_: readonly unknown[],
    options?: MuonWebViewRpcCallOptions
  ): Promise<unknown> => {
    if (disposed) {
      return Promise.reject(new Error('muon WebView RPC client was disposed'));
    }
    if (options?.signal?.aborted === true) {
      return Promise.reject(createAbortError());
    }
    if (nextCallId > maximumCallId) {
      return Promise.reject(new RangeError('WebView RPC callId was exhausted'));
    }

    const callId = nextCallId;
    nextCallId += 1;
    let encodedArguments: EncodedArguments;
    let callMessage: string;
    try {
      const functionMetadata = functionsByPath.get(functionPath);
      encodedArguments =
        functionMetadata === undefined
          ? encodeArguments(arguments_)
          : encodeNativeArguments(functionMetadata, arguments_);
      callMessage = JSON.stringify({
        version: protocolVersion,
        type: 'call',
        callId,
        capabilityId,
        functionPath,
        arguments: encodedArguments.value,
      });
    } catch (error) {
      return Promise.reject(error);
    }

    return new Promise<unknown>((resolve, reject) => {
      const pending: PendingCall = {
        resolve,
        reject,
        signal: options?.signal,
        returnType: functionsByPath.get(functionPath)?.returnType,
        abortListener: undefined,
      };
      if (pending.signal !== undefined) {
        pending.abortListener = () => {
          if (takePending(callId) === undefined) {
            return;
          }
          try {
            transport.send(
              JSON.stringify({
                version: protocolVersion,
                type: 'cancel',
                callId,
              })
            );
          } finally {
            reject(createAbortError());
          }
        };
        pending.signal.addEventListener('abort', pending.abortListener, {
          once: true,
        });
      }
      pendingCalls.set(callId, pending);

      try {
        transport.send(callMessage);
        encodedArguments.attachments.forEach((payload, attachment) => {
          transport.send(
            encodeMuonWebViewRpcBinaryFrame({
              kind: 'argument',
              callId,
              attachment,
              payload,
            })
          );
        });
      } catch (error) {
        const failed = takePending(callId);
        failed?.reject(error);
      }
    });
  };

  const dispose = (): void => {
    if (disposed) {
      return;
    }
    disposed = true;
    transport.setMessageHandler(undefined);
    try {
      transport.send(
        JSON.stringify({ version: protocolVersion, type: 'release' })
      );
    } catch {
      // The JavaScript context is being released, so transport errors are final.
    }
    for (const pending of pendingCalls.values()) {
      detachAbortListener(pending);
      pending.reject(new Error('muon WebView RPC client was disposed'));
    }
    pendingCalls.clear();
  };

  return {
    call,
    dispose,
    getPendingCallCount: () => pendingCalls.size,
  };
};

/**
 * Installs the capability call boundary consumed by generated muon modules.
 *
 * @param client - WebView RPC client used for generated capability calls.
 * @param target - Global-like object on which to install the boundary.
 * @returns A function that restores the previous property state.
 */
export const installMuonWebViewCapabilityBridge = (
  client: MuonWebViewRpcClient,
  target: Record<PropertyKey, unknown> = globalThis as unknown as Record<
    PropertyKey,
    unknown
  >
): (() => void) => {
  const property = '__muon_plugin_call';
  const previous = Object.getOwnPropertyDescriptor(target, property);
  Object.defineProperty(target, property, {
    configurable: true,
    enumerable: false,
    writable: true,
    value: (
      capabilityId: string,
      functionPath: string,
      arguments_: readonly unknown[]
    ) => client.call(capabilityId, functionPath, arguments_),
  });

  return () => {
    if (previous === undefined) {
      Reflect.deleteProperty(target, property);
    } else {
      Object.defineProperty(target, property, previous);
    }
  };
};
