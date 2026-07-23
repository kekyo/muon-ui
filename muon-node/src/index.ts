// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { timingSafeEqual } from 'node:crypto';
import { readFile, realpath } from 'node:fs/promises';
import { createRequire, isBuiltin } from 'node:module';
import {
  dirname,
  extname,
  isAbsolute,
  join,
  parse,
  resolve,
  sep,
} from 'node:path';
import type { Readable, Writable } from 'node:stream';
import { fileURLToPath, pathToFileURL } from 'node:url';

import { resolve as resolveImport } from 'import-meta-resolve';
import semverSatisfies from 'semver/functions/satisfies.js';
import semverValidRange from 'semver/ranges/valid.js';

const protocolName = 'muon-node/1';
const supportedNodeRange = '^20.19.0 || >=22.12.0';
const maximumFramePayloadLength = 16 * 1024 * 1024;
const signed64Minimum = -(2n ** 63n);
const signed64Maximum = 2n ** 63n - 1n;
const unsigned64Maximum = 2n ** 64n - 1n;
const reservedModuleExportNames = new Set(['$release', 'then']);
const utf8Decoder = new TextDecoder('utf-8', {
  fatal: true,
});

/**
 * Options used to host the muon Node protocol.
 */
export interface MuonNodeBridgeOptions {
  /**
   * Binary stream carrying framed requests and callback results.
   */
  input: Readable;

  /**
   * Binary stream receiving framed responses and callback requests.
   */
  output: Writable;

  /**
   * Whether initialization changes the sidecar process working directory to
   * the configured Node project.
   *
   * @remarks
   * Production sidecars enable this so project code observes the same working
   * directory as a conventional project entry point. In-process test hosts can
   * disable it to avoid mutating their owner process.
   */
  changeWorkingDirectory: boolean;

  /**
   * Token required in the initialize request.
   *
   * @remarks
   * Omit this only for an in-process trusted transport, such as a unit test.
   * The production CLI always supplies the token received from its environment.
   */
  expectedToken?: string;
}

/**
 * A running muon Node protocol host.
 */
export interface MuonNodeBridge {
  /**
   * Resolves after a shutdown request has been acknowledged or the input ends.
   *
   * @returns A promise that resolves when the owner should stop the sidecar.
   *
   * @remarks
   * Fatal protocol and transport errors reject the promise so the sidecar can
   * terminate unsuccessfully and report the diagnostic.
   */
  waitForShutdown(): Promise<void>;

  /**
   * Stops accepting input and releases bridge-owned state.
   *
   * @returns A promise that resolves after bridge-owned resources are released.
   *
   * @remarks
   * Arbitrary project promises cannot be cancelled, so close does not wait for
   * active calls to settle.
   */
  close(): Promise<void>;
}

interface BridgeError extends Error {
  code: string;
}

interface WireRequest {
  kind: 'request';
  id: string;
  command: string;
  params: Readonly<Record<string, unknown>>;
}

interface WireCallbackResult {
  kind: 'callbackResult';
  id: string;
  ok: boolean;
  value: unknown;
  error: {
    code: string;
    message: string;
  } | null;
}

interface WireSuccessResponse {
  kind: 'response';
  id: string;
  ok: true;
  value: unknown;
}

interface WireFailureResponse {
  kind: 'response';
  id: string;
  ok: false;
  error: {
    code: string;
    message: string;
  };
}

interface WireCallbackRequest {
  kind: 'callback';
  id: string;
  handle: string;
  arguments: readonly unknown[];
}

type OutgoingWireMessage =
  | WireSuccessResponse
  | WireFailureResponse
  | WireCallbackRequest;

interface PendingCallback {
  resolve(value: unknown): void;
  reject(error: Error): void;
}

interface ModuleExportDescriptor {
  name: string;
  kind: 'function' | 'primitive';
  value?: unknown;
}

interface ModuleExportRecord {
  value: unknown;
  receiver: unknown;
}

interface ModuleRecord {
  exports: ReadonlyMap<string, ModuleExportRecord>;
}

interface ProjectState {
  root: string;
  parentUrl: string;
}

interface CommandResult {
  value: unknown;
  shutdown: boolean;
  responseFailure: (() => void) | undefined;
}

interface ImportedModule {
  namespace: Readonly<Record<string, unknown>>;
  resolvedFile: string | undefined;
}

interface CollectedModuleExports {
  exports: ReadonlyMap<string, ModuleExportRecord>;
}

interface ActiveRequestOperation {
  operation: Promise<void> | undefined;
}

const createBridgeError = (code: string, message: string): BridgeError => {
  const error = new Error(message) as BridgeError;
  error.code = code;
  return error;
};

const isRecord = (value: unknown): value is Record<string, unknown> =>
  typeof value === 'object' && value !== null && !Array.isArray(value);

const isNodeError = (value: unknown): value is NodeJS.ErrnoException =>
  value instanceof Error;

const isProjectRelativeSpecifier = (specifier: string): boolean =>
  specifier === '.' ||
  specifier === '..' ||
  specifier.startsWith('./') ||
  specifier.startsWith('../');

const getErrorCode = (error: unknown, fallbackCode: string): string => {
  if (
    isNodeError(error) &&
    typeof error.code === 'string' &&
    error.code.length !== 0
  ) {
    return error.code;
  }
  return fallbackCode;
};

const getErrorMessage = (error: unknown): string =>
  error instanceof Error ? error.message : String(error);

const requireRecord = (
  value: unknown,
  code: string,
  description: string
): Readonly<Record<string, unknown>> => {
  if (!isRecord(value)) {
    throw createBridgeError(code, `${description} must be an object.`);
  }
  return value;
};

const requireString = (
  value: unknown,
  code: string,
  description: string
): string => {
  if (typeof value !== 'string' || value.length === 0) {
    throw createBridgeError(code, `${description} must be a non-empty string.`);
  }
  return value;
};

const requireArray = (
  value: unknown,
  code: string,
  description: string
): readonly unknown[] => {
  if (!Array.isArray(value)) {
    throw createBridgeError(code, `${description} must be an array.`);
  }
  return value;
};

const encodeFrame = (message: OutgoingWireMessage): Buffer => {
  const payload = Buffer.from(JSON.stringify(message), 'utf8');
  if (payload.length > maximumFramePayloadLength) {
    throw createBridgeError(
      'ERR_MUON_NODE_FRAME_TOO_LARGE',
      'The encoded protocol frame is too large.'
    );
  }
  const header = Buffer.allocUnsafe(4);
  header.writeUInt32BE(payload.length);
  return Buffer.concat([header, payload]);
};

const decodeIntegerTag = (
  value: Readonly<Record<string, unknown>>,
  kind: 'i64' | 'u64'
): bigint => {
  const decimal = value.value;
  if (typeof decimal !== 'string' || !/^(?:0|-?[1-9][0-9]*)$/.test(decimal)) {
    throw createBridgeError(
      'ERR_MUON_NODE_UNSUPPORTED_VALUE',
      `${kind} values must contain a canonical decimal string.`
    );
  }

  let decoded: bigint;
  try {
    decoded = BigInt(decimal);
  } catch {
    throw createBridgeError(
      'ERR_MUON_NODE_UNSUPPORTED_VALUE',
      `${kind} contains an invalid integer.`
    );
  }

  const inRange =
    kind === 'i64'
      ? decoded >= signed64Minimum && decoded <= signed64Maximum
      : decoded >= 0n && decoded <= unsigned64Maximum;
  if (!inRange) {
    throw createBridgeError(
      'ERR_MUON_NODE_UNSUPPORTED_VALUE',
      `${kind} is outside its supported range.`
    );
  }
  return decoded;
};

const decodeBufferTag = (value: Readonly<Record<string, unknown>>): Buffer => {
  const data = value.data;
  if (typeof data !== 'string') {
    throw createBridgeError(
      'ERR_MUON_NODE_UNSUPPORTED_VALUE',
      'buffer values must contain base64 data.'
    );
  }
  const decoded = Buffer.from(data, 'base64');
  if (decoded.toString('base64') !== data) {
    throw createBridgeError(
      'ERR_MUON_NODE_UNSUPPORTED_VALUE',
      'buffer contains non-canonical base64 data.'
    );
  }
  return decoded;
};

const getBinaryView = (value: unknown): Buffer | undefined => {
  if (Buffer.isBuffer(value)) {
    return value;
  }
  if (value instanceof ArrayBuffer) {
    return Buffer.from(value);
  }
  if (ArrayBuffer.isView(value)) {
    return Buffer.from(value.buffer, value.byteOffset, value.byteLength);
  }
  return undefined;
};

const encodeWireValue = (value: unknown): unknown => {
  if (value === undefined) {
    return {
      kind: 'undefined',
    };
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
      throw createBridgeError(
        'ERR_MUON_NODE_UNSUPPORTED_VALUE',
        'Only finite number values other than negative zero are supported.'
      );
    }
    return value;
  }
  if (typeof value === 'bigint') {
    if (value >= signed64Minimum && value <= signed64Maximum) {
      return {
        kind: 'i64',
        value: value.toString(),
      };
    }
    if (value >= 0n && value <= unsigned64Maximum) {
      return {
        kind: 'u64',
        value: value.toString(),
      };
    }
    throw createBridgeError(
      'ERR_MUON_NODE_UNSUPPORTED_VALUE',
      'The bigint value is outside the i64/u64 range.'
    );
  }
  const binary = getBinaryView(value);
  if (binary !== undefined) {
    return {
      kind: 'buffer',
      data: binary.toString('base64'),
    };
  }
  throw createBridgeError(
    'ERR_MUON_NODE_UNSUPPORTED_VALUE',
    'Only primitive, i64, u64, and buffer values can cross the bridge.'
  );
};

const tokensMatch = (actual: string, expected: string): boolean => {
  const actualBytes = Buffer.from(actual, 'utf8');
  const expectedBytes = Buffer.from(expected, 'utf8');
  return (
    actualBytes.length === expectedBytes.length &&
    timingSafeEqual(actualBytes, expectedBytes)
  );
};

const readPackageManifest = async (
  projectRoot: string
): Promise<Readonly<Record<string, unknown>>> => {
  const packageFile = join(projectRoot, 'package.json');
  let source: string;
  try {
    source = await readFile(packageFile, 'utf8');
  } catch (error) {
    throw createBridgeError(
      'ERR_MUON_NODE_PROJECT',
      `Unable to read the Node project package.json: ${getErrorMessage(error)}`
    );
  }

  let parsed: unknown;
  try {
    parsed = JSON.parse(source) as unknown;
  } catch (error) {
    throw createBridgeError(
      'ERR_MUON_NODE_PROJECT',
      `The Node project package.json is invalid: ${getErrorMessage(error)}`
    );
  }
  return requireRecord(
    parsed,
    'ERR_MUON_NODE_PROJECT',
    'The Node project package.json'
  );
};

const validateNodeEngine = (
  manifest: Readonly<Record<string, unknown>>
): void => {
  const enginesValue = manifest.engines;
  if (enginesValue === undefined) {
    return;
  }
  const engines = requireRecord(
    enginesValue,
    'ERR_MUON_NODE_ENGINE',
    'package.json engines'
  );
  const nodeRange = engines.node;
  if (nodeRange === undefined) {
    return;
  }
  if (typeof nodeRange !== 'string' || nodeRange.length === 0) {
    throw createBridgeError(
      'ERR_MUON_NODE_ENGINE',
      'package.json engines.node must be a non-empty string.'
    );
  }

  // node-semver documents satisfies(version, range) as the strict range
  // membership API. Its default prerelease exclusion is retained here.
  if (semverValidRange(nodeRange) === null) {
    throw createBridgeError(
      'ERR_MUON_NODE_ENGINE',
      `package.json engines.node is not a valid range: ${nodeRange}.`
    );
  }
  if (!semverSatisfies(process.version, nodeRange)) {
    throw createBridgeError(
      'ERR_MUON_NODE_ENGINE',
      `Node ${process.version} does not satisfy engines.node ${nodeRange}.`
    );
  }
};

const validateHostedNodeEngine = (): void => {
  if (!semverSatisfies(process.version, supportedNodeRange)) {
    throw createBridgeError(
      'ERR_MUON_NODE_ENGINE',
      `Node ${process.version} is not supported by the muon Node sidecar; expected ${supportedNodeRange}.`
    );
  }
};

const initializeProject = async (
  params: Readonly<Record<string, unknown>>,
  expectedToken: string | undefined,
  changeWorkingDirectory: boolean
): Promise<ProjectState> => {
  const requestedProtocol = requireString(
    params.protocol,
    'ERR_MUON_NODE_PROTOCOL',
    'initialize protocol'
  );
  if (requestedProtocol !== protocolName) {
    throw createBridgeError(
      'ERR_MUON_NODE_PROTOCOL',
      `Unsupported muon Node protocol ${requestedProtocol}.`
    );
  }

  if (expectedToken !== undefined) {
    const actualToken = requireString(
      params.token,
      'ERR_MUON_NODE_AUTH',
      'initialize token'
    );
    if (!tokensMatch(actualToken, expectedToken)) {
      throw createBridgeError(
        'ERR_MUON_NODE_AUTH',
        'The initialize token is invalid.'
      );
    }
  }

  validateHostedNodeEngine();

  const requestedRoot = requireString(
    params.projectRoot,
    'ERR_MUON_NODE_PROJECT',
    'initialize projectRoot'
  );
  let root: string;
  try {
    root = await realpath(resolve(requestedRoot));
  } catch (error) {
    throw createBridgeError(
      'ERR_MUON_NODE_PROJECT',
      `Unable to resolve the Node project: ${getErrorMessage(error)}`
    );
  }
  const manifest = await readPackageManifest(root);
  validateNodeEngine(manifest);
  if (changeWorkingDirectory) {
    try {
      process.chdir(root);
    } catch (error) {
      throw createBridgeError(
        'ERR_MUON_NODE_PROJECT',
        `Unable to use the Node project as the working directory: ${getErrorMessage(error)}`
      );
    }
  }
  return {
    root,
    parentUrl: pathToFileURL(join(root, 'package.json')).href,
  };
};

const isCommonJsFile = async (filePath: string): Promise<boolean> => {
  const extension = extname(filePath);
  if (extension === '.cjs') {
    return true;
  }
  if (extension === '.mjs') {
    return false;
  }
  if (extension !== '.js') {
    return false;
  }

  let directory = dirname(filePath);
  const filesystemRoot = parse(directory).root;
  while (true) {
    try {
      const source = await readFile(join(directory, 'package.json'), 'utf8');
      const manifest = JSON.parse(source) as unknown;
      return !isRecord(manifest) || manifest.type !== 'module';
    } catch (error) {
      if (!isNodeError(error) || error.code !== 'ENOENT') {
        throw error;
      }
    }
    if (directory === filesystemRoot) {
      return true;
    }
    directory = dirname(directory);
  }
};

const importNamespaceWithoutThenableAssimilation = async (
  specifier: string
): Promise<{ namespace: Readonly<Record<string, unknown>> }> => {
  // Dynamic import assimilates a module namespace that exports a callable
  // "then". A data-module wrapper captures the namespace through a static
  // import and exposes a non-thenable holder instead.
  const wrapperSource = [
    `import * as namespace from ${JSON.stringify(specifier)};`,
    'export default { namespace };',
  ].join('\n');
  const wrapperUrl = `data:text/javascript;charset=utf-8,${encodeURIComponent(wrapperSource)}`;
  const wrapperNamespace = (await import(
    /* @vite-ignore */ wrapperUrl
  )) as Readonly<Record<string, unknown>>;
  const holder = requireRecord(
    wrapperNamespace.default,
    'ERR_MUON_NODE_MODULE_SPECIFIER',
    'Internal module namespace holder'
  );
  return {
    namespace: requireRecord(
      holder.namespace,
      'ERR_MUON_NODE_MODULE_SPECIFIER',
      'Imported module namespace'
    ),
  };
};

const importProjectModule = async (
  project: ProjectState,
  specifier: string
): Promise<ImportedModule> => {
  if (specifier.startsWith('node:')) {
    if (!isBuiltin(specifier)) {
      throw createBridgeError(
        'ERR_MUON_NODE_BUILTIN_SPECIFIER',
        `${specifier} is not a Node built-in module.`
      );
    }
    const imported =
      await importNamespaceWithoutThenableAssimilation(specifier);
    return {
      namespace: imported.namespace,
      resolvedFile: undefined,
    };
  }
  if (isBuiltin(specifier)) {
    throw createBridgeError(
      'ERR_MUON_NODE_BUILTIN_SPECIFIER',
      `Node built-in modules must use the node: prefix: node:${specifier}.`
    );
  }

  if (isAbsolute(specifier) || /^[A-Za-z][A-Za-z+.-]*:/.test(specifier)) {
    throw createBridgeError(
      'ERR_MUON_NODE_MODULE_OUTSIDE_PROJECT',
      'Absolute module specifiers are not supported.'
    );
  }

  let resolvedUrl: URL;
  if (specifier === '.') {
    try {
      const projectRequire = createRequire(project.parentUrl);
      resolvedUrl = pathToFileURL(projectRequire.resolve(project.root));
    } catch (error) {
      throw createBridgeError(
        getErrorCode(error, 'ERR_MUON_NODE_MODULE_NOT_FOUND'),
        getErrorMessage(error)
      );
    }
  } else {
    try {
      resolvedUrl = new URL(resolveImport(specifier, project.parentUrl));
    } catch (error) {
      throw createBridgeError(
        getErrorCode(error, 'ERR_MUON_NODE_MODULE_NOT_FOUND'),
        getErrorMessage(error)
      );
    }
  }
  if (resolvedUrl.protocol !== 'file:') {
    throw createBridgeError(
      'ERR_MUON_NODE_MODULE_SPECIFIER',
      `Unsupported resolved module protocol ${resolvedUrl.protocol}.`
    );
  }
  const resolvedFile = fileURLToPath(resolvedUrl);
  if (isProjectRelativeSpecifier(specifier)) {
    if (
      resolvedFile !== project.root &&
      !resolvedFile.startsWith(`${project.root}${sep}`)
    ) {
      throw createBridgeError(
        'ERR_MUON_NODE_MODULE_OUTSIDE_PROJECT',
        'Project-relative imports cannot leave the configured project root.'
      );
    }
  }

  const imported = await importNamespaceWithoutThenableAssimilation(
    resolvedUrl.href
  );
  return {
    namespace: imported.namespace,
    resolvedFile,
  };
};

const collectModuleExports = async (
  imported: ImportedModule
): Promise<CollectedModuleExports> => {
  const exports = new Map<string, ModuleExportRecord>();
  for (const name of Object.keys(imported.namespace)) {
    exports.set(name, {
      value: imported.namespace[name],
      receiver: undefined,
    });
  }

  if (
    imported.resolvedFile !== undefined &&
    (await isCommonJsFile(imported.resolvedFile))
  ) {
    const commonJsExports = imported.namespace.default;
    if (
      commonJsExports !== null &&
      (typeof commonJsExports === 'object' ||
        typeof commonJsExports === 'function')
    ) {
      for (const name of Object.keys(commonJsExports)) {
        // The namespace default always represents module.exports itself.
        // A same-named property must not replace that callable facade entry.
        if (name === 'default') {
          continue;
        }
        exports.set(name, {
          value: Reflect.get(commonJsExports, name),
          receiver: commonJsExports,
        });
      }
    }
  }
  return {
    exports,
  };
};

const describeModuleExports = (
  exports: ReadonlyMap<string, ModuleExportRecord>
): readonly ModuleExportDescriptor[] => {
  const descriptor: ModuleExportDescriptor[] = [];
  const names = [...exports.keys()].sort();
  for (const name of names) {
    const value = exports.get(name)?.value;
    if (typeof value === 'function') {
      descriptor.push({
        name,
        kind: 'function',
        value: undefined,
      });
      continue;
    }
    try {
      descriptor.push({
        name,
        kind: 'primitive',
        value: encodeWireValue(value),
      });
    } catch (error) {
      if (getErrorCode(error, '') !== 'ERR_MUON_NODE_UNSUPPORTED_VALUE') {
        throw error;
      }
    }
  }
  return descriptor.map((entry) =>
    entry.kind === 'function'
      ? {
          name: entry.name,
          kind: entry.kind,
        }
      : entry
  );
};

const parseRequest = (value: unknown): WireRequest => {
  const message = requireRecord(
    value,
    'ERR_MUON_NODE_PROTOCOL',
    'Protocol message'
  );
  if (message.kind !== 'request') {
    throw createBridgeError(
      'ERR_MUON_NODE_PROTOCOL',
      'Expected a request or callbackResult message.'
    );
  }
  return {
    kind: 'request',
    id: requireString(message.id, 'ERR_MUON_NODE_PROTOCOL', 'Request id'),
    command: requireString(
      message.command,
      'ERR_MUON_NODE_PROTOCOL',
      'Request command'
    ),
    params: requireRecord(
      message.params,
      'ERR_MUON_NODE_PROTOCOL',
      'Request params'
    ),
  };
};

const parseCallbackResult = (value: unknown): WireCallbackResult => {
  const message = requireRecord(
    value,
    'ERR_MUON_NODE_PROTOCOL',
    'Callback result'
  );
  const id = requireString(
    message.id,
    'ERR_MUON_NODE_PROTOCOL',
    'Callback result id'
  );
  if (typeof message.ok !== 'boolean') {
    throw createBridgeError(
      'ERR_MUON_NODE_PROTOCOL',
      'Callback result ok must be a boolean.'
    );
  }
  if (message.ok) {
    return {
      kind: 'callbackResult',
      id,
      ok: true,
      value: message.value,
      error: null,
    };
  }
  const error = requireRecord(
    message.error,
    'ERR_MUON_NODE_PROTOCOL',
    'Callback result error'
  );
  return {
    kind: 'callbackResult',
    id,
    ok: false,
    value: undefined,
    error: {
      code: requireString(
        error.code,
        'ERR_MUON_NODE_PROTOCOL',
        'Callback error code'
      ),
      message: requireString(
        error.message,
        'ERR_MUON_NODE_PROTOCOL',
        'Callback error message'
      ),
    },
  };
};

/**
 * Creates a host for framed muon Node requests.
 *
 * @param options - Streams and optional transport authentication token.
 * @returns A bridge whose lifecycle remains owned by the caller.
 */
export const createMuonNodeBridge = (
  options: MuonNodeBridgeOptions
): MuonNodeBridge => {
  let pendingInput = Buffer.alloc(0);
  let project: ProjectState | undefined = undefined;
  let nextModuleId = 1;
  let nextCallbackId = 1;
  let initializationDrain: Promise<void> | undefined = undefined;
  let closeOperation: Promise<void> | undefined = undefined;
  let closing = false;
  let shutdownRequested = false;
  let shutdownSettled = false;
  const requests: WireRequest[] = [];
  const activeRequests = new Set<ActiveRequestOperation>();
  const modules = new Map<string, ModuleRecord>();
  const releasedModules = new Set<string>();
  const pendingCallbacks = new Map<string, PendingCallback>();
  let resolveShutdown: () => void = () => undefined;
  let rejectShutdown: (error: Error) => void = () => undefined;
  const shutdown = new Promise<void>((resolvePromise, rejectPromise) => {
    resolveShutdown = resolvePromise;
    rejectShutdown = rejectPromise;
  });

  const signalShutdown = (error: Error | undefined): void => {
    if (shutdownSettled) {
      return;
    }
    shutdownSettled = true;
    if (error === undefined) {
      resolveShutdown();
    } else {
      rejectShutdown(error);
    }
  };

  const writeMessage = async (message: OutgoingWireMessage): Promise<void> => {
    const frame = encodeFrame(message);
    await new Promise<void>((resolvePromise, rejectPromise) => {
      options.output.write(frame, (error: Error | null | undefined) => {
        if (error !== undefined && error !== null) {
          rejectPromise(error);
          return;
        }
        resolvePromise();
      });
    });
  };

  const rejectPendingCallbacks = (error: Error): void => {
    for (const callback of pendingCallbacks.values()) {
      callback.reject(error);
    }
    pendingCallbacks.clear();
  };

  const invokeRemoteCallback = async (
    handle: string,
    argumentsValue: readonly unknown[]
  ): Promise<unknown> => {
    if (closing) {
      throw createBridgeError(
        'ERR_MUON_NODE_CLOSED',
        'The muon Node bridge is closing.'
      );
    }

    const id = `callback-${nextCallbackId}`;
    nextCallbackId += 1;
    const encodedArguments = argumentsValue.map((value) =>
      encodeWireValue(value)
    );
    const result = new Promise<unknown>((resolvePromise, rejectPromise) => {
      pendingCallbacks.set(id, {
        resolve: resolvePromise,
        reject: rejectPromise,
      });
    });
    try {
      const [, callbackResult] = await Promise.all([
        writeMessage({
          kind: 'callback',
          id,
          handle,
          arguments: encodedArguments,
        }),
        result,
      ]);
      return callbackResult;
    } finally {
      pendingCallbacks.delete(id);
    }
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
        throw createBridgeError(
          'ERR_MUON_NODE_UNSUPPORTED_VALUE',
          'Only finite number values other than negative zero are supported.'
        );
      }
      return value;
    }

    const tagged = requireRecord(
      value,
      'ERR_MUON_NODE_UNSUPPORTED_VALUE',
      'Wire value'
    );
    if (tagged.kind === 'undefined') {
      return undefined;
    }
    if (tagged.kind === 'i64' || tagged.kind === 'u64') {
      return decodeIntegerTag(tagged, tagged.kind);
    }
    if (tagged.kind === 'buffer') {
      return decodeBufferTag(tagged);
    }
    if (tagged.kind === 'function') {
      const handle = requireString(
        tagged.handle,
        'ERR_MUON_NODE_UNSUPPORTED_VALUE',
        'Function handle'
      );
      return async (...argumentsValue: readonly unknown[]): Promise<unknown> =>
        await invokeRemoteCallback(handle, argumentsValue);
    }
    throw createBridgeError(
      'ERR_MUON_NODE_UNSUPPORTED_VALUE',
      'Arbitrary object values cannot cross the muon Node bridge.'
    );
  };

  const requireProject = (): ProjectState => {
    if (project === undefined) {
      throw createBridgeError(
        'ERR_MUON_NODE_NOT_INITIALIZED',
        'The muon Node bridge has not been initialized.'
      );
    }
    return project;
  };

  const importModule = async (
    params: Readonly<Record<string, unknown>>
  ): Promise<{
    moduleId: string;
    descriptor: {
      exports: readonly ModuleExportDescriptor[];
    };
  }> => {
    const currentProject = requireProject();
    const specifier = requireString(
      params.specifier,
      'ERR_MUON_NODE_MODULE_SPECIFIER',
      'Module specifier'
    );
    const imported = await importProjectModule(currentProject, specifier);
    const collected = await collectModuleExports(imported);
    for (const reservedName of reservedModuleExportNames) {
      if (collected.exports.has(reservedName)) {
        throw createBridgeError(
          'ERR_MUON_NODE_RESERVED_EXPORT',
          `Module export ${reservedName} is reserved by the muon Node facade.`
        );
      }
    }
    const descriptor = {
      exports: describeModuleExports(collected.exports),
    };
    const moduleId = `module-${nextModuleId}`;
    nextModuleId += 1;
    modules.set(moduleId, {
      exports: collected.exports,
    });
    return {
      moduleId,
      descriptor,
    };
  };

  const callModuleExport = async (
    params: Readonly<Record<string, unknown>>
  ): Promise<unknown> => {
    requireProject();
    const moduleId = requireString(
      params.moduleId,
      'ERR_MUON_NODE_UNKNOWN_HANDLE',
      'Module id'
    );
    if (releasedModules.has(moduleId)) {
      throw createBridgeError(
        'ERR_MUON_NODE_RELEASED_HANDLE',
        `Module handle ${moduleId} has been released.`
      );
    }
    const module = modules.get(moduleId);
    if (module === undefined) {
      throw createBridgeError(
        'ERR_MUON_NODE_UNKNOWN_HANDLE',
        `Unknown module handle ${moduleId}.`
      );
    }
    const exportName = requireString(
      params.exportName,
      'ERR_MUON_NODE_EXPORT',
      'Export name'
    );
    const exported = module.exports.get(exportName);
    if (exported === undefined || typeof exported.value !== 'function') {
      throw createBridgeError(
        'ERR_MUON_NODE_NOT_CALLABLE',
        `Module export ${exportName} is not callable.`
      );
    }
    const argumentsValue = requireArray(
      params.arguments,
      'ERR_MUON_NODE_UNSUPPORTED_VALUE',
      'Call arguments'
    ).map((value) => decodeWireValue(value));
    const result = await Reflect.apply(
      exported.value,
      exported.receiver,
      argumentsValue
    );
    return encodeWireValue(result);
  };

  const releaseHandle = (
    params: Readonly<Record<string, unknown>>
  ): unknown => {
    requireProject();
    const kind = requireString(
      params.kind,
      'ERR_MUON_NODE_UNKNOWN_HANDLE',
      'Release kind'
    );
    const handle = requireString(
      params.handle,
      'ERR_MUON_NODE_UNKNOWN_HANDLE',
      'Release handle'
    );
    if (kind === 'module') {
      const released = modules.delete(handle);
      releasedModules.add(handle);
      return {
        released,
      };
    }
    throw createBridgeError(
      'ERR_MUON_NODE_UNKNOWN_HANDLE',
      `Unsupported release kind ${kind}.`
    );
  };

  const executeCommand = async (
    request: WireRequest
  ): Promise<CommandResult> => {
    if (shutdownRequested) {
      throw createBridgeError(
        'ERR_MUON_NODE_CLOSED',
        'The muon Node bridge is shutting down.'
      );
    }
    switch (request.command) {
      case 'initialize': {
        if (project !== undefined) {
          throw createBridgeError(
            'ERR_MUON_NODE_ALREADY_INITIALIZED',
            'The muon Node bridge is already initialized.'
          );
        }
        project = await initializeProject(
          request.params,
          options.expectedToken,
          options.changeWorkingDirectory
        );
        return {
          value: {
            protocol: protocolName,
            nodeVersion: process.version,
          },
          shutdown: false,
          responseFailure: undefined,
        };
      }
      case 'importModule': {
        const imported = await importModule(request.params);
        return {
          value: imported,
          shutdown: false,
          responseFailure: () => {
            modules.delete(imported.moduleId);
          },
        };
      }
      case 'call':
        return {
          value: await callModuleExport(request.params),
          shutdown: false,
          responseFailure: undefined,
        };
      case 'release':
        return {
          value: releaseHandle(request.params),
          shutdown: false,
          responseFailure: undefined,
        };
      case 'shutdown':
        requireProject();
        shutdownRequested = true;
        return {
          value: {
            shutdown: true,
          },
          shutdown: true,
          responseFailure: undefined,
        };
      default:
        throw createBridgeError(
          'ERR_MUON_NODE_UNKNOWN_COMMAND',
          `Unknown muon Node command ${request.command}.`
        );
    }
  };

  const processRequest = async (request: WireRequest): Promise<void> => {
    const writeFailure = async (error: unknown): Promise<void> => {
      if (closing) {
        return;
      }
      await writeMessage({
        kind: 'response',
        id: request.id,
        ok: false,
        error: {
          code: getErrorCode(error, 'ERR_MUON_NODE_REQUEST'),
          message: getErrorMessage(error),
        },
      });
    };

    let result: CommandResult;
    try {
      result = await executeCommand(request);
    } catch (error) {
      await writeFailure(error);
      return;
    }

    if (closing) {
      result.responseFailure?.();
      return;
    }
    try {
      await writeMessage({
        kind: 'response',
        id: request.id,
        ok: true,
        value: result.value,
      });
    } catch (error) {
      result.responseFailure?.();
      if (getErrorCode(error, '') !== 'ERR_MUON_NODE_FRAME_TOO_LARGE') {
        throw error;
      }
      await writeFailure(error);
      return;
    }
    if (result.shutdown) {
      signalShutdown(undefined);
    }
  };

  const handleCallbackResult = (result: WireCallbackResult): void => {
    const callback = pendingCallbacks.get(result.id);
    if (callback === undefined) {
      throw createBridgeError(
        'ERR_MUON_NODE_UNKNOWN_CALLBACK',
        `Unknown callback result id ${result.id}.`
      );
    }
    pendingCallbacks.delete(result.id);
    if (result.ok) {
      try {
        callback.resolve(decodeWireValue(result.value));
      } catch (error) {
        callback.reject(
          error instanceof Error ? error : new Error(String(error))
        );
      }
      return;
    }
    const callbackError = result.error;
    if (callbackError === null) {
      callback.reject(
        createBridgeError(
          'ERR_MUON_NODE_PROTOCOL',
          'A failed callback result omitted its error.'
        )
      );
      return;
    }
    callback.reject(
      createBridgeError(callbackError.code, callbackError.message)
    );
  };

  const parseInputFrames = (): void => {
    while (pendingInput.length >= 4) {
      const payloadLength = pendingInput.readUInt32BE(0);
      if (payloadLength > maximumFramePayloadLength) {
        throw createBridgeError(
          'ERR_MUON_NODE_FRAME_TOO_LARGE',
          'The incoming protocol frame is too large.'
        );
      }
      const frameLength = payloadLength + 4;
      if (pendingInput.length < frameLength) {
        return;
      }

      const payload = pendingInput.subarray(4, frameLength);
      pendingInput = pendingInput.subarray(frameLength);
      let source: string;
      try {
        source = utf8Decoder.decode(payload);
      } catch {
        throw createBridgeError(
          'ERR_MUON_NODE_PROTOCOL',
          'The protocol frame payload is not valid UTF-8.'
        );
      }
      let decoded: unknown;
      try {
        decoded = JSON.parse(source) as unknown;
      } catch (error) {
        throw createBridgeError(
          'ERR_MUON_NODE_PROTOCOL',
          `The protocol frame is not valid JSON: ${getErrorMessage(error)}`
        );
      }

      if (isRecord(decoded) && decoded.kind === 'callbackResult') {
        handleCallbackResult(parseCallbackResult(decoded));
      } else {
        requests.push(parseRequest(decoded));
      }
    }
  };

  const handleFatalError = (error: unknown): void => {
    if (closing) {
      return;
    }
    closing = true;
    options.input.pause();
    const fatalError = createBridgeError(
      getErrorCode(error, 'ERR_MUON_NODE_PROTOCOL'),
      getErrorMessage(error)
    );
    rejectPendingCallbacks(fatalError);
    signalShutdown(fatalError);
  };

  const runActiveRequest = async (
    activeRequest: ActiveRequestOperation,
    request: WireRequest
  ): Promise<void> => {
    try {
      await processRequest(request);
    } catch (error) {
      handleFatalError(error);
    } finally {
      activeRequests.delete(activeRequest);
    }
  };

  const startActiveRequest = (request: WireRequest): void => {
    if (closing) {
      return;
    }
    const activeRequest: ActiveRequestOperation = {
      operation: undefined,
    };
    activeRequests.add(activeRequest);
    activeRequest.operation = runActiveRequest(activeRequest, request);
  };

  const scheduleRequests = (): void => {
    if (closing || requests.length === 0 || initializationDrain !== undefined) {
      return;
    }
    if (project !== undefined) {
      for (;;) {
        const request = requests.shift();
        if (request === undefined) {
          return;
        }
        startActiveRequest(request);
      }
    }

    initializationDrain = drainInitializationRequests();
  };

  const drainInitializationRequests = async (): Promise<void> => {
    try {
      while (!closing && project === undefined) {
        const request = requests.shift();
        if (request === undefined) {
          return;
        }
        await processRequest(request);
      }
      if (!closing) {
        for (;;) {
          const request = requests.shift();
          if (request === undefined) {
            return;
          }
          startActiveRequest(request);
        }
      }
    } catch (error) {
      handleFatalError(error);
    } finally {
      initializationDrain = undefined;
      scheduleRequests();
    }
  };

  const consumeInput = (chunk: unknown): void => {
    const bytes =
      typeof chunk === 'string'
        ? Buffer.from(chunk)
        : Buffer.isBuffer(chunk)
          ? chunk
          : chunk instanceof Uint8Array
            ? Buffer.from(chunk)
            : undefined;
    if (bytes === undefined) {
      throw createBridgeError(
        'ERR_MUON_NODE_PROTOCOL',
        'The protocol input emitted a non-binary chunk.'
      );
    }
    pendingInput = Buffer.concat([pendingInput, bytes]);
    parseInputFrames();
    scheduleRequests();
  };

  const handleInputData = (chunk: unknown): void => {
    try {
      consumeInput(chunk);
    } catch (error) {
      handleFatalError(error);
    }
  };

  const handleInputEnd = (): void => {
    if (pendingInput.length !== 0) {
      handleFatalError(
        createBridgeError(
          'ERR_MUON_NODE_PROTOCOL',
          'The protocol input ended with an incomplete frame.'
        )
      );
      return;
    }
    closing = true;
    rejectPendingCallbacks(
      createBridgeError(
        'ERR_MUON_NODE_CLOSED',
        'The muon Node protocol input ended.'
      )
    );
    signalShutdown(undefined);
  };

  const handleStreamError = (error: Error): void => {
    handleFatalError(error);
  };

  options.input.on('data', handleInputData);
  options.input.on('end', handleInputEnd);
  options.input.on('error', handleStreamError);
  options.output.on('error', handleStreamError);

  const performClose = async (): Promise<void> => {
    closing = true;
    options.input.removeListener('data', handleInputData);
    options.input.removeListener('end', handleInputEnd);
    options.input.removeListener('error', handleStreamError);
    rejectPendingCallbacks(
      createBridgeError(
        'ERR_MUON_NODE_CLOSED',
        'The muon Node bridge was closed.'
      )
    );
    options.output.removeListener('error', handleStreamError);
    requests.splice(0);
    activeRequests.clear();
    modules.clear();
    signalShutdown(undefined);
  };

  return {
    waitForShutdown: async (): Promise<void> => {
      await shutdown;
    },
    close: async (): Promise<void> => {
      if (closeOperation === undefined) {
        closeOperation = performClose();
      }
      await closeOperation;
    },
  };
};
