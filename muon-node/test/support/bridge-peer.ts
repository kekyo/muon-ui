// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { PassThrough, type Readable, type Writable } from 'node:stream';

import { vi } from 'vitest';

export const muonNodeProtocol = 'muon-node/1';

export interface WireRequest {
  kind: 'request';
  id: string;
  command: string;
  params: Readonly<Record<string, unknown>>;
}

export interface WireSuccessResponse {
  kind: 'response';
  id: string;
  ok: true;
  value: unknown;
}

export interface WireFailureResponse {
  kind: 'response';
  id: string;
  ok: false;
  error: {
    code: string;
    message: string;
  };
}

export interface WireCallbackRequest {
  kind: 'callback';
  id: string;
  handle: string;
  arguments: readonly unknown[];
}

export interface WireCallbackResult {
  kind: 'callbackResult';
  id: string;
  ok: boolean;
  value?: unknown;
  error?: {
    code: string;
    message: string;
  };
}

export type WireResponse = WireSuccessResponse | WireFailureResponse;
export type WireMessage =
  | WireRequest
  | WireResponse
  | WireCallbackRequest
  | WireCallbackResult;

interface MuonNodeBridge {
  waitForShutdown(): Promise<void>;
  close(): Promise<void>;
}

interface MuonNodeBridgeModule {
  createMuonNodeBridge(options: {
    input: Readable;
    output: Writable;
    changeWorkingDirectory: boolean;
    expectedToken?: string;
  }): MuonNodeBridge | Promise<MuonNodeBridge>;
}

interface MessageWaiter {
  resolve(message: WireMessage): void;
  reject(error: Error): void;
}

export interface BridgePeer {
  sendRequest(
    command: string,
    params: Readonly<Record<string, unknown>>
  ): string;
  request(
    command: string,
    params: Readonly<Record<string, unknown>>
  ): Promise<WireResponse>;
  nextMessage(): Promise<WireMessage>;
  nextResponse(id: string): Promise<WireResponse>;
  send(message: WireMessage): void;
  sendRaw(bytes: Uint8Array): void;
  sendCombined(messages: readonly WireMessage[]): void;
  sendFragmented(message: WireMessage, fragmentSizes: readonly number[]): void;
  waitForShutdown(): Promise<void>;
  close(): Promise<void>;
}

export const encodeWireFrame = (message: WireMessage): Buffer => {
  const payload = Buffer.from(JSON.stringify(message), 'utf8');
  const header = Buffer.alloc(4);
  header.writeUInt32BE(payload.length);
  return Buffer.concat([header, payload]);
};

const decodeWireMessage = (payload: Buffer): WireMessage => {
  const parsed = JSON.parse(payload.toString('utf8')) as unknown;
  if (typeof parsed !== 'object' || parsed === null) {
    throw new Error('Wire frame payload must be an object.');
  }
  return parsed as WireMessage;
};

const createMessageReader = (
  output: PassThrough
): {
  nextMessage(): Promise<WireMessage>;
  stop(): void;
} => {
  let pending = Buffer.alloc(0);
  let failure: Error | undefined = undefined;
  const messages: WireMessage[] = [];
  const waiters: MessageWaiter[] = [];

  const fail = (error: Error): void => {
    if (failure !== undefined) {
      return;
    }
    failure = error;
    for (const waiter of waiters.splice(0)) {
      waiter.reject(error);
    }
  };

  const deliver = (message: WireMessage): void => {
    const waiter = waiters.shift();
    if (waiter === undefined) {
      messages.push(message);
      return;
    }
    waiter.resolve(message);
  };

  const readFrames = (): void => {
    while (pending.length >= 4) {
      const payloadLength = pending.readUInt32BE(0);
      const frameLength = 4 + payloadLength;
      if (pending.length < frameLength) {
        return;
      }
      const payload = pending.subarray(4, frameLength);
      pending = pending.subarray(frameLength);
      try {
        deliver(decodeWireMessage(payload));
      } catch (error) {
        fail(error instanceof Error ? error : new Error(String(error)));
        return;
      }
    }
  };

  output.on('data', (chunk: Buffer | string) => {
    pending = Buffer.concat([
      pending,
      typeof chunk === 'string' ? Buffer.from(chunk) : chunk,
    ]);
    readFrames();
  });
  output.on('error', (error) => {
    fail(error);
  });
  output.on('end', () => {
    if (pending.length !== 0) {
      fail(new Error('Bridge output ended with an incomplete frame.'));
    }
  });

  return {
    nextMessage: async (): Promise<WireMessage> => {
      const message = messages.shift();
      if (message !== undefined) {
        return message;
      }
      if (failure !== undefined) {
        throw failure;
      }
      return await new Promise<WireMessage>((resolve, reject) => {
        waiters.push({ resolve, reject });
      });
    },
    stop: (): void => {
      output.removeAllListeners();
      fail(new Error('Bridge peer stopped.'));
    },
  };
};

export const createBridgePeer = async (
  expectedToken?: string
): Promise<BridgePeer> => {
  const input = new PassThrough();
  const output = new PassThrough();
  const reader = createMessageReader(output);
  const bridgeModule =
    await vi.importActual<MuonNodeBridgeModule>('../../src/index.js');
  const bridge = await bridgeModule.createMuonNodeBridge({
    input,
    output,
    changeWorkingDirectory: false,
    ...(expectedToken === undefined ? {} : { expectedToken }),
  });
  let nextRequestId = 1;
  let closed = false;

  const send = (message: WireMessage): void => {
    input.write(encodeWireFrame(message));
  };

  const sendRequest = (
    command: string,
    params: Readonly<Record<string, unknown>>
  ): string => {
    const id = String(nextRequestId);
    nextRequestId += 1;
    send({
      kind: 'request',
      id,
      command,
      params,
    });
    return id;
  };

  const nextResponse = async (id: string): Promise<WireResponse> => {
    for (;;) {
      const message = await reader.nextMessage();
      if (message.kind === 'response' && message.id === id) {
        return message;
      }
      throw new Error(
        `Unexpected wire message while waiting for response ${id}.`
      );
    }
  };

  return {
    sendRequest,
    request: async (command, params): Promise<WireResponse> => {
      const id = sendRequest(command, params);
      return await nextResponse(id);
    },
    nextMessage: async (): Promise<WireMessage> => await reader.nextMessage(),
    nextResponse,
    send,
    sendRaw: (bytes): void => {
      input.write(bytes);
    },
    sendCombined: (messages): void => {
      input.write(Buffer.concat(messages.map(encodeWireFrame)));
    },
    sendFragmented: (message, fragmentSizes): void => {
      const frame = encodeWireFrame(message);
      let offset = 0;
      for (const requestedSize of fragmentSizes) {
        if (offset >= frame.length) {
          break;
        }
        const size = Math.max(
          0,
          Math.min(requestedSize, frame.length - offset)
        );
        input.write(frame.subarray(offset, offset + size));
        offset += size;
      }
      if (offset < frame.length) {
        input.write(frame.subarray(offset));
      }
    },
    waitForShutdown: async (): Promise<void> => {
      await bridge.waitForShutdown();
    },
    close: async (): Promise<void> => {
      if (closed) {
        return;
      }
      closed = true;
      input.end();
      await bridge.close();
      reader.stop();
      output.destroy();
    },
  };
};

export const initializeBridgePeer = async (
  projectRoot: string
): Promise<BridgePeer> => {
  const peer = await createBridgePeer();
  const response = await peer.request('initialize', {
    protocol: muonNodeProtocol,
    projectRoot,
  });
  if (!response.ok) {
    await peer.close();
    throw new Error(
      `Bridge initialization failed: ${response.error.code}: ${response.error.message}`
    );
  }
  return peer;
};

export const requireSuccessValue = (response: WireResponse): unknown => {
  if (!response.ok) {
    throw new Error(
      `Expected success, received ${response.error.code}: ${response.error.message}`
    );
  }
  return response.value;
};
