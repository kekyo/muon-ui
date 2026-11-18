// Runs against the real HTTP, TLS and service transports on each desktop target.
import assert from 'node:assert/strict';
import { once } from 'node:events';
import { readFile } from 'node:fs/promises';
import { createServer, type ServerResponse } from 'node:http';
import { createServer as createTls } from 'node:https';
import { join } from 'node:path';
import type { createMuonServicesApi } from '../common/renderer/services.ts';

export const createHttpDriver = async (assets: string, binaries: string) => {
  const server = createServer();
  const tls = createTls({ key: await readFile(join(assets, 'localhost-key.pem')), cert: await readFile(join(assets, 'localhost-cert.pem')) });
  const originOf = async (endpoint: typeof server | typeof tls, scheme: string) => {
    endpoint.listen(0, '127.0.0.1');
    await once(endpoint, 'listening');
    const address = endpoint.address();
    assert(address && typeof address !== 'string');
    return scheme + '://127.0.0.1:' + address.port;
  };
  const origin = await originOf(server, 'http');
  const secure = await originOf(tls, 'https');
  let entered: ((response: ServerResponse) => void) | undefined;
  let forbiddenVisits = 0;
  server.on('request', async (req, res) => {
    if (req.url === '/held') { entered?.(res); return; }
    if (req.url === '/follow') { res.writeHead(303, {location: '/text'}); res.end(); return; }
    if (req.url === '/redirect') { res.writeHead(302, { location: origin.replace('127.0.0.1', 'localhost') + '/forbidden' }); res.end(); return; }
    if (req.url === '/forbidden') ++forbiddenVisits;
    if (req.url === '/loop') { res.writeHead(307, {location:'/loop'}); res.end(); return; }
    if (req.url === '/large') { req.resume(); res.end(Buffer.alloc(1024 * 1024 + 1)); return; }
    if (req.url === '/text') { res.writeHead(418, { 'x-answer': 'yes' }); res.end('茶'); return; }
    const chunks: Buffer[] = [];
    for await (const chunk of req) chunks.push(Buffer.from(chunk));
    res.writeHead(200, {'x-request': String(req.headers['x-request'])});
    res.end(Buffer.concat(chunks));
  });
  const io = { http: { origins: [origin, secure] } };
  return {
    services: {
      httpNative: { library: join(binaries, process.platform === 'win32' ? 'http-service.dll' : 'http-service.so'), io },
      httpQuick: { runtime: 'quickjs', script: join(assets, 'http-quick.js'), io },
      httpNode: { runtime: 'node', project: assets, entry: './http-node.mjs', executable: process.execPath, bridge: join(assets, 'node-bridge.mjs'), io },
    },
    verify: async (api: ReturnType<typeof createMuonServicesApi>) => {
      for (const id of ['httpNative','httpQuick','httpNode']) {
        const client = await api.acquire(id);
        const second = await api.acquire(id === 'httpNative' ? 'httpQuick' : 'httpNative');
        const call = (path: string, timeoutMs = 30000, signal?: AbortSignal) => client.call('bytes', [
          JSON.stringify({ url: origin + path, method: 'POST', timeoutMs, headers: {'x-request':'recorded'} }), new Uint8Array([0,255]),
        ], signal ? {signal} : undefined);
        try {
          assert.deepEqual(await call('/echo'), new Uint8Array([0,255]));
          const args = [JSON.stringify({url: origin + '/text', responseType: 'text'}), new Uint8Array()];
          const reply = JSON.parse(await client.call('describe',args) as string);
          assert.equal(reply.status,418); assert.equal(reply.headers['x-answer'],'yes'); assert.equal(reply.text,'茶');
          assert.equal(JSON.parse(await client.call('describe', [JSON.stringify({url:origin + '/follow',responseType:'text'}),new Uint8Array()]) as string).text,'茶');
          for (const [path,code] of [['/redirect','permission-denied'],['/large','size-limit'],['/loop','redirect-limit']]) {
            await assert.rejects(call(path),{code});
          }
          assert.equal(forbiddenVisits,0);
          await assert.rejects(client.call('bytes',[JSON.stringify({url:secure + '/'}),new Uint8Array()]), {code:'tls-error'});
          const held = new Promise<ServerResponse>(resolve => { entered = resolve; });
          const controller = new AbortController();
          const cancelled = assert.rejects(call('/held',30000,controller.signal), {code:'cancelled'});
          const response = await held;
          const disconnected = once(response,'close');
          assert.equal(JSON.parse(await second.call('describe',args) as string).status,418);
          controller.abort();
          await Promise.all([cancelled,disconnected]);
          await assert.rejects(call('/held',100), {code:'timeout'});
          const heldRequests: ServerResponse[] = [];
          const saturated = new Promise<void>(resolve => { entered = response => {
            heldRequests.push(response); if (heldRequests.length === 8) resolve();
          }; });
          const transfers = Array.from({length:8}, () => call('/held'));
          try { await saturated; await assert.rejects(call('/held'), {code:'resource-exhausted'}); }
          finally { for (const response of heldRequests) response.end('done'); await Promise.all(transfers); }
          const stopped = new Promise<ServerResponse>(resolve => { entered = resolve; });
          const previous = client.generation;
          const waiting = Promise.allSettled([call('/held')]);
          const oldResponse = await stopped;
          const closed = once(oldResponse,'close');
          await client.release();
          await closed;
          assert.equal((await waiting)[0].status,'rejected');
          const next = await api.acquire(id);
          assert.notEqual(next.generation, previous);
          oldResponse.end('obsolete');
          assert.equal(JSON.parse(await next.call('describe',args) as string).text, '茶');
          await next.release();
        } finally { await client.release(); await second.release(); }
      }
      console.log(JSON.stringify({serviceHttp:true, passed:true}));
    },
    close: async () => {
      for (const endpoint of [server,tls]) { const closed = once(endpoint,'close'); endpoint.close(); endpoint.closeAllConnections(); await closed; }
    },
  };
};
