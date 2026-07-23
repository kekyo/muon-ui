// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { describe, expect, it } from 'vitest';

import {
  createBridgePeer,
  muonNodeProtocol,
  type WireRequest,
  type WireResponse,
} from './support/bridge-peer.js';
import { createProjectFixture } from './support/project-fixture.js';

const findResponse = (
  messages: readonly unknown[],
  id: string
): WireResponse | undefined =>
  messages.find(
    (message): message is WireResponse =>
      typeof message === 'object' &&
      message !== null &&
      'kind' in message &&
      message.kind === 'response' &&
      'id' in message &&
      message.id === id
  );

const encodePayloadFrame = (payload: Uint8Array): Buffer => {
  const header = Buffer.alloc(4);
  header.writeUInt32BE(payload.byteLength);
  return Buffer.concat([header, payload]);
};

describe('muon Node framed protocol', () => {
  it('rejects an invalid session token without accepting initialization', async () => {
    const project = await createProjectFixture();
    const peer = await createBridgePeer('expected-token');
    try {
      await expect(
        peer.request('initialize', {
          protocol: muonNodeProtocol,
          projectRoot: project.root,
          token: 'wrong-token',
        })
      ).resolves.toMatchObject({
        ok: false,
        error: {
          code: 'ERR_MUON_NODE_AUTH',
        },
      });
      await expect(
        peer.request('initialize', {
          protocol: muonNodeProtocol,
          projectRoot: project.root,
          token: 'expected-token',
        })
      ).resolves.toMatchObject({
        ok: true,
      });
    } finally {
      await peer.close();
      await project.dispose();
    }
  });

  it('accepts a frame split across header and payload boundaries', async () => {
    const project = await createProjectFixture();
    const peer = await createBridgePeer();
    try {
      const request: WireRequest = {
        kind: 'request',
        id: 'fragmented-initialize',
        command: 'initialize',
        params: {
          protocol: muonNodeProtocol,
          projectRoot: project.root,
        },
      };

      peer.sendFragmented(request, [1, 2, 1, 3, 5, 8]);

      await expect(peer.nextResponse(request.id)).resolves.toEqual({
        kind: 'response',
        id: request.id,
        ok: true,
        value: {
          protocol: muonNodeProtocol,
          nodeVersion: process.version,
        },
      });
    } finally {
      await peer.close();
      await project.dispose();
    }
  });

  it('decodes coalesced frames without losing request boundaries', async () => {
    const project = await createProjectFixture();
    const peer = await createBridgePeer();
    try {
      const initializeRequest: WireRequest = {
        kind: 'request',
        id: 'coalesced-initialize',
        command: 'initialize',
        params: {
          protocol: muonNodeProtocol,
          projectRoot: project.root,
        },
      };
      const importRequest: WireRequest = {
        kind: 'request',
        id: 'coalesced-import',
        command: 'importModule',
        params: {
          specifier: './backend.js',
        },
      };

      peer.sendCombined([initializeRequest, importRequest]);

      const messages = [await peer.nextMessage(), await peer.nextMessage()];
      expect(findResponse(messages, initializeRequest.id)).toMatchObject({
        ok: true,
      });
      expect(findResponse(messages, importRequest.id)).toMatchObject({
        ok: true,
        value: {
          moduleId: expect.any(String),
          descriptor: {
            exports: expect.arrayContaining([
              {
                name: 'echo',
                kind: 'function',
              },
            ]),
          },
        },
      });
    } finally {
      await peer.close();
      await project.dispose();
    }
  });

  it('rejects shutdown waiting when protocol input is fatally invalid', async () => {
    const peer = await createBridgePeer();
    try {
      peer.sendRaw(encodePayloadFrame(Buffer.from('{', 'utf8')));

      await expect(peer.waitForShutdown()).rejects.toMatchObject({
        code: 'ERR_MUON_NODE_PROTOCOL',
        message: expect.stringContaining('not valid JSON'),
      });
    } finally {
      await peer.close();
    }
  });
});
