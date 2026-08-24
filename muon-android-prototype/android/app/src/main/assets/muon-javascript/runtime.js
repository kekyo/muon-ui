/* muon QuickJS runtime protocol and Android host modules. */

'use strict';

(() => {
  const signed64Minimum = -(1n << 63n);
  const signed64Maximum = (1n << 63n) - 1n;
  const unsigned64Maximum = (1n << 64n) - 1n;
  const base64Alphabet =
    'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
  const moduleHandles = new Map();
  const rendererCallbacks = new Map();
  let nextModuleHandle = 1;
  let nextCallbackRequest = 1;

  const createError = (code, message) => {
    const error = new Error(message);
    error.code = code;
    return error;
  };

  const encodeBase64 = (value) => {
    let result = '';
    for (let index = 0; index < value.length; index += 3) {
      const first = value[index];
      const hasSecond = index + 1 < value.length;
      const hasThird = index + 2 < value.length;
      const second = hasSecond ? value[index + 1] : 0;
      const third = hasThird ? value[index + 2] : 0;
      const packed = (first << 16) | (second << 8) | third;
      result += base64Alphabet[(packed >>> 18) & 63];
      result += base64Alphabet[(packed >>> 12) & 63];
      result += hasSecond ? base64Alphabet[(packed >>> 6) & 63] : '=';
      result += hasThird ? base64Alphabet[packed & 63] : '=';
    }
    return result;
  };

  const decodeBase64 = (value) => {
    if (
      typeof value !== 'string' ||
      value.length % 4 !== 0 ||
      !/^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/.test(
        value
      )
    ) {
      throw createError(
        'ERR_MUON_JS_UNSUPPORTED_VALUE',
        'buffer contains invalid base64 data'
      );
    }
    const padding = value.endsWith('==') ? 2 : value.endsWith('=') ? 1 : 0;
    const result = new Uint8Array((value.length / 4) * 3 - padding);
    let output = 0;
    for (let index = 0; index < value.length; index += 4) {
      const first = base64Alphabet.indexOf(value[index]);
      const second = base64Alphabet.indexOf(value[index + 1]);
      const third =
        value[index + 2] === '=' ? 0 : base64Alphabet.indexOf(value[index + 2]);
      const fourth =
        value[index + 3] === '=' ? 0 : base64Alphabet.indexOf(value[index + 3]);
      const packed = (first << 18) | (second << 12) | (third << 6) | fourth;
      if (output < result.length) result[output++] = (packed >>> 16) & 255;
      if (output < result.length) result[output++] = (packed >>> 8) & 255;
      if (output < result.length) result[output++] = packed & 255;
    }
    if (encodeBase64(result) !== value) {
      throw createError(
        'ERR_MUON_JS_UNSUPPORTED_VALUE',
        'buffer contains non-canonical base64 data'
      );
    }
    return result;
  };

  const normalizeJson = (value, ancestors) => {
    if (
      value === null ||
      typeof value === 'boolean' ||
      typeof value === 'string'
    ) {
      return value;
    }
    if (typeof value === 'number') {
      if (!Number.isFinite(value) || Object.is(value, -0)) {
        throw createError(
          'ERR_MUON_JS_UNSUPPORTED_VALUE',
          'JSON containers support only finite numbers other than negative zero'
        );
      }
      return value;
    }
    if (typeof value !== 'object' || value === null) {
      throw createError(
        'ERR_MUON_JS_UNSUPPORTED_VALUE',
        'JSON containers can contain only JSON-compatible values'
      );
    }
    if (ancestors.has(value)) {
      throw createError(
        'ERR_MUON_JS_UNSUPPORTED_VALUE',
        'JSON containers cannot contain cycles'
      );
    }
    ancestors.add(value);
    try {
      if (Array.isArray(value)) {
        return value.map((entry) => normalizeJson(entry, ancestors));
      }
      if (
        Object.getPrototypeOf(value) !== Object.prototype &&
        Object.getPrototypeOf(value) !== null
      ) {
        throw createError(
          'ERR_MUON_JS_UNSUPPORTED_VALUE',
          'JSON objects must be plain objects'
        );
      }
      const result = {};
      for (const key of Object.keys(value)) {
        result[key] = normalizeJson(value[key], ancestors);
      }
      return result;
    } finally {
      ancestors.delete(value);
    }
  };

  const encodeValue = (value) => {
    if (value === undefined) return { kind: 'undefined' };
    if (
      value === null ||
      typeof value === 'boolean' ||
      typeof value === 'string'
    ) {
      return value;
    }
    if (typeof value === 'number') {
      if (!Number.isFinite(value) || Object.is(value, -0)) {
        throw createError(
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
      throw createError(
        'ERR_MUON_JS_UNSUPPORTED_VALUE',
        'The bigint value is outside the i64/u64 range'
      );
    }
    if (value instanceof Uint8Array) {
      return { kind: 'buffer', data: encodeBase64(value) };
    }
    if (value instanceof ArrayBuffer) {
      return { kind: 'buffer', data: encodeBase64(new Uint8Array(value)) };
    }
    return { kind: 'json', value: normalizeJson(value, new Set()) };
  };

  const decodeInteger = (value) => {
    if (
      typeof value.value !== 'string' ||
      !/^(?:0|-?[1-9][0-9]*)$/.test(value.value)
    ) {
      throw createError(
        'ERR_MUON_JS_UNSUPPORTED_VALUE',
        'Integer tags must contain a canonical decimal string'
      );
    }
    const decoded = BigInt(value.value);
    const valid =
      value.kind === 'i64'
        ? decoded >= signed64Minimum && decoded <= signed64Maximum
        : decoded >= 0n && decoded <= unsigned64Maximum;
    if (!valid) {
      throw createError(
        'ERR_MUON_JS_UNSUPPORTED_VALUE',
        `${value.kind} is outside its supported range`
      );
    }
    return decoded;
  };

  const invokeRendererCallback = async (handle, arguments_) => {
    const id = `callback-${nextCallbackRequest++}`;
    let resolveOperation;
    let rejectOperation;
    const result = new Promise((resolve, reject) => {
      resolveOperation = resolve;
      rejectOperation = reject;
    });
    rendererCallbacks.set(id, {
      resolve: resolveOperation,
      reject: rejectOperation,
    });
    __muonPostMessage(
      JSON.stringify({
        kind: 'callback',
        id,
        handle,
        arguments: arguments_.map(encodeValue),
      })
    );
    return await result;
  };

  const decodeValue = (value) => {
    if (
      value === null ||
      typeof value === 'boolean' ||
      typeof value === 'string' ||
      typeof value === 'number'
    ) {
      return value;
    }
    if (typeof value !== 'object' || value === null || Array.isArray(value)) {
      throw createError(
        'ERR_MUON_JS_UNSUPPORTED_VALUE',
        'Wire values must be supported primitives or tagged objects'
      );
    }
    if (value.kind === 'undefined') return undefined;
    if (value.kind === 'i64' || value.kind === 'u64')
      return decodeInteger(value);
    if (value.kind === 'buffer') return decodeBase64(value.data);
    if (value.kind === 'json') return normalizeJson(value.value, new Set());
    if (value.kind === 'function' && typeof value.handle === 'string') {
      return async (...arguments_) =>
        await invokeRendererCallback(value.handle, arguments_);
    }
    throw createError(
      'ERR_MUON_JS_UNSUPPORTED_VALUE',
      'The bridge supplied an unknown wire value'
    );
  };

  const normalizeBufferEncoding = (encoding) => {
    const normalized = String(encoding ?? 'utf8').toLowerCase();
    if (normalized === 'utf8' || normalized === 'utf-8') return 'utf8';
    if (normalized === 'hex') return 'hex';
    if (normalized === 'base64') return 'base64';
    if (normalized === 'base64url') return 'base64url';
    if (
      normalized === 'latin1' ||
      normalized === 'binary' ||
      normalized === 'ascii'
    ) {
      return 'latin1';
    }
    throw createError(
      'ERR_UNKNOWN_ENCODING',
      `Unknown buffer encoding: ${encoding}`
    );
  };

  const encodeUtf8 = (value) => {
    const source = String(value);
    const bytes = [];
    for (let index = 0; index < source.length; index += 1) {
      let point = source.codePointAt(index);
      if (point >= 0xd800 && point <= 0xdfff) point = 0xfffd;
      if (point > 0xffff) index += 1;
      if (point <= 0x7f) {
        bytes.push(point);
      } else if (point <= 0x7ff) {
        bytes.push(0xc0 | (point >> 6), 0x80 | (point & 0x3f));
      } else if (point <= 0xffff) {
        bytes.push(
          0xe0 | (point >> 12),
          0x80 | ((point >> 6) & 0x3f),
          0x80 | (point & 0x3f)
        );
      } else {
        bytes.push(
          0xf0 | (point >> 18),
          0x80 | ((point >> 12) & 0x3f),
          0x80 | ((point >> 6) & 0x3f),
          0x80 | (point & 0x3f)
        );
      }
    }
    return Uint8Array.from(bytes);
  };

  const decodeUtf8 = (bytes) => {
    let result = '';
    let index = 0;
    const continuation = (offset) =>
      index + offset < bytes.length && (bytes[index + offset] & 0xc0) === 0x80;
    while (index < bytes.length) {
      const first = bytes[index];
      let point = 0xfffd;
      let length = 1;
      if (first <= 0x7f) {
        point = first;
      } else if (first >= 0xc2 && first <= 0xdf && continuation(1)) {
        point = ((first & 0x1f) << 6) | (bytes[index + 1] & 0x3f);
        length = 2;
      } else if (
        first >= 0xe0 &&
        first <= 0xef &&
        continuation(1) &&
        continuation(2) &&
        !(first === 0xe0 && bytes[index + 1] < 0xa0) &&
        !(first === 0xed && bytes[index + 1] >= 0xa0)
      ) {
        point =
          ((first & 0x0f) << 12) |
          ((bytes[index + 1] & 0x3f) << 6) |
          (bytes[index + 2] & 0x3f);
        length = 3;
      } else if (
        first >= 0xf0 &&
        first <= 0xf4 &&
        continuation(1) &&
        continuation(2) &&
        continuation(3) &&
        !(first === 0xf0 && bytes[index + 1] < 0x90) &&
        !(first === 0xf4 && bytes[index + 1] >= 0x90)
      ) {
        point =
          ((first & 0x07) << 18) |
          ((bytes[index + 1] & 0x3f) << 12) |
          ((bytes[index + 2] & 0x3f) << 6) |
          (bytes[index + 3] & 0x3f);
        length = 4;
      }
      result += String.fromCodePoint(point);
      index += length;
    }
    return result;
  };

  const encodeHexBuffer = (value) => {
    const source = String(value);
    const bytes = [];
    for (let index = 0; index + 1 < source.length; index += 2) {
      const pair = source.slice(index, index + 2);
      if (!/^[0-9a-fA-F]{2}$/.test(pair)) break;
      bytes.push(Number.parseInt(pair, 16));
    }
    return Uint8Array.from(bytes);
  };

  const decodeBufferBase64 = (value, urlSafe) => {
    let source = String(value).replaceAll(/\s/g, '');
    if (urlSafe) source = source.replaceAll('-', '+').replaceAll('_', '/');
    source = source.replace(/=+$/, '');
    if (!/^[A-Za-z0-9+/]*$/.test(source) || source.length % 4 === 1) {
      throw createError('ERR_INVALID_ARG_VALUE', 'Invalid base64 data');
    }
    source += '='.repeat((4 - (source.length % 4)) % 4);
    return decodeBase64(source);
  };

  const encodeBufferString = (value, encoding) => {
    const normalized = normalizeBufferEncoding(encoding);
    if (normalized === 'utf8') return encodeUtf8(value);
    if (normalized === 'hex') return encodeHexBuffer(value);
    if (normalized === 'base64' || normalized === 'base64url') {
      return decodeBufferBase64(value, normalized === 'base64url');
    }
    return Uint8Array.from(
      String(value),
      (character) => character.charCodeAt(0) & 0xff
    );
  };

  const decodeBufferString = (bytes, encoding) => {
    const normalized = normalizeBufferEncoding(encoding);
    if (normalized === 'utf8') return decodeUtf8(bytes);
    if (normalized === 'hex') {
      return Array.from(bytes, (value) =>
        value.toString(16).padStart(2, '0')
      ).join('');
    }
    if (normalized === 'base64' || normalized === 'base64url') {
      const encoded = encodeBase64(bytes);
      return normalized === 'base64url'
        ? encoded.replaceAll('+', '-').replaceAll('/', '_').replace(/=+$/, '')
        : encoded;
    }
    return Array.from(bytes, (value) => String.fromCharCode(value)).join('');
  };

  const bufferMarker = Symbol('muon.buffer');
  const bufferPrototype = Object.create(Uint8Array.prototype);
  const markBuffer = (value) => {
    Object.setPrototypeOf(value, bufferPrototype);
    return value;
  };

  const validateBufferSize = (size) => {
    const normalized = Number(size);
    if (!Number.isInteger(normalized) || normalized < 0) {
      throw createError(
        'ERR_INVALID_ARG_VALUE',
        'Buffer size must be a non-negative integer'
      );
    }
    return normalized;
  };

  const Buffer = function (value, encodingOrOffset, length) {
    if (typeof value === 'number') return Buffer.allocUnsafe(value);
    return Buffer.from(value, encodingOrOffset, length);
  };

  Object.defineProperty(bufferPrototype, 'constructor', {
    value: Buffer,
    configurable: true,
    enumerable: false,
    writable: true,
  });
  Object.defineProperty(bufferPrototype, bufferMarker, {
    value: true,
    configurable: false,
    enumerable: false,
    writable: false,
  });
  Buffer.prototype = bufferPrototype;

  Buffer.from = (value, encodingOrOffset, length) => {
    if (typeof value === 'string') {
      return markBuffer(encodeBufferString(value, encodingOrOffset));
    }
    if (value instanceof ArrayBuffer) {
      const offset =
        encodingOrOffset === undefined ? 0 : Number(encodingOrOffset);
      const available = value.byteLength - offset;
      const byteLength = length === undefined ? available : Number(length);
      if (
        !Number.isInteger(offset) ||
        !Number.isInteger(byteLength) ||
        offset < 0 ||
        byteLength < 0 ||
        offset + byteLength > value.byteLength
      ) {
        throw createError(
          'ERR_BUFFER_OUT_OF_BOUNDS',
          'Buffer view is outside the ArrayBuffer'
        );
      }
      return markBuffer(new Uint8Array(value, offset, byteLength));
    }
    if (ArrayBuffer.isView(value)) {
      return markBuffer(
        Uint8Array.from(
          new Uint8Array(value.buffer, value.byteOffset, value.byteLength)
        )
      );
    }
    if (value !== null && typeof value === 'object') {
      if (value.type === 'Buffer' && Array.isArray(value.data)) {
        return markBuffer(Uint8Array.from(value.data));
      }
      if (typeof value.length === 'number' || value[Symbol.iterator]) {
        return markBuffer(Uint8Array.from(value));
      }
    }
    throw createError(
      'ERR_INVALID_ARG_TYPE',
      'Buffer.from value must be text, bytes, or an ArrayBuffer'
    );
  };

  Buffer.alloc = (size, fill, encoding) => {
    const result = markBuffer(new Uint8Array(validateBufferSize(size)));
    if (fill !== undefined) result.fill(fill, 0, result.length, encoding);
    return result;
  };
  Buffer.allocUnsafe = (size) =>
    markBuffer(new Uint8Array(validateBufferSize(size)));
  Buffer.allocUnsafeSlow = Buffer.allocUnsafe;
  Buffer.isBuffer = (value) => Boolean(value && value[bufferMarker] === true);
  Buffer.isEncoding = (encoding) => {
    try {
      normalizeBufferEncoding(encoding);
      return true;
    } catch {
      return false;
    }
  };
  Buffer.byteLength = (value, encoding) => {
    if (value instanceof ArrayBuffer || ArrayBuffer.isView(value)) {
      return value.byteLength;
    }
    return encodeBufferString(String(value), encoding).byteLength;
  };
  Buffer.compare = (left, right) => {
    if (!(left instanceof Uint8Array) || !(right instanceof Uint8Array)) {
      throw createError(
        'ERR_INVALID_ARG_TYPE',
        'Buffer.compare values must be Uint8Array instances'
      );
    }
    const length = Math.min(left.length, right.length);
    for (let index = 0; index < length; index += 1) {
      if (left[index] !== right[index])
        return left[index] < right[index] ? -1 : 1;
    }
    return left.length === right.length
      ? 0
      : left.length < right.length
        ? -1
        : 1;
  };
  Buffer.concat = (list, totalLength) => {
    if (!Array.isArray(list)) {
      throw createError(
        'ERR_INVALID_ARG_TYPE',
        'Buffer.concat list must be an array'
      );
    }
    const length =
      totalLength === undefined
        ? list.reduce((sum, value) => sum + value.byteLength, 0)
        : validateBufferSize(totalLength);
    const result = Buffer.alloc(length);
    let offset = 0;
    for (const value of list) {
      if (!(value instanceof Uint8Array)) {
        throw createError(
          'ERR_INVALID_ARG_TYPE',
          'Buffer.concat entries must be Uint8Array instances'
        );
      }
      const available = Math.min(value.length, result.length - offset);
      if (available <= 0) break;
      Uint8Array.prototype.set.call(
        result,
        value.subarray(0, available),
        offset
      );
      offset += available;
    }
    return result;
  };

  const normalizeBufferIndex = (value, length, fallback) => {
    if (value === undefined) return fallback;
    const integer = Math.trunc(Number(value));
    if (!Number.isFinite(integer)) return integer < 0 ? 0 : length;
    if (integer < 0) return Math.max(length + integer, 0);
    return Math.min(integer, length);
  };

  const bufferToString = function (encoding, start, end) {
    const from = normalizeBufferIndex(start, this.length, 0);
    const to = normalizeBufferIndex(end, this.length, this.length);
    const bytes = new Uint8Array(
      this.buffer,
      this.byteOffset + Math.min(from, to),
      Math.max(to - from, 0)
    );
    return decodeBufferString(bytes, encoding);
  };
  const bufferEquals = function (other) {
    return Buffer.compare(this, other) === 0;
  };
  const bufferCompare = function (other) {
    return Buffer.compare(this, other);
  };
  const bufferSubarray = function (start, end) {
    const from = normalizeBufferIndex(start, this.length, 0);
    const to = normalizeBufferIndex(end, this.length, this.length);
    return markBuffer(
      new Uint8Array(
        this.buffer,
        this.byteOffset + Math.min(from, to),
        Math.max(to - from, 0)
      )
    );
  };
  const bufferFill = function (value, start, end, encoding) {
    const from = normalizeBufferIndex(start, this.length, 0);
    const to = normalizeBufferIndex(end, this.length, this.length);
    if (typeof value === 'number') {
      Uint8Array.prototype.fill.call(this, value & 0xff, from, to);
      return this;
    }
    const pattern = Buffer.isBuffer(value)
      ? value
      : encodeBufferString(String(value), encoding);
    if (pattern.length === 0) return this;
    for (let index = from; index < to; index += 1) {
      this[index] = pattern[(index - from) % pattern.length];
    }
    return this;
  };
  const bufferWrite = function (value, offset, length, encoding) {
    const from = normalizeBufferIndex(offset, this.length, 0);
    const available =
      length === undefined ? this.length - from : validateBufferSize(length);
    const source = encodeBufferString(value, encoding);
    const count = Math.min(available, source.length, this.length - from);
    Uint8Array.prototype.set.call(this, source.subarray(0, count), from);
    return count;
  };
  const bufferCopy = function (target, targetStart, sourceStart, sourceEnd) {
    if (!(target instanceof Uint8Array)) {
      throw createError(
        'ERR_INVALID_ARG_TYPE',
        'copy target must be a Uint8Array'
      );
    }
    const targetOffset = normalizeBufferIndex(targetStart, target.length, 0);
    const source = bufferSubarray.call(this, sourceStart, sourceEnd);
    const count = Math.min(source.length, target.length - targetOffset);
    Uint8Array.prototype.set.call(
      target,
      source.subarray(0, count),
      targetOffset
    );
    return count;
  };
  const bufferToJson = function () {
    return { type: 'Buffer', data: Array.from(this) };
  };

  bufferPrototype.toString = bufferToString;
  bufferPrototype.equals = bufferEquals;
  bufferPrototype.compare = bufferCompare;
  bufferPrototype.subarray = bufferSubarray;
  bufferPrototype.slice = bufferSubarray;
  bufferPrototype.fill = bufferFill;
  bufferPrototype.write = bufferWrite;
  bufferPrototype.copy = bufferCopy;
  bufferPrototype.toJSON = bufferToJson;

  const bufferModule = Object.freeze({
    Buffer,
    SlowBuffer: (size) => Buffer.alloc(size),
    atob: (value) =>
      decodeBufferString(decodeBufferBase64(value, false), 'latin1'),
    btoa: (value) => encodeBase64(encodeBufferString(value, 'latin1')),
  });

  const eventType = Symbol('muon.event.type');
  const eventTarget = Symbol('muon.event.target');
  const eventCurrentTarget = Symbol('muon.event.currentTarget');
  const eventPropagationStopped = Symbol('muon.event.propagationStopped');
  const eventImmediatePropagationStopped = Symbol(
    'muon.event.immediatePropagationStopped'
  );
  const eventTargetListeners = Symbol('muon.eventTarget.listeners');

  const Event = function (type, options) {
    if (type === undefined) {
      throw createError('ERR_MISSING_ARGS', 'Event type is required');
    }
    this[eventType] = String(type);
    this[eventTarget] = null;
    this[eventCurrentTarget] = null;
    this[eventPropagationStopped] = false;
    this[eventImmediatePropagationStopped] = false;
    this.bubbles = Boolean(options && options.bubbles);
    this.cancelable = Boolean(options && options.cancelable);
    this.composed = Boolean(options && options.composed);
    this.defaultPrevented = false;
    this.timeStamp = Date.now();
  };
  Object.defineProperties(Event.prototype, {
    type: {
      get: function () {
        return this[eventType];
      },
    },
    target: {
      get: function () {
        return this[eventTarget];
      },
    },
    currentTarget: {
      get: function () {
        return this[eventCurrentTarget];
      },
    },
    eventPhase: { get: () => 2 },
  });
  Event.prototype.composedPath = function () {
    return this[eventTarget] === null ? [] : [this[eventTarget]];
  };
  Event.prototype.preventDefault = function () {
    if (this.cancelable) this.defaultPrevented = true;
  };
  Event.prototype.stopPropagation = function () {
    this[eventPropagationStopped] = true;
  };
  Event.prototype.stopImmediatePropagation = function () {
    this[eventPropagationStopped] = true;
    this[eventImmediatePropagationStopped] = true;
  };

  const EventTarget = function () {
    Object.defineProperty(this, eventTargetListeners, {
      value: new Map(),
      configurable: false,
      enumerable: false,
      writable: false,
    });
  };

  const requireEventTargetListeners = (target) => {
    const listeners = target[eventTargetListeners];
    if (!(listeners instanceof Map)) {
      throw createError(
        'ERR_INVALID_THIS',
        'EventTarget method called on an incompatible receiver'
      );
    }
    return listeners;
  };

  const normalizeEventOptions = (options) =>
    typeof options === 'boolean'
      ? { capture: options, once: false, signal: undefined }
      : {
          capture: Boolean(options && options.capture),
          once: Boolean(options && options.once),
          signal: options && options.signal,
        };

  const eventTargetRemoveEventListener = function (type, listener, options) {
    if (listener === null || listener === undefined) return;
    const listeners = requireEventTargetListeners(this);
    const name = String(type);
    const capture = normalizeEventOptions(options).capture;
    const entries = listeners.get(name);
    if (!entries) return;
    const index = entries.findIndex(
      (entry) => entry.listener === listener && entry.capture === capture
    );
    if (index < 0) return;
    const [entry] = entries.splice(index, 1);
    if (entries.length === 0) listeners.delete(name);
    if (entry.abortSignal && entry.abortListener) {
      entry.abortSignal.removeEventListener('abort', entry.abortListener);
    }
  };

  const eventTargetAddEventListener = function (type, listener, options) {
    if (listener === null || listener === undefined) return;
    if (
      typeof listener !== 'function' &&
      (typeof listener !== 'object' ||
        typeof listener.handleEvent !== 'function')
    ) {
      throw createError(
        'ERR_INVALID_ARG_TYPE',
        'Event listener must be a function or EventListener object'
      );
    }
    const listeners = requireEventTargetListeners(this);
    const name = String(type);
    const normalized = normalizeEventOptions(options);
    if (normalized.signal && normalized.signal.aborted) return;
    const entries = listeners.get(name) ?? [];
    if (
      entries.some(
        (entry) =>
          entry.listener === listener && entry.capture === normalized.capture
      )
    ) {
      return;
    }
    const entry = {
      listener,
      capture: normalized.capture,
      once: normalized.once,
      abortSignal: normalized.signal,
      abortListener: undefined,
    };
    entries.push(entry);
    listeners.set(name, entries);
    if (normalized.signal) {
      entry.abortListener = () =>
        eventTargetRemoveEventListener.call(this, name, listener, {
          capture: normalized.capture,
        });
      normalized.signal.addEventListener('abort', entry.abortListener, {
        once: true,
      });
    }
  };

  const eventTargetDispatchEvent = function (event) {
    if (!(event instanceof Event)) {
      throw createError(
        'ERR_INVALID_ARG_TYPE',
        'dispatchEvent requires an Event'
      );
    }
    const listeners = requireEventTargetListeners(this);
    event[eventTarget] = this;
    event[eventCurrentTarget] = this;
    event[eventPropagationStopped] = false;
    event[eventImmediatePropagationStopped] = false;
    const entries = [...(listeners.get(event.type) ?? [])];
    for (const entry of entries) {
      if (event[eventImmediatePropagationStopped]) break;
      if (entry.once) {
        eventTargetRemoveEventListener.call(this, event.type, entry.listener, {
          capture: entry.capture,
        });
      }
      if (typeof entry.listener === 'function') {
        Reflect.apply(entry.listener, this, [event]);
      } else {
        Reflect.apply(entry.listener.handleEvent, entry.listener, [event]);
      }
    }
    event[eventCurrentTarget] = null;
    return !event.defaultPrevented;
  };

  EventTarget.prototype.addEventListener = eventTargetAddEventListener;
  EventTarget.prototype.removeEventListener = eventTargetRemoveEventListener;
  EventTarget.prototype.dispatchEvent = eventTargetDispatchEvent;

  const domExceptionCodes = Object.freeze({
    AbortError: 20,
    TimeoutError: 23,
  });
  const DOMException = function (message, name) {
    const error = new Error(message === undefined ? '' : String(message));
    Object.setPrototypeOf(error, DOMException.prototype);
    error.name = name === undefined ? 'Error' : String(name);
    Object.defineProperty(error, 'code', {
      value: domExceptionCodes[error.name] ?? 0,
      configurable: true,
      enumerable: true,
      writable: false,
    });
    return error;
  };
  DOMException.prototype = Object.create(Error.prototype);
  Object.defineProperty(DOMException.prototype, 'constructor', {
    value: DOMException,
    configurable: true,
    enumerable: false,
    writable: true,
  });

  const abortSignalKey = {};
  const abortSignalAborted = Symbol('muon.abortSignal.aborted');
  const abortSignalReason = Symbol('muon.abortSignal.reason');
  const AbortSignal = function (key) {
    if (key !== abortSignalKey) {
      throw createError(
        'ERR_ILLEGAL_CONSTRUCTOR',
        'AbortSignal cannot be constructed directly'
      );
    }
    EventTarget.call(this);
    this[abortSignalAborted] = false;
    this[abortSignalReason] = undefined;
    this.onabort = null;
  };
  AbortSignal.prototype = Object.create(EventTarget.prototype);
  Object.defineProperty(AbortSignal.prototype, 'constructor', {
    value: AbortSignal,
    configurable: true,
    enumerable: false,
    writable: true,
  });
  Object.defineProperties(AbortSignal.prototype, {
    aborted: {
      get: function () {
        return this[abortSignalAborted];
      },
    },
    reason: {
      get: function () {
        return this[abortSignalReason];
      },
    },
  });
  AbortSignal.prototype.throwIfAborted = function () {
    if (this[abortSignalAborted]) throw this[abortSignalReason];
  };

  const abortSignal = (signal, reason) => {
    if (signal[abortSignalAborted]) return;
    signal[abortSignalAborted] = true;
    signal[abortSignalReason] =
      reason === undefined
        ? new DOMException('This operation was aborted', 'AbortError')
        : reason;
    const event = new Event('abort');
    signal.dispatchEvent(event);
    if (typeof signal.onabort === 'function') {
      Reflect.apply(signal.onabort, signal, [event]);
    }
  };

  AbortSignal.abort = (reason) => {
    const signal = new AbortSignal(abortSignalKey);
    abortSignal(signal, reason);
    return signal;
  };
  AbortSignal.timeout = (delay) => {
    const milliseconds = Number(delay);
    if (!Number.isInteger(milliseconds) || milliseconds < 0) {
      throw createError(
        'ERR_OUT_OF_RANGE',
        'AbortSignal timeout must be a non-negative integer'
      );
    }
    const signal = new AbortSignal(abortSignalKey);
    globalThis.setTimeout(
      () =>
        abortSignal(
          signal,
          new DOMException('The operation timed out', 'TimeoutError')
        ),
      milliseconds
    );
    return signal;
  };
  AbortSignal.any = (signals) => {
    if (
      signals === null ||
      signals === undefined ||
      !signals[Symbol.iterator]
    ) {
      throw createError('ERR_INVALID_ARG_TYPE', 'signals must be iterable');
    }
    const result = new AbortSignal(abortSignalKey);
    const subscriptions = [];
    const detach = () => {
      for (const subscription of subscriptions) {
        subscription.signal.removeEventListener('abort', subscription.listener);
      }
      subscriptions.length = 0;
    };
    for (const signal of signals) {
      if (!(signal instanceof AbortSignal)) {
        detach();
        throw createError(
          'ERR_INVALID_ARG_TYPE',
          'signals entries must be AbortSignal instances'
        );
      }
      if (signal.aborted) {
        detach();
        abortSignal(result, signal.reason);
        return result;
      }
      const listener = () => {
        detach();
        abortSignal(result, signal.reason);
      };
      subscriptions.push({ signal, listener });
      signal.addEventListener('abort', listener, { once: true });
    }
    return result;
  };

  const AbortController = function () {
    Object.defineProperty(this, 'signal', {
      value: new AbortSignal(abortSignalKey),
      configurable: false,
      enumerable: true,
      writable: false,
    });
  };
  AbortController.prototype.abort = function (reason) {
    abortSignal(this.signal, reason);
  };

  const createAbortError = (reason) => {
    const error = new Error('The operation was aborted');
    error.name = 'AbortError';
    error.code = 'ABORT_ERR';
    error.cause = reason;
    return error;
  };

  const eventEmitterListeners = Symbol('muon.eventEmitter.listeners');
  const eventEmitterMaximum = Symbol('muon.eventEmitter.maximum');
  const EventEmitter = function () {
    if (!(this instanceof EventEmitter)) return new EventEmitter();
    Object.defineProperty(this, eventEmitterListeners, {
      value: new Map(),
      configurable: false,
      enumerable: false,
      writable: false,
    });
    this[eventEmitterMaximum] = undefined;
  };
  EventEmitter.defaultMaxListeners = 10;

  const requireEmitterListeners = (emitter) => {
    const listeners = emitter[eventEmitterListeners];
    if (!(listeners instanceof Map)) {
      throw createError(
        'ERR_INVALID_THIS',
        'EventEmitter method called on an incompatible receiver'
      );
    }
    return listeners;
  };
  const validateEventListener = (listener) => {
    if (typeof listener !== 'function') {
      throw createError('ERR_INVALID_ARG_TYPE', 'listener must be a function');
    }
  };
  const addEmitterListener = (emitter, name, listener, prepend) => {
    validateEventListener(listener);
    const listeners = requireEmitterListeners(emitter);
    if (listeners.has('newListener')) {
      emitter.emit('newListener', name, listener.listener ?? listener);
    }
    const entries = listeners.get(name) ?? [];
    if (prepend) entries.unshift(listener);
    else entries.push(listener);
    listeners.set(name, entries);
    return emitter;
  };
  const eventEmitterOn = function (name, listener) {
    return addEmitterListener(this, name, listener, false);
  };
  const eventEmitterPrependListener = function (name, listener) {
    return addEmitterListener(this, name, listener, true);
  };
  const createOnceListener = (emitter, name, listener) => {
    let fired = false;
    const wrapper = function (...arguments_) {
      if (fired) return undefined;
      fired = true;
      emitter.removeListener(name, wrapper);
      return Reflect.apply(listener, emitter, arguments_);
    };
    wrapper.listener = listener;
    return wrapper;
  };
  const eventEmitterOnce = function (name, listener) {
    validateEventListener(listener);
    return this.on(name, createOnceListener(this, name, listener));
  };
  const eventEmitterPrependOnceListener = function (name, listener) {
    validateEventListener(listener);
    return this.prependListener(name, createOnceListener(this, name, listener));
  };
  const eventEmitterEmit = function (name, ...arguments_) {
    const listeners = requireEmitterListeners(this);
    const entries = listeners.get(name);
    if ((!entries || entries.length === 0) && name === 'error') {
      const error = arguments_[0];
      throw error instanceof Error
        ? error
        : createError(
            'ERR_UNHANDLED_ERROR',
            `Unhandled error: ${String(error)}`
          );
    }
    if (!entries || entries.length === 0) return false;
    for (const listener of [...entries]) {
      Reflect.apply(listener, this, arguments_);
    }
    return true;
  };
  const eventEmitterRemoveListener = function (name, listener) {
    validateEventListener(listener);
    const listeners = requireEmitterListeners(this);
    const entries = listeners.get(name);
    if (!entries) return this;
    let removed;
    for (let index = entries.length - 1; index >= 0; index -= 1) {
      const entry = entries[index];
      if (entry === listener || entry.listener === listener) {
        [removed] = entries.splice(index, 1);
        break;
      }
    }
    if (!removed) return this;
    if (entries.length === 0) listeners.delete(name);
    if (listeners.has('removeListener')) {
      this.emit('removeListener', name, removed.listener ?? removed);
    }
    return this;
  };
  const eventEmitterRemoveAllListeners = function (name) {
    const listeners = requireEmitterListeners(this);
    if (name !== undefined) {
      for (const listener of [...(listeners.get(name) ?? [])].reverse()) {
        this.removeListener(name, listener);
      }
      return this;
    }
    for (const eventName of [...listeners.keys()]) {
      if (eventName !== 'removeListener') this.removeAllListeners(eventName);
    }
    this.removeAllListeners('removeListener');
    return this;
  };
  const eventEmitterListenersFor = function (name) {
    return (requireEmitterListeners(this).get(name) ?? []).map(
      (listener) => listener.listener ?? listener
    );
  };
  const eventEmitterRawListeners = function (name) {
    return [...(requireEmitterListeners(this).get(name) ?? [])];
  };
  const eventEmitterListenerCount = function (name, listener) {
    const entries = requireEmitterListeners(this).get(name) ?? [];
    if (listener === undefined) return entries.length;
    return entries.filter(
      (entry) => entry === listener || entry.listener === listener
    ).length;
  };
  const eventEmitterEventNames = function () {
    return [...requireEmitterListeners(this).keys()];
  };
  const eventEmitterSetMaxListeners = function (maximum) {
    if (!Number.isInteger(maximum) || maximum < 0) {
      throw createError(
        'ERR_OUT_OF_RANGE',
        'maximum listeners must be a non-negative integer'
      );
    }
    this[eventEmitterMaximum] = maximum;
    return this;
  };
  const eventEmitterGetMaxListeners = function () {
    return this[eventEmitterMaximum] ?? EventEmitter.defaultMaxListeners;
  };

  EventEmitter.prototype.addListener = eventEmitterOn;
  EventEmitter.prototype.on = eventEmitterOn;
  EventEmitter.prototype.prependListener = eventEmitterPrependListener;
  EventEmitter.prototype.once = eventEmitterOnce;
  EventEmitter.prototype.prependOnceListener = eventEmitterPrependOnceListener;
  EventEmitter.prototype.emit = eventEmitterEmit;
  EventEmitter.prototype.removeListener = eventEmitterRemoveListener;
  EventEmitter.prototype.off = eventEmitterRemoveListener;
  EventEmitter.prototype.removeAllListeners = eventEmitterRemoveAllListeners;
  EventEmitter.prototype.listeners = eventEmitterListenersFor;
  EventEmitter.prototype.rawListeners = eventEmitterRawListeners;
  EventEmitter.prototype.listenerCount = eventEmitterListenerCount;
  EventEmitter.prototype.eventNames = eventEmitterEventNames;
  EventEmitter.prototype.setMaxListeners = eventEmitterSetMaxListeners;
  EventEmitter.prototype.getMaxListeners = eventEmitterGetMaxListeners;
  EventEmitter.listenerCount = (emitter, name) => emitter.listenerCount(name);

  const onceEvent = (emitter, name, options) =>
    new Promise((resolve, reject) => {
      const signal = options && options.signal;
      let settled = false;
      const usesEventTarget =
        typeof emitter.on !== 'function' &&
        typeof emitter.addEventListener === 'function';
      const remove = (eventName, listener) => {
        if (usesEventTarget) emitter.removeEventListener(eventName, listener);
        else emitter.removeListener(eventName, listener);
      };
      const add = (eventName, listener) => {
        if (usesEventTarget) {
          emitter.addEventListener(eventName, listener, { once: true });
        } else {
          emitter.once(eventName, listener);
        }
      };
      const cleanup = () => {
        remove(name, eventListener);
        if (!usesEventTarget && name !== 'error')
          remove('error', errorListener);
        if (signal) signal.removeEventListener('abort', abortListener);
      };
      const eventListener = (...arguments_) => {
        if (settled) return;
        settled = true;
        cleanup();
        resolve(arguments_);
      };
      const errorListener = (error) => {
        if (settled) return;
        settled = true;
        cleanup();
        reject(error);
      };
      const abortListener = () => {
        if (settled) return;
        settled = true;
        cleanup();
        reject(createAbortError(signal.reason));
      };
      if (signal && signal.aborted) {
        abortListener();
        return;
      }
      add(name, eventListener);
      if (!usesEventTarget && name !== 'error') add('error', errorListener);
      if (signal)
        signal.addEventListener('abort', abortListener, { once: true });
    });

  const addAbortListener = (signal, listener) => {
    if (!(signal instanceof AbortSignal)) {
      throw createError(
        'ERR_INVALID_ARG_TYPE',
        'signal must be an AbortSignal'
      );
    }
    validateEventListener(listener);
    if (signal.aborted) listener();
    else signal.addEventListener('abort', listener, { once: true });
    const disposeSymbol = Symbol.dispose ?? Symbol.for('nodejs.dispose');
    return {
      [disposeSymbol]: () => signal.removeEventListener('abort', listener),
    };
  };

  const eventsModule = Object.freeze({
    default: EventEmitter,
    EventEmitter,
    once: onceEvent,
    addAbortListener,
    getEventListeners: (emitter, name) =>
      typeof emitter.listeners === 'function'
        ? emitter.listeners(name)
        : (emitter[eventTargetListeners]?.get(String(name)) ?? []).map(
            (entry) => entry.listener
          ),
    getMaxListeners: (emitter) =>
      typeof emitter.getMaxListeners === 'function'
        ? emitter.getMaxListeners()
        : EventEmitter.defaultMaxListeners,
    listenerCount: (emitter, name) => emitter.listenerCount(name),
    setMaxListeners: (maximum, ...emitters) => {
      if (emitters.length === 0) EventEmitter.defaultMaxListeners = maximum;
      else for (const emitter of emitters) emitter.setMaxListeners(maximum);
    },
  });

  const normalizePath = (value) => {
    const source = String(value).replaceAll('\\', '/');
    const absolute = source.startsWith('/');
    const parts = [];
    for (const part of source.split('/')) {
      if (part === '' || part === '.') continue;
      if (part === '..') {
        if (parts.length > 0 && parts[parts.length - 1] !== '..') {
          parts.pop();
        } else if (!absolute) {
          parts.push(part);
        }
      } else {
        parts.push(part);
      }
    }
    const normalized = `${absolute ? '/' : ''}${parts.join('/')}`;
    return normalized || (absolute ? '/' : '.');
  };

  const pathModule = Object.freeze({
    sep: '/',
    delimiter: ':',
    normalize: (path) => normalizePath(path),
    isAbsolute: (path) => String(path).startsWith('/'),
    join: (...parts) =>
      normalizePath(parts.filter((part) => String(part).length > 0).join('/')),
    resolve: (...parts) =>
      `/${normalizePath(parts.join('/')).replace(/^\/+/, '')}`,
    basename: (path, suffix) => {
      const name = normalizePath(path).split('/').filter(Boolean).at(-1) ?? '';
      return suffix && name.endsWith(suffix)
        ? name.slice(0, -suffix.length)
        : name;
    },
    dirname: (path) => {
      const normalized = normalizePath(path);
      const separator = normalized.lastIndexOf('/');
      return separator < 0 ? '.' : normalized.slice(0, separator) || '/';
    },
    extname: (path) => {
      const name = pathModule.basename(path);
      const dot = name.lastIndexOf('.');
      return dot <= 0 ? '' : name.slice(dot);
    },
    relative: (from, to) => {
      const first = normalizePath(from).split('/').filter(Boolean);
      const second = normalizePath(to).split('/').filter(Boolean);
      let common = 0;
      while (common < first.length && first[common] === second[common])
        common += 1;
      return (
        [...first.slice(common).map(() => '..'), ...second.slice(common)].join(
          '/'
        ) || ''
      );
    },
  });

  const urlState = Symbol('muon.url.state');
  const urlSearchParamsPairs = Symbol('muon.urlSearchParams.pairs');
  const urlSearchParamsUpdate = Symbol('muon.urlSearchParams.update');
  const urlSearchParamsInternal = {};
  const defaultUrlPorts = Object.freeze({
    ftp: '21',
    http: '80',
    https: '443',
    ws: '80',
    wss: '443',
  });
  const authorityUrlSchemes = new Set([
    'file',
    'ftp',
    'http',
    'https',
    'ws',
    'wss',
  ]);

  const decodeUrlComponent = (value, label) => {
    try {
      return decodeURIComponent(value);
    } catch {
      throw createError(
        'ERR_INVALID_URL',
        `Invalid percent escape in ${label}`
      );
    }
  };

  const encodeUrlText = (value, safeCharacters) => {
    const source = String(value);
    let result = '';
    for (let index = 0; index < source.length; ) {
      const point = source.codePointAt(index);
      const character = String.fromCodePoint(point);
      if (
        /^[A-Za-z0-9]$/.test(character) ||
        safeCharacters.includes(character)
      ) {
        result += character;
      } else if (
        character === '%' &&
        /^[0-9A-Fa-f]{2}$/.test(source.slice(index + 1, index + 3))
      ) {
        result += `%${source.slice(index + 1, index + 3).toUpperCase()}`;
        index += 2;
      } else {
        result += encodeURIComponent(character);
      }
      index += character.length;
    }
    return result;
  };

  const encodeUrlPath = (value) => encodeUrlText(value, "-._~!$&'()*+,;=:@/");
  const encodeUrlQuery = (value) => encodeUrlText(value, "-._~!$&'()*+,;=:@/?");
  const encodeUrlHash = (value) => encodeUrlText(value, "-._~!$&'()*+,;=:@/?#");
  const encodeUrlCredential = (value) =>
    encodeUrlText(value, "-._~!$&'()*+,;=");
  const encodeUrlSearchParameter = (value) =>
    encodeURIComponent(String(value))
      .replaceAll('%20', '+')
      .replace(
        /[!'()~]/g,
        (character) => `%${character.charCodeAt(0).toString(16).toUpperCase()}`
      );

  const normalizeUrlPath = (value) => {
    const source = String(value);
    const absolute = source.startsWith('/');
    const trailingSlash = source.endsWith('/');
    const parts = [];
    for (const part of source.split('/')) {
      const lower = part.toLowerCase();
      if (part === '' || lower === '.' || lower === '%2e') continue;
      if (
        lower === '..' ||
        lower === '.%2e' ||
        lower === '%2e.' ||
        lower === '%2e%2e'
      ) {
        if (parts.length > 0) parts.pop();
        continue;
      }
      parts.push(part);
    }
    const joined = parts.join('/');
    const normalized = `${absolute ? '/' : ''}${joined}`;
    if (trailingSlash && normalized !== '/' && normalized !== '') {
      return `${normalized}/`;
    }
    return normalized || (absolute ? '/' : '');
  };

  const splitUrlSuffix = (value) => {
    const hashIndex = value.indexOf('#');
    const beforeHash = hashIndex < 0 ? value : value.slice(0, hashIndex);
    const hash = hashIndex < 0 ? '' : value.slice(hashIndex + 1);
    const queryIndex = beforeHash.indexOf('?');
    return {
      path: queryIndex < 0 ? beforeHash : beforeHash.slice(0, queryIndex),
      query: queryIndex < 0 ? '' : beforeHash.slice(queryIndex + 1),
      hasQuery: queryIndex >= 0,
      hash,
      hasHash: hashIndex >= 0,
    };
  };

  const parseUrlHost = (value, scheme) => {
    let authority = String(value);
    let username = '';
    let password = '';
    const at = authority.lastIndexOf('@');
    if (at >= 0) {
      const credentials = authority.slice(0, at);
      authority = authority.slice(at + 1);
      const colon = credentials.indexOf(':');
      username = decodeUrlComponent(
        colon < 0 ? credentials : credentials.slice(0, colon),
        'URL username'
      );
      password = decodeUrlComponent(
        colon < 0 ? '' : credentials.slice(colon + 1),
        'URL password'
      );
    }

    let hostname = '';
    let port = '';
    if (authority.startsWith('[')) {
      const close = authority.indexOf(']');
      if (close < 0) {
        throw createError('ERR_INVALID_URL', 'IPv6 URL host is missing ]');
      }
      hostname = authority.slice(1, close).toLowerCase();
      const remainder = authority.slice(close + 1);
      if (remainder !== '') {
        if (!remainder.startsWith(':')) {
          throw createError('ERR_INVALID_URL', 'Invalid IPv6 URL host');
        }
        port = remainder.slice(1);
      }
      if (!/^[0-9A-Fa-f:.]+$/.test(hostname)) {
        throw createError('ERR_INVALID_URL', 'Invalid IPv6 URL host');
      }
    } else {
      const colon = authority.lastIndexOf(':');
      if (colon >= 0) {
        hostname = authority.slice(0, colon);
        port = authority.slice(colon + 1);
      } else {
        hostname = authority;
      }
      hostname = hostname.toLowerCase();
      if (hostname !== '' && !/^[A-Za-z0-9.-]+$/.test(hostname)) {
        throw createError(
          'ERR_MUON_JS_UNSUPPORTED_HOSTNAME',
          'Only ASCII, IPv4, and IPv6 URL hostnames are supported'
        );
      }
    }
    if (port !== '') {
      if (!/^[0-9]+$/.test(port) || Number(port) > 65535) {
        throw createError('ERR_INVALID_URL', 'Invalid URL port');
      }
      port = String(Number(port));
      if (defaultUrlPorts[scheme] === port) port = '';
    }
    if (scheme !== 'file' && hostname === '') {
      throw createError('ERR_INVALID_URL', 'URL hostname is required');
    }
    return { username, password, hostname, port };
  };

  const cloneUrlState = (state) => ({
    scheme: state.scheme,
    hasAuthority: state.hasAuthority,
    username: state.username,
    password: state.password,
    hostname: state.hostname,
    port: state.port,
    pathname: state.pathname,
    query: state.query,
    hasQuery: state.hasQuery,
    hash: state.hash,
    hasHash: state.hasHash,
    searchParams: undefined,
  });

  const parseAbsoluteUrlState = (value) => {
    const match = /^([A-Za-z][A-Za-z0-9+.-]*):(.*)$/.exec(value);
    if (!match) throw createError('ERR_INVALID_URL', 'URL scheme is required');
    const scheme = match[1].toLowerCase();
    let remainder = match[2];
    const hasAuthority = remainder.startsWith('//');
    if (authorityUrlSchemes.has(scheme) && !hasAuthority) {
      if (scheme !== 'file') {
        throw createError('ERR_INVALID_URL', `${scheme}: URL requires //`);
      }
      remainder = `//${remainder}`;
    }

    let host = { username: '', password: '', hostname: '', port: '' };
    let suffixSource = remainder;
    if (remainder.startsWith('//')) {
      suffixSource = remainder.slice(2);
      const boundary = suffixSource.search(/[/?#]/);
      const authority =
        boundary < 0 ? suffixSource : suffixSource.slice(0, boundary);
      suffixSource = boundary < 0 ? '' : suffixSource.slice(boundary);
      host = parseUrlHost(authority, scheme);
    }
    const suffix = splitUrlSuffix(suffixSource);
    let pathname = normalizeUrlPath(encodeUrlPath(suffix.path));
    if (hasAuthority && pathname === '') pathname = '/';
    return {
      scheme,
      hasAuthority: remainder.startsWith('//'),
      ...host,
      pathname,
      query: encodeUrlQuery(suffix.query),
      hasQuery: suffix.hasQuery,
      hash: encodeUrlHash(suffix.hash),
      hasHash: suffix.hasHash,
      searchParams: undefined,
    };
  };

  const parseUrlState = (value, base) => {
    const source = String(value).trim();
    if (/^[A-Za-z][A-Za-z0-9+.-]*:/.test(source)) {
      return parseAbsoluteUrlState(source);
    }
    if (base === undefined) {
      throw createError('ERR_INVALID_URL', 'Relative URL requires a base URL');
    }
    const baseState =
      base && typeof base === 'object' && base[urlState]
        ? base[urlState]
        : parseAbsoluteUrlState(String(base).trim());
    if (source.startsWith('//')) {
      return parseAbsoluteUrlState(`${baseState.scheme}:${source}`);
    }
    const result = cloneUrlState(baseState);
    const suffix = splitUrlSuffix(source);
    if (source.startsWith('#')) {
      result.hash = encodeUrlHash(suffix.hash);
      result.hasHash = true;
      return result;
    }
    if (source.startsWith('?')) {
      result.query = encodeUrlQuery(suffix.query);
      result.hasQuery = true;
      result.hash = encodeUrlHash(suffix.hash);
      result.hasHash = suffix.hasHash;
      return result;
    }
    if (suffix.path.startsWith('/')) {
      result.pathname = normalizeUrlPath(encodeUrlPath(suffix.path));
    } else if (suffix.path !== '') {
      const slash = result.pathname.lastIndexOf('/');
      const directory = slash < 0 ? '' : result.pathname.slice(0, slash + 1);
      result.pathname = normalizeUrlPath(
        encodeUrlPath(`${directory}${suffix.path}`)
      );
    }
    result.query = encodeUrlQuery(suffix.query);
    result.hasQuery = suffix.hasQuery;
    result.hash = encodeUrlHash(suffix.hash);
    result.hasHash = suffix.hasHash;
    return result;
  };

  const serializeUrlHost = (state) => {
    const hostname = state.hostname.includes(':')
      ? `[${state.hostname}]`
      : state.hostname;
    return `${hostname}${state.port === '' ? '' : `:${state.port}`}`;
  };

  const serializeUrlState = (state) => {
    let result = `${state.scheme}:`;
    if (state.hasAuthority) {
      result += '//';
      if (state.username !== '' || state.password !== '') {
        result += encodeUrlCredential(state.username);
        if (state.password !== '') {
          result += `:${encodeUrlCredential(state.password)}`;
        }
        result += '@';
      }
      result += serializeUrlHost(state);
    }
    result += state.pathname;
    if (state.hasQuery) result += `?${state.query}`;
    if (state.hasHash) result += `#${state.hash}`;
    return result;
  };

  const notifyUrlSearchParams = (parameters) => {
    const update = parameters[urlSearchParamsUpdate];
    if (typeof update === 'function') update(parameters.toString());
  };

  const parseUrlSearchParameters = (value) => {
    const source = String(value).replace(/^\?/, '');
    if (source === '') return [];
    return source.split('&').map((entry) => {
      const equals = entry.indexOf('=');
      const name = equals < 0 ? entry : entry.slice(0, equals);
      const content = equals < 0 ? '' : entry.slice(equals + 1);
      return [
        decodeUrlComponent(name.replaceAll('+', ' '), 'search parameter'),
        decodeUrlComponent(content.replaceAll('+', ' '), 'search parameter'),
      ];
    });
  };

  const URLSearchParams = function (initial, internalKey, update) {
    if (!(this instanceof URLSearchParams)) {
      throw new TypeError('URLSearchParams constructor requires new');
    }
    let pairs;
    if (internalKey === urlSearchParamsInternal) {
      pairs = parseUrlSearchParameters(initial ?? '');
    } else if (initial === undefined) {
      pairs = [];
    } else if (typeof initial === 'string') {
      pairs = parseUrlSearchParameters(initial);
    } else if (initial !== null && initial[Symbol.iterator]) {
      pairs = [];
      for (const entry of initial) {
        const tuple = [...entry];
        if (tuple.length !== 2) {
          throw createError(
            'ERR_INVALID_TUPLE',
            'Each query pair must contain exactly two values'
          );
        }
        pairs.push([String(tuple[0]), String(tuple[1])]);
      }
    } else if (initial !== null && typeof initial === 'object') {
      pairs = Object.keys(initial).map((name) => [
        String(name),
        String(initial[name]),
      ]);
    } else {
      pairs = parseUrlSearchParameters(String(initial));
    }
    Object.defineProperties(this, {
      [urlSearchParamsPairs]: { value: pairs, writable: true },
      [urlSearchParamsUpdate]: {
        value: internalKey === urlSearchParamsInternal ? update : undefined,
        writable: true,
      },
    });
  };

  const requireUrlSearchParamsPairs = (parameters) => {
    const pairs = parameters[urlSearchParamsPairs];
    if (!Array.isArray(pairs)) {
      throw createError(
        'ERR_INVALID_THIS',
        'URLSearchParams method called on an incompatible receiver'
      );
    }
    return pairs;
  };

  const urlSearchParamsAppend = function (name, value) {
    requireUrlSearchParamsPairs(this).push([String(name), String(value)]);
    notifyUrlSearchParams(this);
  };
  const urlSearchParamsDelete = function (name, value) {
    const normalizedName = String(name);
    const hasValue = value !== undefined;
    const normalizedValue = String(value);
    this[urlSearchParamsPairs] = requireUrlSearchParamsPairs(this).filter(
      (entry) =>
        entry[0] !== normalizedName ||
        (hasValue && entry[1] !== normalizedValue)
    );
    notifyUrlSearchParams(this);
  };
  const urlSearchParamsGet = function (name) {
    const normalized = String(name);
    return (
      requireUrlSearchParamsPairs(this).find(
        (entry) => entry[0] === normalized
      )?.[1] ?? null
    );
  };
  const urlSearchParamsGetAll = function (name) {
    const normalized = String(name);
    return requireUrlSearchParamsPairs(this)
      .filter((entry) => entry[0] === normalized)
      .map((entry) => entry[1]);
  };
  const urlSearchParamsHas = function (name, value) {
    const normalizedName = String(name);
    const hasValue = value !== undefined;
    const normalizedValue = String(value);
    return requireUrlSearchParamsPairs(this).some(
      (entry) =>
        entry[0] === normalizedName &&
        (!hasValue || entry[1] === normalizedValue)
    );
  };
  const urlSearchParamsSet = function (name, value) {
    const normalizedName = String(name);
    const normalizedValue = String(value);
    const pairs = requireUrlSearchParamsPairs(this);
    const first = pairs.findIndex((entry) => entry[0] === normalizedName);
    if (first < 0) {
      pairs.push([normalizedName, normalizedValue]);
    } else {
      pairs[first][1] = normalizedValue;
      for (let index = pairs.length - 1; index > first; index -= 1) {
        if (pairs[index][0] === normalizedName) pairs.splice(index, 1);
      }
    }
    notifyUrlSearchParams(this);
  };
  const urlSearchParamsSort = function () {
    requireUrlSearchParamsPairs(this).sort((left, right) =>
      left[0] < right[0] ? -1 : left[0] > right[0] ? 1 : 0
    );
    notifyUrlSearchParams(this);
  };
  const urlSearchParamsEntries = function () {
    return requireUrlSearchParamsPairs(this)
      .map((entry) => [...entry])
      [Symbol.iterator]();
  };
  const urlSearchParamsKeys = function () {
    return requireUrlSearchParamsPairs(this)
      .map((entry) => entry[0])
      [Symbol.iterator]();
  };
  const urlSearchParamsValues = function () {
    return requireUrlSearchParamsPairs(this)
      .map((entry) => entry[1])
      [Symbol.iterator]();
  };
  const urlSearchParamsForEach = function (callback, thisArgument) {
    if (typeof callback !== 'function') {
      throw createError('ERR_INVALID_ARG_TYPE', 'callback must be a function');
    }
    for (const [name, value] of requireUrlSearchParamsPairs(this)) {
      Reflect.apply(callback, thisArgument, [value, name, this]);
    }
  };
  const urlSearchParamsToString = function () {
    return requireUrlSearchParamsPairs(this)
      .map(
        ([name, value]) =>
          `${encodeUrlSearchParameter(name)}=${encodeUrlSearchParameter(value)}`
      )
      .join('&');
  };

  URLSearchParams.prototype.append = urlSearchParamsAppend;
  URLSearchParams.prototype.delete = urlSearchParamsDelete;
  URLSearchParams.prototype.get = urlSearchParamsGet;
  URLSearchParams.prototype.getAll = urlSearchParamsGetAll;
  URLSearchParams.prototype.has = urlSearchParamsHas;
  URLSearchParams.prototype.set = urlSearchParamsSet;
  URLSearchParams.prototype.sort = urlSearchParamsSort;
  URLSearchParams.prototype.entries = urlSearchParamsEntries;
  URLSearchParams.prototype.keys = urlSearchParamsKeys;
  URLSearchParams.prototype.values = urlSearchParamsValues;
  URLSearchParams.prototype.forEach = urlSearchParamsForEach;
  URLSearchParams.prototype.toString = urlSearchParamsToString;
  URLSearchParams.prototype[Symbol.iterator] = urlSearchParamsEntries;
  Object.defineProperties(URLSearchParams.prototype, {
    size: {
      get: function () {
        return requireUrlSearchParamsPairs(this).length;
      },
    },
    [Symbol.toStringTag]: { value: 'URLSearchParams' },
  });

  const attachUrlSearchParams = (state) => {
    const parameters = new URLSearchParams(
      state.hasQuery ? state.query : '',
      urlSearchParamsInternal,
      (query) => {
        state.query = query;
        state.hasQuery = query !== '';
      }
    );
    state.searchParams = parameters;
  };

  const replaceUrlState = (target, state) => {
    attachUrlSearchParams(state);
    target[urlState] = state;
  };

  const URL = function (input, base) {
    if (!(this instanceof URL)) {
      throw new TypeError('URL constructor requires new');
    }
    Object.defineProperty(this, urlState, {
      value: undefined,
      configurable: false,
      enumerable: false,
      writable: true,
    });
    replaceUrlState(this, parseUrlState(input, base));
  };

  const requireUrlState = (url) => {
    const state = url[urlState];
    if (!state) {
      throw createError(
        'ERR_INVALID_THIS',
        'URL method called on an incompatible receiver'
      );
    }
    return state;
  };

  Object.defineProperties(URL.prototype, {
    href: {
      get: function () {
        return serializeUrlState(requireUrlState(this));
      },
      set: function (value) {
        replaceUrlState(this, parseUrlState(value, undefined));
      },
    },
    protocol: {
      get: function () {
        return `${requireUrlState(this).scheme}:`;
      },
      set: function (value) {
        const match = /^([A-Za-z][A-Za-z0-9+.-]*):?$/.exec(String(value));
        if (!match)
          throw createError('ERR_INVALID_URL', 'Invalid URL protocol');
        requireUrlState(this).scheme = match[1].toLowerCase();
      },
    },
    username: {
      get: function () {
        return requireUrlState(this).username;
      },
      set: function (value) {
        requireUrlState(this).username = String(value);
      },
    },
    password: {
      get: function () {
        return requireUrlState(this).password;
      },
      set: function (value) {
        requireUrlState(this).password = String(value);
      },
    },
    hostname: {
      get: function () {
        return requireUrlState(this).hostname;
      },
      set: function (value) {
        const state = requireUrlState(this);
        const parsed = parseUrlHost(
          `${String(value)}${state.port === '' ? '' : `:${state.port}`}`,
          state.scheme
        );
        state.hostname = parsed.hostname;
      },
    },
    port: {
      get: function () {
        return requireUrlState(this).port;
      },
      set: function (value) {
        const state = requireUrlState(this);
        const parsed = parseUrlHost(
          `${state.hostname.includes(':') ? `[${state.hostname}]` : state.hostname}:${String(value)}`,
          state.scheme
        );
        state.port = parsed.port;
      },
    },
    host: {
      get: function () {
        return serializeUrlHost(requireUrlState(this));
      },
      set: function (value) {
        const state = requireUrlState(this);
        const parsed = parseUrlHost(value, state.scheme);
        state.hostname = parsed.hostname;
        state.port = parsed.port;
      },
    },
    origin: {
      get: function () {
        const state = requireUrlState(this);
        return ['ftp', 'http', 'https', 'ws', 'wss'].includes(state.scheme)
          ? `${state.scheme}://${serializeUrlHost(state)}`
          : 'null';
      },
    },
    pathname: {
      get: function () {
        return requireUrlState(this).pathname;
      },
      set: function (value) {
        requireUrlState(this).pathname = normalizeUrlPath(encodeUrlPath(value));
      },
    },
    search: {
      get: function () {
        const state = requireUrlState(this);
        return state.hasQuery ? `?${state.query}` : '';
      },
      set: function (value) {
        const state = requireUrlState(this);
        const source = String(value).replace(/^\?/, '');
        state.query = encodeUrlQuery(source);
        state.hasQuery = source !== '';
        state.searchParams[urlSearchParamsPairs] = parseUrlSearchParameters(
          state.query
        );
      },
    },
    searchParams: {
      get: function () {
        return requireUrlState(this).searchParams;
      },
    },
    hash: {
      get: function () {
        const state = requireUrlState(this);
        return state.hasHash ? `#${state.hash}` : '';
      },
      set: function (value) {
        const state = requireUrlState(this);
        const source = String(value).replace(/^#/, '');
        state.hash = encodeUrlHash(source);
        state.hasHash = source !== '';
      },
    },
    [Symbol.toStringTag]: { value: 'URL' },
  });
  URL.prototype.toString = function () {
    return serializeUrlState(requireUrlState(this));
  };
  URL.prototype.toJSON = URL.prototype.toString;
  URL.canParse = (input, base) => {
    try {
      parseUrlState(input, base);
      return true;
    } catch {
      return false;
    }
  };
  URL.parse = (input, base) => {
    try {
      return new URL(input, base);
    } catch {
      return null;
    }
  };

  const pathToFileURL = (path) => {
    const source = String(path);
    if (!source.startsWith('/')) {
      throw createError(
        'ERR_MUON_JS_RELATIVE_FILE_URL',
        'pathToFileURL requires an absolute POSIX path'
      );
    }
    return new URL(`file://${encodeUrlPath(normalizePath(source))}`);
  };

  const fileURLToPath = (value) => {
    const url = value instanceof URL ? value : new URL(value);
    if (url.protocol !== 'file:') {
      throw createError('ERR_INVALID_URL_SCHEME', 'URL must use file:');
    }
    if (url.hostname !== '' && url.hostname !== 'localhost') {
      throw createError(
        'ERR_INVALID_FILE_URL_HOST',
        'POSIX file URL host must be empty or localhost'
      );
    }
    if (/%2f|%5c/i.test(url.pathname)) {
      throw createError(
        'ERR_INVALID_FILE_URL_PATH',
        'Encoded path separators are not permitted in file URLs'
      );
    }
    return decodeUrlComponent(url.pathname, 'file URL path');
  };

  const urlToHttpOptions = (value) => {
    const url = value instanceof URL ? value : new URL(value);
    const options = {
      protocol: url.protocol,
      hostname: url.hostname,
      hash: url.hash,
      search: url.search,
      pathname: url.pathname,
      path: `${url.pathname}${url.search}`,
      href: url.href,
    };
    if (url.port !== '') options.port = Number(url.port);
    if (url.username !== '' || url.password !== '') {
      options.auth = `${url.username}:${url.password}`;
    }
    return options;
  };

  const urlModule = Object.freeze({
    URL,
    URLSearchParams,
    fileURLToPath,
    pathToFileURL,
    urlToHttpOptions,
    format: (value) =>
      value instanceof URL ? value.href : new URL(value).href,
  });

  const readEncoding = (options) =>
    typeof options === 'string'
      ? options
      : options && typeof options === 'object'
        ? options.encoding
        : undefined;

  const fsPromises = Object.freeze({
    readFile: async (path, options) => {
      const encoding = readEncoding(options);
      if (encoding === 'utf8' || encoding === 'utf-8') {
        return __muonFsReadText(String(path));
      }
      if (encoding !== undefined && encoding !== null) {
        throw createError(
          'ERR_MUON_JS_UNSUPPORTED_ENCODING',
          `Unsupported encoding: ${encoding}`
        );
      }
      return new Uint8Array(__muonFsReadBuffer(String(path)));
    },
    writeFile: async (path, data) => {
      if (typeof data === 'string') {
        __muonFsWriteText(String(path), data);
      } else if (data instanceof Uint8Array) {
        const exact =
          data.byteOffset === 0 && data.byteLength === data.buffer.byteLength
            ? data.buffer
            : data.buffer.slice(
                data.byteOffset,
                data.byteOffset + data.byteLength
              );
        __muonFsWriteBuffer(String(path), exact);
      } else if (data instanceof ArrayBuffer) {
        __muonFsWriteBuffer(String(path), data);
      } else {
        throw createError(
          'ERR_MUON_JS_UNSUPPORTED_VALUE',
          'writeFile data must be text or bytes'
        );
      }
    },
    mkdir: async (path, options) =>
      __muonFsMkdir(String(path), Boolean(options && options.recursive)),
    readdir: async (path) => __muonFsReaddir(String(path)),
    stat: async (path) => __muonFsStat(String(path)),
    lstat: async (path) => __muonFsStat(String(path)),
    access: async (path) => __muonFsAccess(String(path)),
    rename: async (from, to) => __muonFsRename(String(from), String(to)),
    unlink: async (path) => __muonFsUnlink(String(path)),
    rm: async (path, options) =>
      __muonFsRm(
        String(path),
        Boolean(options && options.recursive),
        Boolean(options && options.force)
      ),
  });

  const callbackError = (error) => ({
    name: 'Error',
    message: error instanceof Error ? error.message : String(error),
    code:
      error && typeof error === 'object' && typeof error.code === 'string'
        ? error.code
        : 'ERR_MUON_JS_FS',
  });

  const invokeFsCallback = async (operation, callback) => {
    if (typeof callback !== 'function') {
      throw new TypeError('A filesystem callback is required');
    }
    try {
      const value = await operation();
      await callback(null, value);
    } catch (error) {
      await callback(callbackError(error));
    }
  };

  const fsCallbacks = Object.freeze({
    readFile: async (path, options, callback) => {
      if (typeof options === 'function') {
        callback = options;
        options = undefined;
      }
      await invokeFsCallback(
        () => fsPromises.readFile(path, options),
        callback
      );
    },
    writeFile: async (path, data, callback) =>
      await invokeFsCallback(() => fsPromises.writeFile(path, data), callback),
    mkdir: async (path, options, callback) => {
      if (typeof options === 'function') {
        callback = options;
        options = undefined;
      }
      await invokeFsCallback(() => fsPromises.mkdir(path, options), callback);
    },
    readdir: async (path, callback) =>
      await invokeFsCallback(() => fsPromises.readdir(path), callback),
    stat: async (path, callback) =>
      await invokeFsCallback(() => fsPromises.stat(path), callback),
    lstat: async (path, callback) =>
      await invokeFsCallback(() => fsPromises.lstat(path), callback),
    access: async (path, callback) =>
      await invokeFsCallback(() => fsPromises.access(path), callback),
    rename: async (from, to, callback) =>
      await invokeFsCallback(() => fsPromises.rename(from, to), callback),
    unlink: async (path, callback) =>
      await invokeFsCallback(() => fsPromises.unlink(path), callback),
    rm: async (path, options, callback) => {
      if (typeof options === 'function') {
        callback = options;
        options = undefined;
      }
      await invokeFsCallback(() => fsPromises.rm(path, options), callback);
    },
  });

  let nextTimerIdentifier = 1;
  const activeTimers = new Map();
  const timerHandleState = Symbol('muon.timer.state');

  const normalizeTimerDelay = (delay, fallback) => {
    const numeric = delay === undefined ? fallback : Number(delay);
    if (!Number.isFinite(numeric) || numeric < 0) return 1;
    const normalized = Math.trunc(numeric);
    if (normalized > 60000) {
      throw createError(
        'ERR_OUT_OF_RANGE',
        'Timer delay must be between 0 and 60000 ms'
      );
    }
    return normalized === 0 && fallback > 0 ? fallback : normalized;
  };

  const scheduleTimerState = (state) =>
    __muonScheduleTimer(state.identifier, state.delay);

  const dispatchTimer = (identifier) => {
    const state = activeTimers.get(Number(identifier));
    if (!state || !state.active) return;
    try {
      if (!state.repeating) {
        state.active = false;
        activeTimers.delete(state.identifier);
      }
      Reflect.apply(state.callback, state.handle, state.arguments);
      if (state.repeating && state.active) scheduleTimerState(state);
    } catch (error) {
      const diagnostic = new Error(
        `Timer ${state.identifier} callback failed: ${
          error instanceof Error ? error.message : String(error)
        }`
      );
      diagnostic.cause = error;
      throw diagnostic;
    }
  };

  const cancelTimer = (handle) => {
    const state =
      handle && typeof handle === 'object'
        ? handle[timerHandleState]
        : activeTimers.get(Number(handle));
    if (!state || !state.active) return;
    state.active = false;
    activeTimers.delete(state.identifier);
    __muonCancelTimer(state.identifier);
  };

  const createTimerHandle = (state) => {
    const handle = {};
    Object.defineProperty(handle, timerHandleState, {
      value: state,
      configurable: false,
      enumerable: false,
      writable: false,
    });
    handle.hasRef = () => state.referenced;
    handle.ref = () => {
      state.referenced = true;
      return handle;
    };
    handle.unref = () => {
      state.referenced = false;
      return handle;
    };
    handle.refresh = () => {
      if (state.active) __muonCancelTimer(state.identifier);
      else {
        state.active = true;
        activeTimers.set(state.identifier, state);
      }
      scheduleTimerState(state);
      return handle;
    };
    handle.close = () => {
      cancelTimer(handle);
      return handle;
    };
    handle[Symbol.toPrimitive] = () => state.identifier;
    const disposeSymbol = Symbol.dispose ?? Symbol.for('nodejs.dispose');
    handle[disposeSymbol] = () => cancelTimer(handle);
    return handle;
  };

  const createTimer = (callback, delay, repeating, arguments_, fallback) => {
    if (typeof callback !== 'function') {
      throw createError(
        'ERR_INVALID_ARG_TYPE',
        'Timer callback must be a function'
      );
    }
    const state = {
      identifier: nextTimerIdentifier++,
      delay: normalizeTimerDelay(delay, fallback),
      repeating,
      callback,
      arguments: arguments_,
      active: true,
      referenced: true,
      handle: undefined,
    };
    state.handle = createTimerHandle(state);
    activeTimers.set(state.identifier, state);
    scheduleTimerState(state);
    return state.handle;
  };

  const setCallbackTimeout = (callback, delay, ...arguments_) =>
    createTimer(callback, delay, false, arguments_, 1);
  const clearCallbackTimeout = (handle) => cancelTimer(handle);
  const setCallbackInterval = (callback, delay, ...arguments_) =>
    createTimer(callback, delay, true, arguments_, 1);
  const clearCallbackInterval = (handle) => cancelTimer(handle);
  const setCallbackImmediate = (callback, ...arguments_) =>
    createTimer(callback, 0, false, arguments_, 0);
  const clearCallbackImmediate = (handle) => cancelTimer(handle);

  const setPromiseTimeout = async (delay, value, options) => {
    const normalizedOptions = options ?? {};
    if (typeof normalizedOptions !== 'object') {
      throw createError(
        'ERR_INVALID_ARG_TYPE',
        'Timer options must be an object'
      );
    }
    const signal = normalizedOptions.signal;
    if (signal !== undefined && !(signal instanceof AbortSignal)) {
      throw createError(
        'ERR_INVALID_ARG_TYPE',
        'signal must be an AbortSignal'
      );
    }
    if (signal && signal.aborted) throw createAbortError(signal.reason);
    return await new Promise((resolve, reject) => {
      let handle;
      const cleanup = () => {
        if (signal) signal.removeEventListener('abort', abortListener);
      };
      const abortListener = () => {
        clearCallbackTimeout(handle);
        cleanup();
        reject(createAbortError(signal.reason));
      };
      handle = setCallbackTimeout(() => {
        cleanup();
        resolve(value);
      }, delay);
      if (normalizedOptions.ref === false) handle.unref();
      if (signal)
        signal.addEventListener('abort', abortListener, { once: true });
    });
  };

  const setPromiseImmediate = async (value, options) =>
    await setPromiseTimeout(0, value, options);

  const timersModule = Object.freeze({
    setTimeout: setCallbackTimeout,
    clearTimeout: clearCallbackTimeout,
    setInterval: setCallbackInterval,
    clearInterval: clearCallbackInterval,
    setImmediate: setCallbackImmediate,
    clearImmediate: clearCallbackImmediate,
  });

  const timersPromises = Object.freeze({
    setTimeout: setPromiseTimeout,
    setImmediate: setPromiseImmediate,
  });

  const readableStreamState = Symbol('muon.stream.readableState');
  const writableStreamState = Symbol('muon.stream.writableState');

  const Stream = function () {
    if (!(this instanceof Stream)) return new Stream();
    EventEmitter.call(this);
  };
  Stream.prototype = Object.create(EventEmitter.prototype);
  Object.defineProperty(Stream.prototype, 'constructor', {
    value: Stream,
    configurable: true,
    enumerable: false,
    writable: true,
  });

  const streamChunkSize = (chunk, objectMode) =>
    objectMode ? 1 : chunk.byteLength;

  const normalizeStreamChunk = (chunk, encoding, objectMode) => {
    if (chunk === null) {
      throw createError(
        'ERR_STREAM_NULL_VALUES',
        'Stream chunk cannot be null'
      );
    }
    if (objectMode) return chunk;
    if (typeof chunk === 'string') return Buffer.from(chunk, encoding);
    if (chunk instanceof Uint8Array) return Buffer.from(chunk);
    if (chunk instanceof ArrayBuffer) return Buffer.from(chunk);
    throw createError(
      'ERR_INVALID_ARG_TYPE',
      'Stream chunk must be text, Buffer, Uint8Array, or ArrayBuffer'
    );
  };

  const markStreamDestroyed = (stream, error) => {
    const readable = stream[readableStreamState];
    const writable = stream[writableStreamState];
    if (readable) {
      readable.destroyed = true;
      if (error) readable.errored = error;
    }
    if (writable) {
      writable.destroyed = true;
      if (error) writable.errored = error;
    }
  };

  const streamDestroy = function (error) {
    const readable = this[readableStreamState];
    const writable = this[writableStreamState];
    if (readable?.destroyed || writable?.destroyed) return this;
    markStreamDestroyed(this, error);
    let completed = false;
    const complete = (destroyError) => {
      if (completed) return;
      completed = true;
      const finalError = destroyError ?? error;
      if (finalError) this.emit('error', finalError);
      if (readable) readable.closed = true;
      if (writable) writable.closed = true;
      this.emit('close');
    };
    try {
      this._destroy(error ?? null, complete);
    } catch (destroyError) {
      complete(destroyError);
    }
    return this;
  };

  const streamDefaultDestroy = (error, callback) => callback(error);

  const initializeReadable = (stream, options) => {
    const normalizedOptions = options ?? {};
    const objectMode = Boolean(normalizedOptions.objectMode);
    const highWaterMark =
      normalizedOptions.highWaterMark === undefined
        ? objectMode
          ? 16
          : 16384
        : Number(normalizedOptions.highWaterMark);
    if (!Number.isInteger(highWaterMark) || highWaterMark < 0) {
      throw createError(
        'ERR_INVALID_ARG_VALUE',
        'Readable highWaterMark must be a non-negative integer'
      );
    }
    Object.defineProperty(stream, readableStreamState, {
      value: {
        queue: [],
        length: 0,
        objectMode,
        highWaterMark,
        ended: false,
        endEmitted: false,
        flowing: null,
        reading: false,
        destroyed: false,
        closed: false,
        errored: null,
        pipes: new Map(),
      },
      configurable: false,
      enumerable: false,
      writable: false,
    });
    if (typeof normalizedOptions.read === 'function') {
      stream._read = normalizedOptions.read;
    }
    if (typeof normalizedOptions.destroy === 'function') {
      stream._destroy = normalizedOptions.destroy;
    }
    if (normalizedOptions.signal) {
      setCallbackImmediate(() =>
        addAbortSignal(normalizedOptions.signal, stream)
      );
    }
  };

  const requireReadableState = (stream) => {
    const state = stream[readableStreamState];
    if (!state) {
      throw createError(
        'ERR_INVALID_THIS',
        'Readable method called on an incompatible receiver'
      );
    }
    return state;
  };

  const finishReadable = (stream, state) => {
    if (
      !state.endEmitted &&
      state.ended &&
      state.length === 0 &&
      !state.destroyed
    ) {
      state.endEmitted = true;
      state.flowing = false;
      stream.emit('end');
    }
  };

  const invokeReadableRead = (stream, state) => {
    if (state.reading || state.ended || state.destroyed) return;
    state.reading = true;
    try {
      stream._read(state.highWaterMark);
    } catch (error) {
      stream.destroy(error);
    } finally {
      state.reading = false;
    }
  };

  const shiftReadableChunk = (state) => {
    const chunk = state.queue.shift();
    if (chunk !== undefined) {
      state.length -= streamChunkSize(chunk, state.objectMode);
    }
    return chunk;
  };

  const drainReadable = (stream, state) => {
    while (state.flowing === true && state.queue.length > 0) {
      const chunk = shiftReadableChunk(state);
      stream.emit('data', chunk);
      if (state.destroyed) return;
    }
    if (state.queue.length === 0) {
      finishReadable(stream, state);
      if (!state.ended && state.flowing === true) {
        invokeReadableRead(stream, state);
      }
    }
  };

  const Readable = function (options) {
    if (!(this instanceof Readable)) return new Readable(options);
    Stream.call(this);
    initializeReadable(this, options);
  };
  Readable.prototype = Object.create(Stream.prototype);
  Object.defineProperty(Readable.prototype, 'constructor', {
    value: Readable,
    configurable: true,
    enumerable: false,
    writable: true,
  });
  Readable.prototype._read = () => {};
  Readable.prototype._destroy = streamDefaultDestroy;
  Readable.prototype.destroy = streamDestroy;
  Readable.prototype.push = function (chunk, encoding) {
    const state = requireReadableState(this);
    if (state.destroyed) return false;
    if (chunk === null) {
      if (state.ended) return false;
      state.ended = true;
      this.emit('readable');
      if (state.flowing === true) drainReadable(this, state);
      return false;
    }
    if (state.ended) {
      this.destroy(
        createError('ERR_STREAM_PUSH_AFTER_EOF', 'Cannot push after EOF')
      );
      return false;
    }
    const normalized = normalizeStreamChunk(chunk, encoding, state.objectMode);
    if (!state.objectMode && normalized.byteLength === 0) {
      return state.length < state.highWaterMark;
    }
    if (state.flowing === true && state.length === 0) {
      this.emit('data', normalized);
    } else {
      state.queue.push(normalized);
      state.length += streamChunkSize(normalized, state.objectMode);
      this.emit('readable');
    }
    return state.length < state.highWaterMark;
  };
  Readable.prototype.unshift = function (chunk, encoding) {
    const state = requireReadableState(this);
    const normalized = normalizeStreamChunk(chunk, encoding, state.objectMode);
    state.queue.unshift(normalized);
    state.length += streamChunkSize(normalized, state.objectMode);
    this.emit('readable');
  };
  Readable.prototype.read = function (size) {
    const state = requireReadableState(this);
    if (state.length === 0 && !state.ended) invokeReadableRead(this, state);
    if (state.length === 0) {
      finishReadable(this, state);
      return null;
    }
    if (state.objectMode) {
      const value = shiftReadableChunk(state);
      finishReadable(this, state);
      return value;
    }
    const requested =
      size === undefined || Number.isNaN(Number(size))
        ? state.length
        : Math.max(0, Math.trunc(Number(size)));
    if (requested === 0 || requested > state.length) return null;
    if (requested === state.queue[0].byteLength) {
      const value = shiftReadableChunk(state);
      finishReadable(this, state);
      return value;
    }
    if (requested < state.queue[0].byteLength) {
      const first = state.queue[0];
      const value = first.subarray(0, requested);
      state.queue[0] = first.subarray(requested);
      state.length -= requested;
      return value;
    }
    const result = Buffer.alloc(requested);
    let offset = 0;
    while (offset < requested) {
      const chunk = state.queue[0];
      const count = Math.min(chunk.byteLength, requested - offset);
      Uint8Array.prototype.set.call(result, chunk.subarray(0, count), offset);
      offset += count;
      state.length -= count;
      if (count === chunk.byteLength) state.queue.shift();
      else state.queue[0] = chunk.subarray(count);
    }
    finishReadable(this, state);
    return result;
  };
  Readable.prototype.pause = function () {
    const state = requireReadableState(this);
    state.flowing = false;
    this.emit('pause');
    return this;
  };
  Readable.prototype.resume = function () {
    const state = requireReadableState(this);
    if (state.destroyed) return this;
    const changed = state.flowing !== true;
    state.flowing = true;
    if (changed) this.emit('resume');
    drainReadable(this, state);
    return this;
  };
  Readable.prototype.isPaused = function () {
    return requireReadableState(this).flowing === false;
  };
  Readable.prototype.on = function (name, listener) {
    EventEmitter.prototype.on.call(this, name, listener);
    if (name === 'data') this.resume();
    return this;
  };
  Readable.prototype.addListener = Readable.prototype.on;
  Readable.prototype.pipe = function (destination, options) {
    if (!destination || typeof destination.write !== 'function') {
      throw createError(
        'ERR_INVALID_ARG_TYPE',
        'pipe destination must be writable'
      );
    }
    const state = requireReadableState(this);
    const shouldEnd = !options || options.end !== false;
    const dataListener = (chunk) => {
      if (!destination.write(chunk)) {
        this.pause();
        destination.once('drain', () => this.resume());
      }
    };
    const endListener = () => {
      if (shouldEnd) destination.end();
    };
    const errorListener = (error) => destination.destroy(error);
    state.pipes.set(destination, {
      dataListener,
      endListener,
      errorListener,
    });
    this.on('data', dataListener);
    this.once('end', endListener);
    this.once('error', errorListener);
    destination.emit('pipe', this);
    return destination;
  };
  Readable.prototype.unpipe = function (destination) {
    const state = requireReadableState(this);
    const destinations = destination ? [destination] : [...state.pipes.keys()];
    for (const target of destinations) {
      const listeners = state.pipes.get(target);
      if (!listeners) continue;
      this.removeListener('data', listeners.dataListener);
      this.removeListener('end', listeners.endListener);
      this.removeListener('error', listeners.errorListener);
      state.pipes.delete(target);
      target.emit('unpipe', this);
    }
    if (state.pipes.size === 0) state.flowing = false;
    return this;
  };
  Readable.prototype[Symbol.asyncIterator] = async function* () {
    const state = requireReadableState(this);
    while (true) {
      const chunk = this.read();
      if (chunk !== null) {
        yield chunk;
        continue;
      }
      if (state.ended || state.destroyed) break;
      await onceEvent(this, 'readable');
    }
    finishReadable(this, state);
  };
  Readable.from = (iterable, options) => {
    if (
      iterable === null ||
      iterable === undefined ||
      (!iterable[Symbol.iterator] && !iterable[Symbol.asyncIterator])
    ) {
      throw createError('ERR_INVALID_ARG_TYPE', 'iterable must be iterable');
    }
    const source = typeof iterable === 'string' ? [iterable] : iterable;
    const stream = new Readable({ objectMode: true, ...(options ?? {}) });
    setCallbackImmediate(async () => {
      try {
        for await (const chunk of source) stream.push(chunk);
        stream.push(null);
      } catch (error) {
        stream.destroy(error);
      }
    });
    return stream;
  };
  Readable.isDisturbed = (stream) => {
    const state = stream?.[readableStreamState];
    return Boolean(state && (state.endEmitted || state.length > 0));
  };
  Object.defineProperties(Readable.prototype, {
    readable: {
      get: function () {
        const state = requireReadableState(this);
        return !state.destroyed && !state.endEmitted;
      },
    },
    readableEnded: {
      get: function () {
        return requireReadableState(this).endEmitted;
      },
    },
    readableFlowing: {
      get: function () {
        return requireReadableState(this).flowing;
      },
    },
    readableHighWaterMark: {
      get: function () {
        return requireReadableState(this).highWaterMark;
      },
    },
    readableLength: {
      get: function () {
        return requireReadableState(this).length;
      },
    },
    destroyed: {
      get: function () {
        return requireReadableState(this).destroyed;
      },
    },
    closed: {
      get: function () {
        return requireReadableState(this).closed;
      },
    },
    errored: {
      get: function () {
        return requireReadableState(this).errored;
      },
    },
  });

  const initializeWritable = (stream, options) => {
    const normalizedOptions = options ?? {};
    const objectMode = Boolean(normalizedOptions.objectMode);
    const highWaterMark =
      normalizedOptions.highWaterMark === undefined
        ? objectMode
          ? 16
          : 16384
        : Number(normalizedOptions.highWaterMark);
    if (!Number.isInteger(highWaterMark) || highWaterMark < 0) {
      throw createError(
        'ERR_INVALID_ARG_VALUE',
        'Writable highWaterMark must be a non-negative integer'
      );
    }
    Object.defineProperty(stream, writableStreamState, {
      value: {
        queue: [],
        length: 0,
        objectMode,
        highWaterMark,
        defaultEncoding: normalizedOptions.defaultEncoding ?? 'utf8',
        writing: false,
        corked: 0,
        ending: false,
        ended: false,
        finished: false,
        finalizing: false,
        needDrain: false,
        destroyed: false,
        closed: false,
        errored: null,
      },
      configurable: false,
      enumerable: false,
      writable: false,
    });
    if (typeof normalizedOptions.write === 'function') {
      stream._write = normalizedOptions.write;
    }
    if (typeof normalizedOptions.final === 'function') {
      stream._final = normalizedOptions.final;
    }
    if (typeof normalizedOptions.destroy === 'function') {
      stream._destroy = normalizedOptions.destroy;
    }
    if (normalizedOptions.signal) {
      setCallbackImmediate(() =>
        addAbortSignal(normalizedOptions.signal, stream)
      );
    }
  };

  const requireWritableState = (stream) => {
    const state = stream[writableStreamState];
    if (!state) {
      throw createError(
        'ERR_INVALID_THIS',
        'Writable method called on an incompatible receiver'
      );
    }
    return state;
  };

  const finishWritable = (stream, state) => {
    if (
      !state.ending ||
      state.finished ||
      state.finalizing ||
      state.writing ||
      state.queue.length > 0 ||
      state.destroyed
    ) {
      return;
    }
    state.finalizing = true;
    let completed = false;
    const complete = (error) => {
      if (completed) return;
      completed = true;
      state.finalizing = false;
      if (error) {
        stream.destroy(error);
        return;
      }
      state.finished = true;
      stream.emit('prefinish');
      stream.emit('finish');
    };
    try {
      stream._final(complete);
    } catch (error) {
      complete(error);
    }
  };

  const processWritableQueue = (stream, state) => {
    if (
      state.writing ||
      state.corked > 0 ||
      state.destroyed ||
      state.queue.length === 0
    ) {
      finishWritable(stream, state);
      return;
    }
    const request = state.queue.shift();
    state.writing = true;
    let completed = false;
    const complete = (error) => {
      if (completed) return;
      completed = true;
      state.writing = false;
      state.length -= request.size;
      request.callback(error ?? null);
      if (error) {
        stream.destroy(error);
        return;
      }
      if (state.needDrain && state.length < state.highWaterMark) {
        state.needDrain = false;
        stream.emit('drain');
      }
      processWritableQueue(stream, state);
    };
    try {
      stream._write(request.chunk, request.encoding, complete);
    } catch (error) {
      complete(error);
    }
  };

  const Writable = function (options) {
    if (!(this instanceof Writable)) return new Writable(options);
    Stream.call(this);
    initializeWritable(this, options);
  };
  Writable.prototype = Object.create(Stream.prototype);
  Object.defineProperty(Writable.prototype, 'constructor', {
    value: Writable,
    configurable: true,
    enumerable: false,
    writable: true,
  });
  Writable.prototype._write = (chunk, encoding, callback) =>
    callback(
      createError('ERR_METHOD_NOT_IMPLEMENTED', '_write() is not implemented')
    );
  Writable.prototype._final = (callback) => callback();
  Writable.prototype._destroy = streamDefaultDestroy;
  Writable.prototype.destroy = streamDestroy;
  Writable.prototype.write = function (chunk, encoding, callback) {
    const state = requireWritableState(this);
    if (typeof encoding === 'function') {
      callback = encoding;
      encoding = undefined;
    }
    const completion = typeof callback === 'function' ? callback : () => {};
    if (state.ending || state.ended) {
      const error = createError(
        'ERR_STREAM_WRITE_AFTER_END',
        'Cannot write after end'
      );
      completion(error);
      this.destroy(error);
      return false;
    }
    if (state.destroyed) {
      const error = createError(
        'ERR_STREAM_DESTROYED',
        'Cannot write after destroy'
      );
      completion(error);
      return false;
    }
    const normalizedEncoding = encoding ?? state.defaultEncoding;
    const normalized = normalizeStreamChunk(
      chunk,
      normalizedEncoding,
      state.objectMode
    );
    const size = streamChunkSize(normalized, state.objectMode);
    state.length += size;
    state.queue.push({
      chunk: normalized,
      encoding: normalizedEncoding,
      callback: completion,
      size,
    });
    const accepted = state.length < state.highWaterMark;
    if (!accepted) state.needDrain = true;
    processWritableQueue(this, state);
    return accepted;
  };
  Writable.prototype.end = function (chunk, encoding, callback) {
    const state = requireWritableState(this);
    if (typeof chunk === 'function') {
      callback = chunk;
      chunk = undefined;
      encoding = undefined;
    } else if (typeof encoding === 'function') {
      callback = encoding;
      encoding = undefined;
    }
    if (typeof callback === 'function') this.once('finish', callback);
    if (chunk !== undefined) this.write(chunk, encoding);
    state.ending = true;
    state.ended = true;
    finishWritable(this, state);
    return this;
  };
  Writable.prototype.cork = function () {
    requireWritableState(this).corked += 1;
  };
  Writable.prototype.uncork = function () {
    const state = requireWritableState(this);
    if (state.corked > 0) state.corked -= 1;
    processWritableQueue(this, state);
  };
  Writable.prototype.setDefaultEncoding = function (encoding) {
    normalizeBufferEncoding(encoding);
    requireWritableState(this).defaultEncoding = String(encoding);
    return this;
  };
  Object.defineProperties(Writable.prototype, {
    writable: {
      get: function () {
        const state = requireWritableState(this);
        return !state.destroyed && !state.ended;
      },
    },
    writableEnded: {
      get: function () {
        return requireWritableState(this).ended;
      },
    },
    writableFinished: {
      get: function () {
        return requireWritableState(this).finished;
      },
    },
    writableNeedDrain: {
      get: function () {
        return requireWritableState(this).needDrain;
      },
    },
    writableHighWaterMark: {
      get: function () {
        return requireWritableState(this).highWaterMark;
      },
    },
    writableLength: {
      get: function () {
        return requireWritableState(this).length;
      },
    },
    destroyed: {
      get: function () {
        return requireWritableState(this).destroyed;
      },
    },
    closed: {
      get: function () {
        return requireWritableState(this).closed;
      },
    },
    errored: {
      get: function () {
        return requireWritableState(this).errored;
      },
    },
  });

  const Duplex = function (options) {
    if (!(this instanceof Duplex)) return new Duplex(options);
    Stream.call(this);
    const normalizedOptions = options ?? {};
    initializeReadable(this, {
      ...normalizedOptions,
      objectMode:
        normalizedOptions.readableObjectMode ?? normalizedOptions.objectMode,
      highWaterMark:
        normalizedOptions.readableHighWaterMark ??
        normalizedOptions.highWaterMark,
    });
    initializeWritable(this, {
      ...normalizedOptions,
      objectMode:
        normalizedOptions.writableObjectMode ?? normalizedOptions.objectMode,
      highWaterMark:
        normalizedOptions.writableHighWaterMark ??
        normalizedOptions.highWaterMark,
    });
    this.allowHalfOpen = normalizedOptions.allowHalfOpen !== false;
  };
  Duplex.prototype = Object.create(Readable.prototype);
  Object.defineProperty(Duplex.prototype, 'constructor', {
    value: Duplex,
    configurable: true,
    enumerable: false,
    writable: true,
  });
  for (const name of [
    '_write',
    '_final',
    'write',
    'end',
    'cork',
    'uncork',
    'setDefaultEncoding',
  ]) {
    Duplex.prototype[name] = Writable.prototype[name];
  }
  for (const name of [
    'writable',
    'writableEnded',
    'writableFinished',
    'writableNeedDrain',
    'writableHighWaterMark',
    'writableLength',
  ]) {
    Object.defineProperty(
      Duplex.prototype,
      name,
      Object.getOwnPropertyDescriptor(Writable.prototype, name)
    );
  }
  Duplex.prototype._destroy = streamDefaultDestroy;
  Duplex.prototype.destroy = streamDestroy;

  const Transform = function (options) {
    if (!(this instanceof Transform)) return new Transform(options);
    Duplex.call(this, options);
    const normalizedOptions = options ?? {};
    if (typeof normalizedOptions.transform === 'function') {
      this._transform = normalizedOptions.transform;
    }
    if (typeof normalizedOptions.flush === 'function') {
      this._flush = normalizedOptions.flush;
    }
  };
  Transform.prototype = Object.create(Duplex.prototype);
  Object.defineProperty(Transform.prototype, 'constructor', {
    value: Transform,
    configurable: true,
    enumerable: false,
    writable: true,
  });
  Transform.prototype._transform = (chunk, encoding, callback) =>
    callback(
      createError(
        'ERR_METHOD_NOT_IMPLEMENTED',
        '_transform() is not implemented'
      )
    );
  Transform.prototype._flush = (callback) => callback();
  Transform.prototype._write = function (chunk, encoding, callback) {
    let completed = false;
    const complete = (error, output) => {
      if (completed) return;
      completed = true;
      if (output !== undefined && output !== null) this.push(output);
      callback(error);
    };
    try {
      this._transform(chunk, encoding, complete);
    } catch (error) {
      complete(error);
    }
  };
  Transform.prototype._final = function (callback) {
    let completed = false;
    const complete = (error, output) => {
      if (completed) return;
      completed = true;
      if (output !== undefined && output !== null) this.push(output);
      if (!error) this.push(null);
      callback(error);
    };
    try {
      this._flush(complete);
    } catch (error) {
      complete(error);
    }
  };

  const PassThrough = function (options) {
    if (!(this instanceof PassThrough)) return new PassThrough(options);
    Transform.call(this, options);
  };
  PassThrough.prototype = Object.create(Transform.prototype);
  Object.defineProperty(PassThrough.prototype, 'constructor', {
    value: PassThrough,
    configurable: true,
    enumerable: false,
    writable: true,
  });
  PassThrough.prototype._transform = (chunk, encoding, callback) =>
    callback(null, chunk);

  const isReadable = (stream) => {
    const state = stream?.[readableStreamState];
    return Boolean(state && !state.destroyed && !state.endEmitted);
  };
  const isWritable = (stream) => {
    const state = stream?.[writableStreamState];
    return Boolean(state && !state.destroyed && !state.ended);
  };
  const isDestroyed = (stream) => {
    const readable = stream?.[readableStreamState];
    const writable = stream?.[writableStreamState];
    return Boolean(readable?.destroyed || writable?.destroyed);
  };
  const isErrored = (stream) => {
    const readable = stream?.[readableStreamState];
    const writable = stream?.[writableStreamState];
    return Boolean(readable?.errored || writable?.errored);
  };

  const addAbortSignal = (signal, stream) => {
    if (!(signal instanceof AbortSignal)) {
      throw createError(
        'ERR_INVALID_ARG_TYPE',
        'signal must be an AbortSignal'
      );
    }
    if (!stream || typeof stream.destroy !== 'function') {
      throw createError('ERR_INVALID_ARG_TYPE', 'stream must be destroyable');
    }
    const abort = () => stream.destroy(createAbortError(signal.reason));
    if (signal.aborted) abort();
    else signal.addEventListener('abort', abort, { once: true });
    stream.once('close', () => signal.removeEventListener('abort', abort));
    return stream;
  };

  const finishedStreamPromise = async (stream, options) => {
    if (!stream || typeof stream.on !== 'function') {
      throw createError(
        'ERR_INVALID_ARG_TYPE',
        'stream must be an EventEmitter'
      );
    }
    const normalizedOptions = options ?? {};
    const readable = stream[readableStreamState];
    const writable = stream[writableStreamState];
    const waitReadable =
      normalizedOptions.readable === undefined
        ? Boolean(readable)
        : Boolean(normalizedOptions.readable);
    const waitWritable =
      normalizedOptions.writable === undefined
        ? Boolean(writable)
        : Boolean(normalizedOptions.writable);
    const isComplete = () =>
      (!waitReadable || readable?.endEmitted || readable?.destroyed) &&
      (!waitWritable || writable?.finished || writable?.destroyed);
    if (isComplete()) {
      if (readable?.errored) throw readable.errored;
      if (writable?.errored) throw writable.errored;
      return;
    }
    await new Promise((resolve, reject) => {
      let settled = false;
      const cleanup = () => {
        stream.removeListener('end', check);
        stream.removeListener('finish', check);
        stream.removeListener('close', check);
        stream.removeListener('error', fail);
        if (normalizedOptions.signal) {
          normalizedOptions.signal.removeEventListener('abort', abort);
        }
      };
      const settle = (operation, value) => {
        if (settled) return;
        settled = true;
        cleanup();
        operation(value);
      };
      const check = () => {
        if (isComplete()) settle(resolve, undefined);
      };
      const fail = (error) => settle(reject, error);
      const abort = () =>
        settle(reject, createAbortError(normalizedOptions.signal.reason));
      stream.on('end', check);
      stream.on('finish', check);
      stream.on('close', check);
      stream.on('error', fail);
      if (normalizedOptions.signal) {
        if (normalizedOptions.signal.aborted) abort();
        else {
          normalizedOptions.signal.addEventListener('abort', abort, {
            once: true,
          });
        }
      }
      check();
    });
  };

  const finishedStream = (stream, options, callback) => {
    if (typeof options === 'function') {
      callback = options;
      options = undefined;
    }
    if (typeof callback !== 'function') {
      throw createError('ERR_INVALID_ARG_TYPE', 'callback must be a function');
    }
    let active = true;
    setCallbackImmediate(async () => {
      try {
        await finishedStreamPromise(stream, options);
        if (active) callback(null);
      } catch (error) {
        if (active) callback(error);
      }
    });
    return () => {
      active = false;
    };
  };

  const normalizePipelineStreams = (values) => {
    const streams =
      values.length === 1 && Array.isArray(values[0]) ? values[0] : values;
    if (streams.length < 2) {
      throw createError(
        'ERR_MISSING_ARGS',
        'pipeline requires at least a source and destination'
      );
    }
    const normalized = [...streams];
    if (!normalized[0] || typeof normalized[0].pipe !== 'function') {
      normalized[0] = Readable.from(normalized[0]);
    }
    for (const stream of normalized) {
      if (!stream || typeof stream.on !== 'function') {
        throw createError(
          'ERR_INVALID_ARG_TYPE',
          'pipeline entries must be streams or an iterable source'
        );
      }
    }
    return normalized;
  };

  const pipelineStreamsPromise = async (...values) => {
    const streams = normalizePipelineStreams(values);
    const destination = streams.at(-1);
    const completion = new Promise((resolve, reject) => {
      let settled = false;
      const cleanup = () => {
        for (const stream of streams) stream.removeListener('error', fail);
        destination.removeListener('finish', succeed);
        destination.removeListener('close', close);
      };
      const settle = (operation, value) => {
        if (settled) return;
        settled = true;
        cleanup();
        operation(value);
      };
      const fail = (error) => {
        for (const stream of streams) {
          if (typeof stream.destroy === 'function' && !isDestroyed(stream)) {
            stream.destroy();
          }
        }
        settle(reject, error);
      };
      const succeed = () => settle(resolve, undefined);
      const close = () => {
        if (destination.writableFinished || destination.readableEnded) {
          succeed();
        } else {
          fail(
            createError(
              'ERR_STREAM_PREMATURE_CLOSE',
              'Pipeline destination closed prematurely'
            )
          );
        }
      };
      for (const stream of streams) stream.on('error', fail);
      destination.once('finish', succeed);
      destination.once('close', close);
    });
    for (let index = 0; index + 1 < streams.length; index += 1) {
      streams[index].pipe(streams[index + 1]);
    }
    await completion;
  };

  const pipelineStreams = (...values) => {
    const callback = values.pop();
    if (typeof callback !== 'function') {
      throw createError('ERR_INVALID_ARG_TYPE', 'callback must be a function');
    }
    const streams = normalizePipelineStreams(values);
    setCallbackImmediate(async () => {
      try {
        await pipelineStreamsPromise(...streams);
        callback(null);
      } catch (error) {
        callback(error);
      }
    });
    return streams.at(-1);
  };

  const streamPromises = Object.freeze({
    finished: finishedStreamPromise,
    pipeline: pipelineStreamsPromise,
  });
  const streamModule = Object.freeze({
    Stream,
    Readable,
    Writable,
    Duplex,
    Transform,
    PassThrough,
    addAbortSignal,
    finished: finishedStream,
    pipeline: pipelineStreams,
    isReadable,
    isWritable,
    isDestroyed,
    isErrored,
    promises: streamPromises,
  });

  Object.defineProperty(globalThis, '__muonDispatchTimer', {
    value: dispatchTimer,
    configurable: false,
    enumerable: false,
    writable: false,
  });

  Object.defineProperties(globalThis, {
    Event: { value: Event, configurable: true, writable: true },
    EventTarget: { value: EventTarget, configurable: true, writable: true },
    DOMException: { value: DOMException, configurable: true, writable: true },
    AbortSignal: { value: AbortSignal, configurable: true, writable: true },
    AbortController: {
      value: AbortController,
      configurable: true,
      writable: true,
    },
    URL: { value: URL, configurable: true, writable: true },
    URLSearchParams: {
      value: URLSearchParams,
      configurable: true,
      writable: true,
    },
    Buffer: { value: Buffer, configurable: true, writable: true },
    setTimeout: {
      value: setCallbackTimeout,
      configurable: true,
      writable: true,
    },
    clearTimeout: {
      value: clearCallbackTimeout,
      configurable: true,
      writable: true,
    },
    setInterval: {
      value: setCallbackInterval,
      configurable: true,
      writable: true,
    },
    clearInterval: {
      value: clearCallbackInterval,
      configurable: true,
      writable: true,
    },
    setImmediate: {
      value: setCallbackImmediate,
      configurable: true,
      writable: true,
    },
    clearImmediate: {
      value: clearCallbackImmediate,
      configurable: true,
      writable: true,
    },
  });

  const hostModules = Object.freeze({
    'node:fs/promises': fsPromises,
    'fs/promises': fsPromises,
    fs: fsCallbacks,
    'node:fs': fsCallbacks,
    path: pathModule,
    'node:path': pathModule,
    events: eventsModule,
    'node:events': eventsModule,
    buffer: bufferModule,
    'node:buffer': bufferModule,
    timers: timersModule,
    'node:timers': timersModule,
    'timers/promises': timersPromises,
    'node:timers/promises': timersPromises,
    stream: streamModule,
    'node:stream': streamModule,
    'stream/promises': streamPromises,
    'node:stream/promises': streamPromises,
    url: urlModule,
    'node:url': urlModule,
  });

  const findHostModule = (specifier) => {
    const module = hostModules[specifier];
    if (!module) {
      throw createError(
        'ERR_MUON_JS_MODULE_NOT_FOUND',
        `Unknown module: ${specifier}`
      );
    }
    return module;
  };

  const findModule = (specifier) => {
    if (specifier === '.') {
      if (!globalThis.__muonBackendModule) {
        throw createError(
          'ERR_MUON_JS_MODULE',
          'The packaged module is unavailable'
        );
      }
      return globalThis.__muonBackendModule;
    }
    return findHostModule(specifier);
  };

  const importModule = (specifier) => {
    const module = findModule(specifier);
    const moduleId = `module-${nextModuleHandle++}`;
    moduleHandles.set(moduleId, module);
    const exports = Object.keys(module).map((name) => {
      const value = module[name];
      return typeof value === 'function'
        ? { name, kind: 'function' }
        : { name, kind: 'primitive', value: encodeValue(value) };
    });
    return { moduleId, descriptor: { exports } };
  };

  const executeRequest = async (message) => {
    const parameters = message.params ?? {};
    if (message.command === 'importModule') {
      return importModule(parameters.specifier);
    }
    if (message.command === 'call') {
      const module = moduleHandles.get(parameters.moduleId);
      if (!module) {
        throw createError(
          'ERR_MUON_JS_MODULE_RELEASED',
          'Module handle has been released'
        );
      }
      const operation = module[parameters.exportName];
      if (typeof operation !== 'function') {
        throw createError(
          'ERR_MUON_JS_EXPORT',
          `Unknown function export: ${parameters.exportName}`
        );
      }
      const arguments_ = Array.isArray(parameters.arguments)
        ? parameters.arguments.map(decodeValue)
        : [];
      return encodeValue(await Reflect.apply(operation, module, arguments_));
    }
    if (message.command === 'release') {
      if (
        parameters.kind !== 'module' ||
        typeof parameters.handle !== 'string'
      ) {
        throw createError('ERR_MUON_JS_PROTOCOL', 'Invalid release request');
      }
      moduleHandles.delete(parameters.handle);
      return { released: true };
    }
    if (message.command === 'shutdown') {
      moduleHandles.clear();
      globalThis.__muonShouldShutdown = true;
      return { shutdown: true };
    }
    throw createError(
      'ERR_MUON_JS_COMMAND',
      `Unknown command: ${message.command}`
    );
  };

  const normalizeError = (error) => {
    const message = error instanceof Error ? error.message : String(error);
    let code =
      error && typeof error === 'object' && typeof error.code === 'string'
        ? error.code
        : 'ERR_MUON_JS_RUNTIME';
    if (message.toLowerCase().includes('out of memory')) {
      code = 'ERR_MUON_JS_OUT_OF_MEMORY';
    } else if (message.toLowerCase().includes('interrupted')) {
      code = 'ERR_MUON_JS_INTERRUPTED';
    } else if (/^[A-Z][A-Z0-9_]+:/.test(message)) {
      code = message.slice(0, message.indexOf(':'));
    }
    return { code, message };
  };

  globalThis.__muonShouldShutdown = false;
  Object.defineProperty(globalThis, '__muonGetHostModule', {
    value: findHostModule,
    configurable: false,
    enumerable: false,
    writable: false,
  });
  globalThis.__muonHandleMessage = async (source) => {
    const message = JSON.parse(source);
    if (message.kind === 'callbackResult') {
      const pending = rendererCallbacks.get(message.id);
      if (pending) {
        rendererCallbacks.delete(message.id);
        if (message.ok === true) {
          pending.resolve(decodeValue(message.value));
        } else {
          const details = message.error ?? {};
          pending.reject(
            createError(
              typeof details.code === 'string'
                ? details.code
                : 'ERR_MUON_JS_CALLBACK',
              typeof details.message === 'string'
                ? details.message
                : 'Renderer callback failed'
            )
          );
        }
      }
      return null;
    }
    if (message.kind !== 'request' || typeof message.id !== 'string') {
      throw createError('ERR_MUON_JS_PROTOCOL', 'Unsupported runtime message');
    }
    try {
      const value = await executeRequest(message);
      return JSON.stringify({
        kind: 'response',
        id: message.id,
        ok: true,
        value,
        error: null,
      });
    } catch (error) {
      return JSON.stringify({
        kind: 'response',
        id: message.id,
        ok: false,
        value: null,
        error: normalizeError(error),
      });
    }
  };
})();
