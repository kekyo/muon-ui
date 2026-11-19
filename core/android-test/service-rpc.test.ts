// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile, spawn } from 'node:child_process';
import { randomUUID } from 'node:crypto';
import { EventEmitter, on, once } from 'node:events';
import { mkdtemp, readFile } from 'node:fs/promises';
import { createServer as httpServer } from 'node:http';
import { createServer as tlsServer } from 'node:https';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createInterface } from 'node:readline';
import { promisify } from 'node:util';
import { Client } from '@modelcontextprotocol/sdk/client/index.js';
import { InMemoryTransport } from '@modelcontextprotocol/sdk/inMemory.js';
import { createConnection } from '@playwright/mcp';
import { expect, it } from 'vitest';

const execute = promisify(execFile);
const enabled = process.env.MUON_ANDROID_SERVICE_RPC_TEST === '1';
it.skipIf(!enabled).each(['alpha.v1-test', 'quickjs'])(
  'isolates %s page RPC owners over the Android service FD transport',
  async (serviceId) => {
    const serial = process.env.ANDROID_SERIAL;
    if (!serial) throw new Error('ANDROID_SERIAL is required');
    const adb = async (args: string[]) =>
      (await execute('adb', ['-s', serial, ...args])).stdout;
    const app = 'dev.muon.servicefixture';
    const directory = await mkdtemp(join(tmpdir(), 'muon-android-services-'));
    const events = new EventEmitter();
    const logcat = spawn('adb', [
      '-s',
      serial,
      'logcat',
      '-v',
      'brief',
      '-T',
      '1',
      'MuonServiceRpcReady:I',
      '*:S',
    ]);
    const lines = createInterface({ input: logcat.stdout });
    lines.on('line', (line) => events.emit('ready', line));
    const http = httpServer((req, res) => {
      if (req.url === '/held') {
        events.emit('held', res);
        return;
      }
      if (req.url === '/redirect') {
        res.writeHead(302, { location: 'http://localhost:49206/refused' });
        res.end();
        return;
      }
      if (req.url === '/large') {
        res.end(Buffer.alloc(1024 * 1024 + 1));
        return;
      }
      if (req.url === '/text') {
        res.writeHead(418, { 'x-android': 'yes' });
        res.end('茶');
        return;
      }
      req.pipe(res);
    });
    const tls = tlsServer({
      key: await readFile('../service-host/test-assets/localhost-key.pem'),
      cert: await readFile('../service-host/test-assets/localhost-cert.pem'),
    });
    const reverse: string[] = [];
    const listenerPorts: number[] = [];
    let port: string | undefined;
    let client: Client | undefined;
    let closeMcp: (() => Promise<void>) | undefined;
    const start = async () => {
      const label = randomUUID();
      const abort = new AbortController();
      const ready = (async () => {
        const signal = AbortSignal.any([
          AbortSignal.timeout(30_000),
          abort.signal,
        ]);
        for await (const [line] of on(events, 'ready', { signal })) {
          if (String(line).includes(label)) return;
        }
      })();
      try {
        const [result] = await Promise.all([
          adb([
            'shell',
            'am',
            'start',
            '-W',
            '-f',
            '0x18000000',
            '-n',
            app + '/dev.muon.runtime.MuonServiceRpcActivity',
            '--es',
            'owner',
            label,
          ]),
          ready,
        ]);
        expect(result).not.toMatch(/Error|does not exist/u);
        return label;
      } finally {
        abort.abort();
        await Promise.allSettled([ready]);
      }
    };
    const tool = async (name: string, args: Record<string, unknown>) => {
      const result = await client!.callTool({ name, arguments: args });
      const text = (result.content as { type: string; text?: string }[])
        .filter((x) => x.type === 'text')
        .map((x) => x.text ?? '')
        .join('\n');
      if (result.isError) throw new Error(text);
      return text;
    };
    const evaluate = async <T>(source: string): Promise<T> => {
      let text: string;
      try {
        text = await tool('browser_evaluate', {
          function: `async () => { ${source} }`,
        });
      } catch (cause) {
        throw new Error(`Service page evaluation failed: ${source}`, { cause });
      }
      const value = /### Result\n([\s\S]*?)(?:\n### |$)/u.exec(text)?.[1];
      if (!value) throw new Error(text);
      return JSON.parse(value) as T;
    };
    const select = async (label: string) => {
      const tabs = await tool('browser_tabs', { action: 'list' });
      const line = tabs.split('\n').find((line) => line.includes(label));
      const index = line && /^- (\d+):/u.exec(line)?.[1];
      if (!index) throw new Error(tabs);
      await tool('browser_tabs', { action: 'select', index: Number(index) });
    };
    try {
      for (const [server, devicePort] of [
        [http, '49206'],
        [tls, '49207'],
      ] as const) {
        server.listen(0, '127.0.0.1');
        await once(server, 'listening');
        const target = server.address();
        if (!target || typeof target === 'string')
          throw new Error('Missing fixture port');
        await adb(['reverse', 'tcp:' + devicePort, 'tcp:' + target.port]);
        reverse.push(devicePort);
      }
      await adb(['shell', 'am', 'force-stop', app]);
      await adb([
        'shell',
        'run-as',
        app,
        'rm',
        '-rf',
        'files/service-autostart',
      ]);
      const stateId = serviceId === 'quickjs' ? 'stateQuick' : 'stateNative';
      await adb([
        'shell',
        'run-as',
        app,
        'rm',
        '-f',
        'files/services/' + stateId + '/state.bin',
      ]);
      let first = await start();
      const connectPage = async (label: string) => {
        const pid = (await adb(['shell', 'pidof', app])).trim();
        port = (
          await adb([
            'forward',
            'tcp:0',
            'localabstract:webview_devtools_remote_' + pid,
          ])
        ).trim();
        const server = await createConnection({
          browser: { cdpEndpoint: 'http://127.0.0.1:' + port },
          outputDir: directory,
        });
        closeMcp = async () => await server.close();
        const [a, b] = InMemoryTransport.createLinkedPair();
        client = new Client({ name: 'muon-android-services', version: '1' });
        await server.connect(b);
        await client.connect(a);
        await select(label);
      };
      await connectPage(first);
      const listenerId =
        serviceId === 'quickjs' ? 'listenQuick' : 'listenNative';
      const otherId =
        listenerId === 'listenQuick' ? 'listenNative' : 'listenQuick';
      const localPort = listenerId === 'listenQuick' ? 49212 : 49211;
      for (const value of [49211, 49212]) {
        await adb(['forward', '--no-rebind', 'tcp:' + value, 'tcp:' + value]);
        listenerPorts.push(value);
      }
      const generations = await evaluate<string[]>(`const generations=[];
        for (const id of ['${listenerId}','${otherId}']) {
          await muon.services.setAutostart(id,true);
          const listener=await muon.services.acquire(id); generations.push(listener.generation);
        } return generations;`);
      await client?.close();
      await closeMcp?.();
      client = undefined;
      closeMcp = undefined;
      await adb(['forward', '--remove', 'tcp:' + port]);
      port = undefined;
      const pageClosed = (async () => {
        for await (const [line] of on(events, 'ready', {
          signal: AbortSignal.timeout(30000),
        }))
          if (String(line).includes('closed:' + first)) return;
      })();
      await adb([
        'shell',
        'am',
        'broadcast',
        '-a',
        'dev.muon.servicefixture.CLOSE_PAGE',
        '-p',
        app,
        '--es',
        'owner',
        first,
      ]);
      await pageClosed;
      const url = 'http://127.0.0.1:' + localPort;
      const auth = { Authorization: 'Bearer local-service' };
      expect((await fetch(url)).status).toBe(401);
      expect(
        (
          await fetch(url, {
            headers: { ...auth, Origin: 'https://unlisted.test' },
          })
        ).status
      ).toBe(403);
      expect(await (await fetch(url, { headers: auth })).text()).toBe(
        generations[0]
      );
      const ws = new WebSocket('ws://127.0.0.1:' + localPort, [
        'muon.auth',
        'muon.bearer.' + Buffer.from('local-service').toString('base64url'),
      ]);
      ws.binaryType = 'arraybuffer';
      await once(ws, 'open');
      const echo = once(ws, 'message');
      ws.send(new Uint8Array([0, 128, 255]));
      expect(new Uint8Array((await echo)[0].data)).toEqual(
        new Uint8Array([0, 128, 255])
      );
      first = await start();
      await connectPage(first);
      const disconnected = once(ws, 'close');
      expect(
        await evaluate(`const last=await muon.services.acquire('${listenerId}');
        await muon.services.setAutostart('${listenerId}',false);await last.release();return last.generation;`)
      ).toBe(generations[0]);
      await disconnected;
      const otherPort = localPort === 49211 ? 49212 : 49211;
      expect(
        await (
          await fetch('http://127.0.0.1:' + otherPort, { headers: auth })
        ).text()
      ).toBe(generations[1]);
      const next = await evaluate<string>(
        `globalThis.listener=await muon.services.acquire('${listenerId}');return listener.generation;`
      );
      expect(next).not.toBe(generations[0]);
      expect(await (await fetch(url, { headers: auth })).text()).toBe(next);
      await evaluate(
        `await listener.release();await muon.services.setAutostart('${otherId}',false);return true;`
      );
      const httpId = serviceId === 'quickjs' ? 'httpQuick' : 'httpNative';
      expect(
        await evaluate(`globalThis.httpHandle = await muon.services.acquire('${httpId}');
        globalThis.transfer = async (path, method = 'bytes', extras = {}, signal) => await httpHandle.call(method, [
          JSON.stringify({url:'http://127.0.0.1:49206' + path, method:'POST', responseType: method === 'describe' ? 'text':'bytes', ...extras}),
          new Uint8Array([0,128,255])], signal ? {signal} : undefined);
        const reply = JSON.parse(await transfer('/text','describe'));
        const errors = [];
        for (const [path, options] of [['/redirect',{}],['/large',{}],['/',{url:'https://127.0.0.1:49207/'}],['/held',{timeoutMs:250}]]) {
          try { await transfer(path,'bytes',options); } catch (error) { errors.push(error.code); }
        }
        return [Array.from(await transfer('/echo')),reply.status,reply.headers['x-android'],reply.text,errors];`)
      ).toEqual([
        [0, 128, 255],
        418,
        'yes',
        '茶',
        ['permission-denied', 'size-limit', 'tls-error', 'timeout'],
      ]);
      const held = once(events, 'held');
      await evaluate(`globalThis.cancelHttp = new AbortController();
        globalThis.httpDone = (async () => {try {await transfer('/held','bytes',{},cancelHttp.signal); return '';} catch (e) {return e.code;}})();
        return true;`);
      const [heldResponse] = await held;
      const closed = once(heldResponse, 'close');
      expect(
        await evaluate(`const other = await muon.services.acquire('${serviceId}');
        const echo = await other.call('echo',['independent']); await other.release();
        cancelHttp.abort(); const cause = await httpDone; await httpHandle.release(); return [echo,cause];`)
      ).toEqual(['independent', 'cancelled']);
      await closed;
      expect(
        await evaluate(`const saved = await muon.services.acquire('${stateId}');
        const initial = Array.from(await saved.call('get', []));
        await saved.call('set', [new Uint8Array([0, 128, 255])]);
        await saved.release();
        const restored = await muon.services.acquire('${stateId}');
        const bytes = Array.from(await restored.call('get', []));
        await restored.release();
        return [initial, bytes, saved.generation !== restored.generation];`)
      ).toEqual([[0, 255], [0, 128, 255], true]);
      expect(
        await evaluate(`globalThis.handle = await muon.services.acquire('${serviceId}');
      const value = 'Android\\0日本語😀';
      return await handle.call('echo', [value]);`)
      ).toBe('Android\0日本語😀');
      expect(
        await evaluate(`const bytes = new Uint8Array(1024 * 1024); bytes[0]=37; bytes[bytes.length-1]=91;
      let result;
      try { result = await handle.call('echoBytes',[bytes]); }
      catch(error) { throw new Error(JSON.stringify({error:String(error),state:await muon.services.getState('${serviceId}')})); }
      return [result.length,result[0],result[result.length-1],
        await handle.call('echoInt64',[-9223372036854775808n]),
        await handle.call('echoUint64',[18446744073709551615n])].map(x=>String(x));`)
      ).toEqual([
        '1048576',
        '37',
        '91',
        '-9223372036854775808',
        '18446744073709551615',
      ]);
      expect(
        await evaluate(
          `return await handle.call('callback',['outer',async value => await handle.call('echo',[value + ':nested'])]);`
        )
      ).toBe('outer:nested');
      if (serviceId === 'quickjs') {
        expect(
          await evaluate(`const broken = await muon.services.acquire('quickjs-fault');
          let failure=false; try { await broken.call('spin',[]); } catch { failure=true; }
          let changed; const failed=new Promise(resolve=>changed=resolve);
          const watch=await muon.services.watchState('quickjs-fault',state=>{if(state.state==='failed') changed();});
          await failed; await watch.close(); await broken.release();
          const state=await muon.services.getState('quickjs-fault');
          const native=await muon.services.acquire('alpha.v1-test');
          const value=await native.call('echo',['native-after-fault']); await native.release();
          return [failure,state.state,state.clientReferences,state.resident,await handle.call('echo',['other-heap']),value];`)
        ).toEqual([
          true,
          'failed',
          0,
          true,
          'other-heap',
          'native-after-fault',
        ]);
      }
      expect(
        await evaluate(`const second = await muon.services.acquire('${serviceId}'); await second.release();
      const abort = new AbortController(); let entered;
      const ready = new Promise(resolve=>entered=resolve);
      const pending = handle.call('callback',['cancel',()=>{ entered(); return new Promise(()=>{}); }],{signal:abort.signal});
      const result = (async()=>{try { await pending; return 'unexpected'; } catch(error) { return error.code; }})();
      await ready; abort.abort(); return await result;`)
      ).toBe('cancelled');
      await evaluate(`let entered; const ready = new Promise(resolve=>entered=resolve);
      globalThis.pending = (async()=>{try { return await handle.call('callback',['first',()=>{entered(); return new Promise(()=>{});}]); } catch(error) { return error.code; }})();
      await ready; return true;`);
      const second = await start();
      await select(second);
      await evaluate(`globalThis.handle = await muon.services.acquire('${serviceId}');
      let entered; const ready = new Promise(resolve=>entered=resolve);
      globalThis.pending = handle.call('callback',['second',value=>{entered(); return new Promise(resolve=>globalThis.finishCallback=()=>resolve(value+':alive'));}]);
      globalThis.received = new Promise(resolve=>globalThis.receive=resolve);
      globalThis.subscription = await handle.subscribe('progress',value=>receive(value));
      await ready; return true;`);
      await select(first);
      // Navigation destroys the native document owner but leaves the Activity and
      // the second WebView alive. No force-stop or service-process kill is involved.
      await tool('browser_navigate', {
        url: 'https://main.asset.muon.invalid/empty.html',
      });
      await select(second);
      expect(
        await evaluate(`let changed;
      const ownerGone = new Promise(resolve=>changed=resolve);
      const watch = await muon.services.watchState('${serviceId}',state=>{if(state.clientReferences===1) changed();});
      await ownerGone; await watch.close(); finishCallback();
      await handle.call('emit',[41]);
      return [await pending,await received,await handle.call('echo',['still-running'])];`)
      ).toEqual(['second:alive', 41, 'still-running']);
      expect(
        await evaluate(`await subscription.close(); await handle.release();
      const state = await muon.services.getState('${serviceId}');
      return [state.state,state.clientReferences];`)
      ).toEqual(['running', 0]);
      await evaluate(`globalThis.handle = await muon.services.acquire('${serviceId}');
        let entered; const ready = new Promise(resolve=>entered=resolve);
        globalThis.pending = (async()=>{try { await handle.call('callback',['disconnect',()=>{entered(); return new Promise(()=>{});}]); return false; } catch { return true; }})();
        await ready; return true;`);
      const hostPid = (
        await adb(['shell', 'pidof', app + ':muon_services'])
      ).trim();
      expect(hostPid).toMatch(/^\d+$/u);
      await adb(['shell', 'run-as', app, 'kill', '-9', hostPid]);
      expect(await evaluate(`return await pending;`)).toBe(true);
      expect(
        await evaluate(
          `try { await muon.services.acquire('${serviceId}'); return false; } catch { return true; }`
        )
      ).toBe(true);
      const replacement = await start();
      await select(replacement);
      expect(
        await evaluate(
          `globalThis.handle = await muon.services.acquire('${serviceId}'); return await handle.call('echo',['replacement']);`
        )
      ).toBe('replacement');
      await select(second);
      expect(
        await evaluate(
          `try { await handle.release(); return false; } catch { return true; }`
        )
      ).toBe(true);
      await select(replacement);
      expect(
        await evaluate(
          `return [await handle.call('echo',['unaffected']), (await muon.services.getState('${serviceId}')).clientReferences];`
        )
      ).toEqual(['unaffected', 1]);
      await evaluate(`await handle.release(); return true;`);
      expect(
        await evaluate(`await muon.services.setAutostart('${serviceId}',false);
        let stopped; const done = new Promise(resolve=>stopped=resolve);
        const watch = await muon.services.watchState('${serviceId}',state=>{if(state.state==='stopped') stopped();});
        await done; await watch.close(); return (await muon.services.getState('${serviceId}')).state;`)
      ).toBe('stopped');
      if (serviceId === 'quickjs') {
        expect(
          await evaluate(`const native = await muon.services.acquire('alpha.v1-test');
          const value = await native.call('echo',['native-after-quickjs-stop']); await native.release(); return value;`)
        ).toBe('native-after-quickjs-stop');
      }
    } finally {
      const results = await Promise.allSettled([client?.close(), closeMcp?.()]);
      lines.close();
      const exited = once(logcat, 'exit');
      logcat.kill();
      await exited;
      for (const devicePort of reverse)
        await adb(['reverse', '--remove', 'tcp:' + devicePort]);
      for (const value of listenerPorts)
        await adb(['forward', '--remove', 'tcp:' + value]);
      for (const server of [http, tls]) {
        const closed = once(server, 'close');
        server.close();
        server.closeAllConnections();
        await closed;
      }
      if (port) await adb(['forward', '--remove', 'tcp:' + port]);
      await adb(['shell', 'am', 'force-stop', app]);
      for (const result of results)
        if (result.status === 'rejected') throw result.reason;
    }
  },
  120_000
);
