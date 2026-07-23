// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon

import { spawn, spawnSync, type ChildProcess } from 'node:child_process';
import { randomUUID } from 'node:crypto';
import { createServer, type Server, type Socket } from 'node:net';
import type { Writable } from 'node:stream';
import { fileURLToPath } from 'node:url';

import { describe, expect, it } from 'vitest';

const bridgePath = fileURLToPath(
  new URL('../dist/node-bridge.mjs', import.meta.url)
);

interface CliExit {
  status: number | null;
  signal: NodeJS.Signals | null;
  stderr: string;
}

const createFatalProtocolFrame = (): Buffer => {
  const payload = Buffer.from('{', 'utf8');
  const header = Buffer.alloc(4);
  header.writeUInt32BE(payload.length);
  return Buffer.concat([header, payload]);
};

const waitForCliExit = async (child: ChildProcess): Promise<CliExit> => {
  const stderr = child.stderr;
  if (stderr === null) {
    throw new Error('The CLI test requires a piped stderr stream.');
  }
  const chunks: Buffer[] = [];
  stderr.on('data', (chunk: Buffer | string) => {
    chunks.push(typeof chunk === 'string' ? Buffer.from(chunk) : chunk);
  });

  const result = await new Promise<CliExit>((resolvePromise, rejectPromise) => {
    const handleError = (error: Error): void => {
      child.removeListener('close', handleClose);
      rejectPromise(error);
    };
    const handleClose = (
      status: number | null,
      signal: NodeJS.Signals | null
    ): void => {
      child.removeListener('error', handleError);
      resolvePromise({
        status,
        signal,
        stderr: Buffer.concat(chunks).toString('utf8'),
      });
    };
    child.once('error', handleError);
    child.once('close', handleClose);
  });
  return result;
};

const requireWritableTransport = (
  transport: Writable | NodeJS.ReadableStream | null | undefined
): Writable => {
  if (
    transport === null ||
    transport === undefined ||
    !('write' in transport) ||
    !('end' in transport)
  ) {
    throw new Error('The CLI test transport is not writable.');
  }
  return transport as Writable;
};

const listenOnPipe = async (
  server: Server,
  pipeName: string
): Promise<void> => {
  await new Promise<void>((resolvePromise, rejectPromise) => {
    const handleError = (error: Error): void => {
      server.removeListener('listening', handleListening);
      rejectPromise(error);
    };
    const handleListening = (): void => {
      server.removeListener('error', handleError);
      resolvePromise();
    };
    server.once('error', handleError);
    server.once('listening', handleListening);
    server.listen(pipeName);
  });
};

const closeServer = async (server: Server): Promise<void> => {
  if (!server.listening) {
    return;
  }
  await new Promise<void>((resolvePromise) => {
    server.close(() => {
      resolvePromise();
    });
  });
};

const runPosixCliWithFatalInput = async (): Promise<CliExit> => {
  const environment: NodeJS.ProcessEnv = {
    ...process.env,
    MUON_NODE_FD: '3',
    MUON_NODE_TOKEN: 'fatal-protocol-test-token',
  };
  delete environment.MUON_NODE_PIPE;
  const child = spawn(process.execPath, [bridgePath], {
    env: environment,
    shell: false,
    stdio: ['ignore', 'ignore', 'pipe', 'pipe'],
    windowsHide: true,
  });
  const exitOperation = waitForCliExit(child);
  const transport = requireWritableTransport(child.stdio[3]);
  transport.end(createFatalProtocolFrame());
  return await exitOperation;
};

const runWindowsCliWithFatalInput = async (): Promise<CliExit> => {
  const pipeName = `\\\\.\\pipe\\muon-node-cli-test-${process.pid}-${randomUUID()}`;
  let acceptConnection: (socket: Socket) => void = (socket) => {
    socket.destroy();
  };
  const connection = new Promise<Socket>((resolvePromise) => {
    acceptConnection = resolvePromise;
  });
  const server = createServer((socket) => {
    acceptConnection(socket);
  });
  await listenOnPipe(server, pipeName);

  let socket: Socket | undefined = undefined;
  try {
    const environment: NodeJS.ProcessEnv = {
      ...process.env,
      MUON_NODE_PIPE: pipeName,
      MUON_NODE_TOKEN: 'fatal-protocol-test-token',
    };
    delete environment.MUON_NODE_FD;
    const child = spawn(process.execPath, [bridgePath], {
      env: environment,
      shell: false,
      stdio: ['ignore', 'ignore', 'pipe'],
      windowsHide: true,
    });
    const exitOperation = waitForCliExit(child);
    socket = await connection;
    socket.end(createFatalProtocolFrame());
    return await exitOperation;
  } finally {
    socket?.destroy();
    await closeServer(server);
  }
};

const runCliWithFatalInput = async (): Promise<CliExit> =>
  process.platform === 'win32'
    ? await runWindowsCliWithFatalInput()
    : await runPosixCliWithFatalInput();

describe('muon Node sidecar CLI', () => {
  it('writes a fatal startup error to stderr', () => {
    const environment = {
      ...process.env,
    };
    delete environment.MUON_NODE_TOKEN;
    delete environment.MUON_NODE_FD;
    delete environment.MUON_NODE_PIPE;

    const result = spawnSync(process.execPath, [bridgePath], {
      encoding: 'utf8',
      env: environment,
      shell: false,
      timeout: 10_000,
      windowsHide: true,
    });

    expect(result.error).toBeUndefined();
    expect(result.status).toBe(1);
    expect(result.stderr).toContain('MUON_NODE_TOKEN');
  });

  it('reports fatal protocol input and exits unsuccessfully', async () => {
    const result = await runCliWithFatalInput();

    expect(result.signal).toBeNull();
    expect(result.status).toBe(1);
    expect(result.stderr).toContain('not valid JSON');
  });
});
