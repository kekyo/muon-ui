// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

const maximumFramePayloadLength = 16 * 1024 * 1024;
const failure = (code: string, message: string) =>
  Object.assign(new Error(message), { code });

/**
 * Encodes a bounded JSON message for the Node.js service transport.
 * @param message - JSON-compatible protocol message.
 * @returns A big-endian four-byte length followed by UTF-8 JSON.
 * @throws Error when serialization fails or the payload exceeds 16 MiB.
 */
export const encodeMuonNodeFrame = (message: unknown): Buffer => {
  const payload = Buffer.from(JSON.stringify(message), 'utf8');
  if (payload.length > maximumFramePayloadLength)
    throw failure(
      'ERR_MUON_NODE_FRAME_TOO_LARGE',
      'The encoded protocol frame is too large.'
    );
  const header = Buffer.alloc(4);
  header.writeUInt32BE(payload.length);
  return Buffer.concat([header, payload]);
};

/** Stateful decoder owned by exactly one Node transport connection. */
export interface MuonNodeFrameDecoder {
  /** Receives bytes and delivers every complete message in wire order. */
  readonly write: (chunk: unknown) => void;
  /** Validates that EOF did not truncate a frame. */
  readonly finish: () => void;
}

/**
 * Decodes fragmented/coalesced frames without accepting malformed UTF-8.
 * @param receive - Validates and dispatches one parsed message synchronously.
 * @returns Decoder to retain until the owning connection closes.
 * @throws Error for malformed JSON, non-binary input, or oversized frames.
 */
export const createMuonNodeFrameDecoder = (
  receive: (message: unknown) => void
): MuonNodeFrameDecoder => {
  let pending = Buffer.alloc(0);
  const decoder = new TextDecoder('utf-8', { fatal: true });
  return {
    write: (chunk) => {
      const bytes =
        typeof chunk === 'string'
          ? Buffer.from(chunk)
          : Buffer.isBuffer(chunk)
            ? chunk
            : chunk instanceof Uint8Array
              ? Buffer.from(chunk)
              : undefined;
      if (bytes === undefined)
        throw failure(
          'ERR_MUON_NODE_PROTOCOL',
          'The protocol input emitted a non-binary chunk.'
        );
      pending = Buffer.concat([pending, bytes]);
      while (pending.length >= 4) {
        const length = pending.readUInt32BE(0);
        if (length > maximumFramePayloadLength)
          throw failure(
            'ERR_MUON_NODE_FRAME_TOO_LARGE',
            'The incoming protocol frame is too large.'
          );
        if (pending.length < length + 4) return;
        const payload = pending.subarray(4, length + 4);
        pending = pending.subarray(length + 4);
        let source: string;
        try {
          source = decoder.decode(payload);
        } catch {
          throw failure(
            'ERR_MUON_NODE_PROTOCOL',
            'The protocol frame payload is not valid UTF-8.'
          );
        }
        let parsed: unknown;
        try {
          parsed = JSON.parse(source);
        } catch (error) {
          throw failure(
            'ERR_MUON_NODE_PROTOCOL',
            `The protocol frame is not valid JSON: ${error instanceof Error ? error.message : String(error)}`
          );
        }
        receive(parsed);
      }
    },
    finish: () => {
      if (pending.length !== 0)
        throw failure(
          'ERR_MUON_NODE_PROTOCOL',
          'The protocol input ended with an incomplete frame.'
        );
    },
  };
};
