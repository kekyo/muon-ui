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

  const timersPromises = Object.freeze({
    setTimeout: async (delay, value) => {
      await __muonSleep(delay);
      return value;
    },
  });

  const hostModules = Object.freeze({
    'node:fs/promises': fsPromises,
    'fs/promises': fsPromises,
    fs: fsCallbacks,
    'node:fs': fsCallbacks,
    path: pathModule,
    'node:path': pathModule,
    'timers/promises': timersPromises,
    'node:timers/promises': timersPromises,
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
