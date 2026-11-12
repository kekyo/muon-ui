// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui

import { execFile, spawn } from 'node:child_process';
import { randomUUID } from 'node:crypto';
import { EventEmitter, on, once } from 'node:events';
import { mkdtemp } from 'node:fs/promises';
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
it.skipIf(!enabled)(
  'isolates page RPC owners over the Android service FD transport',
  async () => {
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
      const text = await tool('browser_evaluate', {
        function: `async () => { ${source} }`,
      });
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
      await adb(['shell', 'am', 'force-stop', app]);
      await adb([
        'shell',
        'run-as',
        app,
        'rm',
        '-rf',
        'files/service-autostart',
      ]);
      const first = await start();
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
      await select(first);
      expect(
        await evaluate(`globalThis.handle = await muon.services.acquire('alpha.v1-test');
      const value = 'Android\\0日本語😀';
      return await handle.call('echo', [value]);`)
      ).toBe('Android\0日本語😀');
      expect(
        await evaluate(`const bytes = new Uint8Array(1024 * 1024); bytes[0]=37; bytes[bytes.length-1]=91;
      const result = await handle.call('echoBytes',[bytes]);
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
      expect(
        await evaluate(`const second = await muon.services.acquire('alpha.v1-test'); await second.release();
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
      await evaluate(`globalThis.handle = await muon.services.acquire('alpha.v1-test');
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
      const watch = await muon.services.watchState('alpha.v1-test',state=>{if(state.clientReferences===1) changed();});
      await ownerGone; await watch.close(); finishCallback();
      await handle.call('emit',[41]);
      return [await pending,await received,await handle.call('echo',['still-running'])];`)
      ).toEqual(['second:alive', 41, 'still-running']);
      expect(
        await evaluate(`await subscription.close(); await handle.release();
      const state = await muon.services.getState('alpha.v1-test');
      return [state.state,state.clientReferences];`)
      ).toEqual(['running', 0]);
      await evaluate(`globalThis.handle = await muon.services.acquire('alpha.v1-test');
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
          `try { await muon.services.acquire('alpha.v1-test'); return false; } catch { return true; }`
        )
      ).toBe(true);
      const replacement = await start();
      await select(replacement);
      expect(
        await evaluate(
          `globalThis.handle = await muon.services.acquire('alpha.v1-test'); return await handle.call('echo',['replacement']);`
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
          `return [await handle.call('echo',['unaffected']), (await muon.services.getState('alpha.v1-test')).clientReferences];`
        )
      ).toEqual(['unaffected', 1]);
      await evaluate(`await handle.release(); return true;`);
    } finally {
      const results = await Promise.allSettled([client?.close(), closeMcp?.()]);
      lines.close();
      const exited = once(logcat, 'exit');
      logcat.kill();
      await exited;
      if (port) await adb(['forward', '--remove', 'tcp:' + port]);
      await adb(['shell', 'am', 'force-stop', app]);
      for (const result of results)
        if (result.status === 'rejected') throw result.reason;
    }
  },
  120_000
);
