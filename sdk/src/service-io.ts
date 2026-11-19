import type { ServiceCancellationSignal } from './service-connection.ts';

const ioErrorCodes = [
  'permission-denied',
  'not-found',
  'invalid-input',
  'io-error',
  'resource-exhausted',
  'size-limit',
  'timeout',
  'cancelled',
  'network-error',
  'tls-error',
  'redirect-limit',
] as const;

/** A stable error code from the portable file, HTTP or local listener API. */
export type ServiceIoErrorCode = (typeof ioErrorCodes)[number];

/** @param error A caught value. @returns Whether it carries a portable I/O error code and diagnostic. */
export const isServiceIoError = (
  error: unknown
): error is { readonly code: ServiceIoErrorCode; readonly message: string } => {
  if (
    !error ||
    typeof error !== 'object' ||
    !('code' in error) ||
    !('message' in error)
  )
    return false;
  return (
    typeof error.message === 'string' &&
    ioErrorCodes.some((code) => code === error.code)
  );
};

/** The metadata and binary payload returned by an operation on the owner dispatcher. */
export interface ServiceIoReply {
  /** Operation-specific metadata, such as a directory listing. */ readonly metadata: unknown;
  /** Owned bytes, empty for operations without a binary response. */ readonly body: Uint8Array;
}

/**
 * Executes an operation for the current generation.
 * @param operation - Portable operation name.
 * @param parameters - Operation-specific metadata.
 * @param body - Owned request bytes.
 * @param signal - Cancellation source for this operation.
 * @returns The operation's metadata and bytes, or an error with a framework code.
 */
export type ServiceIoBridge = (
  operation: string,
  parameters: Readonly<Record<string, unknown>>,
  body: Uint8Array,
  signal: ServiceCancellationSignal
) => Promise<ServiceIoReply>;

/** File access in the private data area, a named grant, or the read-only assets area. */
export interface ServiceFileOptions {
  /** Configured area name. Defaults to data. */ readonly area?: string;
  /** Cancellation source. Use the stop handler's signal when saving state at shutdown. */ readonly signal?: ServiceCancellationSignal;
}

/** Asynchronous file operations scoped to this service generation's grants. */
export interface ServiceFileSystem {
  /** @param path Relative file name. @param options Area and cancellation. @returns File contents. */
  readonly readBytes: (
    path: string,
    options?: ServiceFileOptions
  ) => Promise<Uint8Array>;
  /** @param path Relative file name. @param options Area and cancellation. @returns Strictly decoded UTF-8 text. */
  readonly readText: (
    path: string,
    options?: ServiceFileOptions
  ) => Promise<string>;
  /** @param path Relative destination. @param value Bytes to replace the file atomically. @param options Area and cancellation. */
  readonly writeBytes: (
    path: string,
    value: Uint8Array,
    options?: ServiceFileOptions
  ) => Promise<void>;
  /** @param path Relative destination. @param value UTF-8 text to replace the file atomically. @param options Area and cancellation. */
  readonly writeText: (
    path: string,
    value: string,
    options?: ServiceFileOptions
  ) => Promise<void>;
  /** @param path Relative directory to create in an existing parent. @param options Area and cancellation. */
  readonly mkdir: (path: string, options?: ServiceFileOptions) => Promise<void>;
  /** @param path Relative directory, or an empty string for its root. @param options Area and cancellation. @returns Sorted direct entry names. */
  readonly list: (
    path: string,
    options?: ServiceFileOptions
  ) => Promise<readonly string[]>;
  /** @param path Existing entry. @param destination New relative name in the same area. @param options Area and cancellation. */
  readonly rename: (
    path: string,
    destination: string,
    options?: ServiceFileOptions
  ) => Promise<void>;
}

/** @internal Constructs the portable file facade from a generation-bound host operation. */
export const createServiceFiles = (
  io: ServiceIoBridge,
  lifetime: ServiceCancellationSignal
): ServiceFileSystem => {
  const call = async (
    name: string,
    path: string,
    options: ServiceFileOptions | undefined,
    extra: Record<string, unknown>,
    bytes: Uint8Array | undefined
  ) =>
    await io(
      'fs.' + name,
      { area: options?.area ?? 'data', path, ...extra },
      bytes ?? new Uint8Array(),
      options?.signal ?? lifetime
    );
  return Object.freeze<ServiceFileSystem>({
    readBytes: async (path, options) =>
      (await call('read', path, options, {}, undefined)).body,
    readText: async (path, options) =>
      (await call('readText', path, options, {}, undefined)).metadata as string,
    writeBytes: async (path, value, options) => {
      if (!(value instanceof Uint8Array))
        throw new TypeError('File contents must be a Uint8Array');
      await call('write', path, options, {}, value.slice());
    },
    writeText: async (path, value, options) => {
      if (typeof value !== 'string')
        throw new TypeError('File contents must be text');
      await call('writeText', path, options, { value }, undefined);
    },
    mkdir: async (path, options) => {
      await call('mkdir', path, options, {}, undefined);
    },
    list: async (path, options) =>
      (await call('list', path, options, {}, undefined))
        .metadata as readonly string[],
    rename: async (path, destination, options) => {
      await call('rename', path, options, { destination }, undefined);
    },
  });
};
