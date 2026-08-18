// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import type { MuonWebViewRpcClient } from './webview-rpc.js';

const browserFunctionNames = [
  'reload',
  'toggleFullscreen',
  'enterFullscreen',
  'exitFullscreen',
  'zoomIn',
  'zoomOut',
  'resetZoom',
  'close',
] as const;

const environmentFunctionNames = [
  'getVariables',
  'getConfigValues',
  'getProcessId',
  'getRuntimeInfo',
] as const;

const filesystemFunctionNames = [
  'readFile',
  'writeFile',
  'readTextFile',
  'writeTextFile',
  'stat',
  'lstat',
  'exists',
  'access',
  'readdir',
  'mkdir',
  'rm',
  'unlink',
  'rmdir',
  'rename',
  'copyFile',
  'appendFile',
  'appendTextFile',
  'truncate',
  'realpath',
  'readlink',
  'symlink',
  'watch',
] as const;

/** Fully-qualified built-in function paths implemented by the Android backend. */
export const muonAndroidBuiltinFunctionPaths = [
  ...browserFunctionNames.map((name) => `muon.browser.${name}` as const),
  ...environmentFunctionNames.map(
    (name) => `muon.environments.${name}` as const
  ),
  ...filesystemFunctionNames.map((name) => `muon.fs.${name}` as const),
] as const;

/** A built-in function path available on Android. */
export type MuonAndroidBuiltinFunctionPath =
  (typeof muonAndroidBuiltinFunctionPaths)[number];

/** Capability ids assigned to Android built-in namespaces. */
export interface MuonAndroidCapabilityIds {
  /** Capability for browser operations. */
  readonly 'muon.browser': string;

  /** Capability for environment information. */
  readonly 'muon.environments': string;

  /** Capability for sandboxed filesystem operations. */
  readonly 'muon.fs': string;
}

/** Android browser functions available in simple mode. */
export type MuonAndroidBrowserApi = Readonly<
  Record<(typeof browserFunctionNames)[number], () => Promise<void>>
>;

/** Runtime information reported by the Android WebView backend. */
export interface MuonAndroidRuntimeInfo {
  /** Selects the Android arm of the shared runtime-information union. */
  readonly backend: 'android-webview';

  /** Android operating-system identifier. */
  readonly os: 'android';

  /** User-visible Android release string. */
  readonly osVersion: string;

  /** Android SDK level running this application process. */
  readonly apiLevel: number;

  /** Primary ABI selected for this process. */
  readonly abi: string;

  /** Android application package name. */
  readonly applicationId: string;

  /** Android application version name. */
  readonly applicationVersion: string;

  /** Package providing WebView to this process. */
  readonly webViewPackage: string;

  /** Version of the WebView provider package. */
  readonly webViewVersion: string;
}

/** Android environment functions available in simple mode. */
export interface MuonAndroidEnvironmentsApi {
  /** Returns the current process environment variables. */
  readonly getVariables: () => Promise<Record<string, string>>;

  /** Returns the merged application string configuration. */
  readonly getConfigValues: () => Promise<Record<string, string>>;

  /** Returns the current Android application process id. */
  readonly getProcessId: () => Promise<number>;

  /** Returns Android and WebView runtime information. */
  readonly getRuntimeInfo: () => Promise<MuonAndroidRuntimeInfo>;
}

/** Options shared by Android filesystem operations. */
export interface MuonAndroidFsOperationOptions {
  /** Signal used to request best-effort cancellation. */
  readonly signal?: AbortSignal;
}

/** Options for reading part or all of a binary file. */
export interface MuonAndroidFsReadFileOptions extends MuonAndroidFsOperationOptions {
  /** Non-negative byte offset. */
  readonly position?: number;

  /** Maximum number of bytes to return. */
  readonly length?: number;
}

/** Options for writing binary data. */
export interface MuonAndroidFsWriteFileOptions extends MuonAndroidFsOperationOptions {
  /** Non-negative byte offset, or undefined to replace the file. */
  readonly position?: number;
}

/** Access modes checked by the Android filesystem backend. */
export type MuonAndroidFsAccessMode = 'read' | 'write' | 'execute';

/** Options for checking filesystem access. */
export interface MuonAndroidFsAccessOptions extends MuonAndroidFsOperationOptions {
  /** Access modes that must all be allowed. */
  readonly mode?: readonly MuonAndroidFsAccessMode[];
}

/** Options for directory enumeration. */
export interface MuonAndroidFsReadDirectoryOptions extends MuonAndroidFsOperationOptions {
  /** Returns metadata-bearing entries when true. */
  readonly withFileTypes?: boolean;
}

/** Options for directory creation. */
export interface MuonAndroidFsMakeDirectoryOptions extends MuonAndroidFsOperationOptions {
  /** Creates missing parent directories when true. */
  readonly recursive?: boolean;
}

/** Options for recursive or missing-tolerant removal. */
export interface MuonAndroidFsRemoveOptions extends MuonAndroidFsOperationOptions {
  /** Removes directory trees when true. */
  readonly recursive?: boolean;

  /** Suppresses a missing-path error when true. */
  readonly force?: boolean;
}

/** Options for copying a regular file. */
export interface MuonAndroidFsCopyFileOptions extends MuonAndroidFsOperationOptions {
  /** Replaces an existing destination unless false. */
  readonly overwrite?: boolean;
}

/** Filesystem entry kinds reported by Android. */
export type MuonAndroidFsEntryType =
  | 'file'
  | 'directory'
  | 'symlink'
  | 'blockDevice'
  | 'characterDevice'
  | 'fifo'
  | 'socket'
  | 'other';

/** Metadata returned for an Android filesystem entry. */
export interface MuonAndroidFsStats {
  /** Entry kind. */
  readonly type: MuonAndroidFsEntryType;

  /** Regular-file byte length, or zero for other entry kinds. */
  readonly size: number;

  /** Last modification time in Unix epoch milliseconds. */
  readonly mtimeMs: number;

  /** Whether no filesystem write permission bit is set. */
  readonly readonly: boolean;

  /** Returns whether this entry is a regular file. */
  readonly isFile: () => boolean;

  /** Returns whether this entry is a directory. */
  readonly isDirectory: () => boolean;

  /** Returns whether this entry is a symbolic link. */
  readonly isSymbolicLink: () => boolean;
}

/** Directory entry with Android filesystem metadata. */
export interface MuonAndroidFsDirent extends MuonAndroidFsStats {
  /** Entry name relative to the enumerated directory. */
  readonly name: string;
}

/** Symbolic-link kinds accepted by the shared API. */
export type MuonAndroidFsSymlinkType = 'file' | 'dir' | 'junction';

/** Event delivered by an Android filesystem watcher. */
export interface MuonAndroidFsWatchEvent {
  /** Event kind. */
  readonly eventType: 'rename' | 'change' | 'error';

  /** Changed child name, or null for the watched path itself. */
  readonly filename: string | null;

  /** Diagnostic for an error event. */
  readonly message?: string;
}

/** Listener invoked for Android filesystem watch events. */
export type MuonAndroidFsWatchListener = (
  event: MuonAndroidFsWatchEvent
) => void | Promise<void>;

/** Handle for an active Android filesystem watcher. */
export interface MuonAndroidFsWatcher {
  /** Releases the native watcher lease and stops polling. */
  readonly close: () => Promise<void>;
}

/** Android filesystem functions available in simple mode. */
export interface MuonAndroidFsApi {
  /** Reads binary file data. */
  readonly readFile: (
    path: string,
    options?: MuonAndroidFsReadFileOptions
  ) => Promise<ArrayBuffer>;

  /** Writes binary file data. */
  readonly writeFile: (
    path: string,
    data: BufferSource,
    options?: MuonAndroidFsWriteFileOptions
  ) => Promise<void>;

  /** Reads strict UTF-8 text without NUL bytes. */
  readonly readTextFile: (
    path: string,
    encoding: 'utf8' | 'utf-8',
    options?: MuonAndroidFsOperationOptions
  ) => Promise<string>;

  /** Writes strict UTF-8 text without NUL bytes. */
  readonly writeTextFile: (
    path: string,
    data: string,
    encoding: 'utf8' | 'utf-8',
    options?: MuonAndroidFsOperationOptions
  ) => Promise<void>;

  /** Reads metadata while following symbolic links. */
  readonly stat: (
    path: string,
    options?: MuonAndroidFsOperationOptions
  ) => Promise<MuonAndroidFsStats>;

  /** Reads metadata without following symbolic links. */
  readonly lstat: (
    path: string,
    options?: MuonAndroidFsOperationOptions
  ) => Promise<MuonAndroidFsStats>;

  /** Tests whether a path exists. */
  readonly exists: (
    path: string,
    options?: MuonAndroidFsOperationOptions
  ) => Promise<boolean>;

  /** Tests requested access modes. */
  readonly access: (
    path: string,
    options?: MuonAndroidFsAccessOptions
  ) => Promise<boolean>;

  /** Enumerates a directory as names or metadata-bearing entries. */
  readonly readdir: (
    path: string,
    options?: MuonAndroidFsReadDirectoryOptions
  ) => Promise<string[] | MuonAndroidFsDirent[]>;

  /** Creates a directory. */
  readonly mkdir: (
    path: string,
    options?: MuonAndroidFsMakeDirectoryOptions
  ) => Promise<void>;

  /** Removes a file or directory tree. */
  readonly rm: (
    path: string,
    options?: MuonAndroidFsRemoveOptions
  ) => Promise<void>;

  /** Removes a file or symbolic link. */
  readonly unlink: (
    path: string,
    options?: MuonAndroidFsOperationOptions
  ) => Promise<void>;

  /** Removes an empty directory. */
  readonly rmdir: (
    path: string,
    options?: MuonAndroidFsOperationOptions
  ) => Promise<void>;

  /** Renames a filesystem entry. */
  readonly rename: (
    oldPath: string,
    newPath: string,
    options?: MuonAndroidFsOperationOptions
  ) => Promise<void>;

  /** Copies a regular file. */
  readonly copyFile: (
    source: string,
    destination: string,
    options?: MuonAndroidFsCopyFileOptions
  ) => Promise<void>;

  /** Appends binary data to a file. */
  readonly appendFile: (
    path: string,
    data: BufferSource,
    options?: MuonAndroidFsOperationOptions
  ) => Promise<void>;

  /** Appends strict UTF-8 text to a file. */
  readonly appendTextFile: (
    path: string,
    data: string,
    encoding: 'utf8' | 'utf-8',
    options?: MuonAndroidFsOperationOptions
  ) => Promise<void>;

  /** Truncates or extends a file. */
  readonly truncate: (
    path: string,
    lengthOrOptions?: number | MuonAndroidFsOperationOptions,
    options?: MuonAndroidFsOperationOptions
  ) => Promise<void>;

  /** Resolves an existing path to its canonical absolute path. */
  readonly realpath: (
    path: string,
    options?: MuonAndroidFsOperationOptions
  ) => Promise<string>;

  /** Reads a symbolic-link target. */
  readonly readlink: (
    path: string,
    options?: MuonAndroidFsOperationOptions
  ) => Promise<string>;

  /** Creates a symbolic link. */
  readonly symlink: (
    target: string,
    path: string,
    typeOrOptions?: MuonAndroidFsSymlinkType | MuonAndroidFsOperationOptions,
    options?: MuonAndroidFsOperationOptions
  ) => Promise<void>;

  /** Watches a path until the returned handle is closed. */
  readonly watch: (
    path: string,
    listener: MuonAndroidFsWatchListener,
    options?: MuonAndroidFsOperationOptions
  ) => Promise<MuonAndroidFsWatcher>;
}

/** Built-in API object exposed by Android simple mode. */
export interface MuonAndroidSimpleApi {
  /** Operations for the owning Activity and WebView. */
  readonly browser: MuonAndroidBrowserApi;

  /** Information about the current Android application process. */
  readonly environments: MuonAndroidEnvironmentsApi;

  /** Sandboxed real-path filesystem operations. */
  readonly fs: MuonAndroidFsApi;
}

const escapeRegularExpression = (source: string): string =>
  source.replace(/[\\^$+?.()|[\]{}]/gu, '\\$&');

const createFunctionAllowExpression = (allow: string): RegExp => {
  let source = '^';
  for (let index = 0; index < allow.length; index += 1) {
    const character = allow[index] ?? '';
    const next = allow[index + 1];
    if (character === '*' && next === '*') {
      source += '.*';
      index += 1;
    } else if (character === '*') {
      source += '[^.]*';
    } else {
      source += escapeRegularExpression(character);
    }
  }
  source += '$';
  return new RegExp(source, 'u');
};

/**
 * Expands Android capability allow patterns into available function paths.
 *
 * @param allows - Exact paths or glob patterns requested by a validate build.
 * @returns Available Android paths in stable metadata order.
 * @remarks An exact unsupported path, or a pattern matching no Android
 * functions, fails during the build instead of producing a runtime stub.
 */
export const expandMuonAndroidFunctionAllows = (
  allows: readonly string[]
): readonly MuonAndroidBuiltinFunctionPath[] => {
  const selected = new Set<MuonAndroidBuiltinFunctionPath>();
  for (const allow of allows) {
    if (!allow.includes('*')) {
      const functionPath = muonAndroidBuiltinFunctionPaths.find(
        (candidate) => candidate === allow
      );
      if (functionPath === undefined) {
        throw new Error(`Muon function is unavailable for Android: ${allow}`);
      }
      selected.add(functionPath);
      continue;
    }

    const expression = createFunctionAllowExpression(allow);
    const matches = muonAndroidBuiltinFunctionPaths.filter((functionPath) =>
      expression.test(functionPath)
    );
    if (matches.length === 0) {
      throw new Error(
        `Muon function pattern is unavailable for Android: ${allow}`
      );
    }
    for (const functionPath of matches) {
      selected.add(functionPath);
    }
  }
  return muonAndroidBuiltinFunctionPaths.filter((functionPath) =>
    selected.has(functionPath)
  );
};

const createVoidNamespace = <TName extends string>(
  client: MuonWebViewRpcClient,
  capabilityId: string,
  namespace: string,
  functionNames: readonly TName[]
): Readonly<Record<TName, () => Promise<void>>> => {
  const entries = functionNames.map(
    (name) =>
      [
        name,
        async () => {
          await client.call(capabilityId, `${namespace}.${name}`, []);
        },
      ] as const
  );
  return Object.freeze(Object.fromEntries(entries)) as Readonly<
    Record<TName, () => Promise<void>>
  >;
};

const parseNativeJson = async <T>(source: Promise<unknown>): Promise<T> => {
  const value = await source;
  if (typeof value !== 'string') {
    throw new TypeError('Android native JSON result is invalid');
  }
  return JSON.parse(value) as T;
};

const isRecord = (value: unknown): value is Record<string, unknown> =>
  typeof value === 'object' && value !== null && !Array.isArray(value);

const validatePath = (value: string, name = 'path'): string => {
  if (typeof value !== 'string' || value.length === 0) {
    throw new TypeError(`${name} must be a non-empty string`);
  }
  if (value.includes('\0')) {
    throw new TypeError(`${name} must not contain NUL`);
  }
  return value;
};

const validateSafeUnsignedInteger = (value: number, name: string): number => {
  if (!Number.isSafeInteger(value) || value < 0) {
    throw new TypeError(`${name} must be a non-negative safe integer`);
  }
  return value;
};

const validateEncoding = (value: string): 'utf8' | 'utf-8' => {
  if (value !== 'utf8' && value !== 'utf-8') {
    throw new TypeError('encoding must be utf8 or utf-8');
  }
  return value;
};

const getOperationSignal = (
  options: MuonAndroidFsOperationOptions | undefined
): AbortSignal | undefined => {
  if (options === undefined) {
    return undefined;
  }
  if (!isRecord(options)) {
    throw new TypeError('filesystem options must be an object');
  }
  const { signal } = options;
  if (signal !== undefined && !(signal instanceof AbortSignal)) {
    throw new TypeError('signal must be an AbortSignal');
  }
  return signal;
};

const normalizeOptionalSafeUnsignedInteger = (
  options: object,
  name: string
): number | undefined => {
  const value = (options as Record<string, unknown>)[name];
  return value === undefined
    ? undefined
    : validateSafeUnsignedInteger(value as number, name);
};

const normalizeReadFileOptions = (
  options: MuonAndroidFsReadFileOptions | undefined
): Readonly<{ position?: number; length?: number }> => {
  const source = options ?? {};
  const position = normalizeOptionalSafeUnsignedInteger(source, 'position');
  const length = normalizeOptionalSafeUnsignedInteger(source, 'length');
  return Object.freeze({
    ...(position === undefined ? {} : { position }),
    ...(length === undefined ? {} : { length }),
  });
};

const normalizeWriteFileOptions = (
  options: MuonAndroidFsWriteFileOptions | undefined
): Readonly<{ position?: number }> => {
  const source = options ?? {};
  const position = normalizeOptionalSafeUnsignedInteger(source, 'position');
  return Object.freeze(position === undefined ? {} : { position });
};

const normalizeBooleanOptions = (
  options: MuonAndroidFsOperationOptions | undefined,
  names: readonly string[]
): Readonly<Record<string, boolean>> => {
  const source = options ?? {};
  const result: Record<string, boolean> = {};
  for (const name of names) {
    const value = source[name as keyof typeof source];
    if (value !== undefined) {
      if (typeof value !== 'boolean') {
        throw new TypeError(`${name} must be a boolean`);
      }
      result[name] = value;
    }
  }
  return Object.freeze(result);
};

const normalizeAccessOptions = (
  options: MuonAndroidFsAccessOptions | undefined
): Readonly<{ mode?: readonly MuonAndroidFsAccessMode[] }> => {
  if (options?.mode === undefined) {
    return Object.freeze({});
  }
  if (!Array.isArray(options.mode)) {
    throw new TypeError('mode must be an array');
  }
  const mode = options.mode.map((value) => {
    if (value !== 'read' && value !== 'write' && value !== 'execute') {
      throw new TypeError('mode entries must be read, write, or execute');
    }
    return value;
  });
  return Object.freeze({ mode: Object.freeze(mode) });
};

const validateBufferSource = (value: BufferSource): BufferSource => {
  if (!(value instanceof ArrayBuffer) && !ArrayBuffer.isView(value)) {
    throw new TypeError('data must be a BufferSource');
  }
  return value;
};

const createAbortReason = (signal: AbortSignal): unknown =>
  signal.reason ?? new DOMException('The operation was aborted', 'AbortError');

const addStatsMethods = <T extends Record<string, unknown>>(
  value: T
): T & MuonAndroidFsStats => {
  const types: readonly MuonAndroidFsEntryType[] = [
    'file',
    'directory',
    'symlink',
    'blockDevice',
    'characterDevice',
    'fifo',
    'socket',
    'other',
  ];
  if (
    typeof value.type !== 'string' ||
    !types.includes(value.type as MuonAndroidFsEntryType) ||
    typeof value.size !== 'number' ||
    !Number.isFinite(value.size) ||
    value.size < 0 ||
    typeof value.mtimeMs !== 'number' ||
    !Number.isFinite(value.mtimeMs) ||
    typeof value.readonly !== 'boolean'
  ) {
    throw new TypeError('Android filesystem metadata is invalid');
  }
  Object.defineProperties(value, {
    isFile: { value: () => value.type === 'file' },
    isDirectory: { value: () => value.type === 'directory' },
    isSymbolicLink: { value: () => value.type === 'symlink' },
  });
  return value as T & MuonAndroidFsStats;
};

const parseStats = async (
  source: Promise<unknown>
): Promise<MuonAndroidFsStats> => {
  const value = await parseNativeJson<Record<string, unknown>>(source);
  if (!isRecord(value)) {
    throw new TypeError('Android filesystem metadata is invalid');
  }
  return addStatsMethods(value);
};

const parseDirectoryEntries = async (
  source: Promise<unknown>,
  withFileTypes: boolean
): Promise<string[] | MuonAndroidFsDirent[]> => {
  const values = await parseNativeJson<unknown[]>(source);
  if (!Array.isArray(values)) {
    throw new TypeError('Android directory result is invalid');
  }
  if (!withFileTypes) {
    if (!values.every((value) => typeof value === 'string')) {
      throw new TypeError('Android directory name result is invalid');
    }
    return values as string[];
  }
  return values.map((value) => {
    if (!isRecord(value) || typeof value.name !== 'string') {
      throw new TypeError('Android directory entry result is invalid');
    }
    return addStatsMethods(value) as unknown as MuonAndroidFsDirent;
  });
};

interface MuonAndroidFsWatchSnapshot {
  readonly root: MuonAndroidFsStats;
  readonly entries: readonly MuonAndroidFsDirent[];
}

const parseWatchSnapshot = async (
  source: Promise<unknown>
): Promise<MuonAndroidFsWatchSnapshot> => {
  const value = await parseNativeJson<Record<string, unknown>>(source);
  if (
    !isRecord(value) ||
    !isRecord(value.root) ||
    !Array.isArray(value.entries)
  ) {
    throw new TypeError('Android filesystem watch snapshot is invalid');
  }
  const root = addStatsMethods(value.root);
  const entries = value.entries
    .map((entry) => {
      if (!isRecord(entry) || typeof entry.name !== 'string') {
        throw new TypeError('Android filesystem watch entry is invalid');
      }
      return addStatsMethods(entry) as unknown as MuonAndroidFsDirent;
    })
    .sort((left, right) => left.name.localeCompare(right.name));
  return { root, entries };
};

const filesystemStatusKey = (entry: MuonAndroidFsStats): string =>
  [entry.type, entry.size, entry.mtimeMs, entry.readonly].join(':');

const notifyFilesystemWatchListener = (
  listener: MuonAndroidFsWatchListener,
  event: MuonAndroidFsWatchEvent
): void => {
  let result: void | Promise<void>;
  try {
    result = listener(event);
  } catch {
    // Listener failures do not stop the native watcher lease.
    return;
  }
  if (result !== undefined) {
    const observeResult = async (): Promise<void> => {
      try {
        await result;
      } catch {
        // Asynchronous listener failures are ignored by the public contract.
      }
    };
    globalThis.queueMicrotask(observeResult);
  }
};

const emitFilesystemWatchDiff = (
  previous: MuonAndroidFsWatchSnapshot,
  next: MuonAndroidFsWatchSnapshot,
  listener: MuonAndroidFsWatchListener
): void => {
  if (filesystemStatusKey(previous.root) !== filesystemStatusKey(next.root)) {
    notifyFilesystemWatchListener(listener, {
      eventType: 'change',
      filename: null,
    });
  }
  const previousEntries = new Map(
    previous.entries.map((entry) => [entry.name, entry] as const)
  );
  const nextEntries = new Map(
    next.entries.map((entry) => [entry.name, entry] as const)
  );
  for (const [name, entry] of previousEntries) {
    const nextEntry = nextEntries.get(name);
    if (nextEntry === undefined) {
      notifyFilesystemWatchListener(listener, {
        eventType: 'rename',
        filename: name,
      });
    } else if (filesystemStatusKey(entry) !== filesystemStatusKey(nextEntry)) {
      notifyFilesystemWatchListener(listener, {
        eventType: 'change',
        filename: name,
      });
    }
  }
  for (const name of nextEntries.keys()) {
    if (!previousEntries.has(name)) {
      notifyFilesystemWatchListener(listener, {
        eventType: 'rename',
        filename: name,
      });
    }
  }
};

const createMuonAndroidFsApi = (
  client: MuonWebViewRpcClient,
  capabilityId: string
): MuonAndroidFsApi => {
  const call = (
    functionName: (typeof filesystemFunctionNames)[number],
    arguments_: readonly unknown[],
    options: MuonAndroidFsOperationOptions | undefined
  ): Promise<unknown> => {
    const signal = getOperationSignal(options);
    return client.call(
      capabilityId,
      `muon.fs.${functionName}`,
      arguments_,
      signal === undefined ? undefined : { signal }
    );
  };

  const callVoid = async (
    functionName: (typeof filesystemFunctionNames)[number],
    arguments_: readonly unknown[],
    options: MuonAndroidFsOperationOptions | undefined
  ): Promise<void> => {
    await call(functionName, arguments_, options);
  };

  const callString = async (
    functionName: (typeof filesystemFunctionNames)[number],
    arguments_: readonly unknown[],
    options: MuonAndroidFsOperationOptions | undefined
  ): Promise<string> => {
    const value = await call(functionName, arguments_, options);
    if (typeof value !== 'string') {
      throw new TypeError('Android filesystem string result is invalid');
    }
    return value;
  };

  const callBoolean = async (
    functionName: (typeof filesystemFunctionNames)[number],
    arguments_: readonly unknown[],
    options: MuonAndroidFsOperationOptions | undefined
  ): Promise<boolean> => {
    const value = await call(functionName, arguments_, options);
    if (typeof value !== 'boolean') {
      throw new TypeError('Android filesystem boolean result is invalid');
    }
    return value;
  };

  const api: MuonAndroidFsApi = {
    readFile: async (path, options) => {
      const value = await call(
        'readFile',
        [validatePath(path), normalizeReadFileOptions(options)],
        options
      );
      if (!(value instanceof ArrayBuffer)) {
        throw new TypeError('Android filesystem binary result is invalid');
      }
      return value;
    },
    writeFile: async (path, data, options) =>
      await callVoid(
        'writeFile',
        [
          validatePath(path),
          validateBufferSource(data),
          normalizeWriteFileOptions(options),
        ],
        options
      ),
    readTextFile: async (path, encoding, options) =>
      await callString(
        'readTextFile',
        [validatePath(path), validateEncoding(encoding)],
        options
      ),
    writeTextFile: async (path, data, encoding, options) => {
      if (typeof data !== 'string') {
        throw new TypeError('data must be a string');
      }
      await callVoid(
        'writeTextFile',
        [validatePath(path), data, validateEncoding(encoding)],
        options
      );
    },
    stat: async (path, options) =>
      await parseStats(call('stat', [validatePath(path)], options)),
    lstat: async (path, options) =>
      await parseStats(call('lstat', [validatePath(path)], options)),
    exists: async (path, options) =>
      await callBoolean('exists', [validatePath(path)], options),
    access: async (path, options) =>
      await callBoolean(
        'access',
        [validatePath(path), normalizeAccessOptions(options)],
        options
      ),
    readdir: async (path, options) => {
      const nativeOptions = normalizeBooleanOptions(options, ['withFileTypes']);
      return await parseDirectoryEntries(
        call('readdir', [validatePath(path), nativeOptions], options),
        nativeOptions.withFileTypes === true
      );
    },
    mkdir: async (path, options) =>
      await callVoid(
        'mkdir',
        [validatePath(path), normalizeBooleanOptions(options, ['recursive'])],
        options
      ),
    rm: async (path, options) =>
      await callVoid(
        'rm',
        [
          validatePath(path),
          normalizeBooleanOptions(options, ['recursive', 'force']),
        ],
        options
      ),
    unlink: async (path, options) =>
      await callVoid('unlink', [validatePath(path)], options),
    rmdir: async (path, options) =>
      await callVoid('rmdir', [validatePath(path)], options),
    rename: async (oldPath, newPath, options) =>
      await callVoid(
        'rename',
        [validatePath(oldPath, 'oldPath'), validatePath(newPath, 'newPath')],
        options
      ),
    copyFile: async (source, destination, options) =>
      await callVoid(
        'copyFile',
        [
          validatePath(source, 'source'),
          validatePath(destination, 'destination'),
          normalizeBooleanOptions(options, ['overwrite']),
        ],
        options
      ),
    appendFile: async (path, data, options) =>
      await callVoid(
        'appendFile',
        [validatePath(path), validateBufferSource(data)],
        options
      ),
    appendTextFile: async (path, data, encoding, options) => {
      if (typeof data !== 'string') {
        throw new TypeError('data must be a string');
      }
      await callVoid(
        'appendTextFile',
        [validatePath(path), data, validateEncoding(encoding)],
        options
      );
    },
    truncate: async (path, lengthOrOptions, options) => {
      const operationOptions =
        typeof lengthOrOptions === 'object' && lengthOrOptions !== null
          ? lengthOrOptions
          : options;
      const length =
        typeof lengthOrOptions === 'number'
          ? validateSafeUnsignedInteger(lengthOrOptions, 'length')
          : 0;
      await callVoid(
        'truncate',
        [validatePath(path), { length }],
        operationOptions
      );
    },
    realpath: async (path, options) =>
      await callString('realpath', [validatePath(path)], options),
    readlink: async (path, options) =>
      await callString('readlink', [validatePath(path)], options),
    symlink: async (target, path, typeOrOptions, options) => {
      if (typeof target !== 'string' || target.length === 0) {
        throw new TypeError('target must be a non-empty string');
      }
      if (target.includes('\0')) {
        throw new TypeError('target must not contain NUL');
      }
      const operationOptions =
        typeof typeOrOptions === 'object' && typeOrOptions !== null
          ? typeOrOptions
          : options;
      const type = typeof typeOrOptions === 'string' ? typeOrOptions : 'file';
      if (type !== 'file' && type !== 'dir' && type !== 'junction') {
        throw new TypeError('type must be file, dir, or junction');
      }
      if (type === 'junction') {
        throw new TypeError(
          'junction symbolic links are unavailable on Android'
        );
      }
      await callVoid(
        'symlink',
        [target, validatePath(path), type],
        operationOptions
      );
    },
    watch: async (path, listener, options) => {
      const validatedPath = validatePath(path);
      if (typeof listener !== 'function') {
        throw new TypeError('listener must be a function');
      }
      const signal = getOperationSignal(options);
      if (signal !== undefined && Boolean(signal.aborted)) {
        throw createAbortReason(signal);
      }
      const runWatchRpc = async (
        request: Readonly<Record<string, unknown>>,
        operationOptions: MuonAndroidFsOperationOptions | undefined
      ): Promise<Record<string, unknown>> =>
        await parseNativeJson<Record<string, unknown>>(
          call('watch', [request], operationOptions)
        );
      const lease = await runWatchRpc({ operation: 'acquire' }, undefined);
      if (typeof lease.token !== 'string' || lease.token.length === 0) {
        throw new TypeError('Android filesystem watcher lease is invalid');
      }
      const token = lease.token;
      let released = false;
      const releaseLease = async (): Promise<void> => {
        if (released) {
          return;
        }
        released = true;
        try {
          await runWatchRpc({ operation: 'release', token }, undefined);
        } catch {
          // Context release is the final fallback for an unavailable transport.
        }
      };
      if (signal !== undefined && Boolean(signal.aborted)) {
        await releaseLease();
        throw createAbortReason(signal);
      }

      let snapshot: MuonAndroidFsWatchSnapshot;
      try {
        snapshot = await parseWatchSnapshot(
          call(
            'watch',
            [{ operation: 'snapshot', path: validatedPath, token }],
            options
          )
        );
      } catch (error) {
        await releaseLease();
        throw error;
      }

      let closed = false;
      let polling = false;
      let pollPromise: Promise<void> | undefined = undefined;
      let timer: ReturnType<typeof globalThis.setInterval> | undefined =
        undefined;
      let closePromise: Promise<void> | undefined = undefined;
      const beginClose = async (waitForPoll: boolean): Promise<void> => {
        if (closePromise !== undefined) {
          await closePromise;
          return;
        }
        const pendingPoll = waitForPoll ? pollPromise : undefined;
        closePromise = (async () => {
          if (closed) {
            return;
          }
          closed = true;
          if (timer !== undefined) {
            globalThis.clearInterval(timer);
            timer = undefined;
          }
          if (signal !== undefined) {
            signal.removeEventListener('abort', onAbort);
          }
          if (pendingPoll !== undefined) {
            try {
              await pendingPoll;
            } catch {
              // Poll errors are reported to the listener before lease release.
            }
          }
          await releaseLease();
        })();
        await closePromise;
      };
      const close = async (): Promise<void> => {
        await beginClose(true);
      };
      const onAbort = async (): Promise<void> => {
        await close();
      };
      const runPoll = async (): Promise<void> => {
        try {
          const next = await parseWatchSnapshot(
            call(
              'watch',
              [{ operation: 'snapshot', path: validatedPath, token }],
              undefined
            )
          );
          if (closed) {
            return;
          }
          emitFilesystemWatchDiff(snapshot, next, listener);
          snapshot = next;
        } catch (error) {
          if (closed) {
            return;
          }
          notifyFilesystemWatchListener(listener, {
            eventType: 'error',
            filename: null,
            message: error instanceof Error ? error.message : String(error),
          });
          await beginClose(false);
        }
      };
      const poll = async (): Promise<void> => {
        if (closed || polling) {
          return;
        }
        polling = true;
        const currentPoll = runPoll();
        pollPromise = currentPoll;
        try {
          await currentPoll;
        } finally {
          if (pollPromise === currentPoll) {
            pollPromise = undefined;
          }
          polling = false;
        }
      };
      timer = globalThis.setInterval(async () => {
        await poll();
      }, 100);
      if (signal !== undefined) {
        signal.addEventListener('abort', onAbort, { once: true });
        if (signal.aborted) {
          await close();
          throw createAbortReason(signal);
        }
      }
      return Object.freeze({ close });
    },
  };
  return Object.freeze(api);
};

/**
 * Creates the Android simple-mode `window.muon` object.
 *
 * @param client - WebView RPC client used by every exposed function.
 * @param capabilityIds - Namespace capability ids generated for this page.
 * @returns An immutable object containing only implemented Android functions.
 */
export const createMuonAndroidSimpleApi = (
  client: MuonWebViewRpcClient,
  capabilityIds: MuonAndroidCapabilityIds
): MuonAndroidSimpleApi =>
  Object.freeze({
    browser: createVoidNamespace(
      client,
      capabilityIds['muon.browser'],
      'muon.browser',
      browserFunctionNames
    ),
    environments: Object.freeze({
      getVariables: () =>
        parseNativeJson<Record<string, string>>(
          client.call(
            capabilityIds['muon.environments'],
            'muon.environments.getVariables',
            []
          )
        ),
      getConfigValues: () =>
        parseNativeJson<Record<string, string>>(
          client.call(
            capabilityIds['muon.environments'],
            'muon.environments.getConfigValues',
            []
          )
        ),
      getProcessId: async () => {
        const value = await client.call(
          capabilityIds['muon.environments'],
          'muon.environments.getProcessId',
          []
        );
        if (
          typeof value !== 'number' ||
          !Number.isInteger(value) ||
          value <= 0 ||
          value > 0xffffffff
        ) {
          throw new TypeError('Android process id result is invalid');
        }
        return value;
      },
      getRuntimeInfo: () =>
        parseNativeJson<MuonAndroidRuntimeInfo>(
          client.call(
            capabilityIds['muon.environments'],
            'muon.environments.getRuntimeInfo',
            []
          )
        ),
    }),
    fs: createMuonAndroidFsApi(client, capabilityIds['muon.fs']),
  });
