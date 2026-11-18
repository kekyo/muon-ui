// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { writeFile } from 'node:fs/promises';
import { createConnection, Socket } from 'node:net';
import { join } from 'node:path';
import { createMuonNodeFrameDecoder } from '../../src/framing.ts';

// Simulate a peer losing its transport while the OS process remains alive.
// The native adapter must reap it and settle the outstanding public method.
const socket =
  process.platform === 'win32'
    ? createConnection(process.env.MUON_NODE_PIPE!)
    : new Socket({
        fd: Number(process.env.MUON_NODE_FD),
        readable: true,
        writable: true,
      });
setInterval(() => {}, 60000);
const receive = async (message: any) => {
  if (message.command === 'initialize') {
    if (message.params.token !== process.env.MUON_NODE_TOKEN)
      throw new Error('Invalid host token');
    await writeFile(
      join(message.params.dataDirectory, 'pid'),
      String(process.pid)
    );
    const body = Buffer.from(
      JSON.stringify({
        kind: 'response',
        id: message.id,
        ok: true,
        value: {
          protocol: 'node-service/1',
          interface: {
            schemaVersion: 1,
            methods: { drop: { parameters: [], returns: 'void', errors: [] } },
            events: {},
          },
        },
      })
    );
    const header = Buffer.alloc(4);
    header.writeUInt32BE(body.length);
    socket.write(Buffer.concat([header, body]));
  } else if (message.command === 'invoke') socket.end();
};
const decoder = createMuonNodeFrameDecoder((message) => {
  void receive(message);
});
socket.on('data', (chunk) => decoder.write(chunk));
