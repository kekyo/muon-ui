// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

// The test suite bundles this observation driver and the SDK fixture before
// uploading them. Remote machines need only Node and the native host binaries.
import { createServerDriver, verifyListenerShutdown } from './server-driver.ts';
import { createHttpDriver } from './http-driver.ts';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { randomUUID } from 'node:crypto';
import { once } from 'node:events';
import { mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { createServer } from 'node:http';
import { createConnection } from 'node:net';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { createInterface } from 'node:readline';
import { createMuonServicesApi } from '../common/renderer/services.ts';

const binaries = resolve(process.argv[2]!);
const assets = resolve(process.argv[3]!);
const root = await mkdtemp(join(tmpdir(), 'muon-node 日本語 '));
const endpoint = process.platform === 'win32' ? String.raw`\\.\pipe\muon-service-node-${randomUUID()}` : join(root, 'control');
const marker = join(root, 'stopped');
const control = join(root, 'mode');
const childPid = join(root, 'child-pid');
await writeFile(control, 'normal');
const observer = createServer();
observer.listen(0, '127.0.0.1');
await once(observer, 'listening');
const address = observer.address();
assert(address && typeof address !== 'string');
const httpDriver = await createHttpDriver(assets, binaries);
const listenerDriver = await createServerDriver(assets,binaries);
await writeFile(join(root, 'services.json'), JSON.stringify({ services: {
  ...httpDriver.services,
  ...listenerDriver.services,
  draining: { runtime: 'node', project: assets, entry: './draining-listener.mjs',
    executable: process.execPath, bridge: join(assets, 'node-service-bridge.mjs'),
    config: { origin: `http://127.0.0.1:${address.port}` } },
  worker: { runtime: 'node', project: assets, entry: './service.mjs',
    executable: process.execPath, bridge: join(assets, 'node-service-bridge.mjs'), autostart: true,
    config: { origin: `http://127.0.0.1:${address.port}`, marker } },
  lifecycle: { runtime: 'node', project: assets, entry: './lifecycle.mjs',
    executable: process.execPath, bridge: join(assets, 'node-service-bridge.mjs'),
    config: { origin: `http://127.0.0.1:${address.port}`, control, pid: childPid, marker: join(root, 'lifecycle-marker') } },
  disconnected: { runtime: 'node', project: assets, entry: './service.mjs',
    executable: process.execPath, bridge: join(assets, 'disconnect-bridge.mjs') },
  absent: { runtime: 'node', project: assets, entry: './service.mjs',
    executable: join(root, 'absent-node'), bridge: join(assets, 'node-service-bridge.mjs') },
} }));
const workEntered = once(observer, 'request');
const host = spawn(join(binaries, process.platform === 'win32' ? 'muon-service-host.exe' : 'muon-service-host'), [
  '--config', join(root, 'services.json'), '--endpoint', endpoint, '--data-root', join(root, 'data'), '--bootstrap', 'development',
], { stdio: process.platform === 'win32' ? 'overlapped' : 'pipe', windowsHide: true });
const exited = once(host, 'close');
let diagnostic = '';
host.stderr!.on('data', (chunk) => { diagnostic += String(chunk); });
const lines = createInterface({ input: host.stdout! });
const unexpectedOutput: string[] = [];
const ready = new Promise<void>((resolve) => lines.on('line', (line) => {
  try { if (JSON.parse(line).ready) resolve(); }
  catch { unexpectedOutput.push(line); }
}));
const sockets: ReturnType<typeof createConnection>[] = [];
let ui: ReturnType<typeof spawn> | undefined;
try {
  await Promise.race([ready, (async () => { await exited; throw new Error(diagnostic); })()]);
  const connect = async () => {
    const socket = createConnection(endpoint);
    sockets.push(socket);
    await once(socket, 'connect');
    let sequence = 0;
    const pending = new Map<string, { resolve: (value: unknown) => void; reject: (error: Error) => void }>();
    createInterface({ input: socket }).on('line', (line) => {
      const reply = JSON.parse(line);
      const waiting = pending.get(reply.requestId);
      if (!waiting) return;
      pending.delete(reply.requestId);
      if (reply.error) waiting.reject(new Error(reply.error.message));
      else waiting.resolve(reply.result);
    });
    socket.on('close', () => {
      for (const entry of pending.values()) entry.reject(new Error('Host disconnected: ' + diagnostic));
      pending.clear();
    });
    return { socket, api: createMuonServicesApi(async (op, json) => {
      const requestId = String(++sequence);
      const reply = new Promise<unknown>((resolve, reject) => pending.set(requestId, { resolve, reject }));
      socket.write(JSON.stringify({ ...JSON.parse(json), op, requestId }) + '\n');
      return JSON.stringify(await reply);
    }) };
  };
  const first = await connect();
  await httpDriver.verify(first.api);
  await listenerDriver.verify(connect);
  const service = await first.api.acquire('worker');
  const [workRequest, workResponse] = await workEntered;
  assert.equal(workRequest.url, '/work');
  assert.equal(await service.call('integer', [-9223372036854775808n]), -9223372036854775808n);
  assert.deepEqual(await service.call('bytes', [new Uint8Array([0, 1, 255])]), new Uint8Array([0, 1, 255]));
  assert.equal(await service.call('callback', ['input', async (value: string) => value + ' reply']), 'input reply');
  const controller = new AbortController();
  let waiting!: () => void;
  const entered = new Promise<void>((resolve) => { waiting = resolve; });
  const subscription = await service.subscribe('progress', (value) => { if (value === 'waiting') waiting(); });
  const canceled = assert.rejects(service.call('wait', [], { signal: controller.signal }), { code: 'cancelled' });
  await entered;
  controller.abort();
  await canceled;
  await subscription.close();
  const generation = service.generation;
  await service.release();
  first.socket.destroy();
  ui = spawn(process.execPath, ['-e', `
    const net = require('node:net'); const readline = require('node:readline');
    const socket = net.createConnection(process.argv[1]);
    socket.on('connect', () => socket.write(JSON.stringify({requestId:'1',op:'acquire',serviceId:'worker'})+'\\n'));
    readline.createInterface({input:socket}).on('line', line => {
      const reply=JSON.parse(line); if(reply.requestId==='1') process.send(reply);
    });
  `, endpoint], { stdio: ['ignore', 'ignore', 'pipe', 'ipc'], windowsHide: true });
  const uiExited = once(ui, 'close');
  assert.equal((await once(ui, 'message'))[0].result.generation, generation);
  ui.kill();
  await uiExited;
  const second = await connect();
  const reconnected = await second.api.acquire('worker');
  assert.equal(reconnected.generation, generation);
  let progressed!: () => void;
  const progress = new Promise<void>((resolve) => { progressed = resolve; });
  const resumed = await reconnected.subscribe('progress', (value) => { if (value === 'after UI exit') progressed(); });
  workResponse.end('after UI exit');
  await progress;
  assert.equal(await reconnected.call('status', []), 'after UI exit');
  await resumed.close();
  await reconnected.release();
  const draining = await second.api.acquire('draining');
  const drainingPort = Number(await draining.call('port', []));
  const drainingSocket = new WebSocket(`ws://127.0.0.1:${drainingPort}`);
  await once(drainingSocket, 'open');
  const disconnectedSocket = once(drainingSocket, 'close');
  const enteredDrain = once(observer, 'request');
  await Promise.all([
    verifyListenerShutdown(second.api, draining, disconnectedSocket, drainingPort),
    (async () => {
      const [request, response] = await enteredDrain;
      try {
        assert.equal(request.url, '/listener-stop');
        await disconnectedSocket;
        const state = await second.api.getState(draining.id);
        assert.equal(state.generation, draining.generation);
        assert.equal(state.state, 'stopping');
        const alive = await fetch(`http://127.0.0.1:${drainingPort}/during-stop`);
        assert.equal(await alive.text(), 'listening');
      } finally {
        response.end('finish closing the listener');
      }
    })(),
  ]);
  console.log(JSON.stringify({ drainingListener: true, passed: true }));
  await assert.rejects(second.api.acquire('absent'), /Failed to start Node executable/);
  assert.equal((await second.api.getState('absent')).state, 'failed');
  // A failed generation must be reaped before a later acquire starts another.
  for (const mode of ['start-failure', 'start-timeout', 'crash', 'stop-crash', 'stop-timeout']) {
    await writeFile(control, mode);
    const previous = await second.api.getState('lifecycle');
    let failed!: () => void;
    const failure = new Promise<void>((resolve) => { failed = resolve; });
    const watcher = await second.api.watchState('lifecycle', (state) => {
      if (state.state === 'failed' && state.generation !== previous.generation) failed();
    });
    if (mode.startsWith('start-')) {
      await assert.rejects(second.api.acquire('lifecycle'));
    } else {
      const handle = await second.api.acquire('lifecycle');
      if (mode === 'crash') await assert.rejects(handle.call('crash', []));
      else await handle.release();
    }
    await failure;
    const state = await second.api.getState('lifecycle');
    assert.equal(state.state, 'failed');
    assert(state.error.length > 0);
    if (mode.endsWith('timeout')) assert(state.error.includes('deadline exceeded'), state.error);
    const pid = Number(await readFile(childPid, 'utf8'));
    assert.throws(() => process.kill(pid, 0), { code: 'ESRCH' });
    await watcher.close();
    await writeFile(control, 'normal');
    const replacement = await second.api.acquire('lifecycle');
    assert.notEqual(replacement.generation, state.generation);
    assert.notEqual(await replacement.call('pid', []), pid);
    let stopped!: () => void;
    const stop = new Promise<void>((resolve) => { stopped = resolve; });
    const stopWatcher = await second.api.watchState('lifecycle', (state) => {
      if (state.state === 'stopped') stopped();
    });
    await replacement.release();
    await stop;
    await stopWatcher.close();
    console.log(JSON.stringify({ nodeLifecycle: mode, passed: true }));
  }
  await writeFile(control, 'stop-wait');
  const stopping = await second.api.acquire('lifecycle');
  const enteredStop = once(observer, 'request');
  await stopping.release();
  const [gatedRequest, gatedResponse] = await enteredStop;
  assert.equal(gatedRequest.url, '/lifecycle-stop');
  assert.equal((await second.api.getState('lifecycle')).state, 'stopping');
  let acquired = false;
  const acquisition = (async () => {
    const handle = await second.api.acquire('lifecycle');
    acquired = true;
    return handle;
  })();
  // A state round trip orders acquire behind the pending stop without sleeping.
  assert.equal((await second.api.getState('lifecycle')).state, 'stopping');
  assert.equal(acquired, false);
  await writeFile(control, 'normal');
  gatedResponse.end('finish the old generation');
  const next = await acquisition;
  assert.notEqual(next.generation, stopping.generation);
  await next.release();
  console.log(JSON.stringify({ nodeReacquireDuringStop: true, passed: true }));
  const disconnected = await second.api.acquire('disconnected');
  let failedTransport!: () => void;
  const lost = new Promise<void>((resolve) => { failedTransport = resolve; });
  const lostWatcher = await second.api.watchState('disconnected', (state) => {
    if (state.state === 'failed') failedTransport();
  });
  await assert.rejects(disconnected.call('drop', []));
  await lost;
  const disconnectedPid = Number(await readFile(join(root, 'data/disconnected/pid'), 'utf8'));
  assert.throws(() => process.kill(disconnectedPid, 0), { code: 'ESRCH' });
  await lostWatcher.close();
  console.log(JSON.stringify({ nodeTransportDisconnected: true, passed: true }));
  second.socket.destroy();
  const stopEntered = once(observer, 'request');
  host.stdin!.end();
  const [stopRequest, stopResponse] = await stopEntered;
  assert.equal(stopRequest.url, '/stop');
  assert.equal(host.exitCode, null, diagnostic);
  stopResponse.end('cleanup may finish');
  assert.deepEqual(await exited, [0, null], diagnostic);
  assert.deepEqual(JSON.parse(await readFile(marker, 'utf8')), [generation, 'app-start', 'development-exit']);
  assert.deepEqual(unexpectedOutput, []);
  assert(diagnostic.includes('Node service author output'));
  console.log(JSON.stringify({ nodeService: true, passed: true }));
} finally {
  for (const socket of sockets) socket.destroy();
  if (ui && ui.exitCode === null && ui.signalCode === null) ui.kill('SIGKILL');
  if (host.exitCode === null && host.signalCode === null) { host.kill('SIGKILL'); await exited; }
  await httpDriver.close();
  lines.close();
  observer.closeAllConnections();
  await new Promise<void>((resolve) => observer.close(() => resolve()));
  await rm(root, { recursive: true, force: true });
}
