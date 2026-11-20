// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { constants } from 'node:fs';
import {
  lstat,
  mkdir,
  open,
  readdir,
  rename,
  unlink,
  type FileHandle,
} from 'node:fs/promises';
import { randomUUID } from 'node:crypto';
import { join } from 'node:path';
import type {
  ServiceCancellationSignal,
  ServiceIoBridge,
} from 'muon-plugin-sdk/services';

interface FileGrant {
  readonly path: string;
  readonly access: string;
}
interface FileGrants {
  readonly data?: string;
  readonly assets?: Readonly<Record<string, string>>;
  readonly directories?: Readonly<Record<string, FileGrant>>;
}
/** Normalized permissions received from the authenticated parent host. */
export interface NodeServiceIoPolicy {
  /** File areas available to this service. */ readonly fs?: FileGrants;
}

const failure = (code: string, message: string) =>
  Object.assign(new Error(message), { code });
const denied = () => failure('permission-denied', 'Service file access denied');
const check = (signal: ServiceCancellationSignal) => {
  if (signal.aborted) throw failure('cancelled', 'Service I/O was cancelled');
};
const limit = 1024 * 1024;
const partsOf = (path: unknown, rootAllowed: boolean): string[] => {
  if (path === '' && rootAllowed) return [];
  if (typeof path !== 'string' || path.length > 4096) throw denied();
  const parts = path.split('/');
  if (
    parts.some(
      (part) =>
        !part ||
        part === '.' ||
        part.startsWith('.muon-tmp-') ||
        /[\\:\u0000-\u001f<>|"?*]|[ .]$/u.test(part) ||
        /^(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\.|$)/iu.test(part)
    )
  )
    throw denied();
  return parts;
};
const checkedEntry = async (path: string, missing: boolean) => {
  try {
    const entry = await lstat(path);
    if (
      entry.isSymbolicLink() ||
      (!entry.isDirectory() && (!entry.isFile() || entry.nlink !== 1))
    )
      throw denied();
    return entry;
  } catch (error) {
    if (missing && (error as NodeJS.ErrnoException).code === 'ENOENT')
      return undefined;
    throw error;
  }
};
const parentPath = async (
  root: string,
  parts: string[],
  handles: FileHandle[]
) => {
  let directory = root;
  const name = parts.pop();
  for (const segment of ['', ...parts]) {
    if (segment) directory = join(directory, segment);
    if (!(await checkedEntry(directory, false))?.isDirectory()) throw denied();
    const handle = await open(
      directory,
      constants.O_RDONLY | constants.O_DIRECTORY | constants.O_NOFOLLOW
    );
    handles.push(handle);
    directory = '/proc/self/fd/' + handle.fd;
  }
  return { directory, path: name ? join(directory, name) : directory };
};
const empty = () => ({ metadata: null, body: new Uint8Array() });
const readValue = (bytes: Uint8Array, asText: boolean) => {
  if (bytes.length > limit)
    throw failure('size-limit', 'Service file exceeds 1 MiB');
  return asText
    ? {
        metadata: new TextDecoder('utf-8', {
          fatal: true,
          ignoreBOM: true,
        }).decode(bytes),
        body: new Uint8Array(),
      }
    : { metadata: null, body: Uint8Array.from(bytes) };
};

/**
 * Implements asynchronous filesystem access for a single Node service generation.
 * @param dataDirectory - Service-private root selected by the parent host.
 * @param policy - File grants and immutable packaged assets.
 * @returns Operations that check the grant and cancellation before replacing a file.
 */
export const createNodeServiceIo =
  (dataDirectory: string, policy: NodeServiceIoPolicy): ServiceIoBridge =>
  async (operation, parameters, body, signal) => {
    const handles: FileHandle[] = [];
    try {
      check(signal);
      if (process.platform !== 'linux') throw denied();
      const grants = policy.fs;
      const area = parameters.area;
      if (!grants || typeof area !== 'string') throw denied();
      const parts = partsOf(parameters.path, operation === 'fs.list');
      const read = operation === 'fs.read' || operation === 'fs.readText';
      if (area === 'assets') {
        if ((!read && operation !== 'fs.list') || !grants.assets)
          throw denied();
        const name = parts.join('/');
        if (operation === 'fs.list') {
          const prefix = name ? name + '/' : '';
          const entries = Object.keys(grants.assets)
            .filter((key) => key.startsWith(prefix))
            .map((key) => key.slice(prefix.length).split('/')[0]!);
          if (name && !entries.length)
            throw failure('not-found', 'Unknown asset directory');
          return {
            metadata: [...new Set(entries)].sort(),
            body: new Uint8Array(),
          };
        }
        if (!Object.hasOwn(grants.assets, name))
          throw failure('not-found', 'Unknown service asset');
        return readValue(
          Buffer.from(grants.assets[name]!, 'base64'),
          operation === 'fs.readText'
        );
      }
      const grant =
        area === 'data'
          ? { path: dataDirectory, access: grants.data }
          : grants.directories && Object.hasOwn(grants.directories, area)
            ? grants.directories[area]
            : undefined;
      if (
        !grant ||
        (grant.access !== 'read-write' &&
          !(grant.access === 'read' && (read || operation === 'fs.list')))
      )
        throw denied();
      const { directory, path } = await parentPath(
        grant.path,
        operation === 'fs.list' ? [...parts, ''] : parts,
        handles
      );
      if (operation === 'fs.list' && path === directory) {
        return {
          metadata: (await readdir(directory))
            .filter((name) => !name.startsWith('.muon-tmp-'))
            .sort(),
          body: new Uint8Array(),
        };
      }
      const entry = await checkedEntry(path, true);
      check(signal);
      if (read) {
        if (!entry?.isFile())
          throw failure('not-found', 'Unknown service file');
        const file = await open(
          path,
          constants.O_RDONLY |
            (constants.O_NOFOLLOW ?? 0) |
            (constants.O_NONBLOCK ?? 0)
        );
        handles.push(file);
        const current = await file.stat();
        if (
          !current.isFile() ||
          current.nlink !== 1 ||
          current.dev !== entry.dev ||
          current.ino !== entry.ino
        )
          throw denied();
        if (current.size > limit)
          throw failure('size-limit', 'Service file exceeds 1 MiB');
        const bytes = Buffer.alloc(current.size + 1);
        let offset = 0;
        while (offset < bytes.length) {
          check(signal);
          const { bytesRead } = await file.read(
            bytes,
            offset,
            bytes.length - offset
          );
          if (!bytesRead) break;
          offset += bytesRead;
        }
        check(signal);
        if (offset > current.size)
          throw failure('io-error', 'File changed while reading');
        return readValue(
          bytes.subarray(0, offset),
          operation === 'fs.readText'
        );
      }
      if (operation === 'fs.mkdir') {
        await mkdir(path, { mode: 0o700 });
        return empty();
      }
      if (operation === 'fs.rename') {
        const destination = await parentPath(
          grant.path,
          partsOf(parameters.destination, false),
          handles
        );
        await checkedEntry(destination.path, true);
        check(signal);
        await rename(path, destination.path);
        return empty();
      }
      if (!['fs.write', 'fs.writeText'].includes(operation)) throw denied();
      if (operation === 'fs.writeText' && typeof parameters.value !== 'string')
        throw failure('invalid-input', 'Expected UTF-8 text');
      const bytes =
        operation === 'fs.writeText'
          ? Buffer.from(parameters.value as string)
          : body;
      if (bytes.length > limit)
        throw failure('size-limit', 'Service file exceeds 1 MiB');
      const temporary = join(directory, '.muon-tmp-' + randomUUID());
      const file = await open(temporary, 'wx', 0o600);
      let committed = false;
      try {
        try {
          await file.writeFile(bytes);
          await file.sync();
        } finally {
          await file.close();
        }
        check(signal);
        await rename(temporary, path);
        committed = true;
      } finally {
        if (!committed) await unlink(temporary);
      }
      return empty();
    } catch (error) {
      const code = (error as NodeJS.ErrnoException).code;
      if (
        code &&
        [
          'cancelled',
          'permission-denied',
          'size-limit',
          'not-found',
          'invalid-input',
          'io-error',
        ].includes(code)
      )
        throw error;
      if (['ELOOP', 'EACCES', 'EPERM', 'ENOTDIR'].includes(code ?? ''))
        throw denied();
      throw failure(
        code === 'ENOENT' ? 'not-found' : 'io-error',
        error instanceof Error ? error.message : 'File operation failed'
      );
    } finally {
      for (const handle of handles.reverse()) await handle.close();
    }
  };
