// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { createHash } from 'node:crypto';
import { once } from 'node:events';
import { createServer } from 'node:http';
import type { Duplex } from 'node:stream';
import {
  defineService,
  defineServiceInterface,
} from 'muon-plugin-sdk/services';

export default defineService({
  interface: defineServiceInterface({
    schemaVersion: 1,
    methods: { port: { parameters: [], returns: 'int32', errors: [] } },
    events: {},
  }),
  start: async (context) => {
    let peer: Duplex | undefined;
    const server = createServer((_request, response) =>
      response.end('listening')
    );
    // Only the opening handshake is needed. Stop drops the established transport.
    server.once('upgrade', (request, socket) => {
      const accept = createHash('sha1')
        .update(
          request.headers['sec-websocket-key'] +
            '258EAFA5-E914-47DA-95CA-C5AB0DC85B11'
        )
        .digest('base64');
      peer = socket;
      socket.write(
        'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n' +
          'Connection: Upgrade\r\nSec-WebSocket-Accept: ' +
          accept +
          '\r\n\r\n'
      );
    });
    server.listen(0, '127.0.0.1');
    await once(server, 'listening');
    const address = server.address();
    if (!address || typeof address === 'string')
      throw new Error('Expected a loopback TCP port');
    return {
      methods: { port: () => address.port },
      stop: async () => {
        peer?.destroy();
        try {
          const gate = await fetch(context.config.origin + '/listener-stop');
          await gate.text();
        } finally {
          const closed = once(server, 'close');
          server.close();
          await closed;
        }
      },
    };
  },
});
