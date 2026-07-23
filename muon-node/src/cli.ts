// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { createConnection, Socket } from 'node:net';
import type { Readable, Writable } from 'node:stream';

import { createMuonNodeBridge } from './index.js';

interface SidecarConnection {
  input: Readable;
  output: Writable;
  close(): Promise<void>;
}

const requireEnvironment = (name: string): string => {
  const value = process.env[name];
  if (value === undefined || value.length === 0) {
    throw new Error(`${name} must contain a non-empty value.`);
  }
  return value;
};

const finishWritable = async (output: Writable): Promise<void> => {
  if (output.destroyed || output.writableEnded) {
    return;
  }
  await new Promise<void>((resolvePromise, rejectPromise) => {
    const handleError = (error: Error): void => {
      output.removeListener('finish', handleFinish);
      rejectPromise(error);
    };
    const handleFinish = (): void => {
      output.removeListener('error', handleError);
      resolvePromise();
    };
    output.once('error', handleError);
    output.once('finish', handleFinish);
    output.end();
  });
};

const writeStandardError = async (message: string): Promise<void> => {
  await new Promise<void>((resolvePromise, rejectPromise) => {
    process.stderr.write(message, (error) => {
      if (error !== null && error !== undefined) {
        rejectPromise(error);
        return;
      }
      resolvePromise();
    });
  });
};

const parseInheritedFileDescriptor = (source: string): number => {
  if (!/^[0-9]+$/.test(source)) {
    throw new Error('MUON_NODE_FD must be a decimal file descriptor.');
  }
  const fileDescriptor = Number(source);
  if (!Number.isSafeInteger(fileDescriptor) || fileDescriptor < 3) {
    throw new Error('MUON_NODE_FD must identify an inherited descriptor.');
  }
  return fileDescriptor;
};

const openInheritedFileDescriptor = (
  fileDescriptor: number
): SidecarConnection => {
  const socket = new Socket({
    fd: fileDescriptor,
    readable: true,
    writable: true,
    allowHalfOpen: true,
  });
  let closed = false;

  return {
    input: socket,
    output: socket,
    close: async (): Promise<void> => {
      if (closed) {
        return;
      }
      closed = true;
      try {
        await finishWritable(socket);
      } finally {
        socket.destroy();
      }
    },
  };
};

const waitForSocketConnection = async (socket: Socket): Promise<void> => {
  await new Promise<void>((resolvePromise, rejectPromise) => {
    const handleConnect = (): void => {
      socket.removeListener('error', handleError);
      resolvePromise();
    };
    const handleError = (error: Error): void => {
      socket.removeListener('connect', handleConnect);
      rejectPromise(error);
    };
    socket.once('connect', handleConnect);
    socket.once('error', handleError);
  });
};

const openWindowsNamedPipe = async (
  pipeName: string
): Promise<SidecarConnection> => {
  const socket = createConnection({
    path: pipeName,
    allowHalfOpen: true,
  });
  try {
    await waitForSocketConnection(socket);
  } catch (error) {
    socket.destroy();
    throw error;
  }
  let closed = false;

  return {
    input: socket,
    output: socket,
    close: async (): Promise<void> => {
      if (closed) {
        return;
      }
      closed = true;
      try {
        await finishWritable(socket);
      } finally {
        socket.destroy();
      }
    },
  };
};

const openSidecarConnection = async (): Promise<SidecarConnection> => {
  if (process.platform === 'win32') {
    return await openWindowsNamedPipe(requireEnvironment('MUON_NODE_PIPE'));
  }
  const fileDescriptor = parseInheritedFileDescriptor(
    requireEnvironment('MUON_NODE_FD')
  );
  return openInheritedFileDescriptor(fileDescriptor);
};

const runSidecar = async (): Promise<void> => {
  const expectedToken = requireEnvironment('MUON_NODE_TOKEN');
  delete process.env.MUON_NODE_TOKEN;
  const connection = await openSidecarConnection();
  const bridge = createMuonNodeBridge({
    input: connection.input,
    output: connection.output,
    expectedToken,
    changeWorkingDirectory: true,
  });

  try {
    await bridge.waitForShutdown();
  } finally {
    try {
      await bridge.close();
    } finally {
      await connection.close();
    }
  }
};

try {
  await runSidecar();
  process.exit(0);
} catch (error) {
  const message =
    error instanceof Error ? (error.stack ?? error.message) : String(error);
  try {
    await writeStandardError(`muon Node sidecar failed: ${message}\n`);
  } finally {
    process.exit(1);
  }
}
