// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

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
  transport: MuonWebViewRpcTransport
): MuonWebViewRpcClient => {
  const pendingCalls = new Map<number, PendingCall>();
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
      pending.resolve(parsed.value);
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
    pending?.resolve(frame.payload);
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
      encodedArguments = encodeArguments(arguments_);
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
